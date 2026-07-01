#!/usr/bin/env python3
"""GCC compiler launcher that supplies Highway header units to CMake modules.

GCC 16 rejects several Highway declarations when they are textually included in
an exported C++ module interface.  CMake does not yet model non-standard-library
header units, so this launcher prebuilds the small stable Highway header-unit set
and appends the resulting mappings to CMake's per-TU module mapper.
"""

from __future__ import annotations

import fcntl
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
from typing import Iterable


HIGHWAY_HEADER_UNITS = (
    "hwy/base.h",
    "hwy/targets.h",
    "hwy/per_target.h",
    "hwy/aligned_allocator.h",
    "hwy/cache_control.h",
    "hwy/print.h",
    "hwy/highway.h",
    "hwy/print-inl.h",
)

HEADER_UNIT_DEFINE_PREFIXES = (
    "HWY",
    "TOOLCHAIN_",
    "_GLIBCXX",
)

HEADER_UNIT_DEFINE_NAMES = {
    "NDEBUG",
}

OPTIONS_WITH_VALUE = {
    "-I",
    "-D",
    "-U",
    "-include",
    "-imacros",
    "-isystem",
    "-iquote",
    "-idirafter",
    "-isysroot",
    "-MF",
    "-MT",
    "-MQ",
    "-MJ",
    "-o",
    "-x",
    "-fmodule-mapper",
    "-fdeps-file",
    "-fdeps-target",
}

SKIP_FLAGS = {
    "-c",
    "-E",
    "-MD",
    "-MMD",
    "-MP",
    "-fsyntax-only",
}

SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".c++",
    ".C",
    ".m",
    ".mm",
    ".ixx",
    ".cppm",
    ".cxxm",
    ".ccm",
}


def expand_response_args(args: list[str], cwd: Path) -> list[str]:
    expanded: list[str] = []
    for arg in args:
        if arg.startswith("@") and len(arg) > 1:
            response_path = Path(arg[1:])
            if not response_path.is_absolute():
                response_path = cwd / response_path
            try:
                expanded.extend(shlex.split(response_path.read_text()))
            except OSError:
                expanded.append(arg)
        else:
            expanded.append(arg)
    return expanded


def arg_value(arg: str, args: list[str], idx: int) -> tuple[str | None, int]:
    if arg in OPTIONS_WITH_VALUE:
        if idx + 1 >= len(args):
            return None, idx + 1
        return args[idx + 1], idx + 2
    return None, idx + 1


def collect_include_dirs(args: list[str], cwd: Path) -> list[Path]:
    include_dirs: list[Path] = []
    idx = 0
    while idx < len(args):
        arg = args[idx]
        if arg == "-I" or arg in {"-isystem", "-iquote", "-idirafter"}:
            value, idx = arg_value(arg, args, idx)
            if value:
                include_dirs.append(resolve_path(value, cwd))
            continue
        if arg.startswith("-I") and len(arg) > 2:
            include_dirs.append(resolve_path(arg[2:], cwd))
        elif arg.startswith("-isystem") and len(arg) > len("-isystem"):
            include_dirs.append(resolve_path(arg[len("-isystem"):], cwd))
        elif arg.startswith("-iquote") and len(arg) > len("-iquote"):
            include_dirs.append(resolve_path(arg[len("-iquote"):], cwd))
        elif arg.startswith("-idirafter") and len(arg) > len("-idirafter"):
            include_dirs.append(resolve_path(arg[len("-idirafter"):], cwd))
        idx += 1
    return include_dirs


def resolve_path(value: str, cwd: Path) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = cwd / path
    return path.resolve()


def find_module_mapper(args: list[str], cwd: Path) -> Path | None:
    idx = 0
    while idx < len(args):
        arg = args[idx]
        if arg.startswith("-fmodule-mapper="):
            value = arg.split("=", 1)[1]
            return resolve_path(value, cwd)
        if arg == "-fmodule-mapper":
            value, idx = arg_value(arg, args, idx)
            return resolve_path(value, cwd) if value else None
        idx += 1
    return None


def has_modules_ts(args: list[str]) -> bool:
    return any(arg == "-fmodules-ts" for arg in args)


def sanitized_object_name(header: str) -> str:
    return header.replace("/", "_").replace(".", "_") + ".o"


def cmi_path(cache_dir: Path, header_abs: Path) -> Path:
    relative_header = header_abs.relative_to("/")
    return (cache_dir / "gcm.cache" / relative_header).with_suffix(relative_header.suffix + ".gcm")


def mapper_entries(header: str, header_abs: Path, cmi: Path) -> list[str]:
    return [
        f"{header_abs} {cmi}",
        f"{header} {cmi}",
    ]


def buildable_flags(args: list[str]) -> list[str]:
    flags: list[str] = []
    idx = 0
    while idx < len(args):
        arg = args[idx]
        if arg.startswith("-fmodule-mapper=") or arg.startswith("-fmodule-output="):
            idx += 1
            continue
        if arg in OPTIONS_WITH_VALUE:
            value, next_idx = arg_value(arg, args, idx)
            skipped_value_options = {
                "-MF",
                "-MT",
                "-MQ",
                "-MJ",
                "-o",
                "-x",
                "-fmodule-mapper",
                "-fdeps-file",
                "-fdeps-target",
            }
            if arg not in skipped_value_options and value is not None:
                flags.extend([arg, value])
            idx = next_idx
            continue
        if arg.startswith("-MF") or arg.startswith("-MT") or arg.startswith("-MQ") or arg.startswith("-MJ"):
            idx += 1
            continue
        if arg.startswith("-fdeps-format") or arg.startswith("-fdeps-file=") or arg.startswith("-fdeps-target="):
            idx += 1
            continue
        if arg in SKIP_FLAGS:
            idx += 1
            continue
        if Path(arg).suffix in SOURCE_SUFFIXES:
            idx += 1
            continue
        flags.append(arg)
        idx += 1
    if "-fmodules-ts" not in flags:
        flags.append("-fmodules-ts")
    return flags


def highway_header_unit_flags(args: list[str], header_roots: list[Path]) -> list[str]:
    """Return a stable flag set for Highway header units.

    The generated CMI is shared by modules from several CMake targets. Target
    defines such as NO_POSTGRES or *_EXPORTS do not affect Highway, but they do
    change GCC's header-unit CRC if included in the build. Keep only flags that
    can affect Highway preprocessing, target selection, or libstdc++'s header
    configuration.
    """

    flags: list[str] = []
    optimization_flag_seen = False
    raw_flags = buildable_flags(args)
    idx = 0
    while idx < len(raw_flags):
        arg = raw_flags[idx]
        if arg in {"-I", "-isystem", "-iquote", "-idirafter"}:
            idx += 2
            continue
        if arg.startswith("-I") or arg.startswith("-isystem"):
            idx += 1
            continue
        if arg == "-D":
            if idx + 1 < len(raw_flags):
                define = raw_flags[idx + 1]
                define_name = define.split("=", 1)[0]
                if define.startswith(HEADER_UNIT_DEFINE_PREFIXES) or define_name in HEADER_UNIT_DEFINE_NAMES:
                    flags.extend([arg, define])
            idx += 2
            continue
        if arg.startswith("-D"):
            define = arg[2:]
            define_name = define.split("=", 1)[0]
            if define.startswith(HEADER_UNIT_DEFINE_PREFIXES) or define_name in HEADER_UNIT_DEFINE_NAMES:
                flags.append(arg)
            idx += 1
            continue
        if arg.startswith("-O"):
            # CMake appends source-level COMPILE_OPTIONS after the target flags.
            # Header units must stay compatible with module CMIs built from the
            # target flag set, so ignore later per-source optimization overrides.
            if not optimization_flag_seen:
                flags.append(arg)
                optimization_flag_seen = True
            idx += 1
            continue
        # Do not retain codegen/linkage-only flags such as -fPIC here. Shared
        # module BMIs and executable consumers must import the same Highway CMI.
        if (
            arg.startswith("-std=")
            or arg == "-fmodules-ts"
            or arg == "-fpermissive"
            or arg.startswith("-fabi-version")
            or arg.startswith("-Wno-")
            or arg.startswith(("-march=", "-mtune=", "-mcpu="))
            or arg.startswith(("-mavx", "-msse", "-mfma", "-mbmi", "-mlzcnt", "-mpopcnt"))
        ):
            flags.append(arg)
        idx += 1

    seen_roots: set[Path] = set()
    for root in header_roots:
        if root in seen_roots:
            continue
        seen_roots.add(root)
        flags.extend(["-isystem", str(root)])

    if "-fmodules-ts" not in flags:
        flags.append("-fmodules-ts")
    return flags


def fingerprint(parts: Iterable[str]) -> str:
    digest = hashlib.sha256()
    for part in parts:
        digest.update(part.encode("utf-8", "surrogateescape"))
        digest.update(b"\0")
    return digest.hexdigest()[:24]


def header_signature(header_paths: list[Path]) -> list[str]:
    signature: list[str] = []
    for path in header_paths:
        stat = path.stat()
        signature.append(f"{path}:{stat.st_mtime_ns}:{stat.st_size}")
    return signature


def mapper_entry_key(line: str) -> str | None:
    stripped = line.strip()
    if not stripped or stripped.startswith("$"):
        return None
    return stripped.split(maxsplit=1)[0]


def append_missing_mapper_entries(mapper_path: Path, lines: list[str]) -> None:
    existing = mapper_path.read_text().splitlines() if mapper_path.exists() else []
    replacement_keys = {
        key
        for line in lines
        if (key := mapper_entry_key(line)) is not None
    }
    retained = [
        line
        for line in existing
        if mapper_entry_key(line) not in replacement_keys
    ]
    retained_set = set(retained)
    missing = [line for line in lines if line not in retained_set]
    if not missing and retained == existing:
        return
    mapper_path.write_text("\n".join([*retained, *missing]) + "\n", encoding="utf-8")


def prebuild_header_units(
    compiler: str,
    parsed_args: list[str],
    cwd: Path,
    mapper_path: Path,
) -> None:
    include_dirs = collect_include_dirs(parsed_args, cwd)
    header_paths: list[Path] = []
    header_roots: list[Path] = []
    for header in HIGHWAY_HEADER_UNITS:
        for include_dir in include_dirs:
            candidate = include_dir / header
            if candidate.exists():
                header_paths.append(candidate.resolve())
                header_roots.append(include_dir)
                break
        else:
            return

    header_flags = highway_header_unit_flags(parsed_args, header_roots)
    cache_key = fingerprint([compiler, *header_flags, *header_signature(header_paths)])
    cache_dir = cwd / ".gcc-hwy-header-units" / cache_key
    cache_dir.mkdir(parents=True, exist_ok=True)

    lock_path = cache_dir / ".lock"
    with lock_path.open("w", encoding="utf-8") as lock_file:
        fcntl.flock(lock_file, fcntl.LOCK_EX)
        mapper_cache = cache_dir / "mapper.modmap"
        mapper_cache.write_text(f"$root {cache_dir}\n", encoding="utf-8")

        built_entries: list[str] = []
        for header, header_abs in zip(HIGHWAY_HEADER_UNITS, header_paths):
            cmi = cmi_path(cache_dir, header_abs)
            cmi.parent.mkdir(parents=True, exist_ok=True)
            entries = mapper_entries(header, header_abs, cmi)
            header_mapper = cache_dir / f"mapper-{sanitized_object_name(header)}.modmap"
            header_mapper.write_text(
                f"$root {cache_dir}\n" + "\n".join([*built_entries, *entries]) + "\n",
                encoding="utf-8",
            )

            if not cmi.exists():
                log_path = cache_dir / f"{sanitized_object_name(header)}.log"
                command = [
                    compiler,
                    *header_flags,
                    f"-fmodule-mapper={header_mapper}",
                    "-x",
                    "c++-system-header",
                    header,
                    "-c",
                    "-o",
                    str(cache_dir / sanitized_object_name(header)),
                ]
                result = subprocess.run(
                    command,
                    cwd=cwd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                )
                log_path.write_text(result.stdout + result.stderr, encoding="utf-8")
                if result.returncode != 0:
                    sys.stderr.write(
                        f"bioseq GCC Highway header-unit build failed for {header}; "
                        f"see {log_path}\n"
                    )
                    sys.stderr.write(result.stderr)
                    sys.exit(result.returncode)

            built_entries.extend(entries)

        mapper_cache.write_text(f"$root {cache_dir}\n" + "\n".join(built_entries) + "\n", encoding="utf-8")
        append_missing_mapper_entries(mapper_path, built_entries)


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(1)

    compiler = sys.argv[1]
    original_args = sys.argv[2:]
    cwd = Path.cwd()
    parsed_args = expand_response_args(original_args, cwd)

    mapper_path = find_module_mapper(parsed_args, cwd)
    if mapper_path is not None and has_modules_ts(parsed_args):
        prebuild_header_units(compiler, parsed_args, cwd, mapper_path)

    os.execv(compiler, [compiler, *original_args])


if __name__ == "__main__":
    main()
