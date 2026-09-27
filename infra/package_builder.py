#!/usr/bin/env python3
"""Provision, run, verify, collect, and image BioSeq's native AWS package builders.

The normal ``run`` command
owns the complete lifecycle: it selects architecture-specific cached AMIs,
applies the isolated OpenTofu stack without permitting instance replacement,
starts only the exact instances recorded in that stack, prepares lock-hashed
Poetry environments, builds on both native architectures, validates the remote
manifests and exact S3 object versions, and creates post-build AMIs only for the
complete canonical matrix. Release automation may rediscover the existing
tagged stack without local OpenTofu state and stop it through its configured
instance-initiated ``stop`` behavior. Receipt-scoped release cleanup stops only
builders it started; it never terminates an instance.

Only committed source is transferred to builders.  Package signing material is
kept in AWS Secrets Manager and is never copied through this process or placed
in OpenTofu state.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import datetime as dt
import fcntl
import hashlib
import ipaddress
import json
import os
import re
import secrets
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence


HERE = Path(__file__).resolve().parent
REPOSITORY_ROOT = HERE.parent
STACK_DIR = HERE / "package_build_infra"
PACKAGING_DIR = HERE / "packaging"
POETRY_LOCK_FILE = PACKAGING_DIR / "poetry.lock"
MATRIX_FILE = HERE / "package_matrix.json"
RUNTIME_DIR = STACK_DIR / ".runtime"
LOG_DIR = STACK_DIR / ".logs"
LAST_RUN_FILE = RUNTIME_DIR / "last-run.json"
LOCK_FILE = RUNTIME_DIR / "package-builder.lock"

PROTECTED_INSTANCE_IDS = frozenset(
    {
        "i-05b18218430e9fecb",
        "i-05d1fa02e86288637",
    }
)

BOOTSTRAP_SCHEMA_REVISION = 2
WORKER_GENERATION = "python-poetry"
WORKER_GENERATION_REVISION = 1

EXPECTED_INSTANCE_TAGS = {
    "Project": "bioseq",
    "Component": "package-build-machine",
    "ManagedBy": "opentofu",
    "ProtectedFromDelete": "true",
    "Lifecycle": "persistent-build-cache",
}

CACHE_TAGS = {
    "Project": "bioseq",
    "Component": "package-builder-cache",
    "ManagedBy": "bioseq-package-builder",
    "BaseOS": "ubuntu-24.04",
    "CacheStage": "post-build",
    "WorkerGeneration": WORKER_GENERATION,
    "WorkerGenerationRevision": str(WORKER_GENERATION_REVISION),
}

EXPECTED_PACKAGE_VERSION = "4.2.3"
EXPECTED_SIGNING_FINGERPRINT = "452F34C5F1EF6D471EC041ADDD94780B0728F093"
DEFAULT_REGION = "us-east-1"
DEFAULT_S3_PREFIX = "bioseq/packages"
RESOURCE_NAME_PREFIX = "bioseq-package-builder"
CI_ACCESS_SCHEMA_REVISION = 3
CI_ACCESS_DESCRIPTION = "bioseq package release ephemeral SSH"
CONTROLLER_SCHEMA_REVISION = 1
CONTROLLER_KIND = "bioseq-native-package-run"
CONTROLLER_STATES = frozenset(
    {"prepared", "active", "completed", "finalized", "collected", "failed"}
)
CLEANUP_EVIDENCE_SCHEMA_REVISION = 1
COLLECTION_SCHEMA_REVISION = 2
REMOTE_WORKSPACE = "/opt/bioseq-package-builder/workspace"
REMOTE_ROOT = f"{REMOTE_WORKSPACE}/builds"
REMOTE_POETRY_VENV_ROOT = f"{REMOTE_WORKSPACE}/caches/poetry-venvs"
REMOTE_PACKAGE_CMAKE_CACHE_ROOT = f"{REMOTE_WORKSPACE}/caches/package-cmake"
REMOTE_PACKAGE_LIFECYCLE_LOCK = "/run/lock/bioseq-package-builder-controller.lock"
REMOTE_PACKAGE_LIFECYCLE_LOCK_WAIT_SECONDS = 60
REMOTE_BUILD_RUNTIME_MAX = "8h"
REMOTE_BUILD_RUNTIME_MAX_SECONDS = 8 * 60 * 60
REMOTE_BUILD_STOP_TIMEOUT = "90s"
REMOTE_BUILD_WAIT_TIMEOUT_SECONDS = REMOTE_BUILD_RUNTIME_MAX_SECONDS + 5 * 60
CONTROLLER_CLEANUP_TIMEOUT_SECONDS = 5 * 60
SSM_CLEANUP_TIMEOUT_SECONDS = 120
SSM_CANCEL_RESERVE_SECONDS = 30
EC2_CLEANUP_STATE_TIMEOUT_SECONDS = 120
SOURCE_ARCHIVE_IDENTITY = "repository-root-sealed-papers-excluded"
SOURCE_ARCHIVE_REVISION = 4
PACKAGE_MATRIX_SCHEMA_REVISION = 1
WORKER_PACKAGE_MANIFEST_SCHEMA_REVISION = 1
PACKAGE_MANIFEST_SCHEMA_REVISION = 2
PACKAGE_RUN_SCHEMA_REVISION = 1
PRODUCTION_OPTIMIZATION = "O3"
PRODUCTION_OPTIMIZATION_FLAG = "-O3"
OPTIMIZATION_POLICY_REVISION = 2
INSTANCE_CONNECT_AUTH_ATTEMPTS = 10
INSTANCE_CONNECT_AUTH_RETRY_SECONDS = 2
_WORKER_PACKAGE_MANIFEST_ROOT_KEYS = frozenset({
    "schema_revision",
    "package_version",
    "architecture",
    "status",
    "started_at",
    "completed_at",
    "requested",
    "s3",
    "signing",
    "source_revision",
    "poetry_lock_sha256",
    "python_environment",
    "cells",
    "errors",
})
_PACKAGE_MANIFEST_ROOT_KEYS = _WORKER_PACKAGE_MANIFEST_ROOT_KEYS | {
    "compiler_optimization",
}
_PACKAGE_RUN_COMMON_KEYS = frozenset({
    "schema_revision",
    "status",
    "run_id",
    "revision",
    "source_revision",
    "poetry_lock_sha256",
    "worker_generation",
    "worker_generation_revision",
    "completed_at",
    "selection",
    "bucket",
    "s3_prefix",
    "artifact_count",
    "python_environments",
    "worker_units",
    "manifests",
    "optimization",
    "builders_left_running",
})
_PACKAGE_RUN_ROOT_SHAPES = (
    _PACKAGE_RUN_COMMON_KEYS | {"worker_launch_acknowledged"},
    _PACKAGE_RUN_COMMON_KEYS | {
        "worker_launch_acknowledged", "cache_images",
        "complete_matrix_fingerprint",
    },
    _PACKAGE_RUN_COMMON_KEYS | {
        "finalizer_revision", "finalized_from_completed_manifests",
        "cache_images", "complete_matrix_fingerprint",
    },
)
_CONTROLLER_ROOT_KEYS = frozenset({
    "schema_revision",
    "kind",
    "state",
    "created_at",
    "updated_at",
    "account",
    "launch_caller_arn",
    "region",
    "bucket",
    "s3_prefix",
    "store_key",
    "owner_token_sha256",
    "run_id",
    "source_revision",
    "poetry_lock_sha256",
    "matrix_fingerprint",
    "selection",
    "optimization_policy",
    "worker_generation",
    "worker_generation_revision",
    "builders",
    "diagnostics",
    "result",
})
_CONTROLLER_BUILDER_KEYS = frozenset({
    "instance_id",
    "instance_type",
    "aws_architecture",
    "source_ami_id",
    "availability_zone",
    "initial_state",
    "stop_owned",
    "unit_name",
    "remote_log_path",
    "manifest_path",
    "build_root",
    "launch_acknowledged",
    "python_environment",
})
INSTANCE_ID_RE = re.compile(r"^i-[0-9a-f]{8,17}$")
AMI_ID_RE = re.compile(r"^ami-[0-9a-f]{8,17}$")
SECURITY_GROUP_ID_RE = re.compile(r"^sg-[0-9a-f]{8,17}$")
SECURITY_GROUP_RULE_ID_RE = re.compile(r"^sgr-[0-9a-f]{8,17}$")
S3_PREFIX_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]*$")
CONTROLLER_DIAGNOSTIC_LIMIT = 4000

SOURCE_TREE_HASH_SCRIPT = r"""
import hashlib
import os
import stat
import sys

root = os.fsencode(sys.argv[1])
ignored = {
    b".bioseq-source-marker",
    b".bioseq-source-revision",
    b".bioseq-source-tree-sha256",
}
records = []

def visit(directory, prefix=b""):
    with os.scandir(directory) as iterator:
        entries = list(iterator)
    for entry in entries:
        name = entry.name
        relative = name if not prefix else prefix + b"/" + name
        metadata = entry.stat(follow_symlinks=False)
        if stat.S_ISDIR(metadata.st_mode):
            visit(entry.path, relative)
            continue
        if not prefix and name in ignored:
            continue
        if stat.S_ISLNK(metadata.st_mode):
            mode = b"120000"
            content = os.readlink(entry.path)
            if isinstance(content, str):
                content = os.fsencode(content)
            object_hash = hashlib.sha1(
                b"blob " + str(len(content)).encode("ascii") + b"\0" + content
            )
        elif stat.S_ISREG(metadata.st_mode):
            mode = b"100755" if metadata.st_mode & 0o111 else b"100644"
            object_hash = hashlib.sha1(
                b"blob " + str(metadata.st_size).encode("ascii") + b"\0"
            )
            with open(entry.path, "rb") as handle:
                while chunk := handle.read(1024 * 1024):
                    object_hash.update(chunk)
        else:
            raise SystemExit("unsupported source-tree entry: " + os.fsdecode(relative))
        object_id = object_hash.hexdigest().encode("ascii")
        records.append((relative, mode + b" blob " + object_id + b"\t" + relative + b"\0"))

visit(root)
digest = hashlib.sha256(b"".join(record for _path, record in sorted(records))).hexdigest()
print(digest)
""".strip()

REMOTE_OPTIMIZATION_EVIDENCE_SCRIPT = r"""
import hashlib
import json
import pathlib
import re
import shlex
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
architecture = sys.argv[2]
expected_cache_paths = json.loads(sys.argv[3])
if (
    not isinstance(expected_cache_paths, list)
    or not expected_cache_paths
    or not all(isinstance(path, str) and path for path in expected_cache_paths)
    or expected_cache_paths != sorted(set(expected_cache_paths))
):
    raise RuntimeError("expected production package cache paths are invalid")

resolved_cache_paths = []
for relative in expected_cache_paths:
    pure = pathlib.PurePosixPath(relative)
    if (
        pure.is_absolute()
        or ".." in pure.parts
        or len(pure.parts) != 4
        or pure.parts[2:] != ("build-repository-root", "CMakeCache.txt")
        or not pure.parts[0]
        or not pure.parts[1].startswith("postgresql")
        or not pure.parts[1].removeprefix("postgresql").isdigit()
    ):
        raise RuntimeError(
            f"invalid selected production package cache path: {relative}"
        )
    candidate = root.joinpath(*pure.parts)
    try:
        resolved = candidate.resolve(strict=True)
    except FileNotFoundError as error:
        raise RuntimeError(
            f"selected production package cache is missing: {relative}"
        ) from error
    if resolved != candidate or not resolved.is_file():
        raise RuntimeError(
            f"selected production package cache is not a canonical file: {relative}"
        )
    resolved_cache_paths.append(resolved)

def cache_value(cache_path, key):
    matches = []
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if line.startswith(key + ":"):
            separator = line.find("=")
            if separator < 0:
                raise RuntimeError("malformed CMake cache entry for " + key)
            matches.append(line[separator + 1:])
    if len(matches) != 1:
        raise RuntimeError("CMake cache does not contain exactly one " + key)
    return matches[0]

def require_o3_ndebug(tokens, label):
    optimization = sorted({token for token in tokens if token.startswith("-O")})
    embedded_forbidden = sorted({
        token
        for token in tokens
        if re.search(r"(?:^|[,=])-O(?:0|1|2|s|g)(?:$|[,])", token)
    })
    ndebug = [token for token in tokens if "NDEBUG" in token]
    frame_pointer = [token for token in tokens if "omit-frame-pointer" in token]
    if (
        optimization != ["-O3"]
        or embedded_forbidden
        or ndebug != ["-DNDEBUG"]
        or frame_pointer != ["-fno-omit-frame-pointer"]
    ):
        raise RuntimeError(
            "production compiler command must contain only -O3, exactly "
            "-DNDEBUG, and exactly -fno-omit-frame-pointer; "
            f"optimization={optimization} embedded_forbidden={embedded_forbidden} "
            f"ndebug={ndebug} frame_pointer={frame_pointer}: {label}"
        )

expected = {
    "CMAKE_BUILD_TYPE": "Release",
    "CMAKE_C_FLAGS": "",
    "CMAKE_CXX_FLAGS": "-stdlib=libc++",
    "CMAKE_C_FLAGS_RELEASE": "-O3 -DNDEBUG -fno-omit-frame-pointer",
    "CMAKE_CXX_FLAGS_RELEASE": "-O3 -DNDEBUG -fno-omit-frame-pointer",
}
cache_paths = sorted(resolved_cache_paths)
observed_cache_paths = [path.relative_to(root).as_posix() for path in cache_paths]
if observed_cache_paths != expected_cache_paths:
    raise RuntimeError(
        "production package primary CMake caches do not match the selected cells; "
        f"expected={expected_cache_paths} observed={observed_cache_paths}"
    )
observations = []
databases = []
build_files = []
for cache_path in cache_paths:
    observed = {
        key: cache_value(cache_path, key)
        for key in expected
    }
    for key, value in expected.items():
        if observed[key] != value:
            relative = cache_path.relative_to(root)
            raise RuntimeError(
                f"production package cache {relative} has "
                f"{key}={observed[key]!r}, expected {value!r}"
            )
    observed["CMAKE_C_COMPILER"] = cache_value(
        cache_path, "CMAKE_C_COMPILER"
    )
    observed["CMAKE_CXX_COMPILER"] = cache_value(
        cache_path, "CMAKE_CXX_COMPILER"
    )
    observations.append(observed)
    cache_databases = sorted(cache_path.parent.rglob("compile_commands.json"))
    if not cache_databases:
        relative = cache_path.parent.relative_to(root)
        raise RuntimeError(
            f"production package cache {relative} emitted no compile database"
        )
    for database in cache_databases:
        resolved_database = database.resolve(strict=True)
        if resolved_database != database or not resolved_database.is_file():
            relative = database.relative_to(root)
            raise RuntimeError(
                f"production package compile database is not canonical: {relative}"
            )
    databases.extend(cache_databases)
    cache_build_files = sorted(cache_path.parent.rglob("build.ninja"))
    if not cache_build_files:
        relative = cache_path.parent.relative_to(root)
        raise RuntimeError(
            f"production package cache {relative} emitted no Ninja build graph"
        )
    for build_file in cache_build_files:
        resolved_build_file = build_file.resolve(strict=True)
        if resolved_build_file != build_file or not resolved_build_file.is_file():
            relative = build_file.relative_to(root)
            raise RuntimeError(
                f"production package Ninja build graph is not canonical: {relative}"
            )
    build_files.extend(cache_build_files)
if databases != sorted(set(databases)):
    raise RuntimeError("selected production package compile databases overlap")
if build_files != sorted(set(build_files)):
    raise RuntimeError("selected production package Ninja build graphs overlap")
first_observation = observations[0]
if any(observed != first_observation for observed in observations[1:]):
    raise RuntimeError("production package caches resolved inconsistent compiler flags")
database_records = []
database_evidence = []
compile_command_count = 0
language_compile_commands = {"C": 0, "CXX": 0}
cache_language_compile_commands = {
    cache_path.parent: {"C": 0, "CXX": 0}
    for cache_path in cache_paths
}
for path in databases:
    raw = path.read_bytes()
    entries = json.loads(raw)
    if not isinstance(entries, list) or not entries:
        raise RuntimeError(f"compile database is empty: {path}")
    database_language_counts = {"C": 0, "CXX": 0}
    for entry in entries:
        if not isinstance(entry, dict):
            raise RuntimeError(f"invalid compile command in {path}")
        arguments = entry.get("arguments")
        if (
            isinstance(arguments, list)
            and all(isinstance(item, str) for item in arguments)
        ):
            tokens = list(arguments)
        elif isinstance(entry.get("command"), str):
            tokens = shlex.split(entry["command"])
        else:
            raise RuntimeError(f"compile command has no argv in {path}")
        require_o3_ndebug(tokens, entry.get("file", "<unknown>"))
        suffix = pathlib.Path(str(entry.get("file", ""))).suffix.lower()
        if suffix == ".c":
            language_compile_commands["C"] += 1
            database_language_counts["C"] += 1
        elif suffix in {".cc", ".cpp", ".cppm", ".cxx", ".ixx"}:
            language_compile_commands["CXX"] += 1
            database_language_counts["CXX"] += 1
        else:
            raise RuntimeError(
                f"production package compile command has unsupported source: "
                f"{entry.get('file', '<unknown>')}"
            )
        compile_command_count += 1
    relative = path.relative_to(root).as_posix()
    if sum(database_language_counts.values()) != len(entries):
        raise RuntimeError(
            f"compile database does not account for every C/C++ command: {relative}"
        )
    owners = [
        cache_root
        for cache_root in cache_language_compile_commands
        if path.is_relative_to(cache_root)
    ]
    if len(owners) != 1:
        raise RuntimeError(
            f"compile database does not have one selected cache owner: {relative}"
        )
    for language in ("C", "CXX"):
        cache_language_compile_commands[owners[0]][language] += (
            database_language_counts[language]
        )
    digest = hashlib.sha256(raw).hexdigest()
    database_records.append(
        f"{relative}\0{digest}\0{len(entries)}"
    )
    database_evidence.append(
        {
            "path": relative,
            "sha256": digest,
            "command_count": len(entries),
            "language_compile_commands": database_language_counts,
        }
    )
for cache_root, counts in cache_language_compile_commands.items():
    if any(counts[language] <= 0 for language in ("C", "CXX")):
        relative = cache_root.relative_to(root).as_posix()
        raise RuntimeError(
            f"selected production package cache lacks C/C++ coverage: {relative}"
        )

compiler_paths = {
    first_observation["CMAKE_C_COMPILER"],
    first_observation["CMAKE_CXX_COMPILER"],
}
ninja_build_evidence = []
ninja_digest_records = []
ninja_compile_command_count = 0
link_command_count = 0
cache_ninja_command_counts = {
    cache_path.parent: {"compile": 0, "link": 0}
    for cache_path in cache_paths
}
for build_file in build_files:
    relative = build_file.relative_to(root).as_posix()
    owners = [
        cache_root
        for cache_root in cache_ninja_command_counts
        if build_file.is_relative_to(cache_root)
    ]
    if len(owners) != 1:
        raise RuntimeError(
            f"Ninja build graph does not have one selected cache owner: {relative}"
        )
    try:
        commands = subprocess.run(
            ["ninja", "-C", str(build_file.parent), "-t", "commands"],
            check=False,
            capture_output=True,
            text=True,
            timeout=120,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"Ninja command graph timed out: {relative}"
        ) from error
    if commands.returncode != 0:
        raise RuntimeError(
            f"Ninja command graph failed for {relative}: "
            f"{commands.stderr[-1000:]}"
        )
    file_compile_count = 0
    file_link_count = 0
    file_command_records = []
    for line_number, line in enumerate(commands.stdout.splitlines(), 1):
        try:
            tokens = shlex.split(line)
        except ValueError as error:
            raise RuntimeError(
                f"Ninja command graph has invalid shell syntax: "
                f"{relative}:{line_number}"
            ) from error
        for compiler_index, token in enumerate(tokens):
            if token not in compiler_paths:
                continue
            command_end = len(tokens)
            for index in range(compiler_index + 1, len(tokens)):
                if tokens[index] in {"&&", "||", ";"}:
                    command_end = index
                    break
            argv = tokens[compiler_index:command_end]
            is_compile = any(
                marker in argv
                for marker in ("-c", "-E", "-S", "--precompile", "-fsyntax-only")
            )
            is_link = not is_compile and "-o" in argv
            # Compiler paths can also be provenance arguments to code generators.
            # Only argv segments that actually compile or link are policy evidence.
            if not is_compile and not is_link:
                continue
            kind = "compile" if is_compile else "link"
            require_o3_ndebug(argv, f"{relative}:{line_number}:{kind}")
            command_digest = hashlib.sha256(
                "\0".join(argv).encode("utf-8")
            ).hexdigest()
            file_command_records.append(
                f"{line_number}\0{kind}\0{command_digest}"
            )
            if is_compile:
                file_compile_count += 1
                ninja_compile_command_count += 1
                cache_ninja_command_counts[owners[0]]["compile"] += 1
            else:
                file_link_count += 1
                link_command_count += 1
                cache_ninja_command_counts[owners[0]]["link"] += 1
    build_file_digest = hashlib.sha256(build_file.read_bytes()).hexdigest()
    command_set_digest = hashlib.sha256(
        "\n".join(file_command_records).encode("utf-8")
    ).hexdigest()
    ninja_build_evidence.append(
        {
            "path": relative,
            "sha256": build_file_digest,
            "compile_command_count": file_compile_count,
            "link_command_count": file_link_count,
            "compiler_commands_sha256": command_set_digest,
        }
    )
    ninja_digest_records.append(
        f"{relative}\0{build_file_digest}\0{file_compile_count}\0"
        f"{file_link_count}\0{command_set_digest}"
    )
for cache_root, counts in cache_ninja_command_counts.items():
    if counts["compile"] <= 0 or counts["link"] <= 0:
        relative = cache_root.relative_to(root).as_posix()
        raise RuntimeError(
            f"selected production package cache lacks Ninja compile/link "
            f"coverage: {relative}"
        )

effective_c_flags = shlex.split(first_observation["CMAKE_C_FLAGS"])
effective_c_flags += shlex.split(first_observation["CMAKE_C_FLAGS_RELEASE"])
effective_cxx_flags = shlex.split(first_observation["CMAKE_CXX_FLAGS"])
effective_cxx_flags += shlex.split(
    first_observation["CMAKE_CXX_FLAGS_RELEASE"]
)
result = {
    "policy_revision": 2,
    "requested": "O3",
    "architecture": architecture,
    "build_type": first_observation["CMAKE_BUILD_TYPE"],
    "c_compiler": first_observation["CMAKE_C_COMPILER"],
    "cxx_compiler": first_observation["CMAKE_CXX_COMPILER"],
    "effective_c_flags": effective_c_flags,
    "effective_cxx_flags": effective_cxx_flags,
    "optimization_flags": ["-O3"],
    "forbidden_optimization_flags": [],
    "cmake_cache_paths": observed_cache_paths,
    "compile_database_count": len(databases),
    "compile_databases": database_evidence,
    "compile_command_count": compile_command_count,
    "language_compile_commands": language_compile_commands,
    "compile_databases_sha256": hashlib.sha256(
        "\n".join(database_records).encode("utf-8")
    ).hexdigest(),
    "ninja_build_file_count": len(build_files),
    "ninja_build_files": ninja_build_evidence,
    "ninja_compile_command_count": ninja_compile_command_count,
    "link_command_count": link_command_count,
    "ninja_commands_sha256": hashlib.sha256(
        "\n".join(ninja_digest_records).encode("utf-8")
    ).hexdigest(),
}
print(json.dumps(result, sort_keys=True))
""".strip()

REMOTE_PACKAGE_ACTIVITY_PROBE_SCRIPT = r"""
import json
import pathlib
import re
import subprocess
import sys

workspace = pathlib.Path(sys.argv[1])
if not workspace.is_absolute():
    raise RuntimeError("package workspace must be absolute")

worker_pids = []
for process in pathlib.Path("/proc").glob("[0-9]*"):
    try:
        raw = process.joinpath("cmdline").read_bytes()
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        continue
    argv = [
        value.decode("utf-8", "replace")
        for value in raw.rstrip(b"\0").split(b"\0")
        if value
    ]
    if any(value.endswith("/build_packages.py") for value in argv):
        worker_pids.append(int(process.name))

listed = subprocess.run(
    [
        "systemctl", "list-units", "--type=service", "--all", "--plain",
        "--no-legend", "--no-pager", "bioseq-package-*",
    ],
    check=True,
    capture_output=True,
    text=True,
)
unit_names = []
for line in listed.stdout.splitlines():
    fields = line.removeprefix("● ").split()
    if fields and re.fullmatch(r"bioseq-package-[A-Za-z0-9_.@-]+\.service", fields[0]):
        unit_names.append(fields[0])
active_units = []
for unit_name in sorted(set(unit_names)):
    status = subprocess.run(
        ["systemctl", "show", unit_name, "--property=ActiveState", "--value"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if status not in {"inactive", "failed"}:
        active_units.append(f"{unit_name}:{status}")

container_ids = subprocess.run(
    ["docker", "ps", "--no-trunc", "--quiet"],
    check=True,
    capture_output=True,
    text=True,
).stdout.split()
containers = []
if container_ids:
    inspected = subprocess.run(
        ["docker", "inspect", *container_ids],
        check=True,
        capture_output=True,
        text=True,
    )
    rows = json.loads(inspected.stdout)
    if not isinstance(rows, list) or len(rows) != len(container_ids):
        raise RuntimeError("Docker package-activity probe returned malformed rows")
    workspace_prefix = str(workspace) + "/"
    for row in rows:
        if not isinstance(row, dict):
            raise RuntimeError("Docker package-activity probe returned a malformed row")
        container_id = str(row.get("Id", ""))
        name = str(row.get("Name", "")).removeprefix("/")
        mounts = row.get("Mounts", [])
        if not isinstance(mounts, list):
            raise RuntimeError("Docker package-activity probe returned malformed mounts")
        workspace_mount = any(
            isinstance(mount, dict)
            and mount.get("Type") == "bind"
            and isinstance(mount.get("Source"), str)
            and (
                mount["Source"] == str(workspace)
                or mount["Source"].startswith(workspace_prefix)
            )
            for mount in mounts
        )
        package_name = re.fullmatch(
            r"bioseq-(?:arm64|x64)-pg[0-9]+-[A-Za-z0-9_.-]+", name
        ) is not None
        if workspace_mount or package_name:
            containers.append(f"{container_id}:{name}")

result = {
    "worker_pids": sorted(worker_pids),
    "active_units": sorted(active_units),
    "containers": sorted(containers),
}
print(json.dumps(result, sort_keys=True))
if any(result.values()):
    raise SystemExit(70)
""".strip()

REMOTE_CONTROLLER_CLEANUP_SCRIPT = r"""
import hashlib
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import time

workspace = pathlib.PurePosixPath(sys.argv[1])
unit_name = sys.argv[2]
build_root = pathlib.PurePosixPath(sys.argv[3])
expected_worker_argv = json.loads(sys.argv[4])
remote_log_path = pathlib.PurePosixPath(sys.argv[5])
manifest_path = pathlib.PurePosixPath(sys.argv[6])

if (
    not workspace.is_absolute()
    or not build_root.is_absolute()
    or build_root.parent != workspace / "builds"
    or remote_log_path != build_root / "build.log"
    or not manifest_path.is_absolute()
    or manifest_path.parent != workspace / "builds"
    or re.fullmatch(r"bioseq-package-[A-Za-z0-9_.@-]+\.service", unit_name) is None
    or not isinstance(expected_worker_argv, list)
    or any(not isinstance(value, str) or not value for value in expected_worker_argv)
):
    raise RuntimeError("receipt-bound package cleanup identity is malformed")


def run(argv, *, check=True, timeout=30):
    return subprocess.run(
        argv,
        check=check,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def unit_status():
    result = run(
        [
            "systemctl",
            "show",
            unit_name,
            "--property=LoadState",
            "--property=ActiveState",
            "--property=SubState",
            "--property=Result",
            "--property=ExecMainStatus",
        ],
        check=False,
    )
    values = {}
    for line in result.stdout.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            values[key] = value
    expected = {"LoadState", "ActiveState", "SubState", "Result", "ExecMainStatus"}
    if set(values) != expected:
        raise RuntimeError("systemd returned incomplete exact-unit status")
    return values


def process_table():
    rows = {}
    for entry in pathlib.Path("/proc").glob("[0-9]*"):
        try:
            raw_argv = entry.joinpath("cmdline").read_bytes()
            raw_stat = entry.joinpath("stat").read_text(encoding="ascii")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        argv = [
            value.decode("utf-8", "surrogateescape")
            for value in raw_argv.rstrip(b"\0").split(b"\0")
            if value
        ]
        closing = raw_stat.rfind(")")
        fields = raw_stat[closing + 2 :].split() if closing >= 0 else []
        if len(fields) < 20:
            continue
        rows[int(entry.name)] = {
            "argv": argv,
            "ppid": int(fields[1]),
            "starttime": fields[19],
        }
    return rows


def exact_worker_roots(rows):
    if not expected_worker_argv:
        return []
    return sorted(
        pid for pid, value in rows.items() if value["argv"] == expected_worker_argv
    )


def terminate_exact_workers():
    rows = process_table()
    roots = exact_worker_roots(rows)
    owned = set(roots)
    changed = True
    while changed:
        changed = False
        for pid, value in rows.items():
            if pid not in owned and value["ppid"] in owned:
                owned.add(pid)
                changed = True
    identities = {pid: rows[pid]["starttime"] for pid in owned}

    def still_owned(pid):
        current = process_table().get(pid)
        return current is not None and current["starttime"] == identities[pid]

    for pid in sorted(owned, reverse=True):
        if still_owned(pid):
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline and any(still_owned(pid) for pid in owned):
        time.sleep(0.2)
    for pid in sorted(owned, reverse=True):
        if still_owned(pid):
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and any(still_owned(pid) for pid in owned):
        time.sleep(0.1)
    survivors = sorted(pid for pid in owned if still_owned(pid))
    if survivors:
        raise RuntimeError(f"receipt-owned worker descendants survived: {survivors}")
    return roots, sorted(owned)


def exact_build_containers():
    listed = run(["docker", "ps", "--no-trunc", "--quiet"])
    container_ids = listed.stdout.split()
    if not container_ids:
        return []
    inspected = json.loads(run(["docker", "inspect", *container_ids]).stdout)
    if not isinstance(inspected, list) or len(inspected) != len(container_ids):
        raise RuntimeError("Docker returned malformed cleanup identities")
    prefix = str(build_root) + "/"
    selected = []
    for row in inspected:
        if not isinstance(row, dict) or not isinstance(row.get("Mounts"), list):
            raise RuntimeError("Docker returned a malformed cleanup row")
        container_id = str(row.get("Id", ""))
        if re.fullmatch(r"[0-9a-f]{12,64}", container_id) is None:
            raise RuntimeError("Docker returned a malformed container ID")
        if any(
            isinstance(mount, dict)
            and mount.get("Type") == "bind"
            and isinstance(mount.get("Source"), str)
            and (
                mount["Source"] == str(build_root)
                or mount["Source"].startswith(prefix)
            )
            for mount in row["Mounts"]
        ):
            selected.append(container_id)
    return sorted(selected)


def stop_exact_containers():
    selected = exact_build_containers()
    if selected:
        run(["docker", "stop", "--time", "30", *selected], check=False, timeout=45)
        survivors = exact_build_containers()
        if survivors:
            run(["docker", "kill", *survivors], check=False, timeout=15)
    survivors = exact_build_containers()
    if survivors:
        raise RuntimeError(f"receipt-owned containers survived: {survivors}")
    return selected


def artifact_evidence(path):
    candidate = pathlib.Path(path)
    try:
        metadata = candidate.stat()
    except FileNotFoundError:
        return {"present": False, "size": 0, "sha256": None}
    if not candidate.is_file():
        raise RuntimeError(f"cleanup evidence path is not a file: {path}")
    digest = None
    if candidate == pathlib.Path(manifest_path):
        digest = hashlib.sha256(candidate.read_bytes()).hexdigest()
    return {"present": True, "size": metadata.st_size, "sha256": digest}


before = unit_status()
unit_cancelled = before["LoadState"] == "loaded" and before["ActiveState"] not in {
    "inactive",
    "failed",
}
if unit_cancelled:
    try:
        run(["systemctl", "stop", unit_name], timeout=100)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        run(
            ["systemctl", "kill", "--kill-who=all", "--signal=KILL", unit_name],
            check=False,
        )

containers = stop_exact_containers()
worker_roots, worker_processes = terminate_exact_workers()
after = unit_status()
if after["LoadState"] == "loaded" and after["ActiveState"] not in {"inactive", "failed"}:
    run(
        ["systemctl", "kill", "--kill-who=all", "--signal=KILL", unit_name],
        check=False,
    )
    run(["systemctl", "stop", unit_name], check=False, timeout=15)
    after = unit_status()
if after["LoadState"] == "loaded" and after["ActiveState"] not in {"inactive", "failed"}:
    raise RuntimeError(f"receipt-owned unit remained active: {after}")
if exact_worker_roots(process_table()):
    raise RuntimeError("receipt-owned worker root survived cleanup")
if exact_build_containers():
    raise RuntimeError("receipt-owned build container survived cleanup")

result = {
    "unit_name": unit_name,
    "build_root": str(build_root),
    "unit_before": before,
    "unit_after": after,
    "unit_cancelled": unit_cancelled,
    "worker_roots": worker_roots,
    "worker_processes": worker_processes,
    "containers": containers,
    "log": artifact_evidence(remote_log_path),
    "manifest": artifact_evidence(manifest_path),
    "build_root_preserved": pathlib.Path(build_root).is_dir(),
}
print(json.dumps(result, sort_keys=True))
""".strip()

RUN_ID_RE = re.compile(r"^(?P<timestamp>\d{8}T\d{6}Z)-(?P<revision>[0-9a-f]{12})$")
PACKAGE_UNIT_RE = re.compile(r"^bioseq-package-[A-Za-z0-9_.@-]+\.service$")
PACKAGE_CONTAINER_NAME_RE = re.compile(
    r"^/?bioseq-(?:arm64|x64)-pg[0-9]+-"
    r"(?:opensuse_16\.0|fedora_44|debian_13|ubuntu_26\.04|arch)-"
    r"(?:build|rpm-build|deb-repository|rpm-repository|pacman-repository|"
    r"repository|validation|smoke)$"
)
FINALIZE_ALLOWED_COMMITTED_DELTAS = frozenset(
    {
        ".github/workflows/package-release.yml",
        "infra/package_builder.py",
        "infra/tests/test_package_builder.py",
    }
)
FINALIZE_ALLOWED_COMMITTED_PREFIXES = ("DOCS/",)
ACTIVE_CHILDREN: set[subprocess.Popen[str]] = set()
ACTIVE_CHILDREN_LOCK = threading.RLock()


class PackageBuilderError(RuntimeError):
    """An actionable, fail-closed package-builder error."""


def require_exact_keys(
    value: dict[str, Any], expected: frozenset[str] | set[str], label: str,
) -> None:
    actual = set(value)
    if actual != expected:
        raise PackageBuilderError(
            f"{label} has unexpected keys: "
            f"missing={sorted(expected - actual)} extra={sorted(actual - expected)}"
        )


def require_current_package_manifest(
    value: object, *, label: str,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise PackageBuilderError(f"{label} must be an object")
    require_exact_keys(value, _PACKAGE_MANIFEST_ROOT_KEYS, label)
    revision = value["schema_revision"]
    if (
        type(revision) is not int
        or revision != PACKAGE_MANIFEST_SCHEMA_REVISION
    ):
        raise PackageBuilderError(
            f"{label} schema_revision must be integer revision "
            f"{PACKAGE_MANIFEST_SCHEMA_REVISION}"
        )
    return value


def require_worker_package_manifest(
    value: object, *, label: str,
) -> dict[str, Any]:
    """Validate the packaging worker's pre-controller manifest envelope."""

    if not isinstance(value, dict):
        raise PackageBuilderError(f"{label} must be an object")
    require_exact_keys(value, _WORKER_PACKAGE_MANIFEST_ROOT_KEYS, label)
    revision = value["schema_revision"]
    if (
        type(revision) is not int
        or revision != WORKER_PACKAGE_MANIFEST_SCHEMA_REVISION
    ):
        raise PackageBuilderError(
            f"{label} schema_revision must be integer revision "
            f"{WORKER_PACKAGE_MANIFEST_SCHEMA_REVISION}"
        )
    return value


def require_current_package_run_result(
    value: object, *, label: str,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise PackageBuilderError(f"{label} must be an object")
    actual = set(value)
    if actual not in _PACKAGE_RUN_ROOT_SHAPES:
        expected = min(
            _PACKAGE_RUN_ROOT_SHAPES,
            key=lambda candidate: len(actual ^ candidate),
        )
        raise PackageBuilderError(
            f"{label} has unexpected keys: "
            f"missing={sorted(expected - actual)} "
            f"extra={sorted(actual - expected)}"
        )
    revision = value["schema_revision"]
    worker_revision = value["worker_generation_revision"]
    if type(revision) is not int or revision != PACKAGE_RUN_SCHEMA_REVISION:
        raise PackageBuilderError(
            f"{label} schema_revision must be integer revision "
            f"{PACKAGE_RUN_SCHEMA_REVISION}"
        )
    if (
        value["worker_generation"] != WORKER_GENERATION
        or type(worker_revision) is not int
        or worker_revision != WORKER_GENERATION_REVISION
    ):
        raise PackageBuilderError(f"{label} worker generation is not current")
    return value


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def info(message: str) -> None:
    print(f"[package-builder] {message}", flush=True)


def command_text(argv: Sequence[str]) -> str:
    return shlex.join(str(arg) for arg in argv)


def run_command(
    argv: Sequence[str],
    *,
    cwd: Path | None = None,
    capture: bool = False,
    check: bool = True,
    env: dict[str, str] | None = None,
    stdin: Any = None,
    stdout: Any = None,
) -> subprocess.CompletedProcess[str]:
    info(f"$ {command_text(argv)}")
    if capture and stdout is not None:
        raise PackageBuilderError("run_command cannot combine capture=True with stdout")
    process = subprocess.Popen(
        [str(arg) for arg in argv],
        cwd=str(cwd) if cwd else None,
        text=True,
        env=env,
        stdin=stdin,
        stdout=subprocess.PIPE if capture else stdout,
        stderr=subprocess.PIPE if capture else None,
    )
    with ACTIVE_CHILDREN_LOCK:
        ACTIVE_CHILDREN.add(process)
    try:
        captured_stdout, captured_stderr = process.communicate()
    finally:
        with ACTIVE_CHILDREN_LOCK:
            ACTIVE_CHILDREN.discard(process)
    completed = subprocess.CompletedProcess(
        [str(arg) for arg in argv],
        process.returncode,
        captured_stdout,
        captured_stderr,
    )
    if check and completed.returncode != 0:
        detail = ""
        if capture:
            detail = (completed.stderr or completed.stdout or "").strip()
        suffix = f": {detail}" if detail else ""
        raise PackageBuilderError(
            f"command failed with exit {completed.returncode}: "
            f"{command_text(argv)}{suffix}"
        )
    return completed


def interrupt_active_children() -> None:
    """Forward an interrupt so OpenTofu/SSH can flush state and exit first."""

    with ACTIVE_CHILDREN_LOCK:
        children = list(ACTIVE_CHILDREN)
    for child in children:
        if child.poll() is None:
            with contextlib.suppress(ProcessLookupError):
                child.send_signal(signal.SIGINT)
    deadline = time.monotonic() + 30
    for child in children:
        if child.poll() is not None:
            continue
        remaining = max(0.0, deadline - time.monotonic())
        try:
            child.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            # SIGINT is OpenTofu's state-flushing interrupt. Escalate only the
            # local child process to TERM (never KILL); EC2 lifecycle is not
            # changed by signal cleanup.
            with contextlib.suppress(ProcessLookupError):
                child.terminate()


def require_executable(name: str) -> str:
    value = shutil.which(name)
    if value is None:
        raise PackageBuilderError(f"required executable `{name}` is not on PATH")
    return value


def tofu_executable() -> str:
    """Return OpenTofu's executable, deliberately with no alternate fallback."""

    configured = os.environ.get("BIOSEQ_TOFU_BIN")
    if configured:
        path = Path(configured).expanduser().resolve()
        if path.name != "tofu" or not path.is_file() or not os.access(path, os.X_OK):
            raise PackageBuilderError(
                "BIOSEQ_TOFU_BIN must name an executable whose basename is `tofu`"
            )
        return str(path)
    return require_executable("tofu")


def aws_json(region: str, service: str, operation: str, *arguments: str) -> Any:
    argv = [
        require_executable("aws"),
        "--region",
        region,
        "--no-cli-pager",
        service,
        operation,
        *arguments,
        "--output",
        "json",
    ]
    result = run_command(argv, capture=True)
    try:
        return json.loads(result.stdout or "{}")
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"AWS CLI returned invalid JSON for {service} {operation}: {error}"
        ) from error


def aws_no_output(region: str, service: str, operation: str, *arguments: str) -> None:
    argv = [
        require_executable("aws"),
        "--region",
        region,
        "--no-cli-pager",
        service,
        operation,
        *arguments,
        "--output",
        "json",
    ]
    run_command(argv)


def detect_public_ipv4() -> str:
    for url in ("https://checkip.amazonaws.com", "https://api.ipify.org"):
        try:
            with urllib.request.urlopen(url, timeout=10) as response:
                value = response.read().decode("ascii").strip()
            address = ipaddress.ip_address(value)
            if address.version == 4 and not address.is_unspecified:
                return str(address)
        except (OSError, ValueError):
            continue
    raise PackageBuilderError(
        "could not detect a public IPv4 address; pass --ssh-cidr as an explicit /32"
    )


def normalize_ssh_cidr(value: str | None) -> str:
    if value is None:
        value = f"{detect_public_ipv4()}/32"
    try:
        network = ipaddress.ip_network(value, strict=True)
    except ValueError as error:
        raise PackageBuilderError(f"invalid --ssh-cidr {value!r}: {error}") from error
    if network.version != 4 or network.prefixlen != 32:
        raise PackageBuilderError("--ssh-cidr must be one explicit IPv4 /32")
    return str(network)


def ci_access_owner(owner_token: str) -> tuple[str, str]:
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._:/-]{0,239}", owner_token) is None:
        raise PackageBuilderError("CI access owner token is invalid")
    owner_sha256 = hashlib.sha256(owner_token.encode("utf-8")).hexdigest()
    description = f"{CI_ACCESS_DESCRIPTION} owner={owner_sha256[:24]}"
    return owner_sha256, description


@dataclass(frozen=True)
class Architecture:
    name: str
    aws_architecture: str
    artifact_architecture: str
    instance_type: str


@dataclass(frozen=True)
class Distribution:
    distro_id: str
    package_format: str


@dataclass(frozen=True)
class Matrix:
    architectures: dict[str, Architecture]
    distributions: dict[str, Distribution]
    postgresql_majors: tuple[int, ...]


@dataclass(frozen=True)
class Selection:
    architectures: tuple[str, ...]
    distributions: tuple[str, ...]
    postgresql_majors: tuple[int, ...]


@dataclass(frozen=True)
class Builder:
    architecture: str
    instance_id: str
    instance_type: str
    aws_architecture: str
    public_ip: str
    private_ip: str
    source_ami_id: str
    availability_zone: str = ""
    region: str = DEFAULT_REGION
    ssh_access: str = "static-key"


@dataclass(frozen=True)
class PoetryEnvironment:
    lock_sha256: str
    venv_path: str
    python_version: str
    poetry_version: str

    def manifest_value(self) -> dict[str, str]:
        return {
            "python": self.python_version,
            "poetry": self.poetry_version,
        }


@dataclass(frozen=True)
class RemoteBuildHandle:
    builder: Builder
    unit_name: str
    remote_log_path: str
    manifest_path: str
    build_root: str
    local_log_path: Path
    worker_argv: tuple[str, ...]
    launch_acknowledged: bool


def load_matrix(path: Path = MATRIX_FILE) -> Matrix:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PackageBuilderError(f"cannot load package matrix {path}: {error}") from error
    if not isinstance(raw, dict):
        raise PackageBuilderError("package matrix must be an object")
    require_exact_keys(
        raw,
        {"schema_revision", "postgresql_majors", "architectures", "distributions"},
        "package matrix",
    )
    if (
        type(raw["schema_revision"]) is not int
        or raw["schema_revision"] != PACKAGE_MATRIX_SCHEMA_REVISION
    ):
        raise PackageBuilderError(
            "package matrix schema_revision must be integer revision "
            f"{PACKAGE_MATRIX_SCHEMA_REVISION}"
        )

    architecture_rows = raw.get("architectures")
    if not isinstance(architecture_rows, dict) or set(architecture_rows) != {
        "arm64",
        "x64",
    }:
        raise PackageBuilderError("package matrix must define exactly arm64 and x64")
    architectures: dict[str, Architecture] = {}
    expected = {
        "arm64": ("arm64", "aarch64", "c7g.xlarge"),
        "x64": ("x86_64", "x86_64", "m4.xlarge"),
    }
    for name, row in architecture_rows.items():
        if not isinstance(row, dict):
            raise PackageBuilderError(f"matrix architecture {name!r} must be an object")
        require_exact_keys(
            row,
            {"aws_architecture", "artifact_architecture", "instance_type"},
            f"matrix architecture {name!r}",
        )
        values = (
            row.get("aws_architecture"),
            row.get("artifact_architecture"),
            row.get("instance_type"),
        )
        if values != expected[name]:
            raise PackageBuilderError(
                f"matrix architecture {name!r} must be {expected[name]}, got {values}"
            )
        architectures[name] = Architecture(name, *values)

    distribution_rows = raw.get("distributions")
    if not isinstance(distribution_rows, list) or not distribution_rows:
        raise PackageBuilderError("package matrix distributions must be a non-empty list")
    distributions: dict[str, Distribution] = {}
    allowed_formats = {"deb", "rpm", "pacman"}
    for row in distribution_rows:
        if not isinstance(row, dict):
            raise PackageBuilderError("every matrix distribution must be an object")
        require_exact_keys(row, {"id", "package_format"}, "matrix distribution")
        distro_id = row.get("id")
        package_format = row.get("package_format")
        if not isinstance(distro_id, str) or not distro_id:
            raise PackageBuilderError("every matrix distribution needs a non-empty id")
        if distro_id in distributions:
            raise PackageBuilderError(f"duplicate matrix distribution {distro_id!r}")
        if package_format not in allowed_formats:
            raise PackageBuilderError(
                f"distribution {distro_id!r} has invalid package format {package_format!r}"
            )
        distributions[distro_id] = Distribution(distro_id, package_format)

    required_distros = {
        "opensuse_16.0": "rpm",
        "fedora_44": "rpm",
        "debian_13": "deb",
        "ubuntu_26.04": "deb",
        "arch": "pacman",
    }
    actual_distros = {key: value.package_format for key, value in distributions.items()}
    if actual_distros != required_distros:
        raise PackageBuilderError(
            f"matrix distributions must be exactly {required_distros}, got {actual_distros}"
        )

    pg_rows = raw.get("postgresql_majors")
    if (
        not isinstance(pg_rows, list)
        or not pg_rows
        or any(not isinstance(item, int) or isinstance(item, bool) or item < 17 for item in pg_rows)
        or len(pg_rows) != len(set(pg_rows))
    ):
        raise PackageBuilderError(
            "postgresql_majors must be unique integer majors greater than or equal to 17"
        )
    return Matrix(architectures, distributions, tuple(pg_rows))


def csv_values(values: Sequence[str] | None) -> list[str]:
    result: list[str] = []
    for value in values or ():
        result.extend(part.strip() for part in value.split(",") if part.strip())
    return result


def select_matrix(args: argparse.Namespace, matrix: Matrix) -> Selection:
    architectures = csv_values(getattr(args, "architectures", None)) or list(
        matrix.architectures
    )
    distributions = csv_values(getattr(args, "distros", None)) or list(
        matrix.distributions
    )
    pg_values = csv_values(getattr(args, "postgres", None))
    postgresql_majors: list[int] = []
    try:
        postgresql_majors = (
            [int(value) for value in pg_values]
            if pg_values
            else list(matrix.postgresql_majors)
        )
    except ValueError as error:
        raise PackageBuilderError("--postgres accepts integer major versions") from error

    unknown_arches = sorted(set(architectures) - set(matrix.architectures))
    unknown_distros = sorted(set(distributions) - set(matrix.distributions))
    if unknown_arches:
        raise PackageBuilderError(f"unknown architectures requested: {unknown_arches}")
    if unknown_distros:
        raise PackageBuilderError(f"unknown distributions requested: {unknown_distros}")
    if len(architectures) != len(set(architectures)):
        raise PackageBuilderError("architectures must not be repeated")
    if len(distributions) != len(set(distributions)):
        raise PackageBuilderError("distributions must not be repeated")
    if (
        not postgresql_majors
        or any(major < 17 for major in postgresql_majors)
        or len(postgresql_majors) != len(set(postgresql_majors))
    ):
        raise PackageBuilderError("PostgreSQL majors must be unique and at least 17")
    return Selection(
        tuple(architectures), tuple(distributions), tuple(postgresql_majors)
    )


def tofu_init() -> None:
    run_command(
        [tofu_executable(), "init", "-input=false"], cwd=STACK_DIR
    )


def tofu_outputs() -> dict[str, Any]:
    result = run_command(
        [tofu_executable(), "output", "-json"],
        cwd=STACK_DIR,
        capture=True,
        check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        return {}
    try:
        raw = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(f"OpenTofu output is not valid JSON: {error}") from error
    if not isinstance(raw, dict):
        raise PackageBuilderError("OpenTofu output must be a JSON object")
    return {
        key: value.get("value") if isinstance(value, dict) else None
        for key, value in raw.items()
    }


def _walk_state_modules(module: dict[str, Any]) -> Iterable[dict[str, Any]]:
    resources = module.get("resources", [])
    if isinstance(resources, list):
        for resource in resources:
            if isinstance(resource, dict):
                yield resource
    children = module.get("child_modules", [])
    if isinstance(children, list):
        for child in children:
            if isinstance(child, dict):
                yield from _walk_state_modules(child)


def tofu_state_builders(matrix: Matrix) -> dict[str, Builder]:
    """Recover only exact package-builder resources after a partial apply.

    Aggregate outputs can be unknown when one side of the ``for_each`` failed.
    OpenTofu's JSON state still records each instance that did succeed. No
    sensitive state content is printed or persisted by this recovery path.
    """

    result = run_command(
        [tofu_executable(), "show", "-json"],
        cwd=STACK_DIR,
        capture=True,
        check=False,
    )
    if result.returncode != 0:
        local_state_exists = any(
            path.is_file() and path.stat().st_size > 0
            for path in STACK_DIR.glob("*.tfstate*")
        )
        if not local_state_exists:
            return {}
        detail = (result.stderr or result.stdout or "").strip()
        raise PackageBuilderError(
            "cannot inspect non-empty OpenTofu state while recovering builders"
            + (f": {detail}" if detail else "")
        )
    if not result.stdout.strip():
        return {}
    try:
        state = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(f"OpenTofu state JSON is malformed: {error}") from error
    values = state.get("values", {}) if isinstance(state, dict) else {}
    root_module = values.get("root_module", {}) if isinstance(values, dict) else {}
    if not isinstance(root_module, dict):
        return {}
    expected_addresses = {
        f'aws_instance.builder["{name}"]': name for name in matrix.architectures
    }
    builders: dict[str, Builder] = {}
    for resource in _walk_state_modules(root_module):
        address = resource.get("address")
        if address not in expected_addresses:
            continue
        name = expected_addresses[address]
        if name in builders:
            raise PackageBuilderError(
                f"OpenTofu state contains duplicate builder resource for {name}"
            )
        resource_values = resource.get("values", {})
        if not isinstance(resource_values, dict):
            raise PackageBuilderError(
                f"OpenTofu state values for {name} are malformed"
            )
        instance_id = str(resource_values.get("id", ""))
        if not INSTANCE_ID_RE.match(instance_id):
            # A planned/unknown resource is not an actionable instance.
            continue
        if instance_id in PROTECTED_INSTANCE_IDS:
            raise PackageBuilderError(
                f"refusing protected development instance {instance_id} in OpenTofu state"
            )
        architecture = matrix.architectures[name]
        builders[name] = Builder(
            architecture=name,
            instance_id=instance_id,
            instance_type=architecture.instance_type,
            aws_architecture=architecture.aws_architecture,
            public_ip=str(resource_values.get("public_ip") or ""),
            private_ip=str(resource_values.get("private_ip") or ""),
            source_ami_id=str(resource_values.get("ami") or ""),
        )
    return builders


def recover_managed_builders(matrix: Matrix) -> dict[str, Builder]:
    outputs = tofu_outputs()
    if outputs:
        try:
            return output_builders(outputs, matrix)
        except PackageBuilderError as output_error:
            info(f"aggregate builder output unavailable; recovering exact state: {output_error}")
    return tofu_state_builders(matrix)


def guard_local_state_against_protected_instances() -> None:
    """Reject a poisoned/imported local state before OpenTofu can mutate it."""

    for state_path in STACK_DIR.glob("*.tfstate*"):
        if not state_path.is_file():
            continue
        try:
            state_text = state_path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            raise PackageBuilderError(
                f"cannot inspect existing OpenTofu state {state_path.name}: {error}"
            ) from error
        protected = sorted(
            instance_id
            for instance_id in PROTECTED_INSTANCE_IDS
            if instance_id in state_text
        )
        if protected:
            raise PackageBuilderError(
                "refusing OpenTofu apply because local state references protected "
                f"development instance(s): {protected}"
            )


def validate_tofu_plan(plan: dict[str, Any]) -> None:
    """Reject any plan that could replace/delete a builder or touch protected IDs."""

    if not isinstance(plan, dict):
        raise PackageBuilderError("OpenTofu plan JSON must be an object")
    plan_text = json.dumps(plan, separators=(",", ":"), sort_keys=True)
    protected = sorted(
        instance_id for instance_id in PROTECTED_INSTANCE_IDS if instance_id in plan_text
    )
    if protected:
        raise PackageBuilderError(
            "refusing OpenTofu plan because it references protected development "
            f"instance(s): {protected}"
        )
    resource_changes = plan.get("resource_changes", [])
    if not isinstance(resource_changes, list):
        raise PackageBuilderError("OpenTofu plan has malformed resource_changes")
    forbidden: list[str] = []
    for resource in resource_changes:
        if not isinstance(resource, dict):
            raise PackageBuilderError("OpenTofu plan contains a malformed resource change")
        address = str(resource.get("address", ""))
        resource_type = str(resource.get("type", ""))
        if resource_type != "aws_instance" and not address.startswith("aws_instance."):
            continue
        change = resource.get("change", {})
        actions = change.get("actions", []) if isinstance(change, dict) else []
        if not isinstance(actions, list) or any(not isinstance(action, str) for action in actions):
            raise PackageBuilderError(
                f"OpenTofu plan has malformed actions for {address or resource_type}"
            )
        if "delete" in actions:
            forbidden.append(f"{address or resource_type}: {actions}")
    if forbidden:
        raise PackageBuilderError(
            "refusing OpenTofu instance deletion/replacement plan: " + "; ".join(forbidden)
        )


def tofu_apply(
    *,
    region: str,
    ssh_cidr: str,
    cached_amis: dict[str, str],
    artifact_bucket_name: str,
    artifact_prefix: str,
) -> dict[str, Any]:
    guard_local_state_against_protected_instances()
    tofu_init()
    variables = {
        "region": region,
        "ssh_ingress_cidr": ssh_cidr,
        "arm64_ami_id": cached_amis.get("arm64", ""),
        "x64_ami_id": cached_amis.get("x64", ""),
        "artifact_bucket_name": artifact_bucket_name,
        "artifact_prefix": artifact_prefix,
        "ami_schema_revision": BOOTSTRAP_SCHEMA_REVISION,
        "root_volume_size_gb": "500",
        "compiler_rt_priority": "1",
    }
    variable_args = [f"-var={key}={value}" for key, value in variables.items()]
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="tofu-package-plan-", dir=RUNTIME_DIR
    ) as plan_directory:
        plan_path = Path(plan_directory) / "package-build.tfplan"
        run_command(
            [
                tofu_executable(),
                "plan",
                "-input=false",
                f"-out={plan_path}",
                *variable_args,
            ],
            cwd=STACK_DIR,
        )
        rendered = run_command(
            [tofu_executable(), "show", "-json", str(plan_path)],
            cwd=STACK_DIR,
            capture=True,
        )
        try:
            plan = json.loads(rendered.stdout)
        except json.JSONDecodeError as error:
            raise PackageBuilderError(
                f"OpenTofu plan output is not valid JSON: {error}"
            ) from error
        validate_tofu_plan(plan)
        run_command(
            [tofu_executable(), "apply", "-input=false", str(plan_path)],
            cwd=STACK_DIR,
        )
    outputs = tofu_outputs()
    if not outputs:
        raise PackageBuilderError("OpenTofu apply completed without readable outputs")
    return outputs


def aws_preflight(region: str) -> dict[str, Any]:
    identity = aws_json(region, "sts", "get-caller-identity")
    account = identity.get("Account")
    arn = identity.get("Arn")
    if not isinstance(account, str) or not re.fullmatch(r"[0-9]{12}", account):
        raise PackageBuilderError("AWS caller identity did not contain a 12-digit account")
    if not isinstance(arn, str) or not arn:
        raise PackageBuilderError("AWS caller identity did not contain an ARN")
    # This is a permission probe as well as a region sanity check.
    aws_json(region, "ec2", "describe-availability-zones", "--filters", "Name=state,Values=available")
    info(f"AWS preflight passed for account {account} in {region} ({arn})")
    return identity


def normalize_s3_prefix(value: str) -> str:
    if (
        not 1 <= len(value) <= 128
        or S3_PREFIX_RE.fullmatch(value) is None
        or value.endswith("/")
        or "//" in value
    ):
        raise PackageBuilderError(
            "S3 prefix must be a 1-128 character relative key prefix without "
            "a trailing or doubled slash"
        )
    return value


def aws_tags(value: object, *, label: str) -> dict[str, str]:
    if not isinstance(value, list):
        raise PackageBuilderError(f"{label} tags are malformed")
    tags: dict[str, str] = {}
    for item in value:
        if not isinstance(item, dict):
            raise PackageBuilderError(f"{label} tags are malformed")
        key = item.get("Key")
        tag_value = item.get("Value")
        if not isinstance(key, str) or not isinstance(tag_value, str) or key in tags:
            raise PackageBuilderError(f"{label} tags are malformed or duplicated")
        tags[key] = tag_value
    return tags


def require_resource_tags(
    value: object, expected: dict[str, str], *, label: str,
) -> dict[str, str]:
    tags = aws_tags(value, label=label)
    wrong = {
        key: (tags.get(key), expected_value)
        for key, expected_value in expected.items()
        if tags.get(key) != expected_value
    }
    if wrong:
        raise PackageBuilderError(f"{label} failed ownership-tag validation: {wrong}")
    return tags


def validate_existing_artifact_bucket(
    region: str, account: str,
) -> str:
    bucket = f"{RESOURCE_NAME_PREFIX}-{account}-{region}"
    location = aws_json(region, "s3api", "get-bucket-location", "--bucket", bucket)
    observed_region = location.get("LocationConstraint") or "us-east-1"
    if observed_region != region:
        raise PackageBuilderError(
            f"package artifact bucket {bucket} is in {observed_region}, expected {region}"
        )
    tag_payload = aws_json(region, "s3api", "get-bucket-tagging", "--bucket", bucket)
    require_resource_tags(
        tag_payload.get("TagSet"),
        {
            "Project": "bioseq",
            "ManagedBy": "opentofu",
            "Component": "package-artifacts",
            "Name": bucket,
        },
        label=f"bucket {bucket}",
    )
    versioning = aws_json(
        region, "s3api", "get-bucket-versioning", "--bucket", bucket
    )
    if versioning.get("Status") != "Enabled":
        raise PackageBuilderError(f"package artifact bucket {bucket} is not versioned")
    encryption = aws_json(
        region, "s3api", "get-bucket-encryption", "--bucket", bucket
    )
    rules = encryption.get("ServerSideEncryptionConfiguration", {}).get("Rules", [])
    algorithms = {
        rule.get("ApplyServerSideEncryptionByDefault", {}).get("SSEAlgorithm")
        for rule in rules
        if isinstance(rule, dict)
    }
    if algorithms != {"AES256"}:
        raise PackageBuilderError(
            f"package artifact bucket {bucket} must use default AES256 encryption"
        )
    public_access = aws_json(
        region, "s3api", "get-public-access-block", "--bucket", bucket
    ).get("PublicAccessBlockConfiguration", {})
    if any(
        public_access.get(key) is not True
        for key in (
            "BlockPublicAcls",
            "IgnorePublicAcls",
            "BlockPublicPolicy",
            "RestrictPublicBuckets",
        )
    ):
        raise PackageBuilderError(
            f"package artifact bucket {bucket} does not block all public access"
        )
    ownership = aws_json(
        region, "s3api", "get-bucket-ownership-controls", "--bucket", bucket
    ).get("OwnershipControls", {}).get("Rules", [])
    if not isinstance(ownership, list) or {
        rule.get("ObjectOwnership") for rule in ownership if isinstance(rule, dict)
    } != {"BucketOwnerEnforced"}:
        raise PackageBuilderError(
            f"package artifact bucket {bucket} must enforce bucket ownership"
        )
    return bucket


def validate_existing_signing_secret(
    region: str, account: str,
) -> str:
    name = f"{RESOURCE_NAME_PREFIX}-omnipackage-signing-key"
    payload = aws_json(
        region, "secretsmanager", "describe-secret", "--secret-id", name
    )
    arn = payload.get("ARN")
    expected_prefix = f"arn:aws:secretsmanager:{region}:{account}:secret:{name}-"
    if not isinstance(arn, str) or not arn.startswith(expected_prefix):
        raise PackageBuilderError("package signing secret is outside the current account")
    if payload.get("DeletedDate") is not None:
        raise PackageBuilderError("package signing secret is pending deletion")
    require_resource_tags(
        payload.get("Tags"),
        {
            "Project": "bioseq",
            "ManagedBy": "opentofu",
            "Component": "package-signing",
            "Name": name,
        },
        label=f"secret {name}",
    )
    return arn


def discover_existing_builders(
    region: str, matrix: Matrix, *, ephemeral_ssh: bool,
) -> tuple[dict[str, Builder], dict[str, dict[str, Any]]]:
    filters = [
        f"Name=tag:{key},Values={value}" for key, value in EXPECTED_INSTANCE_TAGS.items()
    ]
    filters.append("Name=instance-state-name,Values=pending,running,stopping,stopped")
    payload = aws_json(region, "ec2", "describe-instances", "--filters", *filters)
    instances = [
        instance
        for reservation in payload.get("Reservations", [])
        if isinstance(reservation, dict)
        for instance in reservation.get("Instances", [])
        if isinstance(instance, dict)
    ]
    candidates: dict[str, list[dict[str, Any]]] = {
        name: [] for name in matrix.architectures
    }
    for instance in instances:
        tags = aws_tags(instance.get("Tags", []), label="discovered builder")
        name = tags.get("BuildArchitecture")
        if name in candidates:
            candidates[name].append(instance)
    wrong_counts = {name: len(values) for name, values in candidates.items() if len(values) != 1}
    if wrong_counts:
        raise PackageBuilderError(
            "current AWS account must contain exactly one tagged builder per "
            f"architecture: {wrong_counts}"
        )
    builders: dict[str, Builder] = {}
    for name, values in candidates.items():
        instance = values[0]
        instance_id = str(instance.get("InstanceId", ""))
        image_id = str(instance.get("ImageId", ""))
        placement = instance.get("Placement", {})
        availability_zone = (
            str(placement.get("AvailabilityZone", ""))
            if isinstance(placement, dict)
            else ""
        )
        if (
            INSTANCE_ID_RE.fullmatch(instance_id) is None
            or AMI_ID_RE.fullmatch(image_id) is None
            or not availability_zone.startswith(region)
        ):
            raise PackageBuilderError(f"discovered {name} builder identity is malformed")
        expected = matrix.architectures[name]
        builders[name] = Builder(
            architecture=name,
            instance_id=instance_id,
            instance_type=str(instance.get("InstanceType", "")),
            aws_architecture=str(instance.get("Architecture", "")),
            public_ip=str(instance.get("PublicIpAddress") or ""),
            private_ip=str(instance.get("PrivateIpAddress") or ""),
            source_ami_id=image_id,
            availability_zone=availability_zone,
            region=region,
            ssh_access="ec2-instance-connect" if ephemeral_ssh else "static-key",
        )
        if (
            builders[name].instance_type != expected.instance_type
            or builders[name].aws_architecture != expected.aws_architecture
        ):
            raise PackageBuilderError(f"discovered {name} builder shape is not canonical")
    records = validate_builders(region, builders, matrix)
    return builders, records


def discover_existing_infrastructure(
    region: str,
    matrix: Matrix,
    *,
    ssh_key: Path | None = None,
) -> tuple[dict[str, Any], dict[str, Builder], dict[str, dict[str, Any]]]:
    identity = aws_preflight(region)
    account = str(identity["Account"])
    builders, records = discover_existing_builders(
        region, matrix, ephemeral_ssh=ssh_key is not None
    )
    profile_arns = {
        str(record.get("IamInstanceProfile", {}).get("Arn", ""))
        for record in records.values()
    }
    expected_profile_arn = (
        f"arn:aws:iam::{account}:instance-profile/{RESOURCE_NAME_PREFIX}-profile"
    )
    if profile_arns != {expected_profile_arn}:
        raise PackageBuilderError(
            "package builders do not use the current account's canonical instance profile"
        )
    profile = aws_json(
        region,
        "iam",
        "get-instance-profile",
        "--instance-profile-name",
        f"{RESOURCE_NAME_PREFIX}-profile",
    ).get("InstanceProfile", {})
    roles = profile.get("Roles", []) if isinstance(profile, dict) else []
    expected_role_arn = f"arn:aws:iam::{account}:role/{RESOURCE_NAME_PREFIX}-role"
    if (
        not isinstance(roles, list)
        or len(roles) != 1
        or roles[0].get("Arn") != expected_role_arn
    ):
        raise PackageBuilderError(
            "package builder instance profile does not contain the canonical role"
        )
    bucket = validate_existing_artifact_bucket(region, account)
    secret_arn = validate_existing_signing_secret(region, account)
    outputs: dict[str, Any] = {
        "account_id": account,
        "caller_arn": str(identity["Arn"]),
        "region": region,
        "package_bucket_name": bucket,
        "package_bucket_arn": f"arn:aws:s3:::{bucket}",
        "signing_key_secret_arn": secret_arn,
    }
    if ssh_key is not None:
        outputs["ssh_private_key_path"] = str(ssh_key.resolve())
    return outputs, builders, records


def write_json_atomic(path: Path, payload: object, *, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.chmod(mode)
    temporary.replace(path)


def validated_builder_security_group(
    region: str, records: dict[str, dict[str, Any]],
) -> tuple[str, dict[str, Any]]:
    groups: set[str] = set()
    for name, record in records.items():
        values = record.get("SecurityGroups", [])
        if not isinstance(values, list) or len(values) != 1:
            raise PackageBuilderError(
                f"{name} builder must have exactly one security group"
            )
        group_id = str(values[0].get("GroupId", ""))
        if SECURITY_GROUP_ID_RE.fullmatch(group_id) is None:
            raise PackageBuilderError(f"{name} builder security group is malformed")
        groups.add(group_id)
    if len(groups) != 1:
        raise PackageBuilderError("package builders do not share one security group")
    group_id = groups.pop()
    payload = aws_json(
        region, "ec2", "describe-security-groups", "--group-ids", group_id
    )
    values = payload.get("SecurityGroups", [])
    if not isinstance(values, list) or len(values) != 1:
        raise PackageBuilderError(f"cannot validate builder security group {group_id}")
    group = values[0]
    require_resource_tags(
        group.get("Tags"),
        {
            "Project": "bioseq",
            "ManagedBy": "opentofu",
            "Component": "package-build-machine",
            "Name": f"{RESOURCE_NAME_PREFIX}-sg",
        },
        label=f"security group {group_id}",
    )
    permissions = group.get("IpPermissions", [])
    if not isinstance(permissions, list):
        raise PackageBuilderError(f"security group {group_id} ingress is malformed")
    for permission in permissions:
        if (
            not isinstance(permission, dict)
            or permission.get("IpProtocol") != "tcp"
            or permission.get("FromPort") != 22
            or permission.get("ToPort") != 22
            or permission.get("Ipv6Ranges")
            or permission.get("PrefixListIds")
            or permission.get("UserIdGroupPairs")
        ):
            raise PackageBuilderError(
                f"security group {group_id} has non-canonical ingress"
            )
        for value in permission.get("IpRanges", []):
            normalize_ssh_cidr(str(value.get("CidrIp", "")))
    return group_id, group


def security_group_cidr_description(
    group: dict[str, Any], cidr: str,
) -> str | None:
    matches = [
        str(value.get("Description") or "")
        for permission in group.get("IpPermissions", [])
        if isinstance(permission, dict)
        for value in permission.get("IpRanges", [])
        if isinstance(value, dict)
        and value.get("CidrIp") == cidr
    ]
    if len(matches) > 1:
        raise PackageBuilderError(
            f"security group contains duplicate SSH ingress for {cidr}"
        )
    return matches[0] if matches else None


def security_group_has_cidr(group: dict[str, Any], cidr: str) -> bool:
    return security_group_cidr_description(group, cidr) is not None


def authorize_ci_ssh_ingress(
    region: str, group_id: str, cidr: str, description: str,
) -> None:
    permission = json.dumps(
        [
            {
                "IpProtocol": "tcp",
                "FromPort": 22,
                "ToPort": 22,
                "IpRanges": [{"CidrIp": cidr, "Description": description}],
            }
        ],
        separators=(",", ":"),
    )
    aws_no_output(
        region,
        "ec2",
        "authorize-security-group-ingress",
        "--group-id",
        group_id,
        "--ip-permissions",
        permission,
    )


def read_ci_lifecycle_receipt(
    receipt: Path,
    *,
    region: str,
    identity: dict[str, Any] | None = None,
    builders: dict[str, Builder] | None = None,
    require_open: bool = False,
) -> dict[str, Any]:
    try:
        payload = json.loads(receipt.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PackageBuilderError(
            f"cannot read CI lifecycle receipt {receipt}: {error}"
        ) from error
    if not isinstance(payload, dict):
        raise PackageBuilderError("CI lifecycle receipt must be an object")
    require_exact_keys(
        payload,
        {
            "schema_revision",
            "account",
            "caller_arn",
            "region",
            "security_group_id",
            "cidr",
            "owner_token_sha256",
            "description",
            "rule_added",
            "closed",
            "builders",
        },
        "CI lifecycle receipt",
    )
    caller_arn = payload["caller_arn"]
    if (
        type(payload["schema_revision"]) is not int
        or payload["schema_revision"] != CI_ACCESS_SCHEMA_REVISION
        or re.fullmatch(r"[0-9]{12}", str(payload["account"])) is None
        or not isinstance(caller_arn, str)
        or not caller_arn.startswith("arn:")
        or any(character.isspace() for character in caller_arn)
        or payload["region"] != region
        or SECURITY_GROUP_ID_RE.fullmatch(str(payload["security_group_id"])) is None
        or not isinstance(payload["owner_token_sha256"], str)
        or re.fullmatch(r"[0-9a-f]{64}", payload["owner_token_sha256"]) is None
        or payload["description"] != (
            f"{CI_ACCESS_DESCRIPTION} "
            f"owner={payload['owner_token_sha256'][:24]}"
        )
        or type(payload["rule_added"]) is not bool
        or type(payload["closed"]) is not bool
    ):
        raise PackageBuilderError("CI lifecycle receipt identity is invalid")
    normalize_ssh_cidr(str(payload["cidr"]))
    builder_receipts = payload["builders"]
    matrix = load_matrix()
    if not isinstance(builder_receipts, dict) or set(builder_receipts) != set(
        matrix.architectures
    ):
        raise PackageBuilderError("CI lifecycle receipt builder set is invalid")
    for name, value in builder_receipts.items():
        if not isinstance(value, dict):
            raise PackageBuilderError(f"CI lifecycle receipt {name} builder is malformed")
        require_exact_keys(
            value,
            {"instance_id", "initial_state", "stop_owned"},
            f"CI lifecycle receipt {name} builder",
        )
        if (
            INSTANCE_ID_RE.fullmatch(str(value["instance_id"])) is None
            or value["initial_state"] not in {"pending", "running", "stopping", "stopped"}
            or type(value["stop_owned"]) is not bool
        ):
            raise PackageBuilderError(
                f"CI lifecycle receipt {name} builder identity is invalid"
            )
    if identity is not None and (
        payload["account"] != identity.get("Account")
        or caller_arn != identity.get("Arn")
    ):
        raise PackageBuilderError(
            "CI lifecycle receipt belongs to a different AWS credential identity"
        )
    if builders is not None:
        if set(builders) != set(builder_receipts) or any(
            builder_receipts[name]["instance_id"] != builder.instance_id
            for name, builder in builders.items()
        ):
            raise PackageBuilderError(
                "CI lifecycle receipt does not name the discovered canonical builders"
            )
    if require_open and payload["closed"]:
        raise PackageBuilderError("CI lifecycle receipt was already closed")
    return payload


def record_builder_stop_ownership(
    *,
    receipt: Path,
    region: str,
    identity: dict[str, Any],
    builders: dict[str, Builder],
    architecture: str,
) -> None:
    payload = read_ci_lifecycle_receipt(
        receipt,
        region=region,
        identity=identity,
        builders=builders,
        require_open=True,
    )
    value = payload["builders"][architecture]
    if value["stop_owned"]:
        return
    # Persist exact stop authority before the reversible start mutation so an
    # interrupted controller cannot leak a workflow-owned running instance.
    value["stop_owned"] = True
    write_json_atomic(receipt, payload)


def open_ci_access(
    *,
    region: str,
    ssh_key: Path,
    ssh_cidr: str | None,
    owner_token: str,
    receipt: Path,
) -> dict[str, Any]:
    owner_token_sha256, description = ci_access_owner(owner_token)
    matrix = load_matrix()
    key_path = ssh_key.expanduser().resolve()
    if not key_path.is_file():
        raise PackageBuilderError("CI access requires an ephemeral SSH key pair")
    read_instance_connect_public_key(key_path)
    outputs, builders, records = discover_existing_infrastructure(
        region, matrix, ssh_key=key_path
    )
    cidr = normalize_ssh_cidr(ssh_cidr)
    group_id, group = validated_builder_security_group(region, records)
    if receipt.exists():
        existing = read_ci_lifecycle_receipt(
            receipt,
            region=region,
            identity={
                "Account": outputs["account_id"],
                "Arn": outputs["caller_arn"],
            },
            builders=builders,
            require_open=True,
        )
        if existing["cidr"] != cidr or existing["security_group_id"] != group_id:
            raise PackageBuilderError(
                "existing open CI lifecycle receipt targets different SSH access"
            )
        if existing["owner_token_sha256"] != owner_token_sha256:
            raise PackageBuilderError(
                "existing open CI lifecycle receipt belongs to another workflow run"
            )
        current_description = security_group_cidr_description(group, cidr)
        if existing["rule_added"]:
            if current_description not in {None, existing["description"]}:
                raise PackageBuilderError(
                    "existing open CI lifecycle receipt no longer owns its SSH ingress"
                )
            if current_description is None:
                # The receipt is written before AuthorizeSecurityGroupIngress.
                # If that mutation was interrupted or transiently failed, the
                # exact same receipt can safely finish it on retry.
                authorize_ci_ssh_ingress(
                    region, group_id, cidr, existing["description"]
                )
        if not existing["rule_added"] and current_description is None:
            raise PackageBuilderError(
                "existing open CI lifecycle receipt lost its pre-existing SSH ingress"
            )
        return existing

    existing_description = security_group_cidr_description(group, cidr)
    if (
        existing_description is not None
        and existing_description.startswith(CI_ACCESS_DESCRIPTION)
        and existing_description != description
    ):
        raise PackageBuilderError(
            "existing workflow SSH ingress belongs to another owner token"
        )
    added = existing_description is None
    # A retry on a new runner may inherit the exact workflow-authored rule after
    # the prior runner died before revocation. Adopt only that canonical rule;
    # a matching operator/OpenTofu CIDR with any other description stays unowned.
    cleanup_owned = added or existing_description == description
    payload = {
        "schema_revision": CI_ACCESS_SCHEMA_REVISION,
        "account": outputs["account_id"],
        "caller_arn": outputs["caller_arn"],
        "region": region,
        "security_group_id": group_id,
        "cidr": cidr,
        "owner_token_sha256": owner_token_sha256,
        "description": description,
        "rule_added": cleanup_owned,
        "closed": False,
        "builders": {
            name: {
                "instance_id": builder.instance_id,
                "initial_state": instance_state(records[name]),
                "stop_owned": False,
            }
            for name, builder in builders.items()
        },
    }
    # Persist cleanup intent before the reversible AWS mutation.
    write_json_atomic(receipt, payload)
    read_ci_lifecycle_receipt(
        receipt,
        region=region,
        identity={"Account": outputs["account_id"], "Arn": outputs["caller_arn"]},
        builders=builders,
        require_open=True,
    )
    if added:
        authorize_ci_ssh_ingress(region, group_id, cidr, description)
    elif not cleanup_owned:
        info(
            f"SSH ingress {cidr} already exists with non-ephemeral ownership; "
            "the CI receipt will not revoke it"
        )
    return payload


def close_ci_access(*, region: str, receipt: Path) -> dict[str, Any]:
    payload = read_ci_lifecycle_receipt(receipt, region=region)
    identity = aws_preflight(region)
    payload = read_ci_lifecycle_receipt(
        receipt, region=region, identity=identity
    )
    cidr = normalize_ssh_cidr(str(payload["cidr"]))
    group_id = str(payload["security_group_id"])
    group_payload = aws_json(
        region, "ec2", "describe-security-groups", "--group-ids", group_id
    )
    groups = group_payload.get("SecurityGroups", [])
    if not isinstance(groups, list) or len(groups) != 1:
        raise PackageBuilderError("CI access receipt security group no longer exists")
    group = groups[0]
    require_resource_tags(
        group.get("Tags"),
        {
            "Project": "bioseq",
            "ManagedBy": "opentofu",
            "Component": "package-build-machine",
            "Name": f"{RESOURCE_NAME_PREFIX}-sg",
        },
        label=f"security group {group_id}",
    )
    current_description = security_group_cidr_description(group, cidr)
    if (
        payload["rule_added"]
        and not payload["closed"]
        and current_description is not None
    ):
        if current_description != payload["description"]:
            raise PackageBuilderError(
                f"refusing to revoke SSH ingress {cidr} after its ownership "
                "description changed"
            )
        permission = json.dumps(
            [
                {
                    "IpProtocol": "tcp",
                    "FromPort": 22,
                    "ToPort": 22,
                    "IpRanges": [{"CidrIp": cidr}],
                }
            ],
            separators=(",", ":"),
        )
        aws_no_output(
            region,
            "ec2",
            "revoke-security-group-ingress",
            "--group-id",
            group_id,
            "--ip-permissions",
            permission,
        )
    payload["closed"] = True
    write_json_atomic(receipt, payload)
    return payload


def close_ci_access_owner(*, region: str, owner_token: str) -> dict[str, Any]:
    """Revoke every exact ephemeral SSH rule owned by one workflow job token."""

    owner_token_sha256, description = ci_access_owner(owner_token)
    matrix = load_matrix()
    identity = aws_preflight(region)
    builders, records = discover_existing_builders(
        region, matrix, ephemeral_ssh=False
    )
    del builders
    group_id, _group = validated_builder_security_group(region, records)
    payload = aws_json(
        region,
        "ec2",
        "describe-security-group-rules",
        "--filters",
        f"Name=group-id,Values={group_id}",
    )
    rules = payload.get("SecurityGroupRules", [])
    if not isinstance(rules, list):
        raise PackageBuilderError("security-group rule ownership listing is malformed")
    owned_rule_ids: list[str] = []
    owned_cidrs: list[str] = []
    for rule in rules:
        if not isinstance(rule, dict):
            raise PackageBuilderError("security-group rule ownership row is malformed")
        if rule.get("Description") != description:
            continue
        rule_id = str(rule.get("SecurityGroupRuleId", ""))
        cidr = str(rule.get("CidrIpv4", ""))
        if (
            rule.get("GroupId") != group_id
            or rule.get("IsEgress") is not False
            or rule.get("IpProtocol") != "tcp"
            or rule.get("FromPort") != 22
            or rule.get("ToPort") != 22
            or SECURITY_GROUP_RULE_ID_RE.fullmatch(rule_id) is None
        ):
            raise PackageBuilderError(
                "owner-matched CI ingress is not a canonical SSH rule"
            )
        normalize_ssh_cidr(cidr)
        owned_rule_ids.append(rule_id)
        owned_cidrs.append(cidr)
    if len(set(owned_rule_ids)) != len(owned_rule_ids) or len(set(owned_cidrs)) != len(
        owned_cidrs
    ):
        raise PackageBuilderError("owner-matched CI ingress contains duplicates")
    if owned_rule_ids:
        aws_no_output(
            region,
            "ec2",
            "revoke-security-group-ingress",
            "--group-id",
            group_id,
            "--security-group-rule-ids",
            *sorted(owned_rule_ids),
        )
    return {
        "account": identity["Account"],
        "region": region,
        "security_group_id": group_id,
        "owner_token_sha256": owner_token_sha256,
        "revoked_rule_ids": sorted(owned_rule_ids),
        "revoked_cidrs": sorted(owned_cidrs),
    }


def latest_cached_ami(
    region: str,
    architecture: Architecture,
    lock_sha256: str,
    matrix_fingerprint: str,
) -> dict[str, Any] | None:
    if re.fullmatch(r"[0-9a-f]{64}", lock_sha256) is None or re.fullmatch(
        r"[0-9a-f]{64}", matrix_fingerprint
    ) is None:
        raise PackageBuilderError("cached AMI lookup requires complete SHA-256 provenance")
    filters = [
        f"Name=tag:{key},Values={value}" for key, value in CACHE_TAGS.items()
    ]
    filters.extend(
        [
            f"Name=tag:BuildArchitecture,Values={architecture.name}",
            f"Name=tag:Architecture,Values={architecture.aws_architecture}",
            "Name=tag:BootstrapSchemaRevision,Values="
            f"{BOOTSTRAP_SCHEMA_REVISION}",
            f"Name=tag:PoetryLockSha256,Values={lock_sha256}",
            f"Name=tag:CompleteMatrixFingerprint,Values={matrix_fingerprint}",
            f"Name=architecture,Values={architecture.aws_architecture}",
            "Name=root-device-type,Values=ebs",
            "Name=virtualization-type,Values=hvm",
            "Name=state,Values=available",
        ]
    )
    payload = aws_json(
        region,
        "ec2",
        "describe-images",
        "--owners",
        "self",
        "--filters",
        *filters,
    )
    images = payload.get("Images", [])
    if not isinstance(images, list):
        raise PackageBuilderError("EC2 describe-images returned malformed Images")
    candidates: list[dict[str, Any]] = []
    for image in images:
        if not isinstance(image, dict) or AMI_ID_RE.match(str(image.get("ImageId", ""))) is None:
            continue
        tags = {tag.get("Key"): tag.get("Value") for tag in image.get("Tags", [])}
        required_tags = {
            **CACHE_TAGS,
            "BuildArchitecture": architecture.name,
            "Architecture": architecture.aws_architecture,
            "BootstrapSchemaRevision": str(BOOTSTRAP_SCHEMA_REVISION),
            "PoetryLockSha256": lock_sha256,
            "CompleteMatrixFingerprint": matrix_fingerprint,
        }
        if (
            image.get("Architecture") != architecture.aws_architecture
            or image.get("RootDeviceType") != "ebs"
            or image.get("VirtualizationType") != "hvm"
            or any(tags.get(key) != value for key, value in required_tags.items())
        ):
            continue
        candidates.append(image)
    if not candidates:
        return None
    return max(candidates, key=lambda item: str(item.get("CreationDate", "")))


def output_builders(outputs: dict[str, Any], matrix: Matrix) -> dict[str, Builder]:
    raw = outputs.get("builders")
    if not isinstance(raw, dict) or set(raw) != set(matrix.architectures):
        raise PackageBuilderError(
            "OpenTofu output `builders` must contain exactly arm64 and x64"
        )
    builders: dict[str, Builder] = {}
    for architecture, values in raw.items():
        if not isinstance(values, dict):
            raise PackageBuilderError(f"builder output {architecture!r} must be an object")
        instance_id = str(values.get("instance_id", ""))
        if not INSTANCE_ID_RE.match(instance_id):
            raise PackageBuilderError(f"builder {architecture} has invalid instance ID")
        if instance_id in PROTECTED_INSTANCE_IDS:
            raise PackageBuilderError(
                f"refusing protected development instance {instance_id}"
            )
        expected = matrix.architectures[architecture]
        builders[architecture] = Builder(
            architecture=architecture,
            instance_id=instance_id,
            instance_type=str(values.get("instance_type", "")),
            aws_architecture=str(values.get("architecture", "")),
            public_ip=str(values.get("public_ip") or ""),
            private_ip=str(values.get("private_ip") or ""),
            source_ami_id=str(values.get("source_ami_id") or ""),
        )
        if builders[architecture].instance_type != expected.instance_type:
            raise PackageBuilderError(
                f"builder {architecture} output has instance type "
                f"{builders[architecture].instance_type}, expected {expected.instance_type}"
            )
        if builders[architecture].aws_architecture != expected.aws_architecture:
            raise PackageBuilderError(
                f"builder {architecture} output has AWS architecture "
                f"{builders[architecture].aws_architecture}, expected "
                f"{expected.aws_architecture}"
            )
    return builders


def require_builder_schema_tag(tags: dict[str, str], instance_id: str) -> None:
    expected = str(BOOTSTRAP_SCHEMA_REVISION)
    current = tags.get("AmiSchemaRevision")
    legacy = tags.get("AmiSchemaVersion")
    if not (
        (current == expected and legacy in {None, expected})
        or (current is None and legacy == expected)
    ):
        raise PackageBuilderError(
            f"instance {instance_id} has incompatible AMI schema tags"
        )


def describe_builder(region: str, builder: Builder, expected: Architecture) -> dict[str, Any]:
    if builder.instance_id in PROTECTED_INSTANCE_IDS:
        raise PackageBuilderError(
            f"refusing protected development instance {builder.instance_id}"
        )
    payload = aws_json(
        region,
        "ec2",
        "describe-instances",
        "--instance-ids",
        builder.instance_id,
    )
    instances = [
        instance
        for reservation in payload.get("Reservations", [])
        for instance in reservation.get("Instances", [])
    ]
    if len(instances) != 1:
        raise PackageBuilderError(
            f"expected exactly one EC2 record for {builder.instance_id}, got {len(instances)}"
        )
    instance = instances[0]
    tags = {tag.get("Key"): tag.get("Value") for tag in instance.get("Tags", [])}
    required_tags = {
        **EXPECTED_INSTANCE_TAGS,
        "BuildArchitecture": expected.name,
        "Architecture": expected.aws_architecture,
    }
    wrong_tags = {
        key: (tags.get(key), value)
        for key, value in required_tags.items()
        if tags.get(key) != value
    }
    if wrong_tags:
        raise PackageBuilderError(
            f"instance {builder.instance_id} failed ownership-tag validation: {wrong_tags}"
        )
    require_builder_schema_tag(tags, builder.instance_id)
    if instance.get("InstanceType") != expected.instance_type:
        raise PackageBuilderError(
            f"instance {builder.instance_id} type is {instance.get('InstanceType')}, "
            f"expected {expected.instance_type}"
        )
    if instance.get("Architecture") != expected.aws_architecture:
        raise PackageBuilderError(
            f"instance {builder.instance_id} architecture is {instance.get('Architecture')}, "
            f"expected {expected.aws_architecture}"
        )
    root_name = instance.get("RootDeviceName")
    root_mappings = [
        mapping
        for mapping in instance.get("BlockDeviceMappings", [])
        if mapping.get("DeviceName") == root_name
    ]
    if len(root_mappings) != 1 or not root_mappings[0].get("Ebs", {}).get("VolumeId"):
        raise PackageBuilderError(f"instance {builder.instance_id} has no unique EBS root")
    volume_id = root_mappings[0]["Ebs"]["VolumeId"]
    volumes = aws_json(
        region, "ec2", "describe-volumes", "--volume-ids", volume_id
    ).get("Volumes", [])
    if len(volumes) != 1:
        raise PackageBuilderError(f"could not validate root volume {volume_id}")
    volume = volumes[0]
    if volume.get("Size", 0) < 500 or volume.get("Encrypted") is not True:
        raise PackageBuilderError(
            f"builder root {volume_id} must be encrypted and at least 500 GiB"
        )
    termination = aws_json(
        region,
        "ec2",
        "describe-instance-attribute",
        "--instance-id",
        builder.instance_id,
        "--attribute",
        "disableApiTermination",
    )
    if termination.get("DisableApiTermination", {}).get("Value") is not True:
        raise PackageBuilderError(
            f"instance {builder.instance_id} does not have API termination protection"
        )
    stop_protection = aws_json(
        region,
        "ec2",
        "describe-instance-attribute",
        "--instance-id",
        builder.instance_id,
        "--attribute",
        "disableApiStop",
    )
    stop_protection_value = stop_protection.get("DisableApiStop", {}).get("Value")
    if type(stop_protection_value) is not bool:
        raise PackageBuilderError(
            f"instance {builder.instance_id} returned malformed API stop protection"
        )
    if stop_protection_value is False:
        info(
            f"instance {builder.instance_id} permits API stop; cleanup still uses "
            "its instance-initiated stop behavior"
        )
    shutdown = aws_json(
        region,
        "ec2",
        "describe-instance-attribute",
        "--instance-id",
        builder.instance_id,
        "--attribute",
        "instanceInitiatedShutdownBehavior",
    )
    if shutdown.get("InstanceInitiatedShutdownBehavior", {}).get("Value") != "stop":
        raise PackageBuilderError(
            f"instance {builder.instance_id} shutdown behavior is not stop"
        )
    return instance


def validate_builders(
    region: str, builders: dict[str, Builder], matrix: Matrix
) -> dict[str, dict[str, Any]]:
    return {
        name: describe_builder(region, builder, matrix.architectures[name])
        for name, builder in builders.items()
    }


def instance_state(instance: dict[str, Any]) -> str:
    return str(instance.get("State", {}).get("Name", "unknown"))


def start_builders(
    region: str,
    builders: dict[str, Builder],
    matrix: Matrix,
    *,
    lifecycle_receipt: Path | None = None,
    caller_identity: dict[str, Any] | None = None,
) -> dict[str, dict[str, Any]]:
    if lifecycle_receipt is not None and caller_identity is None:
        raise PackageBuilderError(
            "builder lifecycle ownership requires the current AWS caller identity"
        )
    records = validate_builders(region, builders, matrix)
    for name, builder in builders.items():
        state = instance_state(records[name])
        if state == "running":
            info(f"{name} builder {builder.instance_id} is already running; no-op")
            continue
        if state == "stopping":
            run_command(
                [
                    require_executable("aws"),
                    "--region",
                    region,
                    "ec2",
                    "wait",
                    "instance-stopped",
                    "--instance-ids",
                    builder.instance_id,
                ]
            )
            state = "stopped"
        if state == "stopped":
            if lifecycle_receipt is not None:
                assert caller_identity is not None
                record_builder_stop_ownership(
                    receipt=lifecycle_receipt,
                    region=region,
                    identity=caller_identity,
                    builders=builders,
                    architecture=name,
                )
            aws_no_output(
                region, "ec2", "start-instances", "--instance-ids", builder.instance_id
            )
        elif state != "pending":
            raise PackageBuilderError(
                f"cannot start {name} builder {builder.instance_id} from state {state!r}"
            )

    def wait_until_healthy(builder: Builder) -> None:
        run_command(
            [
                require_executable("aws"),
                "--region",
                region,
                "ec2",
                "wait",
                "instance-running",
                "--instance-ids",
                builder.instance_id,
            ]
        )
        run_command(
            [
                require_executable("aws"),
                "--region",
                region,
                "ec2",
                "wait",
                "instance-status-ok",
                "--instance-ids",
                builder.instance_id,
            ]
        )

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(builders), thread_name_prefix="bioseq-builder-start"
    ) as executor:
        futures = {
            executor.submit(wait_until_healthy, builder): name
            for name, builder in builders.items()
        }
        for future in concurrent.futures.as_completed(futures):
            future.result()
    return validate_builders(region, builders, matrix)


def wait_for_ssm_online(
    region: str,
    instance_id: str,
    *,
    timeout_seconds: int = SSM_CLEANUP_TIMEOUT_SECONDS,
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        payload = aws_json(
            region,
            "ssm",
            "describe-instance-information",
            "--filters",
            f"Key=InstanceIds,Values={instance_id}",
        )
        values = payload.get("InstanceInformationList", [])
        if (
            isinstance(values, list)
            and len(values) == 1
            and values[0].get("InstanceId") == instance_id
            and values[0].get("PingStatus") == "Online"
        ):
            return
        time.sleep(10)
    raise PackageBuilderError(f"SSM did not become online for {instance_id}")


def wait_for_ec2_state(
    region: str,
    instance_id: str,
    expected_state: str,
    *,
    timeout_seconds: int = EC2_CLEANUP_STATE_TIMEOUT_SECONDS,
) -> None:
    if (
        INSTANCE_ID_RE.fullmatch(instance_id) is None
        or expected_state not in {"running", "stopped"}
        or type(timeout_seconds) is not int
        or timeout_seconds <= 0
    ):
        raise PackageBuilderError("EC2 cleanup state wait is invalid")
    deadline = time.monotonic() + timeout_seconds
    last_state = "unknown"
    while time.monotonic() < deadline:
        payload = aws_json(
            region, "ec2", "describe-instances", "--instance-ids", instance_id
        )
        instances = [
            instance
            for reservation in payload.get("Reservations", [])
            if isinstance(reservation, dict)
            for instance in reservation.get("Instances", [])
            if isinstance(instance, dict)
        ]
        if (
            len(instances) != 1
            or instances[0].get("InstanceId") != instance_id
        ):
            raise PackageBuilderError(
                f"EC2 cleanup state wait lost exact instance {instance_id}"
            )
        last_state = instance_state(instances[0])
        if last_state == expected_state:
            return
        if expected_state == "running" and last_state != "pending":
            raise PackageBuilderError(
                f"instance {instance_id} reached {last_state!r} while waiting "
                "for running"
            )
        if expected_state == "stopped" and last_state not in {"running", "stopping"}:
            raise PackageBuilderError(
                f"instance {instance_id} reached {last_state!r} while waiting "
                "for stopped"
            )
        time.sleep(5)
    raise PackageBuilderError(
        f"instance {instance_id} remained {last_state!r}; expected "
        f"{expected_state!r} within {timeout_seconds} seconds"
    )


def remote_package_quiescence_script(
    *, poweroff: bool, not_after_epoch: int | None = None,
) -> str:
    """Return the fail-closed, lifecycle-locked package activity probe."""

    if poweroff != (type(not_after_epoch) is int and not_after_epoch > 0):
        raise PackageBuilderError(
            "poweroff quiescence requires exactly one positive mutation deadline"
        )

    lines = [
        "set -Eeuo pipefail",
        f"exec 9>{shlex.quote(REMOTE_PACKAGE_LIFECYCLE_LOCK)}",
        "flock --exclusive --wait "
        f"{REMOTE_PACKAGE_LIFECYCLE_LOCK_WAIT_SECONDS} 9",
    ]
    if poweroff:
        lines.extend(
            [
                f"not_after={not_after_epoch}",
                "test \"$(date +%s)\" -le \"$not_after\" || exit 76",
            ]
        )
    lines.append(
        command_text(
            [
                "python3",
                "-c",
                REMOTE_PACKAGE_ACTIVITY_PROBE_SCRIPT,
                REMOTE_WORKSPACE,
            ]
        )
    )
    if poweroff:
        lines.extend(
            [
                "test \"$(date +%s)\" -le \"$not_after\" || exit 76",
                "/usr/bin/systemctl poweroff --no-block",
                "for attempt in $(seq 1 100); do",
                "  state=$(systemctl is-system-running 2>/dev/null || true)",
                "  case \"$state\" in stopping|offline) exit 0 ;; esac",
                "  sleep 0.2",
                "done",
                "printf 'systemd did not enter stopping state after poweroff request\\n' >&2",
                "exit 75",
            ]
        )
    return "\n".join(lines) + "\n"


def run_ssm_shell_command_detailed(
    region: str,
    instance_ids: Sequence[str],
    *,
    script: str,
    comment: str,
    timeout_seconds: int = SSM_CLEANUP_TIMEOUT_SECONDS,
) -> tuple[str, list[dict[str, Any]]]:
    """Run one bounded SSM shell command and return its exact terminal rows."""

    expected = tuple(sorted(set(instance_ids)))
    if (
        not expected
        or len(expected) != len(instance_ids)
        or any(INSTANCE_ID_RE.fullmatch(value) is None for value in expected)
        or not script.strip()
        or type(timeout_seconds) is not int
        or timeout_seconds <= SSM_CANCEL_RESERVE_SECONDS
    ):
        raise PackageBuilderError("SSM package cleanup targets are invalid")
    execution_timeout = timeout_seconds - SSM_CANCEL_RESERVE_SECONDS
    response = aws_json(
        region,
        "ssm",
        "send-command",
        "--document-name",
        "AWS-RunShellScript",
        "--comment",
        comment,
        "--instance-ids",
        *expected,
        "--timeout-seconds",
        str(execution_timeout),
        "--parameters",
        json.dumps(
            {
                "commands": [
                    command_text(["/bin/bash", "-lc", script])
                ]
            },
            separators=(",", ":"),
        ),
    )
    command_id = response.get("Command", {}).get("CommandId")
    if not isinstance(command_id, str) or not command_id:
        raise PackageBuilderError("SSM did not acknowledge package cleanup command")

    terminal = {"Success", "Cancelled", "TimedOut", "Failed"}

    def observe() -> list[dict[str, Any]]:
        observed = aws_json(
            region,
            "ssm",
            "list-command-invocations",
            "--command-id",
            command_id,
            "--details",
        ).get("CommandInvocations", [])
        if not isinstance(observed, list):
            raise PackageBuilderError("SSM package cleanup returned malformed rows")
        rows = [value for value in observed if isinstance(value, dict)]
        if len(rows) != len(observed):
            raise PackageBuilderError("SSM package cleanup returned malformed rows")
        observed_ids = sorted(str(value.get("InstanceId", "")) for value in rows)
        if any(value not in expected for value in observed_ids) or len(
            observed_ids
        ) != len(set(observed_ids)):
            raise PackageBuilderError(
                "SSM package cleanup returned unexpected instance identities"
            )
        return rows

    started = time.monotonic()
    execution_deadline = started + execution_timeout
    overall_deadline = started + timeout_seconds
    rows: list[dict[str, Any]] = []
    while True:
        rows = observe()
        observed_ids = sorted(str(value.get("InstanceId", "")) for value in rows)
        if observed_ids == list(expected) and all(
            value.get("Status") in terminal for value in rows
        ):
            break
        if time.monotonic() >= execution_deadline:
            cancel_error: Exception | None = None
            try:
                aws_no_output(
                    region,
                    "ssm",
                    "cancel-command",
                    "--command-id",
                    command_id,
                    "--instance-ids",
                    *expected,
                )
            except Exception as error:
                # The command can cross into a terminal state immediately before
                # cancellation.  Continue observing instead of guessing whether
                # the failed cancel left executable work behind.
                cancel_error = error
            while True:
                rows = observe()
                observed_ids = sorted(
                    str(value.get("InstanceId", "")) for value in rows
                )
                if observed_ids == list(expected) and all(
                    value.get("Status") in terminal for value in rows
                ):
                    break
                if time.monotonic() >= overall_deadline:
                    statuses = {
                        str(value.get("InstanceId", "")): value.get("Status")
                        for value in rows
                    }
                    suffix = (
                        f"; cancel error: {cancel_error}"
                        if cancel_error is not None
                        else ""
                    )
                    raise PackageBuilderError(
                        f"SSM package cleanup {command_id} did not become terminal "
                        f"after cancellation; statuses={statuses}{suffix}"
                    )
                time.sleep(2)
            break
        time.sleep(5)

    if sorted(str(value.get("InstanceId", "")) for value in rows) != list(expected):
        raise PackageBuilderError(
            f"SSM package cleanup {command_id} omitted an exact target"
        )

    failures = []
    for row in rows:
        if row.get("Status") == "Success":
            continue
        outputs = []
        plugins = row.get("CommandPlugins", [])
        if isinstance(plugins, list):
            outputs = [
                str(plugin.get("Output", "")).strip()
                for plugin in plugins
                if isinstance(plugin, dict) and str(plugin.get("Output", "")).strip()
            ]
        detail = " | ".join(outputs)[-2000:]
        failures.append(
            f"{row.get('InstanceId')} status={row.get('Status')} detail={detail!r}"
        )
    if failures:
        raise PackageBuilderError(
            "SSM package cleanup failed closed: " + "; ".join(failures)
        )
    return command_id, rows


def run_ssm_shell_command(
    region: str,
    instance_ids: Sequence[str],
    *,
    script: str,
    comment: str,
    timeout_seconds: int = SSM_CLEANUP_TIMEOUT_SECONDS,
) -> str:
    """Run one bounded SSM shell command and require every exact target to pass."""

    command_id, _rows = run_ssm_shell_command_detailed(
        region,
        instance_ids,
        script=script,
        comment=comment,
        timeout_seconds=timeout_seconds,
    )
    return command_id


def stop_existing_builders(
    region: str,
    matrix: Matrix,
    *,
    lifecycle_receipt: Path | None = None,
    controller_receipt: Path | None = None,
    controller_source_revision: str | None = None,
    controller_owner_token_sha256: str | None = None,
) -> dict[str, str]:
    if lifecycle_receipt is not None and controller_receipt is not None:
        raise PackageBuilderError(
            "stop accepts either a legacy lifecycle receipt or a controller receipt"
        )
    identity = aws_preflight(region)
    builders, records = discover_existing_builders(
        region, matrix, ephemeral_ssh=False
    )
    targets = dict(builders)
    if lifecycle_receipt is not None:
        payload = read_ci_lifecycle_receipt(
            lifecycle_receipt,
            region=region,
            identity=identity,
            builders=builders,
        )
        targets = {
            name: builder
            for name, builder in builders.items()
            if payload["builders"][name]["stop_owned"]
        }
        for name, builder in builders.items():
            if name not in targets:
                info(
                    f"{name} builder {builder.instance_id} was not started by this "
                    "workflow; leaving its lifecycle unchanged"
                )
    if controller_receipt is not None:
        if (
            controller_source_revision is None
            or controller_owner_token_sha256 is None
        ):
            raise PackageBuilderError(
                "controller-scoped stop requires source and owner identity"
            )
        bucket = validate_existing_artifact_bucket(
            region, str(identity["Account"])
        )
        controller = fetch_controller_receipt(
            controller_receipt,
            matrix=matrix,
            region=region,
            bucket=bucket,
            source_revision=controller_source_revision,
            required=True,
        )
        assert controller is not None
        controller = require_controller_receipt(
            controller,
            matrix=matrix,
            label="package stop controller receipt",
            expected_region=region,
            expected_account=str(identity["Account"]),
            expected_bucket=bucket,
            builders=builders,
            expected_source_revision=controller_source_revision,
            expected_owner_token_sha256=controller_owner_token_sha256,
        )
        if controller["state"] != "collected":
            raise PackageBuilderError(
                "controller-scoped stop requires verified collection evidence"
            )
        targets = {
            name: builder
            for name, builder in builders.items()
            if controller["builders"][name]["stop_owned"]
        }
        for name, builder in builders.items():
            if name not in targets:
                info(
                    f"{name} builder {builder.instance_id} is not controller-owned; "
                    "leaving its lifecycle unchanged"
                )
    states = {name: instance_state(records[name]) for name in matrix.architectures}
    errors: list[str] = []
    if not targets:
        info(
            "no package builder lifecycle is owned by this workflow; "
            "leaving every builder unchanged"
        )
        return states

    # Normalize pending instances before any remote proof.  Cancellation can
    # arrive after lifecycle ownership is recorded but before StartInstances
    # reaches running; probing SSH/SSM before this wait would fail and strand a
    # workflow-owned paid machine.
    def prepare_quiescence_probe(name: str, builder: Builder) -> None:
        state = states[name]
        if state == "pending":
            wait_for_ec2_state(region, builder.instance_id, "running")
            states[name] = "running"
            state = "running"
        if state == "running":
            wait_for_ssm_online(region, builder.instance_id)
        elif state not in {"stopped", "stopping"}:
            raise PackageBuilderError(
                f"cannot prove {name} builder {builder.instance_id} quiescent "
                f"from state {state!r}"
            )

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(builders), thread_name_prefix="bioseq-builder-probe-ready"
    ) as executor:
        futures = {
            executor.submit(prepare_quiescence_probe, name, builder): name
            for name, builder in builders.items()
        }
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                future.result()
            except Exception as error:
                errors.append(f"{name}: {error}")
    if errors:
        raise PackageBuilderError(
            "refusing package builder stop without reachable-state proof: "
            + "; ".join(errors)
        )

    # Joint fail-closed barrier across every running canonical builder.  The
    # probe recognizes workers, active package units, named build/repository
    # containers, and unnamed smoke containers through their workspace binds.
    running_ids = [
        builders[name].instance_id
        for name in matrix.architectures
        if states[name] == "running"
    ]
    if running_ids:
        command_id = run_ssm_shell_command(
            region,
            running_ids,
            script=remote_package_quiescence_script(poweroff=False),
            comment="Bioseq package release pre-stop quiescence barrier",
        )
        info(f"package-builder quiescence barrier passed via SSM {command_id}")

    def stop_builder(name: str, builder: Builder) -> None:
        state = states[name]
        if state == "stopped":
            info(f"{name} builder {builder.instance_id} is already stopped; no-op")
            return
        if state == "stopping":
            pass
        elif state == "running":
            not_after_epoch = int(time.time()) + (
                SSM_CLEANUP_TIMEOUT_SECONDS - SSM_CANCEL_RESERVE_SECONDS
            )
            command_id = run_ssm_shell_command(
                region,
                [builder.instance_id],
                script=remote_package_quiescence_script(
                    poweroff=True, not_after_epoch=not_after_epoch
                ),
                comment="Bioseq package release quiescent cost cleanup",
            )
            info(
                f"{name} builder quiescent poweroff acknowledged via SSM "
                f"{command_id}"
            )
        else:
            raise PackageBuilderError(
                f"cannot stop {name} builder {builder.instance_id} from state {state!r}"
            )
        wait_for_ec2_state(region, builder.instance_id, "stopped")

    if targets:
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=len(targets), thread_name_prefix="bioseq-builder-stop"
        ) as executor:
            futures = {
                executor.submit(stop_builder, name, builder): name
                for name, builder in targets.items()
            }
            for future in concurrent.futures.as_completed(futures):
                name = futures[future]
                try:
                    future.result()
                except Exception as error:
                    errors.append(f"{name}: {error}")
    final_records = validate_builders(region, builders, matrix)
    final_states = {
        name: instance_state(final_records[name]) for name in matrix.architectures
    }
    wrong_target_states = {
        name: final_states[name]
        for name in targets
        if final_states[name] != "stopped"
    }
    if wrong_target_states:
        errors.append(f"owned builder final states: {wrong_target_states}")
    if errors:
        raise PackageBuilderError("package builder stop cleanup failed: " + "; ".join(errors))
    return final_states


def refreshed_builders(
    records: dict[str, dict[str, Any]], builders: dict[str, Builder]
) -> dict[str, Builder]:
    refreshed: dict[str, Builder] = {}
    for name, builder in builders.items():
        instance = records[name]
        refreshed[name] = Builder(
            architecture=name,
            instance_id=builder.instance_id,
            instance_type=builder.instance_type,
            aws_architecture=builder.aws_architecture,
            public_ip=str(instance.get("PublicIpAddress") or ""),
            private_ip=str(instance.get("PrivateIpAddress") or ""),
            source_ami_id=builder.source_ami_id,
            availability_zone=str(
                instance.get("Placement", {}).get("AvailabilityZone", "")
            ),
            region=builder.region,
            ssh_access=builder.ssh_access,
        )
        if not refreshed[name].public_ip:
            raise PackageBuilderError(
                f"running builder {builder.instance_id} has no public IPv4 address"
            )
    return refreshed


def ssh_key_path(outputs: dict[str, Any]) -> Path:
    value = outputs.get("ssh_private_key_path")
    if not isinstance(value, str) or not value:
        raise PackageBuilderError("OpenTofu output `ssh_private_key_path` is missing")
    path = Path(value)
    if not path.is_absolute():
        path = (STACK_DIR / path).resolve()
    if not path.is_file():
        raise PackageBuilderError(f"SSH private key does not exist: {path}")
    path.chmod(0o600)
    return path


def ssh_argv(builder: Builder, key_path: Path) -> list[str]:
    if not builder.public_ip:
        raise PackageBuilderError(f"builder {builder.instance_id} has no public IP")
    return [
        require_executable("ssh"),
        "-i",
        str(key_path),
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=15",
        "-o",
        "ServerAliveInterval=30",
        "-o",
        "ServerAliveCountMax=10",
        "-o",
        "StrictHostKeyChecking=accept-new",
        f"ubuntu@{builder.public_ip}",
    ]


def refresh_instance_connect_key(builder: Builder, key_path: Path) -> None:
    if builder.ssh_access != "ec2-instance-connect":
        return
    if not builder.availability_zone.startswith(builder.region):
        raise PackageBuilderError(
            f"builder {builder.instance_id} has no validated availability zone"
        )
    public_key = read_instance_connect_public_key(key_path)
    result = aws_json(
        builder.region,
        "ec2-instance-connect",
        "send-ssh-public-key",
        "--instance-id",
        builder.instance_id,
        "--instance-os-user",
        "ubuntu",
        "--availability-zone",
        builder.availability_zone,
        "--ssh-public-key",
        public_key,
    )
    if result.get("Success") is not True:
        raise PackageBuilderError(
            f"EC2 Instance Connect rejected key for {builder.instance_id}"
        )


def read_instance_connect_public_key(key_path: Path) -> str:
    public_key_path = key_path.with_name(key_path.name + ".pub")
    try:
        public_key = public_key_path.read_text(encoding="ascii").strip()
    except OSError as error:
        raise PackageBuilderError(
            f"cannot read ephemeral SSH public key {public_key_path}: {error}"
        ) from error
    if (
        len(public_key) > 4096
        or re.fullmatch(r"ssh-ed25519 [A-Za-z0-9+/=]+(?: [^\r\n]+)?", public_key)
        is None
    ):
        raise PackageBuilderError("ephemeral SSH public key is not a valid Ed25519 key")
    return public_key


def ssh_run(
    builder: Builder,
    key_path: Path,
    remote_command: str,
    *,
    capture: bool = False,
    check: bool = True,
    stdout: Any = None,
) -> subprocess.CompletedProcess[str]:
    if builder.ssh_access == "ec2-instance-connect":
        control_path = key_path.with_name(
            f".eic-{builder.architecture}-{secrets.token_hex(8)}"
        )
        if control_path.exists() or control_path.is_symlink():
            raise PackageBuilderError(
                f"refusing existing EC2 Instance Connect control path {control_path}"
            )
        base_argv = ssh_argv(builder, key_path)
        destination = base_argv[-1]
        master_argv = base_argv[:-1] + [
            "-M",
            "-S",
            str(control_path),
            "-o",
            "ControlPersist=30",
            "-o",
            "StdinNull=yes",
            "-fN",
            destination,
        ]
        master_attempted = False
        master_established = False
        try:
            for attempt in range(INSTANCE_CONNECT_AUTH_ATTEMPTS):
                refresh_instance_connect_key(builder, key_path)
                # Arm teardown before spawning.  ssh -fN may already have
                # authenticated and backgrounded its master when communicate()
                # is interrupted before run_command can return.
                master_attempted = True
                master = run_command(
                    master_argv,
                    capture=True,
                    check=False,
                )
                authentication_pending = (
                    master.returncode == 255
                    and "Permission denied (publickey)" in (master.stderr or "")
                )
                if (
                    authentication_pending
                    and attempt + 1 < INSTANCE_CONNECT_AUTH_ATTEMPTS
                ):
                    with contextlib.suppress(OSError):
                        control_path.unlink(missing_ok=True)
                    info(
                        f"EC2 Instance Connect key is not active yet on "
                        f"{builder.architecture}; retrying SSH authentication"
                    )
                    time.sleep(INSTANCE_CONNECT_AUTH_RETRY_SECONDS)
                    continue
                if master.returncode != 0:
                    with contextlib.suppress(OSError):
                        control_path.unlink(missing_ok=True)
                    detail = (master.stderr or master.stdout or "").strip()
                    suffix = f": {detail}" if detail else ""
                    raise PackageBuilderError(
                        f"SSH authentication master failed with exit "
                        f"{master.returncode}: {command_text(master.args)}{suffix}"
                    )
                master_established = True
                break
            command_argv = base_argv[:-1] + [
                "-S",
                str(control_path),
                "-o",
                "ControlMaster=no",
                # ControlMaster=no falls back to a fresh network connection
                # when the socket disappears.  A failing proxy makes that
                # documented fallback path terminate locally; an established
                # multiplexed connection never invokes the proxy.
                "-o",
                "ProxyCommand=/usr/bin/false",
                "-o",
                "PreferredAuthentications=none",
                "-o",
                "PubkeyAuthentication=no",
                "-o",
                "PasswordAuthentication=no",
                "-o",
                "KbdInteractiveAuthentication=no",
                "-o",
                "HostbasedAuthentication=no",
                "-o",
                "GSSAPIAuthentication=no",
                destination,
                remote_command,
            ]
            return run_command(
                command_argv,
                capture=capture,
                check=check,
                stdout=stdout,
            )
        finally:
            if master_attempted:
                try:
                    cleanup = run_command(
                        [
                            base_argv[0],
                            "-S",
                            str(control_path),
                            "-O",
                            "exit",
                            destination,
                        ],
                        capture=True,
                        check=False,
                    )
                    if master_established and cleanup.returncode != 0:
                        with contextlib.suppress(BaseException):
                            info(
                                "warning: EC2 Instance Connect SSH master cleanup "
                                f"exited {cleanup.returncode} on "
                                f"{builder.architecture}; ControlPersist=30 bounds "
                                "the already-idle master"
                            )
                except BaseException as error:
                    # Cleanup must never replace the requested command's result
                    # or exception.  The master is already idle and its bounded
                    # ControlPersist lifetime remains the final backstop.
                    with contextlib.suppress(BaseException):
                        info(
                            "warning: EC2 Instance Connect SSH master cleanup failed "
                            f"on {builder.architecture}: {error}; ControlPersist=30 "
                            "bounds the already-idle master"
                        )
                finally:
                    try:
                        control_path.unlink(missing_ok=True)
                    except BaseException as error:
                        with contextlib.suppress(BaseException):
                            info(
                                "warning: cannot remove EC2 Instance Connect "
                                f"control path for {builder.architecture}: {error}"
                            )
    else:
        refresh_instance_connect_key(builder, key_path)
        return run_command(
            ssh_argv(builder, key_path) + [remote_command],
            capture=capture,
            check=check,
            stdout=stdout,
        )


def wait_for_ssh(builder: Builder, key_path: Path, timeout_seconds: int = 1200) -> None:
    deadline = time.monotonic() + timeout_seconds
    last_error = ""
    while time.monotonic() < deadline:
        result = ssh_run(
            builder,
            key_path,
            "true",
            capture=True,
            check=False,
        )
        if result.returncode == 0:
            info(f"SSH is ready on {builder.architecture} ({builder.public_ip})")
            return
        last_error = (result.stderr or result.stdout or "").strip()
        time.sleep(10)
    raise PackageBuilderError(
        f"SSH did not become ready on {builder.instance_id}: {last_error}"
    )


def wait_for_bootstrap(builder: Builder, key_path: Path) -> None:
    wait_for_ssh(builder, key_path)
    # A repaired, idempotently resumed bootstrap can be READY even though
    # cloud-init retains the original first-boot error forever. Skip that stale
    # status only when the stronger on-host READY marker is already present;
    # otherwise cloud-init remains the strict first-boot gate.
    ready_probe = ssh_run(
        builder,
        key_path,
        "test \"$(cat /opt/bioseq-package-builder/PROVISION_STATUS 2>/dev/null)\" = READY",
        capture=True,
        check=False,
    )
    if ready_probe.returncode != 0:
        ssh_run(builder, key_path, "sudo cloud-init status --wait --long")
    expected = shlex.quote(builder.architecture)
    verification = (
        "set -eu; "
        "test \"$(cat /opt/bioseq-package-builder/PROVISION_STATUS)\" = READY; "
        "test -s /opt/bioseq-package-builder/READY; "
        f"grep -Fqx 'architecture={expected}' /opt/bioseq-package-builder/READY; "
        f"grep -Fqx 'ami_schema_revision={BOOTSTRAP_SCHEMA_REVISION}' "
        "/opt/bioseq-package-builder/READY; "
        "sudo /usr/local/sbin/bioseq-package-builder-verify; "
        "test -x /usr/local/bin/bioseq-compiler-rt-launcher; "
        "docker info >/dev/null"
    )
    ssh_run(builder, key_path, verification)
    info(f"bootstrap invariants passed on {builder.architecture}")


def reconcile_remote_bootstrap(
    builder: Builder, key_path: Path, remote_source: str
) -> None:
    """Reconcile the current bootstrap in place without rebooting or stopping."""

    wait_for_ssh(builder, key_path)
    ssh_run(
        builder,
        key_path,
        "for attempt in $(seq 1 720); do "
        "if ! pgrep -f '[b]ioseq-package-builder-bootstrap' >/dev/null; then exit 0; fi; "
        "sleep 5; done; exit 1",
    )
    ready = ssh_run(
        builder,
        key_path,
        "set -eu; "
        "test \"$(cat /opt/bioseq-package-builder/PROVISION_STATUS 2>/dev/null)\" = READY; "
        f"grep -Fqx 'ami_schema_revision={BOOTSTRAP_SCHEMA_REVISION}' "
        "/opt/bioseq-package-builder/READY; "
        "sudo /usr/local/sbin/bioseq-package-builder-verify",
        capture=True,
        check=False,
    )
    if ready.returncode == 0:
        info(f"current bootstrap already reconciled on {builder.architecture}")
        return
    source_bootstrap = (
        f"{remote_source}/infra/package_build_infra/scripts/bootstrap.sh"
    )
    schema_assignment = (
        f"BIOSEQ_AMI_SCHEMA_REVISION='{BOOTSTRAP_SCHEMA_REVISION}'"
    )
    command = (
        "set -Eeuo pipefail; "
        f"test -f {shlex.quote(source_bootstrap)}; "
        f"sudo install -o root -g root -m 0755 {shlex.quote(source_bootstrap)} "
        "/usr/local/sbin/bioseq-package-builder-bootstrap; "
        "if grep -q '^BIOSEQ_AMI_SCHEMA_REVISION=' "
        "/etc/bioseq-package-builder.env; then "
        "sudo sed -i -E "
        f"{shlex.quote('s/^BIOSEQ_AMI_SCHEMA_REVISION=.*/' + schema_assignment + '/')} "
        "/etc/bioseq-package-builder.env; "
        "else "
        f"printf '%s\\n' {shlex.quote(schema_assignment)} | "
        "sudo tee -a /etc/bioseq-package-builder.env >/dev/null; "
        "fi; "
        f"grep -Fqx {shlex.quote(schema_assignment)} "
        "/etc/bioseq-package-builder.env; "
        "sudo /usr/local/sbin/bioseq-package-builder-bootstrap"
    )
    ssh_run(builder, key_path, f"bash -lc {shlex.quote(command)}")
    wait_for_bootstrap(builder, key_path)


def force_remote_package_reconfigure(
    builder: Builder,
    key_path: Path,
) -> None:
    """Preserve the cache while forcing CMake to re-resolve current flags."""

    token = secrets.token_hex(8)
    lock = f"{REMOTE_PACKAGE_CMAKE_CACHE_ROOT}/.controller-reconfigure.lock"
    body = (
        "set -Eeuo pipefail\n"
        f"root={shlex.quote(REMOTE_PACKAGE_CMAKE_CACHE_ROOT)}\n"
        f"token={shlex.quote(token)}\n"
        f"lock={shlex.quote(lock)}\n"
        "if ! test -e \"$root\" && ! test -L \"$root\"; then exit 0; fi\n"
        "test -d \"$root\"\n"
        "test ! -L \"$root\"\n"
        "exec 9>\"$lock\"\n"
        "flock -x 9\n"
        "while IFS= read -r -d '' cache; do\n"
        "  if test -L \"$cache\"; then exit 71; fi\n"
        "  test -f \"$cache\"\n"
        "  backup=\"${cache}.pre-o3-${token}\"\n"
        "  test ! -e \"$backup\" && test ! -L \"$backup\"\n"
        "  sudo -- mv -- \"$cache\" \"$backup\"\n"
        "done < <(find \"$root\" "
        "-path '*/build-repository-root/CMakeCache.txt' -print0)\n"
    )
    ssh_run(builder, key_path, f"bash -lc {shlex.quote(body)}")
    info(
        f"{builder.architecture} package cache will re-resolve the current "
        "production compiler flags"
    )


def create_cached_ami(
    region: str,
    builder: Builder,
    architecture: Architecture,
    key_path: Path,
    revision: str,
    lock_sha256: str,
    matrix_fingerprint: str,
    unit_name: str,
    worker_argv: Sequence[str],
    *,
    allow_not_found: bool,
) -> dict[str, Any]:
    """Create a no-reboot image from a quiescent, still-running builder."""

    record = describe_builder(region, builder, architecture)
    if instance_state(record) != "running":
        raise PackageBuilderError(
            f"refusing to image {builder.instance_id} from non-running state "
            f"{instance_state(record)!r}"
        )
    ssh_run(builder, key_path, "sync")
    # This is intentionally the last remote operation before CreateImage.  The
    # earlier joint preflight protects the pair; this closes the per-image gap.
    require_quiescent_remote_worker(
        builder,
        key_path,
        unit_name,
        worker_argv,
        allow_not_found=allow_not_found,
    )
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%S")
    name = (
        f"bioseq-package-builder-{architecture.name}-schema-revision-"
        f"{BOOTSTRAP_SCHEMA_REVISION}-"
        f"{matrix_fingerprint[:12]}-{stamp}"
    )
    payload = aws_json(
        region,
        "ec2",
        "create-image",
        "--instance-id",
        builder.instance_id,
        "--name",
        name,
        "--description",
        f"BioSeq complete post-build package cache {architecture.name}, revision {revision}",
        "--no-reboot",
    )
    image_id = str(payload.get("ImageId", ""))
    if not AMI_ID_RE.match(image_id):
        raise PackageBuilderError("EC2 create-image returned an invalid AMI id")
    tags = {
        **CACHE_TAGS,
        "BuildArchitecture": architecture.name,
        "Architecture": architecture.aws_architecture,
        "BootstrapSchemaRevision": str(BOOTSTRAP_SCHEMA_REVISION),
        "SourceRevision": revision,
        "SourceInstanceId": builder.instance_id,
        "PoetryLockSha256": lock_sha256,
        "CompleteMatrixFingerprint": matrix_fingerprint,
        "WorkerGeneration": WORKER_GENERATION,
        "WorkerGenerationRevision": str(WORKER_GENERATION_REVISION),
        "Name": name,
    }
    aws_no_output(
        region,
        "ec2",
        "create-tags",
        "--resources",
        image_id,
        "--tags",
        *(f"Key={key},Value={value}" for key, value in tags.items()),
    )
    image = wait_for_cached_ami(region, image_id)
    image_tags = {
        tag.get("Key"): tag.get("Value")
        for tag in image.get("Tags", [])
        if isinstance(tag, dict)
    }
    required_image_tags = {
        **CACHE_TAGS,
        "BuildArchitecture": architecture.name,
        "Architecture": architecture.aws_architecture,
        "BootstrapSchemaRevision": str(BOOTSTRAP_SCHEMA_REVISION),
        "SourceRevision": revision,
        "SourceInstanceId": builder.instance_id,
        "PoetryLockSha256": lock_sha256,
        "CompleteMatrixFingerprint": matrix_fingerprint,
        "WorkerGeneration": WORKER_GENERATION,
        "WorkerGenerationRevision": str(WORKER_GENERATION_REVISION),
        "Name": name,
    }
    if (
        image.get("Architecture") != architecture.aws_architecture
        or image.get("RootDeviceType") != "ebs"
        or image.get("VirtualizationType") != "hvm"
        or any(image_tags.get(key) != value for key, value in required_image_tags.items())
    ):
        raise PackageBuilderError(
            f"created AMI {image_id} failed architecture/provenance validation"
        )
    info(f"created reusable post-build {architecture.name} package-builder AMI {image_id}")
    return image


def wait_for_cached_ami(
    region: str, image_id: str, timeout_seconds: int = 7200
) -> dict[str, Any]:
    """Poll a large AMI beyond the AWS waiter's fixed ten-minute ceiling."""

    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        result = aws_json(region, "ec2", "describe-images", "--image-ids", image_id)
        images = result.get("Images", [])
        if len(images) != 1:
            raise PackageBuilderError(f"EC2 did not return cached AMI {image_id}")
        image = images[0]
        state = image.get("State")
        if state == "available":
            return image
        if state == "failed":
            reason = image.get("StateReason", {}).get("Message", "unknown reason")
            raise PackageBuilderError(f"cached AMI {image_id} failed: {reason}")
        if state != "pending":
            raise PackageBuilderError(
                f"cached AMI {image_id} entered unexpected state {state!r}"
            )
        time.sleep(15)
    raise PackageBuilderError(
        f"cached AMI {image_id} did not become available within "
        f"{timeout_seconds // 60} minutes"
    )


def commit_id() -> str:
    result = run_command(
        ["git", "rev-parse", "--verify", "HEAD"],
        cwd=REPOSITORY_ROOT,
        capture=True,
    )
    value = result.stdout.strip()
    if not re.fullmatch(r"[0-9a-f]{40}", value):
        raise PackageBuilderError("could not resolve a full committed Git HEAD")
    return value


def require_committed_build_input_equivalence(
    recovered_revision: str,
    current_revision: str | None = None,
) -> tuple[str, ...]:
    """Reject every committed delta except reviewed recovery-only paths."""

    current = commit_id() if current_revision is None else current_revision
    for label, value in (
        ("recovered", recovered_revision),
        ("current", current),
    ):
        if re.fullmatch(r"[0-9a-f]{40}", value) is None:
            raise PackageBuilderError(
                f"{label} build-input revision must be a full lower-case Git SHA-1"
            )
    if recovered_revision == current:
        return ()
    result = run_command(
        [
            "git",
            "diff",
            "--name-only",
            "--no-renames",
            "-z",
            recovered_revision,
            current,
            "--",
        ],
        cwd=REPOSITORY_ROOT,
        capture=True,
    )
    if result.stdout and not result.stdout.endswith("\0"):
        raise PackageBuilderError("committed build-input diff returned malformed paths")
    paths = tuple(sorted(set(filter(None, result.stdout.split("\0")))))
    for path in paths:
        if (
            path.startswith("/")
            or path.startswith("../")
            or "/../" in path
            or any(ord(character) < 32 for character in path)
        ):
            raise PackageBuilderError(
                f"committed build-input diff returned unsafe path {path!r}"
            )
    rejected = tuple(
        path
        for path in paths
        if path not in FINALIZE_ALLOWED_COMMITTED_DELTAS
        and not any(
            path.startswith(prefix)
            for prefix in FINALIZE_ALLOWED_COMMITTED_PREFIXES
        )
    )
    if rejected:
        raise PackageBuilderError(
            "completed run is not equivalent to current committed build inputs; "
            f"changed build-affecting paths: {list(rejected)}"
        )
    return paths


def committed_poetry_lock_sha256(revision: str) -> str:
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise PackageBuilderError("Poetry lock provenance requires a full Git revision")
    relative = POETRY_LOCK_FILE.relative_to(REPOSITORY_ROOT).as_posix()
    result = run_command(
        ["git", "show", f"{revision}:{relative}"],
        cwd=REPOSITORY_ROOT,
        capture=True,
    )
    if not result.stdout:
        raise PackageBuilderError("committed Poetry lockfile is empty")
    return hashlib.sha256(result.stdout.encode("utf-8")).hexdigest()


def committed_source_tree_sha256(revision: str) -> str:
    """Hash the exact Git objects and modes included in the source archive."""

    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise PackageBuilderError("source-tree provenance requires a full Git revision")
    result = run_command(
        ["git", "ls-tree", "-r", "-z", revision],
        cwd=REPOSITORY_ROOT,
        capture=True,
    )
    records: list[tuple[bytes, bytes]] = []
    for raw_record in result.stdout.split("\0"):
        if not raw_record:
            continue
        metadata, separator, path = raw_record.partition("\t")
        parts = metadata.split(" ")
        if not separator or len(parts) != 3:
            raise PackageBuilderError("Git returned a malformed source-tree entry")
        _mode, object_type, _object_id = parts
        if path in ("papers", "main/papers") or path.startswith(("papers/", "main/papers/")):
            continue
        if object_type != "blob":
            raise PackageBuilderError(
                f"source archive cannot attest non-blob Git entry {path!r}"
            )
        path_bytes = path.encode("utf-8")
        records.append((path_bytes, raw_record.encode("utf-8") + b"\0"))
    if not records:
        raise PackageBuilderError("committed source tree is empty")
    return hashlib.sha256(
        b"".join(record for _path, record in sorted(records))
    ).hexdigest()


def require_committed_paths(paths: Iterable[Path]) -> None:
    for path in paths:
        try:
            relative = path.resolve().relative_to(REPOSITORY_ROOT)
        except ValueError as error:
            raise PackageBuilderError(f"path is outside repository: {path}") from error
        result = run_command(
            ["git", "cat-file", "-e", f"HEAD:{relative.as_posix()}"],
            cwd=REPOSITORY_ROOT,
            capture=True,
            check=False,
        )
        if result.returncode != 0:
            raise PackageBuilderError(
                f"required build input is not committed at HEAD: {relative}"
            )


def _remote_committed_source_path(revision: str) -> str:
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise PackageBuilderError("committed source sync requires a full Git revision")
    return (
        f"{REMOTE_WORKSPACE}/source-{SOURCE_ARCHIVE_IDENTITY}-revision-"
        f"{SOURCE_ARCHIVE_REVISION}-{revision}"
    )


def sync_committed_source(builder: Builder, key_path: Path, revision: str) -> str:
    remote_source = _remote_committed_source_path(revision)
    transfer_token = secrets.token_hex(16)
    remote_staging = f"{remote_source}.tmp-{transfer_token}"
    remote_quarantine = f"{remote_source}.invalid-{transfer_token}"
    remote_lock = f"{remote_source}.lock"
    tree_sha256 = committed_source_tree_sha256(revision)
    marker = hashlib.sha256(
        (
            f"{SOURCE_ARCHIVE_IDENTITY}\n"
            f"revision={SOURCE_ARCHIVE_REVISION}\n"
            f"source_revision={revision}\n"
            f"source_tree_sha256={tree_sha256}\n"
        ).encode("ascii")
    ).hexdigest()
    tree_hasher = f"/usr/bin/python3 -c {shlex.quote(SOURCE_TREE_HASH_SCRIPT)}"
    remote_source_q = shlex.quote(remote_source)
    remote_staging_q = shlex.quote(remote_staging)
    validation_body = (
        "set -Euo pipefail\n"
        f"destination={remote_source_q}\n"
        f"revision={shlex.quote(revision)}\n"
        f"marker={shlex.quote(marker)}\n"
        f"tree_sha256={shlex.quote(tree_sha256)}\n"
        f"lock={shlex.quote(remote_lock)}\n"
        "exec 9>\"$lock\"\n"
        "flock -x 9\n"
        "if ! test -e \"$destination\" && ! test -L \"$destination\"; "
        "then exit 3; fi\n"
        "test -d \"$destination\" || exit 4\n"
        "test ! -L \"$destination\" || exit 4\n"
        "test -f \"$destination/CMakeLists.txt\" || exit 4\n"
        "cmp -s \"$destination/.bioseq-source-revision\" "
        "<(printf '%s\\n' \"$revision\") || exit 4\n"
        "cmp -s \"$destination/.bioseq-source-marker\" "
        "<(printf '%s\\n' \"$marker\") || exit 4\n"
        "cmp -s \"$destination/.bioseq-source-tree-sha256\" "
        "<(printf '%s\\n' \"$tree_sha256\") || exit 4\n"
        f"actual_tree=$({tree_hasher} \"$destination\") || exit 4\n"
        "test \"$actual_tree\" = \"$tree_sha256\" || exit 4\n"
    )
    validation = f"bash -lc {shlex.quote(validation_body)}"
    existing = ssh_run(
        builder,
        key_path,
        validation,
        capture=True,
        check=False,
    )
    if existing.returncode == 0:
        return remote_source
    if existing.returncode not in {3, 4}:
        raise PackageBuilderError(
            f"could not validate the remote committed source cache on "
            f"{builder.architecture}: status {existing.returncode}"
        )
    if existing.returncode == 4:
        info(
            f"{builder.architecture} committed source cache failed exact "
            "provenance and will be recoverably quarantined"
        )

    def cleanup_staging() -> None:
        try:
            ssh_run(
                builder,
                key_path,
                f"rm -rf -- {remote_staging_q}",
                check=False,
            )
        except BaseException:
            pass

    def stop_transfer_process(process: subprocess.Popen[Any]) -> bool:
        try:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            return process.poll() is not None
        except (OSError, subprocess.SubprocessError):
            return False

    archive_argv = [
        "git",
        "archive",
        "--format=tar",
        revision,
        "--",
        ".",
        ":(top,exclude,literal)papers",
        ":(top,exclude,glob)papers/**",
        ":(top,exclude,literal)main/papers",
        ":(top,exclude,glob)main/papers/**",
    ]
    archive: subprocess.Popen[bytes] | None = None
    extract: subprocess.Popen[bytes] | None = None
    staging_cleanup_owned = True
    transfer_quiescent = True
    archive_environment = os.environ.copy()
    # Bind the transfer to committed Git blobs. Git archive otherwise invokes
    # the local Git LFS smudge filter and makes source bytes depend on ambient
    # LFS cache/network state rather than the selected revision.
    archive_environment["GIT_LFS_SKIP_SMUDGE"] = "1"
    try:
        ssh_run(
            builder,
            key_path,
            f"mkdir -m 0755 -- {remote_staging_q}",
        )
        extract_argv = ssh_argv(builder, key_path) + [
            f"tar -xf - -C {remote_staging_q}"
        ]
        info(
            f"streaming committed source {revision} to {builder.architecture} "
            f"at {remote_source}"
        )
        with tempfile.TemporaryFile() as archive_error_file:
            archive = subprocess.Popen(
                archive_argv,
                cwd=str(REPOSITORY_ROOT),
                env=archive_environment,
                stdout=subprocess.PIPE,
                stderr=archive_error_file,
            )
            with ACTIVE_CHILDREN_LOCK:
                ACTIVE_CHILDREN.add(archive)
            assert archive.stdout is not None
            extract = subprocess.Popen(extract_argv, stdin=archive.stdout)
            with ACTIVE_CHILDREN_LOCK:
                ACTIVE_CHILDREN.add(extract)
            archive.stdout.close()
            extract_return = extract.wait()
            archive_return = archive.wait()
            archive_error_file.seek(0)
            archive_error = archive_error_file.read()
        if archive_return != 0 or extract_return != 0:
            raise PackageBuilderError(
                f"source transfer to {builder.architecture} failed: "
                f"git={archive_return}, ssh={extract_return}, "
                f"detail={archive_error.decode(errors='replace').strip()}"
            )
        publication_body = (
            "set -Eeuo pipefail\n"
            f"staging={remote_staging_q}\n"
            f"destination={remote_source_q}\n"
            f"quarantine={shlex.quote(remote_quarantine)}\n"
            f"revision={shlex.quote(revision)}\n"
            f"marker={shlex.quote(marker)}\n"
            f"tree_sha256={shlex.quote(tree_sha256)}\n"
            f"lock={shlex.quote(remote_lock)}\n"
            "test -d \"$staging\"\n"
            "test ! -L \"$staging\"\n"
            "test -f \"$staging/CMakeLists.txt\"\n"
            "rm -f -- \"$staging/.bioseq-source-revision\" "
            "\"$staging/.bioseq-source-marker\" "
            "\"$staging/.bioseq-source-tree-sha256\"\n"
            f"actual_tree=$({tree_hasher} \"$staging\")\n"
            "test \"$actual_tree\" = \"$tree_sha256\"\n"
            "printf '%s\\n' \"$revision\" >"
            "\"$staging/.bioseq-source-revision\"\n"
            "printf '%s\\n' \"$marker\" >"
            "\"$staging/.bioseq-source-marker\"\n"
            "printf '%s\\n' \"$tree_sha256\" >"
            "\"$staging/.bioseq-source-tree-sha256\"\n"
            "exec 9>\"$lock\"\n"
            "flock -x 9\n"
            "valid_destination() {\n"
            "  test -d \"$destination\" &&\n"
            "  test ! -L \"$destination\" &&\n"
            "  test -f \"$destination/CMakeLists.txt\" &&\n"
            "  cmp -s \"$destination/.bioseq-source-revision\" "
            "<(printf '%s\\n' \"$revision\") &&\n"
            "  cmp -s \"$destination/.bioseq-source-marker\" "
            "<(printf '%s\\n' \"$marker\") &&\n"
            "  cmp -s \"$destination/.bioseq-source-tree-sha256\" "
            "<(printf '%s\\n' \"$tree_sha256\") &&\n"
            f"  test \"$({tree_hasher} \"$destination\")\" = \"$tree_sha256\"\n"
            "}\n"
            "if test -e \"$destination\" || test -L \"$destination\"; then\n"
            "  if valid_destination; then\n"
            "    exit 0\n"
            "  fi\n"
            "  if test -e \"$quarantine\" || test -L \"$quarantine\"; then\n"
            "    exit 74\n"
            "  fi\n"
            "  mv -T -- \"$destination\" \"$quarantine\"\n"
            "fi\n"
            "mv -T -- \"$staging\" \"$destination\"\n"
        )
        ssh_run(
            builder,
            key_path,
            f"bash -lc {shlex.quote(publication_body)}",
        )
        verified = ssh_run(
            builder,
            key_path,
            validation,
            capture=True,
            check=False,
        )
        if verified.returncode != 0:
            raise PackageBuilderError(
                f"published committed source failed validation on "
                f"{builder.architecture}"
            )
    except BaseException:
        stopped = [
            stop_transfer_process(process)
            for process in (extract, archive)
            if process is not None
        ]
        transfer_quiescent = all(stopped)
        raise
    finally:
        with ACTIVE_CHILDREN_LOCK:
            for process in (extract, archive):
                if process is not None:
                    ACTIVE_CHILDREN.discard(process)
        if archive is not None and archive.stdout is not None:
            with contextlib.suppress(OSError):
                archive.stdout.close()
        if staging_cleanup_owned and transfer_quiescent:
            cleanup_staging()
    return remote_source


def prepare_remote_poetry_environment(
    builder: Builder,
    key_path: Path,
    remote_source: str,
    revision: str,
    lock_sha256: str,
) -> PoetryEnvironment:
    """Verify committed provenance and atomically prepare a lock-keyed venv."""

    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise PackageBuilderError("remote Poetry setup requires a full Git revision")
    if re.fullmatch(r"[0-9a-f]{64}", lock_sha256) is None:
        raise PackageBuilderError("remote Poetry setup requires a SHA-256 lock hash")
    packaging_dir = f"{remote_source}/infra/packaging"
    venv_path = f"{REMOTE_POETRY_VENV_ROOT}/{lock_sha256}"
    sentinel_name = ".bioseq-poetry-environment.json"
    source_q = shlex.quote(remote_source)
    packaging_q = shlex.quote(packaging_dir)
    revision_q = shlex.quote(revision)
    lock_q = shlex.quote(lock_sha256)
    cache_q = shlex.quote(REMOTE_POETRY_VENV_ROOT)
    venv_q = shlex.quote(venv_path)
    sentinel_q = shlex.quote(f"{venv_path}/{sentinel_name}")
    setup = f"""set -Eeuo pipefail
test \"$(cat {source_q}/.bioseq-source-revision)\" = {revision_q}
test -f {packaging_q}/pyproject.toml
test -f {packaging_q}/poetry.lock
actual_lock=$(sha256sum {packaging_q}/poetry.lock | awk '{{print $1}}')
test \"$actual_lock\" = {lock_q}
install -d -m 0755 {cache_q}
exec 9>{cache_q}/.{lock_sha256}.lock
flock 9
valid=0
if [[ -x {venv_q}/bin/python && -s {sentinel_q} ]]; then
  if {venv_q}/bin/python -c 'import json,sys; value=json.load(open(sys.argv[1], encoding="utf-8")); raise SystemExit(0 if value.get("lock_sha256") == sys.argv[2] and value.get("python") and value.get("poetry") else 1)' {sentinel_q} {lock_q} && \
     {venv_q}/bin/python -c 'import liquid'; then
    valid=1
  fi
fi
if [[ $valid -eq 0 ]]; then
  if [[ -e {venv_q} ]]; then
    mv -- {venv_q} {venv_q}.invalid.$(date -u +%Y%m%dT%H%M%SZ).$$
  fi
  temporary={venv_q}.tmp.$$
  trap 'rm -rf -- "$temporary"' EXIT
  rm -rf -- "$temporary"
  python3 -m venv "$temporary"
  VIRTUAL_ENV="$temporary" \
    PATH="$temporary/bin:$PATH" \
    POETRY_VIRTUALENVS_CREATE=false \
    poetry install --directory {packaging_q} --only main --no-root --sync --no-interaction --no-ansi
  "$temporary/bin/python" -c 'import liquid'
  python_version=$("$temporary/bin/python" -c 'import platform; print(platform.python_version())')
  poetry_version=$(poetry --version --no-ansi | sed -n 's/^Poetry (version \\(.*\\))$/\\1/p')
  test -n "$python_version"
  test -n "$poetry_version"
  "$temporary/bin/python" -c 'import json,os,sys; path=sys.argv[1]; value={{"lock_sha256":sys.argv[2],"python":sys.argv[3],"poetry":sys.argv[4]}}; handle=open(path,"w",encoding="utf-8"); json.dump(value,handle,sort_keys=True); handle.write("\\n"); handle.flush(); os.fsync(handle.fileno()); handle.close(); descriptor=os.open(os.path.dirname(path),os.O_RDONLY); os.fsync(descriptor); os.close(descriptor)' "$temporary/{sentinel_name}" {lock_q} "$python_version" "$poetry_version"
  mv -- "$temporary" {venv_q}
  trap - EXIT
  python3 -c 'import os,sys; descriptor=os.open(sys.argv[1],os.O_RDONLY); os.fsync(descriptor); os.close(descriptor)' {cache_q}
fi
flock -u 9
"""
    ssh_run(builder, key_path, f"bash -lc {shlex.quote(setup)}")
    sentinel_result = ssh_run(
        builder,
        key_path,
        f"cat {sentinel_q}",
        capture=True,
    )
    try:
        sentinel = json.loads(sentinel_result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"{builder.architecture} Poetry environment sentinel is invalid JSON"
        ) from error
    if not isinstance(sentinel, dict) or sentinel.get("lock_sha256") != lock_sha256:
        raise PackageBuilderError(
            f"{builder.architecture} Poetry environment lock provenance mismatch"
        )
    python_version = str(sentinel.get("python", ""))
    poetry_version = str(sentinel.get("poetry", ""))
    if re.fullmatch(r"3\.12\.\d+", python_version) is None:
        raise PackageBuilderError(
            f"{builder.architecture} Poetry environment has unsupported Python "
            f"{python_version!r}"
        )
    if re.fullmatch(r"1\.8\.\d+", poetry_version) is None:
        raise PackageBuilderError(
            f"{builder.architecture} Poetry environment has unsupported Poetry "
            f"{poetry_version!r}"
        )
    return PoetryEnvironment(lock_sha256, venv_path, python_version, poetry_version)


def initialize_signing_key(
    builder: Builder,
    key_path: Path,
    remote_source: str,
    region: str,
    secret_arn: str,
) -> None:
    script = f"{remote_source}/infra/initialize_package_signing_key.py"
    command = command_text(
        [
            "python3",
            script,
            "--region",
            region,
            "--secret-arn",
            secret_arn,
        ]
    )
    ssh_run(builder, key_path, command)


def remote_worker_argv(
    *,
    builder: Builder,
    remote_source: str,
    revision: str,
    poetry_environment: PoetryEnvironment,
    selection: Selection,
    region: str,
    bucket: str,
    s3_prefix: str,
    secret_arn: str,
    manifest_path: str,
    build_root: str,
) -> tuple[str, ...]:
    """Return the exact argv used to identify one authoritative worker."""

    packaging_dir = f"{remote_source}/infra/packaging"
    return (
        f"{poetry_environment.venv_path}/bin/python",
        f"{packaging_dir}/build_packages.py",
        "--architecture",
        builder.architecture,
        "--bucket",
        bucket,
        "--region",
        region,
        "--source-dir",
        remote_source,
        "--build-root",
        build_root,
        "--manifest",
        manifest_path,
        "--s3-prefix",
        s3_prefix,
        "--signing-secret-arn",
        secret_arn,
        "--postgres",
        ",".join(str(value) for value in selection.postgresql_majors),
        "--distros",
        ",".join(selection.distributions),
        "--source-revision",
        revision,
        "--poetry-lock-sha256",
        poetry_environment.lock_sha256,
        "--fail-fast",
    )


def remote_unit_status(
    builder: Builder, key_path: Path, unit_name: str
) -> dict[str, str]:
    if re.fullmatch(r"[A-Za-z0-9_.@-]+\.service", unit_name) is None:
        raise PackageBuilderError(f"invalid remote systemd unit name {unit_name!r}")
    result = ssh_run(
        builder,
        key_path,
        command_text(
            [
                "sudo",
                "systemctl",
                "show",
                unit_name,
                "--no-pager",
                "--property=LoadState",
                "--property=ActiveState",
                "--property=SubState",
                "--property=Result",
                "--property=ExecMainStatus",
            ]
        ),
        capture=True,
    )
    status: dict[str, str] = {}
    for line in result.stdout.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            status[key] = value
    required = {"LoadState", "ActiveState", "SubState", "Result", "ExecMainStatus"}
    if set(status) != required:
        raise PackageBuilderError(
            f"{builder.architecture} systemd unit {unit_name} returned malformed "
            f"status fields {sorted(status)}"
        )
    return status


def exact_worker_processes(
    builder: Builder,
    key_path: Path,
    worker_argv: Sequence[str],
) -> tuple[int, ...]:
    """Return PIDs whose complete NUL-delimited argv equals the worker argv."""

    expected = tuple(str(value) for value in worker_argv)
    if (
        len(expected) < 2
        or not expected[0].startswith(f"{REMOTE_POETRY_VENV_ROOT}/")
        or not expected[0].endswith("/bin/python")
        or not expected[1].endswith("/infra/packaging/build_packages.py")
    ):
        raise PackageBuilderError(
            f"{builder.architecture} exact worker argv is missing or malformed"
        )
    scanner = """import json
import pathlib
import sys

expected = json.loads(sys.argv[1])
found = []
for process in pathlib.Path("/proc").glob("[0-9]*"):
    try:
        raw = process.joinpath("cmdline").read_bytes()
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        continue
    argv = [value.decode("utf-8", "replace") for value in raw.rstrip(b"\\0").split(b"\\0")]
    if argv == expected:
        found.append(int(process.name))
print(json.dumps(sorted(found)))
"""
    result = ssh_run(
        builder,
        key_path,
        command_text(
            ["sudo", "python3", "-c", scanner, json.dumps(list(expected))]
        ),
        capture=True,
    )
    try:
        raw = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"{builder.architecture} exact worker probe returned invalid JSON"
        ) from error
    if (
        not isinstance(raw, list)
        or any(not isinstance(pid, int) or pid <= 0 for pid in raw)
    ):
        raise PackageBuilderError(
            f"{builder.architecture} exact worker probe returned malformed PIDs"
        )
    return tuple(raw)


def require_exact_worker_absent(
    builder: Builder,
    key_path: Path,
    worker_argv: Sequence[str],
) -> None:
    pids = exact_worker_processes(builder, key_path, worker_argv)
    if pids:
        raise PackageBuilderError(
            f"{builder.architecture} authoritative package worker is still running: "
            f"PIDs {list(pids)}"
        )


def package_worker_processes(builder: Builder, key_path: Path) -> tuple[int, ...]:
    """Return every process running a script named build_packages.py."""

    scanner = """import json
import pathlib

found = []
for process in pathlib.Path("/proc").glob("[0-9]*"):
    try:
        raw = process.joinpath("cmdline").read_bytes()
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        continue
    argv = [value.decode("utf-8", "replace") for value in raw.rstrip(b"\\0").split(b"\\0")]
    if any(value.rsplit("/", 1)[-1] == "build_packages.py" for value in argv):
        found.append(int(process.name))
print(json.dumps(sorted(found)))
"""
    result = ssh_run(
        builder,
        key_path,
        command_text(["sudo", "python3", "-c", scanner]),
        capture=True,
    )
    try:
        raw = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"{builder.architecture} package-worker process probe returned invalid JSON"
        ) from error
    if (
        not isinstance(raw, list)
        or any(
            not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0
            for pid in raw
        )
        or raw != sorted(set(raw))
    ):
        raise PackageBuilderError(
            f"{builder.architecture} package-worker process probe returned malformed PIDs"
        )
    return tuple(raw)


def active_package_units(builder: Builder, key_path: Path) -> tuple[str, ...]:
    """Return every non-quiescent package-worker transient systemd unit."""

    result = ssh_run(
        builder,
        key_path,
        command_text(
            [
                "sudo",
                "systemctl",
                "list-units",
                "bioseq-package-*.service",
                "--type=service",
                "--all",
                "--no-pager",
                "--output=json",
            ]
        ),
        capture=True,
    )
    try:
        raw = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"{builder.architecture} package-unit probe returned invalid JSON"
        ) from error
    if not isinstance(raw, list):
        raise PackageBuilderError(
            f"{builder.architecture} package-unit probe returned malformed rows"
        )
    active: list[str] = []
    for row in raw:
        if (
            not isinstance(row, dict)
            or not isinstance(row.get("unit"), str)
            or PACKAGE_UNIT_RE.fullmatch(row["unit"]) is None
            or not isinstance(row.get("load"), str)
            or not isinstance(row.get("active"), str)
            or not isinstance(row.get("sub"), str)
        ):
            raise PackageBuilderError(
                f"{builder.architecture} package-unit probe returned malformed row {row}"
            )
        state = row["active"]
        if state in {"inactive", "failed"}:
            continue
        if state not in {
            "active",
            "activating",
            "deactivating",
            "reloading",
            "refreshing",
            "maintenance",
        }:
            raise PackageBuilderError(
                f"{builder.architecture} package-unit probe returned unknown state "
                f"{state!r} for {row['unit']}"
            )
        active.append(row["unit"])
    return tuple(sorted(set(active)))


def running_package_containers(
    builder: Builder, key_path: Path
) -> tuple[str, ...]:
    """Return active containers matching package names and reviewed bind mounts."""

    probe = """set -Eeuo pipefail
ids=$(sudo docker ps --filter status=running --no-trunc --quiet)
if [[ -z $ids ]]; then
  printf '[]\n'
else
  readarray -t container_ids <<< "$ids"
  sudo docker inspect --type container "${container_ids[@]}"
fi
"""
    result = ssh_run(
        builder,
        key_path,
        f"bash -lc {shlex.quote(probe)}",
        capture=True,
    )
    try:
        raw = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"{builder.architecture} package-container probe returned invalid JSON"
        ) from error
    if not isinstance(raw, list):
        raise PackageBuilderError(
            f"{builder.architecture} package-container probe returned malformed rows"
        )
    active: list[str] = []
    seen_ids: set[str] = set()
    reviewed_destinations = {
        "/source",
        "/work",
        "/repo",
        "/root/rpmbuild",
        "/opt/bioseq-package-toolchain",
        "/opt/bioseq-package-cmake-cache",
    }
    for row in raw:
        if (
            not isinstance(row, dict)
            or re.fullmatch(r"[0-9a-f]{12,64}", str(row.get("Id", ""))) is None
            or not isinstance(row.get("Name"), str)
            or not isinstance(row.get("State"), dict)
            or not isinstance(row["State"].get("Running"), bool)
            or not isinstance(row.get("Mounts"), list)
        ):
            raise PackageBuilderError(
                f"{builder.architecture} package-container probe returned malformed row"
            )
        container_id = str(row["Id"])
        if container_id in seen_ids:
            raise PackageBuilderError(
                f"{builder.architecture} package-container probe returned duplicate ID"
            )
        seen_ids.add(container_id)
        if not row["State"]["Running"]:
            continue
        reviewed_mount = False
        for mount in row["Mounts"]:
            if (
                not isinstance(mount, dict)
                or not isinstance(mount.get("Type"), str)
                or not isinstance(mount.get("Source"), str)
                or not isinstance(mount.get("Destination"), str)
            ):
                raise PackageBuilderError(
                    f"{builder.architecture} package-container probe returned "
                    "malformed mount"
                )
            source = mount["Source"]
            destination = mount["Destination"]
            source_is_package_workspace = (
                source.startswith(f"{REMOTE_WORKSPACE}/source-")
                or source.startswith(f"{REMOTE_ROOT}/")
                or source.startswith(f"{REMOTE_WORKSPACE}/caches/package-")
            )
            if (
                mount["Type"] == "bind"
                and source_is_package_workspace
                and destination in reviewed_destinations
            ):
                reviewed_mount = True
        reviewed_name = PACKAGE_CONTAINER_NAME_RE.fullmatch(row["Name"]) is not None
        if reviewed_name != reviewed_mount:
            raise PackageBuilderError(
                f"{builder.architecture} running container has inconsistent package "
                f"identity: {row['Name']!r}"
            )
        if reviewed_name:
            active.append(f"{container_id}:{row['Name'].removeprefix('/')}")
    return tuple(sorted(active))


def successful_terminal_unit_status(
    status: dict[str, str], *, allow_not_found: bool
) -> bool:
    load_state = status.get("LoadState")
    return (
        (load_state == "loaded" or (allow_not_found and load_state == "not-found"))
        and status.get("ActiveState") == "inactive"
        and status.get("SubState") == "dead"
        and status.get("Result") == "success"
        and status.get("ExecMainStatus") == "0"
    )


def require_successful_remote_unit(
    builder: Builder,
    key_path: Path,
    unit_name: str,
    *,
    allow_not_found: bool = False,
) -> dict[str, str]:
    status = remote_unit_status(builder, key_path, unit_name)
    if not successful_terminal_unit_status(
        status, allow_not_found=allow_not_found
    ):
        raise PackageBuilderError(
            f"{builder.architecture} systemd unit {unit_name} is not a completed "
            f"success: {status}"
        )
    return status


def require_quiescent_remote_worker(
    builder: Builder,
    key_path: Path,
    unit_name: str,
    worker_argv: Sequence[str],
    *,
    allow_not_found: bool,
) -> dict[str, str]:
    del worker_argv
    processes = package_worker_processes(builder, key_path)
    if processes:
        raise PackageBuilderError(
            f"{builder.architecture} package workers are still running: "
            f"PIDs {list(processes)}"
        )
    units = active_package_units(builder, key_path)
    if units:
        raise PackageBuilderError(
            f"{builder.architecture} package systemd units are still active: "
            f"{list(units)}"
        )
    containers = running_package_containers(builder, key_path)
    if containers:
        raise PackageBuilderError(
            f"{builder.architecture} package Docker containers are still running: "
            f"{list(containers)}"
        )
    return require_successful_remote_unit(
        builder,
        key_path,
        unit_name,
        allow_not_found=allow_not_found,
    )


def launch_remote_build(
    *,
    builder: Builder,
    key_path: Path,
    remote_source: str,
    revision: str,
    poetry_environment: PoetryEnvironment,
    selection: Selection,
    region: str,
    bucket: str,
    s3_prefix: str,
    secret_arn: str,
    run_id: str,
) -> RemoteBuildHandle:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_path = LOG_DIR / f"{run_id}-{builder.architecture}.log"
    manifest_path = f"{REMOTE_ROOT}/{run_id}-{builder.architecture}-manifest.json"
    build_root = f"{REMOTE_ROOT}/{run_id}-{builder.architecture}"
    packaging_dir = f"{remote_source}/infra/packaging"
    safe_run_id = re.sub(r"[^A-Za-z0-9_.-]", "-", run_id)
    unit_name = f"bioseq-package-{safe_run_id}-{builder.architecture}.service"
    remote_log_path = f"{build_root}/build.log"
    worker_argv = remote_worker_argv(
        builder=builder,
        remote_source=remote_source,
        revision=revision,
        poetry_environment=poetry_environment,
        selection=selection,
        region=region,
        bucket=bucket,
        s3_prefix=s3_prefix,
        secret_arn=secret_arn,
        manifest_path=manifest_path,
        build_root=build_root,
    )
    systemd_argv = [
        "sudo",
        "systemd-run",
        "--unit",
        unit_name,
        "--property=Type=exec",
        "--property=User=ubuntu",
        "--property=Group=ubuntu",
        f"--property=RuntimeMaxSec={REMOTE_BUILD_RUNTIME_MAX}",
        f"--property=TimeoutStopSec={REMOTE_BUILD_STOP_TIMEOUT}",
        "--property=KillMode=control-group",
        f"--property=WorkingDirectory={packaging_dir}",
        f"--property=StandardOutput=append:{remote_log_path}",
        f"--property=StandardError=append:{remote_log_path}",
        "--",
        "/usr/bin/env",
        "AWS_DEFAULT_REGION=" + region,
        f"BIOSEQ_PRODUCTION_OPTIMIZATION={PRODUCTION_OPTIMIZATION}",
        "BIOSEQ_TEMPLATE_FLATTEN_PROFILE=dna4_uint64",
        "BIOSEQ_PACKAGE_PARALLELISM=4",
        f"BIOSEQ_POETRY_VERSION={poetry_environment.poetry_version}",
        "BIOSEQ_COMPILER_RT_LAUNCHER=/usr/local/bin/bioseq-compiler-rt-launcher",
        "CMAKE_C_COMPILER_LAUNCHER=/usr/local/bin/bioseq-compiler-rt-launcher",
        "CMAKE_CXX_COMPILER_LAUNCHER=/usr/local/bin/bioseq-compiler-rt-launcher",
        "HOME=/home/ubuntu",
        "VCPKG_MAX_CONCURRENCY=4",
        f"PYTHONPATH={packaging_dir}",
        f"VIRTUAL_ENV={poetry_environment.venv_path}",
        (
            f"PATH={poetry_environment.venv_path}/bin:/usr/local/sbin:"
            "/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
        ),
        *worker_argv,
    ]
    source_check = (
        "set -Eeuo pipefail; "
        f"test \"$(cat {shlex.quote(remote_source + '/.bioseq-source-revision')})\" "
        f"= {shlex.quote(revision)}; "
        f"test \"$(sha256sum {shlex.quote(packaging_dir + '/poetry.lock')} | "
        f"awk '{{print $1}}')\" = {shlex.quote(poetry_environment.lock_sha256)}; "
        f"{command_text(['python3', '-c', REMOTE_PACKAGE_ACTIVITY_PROBE_SCRIPT, REMOTE_WORKSPACE])}; "
        f"install -d -o ubuntu -g ubuntu -m 0755 {shlex.quote(build_root)}; "
        f"{command_text(systemd_argv)}"
    )
    info(f"{builder.architecture} build log: {log_path}")
    ssh_run(
        builder,
        key_path,
        command_text(
            [
                "sudo",
                "flock",
                "--exclusive",
                "--wait",
                "120",
                REMOTE_PACKAGE_LIFECYCLE_LOCK,
                "/bin/bash",
                "-lc",
                source_check,
            ]
        ),
    )
    return RemoteBuildHandle(
        builder=builder,
        unit_name=unit_name,
        remote_log_path=remote_log_path,
        manifest_path=manifest_path,
        build_root=build_root,
        local_log_path=log_path,
        worker_argv=worker_argv,
        launch_acknowledged=True,
    )


def selected_package_cmake_cache_paths(selection: Selection) -> list[str]:
    return sorted(
        f"{distro}/postgresql{postgres}/build-repository-root/CMakeCache.txt"
        for distro in selection.distributions
        for postgres in selection.postgresql_majors
    )


def requested_package_cmake_cache_paths(
    requested: object,
    *,
    label: str,
) -> list[str]:
    if not isinstance(requested, dict):
        raise PackageBuilderError(f"{label} must be an object")
    require_exact_keys(requested, {"postgres", "distros"}, label)
    distros = requested["distros"]
    postgres = requested["postgres"]
    matrix = load_matrix()
    if (
        not isinstance(distros, list)
        or not distros
        or any(not isinstance(value, str) for value in distros)
        or len(set(distros)) != len(distros)
        or any(value not in matrix.distributions for value in distros)
        or not isinstance(postgres, list)
        or not postgres
        or any(type(value) is not int for value in postgres)
        or len(set(postgres)) != len(postgres)
        or any(value not in matrix.postgresql_majors for value in postgres)
    ):
        raise PackageBuilderError(f"{label} contains non-canonical package cells")
    return selected_package_cmake_cache_paths(
        Selection((), tuple(distros), tuple(postgres))
    )


def require_compiler_optimization_evidence(
    value: object,
    *,
    architecture: str,
    label: str,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise PackageBuilderError(f"{label} must be an object")
    require_exact_keys(
        value,
        {
            "policy_revision",
            "requested",
            "architecture",
            "build_type",
            "c_compiler",
            "cxx_compiler",
            "effective_c_flags",
            "effective_cxx_flags",
            "optimization_flags",
            "forbidden_optimization_flags",
            "cmake_cache_paths",
            "compile_database_count",
            "compile_databases",
            "compile_command_count",
            "language_compile_commands",
            "compile_databases_sha256",
            "ninja_build_file_count",
            "ninja_build_files",
            "ninja_compile_command_count",
            "link_command_count",
            "ninja_commands_sha256",
        },
        label,
    )
    expected_c_flags = ["-O3", "-DNDEBUG", "-fno-omit-frame-pointer"]
    expected_cxx_flags = [
        "-stdlib=libc++",
        "-O3",
        "-DNDEBUG",
        "-fno-omit-frame-pointer",
    ]
    if (
        value["policy_revision"] != OPTIMIZATION_POLICY_REVISION
        or type(value["policy_revision"]) is not int
        or value["requested"] != PRODUCTION_OPTIMIZATION
        or value["architecture"] != architecture
        or value["build_type"] != "Release"
        or value["effective_c_flags"] != expected_c_flags
        or value["effective_cxx_flags"] != expected_cxx_flags
        or value["optimization_flags"] != [PRODUCTION_OPTIMIZATION_FLAG]
        or value["forbidden_optimization_flags"] != []
    ):
        raise PackageBuilderError(f"{label} does not prove the O3-only policy")
    for key, basename in (("c_compiler", "clang"), ("cxx_compiler", "clang++")):
        compiler = value[key]
        if (
            not isinstance(compiler, str)
            or not compiler.startswith("/")
            or Path(compiler).name != basename
        ):
            raise PackageBuilderError(f"{label} has an invalid {key}")
    cache_paths = value["cmake_cache_paths"]
    if (
        not isinstance(cache_paths, list)
        or not cache_paths
        or cache_paths != sorted(set(cache_paths))
        or any(
            not isinstance(path, str)
            or re.fullmatch(
                r"[A-Za-z0-9][A-Za-z0-9._-]*/postgresql[0-9]+/"
                r"build-repository-root/CMakeCache\.txt",
                path,
            )
            is None
            for path in cache_paths
        )
    ):
        raise PackageBuilderError(f"{label} has invalid primary CMake cache paths")
    for key in ("compile_database_count", "compile_command_count"):
        if type(value[key]) is not int or value[key] <= 0:
            raise PackageBuilderError(f"{label} has an invalid {key}")
    language_counts = value["language_compile_commands"]
    if (
        not isinstance(language_counts, dict)
        or set(language_counts) != {"C", "CXX"}
        or any(type(language_counts[key]) is not int for key in ("C", "CXX"))
        or any(language_counts[key] <= 0 for key in ("C", "CXX"))
        or sum(language_counts.values()) > value["compile_command_count"]
    ):
        raise PackageBuilderError(
            f"{label} must include positive C and C++ command counts"
        )
    databases = value["compile_databases"]
    if (
        not isinstance(databases, list)
        or len(databases) != value["compile_database_count"]
    ):
        raise PackageBuilderError(f"{label} has invalid compile database records")
    database_paths: list[str] = []
    database_language_totals = {"C": 0, "CXX": 0}
    database_command_total = 0
    database_digest_records: list[str] = []
    cache_directories = [path.removesuffix("CMakeCache.txt") for path in cache_paths]
    cache_language_totals = {
        directory: {"C": 0, "CXX": 0} for directory in cache_directories
    }
    for index, database in enumerate(databases):
        database_label = f"{label} compile database {index}"
        if not isinstance(database, dict):
            raise PackageBuilderError(f"{database_label} must be an object")
        require_exact_keys(
            database,
            {"path", "sha256", "command_count", "language_compile_commands"},
            database_label,
        )
        path = database["path"]
        digest = database["sha256"]
        command_count = database["command_count"]
        counts = database["language_compile_commands"]
        if (
            not isinstance(path, str)
            or path.startswith("/")
            or ".." in path.split("/")
            or not path.endswith("/compile_commands.json")
            or not any(path.startswith(directory) for directory in cache_directories)
            or not isinstance(digest, str)
            or re.fullmatch(r"[0-9a-f]{64}", digest) is None
            or type(command_count) is not int
            or command_count <= 0
            or not isinstance(counts, dict)
            or set(counts) != {"C", "CXX"}
            or any(type(counts[key]) is not int for key in ("C", "CXX"))
            or any(counts[key] < 0 for key in ("C", "CXX"))
            or sum(counts.values()) != command_count
        ):
            raise PackageBuilderError(
                f"{database_label} does not account for every C/C++ O3 command"
            )
        cache_owners = [
            directory for directory in cache_directories if path.startswith(directory)
        ]
        if len(cache_owners) != 1:
            raise PackageBuilderError(
                f"{database_label} does not have one selected cache owner"
            )
        database_paths.append(path)
        database_command_total += command_count
        for language in database_language_totals:
            database_language_totals[language] += counts[language]
            cache_language_totals[cache_owners[0]][language] += counts[language]
        database_digest_records.append(
            f"{path}\0{digest}\0{command_count}"
        )
    if (
        database_paths != sorted(set(database_paths))
        or database_command_total != value["compile_command_count"]
        or database_language_totals != language_counts
        or any(
            not any(path.startswith(directory) for path in database_paths)
            for directory in cache_directories
        )
        or any(
            counts[language] <= 0
            for counts in cache_language_totals.values()
            for language in ("C", "CXX")
        )
    ):
        raise PackageBuilderError(
            f"{label} compile database records do not cover every primary cache"
        )
    digest = value["compile_databases_sha256"]
    expected_digest = hashlib.sha256(
        "\n".join(database_digest_records).encode("utf-8")
    ).hexdigest()
    if digest != expected_digest:
        raise PackageBuilderError(f"{label} has an invalid compile database digest")

    for key in (
        "ninja_build_file_count",
        "ninja_compile_command_count",
        "link_command_count",
    ):
        if type(value[key]) is not int or value[key] <= 0:
            raise PackageBuilderError(f"{label} has an invalid {key}")
    build_files = value["ninja_build_files"]
    if (
        not isinstance(build_files, list)
        or len(build_files) != value["ninja_build_file_count"]
    ):
        raise PackageBuilderError(f"{label} has invalid Ninja build records")
    build_paths: list[str] = []
    ninja_compile_total = 0
    link_total = 0
    ninja_digest_records: list[str] = []
    cache_ninja_totals = {
        directory: {"compile": 0, "link": 0, "build_files": 0}
        for directory in cache_directories
    }
    for index, build_file in enumerate(build_files):
        build_label = f"{label} Ninja build graph {index}"
        if not isinstance(build_file, dict):
            raise PackageBuilderError(f"{build_label} must be an object")
        require_exact_keys(
            build_file,
            {
                "path",
                "sha256",
                "compile_command_count",
                "link_command_count",
                "compiler_commands_sha256",
            },
            build_label,
        )
        path = build_file["path"]
        build_digest = build_file["sha256"]
        compile_count = build_file["compile_command_count"]
        build_link_count = build_file["link_command_count"]
        command_digest = build_file["compiler_commands_sha256"]
        if (
            not isinstance(path, str)
            or path.startswith("/")
            or ".." in path.split("/")
            or not path.endswith("/build.ninja")
            or not any(path.startswith(directory) for directory in cache_directories)
            or not isinstance(build_digest, str)
            or re.fullmatch(r"[0-9a-f]{64}", build_digest) is None
            or type(compile_count) is not int
            or compile_count < 0
            or type(build_link_count) is not int
            or build_link_count < 0
            or not isinstance(command_digest, str)
            or re.fullmatch(r"[0-9a-f]{64}", command_digest) is None
        ):
            raise PackageBuilderError(
                f"{build_label} does not prove compiler/link command policy"
            )
        owners = [
            directory for directory in cache_directories if path.startswith(directory)
        ]
        if len(owners) != 1:
            raise PackageBuilderError(
                f"{build_label} does not have one selected cache owner"
            )
        build_paths.append(path)
        ninja_compile_total += compile_count
        link_total += build_link_count
        cache_ninja_totals[owners[0]]["compile"] += compile_count
        cache_ninja_totals[owners[0]]["link"] += build_link_count
        cache_ninja_totals[owners[0]]["build_files"] += 1
        ninja_digest_records.append(
            f"{path}\0{build_digest}\0{compile_count}\0"
            f"{build_link_count}\0{command_digest}"
        )
    if (
        build_paths != sorted(set(build_paths))
        or ninja_compile_total != value["ninja_compile_command_count"]
        or link_total != value["link_command_count"]
        or any(
            counts["build_files"] <= 0
            or counts["compile"] <= 0
            or counts["link"] <= 0
            for counts in cache_ninja_totals.values()
        )
    ):
        raise PackageBuilderError(
            f"{label} Ninja records do not cover every selected compile/link graph"
        )
    ninja_digest = value["ninja_commands_sha256"]
    expected_ninja_digest = hashlib.sha256(
        "\n".join(ninja_digest_records).encode("utf-8")
    ).hexdigest()
    if ninja_digest != expected_ninja_digest:
        raise PackageBuilderError(f"{label} has an invalid Ninja command digest")
    return value


def attest_remote_compiler_optimization(
    builder: Builder,
    key_path: Path,
    source_revision: str,
    expected_cache_paths: Sequence[str],
) -> dict[str, Any]:
    normalized_cache_paths = sorted(set(expected_cache_paths))
    if not normalized_cache_paths or list(expected_cache_paths) != normalized_cache_paths:
        raise PackageBuilderError(
            f"{builder.architecture} expected package cache paths are invalid"
        )
    remote_source = _remote_committed_source_path(source_revision)
    helper = f"{remote_source}/cmake/verify_o3_compile_database.py"
    selected_roots = [
        f"{REMOTE_PACKAGE_CMAKE_CACHE_ROOT}/{path.removesuffix('/CMakeCache.txt')}"
        for path in normalized_cache_paths
    ]
    selected_cache_checks = "\n".join(
        f"test -f {shlex.quote(root + '/CMakeCache.txt')}\n"
        f"test ! -L {shlex.quote(root + '/CMakeCache.txt')}"
        for root in selected_roots
    )
    find_command = command_text(
        [
            "find",
            *selected_roots,
            "-name",
            "compile_commands.json",
            "-type",
            "f",
            "-print0",
        ]
    )
    helper_check = (
        "set -Eeuo pipefail\n"
        f"helper={shlex.quote(helper)}\n"
        "test -f \"$helper\"\n"
        f"{selected_cache_checks}\n"
        f"mapfile -d '' databases < <({find_command} | sort -zu)\n"
        "(( ${#databases[@]} > 0 ))\n"
        "python3 \"$helper\" \"${databases[@]}\"\n"
    )
    ssh_run(
        builder,
        key_path,
        f"bash -lc {shlex.quote(helper_check)}",
    )
    result = ssh_run(
        builder,
        key_path,
        command_text(
            [
                "sudo",
                "python3",
                "-c",
                REMOTE_OPTIMIZATION_EVIDENCE_SCRIPT,
                REMOTE_PACKAGE_CMAKE_CACHE_ROOT,
                builder.architecture,
                json.dumps(normalized_cache_paths, separators=(",", ":")),
            ]
        ),
        capture=True,
    )
    try:
        evidence = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"{builder.architecture} O3 evidence is invalid JSON"
        ) from error
    return require_compiler_optimization_evidence(
        evidence,
        architecture=builder.architecture,
        label=f"{builder.architecture} compiler optimization evidence",
    )


def controller_package_manifest(
    worker_manifest: dict[str, Any],
    compiler_optimization: dict[str, Any],
    *,
    label: str,
) -> dict[str, Any]:
    result = dict(worker_manifest)
    result["schema_revision"] = PACKAGE_MANIFEST_SCHEMA_REVISION
    result["compiler_optimization"] = compiler_optimization
    return require_current_package_manifest(result, label=label)


def package_run_optimization_receipt(
    manifests: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    architectures = {
        architecture: require_compiler_optimization_evidence(
            manifest.get("compiler_optimization"),
            architecture=architecture,
            label=f"{architecture} run compiler optimization",
        )
        for architecture, manifest in manifests.items()
    }
    return {
        "policy_revision": OPTIMIZATION_POLICY_REVISION,
        "requested": PRODUCTION_OPTIMIZATION,
        "architectures": architectures,
    }


def require_package_run_optimization(
    value: object,
    *,
    architectures: Iterable[str],
    label: str,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise PackageBuilderError(f"{label} must be an object")
    require_exact_keys(
        value, {"policy_revision", "requested", "architectures"}, label
    )
    if (
        type(value["policy_revision"]) is not int
        or value["policy_revision"] != OPTIMIZATION_POLICY_REVISION
        or value["requested"] != PRODUCTION_OPTIMIZATION
    ):
        raise PackageBuilderError(f"{label} does not request the O3-only policy")
    evidence = value["architectures"]
    expected = set(architectures)
    if not isinstance(evidence, dict) or set(evidence) != expected:
        raise PackageBuilderError(f"{label} architecture evidence is incomplete")
    for architecture in expected:
        require_compiler_optimization_evidence(
            evidence[architecture],
            architecture=architecture,
            label=f"{label} {architecture}",
        )
    return value


def wait_remote_build(
    handle: RemoteBuildHandle,
    key_path: Path,
    *,
    timeout_seconds: int = REMOTE_BUILD_WAIT_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    terminal_status: dict[str, str] | None = None
    unit_was_observed = handle.launch_acknowledged
    while time.monotonic() < deadline:
        status = remote_unit_status(handle.builder, key_path, handle.unit_name)
        if status["LoadState"] == "loaded":
            unit_was_observed = True
        if status["ActiveState"] in {"active", "activating", "reloading", "deactivating"}:
            if status["LoadState"] != "loaded":
                raise PackageBuilderError(
                    f"{handle.builder.architecture} systemd unit {handle.unit_name} "
                    f"has ambiguous active status: {status}"
                )
            time.sleep(15)
            continue
        terminal_status = status
        break
    if terminal_status is None:
        raise PackageBuilderError(
            f"{handle.builder.architecture} systemd unit {handle.unit_name} did not "
            f"finish within {timeout_seconds // 3600} hours"
        )

    with handle.local_log_path.open("w", encoding="utf-8") as log:
        ssh_run(
            handle.builder,
            key_path,
            f"cat {shlex.quote(handle.remote_log_path)}",
            stdout=log,
            check=False,
        )
    manifest_result = ssh_run(
        handle.builder,
        key_path,
        f"cat {shlex.quote(handle.manifest_path)}",
        capture=True,
        check=False,
    )
    if not manifest_result.stdout.strip():
        raise PackageBuilderError(
            f"{handle.builder.architecture} produced no manifest; unit status "
            f"{terminal_status}; see {handle.local_log_path}"
        )
    try:
        manifest = json.loads(manifest_result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"{handle.builder.architecture} manifest is invalid JSON: {error}"
        ) from error
    worker_manifest = require_worker_package_manifest(
        manifest,
        label=f"{handle.builder.architecture} worker package manifest",
    )
    if (
        worker_manifest.get("status") != "success"
        or worker_manifest.get("architecture") != handle.builder.architecture
        or worker_manifest.get("errors") != []
    ):
        raise PackageBuilderError(
            f"{handle.builder.architecture} package build has no authoritative "
            f"terminal success manifest; unit status {terminal_status}; "
            f"see {handle.local_log_path}"
        )
    allow_not_found = unit_was_observed and terminal_status.get("LoadState") == "not-found"
    if terminal_status.get("LoadState") == "not-found" and not allow_not_found:
        raise PackageBuilderError(
            f"{handle.builder.architecture} systemd unit {handle.unit_name} was "
            "missing before any authoritative launch/load observation"
        )
    require_exact_worker_absent(handle.builder, key_path, handle.worker_argv)
    if not successful_terminal_unit_status(
        terminal_status, allow_not_found=allow_not_found
    ):
        raise PackageBuilderError(
            f"{handle.builder.architecture} package build failed with unit status "
            f"{terminal_status}; see {handle.local_log_path}; "
            f"manifest={worker_manifest.get('status')}"
        )
    compiler_optimization = attest_remote_compiler_optimization(
        handle.builder,
        key_path,
        str(worker_manifest["source_revision"]),
        requested_package_cmake_cache_paths(
            worker_manifest.get("requested"),
            label=f"{handle.builder.architecture} worker requested matrix",
        ),
    )
    return controller_package_manifest(
        worker_manifest,
        compiler_optimization,
        label=f"{handle.builder.architecture} package manifest",
    )


def expected_artifact_suffix(package_format: str) -> str:
    return {
        "deb": ".deb",
        "rpm": ".rpm",
        "pacman": ".pkg.tar.zst",
    }[package_format]


def require_smoke_attestation(
    smoke: object,
    *,
    architecture: str,
    postgres_major: int,
    distro: str,
    label: str,
) -> None:
    required_true = (
        "passed",
        "package_installed",
        "preload_active",
        "bioseq_extension_created",
        "bioseq_cache_extension_created",
        "nonempty_roundtrip",
        "fresh_container",
    )
    if not isinstance(smoke, dict):
        raise PackageBuilderError(f"{label} is not an object: {smoke}")
    require_exact_keys(
        smoke,
        {
            *required_true,
            "architecture", "postgres_major", "distro", "roundtrip",
            "container_image",
        },
        label,
    )
    if any(smoke.get(name) is not True for name in required_true):
        raise PackageBuilderError(f"{label} did not pass every assertion: {smoke}")
    if (
        smoke.get("architecture") != architecture
        or smoke.get("postgres_major") != postgres_major
        or smoke.get("distro") != distro
        or smoke.get("roundtrip") != "ACGT"
        or not isinstance(smoke.get("container_image"), str)
        or not smoke["container_image"].strip()
    ):
        raise PackageBuilderError(f"{label} attested the wrong matrix cell: {smoke}")


def validate_manifest(
    manifest: dict[str, Any],
    *,
    architecture: str,
    selection: Selection,
    matrix: Matrix,
    bucket: str,
    region: str,
    s3_prefix: str,
    source_revision: str,
    poetry_lock_sha256: str,
    python_environment: PoetryEnvironment,
) -> list[dict[str, Any]]:
    manifest = require_current_package_manifest(
        manifest, label=f"{architecture} package manifest")
    if manifest.get("status") != "success":
        raise PackageBuilderError(
            f"{architecture} manifest did not report success"
        )
    if manifest.get("package_version") != EXPECTED_PACKAGE_VERSION:
        raise PackageBuilderError(f"{architecture} manifest package version mismatch")
    if manifest.get("architecture") != architecture:
        raise PackageBuilderError(f"{architecture} manifest architecture mismatch")
    compiler_optimization = require_compiler_optimization_evidence(
        manifest.get("compiler_optimization"),
        architecture=architecture,
        label=f"{architecture} manifest compiler optimization",
    )
    if compiler_optimization["cmake_cache_paths"] != (
        selected_package_cmake_cache_paths(selection)
    ):
        raise PackageBuilderError(
            f"{architecture} manifest optimization does not cover the selected cells"
        )
    if manifest.get("source_revision") != source_revision:
        raise PackageBuilderError(f"{architecture} manifest source revision mismatch")
    if manifest.get("poetry_lock_sha256") != poetry_lock_sha256:
        raise PackageBuilderError(f"{architecture} manifest Poetry lock mismatch")
    manifest_python = manifest.get("python_environment")
    if not isinstance(manifest_python, dict):
        raise PackageBuilderError(
            f"{architecture} manifest Python environment is not an object")
    require_exact_keys(
        manifest_python, {"python", "poetry"},
        f"{architecture} manifest Python environment",
    )
    if manifest_python != python_environment.manifest_value():
        raise PackageBuilderError(f"{architecture} manifest Python environment mismatch")
    if manifest.get("errors") != []:
        raise PackageBuilderError(f"{architecture} successful manifest contains errors")
    requested = manifest.get("requested")
    if not isinstance(requested, dict):
        raise PackageBuilderError(f"{architecture} requested matrix is not an object")
    require_exact_keys(
        requested, {"postgres", "distros"},
        f"{architecture} requested matrix",
    )
    requested_pg = requested.get("postgres", [])
    if sorted(requested_pg) != sorted(selection.postgresql_majors):
        raise PackageBuilderError(f"{architecture} manifest PostgreSQL selection mismatch")
    if sorted(requested.get("distros", [])) != sorted(selection.distributions):
        raise PackageBuilderError(f"{architecture} manifest distro selection mismatch")
    s3 = manifest.get("s3")
    if not isinstance(s3, dict):
        raise PackageBuilderError(f"{architecture} manifest S3 contract is not an object")
    require_exact_keys(
        s3, {"bucket", "region", "base_prefix"},
        f"{architecture} manifest S3 contract",
    )
    if (
        s3.get("bucket") != bucket
        or s3.get("region") != region
        or s3.get("base_prefix") != s3_prefix
    ):
        raise PackageBuilderError(f"{architecture} manifest S3 contract mismatch")
    signing = manifest.get("signing")
    if not isinstance(signing, dict):
        raise PackageBuilderError(
            f"{architecture} manifest signing evidence is not an object")
    require_exact_keys(
        signing, {"fingerprint"}, f"{architecture} manifest signing evidence")
    signing_fingerprint = str(signing.get("fingerprint", ""))
    if signing_fingerprint != EXPECTED_SIGNING_FINGERPRINT:
        raise PackageBuilderError(
            f"{architecture} manifest signing-key fingerprint is not the pinned key"
        )

    cells = manifest.get("cells")
    expected_cells = {
        (major, distro)
        for major in selection.postgresql_majors
        for distro in selection.distributions
    }
    if not isinstance(cells, list) or len(cells) != len(expected_cells):
        raise PackageBuilderError(
            f"{architecture} manifest has {len(cells) if isinstance(cells, list) else 'invalid'} "
            f"cells; expected {len(expected_cells)}"
        )
    seen: set[tuple[int, str]] = set()
    uploads: list[dict[str, Any]] = []
    for cell in cells:
        key = (cell.get("postgres_major"), cell.get("distro"))
        if key not in expected_cells or key in seen:
            raise PackageBuilderError(
                f"{architecture} manifest has unexpected or duplicate cell {key}"
            )
        seen.add(key)
        if cell.get("status") != "uploaded" or cell.get("error") not in (None, ""):
            raise PackageBuilderError(f"{architecture} cell {key} was not uploaded")
        if cell.get("package_name") != f"bioseq-postgresql{key[0]}":
            raise PackageBuilderError(f"{architecture} cell {key} package name mismatch")
        require_smoke_attestation(
            cell.get("smoke"),
            architecture=architecture,
            postgres_major=key[0],
            distro=key[1],
            label=f"{architecture} cell {key} built-package smoke",
        )
        validation = cell.get("validation", {})
        required_validation = {
            "passed": True,
            "six_artifacts": True,
            "rpath_absent": True,
            "compiler_runtime_safe": True,
            "template_flatten_profile": "dna4_uint64",
        }
        if any(validation.get(name) != value for name, value in required_validation.items()):
            raise PackageBuilderError(
                f"{architecture} cell {key} failed validation: {validation}"
            )
        expected_elf = matrix.architectures[architecture].artifact_architecture
        elf_aliases = {
            "arm64": {"aarch64", "arm64"},
            "x64": {"x86_64", "x86-64", "advanced micro devices x86-64"},
        }
        binaries = validation.get("binaries")
        if not isinstance(binaries, list) or len(binaries) != 2:
            raise PackageBuilderError(
                f"{architecture} cell {key} must attest exactly two extension DSOs"
            )
        binary_names = {
            Path(str(binary.get("path", ""))).name
            for binary in binaries
            if isinstance(binary, dict)
        }
        if binary_names != {"bioseq.so", "bioseq_cache.so"}:
            raise PackageBuilderError(
                f"{architecture} cell {key} binary attestations are {sorted(binary_names)}"
            )
        for binary in binaries:
            if not isinstance(binary, dict):
                raise PackageBuilderError(
                    f"{architecture} cell {key} has malformed binary validation"
                )
            actual_elf = str(binary.get("elf_architecture", "")).lower()
            if (
                actual_elf not in elf_aliases[architecture]
                and expected_elf not in actual_elf
            ):
                raise PackageBuilderError(
                    f"{architecture} cell {key} ELF architecture mismatch: {actual_elf!r}"
                )
            if (
                binary.get("rpath_absent") is not True
                or binary.get("compiler_runtime_safe") is not True
            ):
                raise PackageBuilderError(
                    f"{architecture} cell {key} binary safety attestation failed"
                )
        artifact = cell.get("artifact", {})
        filename = str(artifact.get("filename", ""))
        suffix = expected_artifact_suffix(matrix.distributions[key[1]].package_format)
        if not filename.endswith(suffix):
            raise PackageBuilderError(
                f"{architecture} cell {key} artifact {filename!r} does not end in {suffix}"
            )
        if not isinstance(artifact.get("size"), int) or artifact["size"] <= 0:
            raise PackageBuilderError(f"{architecture} cell {key} artifact is empty")
        if not re.fullmatch(r"[0-9a-f]{64}", str(artifact.get("sha256", ""))):
            raise PackageBuilderError(f"{architecture} cell {key} SHA-256 is invalid")
        if (
            not isinstance(artifact.get("build_size"), int)
            or artifact["build_size"] <= 0
            or not re.fullmatch(r"[0-9a-f]{64}", str(artifact.get("build_sha256", "")))
        ):
            raise PackageBuilderError(f"{architecture} cell {key} build provenance is invalid")
        package_format = matrix.distributions[key[1]].package_format
        # RPM signing mutates the package header during publish. DEB and
        # pacman publishing must preserve the package bytes exactly.
        if package_format != "rpm" and (
            artifact.get("build_size") != artifact.get("size")
            or artifact.get("build_sha256") != artifact.get("sha256")
        ):
            raise PackageBuilderError(
                f"{architecture} cell {key} downloaded artifact differs from built package"
            )
        upload = cell.get("upload", {})
        expected_directory = f"{s3_prefix}/{architecture}/postgresql{key[0]}/{key[1]}"
        if package_format == "deb":
            expected_directory += "/stable"
        expected_key = f"{expected_directory}/{filename}"
        if (
            upload.get("published") is not True
            or upload.get("bucket") != bucket
            or upload.get("key") != expected_key
            or upload.get("size") != artifact.get("size")
            or upload.get("sha256") != artifact.get("sha256")
            or not isinstance(upload.get("etag"), str)
            or not upload["etag"]
            or not isinstance(upload.get("version_id"), str)
            or not upload["version_id"]
            or upload.get("server_side_encryption") != "AES256"
        ):
            raise PackageBuilderError(
                f"{architecture} cell {key} upload contract mismatch: {upload}"
            )
        downloaded_validation = upload.get("download_validation", {})
        if any(
            downloaded_validation.get(name) != value
            for name, value in required_validation.items()
        ):
            raise PackageBuilderError(
                f"{architecture} cell {key} downloaded package failed validation: "
                f"{downloaded_validation}"
            )
        require_smoke_attestation(
            upload.get("download_smoke"),
            architecture=architecture,
            postgres_major=key[0],
            distro=key[1],
            label=f"{architecture} cell {key} S3-download smoke",
        )
        signature = upload.get("signature_verification", {})
        expected_signature_kind = {
            "rpm": "embedded-rpm-openpgp",
            "deb": "signed-deb-release-sha256",
            "pacman": "detached-pacman-openpgp",
        }[package_format]
        if (
            not isinstance(signature, dict)
            or signature.get("passed") is not True
            or signature.get("public_key_imported") is not True
            or signature.get("package_signature_verified") is not True
            or signature.get("repository_signature_verified") is not True
            or signature.get("format") != package_format
            or signature.get("package_signature") != expected_signature_kind
            or signature.get("signing_key_fingerprint") != signing_fingerprint
        ):
            raise PackageBuilderError(
                f"{architecture} cell {key} signature verification failed: {signature}"
            )
        signature_base = f"{s3_prefix}/{architecture}/postgresql{key[0]}/{key[1]}"
        expected_signature_keys = {
            "rpm": {
                f"{signature_base}/repodata/repomd.xml.key",
                f"{signature_base}/repodata/repomd.xml",
                f"{signature_base}/repodata/repomd.xml.asc",
            },
            "deb": {
                f"{signature_base}/stable/Release.key",
                f"{signature_base}/stable/Release",
                f"{signature_base}/stable/Release.gpg",
                f"{signature_base}/stable/InRelease",
            },
            "pacman": {
                f"{signature_base}/public.key",
                f"{expected_key}.sig",
                f"{signature_base}/{cell['package_name']}.db.tar.gz",
                f"{signature_base}/{cell['package_name']}.db.tar.gz.sig",
            },
        }[package_format]
        signature_objects = signature.get("objects")
        if not isinstance(signature_objects, list) or len(signature_objects) != len(
            expected_signature_keys
        ):
            raise PackageBuilderError(
                f"{architecture} cell {key} signature-object count is invalid"
            )
        actual_signature_keys: set[str] = set()
        for signature_object in signature_objects:
            if (
                not isinstance(signature_object, dict)
                or not isinstance(signature_object.get("key"), str)
                or not isinstance(signature_object.get("size"), int)
                or signature_object["size"] <= 0
                or re.fullmatch(
                    r"[0-9a-f]{64}", str(signature_object.get("sha256", ""))
                )
                is None
                or not isinstance(signature_object.get("etag"), str)
                or not signature_object["etag"]
                or not isinstance(signature_object.get("version_id"), str)
                or not signature_object["version_id"]
                or signature_object.get("server_side_encryption") != "AES256"
            ):
                raise PackageBuilderError(
                    f"{architecture} cell {key} has malformed signature object: "
                    f"{signature_object}"
                )
            actual_signature_keys.add(signature_object["key"])
        if actual_signature_keys != expected_signature_keys:
            raise PackageBuilderError(
                f"{architecture} cell {key} signature-object keys mismatch: "
                f"{sorted(actual_signature_keys)}"
            )
        repository_objects = signature.get("repository_objects")
        if not isinstance(repository_objects, list) or not repository_objects:
            raise PackageBuilderError(
                f"{architecture} cell {key} repository-object evidence is missing"
            )
        actual_repository_keys: list[str] = []
        for repository_object in repository_objects:
            if (
                not isinstance(repository_object, dict)
                or not isinstance(repository_object.get("key"), str)
                or not isinstance(repository_object.get("size"), int)
                or repository_object["size"] <= 0
                or re.fullmatch(
                    r"[0-9a-f]{64}", str(repository_object.get("sha256", ""))
                )
                is None
                or not isinstance(repository_object.get("etag"), str)
                or not repository_object["etag"]
                or not isinstance(repository_object.get("version_id"), str)
                or not repository_object["version_id"]
                or repository_object.get("server_side_encryption") != "AES256"
            ):
                raise PackageBuilderError(
                    f"{architecture} cell {key} has malformed repository object: "
                    f"{repository_object}"
                )
            actual_repository_keys.append(repository_object["key"])
        if len(actual_repository_keys) != len(set(actual_repository_keys)):
            raise PackageBuilderError(
                f"{architecture} cell {key} repository-object keys contain duplicates"
            )
        repository_key_set = set(actual_repository_keys)
        if repository_key_set & (expected_signature_keys | {expected_key}):
            raise PackageBuilderError(
                f"{architecture} cell {key} repository-object evidence overlaps the "
                "package or required signature set"
            )
        expected_repository_keys = {
            "deb": {
                f"{signature_base}/stable/Packages",
                f"{signature_base}/stable/Packages.gz",
            },
            "pacman": {
                f"{signature_base}/{cell['package_name']}.files.tar.gz",
                f"{signature_base}/{cell['package_name']}.files.tar.gz.sig",
                f"{signature_base}/{cell['package_name']}.db",
                f"{signature_base}/{cell['package_name']}.db.sig",
                f"{signature_base}/{cell['package_name']}.files",
                f"{signature_base}/{cell['package_name']}.files.sig",
            },
        }.get(package_format)
        if expected_repository_keys is not None:
            if repository_key_set != expected_repository_keys:
                raise PackageBuilderError(
                    f"{architecture} cell {key} repository-object keys mismatch: "
                    f"{sorted(repository_key_set)}"
                )
        else:
            rpm_repodata_prefix = f"{signature_base}/repodata/"
            if any(
                not repository_key.startswith(rpm_repodata_prefix)
                or repository_key == rpm_repodata_prefix
                for repository_key in repository_key_set
            ):
                raise PackageBuilderError(
                    f"{architecture} cell {key} RPM repository objects escaped repodata: "
                    f"{sorted(repository_key_set)}"
                )
        uploads.append(upload)
    if seen != expected_cells:
        raise PackageBuilderError(f"{architecture} manifest omitted cells")
    return uploads


def verify_s3_uploads(region: str, bucket: str, uploads: Iterable[dict[str, Any]]) -> None:
    for upload in uploads:
        signature = upload.get("signature_verification", {})
        signature_objects = signature.get("objects", []) if isinstance(signature, dict) else []
        repository_objects = (
            signature.get("repository_objects", []) if isinstance(signature, dict) else []
        )
        for attestation in [upload, *signature_objects, *repository_objects]:
            version_id = attestation.get("version_id")
            if not isinstance(version_id, str) or not version_id:
                raise PackageBuilderError(
                    f"S3 attestation has no captured VersionId for {attestation.get('key')}"
                )
            if attestation.get("server_side_encryption") != "AES256":
                raise PackageBuilderError(
                    f"S3 attestation does not require AES256 for {attestation.get('key')}"
                )
            payload = aws_json(
                region,
                "s3api",
                "head-object",
                "--bucket",
                bucket,
                "--key",
                str(attestation["key"]),
                "--version-id",
                version_id,
            )
            if payload.get("ContentLength") != attestation.get("size"):
                raise PackageBuilderError(
                    f"S3 object size mismatch for s3://{bucket}/{attestation['key']}"
                )
            expected_etag = str(attestation.get("etag", ""))
            actual_etag = str(payload.get("ETag", "")).strip('"')
            if expected_etag and actual_etag != expected_etag:
                raise PackageBuilderError(
                    f"S3 object ETag mismatch for s3://{bucket}/{attestation['key']}"
                )
            if payload.get("VersionId") != version_id:
                raise PackageBuilderError(
                    f"S3 object version mismatch for s3://{bucket}/{attestation['key']}"
                )
            encryption = payload.get("ServerSideEncryption")
            if encryption != "AES256":
                raise PackageBuilderError(
                    "S3 object does not use exact AES256 encryption: "
                    f"s3://{bucket}/{attestation['key']}"
                )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def download_exact_package(
    *,
    region: str,
    bucket: str,
    upload: dict[str, Any],
    destination: Path,
) -> None:
    version_id = str(upload.get("version_id", ""))
    key = str(upload.get("key", ""))
    if not version_id or not key:
        raise PackageBuilderError("package upload lacks exact S3 object identity")
    temporary = destination.with_name(destination.name + ".tmp")
    response = aws_json(
        region,
        "s3api",
        "get-object",
        "--bucket",
        bucket,
        "--key",
        key,
        "--version-id",
        version_id,
        str(temporary),
    )
    if (
        response.get("VersionId") != version_id
        or response.get("ServerSideEncryption") != "AES256"
    ):
        raise PackageBuilderError(f"exact S3 download identity mismatch for {key}")
    size = temporary.stat().st_size
    digest = sha256_file(temporary)
    if size != upload.get("size") or digest != upload.get("sha256"):
        temporary.unlink(missing_ok=True)
        raise PackageBuilderError(f"exact S3 download bytes mismatch for {key}")
    temporary.replace(destination)


def collect_release_assets(
    *,
    region: str,
    run_result_path: Path,
    output_dir: Path,
    expected_revision: str,
) -> dict[str, Any]:
    try:
        raw_result = json.loads(run_result_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PackageBuilderError(
            f"cannot read package run result {run_result_path}: {error}"
        ) from error
    result = require_current_package_run_result(
        raw_result, label="collected package run result"
    )
    if result.get("status") != "success":
        raise PackageBuilderError("only a successful package run can be collected")
    revision = str(result.get("source_revision", ""))
    if (
        re.fullmatch(r"[0-9a-f]{40}", expected_revision) is None
        or revision != expected_revision
        or result.get("revision") != expected_revision
    ):
        raise PackageBuilderError(
            "package run source revision does not match the requested release commit"
        )
    matrix = load_matrix()
    selection_payload = result.get("selection")
    if not isinstance(selection_payload, dict):
        raise PackageBuilderError("package run selection is malformed")
    require_exact_keys(
        selection_payload,
        {"architectures", "distributions", "postgresql_majors"},
        "package run selection",
    )
    selection = Selection(
        tuple(selection_payload["architectures"]),
        tuple(selection_payload["distributions"]),
        tuple(selection_payload["postgresql_majors"]),
    )
    if not is_complete_matrix_selection(selection, matrix):
        raise PackageBuilderError("release collection requires the complete canonical matrix")
    bucket = str(result.get("bucket", ""))
    prefix = normalize_s3_prefix(str(result.get("s3_prefix", "")))
    expected_count = (
        len(matrix.architectures)
        * len(matrix.distributions)
        * len(matrix.postgresql_majors)
    )
    if result.get("artifact_count") != expected_count:
        raise PackageBuilderError(
            f"package run artifact count is not the canonical {expected_count}"
        )
    manifests = result.get("manifests")
    if not isinstance(manifests, dict) or set(manifests) != set(matrix.architectures):
        raise PackageBuilderError("package run must contain both architecture manifests")
    require_package_run_optimization(
        result.get("optimization"),
        architectures=matrix.architectures,
        label="package run optimization receipt",
    )
    lock_sha256 = str(result.get("poetry_lock_sha256", ""))
    if re.fullmatch(r"[0-9a-f]{64}", lock_sha256) is None:
        raise PackageBuilderError("package run Poetry lock hash is malformed")

    output_dir = output_dir.expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    if any(output_dir.iterdir()):
        raise PackageBuilderError(f"release output directory is not empty: {output_dir}")

    uploads: list[dict[str, Any]] = []
    cells: list[tuple[str, dict[str, Any]]] = []
    for architecture in matrix.architectures:
        manifest = manifests[architecture]
        if (
            manifest.get("compiler_optimization")
            != result["optimization"]["architectures"][architecture]
        ):
            raise PackageBuilderError(
                f"{architecture} manifest optimization differs from run receipt"
            )
        environment = _result_poetry_environment(result, architecture, lock_sha256)
        uploads.extend(
            validate_manifest(
                manifest,
                architecture=architecture,
                selection=selection,
                matrix=matrix,
                bucket=bucket,
                region=region,
                s3_prefix=prefix,
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=environment,
            )
        )
        cells.extend((architecture, cell) for cell in manifest["cells"])
    if len(uploads) != expected_count or len(cells) != expected_count:
        raise PackageBuilderError("validated package manifests omitted release assets")
    verify_s3_uploads(region, bucket, uploads)

    assets: list[dict[str, Any]] = []
    for architecture, cell in sorted(
        cells,
        key=lambda value: (
            value[0],
            int(value[1]["postgres_major"]),
            str(value[1]["distro"]),
        ),
    ):
        artifact = cell["artifact"]
        upload = cell["upload"]
        filename = str(artifact["filename"])
        if (
            Path(filename).name != filename
            or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+~-]*", filename) is None
        ):
            raise PackageBuilderError(f"unsafe package filename in manifest: {filename!r}")
        distro = str(cell["distro"])
        postgres = int(cell["postgres_major"])
        release_name = f"{distro}--{architecture}--pg{postgres}--{filename}"
        destination = output_dir / release_name
        download_exact_package(
            region=region,
            bucket=bucket,
            upload=upload,
            destination=destination,
        )
        assets.append(
            {
                "filename": release_name,
                "architecture": architecture,
                "distro": distro,
                "postgresql_major": postgres,
                "package_format": matrix.distributions[distro].package_format,
                "size": upload["size"],
                "sha256": upload["sha256"],
                "s3": {
                    "bucket": bucket,
                    "key": upload["key"],
                    "version_id": upload["version_id"],
                    "etag": upload["etag"],
                    "server_side_encryption": "AES256",
                },
            }
        )

    run_id = str(result.get("run_id", ""))
    if RUN_ID_RE.fullmatch(run_id) is None:
        raise PackageBuilderError("package run id is malformed")
    release_identity = f"v{EXPECTED_PACKAGE_VERSION}-{revision[:12]}"
    run_receipt = output_dir / f"package-run-{release_identity}.json"
    write_json_atomic(run_receipt, result, mode=0o644)
    for architecture in sorted(manifests):
        write_json_atomic(
            output_dir / f"package-manifest-{release_identity}-{architecture}.json",
            manifests[architecture],
            mode=0o644,
        )
    collection = {
        "schema_revision": COLLECTION_SCHEMA_REVISION,
        "package_version": EXPECTED_PACKAGE_VERSION,
        "source_revision": revision,
        "release_identity": release_identity,
        "run_id": run_id,
        "matrix": {
            "architectures": list(selection.architectures),
            "distributions": list(selection.distributions),
            "postgresql_majors": list(selection.postgresql_majors),
            "artifact_count": expected_count,
        },
        "assets": assets,
    }
    write_json_atomic(
        output_dir / f"package-assets-{release_identity}.json", collection, mode=0o644
    )
    checksummed = sorted(path for path in output_dir.iterdir() if path.is_file())
    checksums = "".join(
        f"{sha256_file(path)}  {path.name}\n" for path in checksummed
    )
    (output_dir / "SHA256SUMS").write_text(checksums, encoding="ascii")
    return collection


def read_release_json(path: Path, *, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PackageBuilderError(f"cannot read {label} {path.name}: {error}") from error
    if not isinstance(value, dict):
        raise PackageBuilderError(f"{label} {path.name} must be an object")
    return value


def verify_release_asset_directory(
    *, asset_dir: Path, expected_revision: str,
) -> dict[str, Any]:
    """Validate a downloaded published release without AWS or mutable state."""

    if re.fullmatch(r"[0-9a-f]{40}", expected_revision) is None:
        raise PackageBuilderError("expected release revision must be a full Git SHA-1")
    if asset_dir.is_symlink() or not asset_dir.is_dir():
        raise PackageBuilderError(f"release asset directory is not a real directory: {asset_dir}")
    entries = list(asset_dir.iterdir())
    if any(path.is_symlink() or not path.is_file() for path in entries):
        raise PackageBuilderError("release asset directory may contain only regular files")
    entry_names = {path.name for path in entries}
    if len(entry_names) != len(entries):
        raise PackageBuilderError("release asset directory contains duplicate names")

    matrix = load_matrix()
    selection = Selection(
        tuple(matrix.architectures),
        tuple(matrix.distributions),
        matrix.postgresql_majors,
    )
    expected_count = (
        len(selection.architectures)
        * len(selection.distributions)
        * len(selection.postgresql_majors)
    )
    release_identity = f"v{EXPECTED_PACKAGE_VERSION}-{expected_revision[:12]}"
    collection_name = f"package-assets-{release_identity}.json"
    run_name = f"package-run-{release_identity}.json"
    manifest_names = {
        architecture: f"package-manifest-{release_identity}-{architecture}.json"
        for architecture in selection.architectures
    }

    collection = read_release_json(
        asset_dir / collection_name, label="release collection receipt"
    )
    require_exact_keys(
        collection,
        {
            "schema_revision",
            "package_version",
            "source_revision",
            "release_identity",
            "run_id",
            "matrix",
            "assets",
        },
        "release collection receipt",
    )
    if (
        type(collection["schema_revision"]) is not int
        or collection["schema_revision"] != COLLECTION_SCHEMA_REVISION
        or collection["package_version"] != EXPECTED_PACKAGE_VERSION
        or collection["source_revision"] != expected_revision
        or collection["release_identity"] != release_identity
    ):
        raise PackageBuilderError("release collection identity is not current")
    run_id = collection["run_id"]
    run_match = RUN_ID_RE.fullmatch(run_id) if isinstance(run_id, str) else None
    if run_match is None or run_match.group("revision") != expected_revision[:12]:
        raise PackageBuilderError("release collection run id does not match the source revision")

    matrix_receipt = collection["matrix"]
    if not isinstance(matrix_receipt, dict):
        raise PackageBuilderError("release collection matrix must be an object")
    require_exact_keys(
        matrix_receipt,
        {"architectures", "distributions", "postgresql_majors", "artifact_count"},
        "release collection matrix",
    )
    expected_matrix = {
        "architectures": list(selection.architectures),
        "distributions": list(selection.distributions),
        "postgresql_majors": list(selection.postgresql_majors),
        "artifact_count": expected_count,
    }
    if matrix_receipt != expected_matrix:
        raise PackageBuilderError("release collection matrix is not the complete current matrix")

    assets = collection["assets"]
    if not isinstance(assets, list) or len(assets) != expected_count:
        raise PackageBuilderError(
            f"release collection must contain exactly {expected_count} packages"
        )
    expected_cells = {
        (architecture, distro, postgres)
        for architecture in selection.architectures
        for distro in selection.distributions
        for postgres in selection.postgresql_majors
    }
    observed_cells: set[tuple[str, str, int]] = set()
    collection_assets_by_cell: dict[tuple[str, str, int], dict[str, Any]] = {}
    package_names: set[str] = set()
    for asset in assets:
        if not isinstance(asset, dict):
            raise PackageBuilderError("release collection contains a malformed package entry")
        require_exact_keys(
            asset,
            {
                "filename",
                "architecture",
                "distro",
                "postgresql_major",
                "package_format",
                "size",
                "sha256",
                "s3",
            },
            "release package entry",
        )
        architecture = asset["architecture"]
        distro = asset["distro"]
        postgres = asset["postgresql_major"]
        cell = (architecture, distro, postgres)
        if cell not in expected_cells or cell in observed_cells:
            raise PackageBuilderError(f"release collection has unexpected package cell {cell}")
        observed_cells.add(cell)
        collection_assets_by_cell[cell] = asset
        filename = asset["filename"]
        prefix = f"{distro}--{architecture}--pg{postgres}--"
        if (
            not isinstance(filename, str)
            or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+~-]*", filename) is None
            or not filename.startswith(prefix)
            or filename == prefix
            or filename in package_names
        ):
            raise PackageBuilderError(f"release package filename is invalid: {filename!r}")
        package_names.add(filename)
        if asset["package_format"] != matrix.distributions[distro].package_format:
            raise PackageBuilderError(f"release package format mismatch for {filename}")
        size = asset["size"]
        digest = asset["sha256"]
        if (
            type(size) is not int
            or size <= 0
            or not isinstance(digest, str)
            or re.fullmatch(r"[0-9a-f]{64}", digest) is None
        ):
            raise PackageBuilderError(f"release package byte identity is invalid: {filename}")
        package_path = asset_dir / filename
        if (
            not package_path.is_file()
            or package_path.stat().st_size != size
            or sha256_file(package_path) != digest
        ):
            raise PackageBuilderError(f"release package bytes do not match receipt: {filename}")
        s3 = asset["s3"]
        if not isinstance(s3, dict):
            raise PackageBuilderError(f"release package S3 identity is invalid: {filename}")
        require_exact_keys(
            s3,
            {"bucket", "key", "version_id", "etag", "server_side_encryption"},
            f"release package S3 identity for {filename}",
        )
        if (
            any(
                not isinstance(s3[key], str) or not s3[key]
                for key in ("bucket", "key", "version_id", "etag")
            )
            or s3["server_side_encryption"] != "AES256"
        ):
            raise PackageBuilderError(f"release package S3 identity is incomplete: {filename}")
    if observed_cells != expected_cells:
        raise PackageBuilderError("release collection omitted matrix cells")

    run_result = require_current_package_run_result(
        read_release_json(asset_dir / run_name, label="release run receipt"),
        label="release run receipt",
    )
    expected_selection = {
        "architectures": list(selection.architectures),
        "distributions": list(selection.distributions),
        "postgresql_majors": list(selection.postgresql_majors),
    }
    if (
        run_result.get("status") != "success"
        or run_result.get("revision") != expected_revision
        or run_result.get("source_revision") != expected_revision
        or run_result.get("run_id") != run_id
        or run_result.get("selection") != expected_selection
        or run_result.get("artifact_count") != expected_count
    ):
        raise PackageBuilderError("release run receipt does not match the release identity")
    embedded_manifests = run_result.get("manifests")
    if not isinstance(embedded_manifests, dict) or set(embedded_manifests) != set(
        selection.architectures
    ):
        raise PackageBuilderError("release run receipt does not contain both manifests")
    require_package_run_optimization(
        run_result.get("optimization"),
        architectures=selection.architectures,
        label="release run optimization receipt",
    )
    lock_sha256 = str(run_result.get("poetry_lock_sha256", ""))
    if re.fullmatch(r"[0-9a-f]{64}", lock_sha256) is None:
        raise PackageBuilderError("release run receipt Poetry lock hash is malformed")
    bucket = str(run_result.get("bucket", ""))
    if not bucket:
        raise PackageBuilderError("release run receipt bucket is empty")
    prefix = normalize_s3_prefix(str(run_result.get("s3_prefix", "")))
    release_region: str | None = None
    linked_cells: set[tuple[str, str, int]] = set()
    for architecture, manifest_name in manifest_names.items():
        manifest = require_current_package_manifest(
            read_release_json(asset_dir / manifest_name, label="release manifest"),
            label=f"{architecture} release manifest",
        )
        if manifest != embedded_manifests[architecture]:
            raise PackageBuilderError(f"{architecture} release manifest differs from run receipt")
        if (
            manifest.get("compiler_optimization")
            != run_result["optimization"]["architectures"][architecture]
        ):
            raise PackageBuilderError(
                f"{architecture} release optimization differs from run receipt"
            )
        manifest_s3 = manifest.get("s3")
        manifest_region = (
            manifest_s3.get("region") if isinstance(manifest_s3, dict) else None
        )
        if not isinstance(manifest_region, str) or not manifest_region:
            raise PackageBuilderError(
                f"{architecture} release manifest region is malformed"
            )
        if release_region is None:
            release_region = manifest_region
        elif release_region != manifest_region:
            raise PackageBuilderError("release manifests disagree on AWS region")
        environment = _result_poetry_environment(
            run_result, architecture, lock_sha256
        )
        validate_manifest(
            manifest,
            architecture=architecture,
            selection=selection,
            matrix=matrix,
            bucket=bucket,
            region=manifest_region,
            s3_prefix=prefix,
            source_revision=expected_revision,
            poetry_lock_sha256=lock_sha256,
            python_environment=environment,
        )
        for manifest_cell in manifest["cells"]:
            distro = str(manifest_cell["distro"])
            postgres = int(manifest_cell["postgres_major"])
            cell = (architecture, distro, postgres)
            artifact = manifest_cell["artifact"]
            upload = manifest_cell["upload"]
            expected_asset = {
                "filename": (
                    f"{distro}--{architecture}--pg{postgres}--"
                    f"{artifact['filename']}"
                ),
                "architecture": architecture,
                "distro": distro,
                "postgresql_major": postgres,
                "package_format": matrix.distributions[distro].package_format,
                "size": upload["size"],
                "sha256": upload["sha256"],
                "s3": {
                    "bucket": upload["bucket"],
                    "key": upload["key"],
                    "version_id": upload["version_id"],
                    "etag": upload["etag"],
                    "server_side_encryption": upload["server_side_encryption"],
                },
            }
            if collection_assets_by_cell.get(cell) != expected_asset:
                raise PackageBuilderError(
                    f"release collection is not cross-linked to manifest cell {cell}"
                )
            linked_cells.add(cell)
    if linked_cells != expected_cells:
        raise PackageBuilderError("release manifests did not cross-link every matrix cell")

    expected_names = package_names | {
        collection_name,
        run_name,
        *manifest_names.values(),
        "SHA256SUMS",
    }
    if entry_names != expected_names or len(entry_names) != expected_count + 5:
        raise PackageBuilderError(
            f"release asset set is incomplete: expected={sorted(expected_names)} "
            f"observed={sorted(entry_names)}"
        )
    try:
        checksum_text = (asset_dir / "SHA256SUMS").read_text(encoding="ascii")
    except (OSError, UnicodeError) as error:
        raise PackageBuilderError(f"cannot read release SHA256SUMS: {error}") from error
    if not checksum_text.endswith("\n"):
        raise PackageBuilderError("release SHA256SUMS must end with a newline")
    checksums: dict[str, str] = {}
    for line in checksum_text.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._+~-]*)", line)
        if match is None or match.group(2) in checksums:
            raise PackageBuilderError("release SHA256SUMS contains a malformed entry")
        checksums[match.group(2)] = match.group(1)
    checksum_names = expected_names - {"SHA256SUMS"}
    if set(checksums) != checksum_names:
        raise PackageBuilderError("release SHA256SUMS does not cover the exact asset set")
    for filename, digest in checksums.items():
        if sha256_file(asset_dir / filename) != digest:
            raise PackageBuilderError(f"release SHA256SUMS mismatch for {filename}")
    return {
        "status": "valid",
        "release_identity": release_identity,
        "source_revision": expected_revision,
        "package_count": expected_count,
        "asset_count": len(entry_names),
    }


def write_last_run(payload: dict[str, Any]) -> None:
    payload = require_current_package_run_result(
        payload, label="package run result")
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    temporary = LAST_RUN_FILE.with_suffix(".tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(LAST_RUN_FILE)


@contextlib.contextmanager
def exclusive_lock() -> Iterable[None]:
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    with LOCK_FILE.open("w", encoding="utf-8") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise PackageBuilderError("another package-builder operation is active") from error
        handle.write(f"pid={os.getpid()} started={utc_now()}\n")
        handle.flush()
        yield


def provision_and_start(args: argparse.Namespace, matrix: Matrix) -> tuple[
    dict[str, Any], dict[str, Builder], dict[str, dict[str, Any]], dict[str, bool]
]:
    artifact_prefix = normalize_s3_prefix(args.s3_prefix)
    if getattr(args, "existing_only", False):
        if artifact_prefix != DEFAULT_S3_PREFIX:
            raise PackageBuilderError(
                "--existing-only requires the repository's canonical S3 prefix"
            )
        if not getattr(args, "ssh_key", None):
            raise PackageBuilderError("--existing-only requires --ssh-key")
        if not getattr(args, "lifecycle_receipt", None):
            raise PackageBuilderError(
                "--existing-only requires the open CI --lifecycle-receipt"
            )
        key_path = Path(args.ssh_key).expanduser().resolve()
        lifecycle_receipt = Path(args.lifecycle_receipt).expanduser().resolve()
        outputs, builders, _ = discover_existing_infrastructure(
            args.region, matrix, ssh_key=key_path
        )
        caller_identity = {
            "Account": outputs["account_id"],
            "Arn": outputs["caller_arn"],
        }
        read_ci_lifecycle_receipt(
            lifecycle_receipt,
            region=args.region,
            identity=caller_identity,
            builders=builders,
            require_open=True,
        )
        # Validate the key before any instance is started.
        ssh_key_path(outputs)
        records = start_builders(
            args.region,
            builders,
            matrix,
            lifecycle_receipt=lifecycle_receipt,
            caller_identity=caller_identity,
        )
        builders = refreshed_builders(records, builders)
        return outputs, builders, records, {
            name: True for name in matrix.architectures
        }
    if getattr(args, "lifecycle_receipt", None):
        raise PackageBuilderError(
            "--lifecycle-receipt is valid only with --existing-only"
        )
    aws_preflight(args.region)
    ssh_cidr = normalize_ssh_cidr(args.ssh_cidr)
    info(f"SSH ingress is fail-closed to {ssh_cidr}")
    revision = commit_id()
    lock_sha256 = committed_poetry_lock_sha256(revision)
    matrix_fingerprint = complete_matrix_fingerprint(matrix, lock_sha256)
    cached: dict[str, str] = {}
    cache_hits: dict[str, bool] = {}
    for name, architecture in matrix.architectures.items():
        image = (
            None
            if args.from_scratch
            else latest_cached_ami(
                args.region,
                architecture,
                lock_sha256,
                matrix_fingerprint,
            )
        )
        cache_hits[name] = image is not None
        if image is not None:
            cached[name] = str(image["ImageId"])
            info(f"selected cached {name} AMI {cached[name]}")
        else:
            info(f"no current cached {name} AMI; using Canonical base")
    outputs = tofu_apply(
        region=args.region,
        ssh_cidr=ssh_cidr,
        cached_amis=cached,
        artifact_bucket_name=args.bucket_name or "",
        artifact_prefix=artifact_prefix,
    )
    builders = output_builders(outputs, matrix)
    records = start_builders(args.region, builders, matrix)
    builders = refreshed_builders(records, builders)
    return outputs, builders, records, cache_hits


def is_complete_matrix_selection(selection: Selection, matrix: Matrix) -> bool:
    return (
        len(selection.architectures) == len(matrix.architectures)
        and set(selection.architectures) == set(matrix.architectures)
        and len(selection.distributions) == len(matrix.distributions)
        and set(selection.distributions) == set(matrix.distributions)
        and len(selection.postgresql_majors) == len(matrix.postgresql_majors)
        and set(selection.postgresql_majors) == set(matrix.postgresql_majors)
    )


def complete_matrix_fingerprint(matrix: Matrix, lock_sha256: str) -> str:
    if re.fullmatch(r"[0-9a-f]{64}", lock_sha256) is None:
        raise PackageBuilderError("complete-matrix fingerprint requires a lock SHA-256")
    payload = {
        "bootstrap_schema_revision": BOOTSTRAP_SCHEMA_REVISION,
        "worker_generation": WORKER_GENERATION,
        "worker_generation_revision": WORKER_GENERATION_REVISION,
        "poetry_lock_sha256": lock_sha256,
        "architectures": {
            name: {
                "aws_architecture": value.aws_architecture,
                "artifact_architecture": value.artifact_architecture,
                "instance_type": value.instance_type,
            }
            for name, value in sorted(matrix.architectures.items())
        },
        "distributions": {
            name: value.package_format
            for name, value in sorted(matrix.distributions.items())
        },
        "postgresql_majors": sorted(matrix.postgresql_majors),
    }
    encoded = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def complete_selection(matrix: Matrix) -> Selection:
    """Return the repository's canonical release selection."""

    return Selection(
        tuple(matrix.architectures),
        tuple(matrix.distributions),
        matrix.postgresql_majors,
    )


def controller_store_key(source_revision: str) -> str:
    """Return the one durable controller key for an exact source revision."""

    if re.fullmatch(r"[0-9a-f]{40}", source_revision) is None:
        raise PackageBuilderError(
            "controller receipt source revision must be a full lower-case Git SHA-1"
        )
    return f"{DEFAULT_S3_PREFIX}/controllers/v1/{source_revision}.json"


def controller_builder_paths(run_id: str, architecture: str) -> dict[str, str]:
    """Return deterministic remote identities for one controller worker."""

    if RUN_ID_RE.fullmatch(run_id) is None or architecture not in {"arm64", "x64"}:
        raise PackageBuilderError("controller worker identity is malformed")
    build_root = f"{REMOTE_ROOT}/{run_id}-{architecture}"
    return {
        "unit_name": f"bioseq-package-{run_id}-{architecture}.service",
        "remote_log_path": f"{build_root}/build.log",
        "manifest_path": f"{REMOTE_ROOT}/{run_id}-{architecture}-manifest.json",
        "build_root": build_root,
    }


def _require_controller_timestamp(value: object, label: str) -> None:
    if not isinstance(value, str):
        raise PackageBuilderError(f"{label} must be an ISO-8601 UTC timestamp")
    try:
        parsed = dt.datetime.fromisoformat(value)
    except ValueError as error:
        raise PackageBuilderError(
            f"{label} must be an ISO-8601 UTC timestamp"
        ) from error
    if parsed.tzinfo is None or parsed.utcoffset() != dt.timedelta(0):
        raise PackageBuilderError(f"{label} must be an ISO-8601 UTC timestamp")


def new_controller_receipt(
    *,
    matrix: Matrix,
    records: dict[str, dict[str, Any]],
    builders: dict[str, Builder],
    outputs: dict[str, Any],
    region: str,
    owner_token: str,
    run_id: str,
    source_revision: str,
    poetry_lock_sha256: str,
    adopt_running: bool = False,
) -> dict[str, Any]:
    """Create lifecycle intent before any controller-owned start or launch."""

    run_match = RUN_ID_RE.fullmatch(run_id)
    if run_match is None or run_match.group("revision") != source_revision[:12]:
        raise PackageBuilderError("controller run ID does not match its source revision")
    if re.fullmatch(r"[0-9a-f]{64}", poetry_lock_sha256) is None:
        raise PackageBuilderError("controller Poetry lock hash is malformed")
    if set(records) != set(matrix.architectures) or set(builders) != set(
        matrix.architectures
    ):
        raise PackageBuilderError("controller requires both canonical builders")
    account = str(outputs.get("account_id", ""))
    caller_arn = str(outputs.get("caller_arn", ""))
    bucket = str(outputs.get("package_bucket_name", ""))
    if (
        re.fullmatch(r"[0-9]{12}", account) is None
        or not caller_arn.startswith("arn:")
        or not bucket
        or outputs.get("region") != region
    ):
        raise PackageBuilderError("controller infrastructure identity is incomplete")
    owner_token_sha256, _description = ci_access_owner(owner_token)
    now = utc_now()
    selection = complete_selection(matrix)
    builder_payload: dict[str, dict[str, Any]] = {}
    for name, builder in builders.items():
        initial_state = instance_state(records[name])
        if initial_state not in {"pending", "running", "stopping", "stopped"}:
            raise PackageBuilderError(
                f"controller cannot own {name} from state {initial_state!r}"
            )
        if adopt_running and initial_state != "running":
            raise PackageBuilderError(
                f"legacy adoption requires {name} already running, got {initial_state!r}"
            )
        builder_payload[name] = {
            "instance_id": builder.instance_id,
            "instance_type": builder.instance_type,
            "aws_architecture": builder.aws_architecture,
            "source_ami_id": builder.source_ami_id,
            "availability_zone": builder.availability_zone,
            "initial_state": initial_state,
            "stop_owned": False if adopt_running else initial_state in {"stopping", "stopped"},
            **controller_builder_paths(run_id, name),
            "launch_acknowledged": False,
            "python_environment": None,
        }
    payload = {
        "schema_revision": CONTROLLER_SCHEMA_REVISION,
        "kind": CONTROLLER_KIND,
        "state": "prepared",
        "created_at": now,
        "updated_at": now,
        "account": account,
        "launch_caller_arn": caller_arn,
        "region": region,
        "bucket": bucket,
        "s3_prefix": DEFAULT_S3_PREFIX,
        "store_key": controller_store_key(source_revision),
        "owner_token_sha256": owner_token_sha256,
        "run_id": run_id,
        "source_revision": source_revision,
        "poetry_lock_sha256": poetry_lock_sha256,
        "matrix_fingerprint": complete_matrix_fingerprint(matrix, poetry_lock_sha256),
        "selection": {
            "architectures": list(selection.architectures),
            "distributions": list(selection.distributions),
            "postgresql_majors": list(selection.postgresql_majors),
        },
        "optimization_policy": {
            "policy_revision": OPTIMIZATION_POLICY_REVISION,
            "requested": PRODUCTION_OPTIMIZATION,
        },
        "worker_generation": WORKER_GENERATION,
        "worker_generation_revision": WORKER_GENERATION_REVISION,
        "builders": builder_payload,
        "diagnostics": [],
        "result": None,
    }
    return require_controller_receipt(
        payload,
        matrix=matrix,
        label="new package controller receipt",
        expected_region=region,
        expected_account=account,
        expected_bucket=bucket,
        builders=builders,
        expected_source_revision=source_revision,
        expected_owner_token_sha256=owner_token_sha256,
    )


def require_controller_receipt(
    value: object,
    *,
    matrix: Matrix,
    label: str,
    expected_region: str | None = None,
    expected_account: str | None = None,
    expected_bucket: str | None = None,
    builders: dict[str, Builder] | None = None,
    expected_source_revision: str | None = None,
    expected_owner_token_sha256: str | None = None,
) -> dict[str, Any]:
    """Strictly validate a durable controller receipt and all bound identities."""

    if not isinstance(value, dict):
        raise PackageBuilderError(f"{label} must be an object")
    require_exact_keys(value, _CONTROLLER_ROOT_KEYS, label)
    source_revision = value["source_revision"]
    run_id = value["run_id"]
    run_match = RUN_ID_RE.fullmatch(str(run_id))
    lock_sha256 = value["poetry_lock_sha256"]
    if (
        type(value["schema_revision"]) is not int
        or value["schema_revision"] != CONTROLLER_SCHEMA_REVISION
        or value["kind"] != CONTROLLER_KIND
        or value["state"] not in CONTROLLER_STATES
        or re.fullmatch(r"[0-9]{12}", str(value["account"])) is None
        or not isinstance(value["launch_caller_arn"], str)
        or not value["launch_caller_arn"].startswith("arn:")
        or not isinstance(value["region"], str)
        or not value["region"]
        or not isinstance(value["bucket"], str)
        or not value["bucket"]
        or value["s3_prefix"] != DEFAULT_S3_PREFIX
        or re.fullmatch(r"[0-9a-f]{64}", str(value["owner_token_sha256"])) is None
        or re.fullmatch(r"[0-9a-f]{40}", str(source_revision)) is None
        or run_match is None
        or run_match.group("revision") != str(source_revision)[:12]
        or re.fullmatch(r"[0-9a-f]{64}", str(lock_sha256)) is None
        or value["store_key"] != controller_store_key(str(source_revision))
        or value["matrix_fingerprint"]
        != complete_matrix_fingerprint(matrix, str(lock_sha256))
        or value["worker_generation"] != WORKER_GENERATION
        or type(value["worker_generation_revision"]) is not int
        or value["worker_generation_revision"] != WORKER_GENERATION_REVISION
    ):
        raise PackageBuilderError(f"{label} identity is invalid")
    _require_controller_timestamp(value["created_at"], f"{label} created_at")
    _require_controller_timestamp(value["updated_at"], f"{label} updated_at")
    selection = value["selection"]
    if not isinstance(selection, dict):
        raise PackageBuilderError(f"{label} selection must be an object")
    require_exact_keys(
        selection,
        {"architectures", "distributions", "postgresql_majors"},
        f"{label} selection",
    )
    expected_selection = complete_selection(matrix)
    if selection != {
        "architectures": list(expected_selection.architectures),
        "distributions": list(expected_selection.distributions),
        "postgresql_majors": list(expected_selection.postgresql_majors),
    }:
        raise PackageBuilderError(f"{label} does not contain the canonical matrix")
    if value["optimization_policy"] != {
        "policy_revision": OPTIMIZATION_POLICY_REVISION,
        "requested": PRODUCTION_OPTIMIZATION,
    }:
        raise PackageBuilderError(f"{label} does not require O3")
    diagnostics = value["diagnostics"]
    if (
        not isinstance(diagnostics, list)
        or len(diagnostics) > len(matrix.architectures)
        or any(
            not isinstance(item, str)
            or not item
            or len(item) > CONTROLLER_DIAGNOSTIC_LIMIT
            for item in diagnostics
        )
    ):
        raise PackageBuilderError(f"{label} diagnostics are invalid")
    receipt_builders = value["builders"]
    if not isinstance(receipt_builders, dict) or set(receipt_builders) != set(
        matrix.architectures
    ):
        raise PackageBuilderError(f"{label} builder set is invalid")
    for name, receipt_builder in receipt_builders.items():
        if not isinstance(receipt_builder, dict):
            raise PackageBuilderError(f"{label} {name} builder must be an object")
        require_exact_keys(
            receipt_builder, _CONTROLLER_BUILDER_KEYS, f"{label} {name} builder"
        )
        expected_architecture = matrix.architectures[name]
        paths = controller_builder_paths(str(run_id), name)
        initial_state = receipt_builder["initial_state"]
        if (
            INSTANCE_ID_RE.fullmatch(str(receipt_builder["instance_id"])) is None
            or receipt_builder["instance_type"] != expected_architecture.instance_type
            or receipt_builder["aws_architecture"]
            != expected_architecture.aws_architecture
            or AMI_ID_RE.fullmatch(str(receipt_builder["source_ami_id"])) is None
            or not isinstance(receipt_builder["availability_zone"], str)
            or not receipt_builder["availability_zone"].startswith(value["region"])
            or initial_state not in {"pending", "running", "stopping", "stopped"}
            or type(receipt_builder["stop_owned"]) is not bool
            or (
                receipt_builder["stop_owned"]
                and initial_state not in {"stopping", "stopped"}
            )
            or type(receipt_builder["launch_acknowledged"]) is not bool
            or any(receipt_builder[key] != expected for key, expected in paths.items())
        ):
            raise PackageBuilderError(f"{label} {name} builder identity is invalid")
        environment = receipt_builder["python_environment"]
        if environment is not None:
            if not isinstance(environment, dict):
                raise PackageBuilderError(
                    f"{label} {name} Python environment must be an object"
                )
            require_exact_keys(
                environment,
                {"venv_path", "python", "poetry"},
                f"{label} {name} Python environment",
            )
            if (
                environment["venv_path"]
                != f"{REMOTE_POETRY_VENV_ROOT}/{lock_sha256}"
                or re.fullmatch(r"3\.12\.\d+", str(environment["python"])) is None
                or re.fullmatch(r"1\.8\.\d+", str(environment["poetry"])) is None
            ):
                raise PackageBuilderError(
                    f"{label} {name} Python environment is invalid"
                )
        if receipt_builder["launch_acknowledged"] and environment is None:
            raise PackageBuilderError(
                f"{label} {name} launched worker lacks Python provenance"
            )
        if builders is not None:
            actual = builders.get(name)
            if actual is None or any(
                (
                    receipt_builder["instance_id"],
                    receipt_builder["instance_type"],
                    receipt_builder["aws_architecture"],
                    receipt_builder["source_ami_id"],
                    receipt_builder["availability_zone"],
                )[index]
                != expected
                for index, expected in enumerate(
                    (
                        actual.instance_id,
                        actual.instance_type,
                        actual.aws_architecture,
                        actual.source_ami_id,
                        actual.availability_zone,
                    )
                )
            ):
                raise PackageBuilderError(
                    f"{label} does not name the discovered {name} builder"
                )
    result = value["result"]
    terminal_success = value["state"] in {"completed", "finalized", "collected"}
    if terminal_success:
        result = require_current_package_run_result(
            result, label=f"{label} aggregate result"
        )
        if (
            result.get("status") != "success"
            or result.get("run_id") != run_id
            or result.get("revision") != source_revision
            or result.get("source_revision") != source_revision
            or result.get("poetry_lock_sha256") != lock_sha256
            or result.get("bucket") != value["bucket"]
            or result.get("s3_prefix") != DEFAULT_S3_PREFIX
            or result.get("selection") != selection
        ):
            raise PackageBuilderError(f"{label} aggregate identity is invalid")
        require_package_run_optimization(
            result.get("optimization"),
            architectures=matrix.architectures,
            label=f"{label} aggregate optimization",
        )
        if value["state"] in {"finalized", "collected"} and (
            "cache_images" not in result
            or result.get("complete_matrix_fingerprint")
            != value["matrix_fingerprint"]
        ):
            raise PackageBuilderError(f"{label} finalization evidence is incomplete")
    elif result is not None:
        raise PackageBuilderError(f"{label} nonterminal result must be null")
    for expected, observed, field in (
        (expected_region, value["region"], "region"),
        (expected_account, value["account"], "account"),
        (expected_bucket, value["bucket"], "bucket"),
        (expected_source_revision, source_revision, "source revision"),
        (
            expected_owner_token_sha256,
            value["owner_token_sha256"],
            "owner token",
        ),
    ):
        if expected is not None and observed != expected:
            raise PackageBuilderError(f"{label} {field} mismatch")
    return value


def controller_poetry_environment(
    receipt: dict[str, Any], architecture: str
) -> PoetryEnvironment:
    value = receipt["builders"][architecture]["python_environment"]
    if not isinstance(value, dict):
        raise PackageBuilderError(
            f"controller receipt has no Python environment for {architecture}"
        )
    return PoetryEnvironment(
        lock_sha256=str(receipt["poetry_lock_sha256"]),
        venv_path=str(value["venv_path"]),
        python_version=str(value["python"]),
        poetry_version=str(value["poetry"]),
    )


def controller_handle(
    receipt: dict[str, Any], builder: Builder, outputs: dict[str, Any]
) -> RemoteBuildHandle:
    """Reconstruct one exact attach handle without a persisted private key."""

    architecture = builder.architecture
    value = receipt["builders"][architecture]
    environment = controller_poetry_environment(receipt, architecture)
    selection = complete_selection(load_matrix())
    remote_source = _remote_committed_source_path(str(receipt["source_revision"]))
    worker_argv = remote_worker_argv(
        builder=builder,
        remote_source=remote_source,
        revision=str(receipt["source_revision"]),
        poetry_environment=environment,
        selection=selection,
        region=str(receipt["region"]),
        bucket=str(receipt["bucket"]),
        s3_prefix=str(receipt["s3_prefix"]),
        secret_arn=str(outputs["signing_key_secret_arn"]),
        manifest_path=str(value["manifest_path"]),
        build_root=str(value["build_root"]),
    )
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    return RemoteBuildHandle(
        builder=builder,
        unit_name=str(value["unit_name"]),
        remote_log_path=str(value["remote_log_path"]),
        manifest_path=str(value["manifest_path"]),
        build_root=str(value["build_root"]),
        local_log_path=LOG_DIR / f"{receipt['run_id']}-{architecture}.log",
        worker_argv=worker_argv,
        launch_acknowledged=value["launch_acknowledged"] is True,
    )


def persist_controller_receipt(
    receipt_path: Path,
    payload: dict[str, Any],
    *,
    matrix: Matrix,
) -> dict[str, Any]:
    """Validate, atomically write, and upload one encrypted versioned receipt."""

    payload["updated_at"] = utc_now()
    payload = require_controller_receipt(
        payload, matrix=matrix, label="persisted package controller receipt"
    )
    receipt_path = receipt_path.expanduser().resolve()
    write_json_atomic(receipt_path, payload)
    digest = sha256_file(receipt_path)
    response = aws_json(
        str(payload["region"]),
        "s3api",
        "put-object",
        "--bucket",
        str(payload["bucket"]),
        "--key",
        str(payload["store_key"]),
        "--body",
        str(receipt_path),
        "--content-type",
        "application/json",
        "--server-side-encryption",
        "AES256",
        "--metadata",
        (
            f"receipt-sha256={digest},"
            f"source-revision={payload['source_revision']},"
            f"schema-revision={CONTROLLER_SCHEMA_REVISION}"
        ),
    )
    version_id = response.get("VersionId")
    if (
        not isinstance(version_id, str)
        or not version_id
        or response.get("ServerSideEncryption") != "AES256"
    ):
        raise PackageBuilderError(
            "S3 did not acknowledge an encrypted versioned controller receipt"
        )
    head = aws_json(
        str(payload["region"]),
        "s3api",
        "head-object",
        "--bucket",
        str(payload["bucket"]),
        "--key",
        str(payload["store_key"]),
        "--version-id",
        version_id,
    )
    if (
        head.get("VersionId") != version_id
        or head.get("ServerSideEncryption") != "AES256"
        or head.get("ContentLength") != receipt_path.stat().st_size
        or head.get("Metadata")
        != {
            "receipt-sha256": digest,
            "source-revision": payload["source_revision"],
            "schema-revision": str(CONTROLLER_SCHEMA_REVISION),
        }
    ):
        raise PackageBuilderError("stored controller receipt failed S3 verification")
    return payload


def fetch_controller_receipt(
    receipt_path: Path,
    *,
    matrix: Matrix,
    region: str,
    bucket: str,
    source_revision: str,
    required: bool,
) -> dict[str, Any] | None:
    """Fetch the exact current durable receipt, rejecting prefix ambiguity."""

    key = controller_store_key(source_revision)
    listing = aws_json(
        region,
        "s3api",
        "list-objects-v2",
        "--bucket",
        bucket,
        "--prefix",
        key,
        "--max-keys",
        "2",
    )
    contents = listing.get("Contents", [])
    if not isinstance(contents, list):
        raise PackageBuilderError("controller receipt listing is malformed")
    keys = [str(item.get("Key", "")) for item in contents if isinstance(item, dict)]
    if keys and keys != [key]:
        raise PackageBuilderError(
            f"controller receipt prefix contains ambiguous objects: {keys}"
        )
    if not keys:
        if required:
            raise PackageBuilderError(
                f"no durable controller receipt exists for {source_revision}"
            )
        return None
    destination = receipt_path.expanduser().resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".download")
    temporary.unlink(missing_ok=True)
    response = aws_json(
        region,
        "s3api",
        "get-object",
        "--bucket",
        bucket,
        "--key",
        key,
        str(temporary),
    )
    try:
        raw = temporary.read_bytes()
        payload = json.loads(raw)
    except (OSError, json.JSONDecodeError) as error:
        temporary.unlink(missing_ok=True)
        raise PackageBuilderError(
            f"durable controller receipt is unreadable: {error}"
        ) from error
    metadata = response.get("Metadata")
    digest = hashlib.sha256(raw).hexdigest()
    if (
        response.get("ServerSideEncryption") != "AES256"
        or not isinstance(response.get("VersionId"), str)
        or not response["VersionId"]
        or metadata
        != {
            "receipt-sha256": digest,
            "source-revision": source_revision,
            "schema-revision": str(CONTROLLER_SCHEMA_REVISION),
        }
    ):
        temporary.unlink(missing_ok=True)
        raise PackageBuilderError("durable controller receipt S3 metadata is invalid")
    payload = require_controller_receipt(
        payload,
        matrix=matrix,
        label="downloaded package controller receipt",
        expected_region=region,
        expected_bucket=bucket,
        expected_source_revision=source_revision,
    )
    temporary.replace(destination)
    return payload


def _result_poetry_environment(
    build_result: dict[str, Any], architecture: str, lock_sha256: str
) -> PoetryEnvironment:
    environments = build_result.get("python_environments")
    value = environments.get(architecture) if isinstance(environments, dict) else None
    if not isinstance(value, dict):
        raise PackageBuilderError(
            f"aggregate result has no Python environment for {architecture}"
        )
    environment = PoetryEnvironment(
        lock_sha256=lock_sha256,
        venv_path=str(value.get("venv_path", "")),
        python_version=str(value.get("python", "")),
        poetry_version=str(value.get("poetry", "")),
    )
    expected_path = f"{REMOTE_POETRY_VENV_ROOT}/{lock_sha256}"
    if (
        environment.venv_path != expected_path
        or re.fullmatch(r"3\.12\.\d+", environment.python_version) is None
        or re.fullmatch(r"1\.8\.\d+", environment.poetry_version) is None
    ):
        raise PackageBuilderError(
            f"aggregate result has invalid Python environment for {architecture}"
        )
    return environment


def create_post_build_amis(
    *,
    args: argparse.Namespace,
    matrix: Matrix,
    selection: Selection,
    outputs: dict[str, Any],
    builders: dict[str, Builder],
    build_result: dict[str, Any],
    lock_sha256: str,
) -> dict[str, dict[str, Any]]:
    """Image both live builders only after the canonical matrix passes again."""

    if not is_complete_matrix_selection(selection, matrix):
        raise PackageBuilderError(
            "post-build AMIs require the complete canonical architecture/distro/"
            "PostgreSQL matrix; partial selections cannot image"
        )
    if build_result.get("status") != "success":
        raise PackageBuilderError("post-build AMIs require a successful aggregate result")
    revision = str(build_result.get("revision", ""))
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise PackageBuilderError("post-build AMIs require committed source provenance")
    run_id = str(build_result.get("run_id", ""))
    run_match = RUN_ID_RE.fullmatch(run_id)
    if run_match is None or run_match.group("revision") != revision[:12]:
        raise PackageBuilderError("post-build AMIs require an exact revision-bound run ID")
    if build_result.get("poetry_lock_sha256") != lock_sha256:
        raise PackageBuilderError("post-build AMI Poetry lock provenance mismatch")
    manifests = build_result.get("manifests")
    worker_units = build_result.get("worker_units")
    expected_architectures = set(matrix.architectures)
    if not isinstance(manifests, dict) or set(manifests) != expected_architectures:
        raise PackageBuilderError("post-build AMIs require both exact architecture manifests")
    if not isinstance(worker_units, dict) or set(worker_units) != expected_architectures:
        raise PackageBuilderError("post-build AMIs require both exact worker units")
    for name in expected_architectures:
        expected_unit = f"bioseq-package-{run_id}-{name}.service"
        if worker_units.get(name) != expected_unit:
            raise PackageBuilderError(
                f"post-build AMI worker unit mismatch for {name}: "
                f"{worker_units.get(name)!r}"
            )
    bucket = outputs.get("package_bucket_name")
    if not isinstance(bucket, str) or not bucket:
        raise PackageBuilderError("OpenTofu output `package_bucket_name` is missing")
    secret_arn = outputs.get("signing_key_secret_arn")
    if not isinstance(secret_arn, str) or not secret_arn.startswith(
        "arn:aws:secretsmanager:"
    ):
        raise PackageBuilderError("OpenTofu output `signing_key_secret_arn` is missing")
    finalized_from_manifests = (
        build_result.get("finalized_from_completed_manifests") is True
    )
    launch_acknowledgements = build_result.get("worker_launch_acknowledged")
    if not finalized_from_manifests and (
        not isinstance(launch_acknowledgements, dict)
        or set(launch_acknowledgements) != expected_architectures
        or any(launch_acknowledgements.get(name) is not True for name in expected_architectures)
    ):
        raise PackageBuilderError(
            "post-build AMIs require authoritative worker launch observations"
        )
    expected_artifact_count = (
        len(matrix.architectures)
        * len(matrix.distributions)
        * len(matrix.postgresql_majors)
    )
    if build_result.get("artifact_count") != expected_artifact_count:
        raise PackageBuilderError(
            f"post-build AMIs require {expected_artifact_count} exact package uploads"
        )
    uploads: list[dict[str, Any]] = []
    for name in matrix.architectures:
        environment = _result_poetry_environment(build_result, name, lock_sha256)
        manifest = manifests[name]
        if not isinstance(manifest, dict):
            raise PackageBuilderError(f"{name} aggregate manifest is malformed")
        uploads.extend(
            validate_manifest(
                manifest,
                architecture=name,
                selection=selection,
                matrix=matrix,
                bucket=bucket,
                region=args.region,
                s3_prefix=args.s3_prefix,
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=environment,
            )
        )
    if len(uploads) != expected_artifact_count:
        raise PackageBuilderError("post-build AMI upload set is not the canonical matrix")
    verify_s3_uploads(args.region, bucket, uploads)

    matrix_fingerprint = complete_matrix_fingerprint(matrix, lock_sha256)
    key_path = ssh_key_path(outputs)
    remote_source = _remote_committed_source_path(revision)
    worker_argv_by_architecture = {
        name: remote_worker_argv(
            builder=builders[name],
            remote_source=remote_source,
            revision=revision,
            poetry_environment=_result_poetry_environment(
                build_result, name, lock_sha256
            ),
            selection=selection,
            region=args.region,
            bucket=bucket,
            s3_prefix=args.s3_prefix,
            secret_arn=secret_arn,
            manifest_path=f"{REMOTE_ROOT}/{run_id}-{name}-manifest.json",
            build_root=f"{REMOTE_ROOT}/{run_id}-{name}",
        )
        for name in matrix.architectures
    }
    allow_not_found_by_architecture = {
        name: (
            finalized_from_manifests
            or bool(launch_acknowledgements.get(name))
        )
        for name in matrix.architectures
    }
    quiescence_errors: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(matrix.architectures),
        thread_name_prefix="bioseq-cache-preflight",
    ) as executor:
        futures = {
            executor.submit(
                require_quiescent_remote_worker,
                builders[name],
                key_path,
                str(worker_units[name]),
                worker_argv_by_architecture[name],
                allow_not_found=allow_not_found_by_architecture[name],
            ): name
            for name in matrix.architectures
        }
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                future.result()
            except Exception as error:
                quiescence_errors.append(f"{name}: {error}")
    if quiescence_errors:
        raise PackageBuilderError(
            "post-build workers are not jointly quiescent: "
            + "; ".join(quiescence_errors)
        )
    images: dict[str, dict[str, Any]] = {}
    errors: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(matrix.architectures), thread_name_prefix="bioseq-cache"
    ) as executor:
        futures = {
            executor.submit(
                create_cached_ami,
                args.region,
                builders[name],
                matrix.architectures[name],
                key_path,
                revision,
                lock_sha256,
                matrix_fingerprint,
                str(worker_units[name]),
                worker_argv_by_architecture[name],
                allow_not_found=allow_not_found_by_architecture[name],
            ): name
            for name in matrix.architectures
        }
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                images[name] = future.result()
            except Exception as error:
                errors.append(f"{name}: {error}")
    if errors:
        raise PackageBuilderError(
            "post-build package cache creation failed: " + "; ".join(errors)
        )
    build_result["complete_matrix_fingerprint"] = matrix_fingerprint
    return images


def execute_build(
    args: argparse.Namespace,
    matrix: Matrix,
    selection: Selection,
    outputs: dict[str, Any],
    builders: dict[str, Builder],
) -> dict[str, Any]:
    revision = commit_id()
    require_committed_paths(
        [
            HERE / "package_builder.py",
            HERE / "initialize_package_signing_key.py",
            HERE / "package_matrix.json",
            STACK_DIR / "main.tf",
            STACK_DIR / "variables.tf",
            STACK_DIR / "outputs.tf",
            STACK_DIR / "scripts" / "bootstrap.sh",
            STACK_DIR / "scripts" / "user_data.sh.tftpl",
            PACKAGING_DIR / "pyproject.toml",
            PACKAGING_DIR / "poetry.lock",
            PACKAGING_DIR / "build_packages.py",
            PACKAGING_DIR / "bioseq_packaging",
            PACKAGING_DIR / "rpm",
            PACKAGING_DIR / "deb",
            PACKAGING_DIR / "pacman",
            PACKAGING_DIR / "scripts",
        ]
    )
    lock_sha256 = committed_poetry_lock_sha256(revision)
    key_path = ssh_key_path(outputs)
    selected_builders = {
        name: builders[name] for name in selection.architectures
    }
    bucket = outputs.get("package_bucket_name")
    secret_arn = outputs.get("signing_key_secret_arn")
    output_region = outputs.get("region")
    if not isinstance(bucket, str) or not bucket:
        raise PackageBuilderError("OpenTofu output `package_bucket_name` is missing")
    if not isinstance(secret_arn, str) or not secret_arn.startswith("arn:aws:secretsmanager:"):
        raise PackageBuilderError("OpenTofu output `signing_key_secret_arn` is missing")
    if output_region != args.region:
        raise PackageBuilderError(
            f"OpenTofu region output {output_region!r} does not match {args.region!r}"
        )

    remote_sources: dict[str, str] = {}
    poetry_environments: dict[str, PoetryEnvironment] = {}

    def prepare_builder(name: str, builder: Builder) -> tuple[str, PoetryEnvironment]:
        wait_for_ssh(builder, key_path)
        remote_source = sync_committed_source(builder, key_path, revision)
        reconcile_remote_bootstrap(builder, key_path, remote_source)
        force_remote_package_reconfigure(builder, key_path)
        environment = prepare_remote_poetry_environment(
            builder,
            key_path,
            remote_source,
            revision,
            lock_sha256,
        )
        return remote_source, environment

    preparation_errors: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(selected_builders), thread_name_prefix="bioseq-prepare"
    ) as executor:
        preparation_futures = {
            executor.submit(prepare_builder, name, builder): name
            for name, builder in selected_builders.items()
        }
        for future in concurrent.futures.as_completed(preparation_futures):
            name = preparation_futures[future]
            try:
                remote_source, environment = future.result()
                remote_sources[name] = remote_source
                poetry_environments[name] = environment
                info(f"{name} committed source and Poetry environment are ready")
            except Exception as error:
                preparation_errors.append(f"{name}: {error}")
    if preparation_errors:
        raise PackageBuilderError(
            "native builder preparation failed: " + "; ".join(preparation_errors)
        )

    signing_host_name = "x64" if "x64" in selected_builders else selection.architectures[0]
    initialize_signing_key(
        selected_builders[signing_host_name],
        key_path,
        remote_sources[signing_host_name],
        args.region,
        secret_arn,
    )

    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + f"-{revision[:12]}"
    handles: dict[str, RemoteBuildHandle] = {}
    build_results: dict[str, dict[str, Any]] = {}
    errors: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(selected_builders), thread_name_prefix="bioseq-launch"
    ) as executor:
        launch_futures = {
            executor.submit(
                launch_remote_build,
                builder=builder,
                key_path=key_path,
                remote_source=remote_sources[name],
                revision=revision,
                poetry_environment=poetry_environments[name],
                selection=selection,
                region=args.region,
                bucket=bucket,
                s3_prefix=args.s3_prefix,
                secret_arn=secret_arn,
                run_id=run_id,
            ): name
            for name, builder in selected_builders.items()
        }
        for future in concurrent.futures.as_completed(launch_futures):
            name = launch_futures[future]
            try:
                handles[name] = future.result()
                info(f"{name} native systemd worker launched")
            except Exception as error:
                errors.append(f"{name} launch: {error}")

    # Every successfully launched unit is allowed to finish even if its sibling
    # failed to launch. Both units are started before either one is awaited.
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=max(1, len(handles)), thread_name_prefix="bioseq-package"
    ) as executor:
        wait_futures = {
            executor.submit(wait_remote_build, handle, key_path): name
            for name, handle in handles.items()
        }
        for future in concurrent.futures.as_completed(wait_futures):
            name = wait_futures[future]
            try:
                build_results[name] = future.result()
                info(f"{name} native package matrix completed")
            except Exception as error:
                errors.append(f"{name} build: {error}")
    if errors:
        raise PackageBuilderError("native package builds failed: " + "; ".join(errors))
    if set(build_results) != set(selected_builders):
        raise PackageBuilderError("native package builds omitted an architecture")

    uploads: list[dict[str, Any]] = []
    for name, manifest in build_results.items():
        uploads.extend(
            validate_manifest(
                manifest,
                architecture=name,
                selection=selection,
                matrix=matrix,
                bucket=bucket,
                region=args.region,
                s3_prefix=args.s3_prefix,
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=poetry_environments[name],
            )
        )
    verify_s3_uploads(args.region, bucket, uploads)
    result = {
        "schema_revision": PACKAGE_RUN_SCHEMA_REVISION,
        "status": "success",
        "run_id": run_id,
        "revision": revision,
        "source_revision": revision,
        "poetry_lock_sha256": lock_sha256,
        "worker_generation": WORKER_GENERATION,
        "worker_generation_revision": WORKER_GENERATION_REVISION,
        "completed_at": utc_now(),
        "selection": {
            "architectures": list(selection.architectures),
            "distributions": list(selection.distributions),
            "postgresql_majors": list(selection.postgresql_majors),
        },
        "bucket": bucket,
        "s3_prefix": args.s3_prefix,
        "artifact_count": len(uploads),
        "python_environments": {
            name: {
                **environment.manifest_value(),
                "venv_path": environment.venv_path,
            }
            for name, environment in poetry_environments.items()
        },
        "worker_units": {
            name: handle.unit_name for name, handle in handles.items()
        },
        "worker_launch_acknowledged": {
            name: handle.launch_acknowledged for name, handle in handles.items()
        },
        "manifests": build_results,
        "optimization": package_run_optimization_receipt(build_results),
    }
    return result


def fetch_completed_manifest(
    builder: Builder,
    key_path: Path,
    run_id: str,
) -> dict[str, Any]:
    manifest_path = f"{REMOTE_ROOT}/{run_id}-{builder.architecture}-manifest.json"
    result = ssh_run(
        builder,
        key_path,
        f"cat {shlex.quote(manifest_path)}",
        capture=True,
        check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        raise PackageBuilderError(
            f"{builder.architecture} completed manifest is missing for run {run_id}"
        )
    try:
        manifest = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(
            f"{builder.architecture} completed manifest is invalid JSON: {error}"
        ) from error
    worker_manifest = require_worker_package_manifest(
        manifest,
        label=f"{builder.architecture} completed worker package manifest",
    )
    if (
        worker_manifest.get("status") != "success"
        or worker_manifest.get("architecture") != builder.architecture
        or worker_manifest.get("errors") != []
    ):
        raise PackageBuilderError(
            f"{builder.architecture} completed worker manifest is not successful"
        )
    return controller_package_manifest(
        worker_manifest,
        attest_remote_compiler_optimization(
            builder,
            key_path,
            str(worker_manifest["source_revision"]),
            requested_package_cmake_cache_paths(
                worker_manifest.get("requested"),
                label=f"{builder.architecture} completed worker requested matrix",
            ),
        ),
        label=f"{builder.architecture} completed package manifest",
    )


def finalize_completed_run(
    args: argparse.Namespace,
    matrix: Matrix,
    outputs: dict[str, Any],
    builders: dict[str, Builder],
) -> dict[str, Any]:
    """Validate and image one already-completed canonical remote run."""

    run_match = RUN_ID_RE.fullmatch(str(args.run_id))
    if run_match is None:
        raise PackageBuilderError(
            "--run-id must have form YYYYMMDDTHHMMSSZ-<12 lowercase revision hex>"
        )
    args.s3_prefix = normalize_s3_prefix(args.s3_prefix)
    records = validate_builders(args.region, builders, matrix)
    non_running = {
        name: instance_state(records[name])
        for name in matrix.architectures
        if instance_state(records[name]) != "running"
    }
    if non_running:
        raise PackageBuilderError(
            f"finalize-run requires both builders already running: {non_running}"
        )
    builders = refreshed_builders(records, builders)
    key_path = ssh_key_path(outputs)

    manifests: dict[str, dict[str, Any]] = {}
    errors: list[str] = []

    def fetch(name: str) -> dict[str, Any]:
        wait_for_ssh(builders[name], key_path)
        return fetch_completed_manifest(builders[name], key_path, args.run_id)

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(matrix.architectures), thread_name_prefix="bioseq-finalize-fetch"
    ) as executor:
        futures = {
            executor.submit(fetch, name): name for name in matrix.architectures
        }
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                manifests[name] = future.result()
            except Exception as error:
                errors.append(f"{name}: {error}")
    if errors:
        raise PackageBuilderError(
            "could not fetch completed canonical manifests: " + "; ".join(errors)
        )

    revisions = {str(manifest.get("source_revision", "")) for manifest in manifests.values()}
    locks = {str(manifest.get("poetry_lock_sha256", "")) for manifest in manifests.values()}
    if len(revisions) != 1:
        raise PackageBuilderError("completed manifests disagree on source revision")
    revision = revisions.pop()
    if (
        re.fullmatch(r"[0-9a-f]{40}", revision) is None
        or revision[:12] != run_match.group("revision")
    ):
        raise PackageBuilderError(
            "completed manifest revision does not match the requested run ID"
        )
    finalizer_revision = commit_id()
    require_committed_build_input_equivalence(revision, finalizer_revision)
    if len(locks) != 1:
        raise PackageBuilderError("completed manifests disagree on Poetry lock hash")
    lock_sha256 = locks.pop()
    if re.fullmatch(r"[0-9a-f]{64}", lock_sha256) is None:
        raise PackageBuilderError("completed manifest Poetry lock hash is invalid")
    if committed_poetry_lock_sha256(revision) != lock_sha256:
        raise PackageBuilderError(
            "completed manifest Poetry lock does not match its committed revision"
        )

    python_environments: dict[str, dict[str, str]] = {}
    for name, manifest in manifests.items():
        value = manifest.get("python_environment")
        if not isinstance(value, dict):
            raise PackageBuilderError(
                f"{name} completed manifest has no Python environment provenance"
            )
        python_environments[name] = {
            "venv_path": f"{REMOTE_POETRY_VENV_ROOT}/{lock_sha256}",
            "python": str(value.get("python", "")),
            "poetry": str(value.get("poetry", "")),
        }

    selection = Selection(
        tuple(matrix.architectures),
        tuple(matrix.distributions),
        matrix.postgresql_majors,
    )
    expected_artifact_count = (
        len(matrix.architectures)
        * len(matrix.distributions)
        * len(matrix.postgresql_majors)
    )
    result: dict[str, Any] = {
        "schema_revision": PACKAGE_RUN_SCHEMA_REVISION,
        "status": "success",
        "run_id": args.run_id,
        "revision": revision,
        "source_revision": revision,
        "finalizer_revision": finalizer_revision,
        "poetry_lock_sha256": lock_sha256,
        "worker_generation": WORKER_GENERATION,
        "worker_generation_revision": WORKER_GENERATION_REVISION,
        "selection": {
            "architectures": list(selection.architectures),
            "distributions": list(selection.distributions),
            "postgresql_majors": list(selection.postgresql_majors),
        },
        "bucket": outputs.get("package_bucket_name"),
        "s3_prefix": args.s3_prefix,
        "artifact_count": expected_artifact_count,
        "python_environments": python_environments,
        "worker_units": {
            name: f"bioseq-package-{args.run_id}-{name}.service"
            for name in matrix.architectures
        },
        "manifests": manifests,
        "optimization": package_run_optimization_receipt(manifests),
        "finalized_from_completed_manifests": True,
    }
    images = create_post_build_amis(
        args=args,
        matrix=matrix,
        selection=selection,
        outputs=outputs,
        builders=builders,
        build_result=result,
        lock_sha256=lock_sha256,
    )
    result["cache_images"] = {
        name: {
            "image_id": str(image["ImageId"]),
            "source_instance_id": builders[name].instance_id,
        }
        for name, image in images.items()
    }
    result["completed_at"] = utc_now()
    result["builders_left_running"] = {
        name: builder.instance_id for name, builder in builders.items()
    }
    write_last_run(result)
    return result


def controller_context(
    args: argparse.Namespace, matrix: Matrix
) -> tuple[
    dict[str, Any],
    dict[str, Builder],
    dict[str, dict[str, Any]],
    Path,
    Path,
    str,
]:
    """Discover exact existing infrastructure and validate one job's access."""

    if not getattr(args, "existing_only", False):
        raise PackageBuilderError("controller commands require --existing-only")
    if not getattr(args, "ssh_key", None):
        raise PackageBuilderError("controller commands require an ephemeral --ssh-key")
    if not getattr(args, "access_receipt", None):
        raise PackageBuilderError("controller commands require --access-receipt")
    if not getattr(args, "receipt", None):
        raise PackageBuilderError("controller commands require --receipt")
    source_revision = str(getattr(args, "source_revision", ""))
    if re.fullmatch(r"[0-9a-f]{40}", source_revision) is None:
        raise PackageBuilderError(
            "controller commands require a full --source-revision"
        )
    key_path = Path(args.ssh_key).expanduser().resolve()
    access_receipt = Path(args.access_receipt).expanduser().resolve()
    receipt_path = Path(args.receipt).expanduser().resolve()
    outputs, builders, records = discover_existing_infrastructure(
        args.region, matrix, ssh_key=key_path
    )
    identity = {"Account": outputs["account_id"], "Arn": outputs["caller_arn"]}
    read_ci_lifecycle_receipt(
        access_receipt,
        region=args.region,
        identity=identity,
        builders=builders,
        require_open=True,
    )
    ssh_key_path(outputs)
    owner_token_sha256, _description = ci_access_owner(str(args.owner_token))
    return (
        outputs,
        builders,
        records,
        key_path,
        receipt_path,
        owner_token_sha256,
    )


def require_controller_source_equivalence(source_revision: str) -> str:
    """Bind attach/finalize code to the exact source or reviewed recovery deltas."""

    current_revision = commit_id()
    require_committed_build_input_equivalence(source_revision, current_revision)
    lock_sha256 = committed_poetry_lock_sha256(source_revision)
    if re.fullmatch(r"[0-9a-f]{64}", lock_sha256) is None:
        raise PackageBuilderError("controller committed Poetry lock is malformed")
    return lock_sha256


def _require_controller_launch_inputs() -> None:
    require_committed_paths(
        [
            HERE / "package_builder.py",
            HERE / "initialize_package_signing_key.py",
            HERE / "package_matrix.json",
            STACK_DIR / "main.tf",
            STACK_DIR / "variables.tf",
            STACK_DIR / "outputs.tf",
            STACK_DIR / "scripts" / "bootstrap.sh",
            STACK_DIR / "scripts" / "user_data.sh.tftpl",
            PACKAGING_DIR / "pyproject.toml",
            PACKAGING_DIR / "poetry.lock",
            PACKAGING_DIR / "build_packages.py",
            PACKAGING_DIR / "bioseq_packaging",
            PACKAGING_DIR / "rpm",
            PACKAGING_DIR / "deb",
            PACKAGING_DIR / "pacman",
            PACKAGING_DIR / "scripts",
        ]
    )


def prepare_controller_builder(
    builder: Builder,
    key_path: Path,
    source_revision: str,
    lock_sha256: str,
) -> tuple[str, PoetryEnvironment]:
    """Prepare one idle builder without disturbing an existing package run."""

    wait_for_ssh(builder, key_path)
    quiescence = remote_package_quiescence_script(poweroff=False)
    ssh_run(builder, key_path, f"bash -lc {shlex.quote(quiescence)}")
    remote_source = sync_committed_source(builder, key_path, source_revision)
    reconcile_remote_bootstrap(builder, key_path, remote_source)
    force_remote_package_reconfigure(builder, key_path)
    environment = prepare_remote_poetry_environment(
        builder,
        key_path,
        remote_source,
        source_revision,
        lock_sha256,
    )
    return remote_source, environment


def _set_controller_python_environment(
    receipt: dict[str, Any], architecture: str, environment: PoetryEnvironment
) -> None:
    receipt["builders"][architecture]["python_environment"] = {
        **environment.manifest_value(),
        "venv_path": environment.venv_path,
    }


def _read_remote_worker_manifest(
    builder: Builder, key_path: Path, manifest_path: str, *, label: str
) -> dict[str, Any]:
    result = ssh_run(
        builder,
        key_path,
        f"cat {shlex.quote(manifest_path)}",
        capture=True,
        check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        raise PackageBuilderError(f"{label} is missing")
    try:
        raw = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise PackageBuilderError(f"{label} is invalid JSON: {error}") from error
    return require_worker_package_manifest(raw, label=label)


def _adopt_worker_environment(
    receipt: dict[str, Any], architecture: str, manifest: dict[str, Any]
) -> None:
    requested_paths = requested_package_cmake_cache_paths(
        manifest.get("requested"),
        label=f"{architecture} adopted worker requested matrix",
    )
    if requested_paths != selected_package_cmake_cache_paths(
        complete_selection(load_matrix())
    ):
        raise PackageBuilderError(
            f"{architecture} adopted worker does not own the canonical matrix"
        )
    environment = manifest.get("python_environment")
    s3 = manifest.get("s3")
    if (
        manifest.get("architecture") != architecture
        or manifest.get("source_revision") != receipt["source_revision"]
        or manifest.get("poetry_lock_sha256") != receipt["poetry_lock_sha256"]
        or not isinstance(environment, dict)
        or set(environment) != {"python", "poetry"}
        or not isinstance(s3, dict)
        or s3
        != {
            "bucket": receipt["bucket"],
            "region": receipt["region"],
            "base_prefix": receipt["s3_prefix"],
        }
    ):
        raise PackageBuilderError(
            f"{architecture} adopted worker provenance does not match the receipt"
        )
    _set_controller_python_environment(
        receipt,
        architecture,
        PoetryEnvironment(
            lock_sha256=str(receipt["poetry_lock_sha256"]),
            venv_path=f"{REMOTE_POETRY_VENV_ROOT}/{receipt['poetry_lock_sha256']}",
            python_version=str(environment.get("python", "")),
            poetry_version=str(environment.get("poetry", "")),
        ),
    )


def launch_controller_run(args: argparse.Namespace) -> dict[str, Any]:
    """Launch once, or attach to the exact durable receipt without relaunching."""

    matrix = load_matrix()
    selection = complete_selection(matrix)
    if getattr(args, "optimization", None) != PRODUCTION_OPTIMIZATION:
        raise PackageBuilderError("controller launch requires O3")
    if not is_complete_matrix_selection(selection, matrix):
        raise PackageBuilderError("controller launch requires the canonical matrix")
    (
        outputs,
        builders,
        records,
        key_path,
        receipt_path,
        owner_token_sha256,
    ) = controller_context(args, matrix)
    source_revision = str(args.source_revision)
    if commit_id() != source_revision:
        raise PackageBuilderError(
            "controller launch source revision must equal the checked-out HEAD"
        )
    _require_controller_launch_inputs()
    lock_sha256 = committed_poetry_lock_sha256(source_revision)
    existing = fetch_controller_receipt(
        receipt_path,
        matrix=matrix,
        region=args.region,
        bucket=str(outputs["package_bucket_name"]),
        source_revision=source_revision,
        required=False,
    )
    if existing is not None:
        existing = require_controller_receipt(
            existing,
            matrix=matrix,
            label="existing package controller receipt",
            expected_region=args.region,
            expected_account=str(outputs["account_id"]),
            expected_bucket=str(outputs["package_bucket_name"]),
            builders=builders,
            expected_source_revision=source_revision,
            expected_owner_token_sha256=owner_token_sha256,
        )
        if existing["poetry_lock_sha256"] != lock_sha256:
            raise PackageBuilderError("existing controller Poetry lock is stale")
        if existing["state"] == "failed":
            raise PackageBuilderError(
                "existing controller run is failed: "
                + "; ".join(existing["diagnostics"])
            )
        if existing["state"] != "prepared":
            return existing
        receipt = existing
    else:
        run_id = (
            dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            + f"-{source_revision[:12]}"
        )
        receipt = new_controller_receipt(
            matrix=matrix,
            records=records,
            builders=builders,
            outputs=outputs,
            region=args.region,
            owner_token=str(args.owner_token),
            run_id=run_id,
            source_revision=source_revision,
            poetry_lock_sha256=lock_sha256,
        )
        # Lifecycle authority reaches versioned S3 before any instance start.
        persist_controller_receipt(receipt_path, receipt, matrix=matrix)

    records = start_builders(args.region, builders, matrix)
    builders = refreshed_builders(records, builders)
    remote_sources: dict[str, str] = {}
    environments: dict[str, PoetryEnvironment] = {}
    errors: list[str] = []
    pending_launches: list[str] = []

    # A retry can arrive after systemd accepted a unit but before the runner
    # persisted its acknowledgement.  Observe exact units before touching any
    # build cache so an active compile is never reconfigured or relaunched.
    for name in matrix.architectures:
        wait_for_ssh(builders[name], key_path)
        value = receipt["builders"][name]
        status = remote_unit_status(builders[name], key_path, str(value["unit_name"]))
        if status["LoadState"] == "not-found":
            if value["launch_acknowledged"]:
                errors.append(f"{name}: acknowledged unit is missing")
            else:
                pending_launches.append(name)
            continue
        if value["python_environment"] is None:
            errors.append(f"{name}: observed unit lacks persisted Python provenance")
            continue
        value["launch_acknowledged"] = True
        if status["ActiveState"] in {
            "active", "activating", "reloading", "deactivating"
        }:
            handle = controller_handle(receipt, builders[name], outputs)
            if not exact_worker_processes(builders[name], key_path, handle.worker_argv):
                errors.append(f"{name}: active unit has no exact receipt-bound worker")
            if active_package_units(builders[name], key_path) != (handle.unit_name,):
                errors.append(f"{name}: active package unit set is not exact")
        elif not successful_terminal_unit_status(status, allow_not_found=False):
            errors.append(f"{name}: observed unit is terminal failure {status}")
    if errors:
        receipt["state"] = "failed"
        receipt["diagnostics"] = [value[-CONTROLLER_DIAGNOSTIC_LIMIT:] for value in errors]
        persist_controller_receipt(receipt_path, receipt, matrix=matrix)
        raise PackageBuilderError("controller resume failed: " + "; ".join(errors))
    if not pending_launches:
        receipt["state"] = "active"
        return persist_controller_receipt(receipt_path, receipt, matrix=matrix)

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(pending_launches), thread_name_prefix="bioseq-controller-prepare"
    ) as executor:
        futures = {
            executor.submit(
                prepare_controller_builder,
                builders[name],
                key_path,
                source_revision,
                lock_sha256,
            ): name
            for name in pending_launches
        }
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                remote_sources[name], environments[name] = future.result()
                _set_controller_python_environment(receipt, name, environments[name])
            except Exception as error:
                errors.append(f"{name} preparation: {error}")
    if errors:
        receipt["state"] = "failed"
        receipt["diagnostics"] = [value[-CONTROLLER_DIAGNOSTIC_LIMIT:] for value in errors]
        persist_controller_receipt(receipt_path, receipt, matrix=matrix)
        raise PackageBuilderError("controller preparation failed: " + "; ".join(errors))
    persist_controller_receipt(receipt_path, receipt, matrix=matrix)

    signing_host = "x64" if "x64" in pending_launches else pending_launches[0]
    initialize_signing_key(
        builders[signing_host],
        key_path,
        remote_sources[signing_host],
        args.region,
        str(outputs["signing_key_secret_arn"]),
    )

    def launch_or_attach(name: str) -> None:
        builder = builders[name]
        value = receipt["builders"][name]
        status = remote_unit_status(builder, key_path, str(value["unit_name"]))
        if status["LoadState"] != "not-found":
            value["launch_acknowledged"] = True
            return
        if value["launch_acknowledged"]:
            raise PackageBuilderError(
                f"{name} acknowledged unit disappeared before terminal validation"
            )
        handle = launch_remote_build(
            builder=builder,
            key_path=key_path,
            remote_source=remote_sources[name],
            revision=source_revision,
            poetry_environment=environments[name],
            selection=selection,
            region=args.region,
            bucket=str(outputs["package_bucket_name"]),
            s3_prefix=DEFAULT_S3_PREFIX,
            secret_arn=str(outputs["signing_key_secret_arn"]),
            run_id=str(receipt["run_id"]),
        )
        if handle.unit_name != value["unit_name"]:
            raise PackageBuilderError(f"{name} launched an unexpected unit")
        value["launch_acknowledged"] = handle.launch_acknowledged

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(builders), thread_name_prefix="bioseq-controller-launch"
    ) as executor:
        futures = {
            executor.submit(launch_or_attach, name): name
            for name in pending_launches
        }
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                future.result()
            except Exception as error:
                # A lost SSH acknowledgement must not cause a duplicate unit.
                try:
                    status = remote_unit_status(
                        builders[name],
                        key_path,
                        str(receipt["builders"][name]["unit_name"]),
                    )
                    if status["LoadState"] != "not-found":
                        receipt["builders"][name]["launch_acknowledged"] = True
                        continue
                except Exception:
                    pass
                errors.append(f"{name} launch: {error}")
    if errors or any(
        not receipt["builders"][name]["launch_acknowledged"]
        for name in matrix.architectures
    ):
        receipt["state"] = "failed"
        receipt["diagnostics"] = [value[-CONTROLLER_DIAGNOSTIC_LIMIT:] for value in errors]
        persist_controller_receipt(receipt_path, receipt, matrix=matrix)
        raise PackageBuilderError("controller launch failed: " + "; ".join(errors))
    receipt["state"] = "active"
    receipt["diagnostics"] = []
    return persist_controller_receipt(receipt_path, receipt, matrix=matrix)


def adopt_controller_run(args: argparse.Namespace) -> dict[str, Any]:
    """Adopt an exact legacy active run without inferring stop ownership."""

    matrix = load_matrix()
    (
        outputs,
        builders,
        records,
        key_path,
        receipt_path,
        owner_token_sha256,
    ) = controller_context(args, matrix)
    source_revision = str(args.source_revision)
    lock_sha256 = require_controller_source_equivalence(source_revision)
    existing = fetch_controller_receipt(
        receipt_path,
        matrix=matrix,
        region=args.region,
        bucket=str(outputs["package_bucket_name"]),
        source_revision=source_revision,
        required=False,
    )
    if existing is not None:
        return require_controller_receipt(
            existing,
            matrix=matrix,
            label="existing adopted controller receipt",
            expected_account=str(outputs["account_id"]),
            expected_owner_token_sha256=owner_token_sha256,
            builders=builders,
            expected_source_revision=source_revision,
        )
    receipt = new_controller_receipt(
        matrix=matrix,
        records=records,
        builders=builders,
        outputs=outputs,
        region=args.region,
        owner_token=str(args.owner_token),
        run_id=str(args.run_id),
        source_revision=source_revision,
        poetry_lock_sha256=lock_sha256,
        adopt_running=True,
    )
    for name in matrix.architectures:
        wait_for_ssh(builders[name], key_path)
        value = receipt["builders"][name]
        status = remote_unit_status(builders[name], key_path, str(value["unit_name"]))
        if status["LoadState"] == "not-found" or status["ActiveState"] not in {
            "active", "activating", "reloading", "deactivating", "inactive", "failed"
        }:
            raise PackageBuilderError(
                f"cannot adopt {name} unit with status {status}"
            )
        manifest = _read_remote_worker_manifest(
            builders[name],
            key_path,
            str(value["manifest_path"]),
            label=f"{name} adopted worker manifest",
        )
        _adopt_worker_environment(receipt, name, manifest)
        value["launch_acknowledged"] = True
        if status["ActiveState"] == "failed":
            raise PackageBuilderError(f"cannot adopt failed {name} unit: {status}")
        handle = controller_handle(receipt, builders[name], outputs)
        if status["ActiveState"] != "inactive" and not exact_worker_processes(
            builders[name], key_path, handle.worker_argv
        ):
            raise PackageBuilderError(
                f"{name} adopted unit has no exact receipt-bound worker"
            )
        active_units = active_package_units(builders[name], key_path)
        if status["ActiveState"] != "inactive" and active_units != (
            str(value["unit_name"]),
        ):
            raise PackageBuilderError(
                f"{name} adoption found unexpected active package units {active_units}"
            )
    receipt["state"] = "active"
    return persist_controller_receipt(receipt_path, receipt, matrix=matrix)


def _controller_failure_diagnostic(
    builder: Builder,
    key_path: Path,
    handle: RemoteBuildHandle,
    status: dict[str, str],
) -> str:
    result = ssh_run(
        builder,
        key_path,
        f"tail -n 40 {shlex.quote(handle.remote_log_path)}",
        capture=True,
        check=False,
    )
    tail = " ".join((result.stdout or result.stderr or "").split())
    return (
        f"{builder.architecture} unit {handle.unit_name} status={status} "
        f"log_tail={tail[-3000:]!r}"
    )[-CONTROLLER_DIAGNOSTIC_LIMIT:]


def _controller_completed_result(
    *,
    receipt: dict[str, Any],
    matrix: Matrix,
    builders: dict[str, Builder],
    manifests: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    selection = complete_selection(matrix)
    uploads: list[dict[str, Any]] = []
    environments: dict[str, dict[str, str]] = {}
    for name in matrix.architectures:
        environment = controller_poetry_environment(receipt, name)
        environments[name] = {
            **environment.manifest_value(),
            "venv_path": environment.venv_path,
        }
        uploads.extend(
            validate_manifest(
                manifests[name],
                architecture=name,
                selection=selection,
                matrix=matrix,
                bucket=str(receipt["bucket"]),
                region=str(receipt["region"]),
                s3_prefix=str(receipt["s3_prefix"]),
                source_revision=str(receipt["source_revision"]),
                poetry_lock_sha256=str(receipt["poetry_lock_sha256"]),
                python_environment=environment,
            )
        )
    expected_count = (
        len(matrix.architectures)
        * len(matrix.distributions)
        * len(matrix.postgresql_majors)
    )
    if len(uploads) != expected_count:
        raise PackageBuilderError("controller manifests omit canonical package uploads")
    verify_s3_uploads(str(receipt["region"]), str(receipt["bucket"]), uploads)
    result = {
        "schema_revision": PACKAGE_RUN_SCHEMA_REVISION,
        "status": "success",
        "run_id": receipt["run_id"],
        "revision": receipt["source_revision"],
        "source_revision": receipt["source_revision"],
        "poetry_lock_sha256": receipt["poetry_lock_sha256"],
        "worker_generation": WORKER_GENERATION,
        "worker_generation_revision": WORKER_GENERATION_REVISION,
        "completed_at": utc_now(),
        "selection": receipt["selection"],
        "bucket": receipt["bucket"],
        "s3_prefix": receipt["s3_prefix"],
        "artifact_count": expected_count,
        "python_environments": environments,
        "worker_units": {
            name: receipt["builders"][name]["unit_name"]
            for name in matrix.architectures
        },
        "worker_launch_acknowledged": {
            name: receipt["builders"][name]["launch_acknowledged"]
            for name in matrix.architectures
        },
        "manifests": manifests,
        "optimization": package_run_optimization_receipt(manifests),
        "builders_left_running": {
            name: builders[name].instance_id for name in matrix.architectures
        },
    }
    return require_current_package_run_result(
        result, label="controller completed aggregate result"
    )


def attach_controller_run(args: argparse.Namespace) -> dict[str, Any]:
    """Wait one bounded slice and return explicit active/completed/failed state."""

    matrix = load_matrix()
    (
        outputs,
        builders,
        records,
        key_path,
        receipt_path,
        owner_token_sha256,
    ) = controller_context(args, matrix)
    del records
    source_revision = str(args.source_revision)
    lock_sha256 = require_controller_source_equivalence(source_revision)
    receipt = fetch_controller_receipt(
        receipt_path,
        matrix=matrix,
        region=args.region,
        bucket=str(outputs["package_bucket_name"]),
        source_revision=source_revision,
        required=True,
    )
    assert receipt is not None
    receipt = require_controller_receipt(
        receipt,
        matrix=matrix,
        label="attached package controller receipt",
        expected_region=args.region,
        expected_account=str(outputs["account_id"]),
        expected_bucket=str(outputs["package_bucket_name"]),
        builders=builders,
        expected_source_revision=source_revision,
        expected_owner_token_sha256=owner_token_sha256,
    )
    if receipt["poetry_lock_sha256"] != lock_sha256:
        raise PackageBuilderError("attached controller Poetry lock is stale")
    if receipt["state"] == "failed":
        raise PackageBuilderError(
            "controller run is failed: " + "; ".join(receipt["diagnostics"])
        )
    if receipt["state"] in {"finalized", "collected"}:
        write_last_run(receipt["result"])
        return receipt
    wait_seconds = int(args.wait_seconds)
    if wait_seconds < 0 or wait_seconds > 5 * 60 * 60:
        raise PackageBuilderError("--wait-seconds must be between 0 and 18000")
    handles = {
        name: controller_handle(receipt, builders[name], outputs)
        for name in matrix.architectures
    }
    for name in matrix.architectures:
        wait_for_ssh(builders[name], key_path)
    deadline = time.monotonic() + wait_seconds
    statuses: dict[str, dict[str, str]] = {}
    while True:
        statuses = {
            name: remote_unit_status(builders[name], key_path, handles[name].unit_name)
            for name in matrix.architectures
        }
        active = {
            name
            for name, status in statuses.items()
            if status["ActiveState"]
            in {"active", "activating", "reloading", "deactivating"}
            and status["LoadState"] == "loaded"
        }
        if not active or time.monotonic() >= deadline:
            break
        time.sleep(min(15, max(1, int(deadline - time.monotonic()))))
    if active:
        identity_errors: list[str] = []
        for name in sorted(active):
            handle = handles[name]
            if not exact_worker_processes(
                builders[name], key_path, handle.worker_argv
            ):
                identity_errors.append(
                    f"{name}: active unit has no exact receipt-bound worker"
                )
            if active_package_units(builders[name], key_path) != (
                handle.unit_name,
            ):
                identity_errors.append(
                    f"{name}: active package unit set is not receipt-exact"
                )
        if identity_errors:
            receipt["state"] = "failed"
            receipt["diagnostics"] = identity_errors
            persist_controller_receipt(receipt_path, receipt, matrix=matrix)
            raise PackageBuilderError(
                "controller active identity failed: "
                + "; ".join(identity_errors)
            )
        receipt["state"] = "active"
        receipt["diagnostics"] = []
        return persist_controller_receipt(receipt_path, receipt, matrix=matrix)

    failures: list[str] = []
    for name, status in statuses.items():
        allow_not_found = handles[name].launch_acknowledged
        if successful_terminal_unit_status(status, allow_not_found=allow_not_found):
            continue
        failures.append(
            _controller_failure_diagnostic(
                builders[name], key_path, handles[name], status
            )
        )
    if failures:
        receipt["state"] = "failed"
        receipt["diagnostics"] = failures
        persist_controller_receipt(receipt_path, receipt, matrix=matrix)
        raise PackageBuilderError("controller workers failed: " + "; ".join(failures))

    manifests: dict[str, dict[str, Any]] = {}
    errors: list[str] = []

    def fetch(name: str) -> dict[str, Any]:
        handle = handles[name]
        require_exact_worker_absent(handle.builder, key_path, handle.worker_argv)
        return fetch_completed_manifest(handle.builder, key_path, str(receipt["run_id"]))

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(builders), thread_name_prefix="bioseq-controller-fetch"
    ) as executor:
        futures = {
            executor.submit(fetch, name): name for name in matrix.architectures
        }
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                manifests[name] = future.result()
            except Exception as error:
                errors.append(f"{name}: {error}")
    if errors:
        receipt["state"] = "failed"
        receipt["diagnostics"] = [value[-CONTROLLER_DIAGNOSTIC_LIMIT:] for value in errors]
        persist_controller_receipt(receipt_path, receipt, matrix=matrix)
        raise PackageBuilderError(
            "controller manifest finalization failed: " + "; ".join(errors)
        )
    receipt["result"] = _controller_completed_result(
        receipt=receipt,
        matrix=matrix,
        builders=builders,
        manifests=manifests,
    )
    receipt["state"] = "completed"
    receipt["diagnostics"] = []
    receipt = persist_controller_receipt(receipt_path, receipt, matrix=matrix)
    write_last_run(receipt["result"])
    return receipt


def finalize_controller_run(args: argparse.Namespace) -> dict[str, Any]:
    """Finalize one completed receipt with exact existing infrastructure."""

    receipt = attach_controller_run(
        argparse.Namespace(**{**vars(args), "wait_seconds": 0})
    )
    if receipt["state"] == "active":
        raise PackageBuilderError("controller run is still active")
    if receipt["state"] in {"finalized", "collected"}:
        write_last_run(receipt["result"])
        return receipt
    if receipt["state"] != "completed":
        raise PackageBuilderError(
            f"controller run cannot finalize from state {receipt['state']!r}"
        )
    matrix = load_matrix()
    outputs, builders, _records = discover_existing_infrastructure(
        args.region,
        matrix,
        ssh_key=Path(args.ssh_key).expanduser().resolve(),
    )
    result = dict(receipt["result"])
    images = create_post_build_amis(
        args=argparse.Namespace(region=args.region, s3_prefix=DEFAULT_S3_PREFIX),
        matrix=matrix,
        selection=complete_selection(matrix),
        outputs=outputs,
        builders=builders,
        build_result=result,
        lock_sha256=str(receipt["poetry_lock_sha256"]),
    )
    result["cache_images"] = {
        name: {
            "image_id": str(image["ImageId"]),
            "source_instance_id": builders[name].instance_id,
        }
        for name, image in images.items()
    }
    result["complete_matrix_fingerprint"] = receipt["matrix_fingerprint"]
    receipt["result"] = require_current_package_run_result(
        result, label="finalized controller aggregate result"
    )
    receipt["state"] = "finalized"
    receipt = persist_controller_receipt(
        Path(args.receipt).expanduser().resolve(), receipt, matrix=matrix
    )
    write_last_run(receipt["result"])
    return receipt


def mark_controller_collected(
    *,
    receipt_path: Path,
    run_result_path: Path,
    expected_revision: str,
    region: str,
) -> dict[str, Any]:
    """Persist proof that exact release assets were collected successfully."""

    try:
        raw_result = json.loads(run_result_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PackageBuilderError(
            f"cannot re-read collected package result: {error}"
        ) from error
    result = require_current_package_run_result(
        raw_result, label="collected controller run result"
    )
    if (
        result.get("source_revision") != expected_revision
        or result.get("revision") != expected_revision
    ):
        raise PackageBuilderError("collected controller source revision mismatch")
    matrix = load_matrix()
    receipt = fetch_controller_receipt(
        receipt_path,
        matrix=matrix,
        region=region,
        bucket=str(result["bucket"]),
        source_revision=expected_revision,
        required=True,
    )
    assert receipt is not None
    identity = aws_preflight(region)
    receipt = require_controller_receipt(
        receipt,
        matrix=matrix,
        label="collected package controller receipt",
        expected_region=region,
        expected_account=str(identity["Account"]),
        expected_bucket=str(result["bucket"]),
        expected_source_revision=expected_revision,
    )
    if receipt["state"] not in {"finalized", "collected"}:
        raise PackageBuilderError(
            f"release collection cannot mark controller state {receipt['state']!r}"
        )
    if receipt["result"] != result:
        raise PackageBuilderError(
            "collected run result differs from the durable controller result"
        )
    receipt["state"] = "collected"
    return persist_controller_receipt(receipt_path, receipt, matrix=matrix)


def controller_cleanup_script(
    receipt: dict[str, Any], builder: Builder, outputs: dict[str, Any]
) -> str:
    """Return one lifecycle-locked, receipt-exact remote cleanup command."""

    value = receipt["builders"][builder.architecture]
    if value["python_environment"] is None:
        worker_argv: tuple[str, ...] = ()
    else:
        worker_argv = controller_handle(receipt, builder, outputs).worker_argv
    command = command_text(
        [
            "python3",
            "-c",
            REMOTE_CONTROLLER_CLEANUP_SCRIPT,
            REMOTE_WORKSPACE,
            str(value["unit_name"]),
            str(value["build_root"]),
            json.dumps(list(worker_argv), separators=(",", ":")),
            str(value["remote_log_path"]),
            str(value["manifest_path"]),
        ]
    )
    return "\n".join(
        [
            "set -Eeuo pipefail",
            f"exec 9>{shlex.quote(REMOTE_PACKAGE_LIFECYCLE_LOCK)}",
            "flock --exclusive --wait "
            f"{REMOTE_PACKAGE_LIFECYCLE_LOCK_WAIT_SECONDS} 9",
            command,
        ]
    ) + "\n"


def require_controller_cleanup_result(
    value: object,
    *,
    receipt: dict[str, Any],
    architecture: str,
) -> dict[str, Any]:
    """Validate the closed evidence emitted by one remote cleanup script."""

    if not isinstance(value, dict):
        raise PackageBuilderError(f"{architecture} cleanup result must be an object")
    require_exact_keys(
        value,
        {
            "unit_name",
            "build_root",
            "unit_before",
            "unit_after",
            "unit_cancelled",
            "worker_roots",
            "worker_processes",
            "containers",
            "log",
            "manifest",
            "build_root_preserved",
        },
        f"{architecture} cleanup result",
    )
    expected = receipt["builders"][architecture]
    if (
        value["unit_name"] != expected["unit_name"]
        or value["build_root"] != expected["build_root"]
        or type(value["unit_cancelled"]) is not bool
        or type(value["build_root_preserved"]) is not bool
    ):
        raise PackageBuilderError(f"{architecture} cleanup identity is invalid")
    for label in ("unit_before", "unit_after"):
        status = value[label]
        if not isinstance(status, dict):
            raise PackageBuilderError(f"{architecture} cleanup {label} is malformed")
        require_exact_keys(
            status,
            {"LoadState", "ActiveState", "SubState", "Result", "ExecMainStatus"},
            f"{architecture} cleanup {label}",
        )
        if any(not isinstance(item, str) for item in status.values()):
            raise PackageBuilderError(f"{architecture} cleanup {label} is malformed")
    after = value["unit_after"]
    if after["LoadState"] == "loaded" and after["ActiveState"] not in {
        "inactive",
        "failed",
    }:
        raise PackageBuilderError(f"{architecture} cleanup left its unit active")
    for label in ("worker_roots", "worker_processes"):
        pids = value[label]
        if (
            not isinstance(pids, list)
            or any(type(pid) is not int or pid <= 0 for pid in pids)
            or len(set(pids)) != len(pids)
        ):
            raise PackageBuilderError(f"{architecture} cleanup {label} is malformed")
    containers = value["containers"]
    if (
        not isinstance(containers, list)
        or any(re.fullmatch(r"[0-9a-f]{12,64}", str(item)) is None for item in containers)
        or len(set(containers)) != len(containers)
    ):
        raise PackageBuilderError(f"{architecture} cleanup containers are malformed")
    for label in ("log", "manifest"):
        artifact = value[label]
        if not isinstance(artifact, dict):
            raise PackageBuilderError(f"{architecture} cleanup {label} is malformed")
        require_exact_keys(
            artifact,
            {"present", "size", "sha256"},
            f"{architecture} cleanup {label}",
        )
        if (
            type(artifact["present"]) is not bool
            or type(artifact["size"]) is not int
            or artifact["size"] < 0
            or (
                artifact["sha256"] is not None
                and re.fullmatch(r"[0-9a-f]{64}", str(artifact["sha256"])) is None
            )
        ):
            raise PackageBuilderError(f"{architecture} cleanup {label} is malformed")
    return value


def controller_cleanup_result_from_rows(
    rows: list[dict[str, Any]],
    *,
    instance_id: str,
    receipt: dict[str, Any],
    architecture: str,
) -> dict[str, Any]:
    """Extract the one JSON evidence row returned for an exact SSM target."""

    if len(rows) != 1 or rows[0].get("InstanceId") != instance_id:
        raise PackageBuilderError(
            f"{architecture} cleanup SSM result omitted its exact instance"
        )
    candidates: list[object] = []
    plugins = rows[0].get("CommandPlugins", [])
    if not isinstance(plugins, list):
        raise PackageBuilderError(f"{architecture} cleanup SSM plugins are malformed")
    for plugin in plugins:
        if not isinstance(plugin, dict):
            raise PackageBuilderError(f"{architecture} cleanup SSM plugin is malformed")
        for line in str(plugin.get("Output", "")).splitlines():
            try:
                candidate = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(candidate, dict) and candidate.get("unit_name") == receipt[
                "builders"
            ][architecture]["unit_name"]:
                candidates.append(candidate)
    if len(candidates) != 1:
        raise PackageBuilderError(
            f"{architecture} cleanup did not return one exact evidence object"
        )
    return require_controller_cleanup_result(
        candidates[0], receipt=receipt, architecture=architecture
    )


def cleanup_controller_run(args: argparse.Namespace) -> dict[str, Any]:
    """Cancel only receipt-owned work and stop only receipt-owned instances."""

    source_revision = str(args.source_revision)
    if re.fullmatch(r"[0-9a-f]{40}", source_revision) is None:
        raise PackageBuilderError("cleanup-run requires a full source revision")
    matrix = load_matrix()
    outputs, builders, records = discover_existing_infrastructure(args.region, matrix)
    receipt_path = Path(args.receipt).expanduser().resolve()
    evidence_path = Path(args.evidence).expanduser().resolve()
    owner_token_sha256, _description = ci_access_owner(str(args.owner_token))
    receipt = fetch_controller_receipt(
        receipt_path,
        matrix=matrix,
        region=args.region,
        bucket=str(outputs["package_bucket_name"]),
        source_revision=source_revision,
        required=not bool(args.if_present),
    )
    evidence: dict[str, Any] = {
        "schema_revision": CLEANUP_EVIDENCE_SCHEMA_REVISION,
        "created_at": utc_now(),
        "region": args.region,
        "source_revision": source_revision,
        "receipt_present": receipt is not None,
        "receipt_state_before": None,
        "receipt_state_after": None,
        "builders": {},
        "errors": [],
    }
    if receipt is None:
        write_json_atomic(evidence_path, evidence)
        return evidence
    receipt = require_controller_receipt(
        receipt,
        matrix=matrix,
        label="workflow-finalizer controller receipt",
        expected_region=args.region,
        expected_account=str(outputs["account_id"]),
        expected_bucket=str(outputs["package_bucket_name"]),
        builders=builders,
        expected_source_revision=source_revision,
        expected_owner_token_sha256=owner_token_sha256,
    )
    state_before = str(receipt["state"])
    evidence["receipt_state_before"] = state_before
    errors: list[str] = []
    current_states = {
        name: instance_state(records[name]) for name in matrix.architectures
    }

    def make_running(name: str) -> None:
        state = current_states[name]
        if state == "pending":
            wait_for_ec2_state(args.region, builders[name].instance_id, "running")
            current_states[name] = "running"
            state = "running"
        if state == "running":
            wait_for_ssm_online(
                args.region,
                builders[name].instance_id,
                timeout_seconds=CONTROLLER_CLEANUP_TIMEOUT_SECONDS,
            )
        elif state not in {"stopped", "stopping"}:
            raise PackageBuilderError(
                f"{name} cleanup cannot proceed from instance state {state!r}"
            )

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=len(builders), thread_name_prefix="bioseq-controller-cleanup-ready"
    ) as executor:
        futures = {
            executor.submit(make_running, name): name for name in matrix.architectures
        }
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                future.result()
            except Exception as error:
                errors.append(f"{name} readiness: {error}")

    def clean_builder(name: str) -> tuple[str, list[dict[str, Any]]]:
        command_id, rows = run_ssm_shell_command_detailed(
            args.region,
            [builders[name].instance_id],
            script=controller_cleanup_script(receipt, builders[name], outputs),
            comment=f"Bioseq receipt-owned package cleanup ({name})",
            timeout_seconds=CONTROLLER_CLEANUP_TIMEOUT_SECONDS,
        )
        result = controller_cleanup_result_from_rows(
            rows,
            instance_id=builders[name].instance_id,
            receipt=receipt,
            architecture=name,
        )
        return command_id, [result]

    runnable = [
        name for name in matrix.architectures if current_states[name] == "running"
    ]
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=max(1, len(runnable)),
        thread_name_prefix="bioseq-controller-cleanup",
    ) as executor:
        futures = {executor.submit(clean_builder, name): name for name in runnable}
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                command_id, results = future.result()
                evidence["builders"][name] = {
                    "instance_id": builders[name].instance_id,
                    "instance_state_before": current_states[name],
                    "stop_owned": receipt["builders"][name]["stop_owned"],
                    "cleanup_command_id": command_id,
                    "remote": results[0],
                }
            except Exception as error:
                errors.append(f"{name} remote cleanup: {error}")
    for name in matrix.architectures:
        if name not in evidence["builders"]:
            evidence["builders"][name] = {
                "instance_id": builders[name].instance_id,
                "instance_state_before": current_states[name],
                "stop_owned": receipt["builders"][name]["stop_owned"],
                "cleanup_command_id": None,
                "remote": None,
            }

    # Cost cleanup is intentionally narrower than the public `stop` command:
    # finalization may run before collection, but only an instance recorded as
    # stopped/stopping before launch is lifecycle-owned by this workflow.
    def stop_owned_builder(name: str) -> tuple[str, str | None]:
        if current_states[name] == "running" and not any(
            error.startswith(f"{name} ") for error in errors
        ):
            not_after_epoch = int(time.time()) + (
                CONTROLLER_CLEANUP_TIMEOUT_SECONDS - SSM_CANCEL_RESERVE_SECONDS
            )
            command_id = run_ssm_shell_command(
                args.region,
                [builders[name].instance_id],
                script=remote_package_quiescence_script(
                    poweroff=True, not_after_epoch=not_after_epoch
                ),
                comment=f"Bioseq controller-owned package builder stop ({name})",
                timeout_seconds=CONTROLLER_CLEANUP_TIMEOUT_SECONDS,
            )
            wait_for_ec2_state(
                args.region,
                builders[name].instance_id,
                "stopped",
                timeout_seconds=CONTROLLER_CLEANUP_TIMEOUT_SECONDS,
            )
            return "stopped", command_id
        elif current_states[name] == "stopping":
            wait_for_ec2_state(
                args.region,
                builders[name].instance_id,
                "stopped",
                timeout_seconds=CONTROLLER_CLEANUP_TIMEOUT_SECONDS,
            )
            return "stopped", None
        return current_states[name], None

    owned = [
        name
        for name in matrix.architectures
        if receipt["builders"][name]["stop_owned"]
    ]
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=max(1, len(owned)),
        thread_name_prefix="bioseq-controller-owned-stop",
    ) as executor:
        futures = {executor.submit(stop_owned_builder, name): name for name in owned}
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                final_state, command_id = future.result()
                current_states[name] = final_state
                if command_id is not None:
                    evidence["builders"][name]["poweroff_command_id"] = command_id
            except Exception as error:
                errors.append(f"{name} owned instance stop: {error}")

    for name in matrix.architectures:
        evidence["builders"][name]["instance_state_after"] = current_states[name]
        if (
            receipt["builders"][name]["stop_owned"]
            and current_states[name] != "stopped"
        ):
            errors.append(
                f"{name} controller-owned instance remained {current_states[name]}"
            )

    if state_before in {"prepared", "active"}:
        receipt["state"] = "failed"
        diagnostic = (
            "workflow finalizer cleanup incomplete: " + "; ".join(errors)
            if errors
            else "workflow finalizer cancelled receipt-owned package workers"
        )
        receipt["diagnostics"] = [diagnostic[-CONTROLLER_DIAGNOSTIC_LIMIT:]]
        try:
            receipt = persist_controller_receipt(
                receipt_path, receipt, matrix=matrix
            )
        except Exception as error:
            errors.append(f"controller receipt finalization: {error}")
    evidence["receipt_state_after"] = receipt["state"]
    evidence["errors"] = [value[-CONTROLLER_DIAGNOSTIC_LIMIT:] for value in errors]
    write_json_atomic(evidence_path, evidence)
    if errors:
        raise PackageBuilderError(
            "workflow-finalizer package cleanup failed: " + "; ".join(errors)
        )
    return evidence


def cmd_validate(args: argparse.Namespace) -> None:
    matrix = load_matrix()
    selection = select_matrix(args, matrix)
    tofu_executable()
    require_executable("aws")
    require_executable("ssh")
    require_executable("git")
    aws_preflight(args.region)
    run_command([tofu_executable(), "fmt", "-check", "-recursive"], cwd=STACK_DIR)
    tofu_init()
    run_command([tofu_executable(), "validate"], cwd=STACK_DIR)
    print(json.dumps({"status": "valid", "selection": selection.__dict__}, indent=2))


def cmd_matrix(args: argparse.Namespace) -> None:
    matrix = load_matrix()
    selection = Selection(
        tuple(matrix.architectures),
        tuple(matrix.distributions),
        matrix.postgresql_majors,
    )
    if args.require_complete and not is_complete_matrix_selection(selection, matrix):
        raise PackageBuilderError("repository package matrix is not complete")
    cells = [
        {
            "architecture": architecture,
            "aws_architecture": matrix.architectures[architecture].aws_architecture,
            "artifact_architecture": (
                matrix.architectures[architecture].artifact_architecture
            ),
            "instance_type": matrix.architectures[architecture].instance_type,
            "postgresql_major": postgres,
            "distro": distro,
            "package_format": matrix.distributions[distro].package_format,
        }
        for architecture in matrix.architectures
        for postgres in matrix.postgresql_majors
        for distro in matrix.distributions
    ]
    print(
        json.dumps(
            {
                "schema_revision": PACKAGE_MATRIX_SCHEMA_REVISION,
                "package_version": EXPECTED_PACKAGE_VERSION,
                "cell_count": len(cells),
                "cells": cells,
            },
            indent=2,
            sort_keys=True,
        )
    )


def cmd_inventory(args: argparse.Namespace) -> None:
    matrix = load_matrix()
    outputs, builders, records = discover_existing_infrastructure(
        args.region, matrix
    )
    security_group_id, _group = validated_builder_security_group(
        args.region, records
    )
    print(
        json.dumps(
            {
                "account": outputs["account_id"],
                "region": args.region,
                "artifact_bucket": outputs["package_bucket_name"],
                "signing_secret_arn": outputs["signing_key_secret_arn"],
                "security_group_id": security_group_id,
                "builders": {
                    name: {
                        "instance_id": builder.instance_id,
                        "source_ami_id": builder.source_ami_id,
                        "instance_type": builder.instance_type,
                        "architecture": builder.aws_architecture,
                        "availability_zone": builder.availability_zone,
                        "state": instance_state(records[name]),
                    }
                    for name, builder in builders.items()
                },
            },
            indent=2,
            sort_keys=True,
        )
    )


def cmd_open_ci_access(args: argparse.Namespace) -> None:
    payload = open_ci_access(
        region=args.region,
        ssh_key=Path(args.ssh_key),
        ssh_cidr=args.ssh_cidr,
        owner_token=args.owner_token,
        receipt=Path(args.receipt).expanduser().resolve(),
    )
    print(json.dumps(payload, indent=2, sort_keys=True))


def cmd_close_ci_access(args: argparse.Namespace) -> None:
    payload = close_ci_access(
        region=args.region, receipt=Path(args.receipt).expanduser().resolve()
    )
    print(json.dumps(payload, indent=2, sort_keys=True))


def cmd_close_ci_access_owner(args: argparse.Namespace) -> None:
    payload = close_ci_access_owner(
        region=args.region, owner_token=str(args.owner_token)
    )
    print(json.dumps(payload, indent=2, sort_keys=True))


def cmd_cleanup_run(args: argparse.Namespace) -> None:
    payload = cleanup_controller_run(args)
    print(json.dumps(payload, indent=2, sort_keys=True))


def cmd_stop(args: argparse.Namespace) -> None:
    lifecycle_receipt = (
        Path(args.lifecycle_receipt).expanduser().resolve()
        if args.lifecycle_receipt
        else None
    )
    controller_receipt = (
        Path(args.controller_receipt).expanduser().resolve()
        if args.controller_receipt
        else None
    )
    owner_token_sha256 = None
    if args.owner_token:
        owner_token_sha256, _description = ci_access_owner(args.owner_token)
    states = stop_existing_builders(
        args.region,
        load_matrix(),
        lifecycle_receipt=lifecycle_receipt,
        controller_receipt=controller_receipt,
        controller_source_revision=args.source_revision,
        controller_owner_token_sha256=owner_token_sha256,
    )
    print(json.dumps({"region": args.region, "builders": states}, indent=2, sort_keys=True))


def cmd_collect(args: argparse.Namespace) -> None:
    run_result_path = Path(args.run_result).expanduser().resolve()
    collection = collect_release_assets(
        region=args.region,
        run_result_path=run_result_path,
        output_dir=Path(args.output_dir).expanduser().resolve(),
        expected_revision=args.expected_revision,
    )
    if args.controller_receipt:
        mark_controller_collected(
            receipt_path=Path(args.controller_receipt).expanduser().resolve(),
            run_result_path=run_result_path,
            expected_revision=args.expected_revision,
            region=args.region,
        )
    print(json.dumps(collection, indent=2, sort_keys=True))


def cmd_verify_release_assets(args: argparse.Namespace) -> None:
    result = verify_release_asset_directory(
        asset_dir=Path(args.asset_dir).expanduser().resolve(),
        expected_revision=args.expected_revision,
    )
    print(json.dumps(result, indent=2, sort_keys=True))


def cmd_output(args: argparse.Namespace) -> None:
    del args
    print(json.dumps(tofu_outputs(), indent=2, sort_keys=True))


def cmd_status(args: argparse.Namespace) -> None:
    matrix = load_matrix()
    outputs = tofu_outputs()
    if not outputs:
        print(json.dumps({"stack": "not-applied"}, indent=2))
        return
    builders = output_builders(outputs, matrix)
    records = validate_builders(args.region, builders, matrix)
    last_run = None
    if LAST_RUN_FILE.exists():
        try:
            last_run = json.loads(LAST_RUN_FILE.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise PackageBuilderError(
                f"cannot read package run result {LAST_RUN_FILE}: {error}"
            ) from error
        last_run = require_current_package_run_result(
            last_run, label="stored package run result")
    payload = {
        "region": args.region,
        "bucket": outputs.get("package_bucket_name"),
        "builders": {
            name: {
                "instance_id": builder.instance_id,
                "instance_type": builder.instance_type,
                "architecture": builder.aws_architecture,
                "state": instance_state(records[name]),
                "public_ip": records[name].get("PublicIpAddress"),
            }
            for name, builder in builders.items()
        },
        "last_run": last_run,
    }
    print(json.dumps(payload, indent=2, sort_keys=True))


def cmd_up(args: argparse.Namespace) -> None:
    matrix = load_matrix()
    outputs, builders, _, _ = provision_and_start(args, matrix)
    payload = {
        "region": args.region,
        "bucket": outputs.get("package_bucket_name"),
        "builders": {name: builder.__dict__ for name, builder in builders.items()},
    }
    print(json.dumps(payload, indent=2, sort_keys=True))
    info("builders remain running under the persistent-build-cache lifecycle")


def cmd_run(args: argparse.Namespace) -> None:
    if getattr(args, "optimization", PRODUCTION_OPTIMIZATION) != PRODUCTION_OPTIMIZATION:
        raise PackageBuilderError(
            f"production package runs require {PRODUCTION_OPTIMIZATION}"
        )
    matrix = load_matrix()
    selection = select_matrix(args, matrix)
    if not args.no_snapshot and not is_complete_matrix_selection(selection, matrix):
        raise PackageBuilderError(
            "partial matrix runs must pass --no-snapshot; only the canonical matrix may image"
        )
    outputs, builders, _, _ = provision_and_start(args, matrix)
    result = execute_build(args, matrix, selection, outputs, builders)
    if not args.no_snapshot:
        images = create_post_build_amis(
            args=args,
            matrix=matrix,
            selection=selection,
            outputs=outputs,
            builders=builders,
            build_result=result,
            lock_sha256=str(result["poetry_lock_sha256"]),
        )
        result["cache_images"] = {
            name: {
                "image_id": str(image["ImageId"]),
                "source_instance_id": builders[name].instance_id,
            }
            for name, image in images.items()
        }
    result["completed_at"] = utc_now()
    result["builders_left_running"] = {
        name: builder.instance_id for name, builder in builders.items()
    }
    write_last_run(result)
    info("package builders remain running; no stop or shutdown action was issued")
    print(json.dumps(result, indent=2, sort_keys=True))


def cmd_launch_run(args: argparse.Namespace) -> None:
    receipt = launch_controller_run(args)
    print(json.dumps(receipt, indent=2, sort_keys=True))


def cmd_attach_run(args: argparse.Namespace) -> None:
    receipt = attach_controller_run(args)
    print(json.dumps(receipt, indent=2, sort_keys=True))


def cmd_adopt_run(args: argparse.Namespace) -> None:
    receipt = adopt_controller_run(args)
    print(json.dumps(receipt, indent=2, sort_keys=True))


def cmd_finalize_run(args: argparse.Namespace) -> None:
    if getattr(args, "existing_only", False):
        receipt = finalize_controller_run(args)
        info("durable controller run finalized; builders remain running")
        print(json.dumps(receipt, indent=2, sort_keys=True))
        return
    if not args.run_id:
        raise PackageBuilderError(
            "legacy finalize-run requires --run-id; durable finalization requires "
            "--existing-only and a controller receipt"
        )
    matrix = load_matrix()
    outputs = tofu_outputs()
    if not outputs:
        raise PackageBuilderError("finalize-run requires an applied OpenTofu stack")
    if outputs.get("region") != args.region:
        raise PackageBuilderError(
            f"OpenTofu region output {outputs.get('region')!r} does not match "
            f"{args.region!r}"
        )
    builders = output_builders(outputs, matrix)
    result = finalize_completed_run(args, matrix, outputs, builders)
    info("completed run finalized; both package builders remain running")
    print(json.dumps(result, indent=2, sort_keys=True))


def add_common_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--region", default=DEFAULT_REGION)
    parser.add_argument(
        "--ssh-cidr",
        help="explicit operator IPv4 /32 (default: detect it; never fails open)",
    )
    parser.add_argument(
        "--bucket-name",
        help="optional globally unique artifact bucket name (OpenTofu derives one by default)",
    )
    parser.add_argument(
        "--s3-prefix",
        default=DEFAULT_S3_PREFIX,
        help="artifact key prefix; the builder IAM policy is scoped to this exact prefix",
    )
    parser.add_argument(
        "--from-scratch",
        action="store_true",
        help="ignore matching package-builder AMI caches for this apply",
    )
    parser.add_argument(
        "--no-snapshot",
        action="store_true",
        help="skip post-build AMIs (required for any partial matrix run)",
    )
    parser.add_argument(
        "--existing-only",
        action="store_true",
        help="rediscover and start only the exact tagged existing stack; never apply OpenTofu",
    )
    parser.add_argument(
        "--ssh-key",
        help="ephemeral SSH private key used with EC2 Instance Connect in existing-only mode",
    )
    parser.add_argument(
        "--lifecycle-receipt",
        help=(
            "open-ci-access receipt used to bind the AWS caller and record exact "
            "builder stop ownership in existing-only mode"
        ),
    )


def add_controller_options(
    parser: argparse.ArgumentParser, *, required: bool = True
) -> None:
    """Add the receipt-bound options shared by hosted controller jobs."""

    parser.add_argument("--region", default=DEFAULT_REGION)
    parser.add_argument("--existing-only", action="store_true")
    parser.add_argument("--ssh-key", required=required)
    parser.add_argument("--access-receipt", required=required)
    parser.add_argument("--receipt", required=required)
    parser.add_argument("--source-revision", required=required)
    parser.add_argument(
        "--owner-token",
        required=required,
        help="stable non-secret release identity shared by every continuation job",
    )


def add_matrix_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--architectures",
        action="append",
        help="comma-separated native architecture keys (default: arm64,x64)",
    )
    parser.add_argument(
        "--distros",
        action="append",
        help="comma-separated native package distro IDs (default: complete current set)",
    )
    parser.add_argument(
        "--postgres",
        action="append",
        help="comma-separated PostgreSQL majors (default: matrix selection, currently 17)",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    matrix_parser = subparsers.add_parser(
        "matrix", help="print the exact repository-declared package matrix without AWS access"
    )
    matrix_parser.add_argument("--require-complete", action="store_true")
    matrix_parser.set_defaults(handler=cmd_matrix)

    inventory_parser = subparsers.add_parser(
        "inventory",
        help="read and validate current-account package infrastructure without mutation",
    )
    inventory_parser.add_argument("--region", default=DEFAULT_REGION)
    inventory_parser.set_defaults(handler=cmd_inventory)

    run_parser = subparsers.add_parser(
        "run", help="provision/start, build, upload and optionally image; leave running"
    )
    add_common_options(run_parser)
    add_matrix_options(run_parser)
    run_parser.add_argument(
        "--optimization",
        choices=(PRODUCTION_OPTIMIZATION,),
        required=True,
        help="required production optimization policy (only O3 is accepted)",
    )
    run_parser.set_defaults(handler=cmd_run)

    launch_parser = subparsers.add_parser(
        "launch-run",
        help="durably record lifecycle ownership and launch one exact canonical run",
    )
    add_controller_options(launch_parser)
    launch_parser.add_argument(
        "--optimization",
        choices=(PRODUCTION_OPTIMIZATION,),
        required=True,
    )
    launch_parser.set_defaults(handler=cmd_launch_run)

    attach_parser = subparsers.add_parser(
        "attach-run",
        help="attach to a durable run and wait one bounded non-launching slice",
    )
    add_controller_options(attach_parser)
    attach_parser.add_argument("--wait-seconds", type=int, default=0)
    attach_parser.set_defaults(handler=cmd_attach_run)

    adopt_parser = subparsers.add_parser(
        "adopt-run",
        help="adopt an exact already-running legacy unit pair without stop ownership",
    )
    add_controller_options(adopt_parser)
    adopt_parser.add_argument("--run-id", required=True)
    adopt_parser.set_defaults(handler=cmd_adopt_run)

    finalize_parser = subparsers.add_parser(
        "finalize-run",
        help="validate and image an already-completed canonical run; leave running",
    )
    finalize_parser.add_argument("--run-id")
    add_controller_options(finalize_parser, required=False)
    finalize_parser.add_argument("--s3-prefix", default=DEFAULT_S3_PREFIX)
    finalize_parser.set_defaults(handler=cmd_finalize_run)

    up_parser = subparsers.add_parser(
        "up", help="provision/start both builders for explicit debugging"
    )
    add_common_options(up_parser)
    up_parser.set_defaults(handler=cmd_up)

    status_parser = subparsers.add_parser(
        "status", help="show validated builder state and the last successful run"
    )
    status_parser.add_argument("--region", default=DEFAULT_REGION)
    status_parser.set_defaults(handler=cmd_status)

    output_parser = subparsers.add_parser("output", help="print raw OpenTofu outputs")
    output_parser.set_defaults(handler=cmd_output)

    validate_parser = subparsers.add_parser(
        "validate", help="validate matrix, AWS identity, and the OpenTofu stack"
    )
    validate_parser.add_argument("--region", default=DEFAULT_REGION)
    add_matrix_options(validate_parser)
    validate_parser.set_defaults(handler=cmd_validate)

    open_access_parser = subparsers.add_parser(
        "open-ci-access",
        help="authorize one ephemeral /32 for the exact tagged existing builders",
    )
    open_access_parser.add_argument("--region", default=DEFAULT_REGION)
    open_access_parser.add_argument("--ssh-key", required=True)
    open_access_parser.add_argument("--ssh-cidr")
    open_access_parser.add_argument(
        "--owner-token",
        required=True,
        help="stable non-secret workflow-run identity used to own a retryable /32",
    )
    open_access_parser.add_argument("--receipt", required=True)
    open_access_parser.set_defaults(handler=cmd_open_ci_access)

    close_access_parser = subparsers.add_parser(
        "close-ci-access", help="revoke only the ingress recorded by open-ci-access"
    )
    close_access_parser.add_argument("--region", default=DEFAULT_REGION)
    close_access_parser.add_argument("--receipt", required=True)
    close_access_parser.set_defaults(handler=cmd_close_ci_access)

    close_access_owner_parser = subparsers.add_parser(
        "close-ci-access-owner",
        help="revoke only ephemeral ingress matching one exact workflow owner token",
    )
    close_access_owner_parser.add_argument("--region", default=DEFAULT_REGION)
    close_access_owner_parser.add_argument("--owner-token", required=True)
    close_access_owner_parser.set_defaults(handler=cmd_close_ci_access_owner)

    cleanup_parser = subparsers.add_parser(
        "cleanup-run",
        help="cancel only receipt-owned package work and preserve cleanup evidence",
    )
    cleanup_parser.add_argument("--region", default=DEFAULT_REGION)
    cleanup_parser.add_argument("--receipt", required=True)
    cleanup_parser.add_argument("--source-revision", required=True)
    cleanup_parser.add_argument("--owner-token", required=True)
    cleanup_parser.add_argument("--evidence", required=True)
    cleanup_parser.add_argument("--if-present", action="store_true")
    cleanup_parser.set_defaults(handler=cmd_cleanup_run)

    stop_parser = subparsers.add_parser(
        "stop", help="stop the exact tagged builders without deleting reusable resources"
    )
    stop_parser.add_argument("--region", default=DEFAULT_REGION)
    stop_parser.add_argument(
        "--lifecycle-receipt",
        help="limit cleanup to builders whose start ownership is recorded in this receipt",
    )
    stop_parser.add_argument(
        "--controller-receipt",
        help="durable collected controller receipt used for lifecycle ownership",
    )
    stop_parser.add_argument("--source-revision")
    stop_parser.add_argument("--owner-token")
    stop_parser.set_defaults(handler=cmd_stop)

    collect_parser = subparsers.add_parser(
        "collect", help="download all exact-version packages and write release receipts"
    )
    collect_parser.add_argument("--region", default=DEFAULT_REGION)
    collect_parser.add_argument("--run-result", required=True)
    collect_parser.add_argument("--output-dir", required=True)
    collect_parser.add_argument("--expected-revision", required=True)
    collect_parser.add_argument(
        "--controller-receipt",
        help="durable receipt to mark collected after exact asset verification",
    )
    collect_parser.set_defaults(handler=cmd_collect)

    verify_release_parser = subparsers.add_parser(
        "verify-release-assets",
        help="verify a downloaded release asset set without AWS access",
    )
    verify_release_parser.add_argument("--asset-dir", required=True)
    verify_release_parser.add_argument("--expected-revision", required=True)
    verify_release_parser.set_defaults(handler=cmd_verify_release_assets)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    previous_sigterm = signal.getsignal(signal.SIGTERM)
    previous_sigint = signal.getsignal(signal.SIGINT)

    def interrupt_for_cleanup(signum: int, _frame: Any) -> None:
        interrupt_active_children()
        raise KeyboardInterrupt(f"received signal {signum}")

    signal.signal(signal.SIGTERM, interrupt_for_cleanup)
    signal.signal(signal.SIGINT, interrupt_for_cleanup)
    try:
        with exclusive_lock():
            args.handler(args)
    except KeyboardInterrupt:
        print("package-builder interrupted", file=sys.stderr)
        return 130
    except PackageBuilderError as error:
        print(f"package-builder failed: {error}", file=sys.stderr)
        return 1
    finally:
        signal.signal(signal.SIGTERM, previous_sigterm)
        signal.signal(signal.SIGINT, previous_sigint)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
