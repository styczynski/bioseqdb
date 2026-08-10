#!/usr/bin/env python3
"""Seal host-native load metadata for the two production extension DSOs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform as host_platform
import re
import stat
import subprocess
import tempfile
from typing import Sequence


EXTENSIONS = ("bioseq", "bioseq_cache")


class QualificationError(RuntimeError):
    """The production DSO qualification failed closed."""


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_json_sha256(value: dict[str, object]) -> str:
    return _sha256_bytes(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    )


def _sealed(value: dict[str, object]) -> dict[str, object]:
    if "content_sha256" in value:
        raise QualificationError("cannot seal a document which is already sealed")
    result = dict(value)
    result["content_sha256"] = _canonical_json_sha256(result)
    return result


def _write_json_atomic(path: Path, value: dict[str, object]) -> None:
    if not path.is_absolute():
        raise QualificationError(f"receipt path must be absolute: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and (path.is_symlink() or not path.is_file()):
        raise QualificationError(f"receipt destination is not regular: {path}")
    body = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            prefix=f".{path.name}.",
            suffix=".tmp",
            dir=path.parent,
            delete=False,
        ) as handle:
            temporary = Path(handle.name)
            handle.write(body)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _regular_file(raw: str, label: str, *, reject_symlink: bool) -> Path:
    requested = Path(raw)
    if not requested.is_absolute():
        raise QualificationError(f"{label} must be absolute: {requested}")
    try:
        metadata = requested.lstat()
        resolved = requested.resolve(strict=True)
        resolved_metadata = resolved.stat()
    except OSError as error:
        raise QualificationError(f"cannot inspect {label} {requested}: {error}") from error
    if (
        (reject_symlink and requested.is_symlink())
        or (
            not stat.S_ISREG(metadata.st_mode)
            and not requested.is_symlink()
        )
        or not stat.S_ISREG(resolved_metadata.st_mode)
    ):
        raise QualificationError(f"{label} must resolve to a regular file: {requested}")
    return resolved


def _file_identity(raw: str, label: str, *, executable: bool) -> dict[str, object]:
    requested = Path(raw)
    resolved = _regular_file(raw, label, reject_symlink=False)
    if executable and not os.access(resolved, os.X_OK):
        raise QualificationError(f"{label} is not executable: {resolved}")
    return {
        "lexical_path": str(requested),
        "resolved_path": str(resolved),
        "size_bytes": resolved.stat().st_size,
        "sha256": _sha256_file(resolved),
    }


def _normalized_listing(value: str) -> str:
    lines = value.replace("\r\n", "\n").replace("\r", "\n").splitlines()
    return "\n".join(line.rstrip() for line in lines).strip() + "\n"


def _run_tool(tool: Path, arguments: Sequence[str]) -> str:
    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "LANG": "C"})
    try:
        completed = subprocess.run(
            [str(tool), *arguments],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
        )
    except OSError as error:
        raise QualificationError(f"cannot execute {tool}: {error}") from error
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()
        raise QualificationError(
            f"inspection command failed ({tool.name} {' '.join(arguments)}): "
            f"{detail or completed.returncode}"
        )
    output = completed.stdout
    if not output.strip() and completed.stderr.strip():
        output = completed.stderr
    return _normalized_listing(output)


def _normalize_architecture(value: str) -> str:
    normalized = value.strip().lower().replace("-", "_")
    aliases = {
        "aarch64": "arm64",
        "arm64": "arm64",
        "arm64e": "arm64",
        "amd64": "x86_64",
        "x86_64": "x86_64",
    }
    try:
        return aliases[normalized]
    except KeyError as error:
        raise QualificationError(f"unsupported native architecture: {value}") from error


def _macho_architectures(listing: str) -> list[str]:
    values = {
        _normalize_architecture(match.group(1))
        for match in re.finditer(
            r"^\s*MH_[A-Z0-9_]+\s+([A-Za-z0-9_-]+)\s+",
            listing,
            re.MULTILINE,
        )
    }
    if not values:
        raise QualificationError("otool did not report a Mach-O architecture")
    return sorted(values)


def _macho_dependencies(listing: str) -> list[str]:
    dependencies: list[str] = []
    for line in listing.splitlines()[1:]:
        match = re.match(r"^\s+(.+?)\s+\(compatibility version ", line)
        if match:
            dependencies.append(match.group(1))
    if not dependencies:
        raise QualificationError("otool reported no direct dependencies")
    if len(dependencies) != len(set(dependencies)):
        raise QualificationError("otool reported duplicate direct dependencies")
    return sorted(dependencies)


def _macho_uuids(listing: str) -> list[str]:
    values = {
        match.group(1).lower()
        for match in re.finditer(
            r"^\s*uuid\s+([0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12})\b",
            listing,
            re.MULTILINE,
        )
    }
    if not values:
        raise QualificationError("Mach-O DSO has no LC_UUID build identity")
    return sorted(values)


def _elf_dependencies(listing: str) -> list[str]:
    dependencies = re.findall(r"\(NEEDED\).*?\[([^]\r\n]+)\]", listing)
    if not dependencies:
        raise QualificationError("readelf reported no DT_NEEDED entries")
    if len(dependencies) != len(set(dependencies)):
        raise QualificationError("readelf reported duplicate DT_NEEDED entries")
    return sorted(dependencies)


def _elf_build_ids(listing: str) -> list[str]:
    values = {
        match.group(1).lower()
        for match in re.finditer(
            r"^\s*Build ID:\s*([0-9A-Fa-f]+)\s*$", listing, re.MULTILINE
        )
    }
    if not values or any(len(value) % 2 != 0 for value in values):
        raise QualificationError("ELF DSO has no valid GNU build-id")
    return sorted(values)


def _elf_header(listing: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for field, key in (("Class", "class"), ("Data", "data"), ("Machine", "machine")):
        match = re.search(rf"^\s*{field}:\s*(.+?)\s*$", listing, re.MULTILINE)
        if not match:
            raise QualificationError(f"ELF header does not report {field}")
        result[key] = match.group(1)
    machine = result["machine"]
    if re.search(r"(?:X86-64|Advanced Micro Devices X86-64)", machine):
        result["architecture"] = "x86_64"
    elif machine == "AArch64":
        result["architecture"] = "arm64"
    else:
        raise QualificationError(f"unsupported ELF machine: {machine}")
    return result


def _dependency_names_sibling(
    dependency: str, sibling_basename: str, suffix: str
) -> bool:
    basename = PurePosixPath(dependency).name
    if basename == sibling_basename or basename.startswith(sibling_basename + "."):
        return True
    if suffix == ".dylib":
        stem = sibling_basename.removesuffix(suffix)
        return basename.startswith(stem + ".") and basename.endswith(suffix)
    return False


def _configure_input(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise QualificationError(f"cannot read configure-input stamp {path}: {error}") from error
    if (
        not isinstance(value, dict)
        or value.get("artifact_kind") != "bioseq-build-mcp-configure-v1"
        or value.get("schema_version") != 1
        or not isinstance(value.get("inputs"), dict)
        or not isinstance(value.get("inputs_sha256"), str)
        or not re.fullmatch(r"[0-9a-f]{64}", value["inputs_sha256"])
        or _canonical_json_sha256(value["inputs"]) != value["inputs_sha256"]
        or not isinstance(value["inputs"].get("preset"), str)
        or not value["inputs"]["preset"]
    ):
        raise QualificationError("configure-input stamp has an invalid seal")
    return value


def _inspect_dso(
    *,
    extension: str,
    path: Path,
    sibling_extension: str,
    sibling_basename: str,
    platform_name: str,
    native_architecture: str,
    tool: Path,
) -> dict[str, object]:
    suffix = ".dylib" if platform_name == "Darwin" else ".so"
    expected_basename = f"{extension}{suffix}"
    if path.name != expected_basename:
        raise QualificationError(
            f"{extension} target resolved {path.name}, expected {expected_basename}"
        )

    if platform_name == "Darwin":
        header_listing = _run_tool(tool, ["-hv", str(path)])
        dependency_listing = _run_tool(tool, ["-L", str(path)])
        identity_listing = _run_tool(tool, ["-l", str(path)])
        architectures = _macho_architectures(header_listing)
        build_identity = {
            "kind": "mach-o-lc-uuid",
            "values": _macho_uuids(identity_listing),
        }
        binary: dict[str, object] = {
            "format": "Mach-O",
            "architectures": architectures,
        }
    else:
        header_listing = _run_tool(tool, ["-h", str(path)])
        dependency_listing = _run_tool(tool, ["-d", str(path)])
        identity_listing = _run_tool(tool, ["-n", str(path)])
        header = _elf_header(header_listing)
        architectures = [header["architecture"]]
        build_identity = {
            "kind": "elf-gnu-build-id",
            "values": _elf_build_ids(identity_listing),
        }
        binary = {"format": "ELF", "architectures": architectures, **header}
    if architectures != [native_architecture]:
        raise QualificationError(
            f"{extension} architecture {architectures} differs from native "
            f"host {native_architecture}"
        )

    sibling_dependencies = [
        dependency
        for dependency in _macho_dependencies(dependency_listing)
        if _dependency_names_sibling(dependency, sibling_basename, suffix)
    ] if platform_name == "Darwin" else [
        dependency
        for dependency in _elf_dependencies(dependency_listing)
        if _dependency_names_sibling(dependency, sibling_basename, suffix)
    ]
    dependencies = (
        _macho_dependencies(dependency_listing)
        if platform_name == "Darwin"
        else _elf_dependencies(dependency_listing)
    )
    if sibling_dependencies:
        raise QualificationError(
            f"{extension} directly depends on sibling {sibling_extension}: "
            + ", ".join(sibling_dependencies)
        )
    return {
        "extension": extension,
        "path": str(path),
        "basename": path.name,
        "size_bytes": path.stat().st_size,
        "sha256": _sha256_file(path),
        "binary": binary,
        "header_listing_sha256": _sha256_bytes(header_listing.encode("utf-8")),
        "build_identity": build_identity,
        "direct_load_dependencies": dependencies,
        "dependency_listing_sha256": _sha256_bytes(
            dependency_listing.encode("utf-8")
        ),
        "build_identity_listing_sha256": _sha256_bytes(
            identity_listing.encode("utf-8")
        ),
        "sibling_extension": sibling_extension,
        "sibling_basename": sibling_basename,
        "sibling_dependency_absent": True,
    }


def qualify(arguments: argparse.Namespace) -> dict[str, object]:
    actual_system = host_platform.system()
    if actual_system != arguments.platform or actual_system not in {"Darwin", "Linux"}:
        raise QualificationError(
            f"actual host platform {actual_system} differs from CMake "
            f"platform {arguments.platform}"
        )
    native_architecture = _normalize_architecture(host_platform.machine())
    cmake_architecture = _normalize_architecture(arguments.system_processor)
    if native_architecture != cmake_architecture:
        raise QualificationError(
            "actual host architecture differs from CMake system processor"
        )
    suffix = ".dylib" if actual_system == "Darwin" else ".so"

    tool_identity = _file_identity(
        arguments.inspection_tool, "inspection tool", executable=True)
    tool = Path(str(tool_identity["resolved_path"]))
    dso_paths = {
        "bioseq": _regular_file(arguments.bioseq_dso, "bioseq DSO", reject_symlink=True),
        "bioseq_cache": _regular_file(
            arguments.cache_dso, "bioseq_cache DSO", reject_symlink=True
        ),
    }
    if len(set(dso_paths.values())) != 2:
        raise QualificationError("production DSO inputs reuse one path")

    build_directory = Path(arguments.build_directory).resolve(strict=True)
    if not build_directory.is_dir():
        raise QualificationError("CMake build directory is not a directory")
    cmake_cache = _regular_file(
        arguments.cmake_cache, "CMakeCache", reject_symlink=True
    )
    configure_stamp_path = _regular_file(
        arguments.configure_input_stamp,
        "configure-input stamp",
        reject_symlink=True,
    )
    if cmake_cache.parent != build_directory or configure_stamp_path.parent != build_directory:
        raise QualificationError("CMake context files escape the exact build directory")
    configure_stamp = _configure_input(configure_stamp_path)
    if arguments.build_type and arguments.build_config != arguments.build_type:
        raise QualificationError("active build configuration differs from CMAKE_BUILD_TYPE")

    compiler_context = {}
    for language, raw, compiler_id, compiler_version in (
        ("c", arguments.c_compiler, arguments.c_compiler_id, arguments.c_compiler_version),
        (
            "cxx",
            arguments.cxx_compiler,
            arguments.cxx_compiler_id,
            arguments.cxx_compiler_version,
        ),
    ):
        if not compiler_id or not compiler_version:
            raise QualificationError(f"CMake did not identify the {language} compiler")
        compiler_context[language] = {
            **_file_identity(raw, f"{language} compiler", executable=True),
            "compiler_id": compiler_id,
            "compiler_version": compiler_version,
        }

    dso_records = []
    for extension in EXTENSIONS:
        sibling = "bioseq_cache" if extension == "bioseq" else "bioseq"
        dso_records.append(
            _inspect_dso(
                extension=extension,
                path=dso_paths[extension],
                sibling_extension=sibling,
                sibling_basename=dso_paths[sibling].name,
                platform_name=actual_system,
                native_architecture=native_architecture,
                tool=tool,
            )
        )

    return _sealed({
        "schema_version": 1,
        "artifact_kind": "bioseq-production-dso-load-qualification",
        "platform": {
            "system_name": actual_system,
            "system_release": host_platform.release(),
            "machine": native_architecture,
            "binary_format": "Mach-O" if actual_system == "Darwin" else "ELF",
            "dso_suffix": suffix,
        },
        "inspection_tool": {
            **tool_identity,
            "identity_kind": "executable-sha256",
            "header_arguments": ["-hv"] if actual_system == "Darwin" else ["-h"],
            "dependency_arguments": ["-L"] if actual_system == "Darwin" else ["-d"],
            "build_identity_arguments": ["-l"] if actual_system == "Darwin" else ["-n"],
        },
        "build_context": {
            "preset": configure_stamp["inputs"]["preset"],
            "configuration": arguments.build_config,
            "build_type": arguments.build_type,
            "generator": arguments.generator,
            "build_directory": str(build_directory),
            "cmake_system": {
                "name": arguments.platform,
                "processor": cmake_architecture,
            },
            "cmake_cache": {
                "path": str(cmake_cache),
                "size_bytes": cmake_cache.stat().st_size,
                "sha256": _sha256_file(cmake_cache),
            },
            "configure_input": {
                "path": str(configure_stamp_path),
                "size_bytes": configure_stamp_path.stat().st_size,
                "sha256": _sha256_file(configure_stamp_path),
                "inputs_sha256": configure_stamp["inputs_sha256"],
            },
            "compilers": compiler_context,
        },
        "dsos": dso_records,
    })


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("Darwin", "Linux"), required=True)
    parser.add_argument("--system-processor", required=True)
    parser.add_argument("--inspection-tool", required=True)
    parser.add_argument("--bioseq-dso", required=True)
    parser.add_argument("--cache-dso", required=True)
    parser.add_argument("--build-directory", required=True)
    parser.add_argument("--build-config", required=True)
    parser.add_argument("--build-type", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--c-compiler", required=True)
    parser.add_argument("--c-compiler-id", required=True)
    parser.add_argument("--c-compiler-version", required=True)
    parser.add_argument("--cxx-compiler", required=True)
    parser.add_argument("--cxx-compiler-id", required=True)
    parser.add_argument("--cxx-compiler-version", required=True)
    parser.add_argument("--cmake-cache", required=True)
    parser.add_argument("--configure-input-stamp", required=True)
    parser.add_argument("--receipt", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        receipt = qualify(arguments)
        _write_json_atomic(Path(arguments.receipt), receipt)
    except QualificationError as error:
        print(f"production DSO qualification failed: {error}", file=os.sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
