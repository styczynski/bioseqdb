"""Cache-mode runtime validation and report contracts."""

from __future__ import annotations

from typing import List, Dict, Set, Optional, Any, Mapping, Sequence

import argparse
import base64
import contextlib
import ctypes
import errno
import fcntl
import hashlib
import json
import math
import os
import platform as host_platform
import re
import select
import secrets
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass, field
from pathlib import Path

from build_presets import (
    _CMAKE_CONFIGURE_STAMP_KIND,
    _CMAKE_CONFIGURE_STAMP_NAME,
    _cmake_cache_value,
)
from integrity import (
    canonical_json_sha256,
    sha256_bytes,
    sha256_file,
)
from publication import (
    copy_descriptor_exact,
    durable_replace_bytes,
    publish_private_exclusive,
)





_CACHE_NATIVE_ARTIFACT_KEYS = frozenset({
    "cache_test_support_postgres_bin_dir",
    "cache_test_support_postgres_stage_root",
    "cache_test_support_postgres_stage_manifest",
    "cache_production_postgres_bin_dir",
    "cache_production_postgres_stage_root",
    "cache_production_postgres_stage_manifest",
    "cache_bioseq_support_postgres_bin_dir",
    "cache_bioseq_support_postgres_stage_root",
    "cache_bioseq_support_postgres_stage_manifest",
    "cache_bioseq_only_postgres_bin_dir",
    "cache_bioseq_only_postgres_stage_root",
    "cache_bioseq_only_postgres_stage_manifest",
    "cache_bioseq_production_postgres_bin_dir",
    "cache_bioseq_production_postgres_stage_root",
    "cache_bioseq_production_postgres_stage_manifest",
    "production_dso_qualification_receipt",
    "cache_control",
    "cache_install_sql",
    "cache_dso",
    "cache_test_control",
    "cache_test_install_sql",
    "cache_test_dso",
    "bioseq_control",
    "bioseq_install_sql",
    "bioseq_dso",
    "provider_peer_test_dso",
})

def _cache_native_artifacts(active_bin_dir: Path) -> dict[str, str]:
    """Load CMake-generated input paths for native cache runners."""
    manifest = active_bin_dir / "cache_native_artifacts.json"
    try:
        payload = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            f"cannot read native cache artifact manifest {manifest}: {exc}"
        ) from exc
    if not isinstance(payload, dict):
        raise RuntimeError(
            f"native cache artifact manifest is not an object: {manifest}")
    unknown = set(payload) - _CACHE_NATIVE_ARTIFACT_KEYS
    missing = _CACHE_NATIVE_ARTIFACT_KEYS - set(payload)
    if unknown or missing:
        raise RuntimeError(
            "native cache artifact manifest keys differ from the MCP "
            f"contract: missing={sorted(missing)} unknown={sorted(unknown)}")
    values: dict[str, str] = {}
    for key, value in payload.items():
        if not isinstance(value, str):
            raise RuntimeError(
                f"native cache artifact {key!r} is not a string")
        if value and not Path(value).is_absolute():
            raise RuntimeError(
                f"native cache artifact {key!r} is not absolute: {value}")
        values[key] = value
    return values

_CACHE_BIOSEQ_DSO_QUALIFICATION_MODES = frozenset({
    "cache-bioseq-integration",
    "cache-bioseq-benchmark",
})

_CACHE_NATIVE_REPORT_MODES = frozenset({
    "cache-unit",
    "cache-integration",
    "cache-benchmark",
    "cache-bioseq-integration",
    "cache-bioseq-benchmark",
})

_CACHE_INTEGRATION_REPORT_KEYS = frozenset({
    "schema_revision",
    "executable",
    "build_identity",
    "arguments",
    "cluster_configuration",
    "cases",
    "stage_timings",
    "host_qualifications",
    "sql_transcripts",
    "artifacts",
    "cleanup_observations",
    "resources",
    "cleanup",
})

_CACHE_BENCHMARK_REPORT_KEYS = frozenset({
    "schema_revision",
    "metadata",
    "scenario",
    "measurement_cohort",
    "exact_2gbp_host_qualification",
    "fixture",
    "correctness_digest",
    "cold_producer",
    "samples",
    "summary",
    "cache_counter_deltas",
    "logical_bytes",
    "stored_bytes",
    "peak_rss_bytes",
    "peak_temp_bytes",
    "cleanup",
})


def _write_json_atomic(path: Path, value: Mapping[str, object]) -> None:
    payload = json.dumps(
        value, indent=2, sort_keys=True, separators=(",", ": ")
    ).encode("utf-8") + b"\n"
    durable_replace_bytes(path, payload)

_CACHE_BIOSEQ_DSO_QUALIFICATION_RECEIPT_KEY = (
    "production_dso_qualification_receipt"
)

def _qualification_exact_keys(
        value: Mapping[str, object], expected: set[str], context: str) -> None:
    actual = set(value)
    if actual != expected:
        raise RuntimeError(
            f"{context} keys differ from the contract: "
            f"missing={sorted(expected - actual)} "
            f"unknown={sorted(actual - expected)}")

def _qualification_digest(value: object, context: str) -> str:
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise RuntimeError(f"{context} is not a lowercase SHA-256 digest")
    return value

def _qualification_architecture(value: object, context: str) -> str:
    if not isinstance(value, str):
        raise RuntimeError(f"{context} is not a string")
    normalized = value.strip().lower().replace("-", "_")
    aliases = {
        "aarch64": "arm64",
        "arm64": "arm64",
        "arm64e": "arm64",
        "amd64": "x86_64",
        "x86_64": "x86_64",
    }
    if normalized not in aliases:
        raise RuntimeError(f"{context} is unsupported: {value!r}")
    return aliases[normalized]

def _qualification_file_identity(
        value: object,
        context: str,
        *,
        validate_live: bool,
        executable: bool,
) -> Path:
    if not isinstance(value, dict):
        raise RuntimeError(f"{context} identity is not an object")
    _qualification_exact_keys(
        value,
        {"lexical_path", "resolved_path", "size_bytes", "sha256"},
        f"{context} identity",
    )
    lexical_raw = value["lexical_path"]
    resolved_raw = value["resolved_path"]
    size = value["size_bytes"]
    digest = _qualification_digest(value["sha256"], f"{context} SHA-256")
    if (
        not isinstance(lexical_raw, str)
        or not Path(lexical_raw).is_absolute()
        or not isinstance(resolved_raw, str)
        or not Path(resolved_raw).is_absolute()
        or not isinstance(size, int)
        or isinstance(size, bool)
        or size < 0
    ):
        raise RuntimeError(f"{context} identity fields are invalid")
    resolved = Path(resolved_raw)
    if not validate_live:
        return resolved
    lexical = Path(lexical_raw)
    try:
        lexical_metadata = lexical.lstat()
        current = lexical.resolve(strict=True)
        current_metadata = current.stat()
    except OSError as exc:
        raise RuntimeError(f"cannot inspect current {context}: {exc}") from exc
    if (
        not (stat.S_ISREG(lexical_metadata.st_mode)
             or stat.S_ISLNK(lexical_metadata.st_mode))
        or not stat.S_ISREG(current_metadata.st_mode)
        or current != resolved
        or current_metadata.st_size != size
        or sha256_file(current) != digest
        or (executable and not os.access(current, os.X_OK))
    ):
        raise RuntimeError(f"current {context} differs from its sealed identity")
    return current

def _run_qualification_inspection(
        tool: Path, arguments: list[str], context: str) -> str:
    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "LANG": "C"})
    try:
        completed = subprocess.run(
            [str(tool), *arguments],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
            timeout=30,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"production DSO {context} exceeded 30 seconds") from exc
    except OSError as exc:
        raise RuntimeError(
            f"cannot rerun production DSO {context}: {exc}") from exc
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()
        raise RuntimeError(
            f"production DSO {context} failed: "
            f"{detail or completed.returncode}")
    output = completed.stdout
    if not output.strip() and completed.stderr.strip():
        output = completed.stderr
    lines = output.replace("\r\n", "\n").replace("\r", "\n").splitlines()
    return "\n".join(line.rstrip() for line in lines).strip() + "\n"

def _live_dso_inspection(
        tool: Path, path: Path, system_name: str) -> dict[str, object]:
    commands = (
        (("-hv",), ("-L",), ("-l",))
        if system_name == "Darwin"
        else (("-h",), ("-d",), ("-n",))
    )
    header = _run_qualification_inspection(
        tool, [*commands[0], str(path)], f"header inspection for {path}")
    dependency = _run_qualification_inspection(
        tool, [*commands[1], str(path)], f"dependency inspection for {path}")
    identity = _run_qualification_inspection(
        tool, [*commands[2], str(path)], f"build-identity inspection for {path}")
    if system_name == "Darwin":
        architectures = sorted({
            _qualification_architecture(match.group(1), "Mach-O architecture")
            for match in re.finditer(
                r"^\s*MH_[A-Z0-9_]+\s+([A-Za-z0-9_-]+)\s+",
                header,
                re.MULTILINE,
            )
        })
        dependencies = sorted(
            match.group(1)
            for line in dependency.splitlines()[1:]
            if (match := re.match(
                r"^\s+(.+?)\s+\(compatibility version ", line))
        )
        identities = sorted({
            match.group(1).lower()
            for match in re.finditer(
                r"^\s*uuid\s+([0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-"
                r"[0-9A-Fa-f]{12})\b",
                identity,
                re.MULTILINE,
            )
        })
        binary: dict[str, object] = {
            "format": "Mach-O",
            "architectures": architectures,
        }
        identity_kind = "mach-o-lc-uuid"
    else:
        fields: dict[str, str] = {}
        for field, key in (("Class", "class"), ("Data", "data"),
                           ("Machine", "machine")):
            match = re.search(
                rf"^\s*{field}:\s*(.+?)\s*$", header, re.MULTILINE)
            if match is None:
                raise RuntimeError(
                    f"live ELF header for {path} omits {field}")
            fields[key] = match.group(1)
        if re.search(
                r"(?:X86-64|Advanced Micro Devices X86-64)",
                fields["machine"]):
            architecture = "x86_64"
        elif fields["machine"] == "AArch64":
            architecture = "arm64"
        else:
            raise RuntimeError(
                f"live ELF machine for {path} is unsupported: "
                f"{fields['machine']}")
        architectures = [architecture]
        dependencies = sorted(
            re.findall(r"\(NEEDED\).*?\[([^]\r\n]+)\]", dependency))
        identities = sorted({
            match.group(1).lower()
            for match in re.finditer(
                r"^\s*Build ID:\s*([0-9A-Fa-f]+)\s*$",
                identity,
                re.MULTILINE,
            )
        })
        binary = {
            "format": "ELF",
            "architectures": architectures,
            **fields,
            "architecture": architecture,
        }
        identity_kind = "elf-gnu-build-id"
    if not architectures or not dependencies or not identities:
        raise RuntimeError(
            f"live production DSO inspection is incomplete for {path}")
    if len(dependencies) != len(set(dependencies)):
        raise RuntimeError(
            f"live production DSO dependencies are duplicated for {path}")
    return {
        "binary": binary,
        "direct_load_dependencies": dependencies,
        "build_identity": {"kind": identity_kind, "values": identities},
        "header_listing_sha256": sha256_bytes(header.encode("utf-8")),
        "dependency_listing_sha256":
            sha256_bytes(dependency.encode("utf-8")),
        "build_identity_listing_sha256":
            sha256_bytes(identity.encode("utf-8")),
    }

def _load_production_dso_qualification_receipt(
        receipt_path: Path,
        *,
        artifacts: Mapping[str, str] | None = None,
        expected_build_dir: Path | None = None,
        expected_preset: str | None = None,
        validate_live: bool,
) -> tuple[Path, dict[str, object]]:
    if not receipt_path.is_absolute():
        raise RuntimeError(
            f"production DSO qualification receipt is not absolute: {receipt_path}")
    try:
        metadata = receipt_path.lstat()
        resolved_receipt = receipt_path.resolve(strict=True)
        document = json.loads(receipt_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            f"cannot read production DSO qualification receipt "
            f"{receipt_path}: {exc}") from exc
    if receipt_path.is_symlink() or not stat.S_ISREG(metadata.st_mode):
        raise RuntimeError(
            "production DSO qualification receipt is not a regular "
            f"non-symlink file: {receipt_path}")
    if not isinstance(document, dict):
        raise RuntimeError(
            "production DSO qualification receipt is not an object")
    _qualification_exact_keys(
        document,
        {
            "schema_revision", "artifact_kind", "platform",
            "inspection_tool", "build_context", "dsos", "content_sha256",
        },
        "production DSO qualification receipt",
    )
    if (
        type(document["schema_revision"]) is not int
        or document["schema_revision"] != 1
        or document["artifact_kind"]
            != "bioseq-production-dso-load-qualification"
    ):
        raise RuntimeError("production DSO qualification receipt schema is invalid")
    try:
        _validate_json_content_seal(
            document, "production DSO qualification receipt")
    except ValueError as exc:
        raise RuntimeError(str(exc)) from exc

    platform_value = document["platform"]
    if not isinstance(platform_value, dict):
        raise RuntimeError("production DSO platform is not an object")
    _qualification_exact_keys(
        platform_value,
        {
            "system_name", "system_release", "machine", "binary_format",
            "dso_suffix",
        },
        "production DSO platform",
    )
    system_name = platform_value["system_name"]
    if not isinstance(system_name, str) or system_name not in {"Darwin", "Linux"}:
        raise RuntimeError("production DSO platform system is unsupported")
    machine = _qualification_architecture(
        platform_value["machine"], "production DSO platform architecture")
    binary_format = "Mach-O" if system_name == "Darwin" else "ELF"
    suffix = ".dylib" if system_name == "Darwin" else ".so"
    if (
        not isinstance(platform_value["system_release"], str)
        or not platform_value["system_release"]
        or platform_value["machine"] != machine
        or platform_value["binary_format"] != binary_format
        or platform_value["dso_suffix"] != suffix
    ):
        raise RuntimeError("production DSO platform fields are inconsistent")
    if validate_live and (
        host_platform.system() != system_name
        or host_platform.release() != platform_value["system_release"]
        or _qualification_architecture(
            host_platform.machine(), "current host architecture") != machine
    ):
        raise RuntimeError("production DSO qualification platform is not current")

    tool_value = document["inspection_tool"]
    if not isinstance(tool_value, dict):
        raise RuntimeError("production DSO inspection tool is not an object")
    _qualification_exact_keys(
        tool_value,
        {
            "lexical_path", "resolved_path", "size_bytes", "sha256",
            "identity_kind", "header_arguments", "dependency_arguments",
            "build_identity_arguments",
        },
        "production DSO inspection tool",
    )
    if tool_value["identity_kind"] != "executable-sha256":
        raise RuntimeError("production DSO inspection tool identity kind is invalid")
    expected_tool_name = "otool" if system_name == "Darwin" else "readelf"
    if Path(str(tool_value["lexical_path"])).name != expected_tool_name:
        raise RuntimeError(
            "production DSO inspection tool is not platform exact")
    expected_arguments = (
        (["-hv"], ["-L"], ["-l"])
        if system_name == "Darwin" else (["-h"], ["-d"], ["-n"])
    )
    if (
        tool_value["header_arguments"],
        tool_value["dependency_arguments"],
        tool_value["build_identity_arguments"],
    ) != expected_arguments:
        raise RuntimeError("production DSO inspection tool arguments are invalid")
    inspection_tool = _qualification_file_identity(
        {
            key: tool_value[key]
            for key in ("lexical_path", "resolved_path", "size_bytes", "sha256")
        },
        "production DSO inspection tool",
        validate_live=validate_live,
        executable=True,
    )

    context = document["build_context"]
    if not isinstance(context, dict):
        raise RuntimeError("production DSO build context is not an object")
    _qualification_exact_keys(
        context,
        {
            "preset", "configuration", "build_type", "generator",
            "build_directory", "cmake_system", "cmake_cache",
            "configure_input", "compilers",
        },
        "production DSO build context",
    )
    for field in ("preset", "configuration", "build_type", "generator"):
        if not isinstance(context[field], str) or not context[field]:
            raise RuntimeError(f"production DSO build context {field} is invalid")
    if context["configuration"] != context["build_type"]:
        raise RuntimeError("production DSO configuration and build type differ")
    build_directory_raw = context["build_directory"]
    if not isinstance(build_directory_raw, str) or not Path(
            build_directory_raw).is_absolute():
        raise RuntimeError("production DSO build directory is invalid")
    build_directory = Path(build_directory_raw)
    if expected_build_dir is not None and (
        build_directory != expected_build_dir.resolve(strict=validate_live)
    ):
        raise RuntimeError("production DSO receipt names the wrong build directory")
    if expected_preset is not None and context["preset"] != expected_preset:
        raise RuntimeError("production DSO receipt names the wrong CMake preset")
    cmake_system = context["cmake_system"]
    if not isinstance(cmake_system, dict):
        raise RuntimeError("production DSO CMake system is not an object")
    _qualification_exact_keys(
        cmake_system, {"name", "processor"}, "production DSO CMake system")
    if (
        cmake_system["name"] != system_name
        or _qualification_architecture(
            cmake_system["processor"], "production DSO CMake processor") != machine
    ):
        raise RuntimeError("production DSO CMake system differs from the binary")

    def context_file(
            raw: object, label: str, expected_name: str) -> tuple[Path, str]:
        if not isinstance(raw, dict):
            raise RuntimeError(f"production DSO {label} is not an object")
        _qualification_exact_keys(
            raw, {"path", "size_bytes", "sha256"},
            f"production DSO {label}")
        path_raw = raw["path"]
        size = raw["size_bytes"]
        digest = _qualification_digest(
            raw["sha256"], f"production DSO {label} SHA-256")
        if (
            not isinstance(path_raw, str)
            or not Path(path_raw).is_absolute()
            or Path(path_raw).name != expected_name
            or not isinstance(size, int)
            or isinstance(size, bool)
            or size < 0
        ):
            raise RuntimeError(f"production DSO {label} fields are invalid")
        path = Path(path_raw)
        if path.parent != build_directory:
            raise RuntimeError(f"production DSO {label} escapes the build directory")
        if validate_live:
            try:
                file_metadata = path.lstat()
            except OSError as exc:
                raise RuntimeError(
                    f"cannot inspect current production DSO {label}: {exc}") from exc
            if (
                path.is_symlink()
                or not stat.S_ISREG(file_metadata.st_mode)
                or file_metadata.st_size != size
                or sha256_file(path) != digest
            ):
                raise RuntimeError(
                    f"current production DSO {label} differs from its seal")
        return path, digest

    cmake_cache_path, _ = context_file(
        context["cmake_cache"], "CMake cache", "CMakeCache.txt")
    configure_input = context["configure_input"]
    if not isinstance(configure_input, dict):
        raise RuntimeError("production DSO configure input is not an object")
    _qualification_exact_keys(
        configure_input,
        {"path", "size_bytes", "sha256", "inputs_sha256"},
        "production DSO configure input",
    )
    configure_inputs_sha256 = _qualification_digest(
        configure_input["inputs_sha256"],
        "production DSO configure-input seal",
    )
    configure_path, _ = context_file(
        {
            key: configure_input[key]
            for key in ("path", "size_bytes", "sha256")
        },
        "configure input",
        _CMAKE_CONFIGURE_STAMP_NAME,
    )

    compilers = context["compilers"]
    if not isinstance(compilers, dict):
        raise RuntimeError("production DSO compilers are not an object")
    _qualification_exact_keys(
        compilers, {"c", "cxx"}, "production DSO compilers")
    for language in ("c", "cxx"):
        compiler = compilers[language]
        if not isinstance(compiler, dict):
            raise RuntimeError(f"production DSO {language} compiler is invalid")
        _qualification_exact_keys(
            compiler,
            {
                "lexical_path", "resolved_path", "size_bytes", "sha256",
                "compiler_id", "compiler_version",
            },
            f"production DSO {language} compiler",
        )
        if (
            not isinstance(compiler["compiler_id"], str)
            or not compiler["compiler_id"]
            or not isinstance(compiler["compiler_version"], str)
            or not compiler["compiler_version"]
        ):
            raise RuntimeError(
                f"production DSO {language} compiler metadata is invalid")
        _qualification_file_identity(
            {
                key: compiler[key]
                for key in ("lexical_path", "resolved_path", "size_bytes", "sha256")
            },
            f"production DSO {language} compiler",
            validate_live=validate_live,
            executable=True,
        )

    if validate_live:
        try:
            configure_stamp = json.loads(configure_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise RuntimeError(
                f"cannot parse current CMake configure stamp: {exc}") from exc
        if (
            not isinstance(configure_stamp, dict)
            or configure_stamp.get("artifact_kind") != _CMAKE_CONFIGURE_STAMP_KIND
            or set(configure_stamp) != {
                "artifact_kind", "schema_revision", "inputs", "inputs_sha256",
            }
            or type(configure_stamp.get("schema_revision")) is not int
            or configure_stamp.get("schema_revision") != 1
            or not isinstance(configure_stamp.get("inputs"), dict)
            or configure_stamp.get("inputs_sha256") != configure_inputs_sha256
            or canonical_json_sha256(configure_stamp["inputs"])
                != configure_inputs_sha256
            or configure_stamp["inputs"].get("preset") != context["preset"]
        ):
            raise RuntimeError("current CMake configure-input seal is invalid")
        if (
            _cmake_cache_value(build_directory, "CMAKE_BUILD_TYPE")
                != context["build_type"]
            or _cmake_cache_value(build_directory, "CMAKE_GENERATOR")
                != context["generator"]
            or _cmake_cache_value(build_directory, "CMAKE_C_COMPILER")
                != compilers["c"]["lexical_path"]
            or _cmake_cache_value(build_directory, "CMAKE_CXX_COMPILER")
                != compilers["cxx"]["lexical_path"]
        ):
            raise RuntimeError("current CMake cache context differs from the receipt")
        if cmake_cache_path != build_directory / "CMakeCache.txt":
            raise RuntimeError("production DSO CMake cache path is not canonical")

    dso_records = document["dsos"]
    if not isinstance(dso_records, list) or len(dso_records) != 2:
        raise RuntimeError("production DSO qualification must contain two DSOs")
    expected_dsos: dict[str, Path] | None = None
    if artifacts is not None:
        try:
            expected_dsos = {
                "bioseq": Path(artifacts["bioseq_dso"]).resolve(
                    strict=validate_live),
                "bioseq_cache": Path(artifacts["cache_dso"]).resolve(
                    strict=validate_live),
            }
        except (KeyError, OSError) as exc:
            raise RuntimeError("canonical production DSO paths are unavailable") from exc
    observed: dict[str, Path] = {}
    for record in dso_records:
        if not isinstance(record, dict):
            raise RuntimeError("production DSO qualification row is not an object")
        _qualification_exact_keys(
            record,
            {
                "extension", "path", "basename", "size_bytes", "sha256",
                "binary", "header_listing_sha256", "build_identity",
                "direct_load_dependencies", "dependency_listing_sha256",
                "build_identity_listing_sha256", "sibling_extension",
                "sibling_basename", "sibling_dependency_absent",
            },
            "production DSO qualification row",
        )
        extension = record["extension"]
        if (
            not isinstance(extension, str)
            or extension not in {"bioseq", "bioseq_cache"}
            or extension in observed
        ):
            raise RuntimeError("production DSO qualification roster is invalid")
        path_raw = record["path"]
        expected_basename = f"{extension}{suffix}"
        size = record["size_bytes"]
        digest = _qualification_digest(
            record["sha256"], f"production {extension} DSO SHA-256")
        if (
            not isinstance(path_raw, str)
            or not Path(path_raw).is_absolute()
            or record["basename"] != expected_basename
            or Path(path_raw).name != expected_basename
            or not isinstance(size, int)
            or isinstance(size, bool)
            or size < 0
        ):
            raise RuntimeError(f"production {extension} DSO identity is invalid")
        path = Path(path_raw)
        binary = record["binary"]
        if not isinstance(binary, dict):
            raise RuntimeError(f"production {extension} binary metadata is invalid")
        expected_binary_keys = (
            {"format", "architectures"}
            if system_name == "Darwin"
            else {"format", "architectures", "class", "data", "machine", "architecture"}
        )
        _qualification_exact_keys(
            binary, expected_binary_keys, f"production {extension} binary metadata")
        if (
            binary["format"] != binary_format
            or binary["architectures"] != [machine]
            or (system_name == "Linux" and binary["architecture"] != machine)
        ):
            raise RuntimeError(
                f"production {extension} binary architecture is invalid")
        if system_name == "Linux" and any(
            not isinstance(binary[field], str) or not binary[field]
            for field in ("class", "data", "machine")
        ):
            raise RuntimeError(f"production {extension} ELF header is incomplete")
        for field in (
            "header_listing_sha256", "dependency_listing_sha256",
            "build_identity_listing_sha256",
        ):
            _qualification_digest(
                record[field], f"production {extension} {field}")
        build_identity = record["build_identity"]
        if not isinstance(build_identity, dict):
            raise RuntimeError(f"production {extension} build identity is invalid")
        _qualification_exact_keys(
            build_identity, {"kind", "values"},
            f"production {extension} build identity")
        identities = build_identity["values"]
        identity_pattern = (
            r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}"
            if system_name == "Darwin" else r"(?:[0-9a-f]{2})+"
        )
        expected_identity_kind = (
            "mach-o-lc-uuid" if system_name == "Darwin" else "elf-gnu-build-id")
        if (
            build_identity["kind"] != expected_identity_kind
            or not isinstance(identities, list)
            or not identities
            or any(not isinstance(identity, str)
                   or re.fullmatch(identity_pattern, identity) is None
                   for identity in identities)
            or identities != sorted(set(identities))
        ):
            raise RuntimeError(f"production {extension} build identity is invalid")
        dependencies = record["direct_load_dependencies"]
        sibling = "bioseq_cache" if extension == "bioseq" else "bioseq"
        sibling_basename = f"{sibling}{suffix}"
        sibling_stem = sibling_basename.removesuffix(suffix)
        if (
            not isinstance(dependencies, list)
            or not dependencies
            or any(not isinstance(dependency, str) or not dependency
                   for dependency in dependencies)
            or dependencies != sorted(set(dependencies))
            or record["sibling_extension"] != sibling
            or record["sibling_basename"] != sibling_basename
            or record["sibling_dependency_absent"] is not True
            or any(
                Path(dependency).name == sibling_basename
                or Path(dependency).name.startswith(sibling_basename + ".")
                or (
                    suffix == ".dylib"
                    and Path(dependency).name.startswith(sibling_stem + ".")
                    and Path(dependency).name.endswith(suffix)
                )
                for dependency in dependencies
            )
        ):
            raise RuntimeError(
                f"production {extension} DSO sibling-dependency proof is invalid")
        if expected_dsos is not None and path.resolve(
                strict=validate_live) != expected_dsos[extension]:
            raise RuntimeError(
                f"production {extension} DSO path is not the canonical target")
        if validate_live:
            try:
                dso_metadata = path.lstat()
            except OSError as exc:
                raise RuntimeError(
                    f"cannot inspect current production {extension} DSO: {exc}") from exc
            if (
                path.is_symlink()
                or not stat.S_ISREG(dso_metadata.st_mode)
                or dso_metadata.st_size != size
                or sha256_file(path) != digest
            ):
                raise RuntimeError(
                    f"current production {extension} DSO differs from its receipt")
            live = _live_dso_inspection(
                inspection_tool, path, system_name)
            for field in (
                "binary",
                "direct_load_dependencies",
                "build_identity",
                "header_listing_sha256",
                "dependency_listing_sha256",
                "build_identity_listing_sha256",
            ):
                if live[field] != record[field]:
                    raise RuntimeError(
                        f"current production {extension} DSO {field} differs "
                        "from its qualification receipt")
        observed[str(extension)] = path.resolve(strict=validate_live)
    if set(observed) != {"bioseq", "bioseq_cache"}:
        raise RuntimeError("production DSO qualification roster is incomplete")
    return resolved_receipt, document

def _cache_bioseq_dso_qualification_evidence(
        active_bin_dir: Path,
        *,
        expected_build_dir: Path | None = None,
        expected_preset: str | None = None,
        snapshot_for_report: Path | None = None,
) -> dict[str, str]:
    """Validate and bind the CMake-owned production DSO receipt."""
    artifacts = _cache_native_artifacts(active_bin_dir)
    raw = artifacts.get(_CACHE_BIOSEQ_DSO_QUALIFICATION_RECEIPT_KEY, "")
    if not raw:
        raise RuntimeError("combined cache qualification has no production DSO receipt")
    path, document = _load_production_dso_qualification_receipt(
        Path(raw),
        artifacts=artifacts,
        expected_build_dir=expected_build_dir,
        expected_preset=expected_preset,
        validate_live=True,
    )
    evidence_path = path
    if snapshot_for_report is not None:
        ordinal = _report_index(snapshot_for_report.name)
        if ordinal < 0:
            raise RuntimeError(
                "cache native report has no stable ordinal for its production "
                f"DSO qualification snapshot: {snapshot_for_report}")
        evidence_path = snapshot_for_report.resolve(strict=True).with_name(
            f"production_dso_qualification_{ordinal}.json")
        if evidence_path.exists() or evidence_path.is_symlink():
            try:
                existing = _load_json_object(
                    evidence_path,
                    "immutable production DSO qualification snapshot",
                )
            except RuntimeError as exc:
                raise RuntimeError(
                    "cannot reuse immutable production DSO qualification "
                    f"snapshot {evidence_path}: {exc}") from exc
            if existing != document:
                raise RuntimeError(
                    "immutable production DSO qualification snapshot already "
                    f"exists with different content: {evidence_path}")
        else:
            _write_json_atomic(evidence_path, document)
        _, snapshot_document = _load_production_dso_qualification_receipt(
            evidence_path, validate_live=False)
        if snapshot_document != document:
            raise RuntimeError(
                "immutable production DSO qualification snapshot differs "
                "from the live CMake receipt")
    return {
        "production_dso_qualification_receipt": str(evidence_path),
        "production_dso_qualification_receipt_sha256":
            sha256_file(evidence_path),
        "production_dso_qualification_content_sha256":
            str(document["content_sha256"]),
    }

_CACHE_EVIDENCE_ROLE_BY_EXECUTABLE = {
    "bioseq_cache_integration_tests": "cache-integration",
    "bioseq_cache_bioseq_support_integration_tests":
        "cache-bioseq-support-integration",
    "bioseq_cache_bioseq_large_input_e2e_tests":
        "cache-bioseq-large-input-e2e",
    "bioseq_cache_benchmark": "cache-benchmark",
    "bioseq_cache_bioseq_benchmark": "cache-bioseq-benchmark",
}

_CACHE_EVIDENCE_EXECUTABLE_BY_ROLE = {
    role: executable
    for executable, role in _CACHE_EVIDENCE_ROLE_BY_EXECUTABLE.items()
}

_CACHE_EVIDENCE_SCENARIO_BY_ROLE = {
    "cache-integration": "cache-integration",
    "cache-bioseq-support-integration": "cache-bioseq-support",
    "cache-bioseq-large-input-e2e": "cache-bioseq-large-input-e2e",
    "cache-benchmark": "standalone",
    "cache-bioseq-benchmark": "bioseq-2gbp",
}

_CACHE_INTEGRATION_EXACT_CASES_BY_EXECUTABLE = {
    "bioseq_cache_integration_tests": 50,
    "bioseq_cache_bioseq_support_integration_tests": 8,
    "bioseq_cache_bioseq_large_input_e2e_tests": 1,
}

_CACHE_INTEGRATION_RUNNER_OPTIONS = frozenset({
    "--postgres-bin-dir",
    "--postgres-stage-root",
    "--postgres-stage-manifest",
    "--bioseq-only-postgres-bin-dir",
    "--bioseq-only-postgres-stage-root",
    "--bioseq-only-postgres-stage-manifest",
    "--cache-control",
    "--cache-install-sql",
    "--cache-dso",
    "--bioseq-control",
    "--bioseq-install-sql",
    "--bioseq-dso",
    "--provider-peer-test-dso",
    "--report",
    "--case-timeout-seconds",
    "--reporter",
})

_CACHE_NATIVE_ARTIFACT_FIELDS = frozenset({
    "name",
    "preinstalled_stage_root",
    "stage_manifest",
    "stage_manifest_sha256",
    "control_file",
    "install_sql",
    "dso",
    "staged_control_file",
    "staged_install_sql",
    "staged_dso",
    "control_sha256",
    "install_sql_sha256",
    "dso_sha256",
    "staged_control_sha256",
})

_CACHE_NATIVE_ARTIFACT_PATH_FIELDS = (
    "preinstalled_stage_root",
    "stage_manifest",
    "control_file",
    "install_sql",
    "dso",
    "staged_control_file",
    "staged_install_sql",
    "staged_dso",
)

_CACHE_NATIVE_ARTIFACT_DIGEST_FIELDS = (
    "stage_manifest_sha256",
    "control_sha256",
    "install_sql_sha256",
    "dso_sha256",
    "staged_control_sha256",
)

def _cache_native_artifact_contracts(
        artifacts: object,
) -> list[dict[str, object]]:
    if (
        not isinstance(artifacts, list)
        or not artifacts
        or any(not isinstance(artifact, dict)
               for artifact in artifacts)
    ):
        raise RuntimeError(
            "native cache report artifact contract is incomplete")
    result: list[dict[str, object]] = []
    for ordinal, artifact in enumerate(artifacts):
        if set(artifact) != _CACHE_NATIVE_ARTIFACT_FIELDS:
            raise RuntimeError(
                "native cache report artifact has an inexact field "
                f"contract at ordinal {ordinal}")
        if artifact.get("name") not in {"bioseq_cache", "bioseq"}:
            raise RuntimeError(
                "native cache report artifact names an unexpected "
                f"extension at ordinal {ordinal}")
        for field in _CACHE_NATIVE_ARTIFACT_PATH_FIELDS:
            value = artifact.get(field)
            if (
                not isinstance(value, str)
                or not Path(value).is_absolute()
                or os.path.normpath(value) != value
            ):
                raise RuntimeError(
                    "native cache report artifact path is not normalized "
                    f"and absolute: artifacts[{ordinal}].{field}")
        for field in _CACHE_NATIVE_ARTIFACT_DIGEST_FIELDS:
            value = artifact.get(field)
            if (
                not isinstance(value, str)
                or not re.fullmatch(r"[0-9a-f]{64}", value)
            ):
                raise RuntimeError(
                    "native cache report artifact digest is invalid: "
                    f"artifacts[{ordinal}].{field}")
        result.append(dict(artifact))
    return result

def _require_absent_nofollow_path(
        path: Path,
        context: str,
) -> None:
    if not path.is_absolute():
        raise RuntimeError(f"{context} is not absolute: {path}")
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current /= component
        try:
            metadata = current.lstat()
        except FileNotFoundError:
            return
        except OSError as exc:
            raise RuntimeError(
                f"cannot inspect {context} component {current}: {exc}"
            ) from exc
        if stat.S_ISLNK(metadata.st_mode):
            raise RuntimeError(
                f"{context} traverses a symlink: {current}")
        if current == path:
            raise RuntimeError(f"{context} still exists: {path}")
        if not stat.S_ISDIR(metadata.st_mode):
            raise RuntimeError(
                f"{context} traverses a non-directory: {current}")
    raise RuntimeError(f"{context} still exists: {path}")

def _cache_cleanup_root_contracts(
        cleanup_observations: list[object],
) -> list[str]:
    canonical_temporary_directory = Path(
        tempfile.gettempdir()).resolve(strict=True)
    roots = [
        observation.get("root")
        if isinstance(observation, dict) else None
        for observation in cleanup_observations
    ]
    if any(
        not isinstance(root, str)
        or not Path(root).is_absolute()
        or os.path.normpath(root) != root
        or not Path(root).name.startswith("bioseq-pg-test-")
        or Path(root).parent != canonical_temporary_directory
        for root in roots
    ):
        raise RuntimeError(
            "native cache integration cleanup roots are invalid or "
            "not marker-owned direct children of the canonical temporary "
            "directory")
    return roots

def _require_cache_cleanup_roots_absent(
        cleanup_roots: list[Path],
        context: str,
) -> None:
    for cleanup_root in cleanup_roots:
        _require_absent_nofollow_path(cleanup_root, context)


def _cache_integration_case_contract(
        cases: object,
        executable: str,
        *,
        focused: bool,
) -> int:
    """Validate selected cases and exact unfiltered executable cardinality."""
    if not isinstance(cases, list):
        raise RuntimeError(
            "native cache integration report cases are not an array")
    if any(
        not isinstance(case, dict)
        or not isinstance(case.get("test_case"), str)
        or not case.get("test_case")
        or not isinstance(case.get("section"), str)
        or case.get("status") != "passed"
        for case in cases
    ):
        raise RuntimeError(
            "native cache integration report contains an invalid or "
            "non-passing case")

    top_level_names = [
        str(case["test_case"])
        for case in cases
        if case["section"] == ""
    ]
    if len(top_level_names) != len(set(top_level_names)):
        raise RuntimeError(
            "native cache integration report repeats a top-level Catch case")
    expected = _CACHE_INTEGRATION_EXACT_CASES_BY_EXECUTABLE.get(executable)
    if expected is None:
        raise RuntimeError(
            "native cache integration executable has no exact case "
            f"cardinality: {executable}")
    if not focused and len(top_level_names) != expected:
        raise RuntimeError(
            "native cache integration report has "
            f"{len(top_level_names)} top-level Catch cases for {executable}; "
            f"the unfiltered contract requires exactly {expected}")
    if focused and len(top_level_names) > expected:
        raise RuntimeError(
            "native cache integration focused report exceeds the executable's "
            f"{expected}-case unfiltered closure")
    return len(top_level_names)


def _cache_integration_argv_contract(
        reported_argv: Sequence[str],
        test_flags: Sequence[str],
) -> dict[str, str]:
    """Validate runner-owned pairs plus the exact focused Catch suffix."""
    focused_flags = list(test_flags)
    if focused_flags:
        if (
            len(focused_flags) != 2
            or focused_flags[0] != "--allow-running-no-tests"
            or not focused_flags[1]
        ):
            raise RuntimeError(
                "native cache integration expected focused Catch arguments "
                "are invalid")
        if (
            len(reported_argv) < len(focused_flags)
            or list(reported_argv[-len(focused_flags):]) != focused_flags
        ):
            raise RuntimeError(
                "native cache integration report does not contain the exact "
                "focused Catch arguments")
        runner_argv = list(reported_argv[:-len(focused_flags)])
    else:
        runner_argv = list(reported_argv)

    if len(runner_argv) % 2 != 1:
        raise RuntimeError(
            "native cache integration report argv has a missing option value")
    reported_options: dict[str, str] = {}
    for index in range(1, len(runner_argv), 2):
        option = runner_argv[index]
        if (
            option not in _CACHE_INTEGRATION_RUNNER_OPTIONS
            or option in reported_options
        ):
            raise RuntimeError(
                "native cache integration report argv contains an unknown "
                "option, duplicate option, or unrequested Catch2 filter")
        reported_options[option] = runner_argv[index + 1]
    if (
        "--reporter" in reported_options
        and reported_options["--reporter"]
            != "console::out=-::colour-mode=ansi"
    ):
        raise RuntimeError(
            "native cache integration report uses an unsupported reporter")
    return reported_options


def _cache_native_report_contract(
        report_path: Path,
        *,
        test_flags: Sequence[str] = (),
) -> dict[str, object]:
    try:
        value = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            f"cannot read native cache report {report_path}: {exc}"
        ) from exc
    if not isinstance(value, dict):
        raise RuntimeError("native cache report is not a JSON object")
    schema_revision = value.get("schema_revision")
    if type(schema_revision) is not int or schema_revision != 2:
        raise RuntimeError(
            "native cache report schema_revision is not the current "
            f"revision 2 contract: {schema_revision!r}")
    benchmark_schema = "metadata" in value
    expected_keys = (
        _CACHE_BENCHMARK_REPORT_KEYS
        if benchmark_schema else _CACHE_INTEGRATION_REPORT_KEYS
    )
    actual_keys = set(value)
    if actual_keys != expected_keys:
        raise RuntimeError(
            "native cache report keys differ from the current contract: "
            f"missing={sorted(expected_keys - actual_keys)} "
            f"unknown={sorted(actual_keys - expected_keys)}")
    integration_schema = not benchmark_schema
    if integration_schema:
        cases = value.get("cases")
        arguments = value.get("arguments")
        cleanup = value.get("cleanup")
        cleanup_observations = value.get("cleanup_observations")
        cluster = value.get("cluster_configuration")
        if (
            not isinstance(cleanup, dict)
            or cleanup.get("succeeded") is not True
            or not isinstance(cleanup_observations, list)
            or (
                isinstance(cases, list)
                and bool(cases)
                and not cleanup_observations
            )
            or any(
                not isinstance(observation, dict)
                or observation.get("succeeded") is not True
                for observation in cleanup_observations
            )
            or not isinstance(cluster, dict)
            or cluster.get("retained-supervision-state") != "none"
        ):
            raise RuntimeError(
                "native cache integration report did not pass with "
                "complete cleanup evidence")
        executable = value.get("executable")
        executable_sha256 = value.get("build_identity")
        raw_artifacts = value.get("artifacts")
        focused_zero_match = (
            bool(test_flags)
            and isinstance(cases, list)
            and not cases
        )
        artifacts = (
            []
            if focused_zero_match and raw_artifacts == []
            else _cache_native_artifact_contracts(raw_artifacts)
        )
        cleanup_roots = _cache_cleanup_root_contracts(
            cleanup_observations)
        if not isinstance(arguments, list) or not arguments:
            raise RuntimeError(
                "native cache integration report has no executed argv")
        reported_argv: list[str] = []
        for index, argument in enumerate(arguments):
            if (
                not isinstance(argument, dict)
                or set(argument) != {"name", "value", "redacted"}
                or argument.get("name") != f"argv[{index}]"
                or not isinstance(argument.get("value"), str)
                or argument.get("redacted") is not False
            ):
                raise RuntimeError(
                    "native cache integration report argv is not an "
                    "exact unredacted sequence")
            reported_argv.append(argument["value"])
        _cache_integration_argv_contract(reported_argv, test_flags)
    else:
        metadata = value.get("metadata")
        cleanup = value.get("cleanup")
        build_identity = (
            metadata.get("build_identity")
            if isinstance(metadata, dict) else None
        )
        if (
            not isinstance(metadata, dict)
            or metadata.get("status") != "passed"
            or metadata.get("error") != ""
            or not isinstance(cleanup, dict)
            or cleanup.get("attempted") is not True
            or cleanup.get("succeeded") is not True
            or cleanup.get("private_root_removed") is not True
            or not isinstance(build_identity, dict)
        ):
            raise RuntimeError(
                "native cache benchmark did not pass with complete "
                "cleanup evidence")
        executable = build_identity.get("executable_path")
        executable_sha256 = build_identity.get("executable_sha256")
        scenario = value.get("scenario")
        configured_samples = metadata.get("configured_samples")
        configured_warmup_samples = metadata.get(
            "configured_warmup_samples")
        artifacts = _cache_native_artifact_contracts(
            metadata.get("artifacts"))
        cleanup_roots = []
        if (
            not isinstance(configured_samples, int)
            or isinstance(configured_samples, bool)
            or configured_samples <= 0
            or not isinstance(configured_warmup_samples, int)
            or isinstance(configured_warmup_samples, bool)
            or configured_warmup_samples < 0
        ):
            raise RuntimeError(
                "native cache benchmark invocation/artifact contract "
                "is incomplete")
    if (
        not isinstance(executable, str)
        or not Path(executable).is_absolute()
        or not isinstance(executable_sha256, str)
        or not re.fullmatch(r"[0-9a-f]{64}", executable_sha256)
    ):
        raise RuntimeError(
            "native cache report executable contract is incomplete")
    expected_executable = Path(executable).name
    role = _CACHE_EVIDENCE_ROLE_BY_EXECUTABLE.get(expected_executable)
    if role is None:
        raise RuntimeError(
            "native cache report executable has no fixed evidence role")
    if integration_schema:
        selected_case_count = _cache_integration_case_contract(
            cases,
            expected_executable,
            focused=bool(test_flags),
        )
        scenario = _CACHE_EVIDENCE_SCENARIO_BY_ROLE[role]
    else:
        selected_case_count = None
    if scenario != _CACHE_EVIDENCE_SCENARIO_BY_ROLE.get(role):
        raise RuntimeError(
            "native cache report scenario differs from its fixed role")
    if (
        integration_schema
        and Path(reported_argv[0]) != Path(executable)
    ):
        raise RuntimeError(
            "native cache report executable is incompatible with its "
            "fixed evidence role")
    return {
        "report_role": role,
        "executable": executable,
        "executable_sha256": executable_sha256,
        "scenario": scenario,
        "selected_case_count": selected_case_count,
        "arguments": (
            reported_argv
            if integration_schema else []
        ),
        "configured_samples": (
            configured_samples
            if benchmark_schema else None
        ),
        "configured_warmup_samples": (
            configured_warmup_samples
            if benchmark_schema else None
        ),
        "artifacts": artifacts,
        "cleanup_roots": cleanup_roots,
    }

def _load_json_object(path: Path, context: str) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"cannot read {context} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise RuntimeError(f"{context} is not a JSON object: {path}")
    return value


CACHE_BIOSEQ_RUNTIME_TOTAL_SEC = 8_400
CACHE_BIOSEQ_ORCHESTRATION_BUDGET_SEC = 600
CACHE_BIOSEQ_SUPPORT_AGGREGATE_TIMEOUT_SEC = 900
CACHE_BIOSEQ_LARGE_INPUT_TIMEOUT_SEC = 6_300
CACHE_LARGE_INPUT_FINALIZATION_RESERVE_SEC = 600
CACHE_BIOSEQ_LARGE_LAUNCH_MIN_REMAINING_SEC = (
    CACHE_BIOSEQ_LARGE_INPUT_TIMEOUT_SEC
    + CACHE_LARGE_INPUT_FINALIZATION_RESERVE_SEC
)
CACHE_BIOSEQ_SUPPORT_LAUNCH_MIN_REMAINING_SEC = (
    CACHE_BIOSEQ_SUPPORT_AGGREGATE_TIMEOUT_SEC
    + CACHE_BIOSEQ_LARGE_LAUNCH_MIN_REMAINING_SEC
)

_CACHE_BIOSEQ_SUPPORT_EXECUTABLE = (
    "bioseq_cache_bioseq_support_integration_tests")
_CACHE_BIOSEQ_LARGE_EXECUTABLE = (
    "bioseq_cache_bioseq_large_input_e2e_tests")


def _cache_case_timeout(argv: Sequence[str]) -> int:
    try:
        option = argv.index("--case-timeout-seconds")
        configured = int(argv[option + 1])
    except (ValueError, IndexError) as exc:
        raise RuntimeError(
            "cache integration command has no valid case watchdog") from exc
    if configured <= 0:
        raise RuntimeError(
            "cache integration command has no positive case watchdog")
    return configured


def _cache_apply_runtime_budget(
        mode: str, argv: list[str], deadline: float,
        *, now: float | None = None) -> list[str]:
    """Require the full large-case watchdog plus its finalization reserve."""
    if (
        mode != "cache-bioseq-integration"
        or not argv
        or Path(argv[0]).name != _CACHE_BIOSEQ_LARGE_EXECUTABLE
    ):
        return argv
    configured = _cache_case_timeout(argv)
    if configured != CACHE_BIOSEQ_LARGE_INPUT_TIMEOUT_SEC:
        raise RuntimeError(
            "large-input command watchdog must remain exactly "
            f"{CACHE_BIOSEQ_LARGE_INPUT_TIMEOUT_SEC} seconds")
    if not math.isfinite(deadline):
        return list(argv)
    current_time = time.time() if now is None else now
    remaining_seconds = deadline - current_time
    if remaining_seconds < CACHE_BIOSEQ_LARGE_LAUNCH_MIN_REMAINING_SEC:
        raise TimeoutError(
            "cache-bioseq-integration refuses to launch the large-input "
            "executable with fewer than "
            f"{CACHE_BIOSEQ_LARGE_LAUNCH_MIN_REMAINING_SEC} seconds "
            "remaining")
    # The 6,300-second watchdog is an acceptance contract, not a value that
    # may be silently shortened to fit prior overhead.
    return list(argv)


def _cache_command_deadline(
        mode: str, argv: Sequence[str], deadline: float,
        *, now: float | None = None) -> float:
    """Return the fixed aggregate support deadline inside the 8,400s run."""
    if (
        mode != "cache-bioseq-integration"
        or not argv
        or Path(argv[0]).name != _CACHE_BIOSEQ_SUPPORT_EXECUTABLE
        or not math.isfinite(deadline)
    ):
        return deadline
    current_time = time.time() if now is None else now
    remaining_seconds = deadline - current_time
    if remaining_seconds < CACHE_BIOSEQ_SUPPORT_LAUNCH_MIN_REMAINING_SEC:
        raise TimeoutError(
            "cache-bioseq-integration exhausted its "
            f"{CACHE_BIOSEQ_ORCHESTRATION_BUDGET_SEC}-second orchestration "
            "budget before the provider-support executable")
    return current_time + CACHE_BIOSEQ_SUPPORT_AGGREGATE_TIMEOUT_SEC

def _validate_failed_cache_terminal_report(report_path: Path) -> None:
    """A timed/failed runner may fail, but never retain owned resources."""
    value = _load_json_object(
        report_path, "failed cache integration terminal report")
    if (
        type(value.get("schema_revision")) is not int
        or value.get("schema_revision") != 2
        or "schema_version" in value
    ):
        raise RuntimeError(
            "failed cache integration has no revision-2 terminal report")
    cleanup = value.get("cleanup")
    observations = value.get("cleanup_observations")
    cluster = value.get("cluster_configuration")
    if (
        not isinstance(cleanup, dict)
        or cleanup.get("succeeded") is not True
        or not isinstance(observations, list)
        or not observations
        or any(
            not isinstance(observation, dict)
            or observation.get("succeeded") is not True
            for observation in observations
        )
        or not isinstance(cluster, dict)
        or cluster.get("retained-supervision-state") != "none"
    ):
        raise RuntimeError(
            "failed cache integration terminal report does not prove "
            "successful cleanup and zero retained supervision state")
    cleanup_roots = _cache_cleanup_root_contracts(observations)
    lexical_cleanup_roots = [
        Path(root) for root in cleanup_roots
    ]
    _require_cache_cleanup_roots_absent(
        lexical_cleanup_roots,
        "failed cache integration cleanup root",
    )
    _require_cache_cleanup_roots_absent(
        lexical_cleanup_roots,
        "failed cache integration cleanup root before acceptance",
    )

def _native_executable_identity(
        metadata: os.stat_result,
) -> tuple[int, ...]:
    # Access time is intentionally omitted because hashing/execution may
    # update it. The execution snapshot is a separate inode, so source link
    # count and ctime remain authoritative retained-handle metadata.
    return (
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mode,
        metadata.st_nlink,
        metadata.st_uid,
        metadata.st_size,
        metadata.st_mtime_ns,
        metadata.st_ctime_ns,
    )

def _open_native_command_identity(
        argv: list[str],
) -> tuple[int, dict[str, object]]:
    if not argv:
        raise RuntimeError("native command invocation is empty")
    requested = Path(argv[0])
    try:
        requested_status = requested.lstat()
        resolved = requested.resolve(strict=True)
    except OSError as exc:
        raise RuntimeError(
            f"native command executable is unavailable: {requested}"
        ) from exc
    if requested.is_symlink() or not stat.S_ISREG(requested_status.st_mode):
        raise RuntimeError(
            "native command executable is not a regular non-symlink file: "
            f"{requested}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(str(resolved), flags)
    except OSError as exc:
        raise RuntimeError(
            f"cannot open native command executable: {resolved}") from exc
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode):
            raise RuntimeError(
                f"native command executable is not regular: {resolved}")
        digest = sha256_file(descriptor)
        after = os.fstat(descriptor)
    except BaseException:
        os.close(descriptor)
        raise
    identity = _native_executable_identity(before)
    if identity != _native_executable_identity(after):
        os.close(descriptor)
        raise RuntimeError(
            "native command executable changed while it was hashed: "
            f"{resolved}")
    return descriptor, {
        "executable": str(resolved),
        "executable_sha256": digest,
        "arguments": list(argv[1:]),
        "_identity": identity,
    }

def _stable_native_command_identity(
        argv: list[str]) -> dict[str, object]:
    descriptor, result = _open_native_command_identity(argv)
    os.close(descriptor)
    return result

def _verified_native_execution_snapshot(
        executable: Path,
        source_descriptor: int,
        expected_sha256: str,
) -> tuple[Path, int, tuple[int, ...]]:
    # A same-directory private snapshot preserves loader-relative semantics
    # while binding execution to bytes read from the retained source descriptor.
    for _attempt in range(32):
        candidate = executable.with_name(
            ".bioseq-verified-exec-"
            f"{os.getpid()}-{secrets.token_hex(12)}")
        snapshot_identity: tuple[int, ...] | None = None

        def write_snapshot(creator_descriptor: int) -> None:
            nonlocal snapshot_identity
            copy_descriptor_exact(source_descriptor, creator_descriptor)
            snapshot_status = os.fstat(creator_descriptor)
            path_status = candidate.lstat()
            snapshot_identity = _native_executable_identity(snapshot_status)
            if (
                candidate.is_symlink()
                or not stat.S_ISREG(snapshot_status.st_mode)
                or _native_executable_identity(path_status) != snapshot_identity
                or sha256_file(source_descriptor) != expected_sha256
                or sha256_file(creator_descriptor) != expected_sha256
                or sha256_file(candidate) != expected_sha256
            ):
                raise RuntimeError(
                    "native executable changed while its verified byte "
                    f"snapshot was created: {executable}")

        try:
            publish_private_exclusive(
                candidate,
                write_snapshot,
                mode=0o500,
            )
        except FileExistsError:
            continue
        except OSError as exc:
            raise RuntimeError(
                "cannot create verified native execution snapshot beside "
                f"{executable}: {exc}") from exc

        retained_descriptor: int | None = None
        try:
            if snapshot_identity is None:
                raise RuntimeError(
                    "verified native snapshot publication returned no identity")
            retained_flags = os.O_RDONLY
            retained_flags |= getattr(os, "O_CLOEXEC", 0)
            retained_flags |= getattr(os, "O_NOFOLLOW", 0)
            retained_descriptor = os.open(candidate, retained_flags)
            retained_status = os.fstat(retained_descriptor)
            if (
                _native_executable_identity(retained_status) != snapshot_identity
                or sha256_file(retained_descriptor) != expected_sha256
                or (
                    fcntl.fcntl(retained_descriptor, fcntl.F_GETFL)
                    & os.O_ACCMODE
                ) != os.O_RDONLY
            ):
                raise RuntimeError(
                    "verified native execution snapshot could not be "
                    "retained read-only")
            result_descriptor = retained_descriptor
            retained_descriptor = None
            return candidate, result_descriptor, snapshot_identity
        except BaseException:
            if retained_descriptor is not None:
                os.close(retained_descriptor)
            with contextlib.suppress(OSError):
                candidate.unlink()
            raise
    raise RuntimeError(
        "cannot allocate a unique verified native execution snapshot "
        f"beside {executable}")


def _remove_verified_native_execution_snapshot(
        snapshot: Path,
) -> None:
    try:
        snapshot.unlink()
    except FileNotFoundError:
        # The desired terminal state is absence. Reconcile it below using
        # component-wise no-follow inspection rather than suppressing an
        # arbitrary unlink failure.
        pass
    except OSError as exc:
        raise RuntimeError(
            "cannot remove verified native execution snapshot "
            f"{snapshot}: {exc}") from exc
    _require_absent_nofollow_path(
        snapshot,
        "verified native execution snapshot after cleanup",
    )

@contextlib.contextmanager
def _verified_native_command_execution(
        argv: list[str],
):
    descriptor, record = _open_native_command_identity(argv)
    executable = Path(str(record["executable"]))
    identity = record["_identity"]
    executable_sha256 = record["executable_sha256"]
    if (
        not isinstance(identity, tuple)
        or not isinstance(executable_sha256, str)
    ):
        os.close(descriptor)
        raise RuntimeError(
            "native executable retained identity is invalid")
    snapshot: Path | None = None
    snapshot_descriptor: int | None = None
    snapshot_identity: tuple[int, ...] | None = None
    try:
        (
            snapshot,
            snapshot_descriptor,
            snapshot_identity,
        ) = _verified_native_execution_snapshot(
            executable, descriptor, executable_sha256)
        yield record, snapshot
        if (
            _native_executable_identity(os.fstat(descriptor))
                != identity
            or sha256_file(descriptor) != executable_sha256
            or _native_executable_identity(
                os.fstat(snapshot_descriptor))
                != snapshot_identity
            or sha256_file(snapshot_descriptor)
                != executable_sha256
            or _native_executable_identity(snapshot.lstat())
                != snapshot_identity
            or sha256_file(snapshot) != executable_sha256
        ):
            raise RuntimeError(
                "native executable changed while its verified handle "
                f"was executing: {executable}")
    finally:
        try:
            if snapshot_descriptor is not None:
                os.close(snapshot_descriptor)
            if snapshot is not None:
                _remove_verified_native_execution_snapshot(
                    snapshot)
        finally:
            os.close(descriptor)

def _validate_json_content_seal(
        value: dict[str, object], context: str) -> str:
    seal = value.get("content_sha256")
    if (
        not isinstance(seal, str)
        or re.fullmatch(r"[0-9a-f]{64}", seal) is None
    ):
        raise ValueError(f"{context} content_sha256 is invalid")
    unsealed = dict(value)
    del unsealed["content_sha256"]
    if canonical_json_sha256(unsealed) != seal:
        raise ValueError(f"{context} content seal does not match")
    return seal



def _report_index(filename: str) -> int:
    """Return the ordinal of an exact native run-report filename.

    The benchmark framework writes ``report_<N>.json``. Exact suffix
    validation deliberately excludes crash/debug sidecars such as
    ``report_debug_<N>.jsonl`` and ``report_<N>.raw.jsonl``.
    """
    if not filename.endswith(".json"):
        return -1
    stem = filename[:-len(".json")]
    prefix = "report_"
    if not stem.startswith(prefix):
        return -1
    ordinal = stem[len(prefix):]
    return int(ordinal) if ordinal.isascii() and ordinal.isdigit() else -1
