#!/usr/bin/env python3
"""Fail-closed validation for staged and rendered bioseq packages."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from typing import Sequence


PACKAGE_VERSION = "4.2.3"
EXTENSION_VERSION = "1.0"
REQUIRED_BASENAMES = frozenset(
    {
        "bioseq.so",
        "bioseq_cache.so",
        f"bioseq--{EXTENSION_VERSION}.sql",
        f"bioseq_cache--{EXTENSION_VERSION}.sql",
        "bioseq.control",
        "bioseq_cache.control",
    }
)
PACKAGE_METADATA_PATHS = {
    "deb": frozenset(),
    "rpm": frozenset(),
    # makepkg stores these package-manager records in the archive root. They
    # are not installed payload and therefore do not count toward the six
    # extension artifacts.
    "pacman": frozenset({".BUILDINFO", ".MTREE", ".PKGINFO"}),
}
COMPILER_RUNTIME_SONAMES = (
    "libc++.so",
    "libc++abi.so",
    "libunwind.so",
    "libstdc++.so",
    "libgcc_s.so",
)


class ValidationError(RuntimeError):
    pass


@dataclass(frozen=True)
class BinaryValidation:
    path: str
    elf_architecture: str
    rpath_absent: bool
    compiler_runtime_safe: bool


def run(command: Sequence[str], *, cwd: Path | None = None) -> str:
    try:
        completed = subprocess.run(
            list(command),
            cwd=cwd,
            check=True,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError as error:
        raise ValidationError(f"required validation tool is missing: {command[0]}") from error
    except subprocess.CalledProcessError as error:
        stderr = error.stderr.strip() if error.stderr else ""
        raise ValidationError(
            f"validation command failed ({' '.join(command)}): {stderr}"
        ) from error
    return completed.stdout


def expected_elf_machine(architecture: str) -> re.Pattern[str]:
    if architecture == "x64":
        return re.compile(r"(?:Advanced Micro Devices X86-64|X86-64)")
    if architecture == "arm64":
        return re.compile(r"AArch64")
    raise ValidationError(f"unsupported architecture {architecture}")


def validate_dso(path: Path, architecture: str) -> BinaryValidation:
    if not path.is_file():
        raise ValidationError(f"extension DSO is missing: {path}")
    header = run(["readelf", "-h", str(path)])
    match = re.search(r"^\s*Machine:\s*(.+)$", header, re.MULTILINE)
    if not match or not expected_elf_machine(architecture).search(match.group(1)):
        raise ValidationError(
            f"{path} has wrong ELF machine {match.group(1) if match else '<unknown>'}"
        )

    dynamic = run(["readelf", "-d", str(path)])
    if re.search(r"\((?:RPATH|RUNPATH)\)", dynamic):
        raise ValidationError(f"{path} contains an RPATH/RUNPATH; package DSOs must contain none")
    needed = re.findall(r"\(NEEDED\).*?\[([^]]+)\]", dynamic)
    unsafe = [
        soname
        for soname in needed
        if any(soname.startswith(prefix) for prefix in COMPILER_RUNTIME_SONAMES)
    ]
    if unsafe:
        raise ValidationError(
            f"{path} dynamically references non-portable compiler runtime(s): {', '.join(unsafe)}"
        )
    return BinaryValidation(
        path=str(path),
        elf_architecture=match.group(1).strip(),
        rpath_absent=True,
        compiler_runtime_safe=True,
    )


def pg_config_value(pg_config: Path, flag: str) -> str:
    return run([str(pg_config), flag]).strip()


def validate_pg_major(pg_config: Path, requested_major: int) -> None:
    version = pg_config_value(pg_config, "--version")
    match = re.search(r"PostgreSQL\s+(\d+)", version)
    if not match or int(match.group(1)) != requested_major:
        raise ValidationError(
            f"pg_config reports {version!r}, not requested PostgreSQL {requested_major}"
        )


def staged_expected_paths(root: Path, pg_config: Path) -> list[Path]:
    pkglibdir = PurePosixPath(pg_config_value(pg_config, "--pkglibdir"))
    sharedir = PurePosixPath(pg_config_value(pg_config, "--sharedir"))
    if not pkglibdir.is_absolute() or not sharedir.is_absolute():
        raise ValidationError("pg_config install directories must be absolute")
    library_root = root.joinpath(*pkglibdir.parts[1:])
    extension_root = root.joinpath(*sharedir.parts[1:], "extension")
    return [
        library_root / "bioseq.so",
        library_root / "bioseq_cache.so",
        extension_root / f"bioseq--{EXTENSION_VERSION}.sql",
        extension_root / f"bioseq_cache--{EXTENSION_VERSION}.sql",
        extension_root / "bioseq.control",
        extension_root / "bioseq_cache.control",
    ]


def validate_staged(
    root: Path, pg_config: Path, postgres_major: int, architecture: str
) -> dict[str, object]:
    validate_pg_major(pg_config, postgres_major)
    expected = staged_expected_paths(root, pg_config)
    missing = [str(path) for path in expected if not path.is_file()]
    if missing:
        raise ValidationError("missing staged extension artifacts: " + ", ".join(missing))
    staged_files = [path for path in root.rglob("*") if path.is_file()]
    if len(staged_files) != 6 or {path.name for path in staged_files} != REQUIRED_BASENAMES:
        raise ValidationError(
            "staged package must contain exactly the six expected bioseq artifacts; "
            f"found {[str(path.relative_to(root)) for path in staged_files]}"
        )
    binaries = [
        asdict(validate_dso(path, architecture))
        for path in expected
        if path.suffix == ".so"
    ]
    return {
        "passed": True,
        "six_artifacts": True,
        "postgres_major": postgres_major,
        "template_flatten_profile": "dna4_uint64",
        "binaries": binaries,
    }


def package_format(path: Path) -> str:
    name = path.name
    if name.endswith(".deb"):
        return "deb"
    if name.endswith(".rpm"):
        return "rpm"
    if name.endswith(".pkg.tar.zst"):
        return "pacman"
    raise ValidationError(f"unsupported package artifact format: {name}")


def safe_package_paths(paths: Sequence[str]) -> list[str]:
    normalized: list[str] = []
    for raw in paths:
        value = raw.strip()
        if not value or value.endswith("/"):
            continue
        relative = value.removeprefix("./").removeprefix("/")
        pure = PurePosixPath(relative)
        if pure.is_absolute() or ".." in pure.parts:
            raise ValidationError(f"unsafe path in package: {value}")
        normalized.append(relative)
    return normalized


def package_paths(path: Path, fmt: str) -> list[str]:
    if fmt == "deb":
        output = run(["dpkg-deb", "--contents", str(path)])
        values = []
        for line in output.splitlines():
            parts = line.split(maxsplit=5)
            if len(parts) == 6:
                values.append(parts[5].split(" -> ", maxsplit=1)[0])
        return safe_package_paths(values)
    if fmt == "rpm":
        return safe_package_paths(run(["rpm", "-qpl", str(path)]).splitlines())
    return safe_package_paths(run(["tar", "-tf", str(path)]).splitlines())


def exact_package_payload_paths(paths: Sequence[str], fmt: str) -> tuple[str, ...]:
    """Return the canonical six installed payload paths or fail closed."""
    try:
        metadata_paths = PACKAGE_METADATA_PATHS[fmt]
    except KeyError as error:
        raise ValidationError(f"unsupported package payload format: {fmt}") from error

    payload = tuple(value for value in paths if value not in metadata_paths)
    unexpected_metadata = [
        value
        for value in paths
        if PurePosixPath(value).name.startswith(".")
        and value not in metadata_paths
    ]
    if unexpected_metadata:
        raise ValidationError(
            "package contains unreviewed package-manager metadata: "
            + ", ".join(unexpected_metadata)
        )

    basename_counts = Counter(PurePosixPath(value).name for value in payload)
    if len(payload) != 6 or set(basename_counts) != REQUIRED_BASENAMES:
        raise ValidationError(
            "package must contain exactly the six extension payload artifacts; "
            f"found {list(payload)}"
        )
    duplicate_basenames = sorted(
        basename for basename, count in basename_counts.items() if count != 1
    )
    if duplicate_basenames:
        raise ValidationError(
            "package contains duplicate extension artifact basenames: "
            + ", ".join(duplicate_basenames)
        )

    by_basename = {
        PurePosixPath(value).name: PurePosixPath(value) for value in payload
    }
    dso_parents = {
        by_basename["bioseq.so"].parent,
        by_basename["bioseq_cache.so"].parent,
    }
    extension_parents = {
        by_basename[f"bioseq--{EXTENSION_VERSION}.sql"].parent,
        by_basename[f"bioseq_cache--{EXTENSION_VERSION}.sql"].parent,
        by_basename["bioseq.control"].parent,
        by_basename["bioseq_cache.control"].parent,
    }
    if len(dso_parents) != 1:
        raise ValidationError("extension DSOs are not installed in one directory")
    if len(extension_parents) != 1:
        raise ValidationError(
            "extension control and SQL files are not installed together"
        )
    dso_parent = next(iter(dso_parents))
    extension_parent = next(iter(extension_parents))
    if (
        not dso_parent.parts
        or dso_parent.parts[0] != "usr"
        or not extension_parent.parts
        or extension_parent.parts[0] != "usr"
        or extension_parent.name != "extension"
    ):
        raise ValidationError(
            "extension payload is outside canonical PostgreSQL /usr install directories"
        )
    return payload


def extract_package(path: Path, fmt: str, destination: Path) -> None:
    if fmt == "deb":
        run(["dpkg-deb", "-x", str(path), str(destination)])
        return
    if fmt == "pacman":
        run(["tar", "-xf", str(path), "-C", str(destination)])
        return
    try:
        first = subprocess.Popen(["rpm2cpio", str(path)], stdout=subprocess.PIPE)
    except FileNotFoundError as error:
        raise ValidationError("required validation tool is missing: rpm2cpio") from error
    assert first.stdout is not None
    try:
        second = subprocess.run(
            ["cpio", "-idm", "--quiet"],
            cwd=destination,
            stdin=first.stdout,
            check=True,
            capture_output=True,
        )
    except FileNotFoundError as error:
        first.kill()
        raise ValidationError("required validation tool is missing: cpio") from error
    finally:
        first.stdout.close()
    first_status = first.wait()
    if first_status != 0 or second.returncode != 0:
        raise ValidationError(f"failed to extract RPM package {path}")


def package_metadata(path: Path, fmt: str) -> tuple[str, str, str, str]:
    if fmt == "deb":
        values = [
            run(["dpkg-deb", "--field", str(path), field]).strip()
            for field in ("Package", "Version", "Architecture", "Depends")
        ]
        return values[0], values[1], values[2], values[3]
    if fmt == "rpm":
        query = run(
            [
                "rpm",
                "-qp",
                "--qf",
                "%{NAME}\n%{VERSION}-%{RELEASE}\n%{ARCH}\n",
                str(path),
            ]
        ).splitlines()
        dependencies = run(["rpm", "-qpR", str(path)])
        if len(query) < 3:
            raise ValidationError("RPM metadata query returned incomplete output")
        return query[0], query[1], query[2], dependencies
    pkginfo = run(["tar", "-xOf", str(path), ".PKGINFO"])
    fields: dict[str, list[str]] = {}
    for line in pkginfo.splitlines():
        if " = " in line:
            key, value = line.split(" = ", maxsplit=1)
            fields.setdefault(key, []).append(value)
    try:
        return (
            fields["pkgname"][0],
            fields["pkgver"][0],
            fields["arch"][0],
            "\n".join(fields.get("depend", [])),
        )
    except (KeyError, IndexError) as error:
        raise ValidationError("pacman package has incomplete .PKGINFO") from error


def expected_package_architecture(fmt: str, architecture: str) -> str:
    mapping = {
        ("deb", "x64"): "amd64",
        ("deb", "arm64"): "arm64",
        ("rpm", "x64"): "x86_64",
        ("rpm", "arm64"): "aarch64",
        ("pacman", "x64"): "x86_64",
        ("pacman", "arm64"): "aarch64",
    }
    try:
        return mapping[(fmt, architecture)]
    except KeyError as error:
        raise ValidationError(f"unsupported package architecture {fmt}/{architecture}") from error


def validate_artifact(
    path: Path,
    architecture: str,
    postgres_major: int,
    postgres_runtime_package: str,
    vulkan_runtime_package: str,
) -> dict[str, object]:
    fmt = package_format(path)
    expected_name = f"bioseq-postgresql{postgres_major}"
    if expected_name not in path.name:
        raise ValidationError(
            f"artifact filename must contain PostgreSQL package marker {expected_name}"
        )
    listed_paths = package_paths(path, fmt)
    payload_paths = exact_package_payload_paths(listed_paths, fmt)

    name, version, package_arch, dependencies = package_metadata(path, fmt)
    if name != expected_name:
        raise ValidationError(f"package name {name!r} does not equal {expected_name!r}")
    if not version.startswith(PACKAGE_VERSION):
        raise ValidationError(f"package version {version!r} does not start with {PACKAGE_VERSION}")
    expected_arch = expected_package_architecture(fmt, architecture)
    if package_arch != expected_arch:
        raise ValidationError(
            f"package architecture {package_arch!r} does not equal {expected_arch!r}"
        )
    for dependency in (postgres_runtime_package, vulkan_runtime_package):
        if dependency not in dependencies:
            raise ValidationError(f"package metadata is missing runtime dependency {dependency}")

    with tempfile.TemporaryDirectory(prefix="bioseq-package-validate-") as temporary:
        extracted = Path(temporary)
        extract_package(path, fmt, extracted)
        extracted_payload_paths = []
        for candidate in extracted.rglob("*"):
            if not candidate.is_file() and not candidate.is_symlink():
                continue
            relative = candidate.relative_to(extracted).as_posix()
            if relative in PACKAGE_METADATA_PATHS[fmt]:
                continue
            extracted_payload_paths.append(relative)
        if (
            len(extracted_payload_paths) != 6
            or set(extracted_payload_paths) != set(payload_paths)
        ):
            raise ValidationError(
                "extracted package payload differs from the exact six listed "
                f"artifacts: {sorted(extracted_payload_paths)}"
            )
        dsos = [
            extracted / value
            for value in payload_paths
            if PurePosixPath(value).name in {"bioseq.so", "bioseq_cache.so"}
        ]
        if len(dsos) != 2 or not all(candidate.is_file() for candidate in dsos):
            raise ValidationError(
                "extracted package does not contain exactly both extension DSOs"
            )
        binaries = [
            asdict(validate_dso(candidate, architecture))
            for candidate in sorted(dsos)
        ]

    return {
        "passed": True,
        "six_artifacts": True,
        "format": fmt,
        "package_name": name,
        "package_version": version,
        "package_architecture": package_arch,
        "postgres_major": postgres_major,
        "postgres_runtime_dependency": postgres_runtime_package,
        "vulkan_runtime_dependency": vulkan_runtime_package,
        "template_flatten_profile": "dna4_uint64",
        "binaries": binaries,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    mode = result.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dso", type=Path)
    mode.add_argument("--staged-root", type=Path)
    mode.add_argument("--artifact", type=Path)
    result.add_argument("--pg-config", type=Path)
    result.add_argument("--postgres-major", type=int)
    result.add_argument("--architecture", choices=("arm64", "x64"), required=True)
    result.add_argument("--postgres-runtime-package")
    result.add_argument("--vulkan-runtime-package")
    return result


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    try:
        if arguments.dso:
            validation: dict[str, object] = asdict(
                validate_dso(arguments.dso, arguments.architecture)
            )
        elif arguments.staged_root:
            if arguments.pg_config is None or arguments.postgres_major is None:
                raise ValidationError(
                    "--staged-root requires --pg-config and --postgres-major"
                )
            validation = validate_staged(
                arguments.staged_root,
                arguments.pg_config,
                arguments.postgres_major,
                arguments.architecture,
            )
        else:
            if (
                arguments.postgres_major is None
                or not arguments.postgres_runtime_package
                or not arguments.vulkan_runtime_package
            ):
                raise ValidationError(
                    "--artifact requires PostgreSQL major and PostgreSQL/Vulkan runtime packages"
                )
            validation = validate_artifact(
                arguments.artifact,
                arguments.architecture,
                arguments.postgres_major,
                arguments.postgres_runtime_package,
                arguments.vulkan_runtime_package,
            )
    except ValidationError as error:
        print(json.dumps({"passed": False, "error": str(error)}, sort_keys=True))
        return 1
    print(json.dumps(validation, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
