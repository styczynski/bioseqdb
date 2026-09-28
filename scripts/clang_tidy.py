#!/usr/bin/env python3
"""Run clang-tidy against authenticated CMake C++ module commands."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Sequence, TextIO


_COMPILE_DATABASE_NAME = "compile_commands.json"
_CONFIG_NAME = ".clang-tidy"
_GENERATED_COMPONENT = "generated"
_SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cppm", ".cxx"})
_EXCLUDED_SOURCE_PREFIXES = (
    ".git/",
    "DOCS/",
    "bench/tool_minimap2/minimap2/",
    "bench/tool_sneakysnake/SneakySnake/",
    "main/papers/",
    "papers/",
    "vendor/",
)
_EXCLUDED_BUILD_COMPONENTS = frozenset({"_deps", "vcpkg_installed"})
_MAX_OWNER_CANDIDATES = 16
_DEFAULT_JOBS = 2
_DEFAULT_UNIT_TIMEOUT_SECONDS = 600
_DIAGNOSTIC_PATTERN = re.compile(
    r"^(?P<path>.+?):(?P<line>\d+):(?P<column>\d+): "
    r"(?P<severity>warning|error): (?P<message>.*?)(?: \[(?P<check>[^]]+)\])?$"
)
_VERSION_PATTERN = re.compile(r"(?:clang|LLVM) version\s+(?P<major>\d+)")
_GENERATOR_COMPILE_DATABASE_PATTERN = re.compile(
    r"codegen/generator-build(?:-[a-z0-9][a-z0-9-]*)?/compile_commands\.json"
)
_NOLINT_COMMENT_PATTERN = re.compile(
    r"^(?P<kind>NOLINT(?:NEXTLINE|BEGIN|END)?)"
    r"\((?P<checks>[^)]*)\)\s*(?:--|:)\s*(?P<reason>\S.*?)\s*$"
)
_NOLINT_TOKEN_PATTERN = re.compile(r"\bNOLINT(?:NEXTLINE|BEGIN|END)?\b")
_CHECK_NAME_PATTERN = re.compile(r"^[a-z0-9_.-]+$")

_NAMED_MODULE_DISABLED_CHECKS = (
    "misc-include-cleaner",
    "misc-use-anonymous-namespace",
    "misc-use-internal-linkage",
)
_POSTGRES_BOUNDARY_DISABLED_CHECKS = (
    "bugprone-macro-parentheses",
    "bugprone-macro-repeated-side-effects",
    "bugprone-multiple-statement-macro",
    "bugprone-reserved-identifier",
    "bugprone-unsafe-functions",
    "cert-dcl37-c",
    "cert-dcl50-cpp",
    "cert-dcl51-cpp",
    "cppcoreguidelines-avoid-c-arrays",
    "cppcoreguidelines-avoid-non-const-global-variables",
    "cppcoreguidelines-macro-usage",
    "cppcoreguidelines-no-malloc",
    "cppcoreguidelines-owning-memory",
    "cppcoreguidelines-pro-bounds-array-to-pointer-decay",
    "cppcoreguidelines-pro-bounds-avoid-unchecked-container-access",
    "cppcoreguidelines-pro-bounds-constant-array-index",
    "cppcoreguidelines-pro-bounds-pointer-arithmetic",
    "cppcoreguidelines-pro-type-reinterpret-cast",
    "cppcoreguidelines-pro-type-vararg",
    "modernize-avoid-c-arrays",
    "modernize-avoid-variadic-functions",
    "misc-unused-parameters",
    "readability-identifier-naming",
)
_GENERATED_HIGHWAY_DISABLED_CHECKS = (
    "bugprone-reserved-identifier",
    "cert-dcl37-c",
    "cert-dcl51-cpp",
    "cppcoreguidelines-avoid-c-arrays",
    "cppcoreguidelines-avoid-non-const-global-variables",
    "cppcoreguidelines-macro-usage",
    "cppcoreguidelines-owning-memory",
    "cppcoreguidelines-pro-bounds-array-to-pointer-decay",
    "cppcoreguidelines-pro-bounds-pointer-arithmetic",
    "modernize-avoid-c-arrays",
    "misc-unused-parameters",
    "portability-simd-intrinsics",
    "readability-identifier-length",
    "readability-identifier-naming",
    "readability-magic-numbers",
)
_HOT_PATH_DISABLED_CHECKS = (
    "cppcoreguidelines-avoid-c-arrays",
    "cppcoreguidelines-avoid-magic-numbers",
    "cppcoreguidelines-no-malloc",
    "cppcoreguidelines-owning-memory",
    "cppcoreguidelines-pro-bounds-array-to-pointer-decay",
    "cppcoreguidelines-pro-bounds-avoid-unchecked-container-access",
    "cppcoreguidelines-pro-bounds-constant-array-index",
    "cppcoreguidelines-pro-bounds-pointer-arithmetic",
    "cppcoreguidelines-pro-type-reinterpret-cast",
    "modernize-avoid-c-arrays",
    "modernize-loop-convert",
    "modernize-make-shared",
    "modernize-make-unique",
    "modernize-min-max-use-initializer-list",
    "modernize-pass-by-value",
    "modernize-use-ranges",
    "readability-magic-numbers",
    "readability-use-anyofallof",
    "readability-use-std-min-max",
)


@dataclass(frozen=True, slots=True)
class CompileCommand:
    """Describe one exact CMake compilation of an owned translation unit."""

    source: Path
    directory: Path
    compiler: Path
    compiler_arguments: tuple[str, ...]
    display_path: str
    is_generated: bool
    variant: int
    compile_database: str = "compile_commands.json"
    response_files: tuple[tuple[str, str], ...] = ()


@dataclass(frozen=True, slots=True)
class LintResult:
    """Capture one clang-tidy process result and its parsed diagnostics."""

    command: CompileCommand
    invocation: tuple[str, ...]
    exit_code: int
    output: str
    duration_seconds: float
    diagnostics: tuple[dict[str, object], ...]
    ninja_owner_candidates: tuple[str, ...]
    heuristic_owner_candidates: tuple[str, ...]
    owner_query_status: str | None
    generator_hint: str | None
    source_suppressions: tuple[dict[str, object], ...]


@dataclass(frozen=True, slots=True)
class GeneratedOwnerRouting:
    """Describe generated-source routing and its Ninja-query evidence."""

    ninja_candidates: tuple[str, ...]
    heuristic_candidates: tuple[str, ...]
    query_status: str


class ClangTidyError(RuntimeError):
    """Report invalid lint inputs, toolchains, or result artifacts."""


def _subprocess_environment() -> dict[str, str]:
    """Return the inherited builder environment with stable diagnostics."""

    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    environment["LANG"] = "C"
    return environment


def _is_relative_to(path: Path, parent: Path) -> bool:
    """Return whether *path* resolves below *parent* without raising."""

    try:
        path.relative_to(parent)
    except ValueError:
        return False
    return True


def _relative_display_path(source: Path, source_root: Path, build_dir: Path) -> str:
    """Return a stable source-root or build-root-relative diagnostic path."""

    if _is_relative_to(source, build_dir):
        return source.relative_to(build_dir).as_posix()
    return source.relative_to(source_root).as_posix()


def _is_excluded_source_path(relative_path: str) -> bool:
    """Return whether an authored path belongs to protected or vendor content."""

    return any(relative_path.startswith(prefix) for prefix in _EXCLUDED_SOURCE_PREFIXES)


def _classify_source(source: Path, source_root: Path, build_dir: Path) -> bool | None:
    """Classify a compile unit as generated, authored, or excluded.

    @return ``True`` for generated build-tree code, ``False`` for authored
        repository code, and ``None`` for toolchain, vendor, or protected code.
    """

    if source.suffix not in _SOURCE_SUFFIXES:
        return None
    if _is_relative_to(source, build_dir):
        relative = source.relative_to(build_dir)
        if any(component in _EXCLUDED_BUILD_COMPONENTS for component in relative.parts):
            return None
        if _GENERATED_COMPONENT not in relative.parts:
            return None
        return True
    if not _is_relative_to(source, source_root):
        return None
    relative_text = source.relative_to(source_root).as_posix()
    if _is_excluded_source_path(relative_text) or relative_text.startswith("build/"):
        return None
    return False


def _split_compile_arguments(entry: dict[str, object]) -> list[str]:
    """Read either standard compile-database argument representation."""

    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(value, str) for value in arguments):
        return list(arguments)
    command = entry.get("command")
    if isinstance(command, str):
        return shlex.split(command)
    raise ValueError("compile command has neither string command nor string arguments")


def _compiler_index(arguments: list[str]) -> int:
    """Locate the Clang driver after any compiler launcher arguments."""

    matches = [
        index
        for index, argument in enumerate(arguments)
        if Path(argument).name in {"clang", "clang++"}
    ]
    if not matches:
        raise ValueError("compile command does not use a Clang driver")
    return matches[-1]


def _strip_driver_outputs(
    arguments: list[str],
    *,
    source: Path,
    directory: Path,
) -> tuple[Path, tuple[str, ...]]:
    """Return the compiler and semantic flags clang-tidy must replay."""

    compiler_position = _compiler_index(arguments)
    compiler = Path(arguments[compiler_position])
    if not compiler.is_absolute():
        compiler = directory / compiler
    compiler = compiler.resolve(strict=False)
    source_resolved = source.resolve(strict=False)
    result: list[str] = []
    index = compiler_position + 1
    paired_output_flags = {"-o", "-MF", "-MT", "-MQ", "--serialize-diagnostics"}
    joined_output_prefixes = ("-o", "-MF", "-MT", "-MQ", "--serialize-diagnostics=")
    standalone_output_flags = {
        "-c",
        "-MD",
        "-MMD",
        "-MP",
        "-fcolor-diagnostics",
        "-fdiagnostics-color",
        "-fno-color-diagnostics",
    }
    while index < len(arguments):
        argument = arguments[index]
        if argument in paired_output_flags:
            index += 2
            continue
        if argument in standalone_output_flags:
            index += 1
            continue
        if any(argument.startswith(prefix) for prefix in joined_output_prefixes):
            index += 1
            continue
        if argument.startswith("-fdiagnostics-color="):
            index += 1
            continue
        candidate = Path(argument)
        if not candidate.is_absolute():
            candidate = directory / candidate
        if candidate.resolve(strict=False) == source_resolved:
            index += 1
            continue
        result.append(argument)
        index += 1
    return compiler, tuple(result)


def _expanded_response_arguments(
    arguments: tuple[str, ...],
    *,
    directory: Path,
    source_root: Path,
) -> tuple[tuple[str, ...], tuple[tuple[str, str], ...]]:
    """Expand authenticated Clang response files for profile validation."""

    source_root = source_root.resolve(strict=True)
    evidence: dict[Path, str] = {}

    def expand(
        nested_arguments: tuple[str, ...],
        active_response_files: frozenset[Path],
    ) -> list[str]:
        expanded: list[str] = []
        for argument in nested_arguments:
            if not argument.startswith("@") or len(argument) == 1:
                expanded.append(argument)
                continue
            response_path = Path(argument[1:])
            if not response_path.is_absolute():
                response_path = directory / response_path
            try:
                response_path = response_path.resolve(strict=True)
            except OSError as error:
                raise ClangTidyError(
                    f"compile response file is unavailable: {argument}: {error}"
                ) from error
            if not _is_relative_to(response_path, source_root):
                raise ClangTidyError(
                    "compile response file escapes the repository: "
                    f"{response_path}"
                )
            if not response_path.is_file():
                raise ClangTidyError(
                    f"compile response path is not a regular file: {response_path}"
                )
            if response_path in active_response_files:
                raise ClangTidyError(
                    f"compile response files contain a cycle at {response_path}"
                )
            try:
                response_bytes = response_path.read_bytes()
                response_text = response_bytes.decode("utf-8")
                response_arguments = tuple(shlex.split(response_text, posix=True))
            except (OSError, UnicodeDecodeError, ValueError) as error:
                raise ClangTidyError(
                    f"cannot parse compile response file {response_path}: {error}"
                ) from error
            evidence[response_path] = hashlib.sha256(response_bytes).hexdigest()
            expanded.extend(
                expand(
                    response_arguments,
                    active_response_files | frozenset({response_path}),
                )
            )
        return expanded

    expanded_arguments = tuple(expand(arguments, frozenset()))
    response_evidence = tuple(
        (path.relative_to(source_root).as_posix(), digest)
        for path, digest in sorted(evidence.items())
    )
    return expanded_arguments, response_evidence


def _validate_compile_profile(source: Path, arguments: tuple[str, ...]) -> None:
    """Require the exact optimized, assertion-free profile used in production.

    Lint is semantic analysis of the actual production/test module graph.  A
    debug or otherwise deoptimized compile command can expose a different AST
    and macro surface, so silently accepting one would invalidate the report.
    """

    optimization_flags = tuple(
        argument for argument in arguments if argument.startswith("-O")
    )
    if optimization_flags != ("-O3",):
        raise ClangTidyError(
            "owned lint compile command must contain exactly one -O3 and no "
            f"competing optimization flags: {source}: {optimization_flags}"
        )
    ndebug_flags = tuple(
        argument
        for argument in arguments
        if argument == "-DNDEBUG" or argument.startswith("-DNDEBUG=")
    )
    ndebug_undefines = tuple(
        argument
        for argument in arguments
        if argument == "-UNDEBUG" or argument.startswith("-UNDEBUG=")
    )
    if len(ndebug_flags) != 1 or ndebug_undefines:
        raise ClangTidyError(
            "owned lint compile command must contain exactly one NDEBUG definition "
            f"and no NDEBUG undefinition: {source}: definitions={ndebug_flags} "
            f"undefinitions={ndebug_undefines}"
        )


def _compile_database_paths(build_dir: Path) -> tuple[Path, ...]:
    """Return the parent and first-party nested generator databases."""

    primary = build_dir / _COMPILE_DATABASE_NAME
    nested = sorted(
        path
        for path in (build_dir / "codegen").glob(
            f"generator-build*/{_COMPILE_DATABASE_NAME}"
        )
        if path.is_file()
    )
    return (primary, *nested)


def _compile_database_evidence(build_dir: Path) -> list[dict[str, str]]:
    """Hash every database whose commands contribute to the lint scope."""

    evidence: list[dict[str, str]] = []
    for path in _compile_database_paths(build_dir):
        evidence.append(
            {
                "path": path.relative_to(build_dir).as_posix(),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        )
    return evidence


def _load_compile_commands(
    source_root: Path, build_dir: Path
) -> tuple[CompileCommand, ...]:
    """Load and classify all unique owned CMake compile commands."""

    commands: list[CompileCommand] = []
    seen: set[tuple[Path, Path, Path, tuple[str, ...]]] = set()
    variant_counts: dict[Path, int] = {}
    for database_path in _compile_database_paths(build_dir):
        database_relative_path = database_path.relative_to(build_dir).as_posix()
        try:
            document = json.loads(database_path.read_text(encoding="utf-8"))
        except FileNotFoundError as error:
            raise ClangTidyError(
                f"compile database does not exist: {database_path}"
            ) from error
        except json.JSONDecodeError as error:
            raise ClangTidyError(
                f"compile database is invalid JSON: {database_path}: {error}"
            ) from error
        if not isinstance(document, list):
            raise ClangTidyError(
                f"compile database root must be an array: {database_path}"
            )
        for raw_entry in document:
            if not isinstance(raw_entry, dict):
                raise ClangTidyError(
                    f"compile database entries must be objects: {database_path}"
                )
            raw_file = raw_entry.get("file")
            raw_directory = raw_entry.get("directory")
            if not isinstance(raw_file, str) or not isinstance(raw_directory, str):
                raise ClangTidyError(
                    "compile database entry requires string file and directory: "
                    f"{database_path}"
                )
            directory = Path(raw_directory).resolve(strict=False)
            source = Path(raw_file)
            if not source.is_absolute():
                source = directory / source
            source = source.resolve(strict=False)
            classification = _classify_source(source, source_root, build_dir)
            if classification is None:
                continue
            try:
                compiler, compiler_arguments = _strip_driver_outputs(
                    _split_compile_arguments(raw_entry),
                    source=source,
                    directory=directory,
                )
            except ValueError as error:
                raise ClangTidyError(
                    "owned compile command cannot be replayed by clang-tidy: "
                    f"{source} in {database_path}: {error}"
                ) from error
            expanded_arguments, response_files = _expanded_response_arguments(
                compiler_arguments,
                directory=directory,
                source_root=source_root,
            )
            _validate_compile_profile(source, expanded_arguments)
            identity = (source, directory, compiler, compiler_arguments)
            if identity in seen:
                continue
            seen.add(identity)
            variant = variant_counts.get(source, 0)
            variant_counts[source] = variant + 1
            commands.append(
                CompileCommand(
                    source=source,
                    directory=directory,
                    compiler=compiler,
                    compiler_arguments=compiler_arguments,
                    display_path=_relative_display_path(
                        source, source_root, build_dir
                    ),
                    is_generated=classification,
                    variant=variant,
                    compile_database=database_relative_path,
                    response_files=response_files,
                )
            )
    ordered = tuple(
        sorted(commands, key=lambda command: (command.display_path, command.variant))
    )
    display_owners: dict[str, tuple[Path, bool]] = {}
    for command in ordered:
        owner = (command.source, command.is_generated)
        previous = display_owners.setdefault(command.display_path, owner)
        if previous != owner:
            raise ClangTidyError(
                "owned compile commands have an ambiguous report path: "
                f"{command.display_path}: {previous} versus {owner}"
            )
    return ordered


def _validate_focused_path(raw_path: str) -> str:
    """Validate and normalize one repository- or build-relative focused path."""

    if not raw_path or "\\" in raw_path or any(ord(character) < 32 for character in raw_path):
        raise ValueError(f"invalid focused path {raw_path!r}")
    path = PurePosixPath(raw_path)
    if path.is_absolute() or any(component in {"", ".", ".."} for component in path.parts):
        raise ValueError(f"focused path must be normalized and relative: {raw_path!r}")
    if path.suffix not in _SOURCE_SUFFIXES:
        raise ValueError(f"focused path must name a C/C++ compile unit: {raw_path!r}")
    return path.as_posix()


def _select_commands(
    commands: tuple[CompileCommand, ...],
    *,
    scope: str,
    focused_paths: tuple[str, ...],
) -> tuple[CompileCommand, ...]:
    """Select all owned commands or exact requested focused compile units."""

    if scope == "full":
        if focused_paths:
            raise ValueError("full scope does not accept focused paths")
        return commands
    if scope != "focused":
        raise ValueError(f"unsupported lint scope {scope!r}")
    if not focused_paths:
        raise ValueError("focused scope requires at least one path")
    normalized = tuple(_validate_focused_path(path) for path in focused_paths)
    requested = set(normalized)
    selected = tuple(command for command in commands if command.display_path in requested)
    found = {command.display_path for command in selected}
    missing = sorted(requested - found)
    if missing:
        raise ValueError(
            "focused paths have no owned compile command: " + ", ".join(missing)
        )
    return selected


def _tool_major_version(executable: Path) -> tuple[int, str]:
    """Return a Clang-family tool's major version and first version line."""

    completed = subprocess.run(
        [str(executable), "--version"],
        cwd=executable.parent,
        env=_subprocess_environment(),
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )
    output = (completed.stdout + completed.stderr).strip()
    match = _VERSION_PATTERN.search(output)
    if completed.returncode != 0 or match is None:
        raise ClangTidyError(
            f"cannot authenticate tool version for {executable}: {output}"
        )
    first_line = output.splitlines()[0] if output else ""
    return int(match.group("major")), first_line


def _matching_clang_tidy(
    commands: tuple[CompileCommand, ...],
) -> tuple[Path, dict[str, object]]:
    """Resolve clang-tidy beside the compiler and reject BMI-major mismatches."""

    compilers = sorted({command.compiler for command in commands})
    if not compilers:
        raise ClangTidyError("no owned Clang compile commands were selected")
    compiler_versions = {
        compiler: _tool_major_version(compiler) for compiler in compilers
    }
    compiler_majors = {version[0] for version in compiler_versions.values()}
    if len(compiler_majors) != 1:
        details = ", ".join(
            f"{compiler}={version[1]}" for compiler, version in compiler_versions.items()
        )
        raise ClangTidyError(f"selected compile commands mix Clang majors: {details}")
    primary_compiler = compilers[0]
    compiler_major = next(iter(compiler_majors))
    tidy = primary_compiler.with_name("clang-tidy")
    if not tidy.is_file() or not os.access(tidy, os.X_OK):
        raise ClangTidyError(
            f"matching clang-tidy is missing or not executable beside compile driver "
            f"{primary_compiler}: expected {tidy}. Install the LLVM "
            f"{compiler_major} tools in the same prefix: on Ubuntu with the "
            f"apt.llvm.org repository, apt-get install clang-tidy-{compiler_major} "
            f"clang-tools-{compiler_major}; on macOS, brew install "
            f"llvm@{compiler_major}."
        )
    tidy_major, tidy_version = _tool_major_version(tidy)
    if tidy_major != compiler_major:
        raise ClangTidyError(
            "clang-tidy major must match the compile-command Clang major for C++ module "
            f"BMIs: compiler={compiler_major}, clang-tidy={tidy_major}"
        )
    return tidy, {
        "clang_tidy": tidy_version,
        "compilers": [version[1] for version in compiler_versions.values()],
        "major": compiler_major,
    }


def _verify_config(tidy: Path, config_path: Path) -> str:
    """Ask the matched clang-tidy binary to validate the root policy."""

    completed = subprocess.run(
        [str(tidy), f"--config-file={config_path}", "--verify-config"],
        cwd=config_path.parent,
        env=_subprocess_environment(),
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )
    verification = (completed.stdout + completed.stderr).strip()
    if completed.returncode != 0:
        raise ClangTidyError(
            f"matched clang-tidy rejected {config_path}: {verification}"
        )
    return verification


def _path_has_fragment(path: str, fragments: tuple[str, ...]) -> bool:
    """Return whether a normalized path contains one of several semantic areas."""

    surrounded = f"/{path.strip('/')}/"
    return any(fragment in surrounded for fragment in fragments)


def _suppression_profiles(command: CompileCommand) -> tuple[str, ...]:
    """Name every narrow exception profile applied to one compile unit."""

    path = command.display_path
    profiles: list[str] = []
    if command.source.suffix == ".cppm":
        profiles.append("named_module")
    if _path_has_fragment(
        path,
        (
            "/backend/pg/",
            "/bioseq_caching/pg/",
            "/common/postgres/",
            "/generated/bioseq/sql/",
            "/generated/bioseq_cache/sql/",
        ),
    ):
        profiles.append("postgres_abi")
    if command.is_generated and (
        "/generated/highway/" in f"/{path}" or "highway-clang-canary/generated" in path
    ):
        profiles.extend(("generated_highway", "zero_cost_hot_path"))
    if _path_has_fragment(
        path,
        (
            "/backend/alignment_hwy/",
            "/backend/common/hashing/",
            "/backend/common/simd/",
            "/backend/gpu/",
            "/backend/pipeline/",
            "/backend/pg/scheduler/",
            "/backend/pg/storage/indexing/",
            "/backend/pg/vectorization/",
            "/backend/sequences/chaining/",
            "/backend/sequences/highway_sequence_ops.cppm",
            "/backend/sequences/indexing/",
            "/backend/sequences/kmers/",
            "/backend/sequences/packed_sequence.cppm",
            "/backend/sequences/packed_sequence_parts/",
            "/backend/sequences/prefilter/",
            "/backend/sequences/rank_pack",
            "/backend/sequences/simd",
            "/backend/vector_compare/",
            "/generated/template_flatten/",
        ),
    ):
        profiles.append("zero_cost_hot_path")
    return tuple(dict.fromkeys(profiles))


def _disabled_checks(command: CompileCommand) -> tuple[str, ...]:
    """Return narrow policy exceptions required by the source boundary."""

    profile_checks = {
        "named_module": _NAMED_MODULE_DISABLED_CHECKS,
        "postgres_abi": _POSTGRES_BOUNDARY_DISABLED_CHECKS,
        "generated_highway": _GENERATED_HIGHWAY_DISABLED_CHECKS,
        "zero_cost_hot_path": _HOT_PATH_DISABLED_CHECKS,
    }
    disabled = {
        check
        for profile in _suppression_profiles(command)
        for check in profile_checks[profile]
    }
    # The C++ vectorization layer keeps the PostgreSQL ABI exceptions but owns
    # its identifiers and remains subject to the repository naming convention.
    if _path_has_fragment(command.display_path, ("/backend/pg/vectorization/",)):
        disabled.discard("readability-identifier-naming")
    return tuple(sorted(disabled))


def _normalized_comment_line(line: str) -> str:
    """Remove ordinary line/block documentation decoration from one comment."""

    normalized = line.strip()
    if normalized.startswith(("*", "/", "!")):
        normalized = normalized[1:].lstrip()
    return normalized


def _raw_string_end(document: str, start: int) -> int | None:
    """Return the exclusive end of a C++ raw string beginning at ``R\"``."""

    if not document.startswith('R"', start):
        return None
    delimiter_start = start + 2
    opening = document.find("(", delimiter_start, delimiter_start + 17)
    if opening == -1:
        return None
    delimiter = document[delimiter_start:opening]
    if any(character.isspace() or character in "\\()" for character in delimiter):
        return None
    terminator = f'){delimiter}"'
    closing = document.find(terminator, opening + 1)
    return len(document) if closing == -1 else closing + len(terminator)


def _comment_lines(document: str) -> tuple[tuple[int, str], ...]:
    """Lex C++ line and block comments while excluding string/character literals."""

    comments: list[tuple[int, str]] = []
    index = 0
    line_number = 1
    while index < len(document):
        character = document[index]
        if character == "\n":
            line_number += 1
            index += 1
            continue
        raw_end = _raw_string_end(document, index)
        if raw_end is not None:
            line_number += document[index:raw_end].count("\n")
            index = raw_end
            continue
        if character in {"'", '"'}:
            quote = character
            index += 1
            while index < len(document):
                current = document[index]
                if current == "\n":
                    line_number += 1
                if current == "\\" and index + 1 < len(document):
                    if document[index + 1] == "\n":
                        line_number += 1
                    index += 2
                    continue
                index += 1
                if current == quote:
                    break
            continue
        if document.startswith("//", index):
            end = document.find("\n", index + 2)
            if end == -1:
                end = len(document)
            comments.append(
                (line_number, _normalized_comment_line(document[index + 2:end]))
            )
            index = end
            continue
        if document.startswith("/*", index):
            end = document.find("*/", index + 2)
            body_end = len(document) if end == -1 else end
            body = document[index + 2:body_end]
            for offset, line in enumerate(body.split("\n")):
                comments.append(
                    (line_number + offset, _normalized_comment_line(line))
                )
            line_number += body.count("\n")
            index = len(document) if end == -1 else end + 2
            continue
        index += 1
    return tuple(comments)


def _source_suppressions(source: Path) -> tuple[dict[str, object], ...]:
    """Return explicit, explained source suppressions and reject broad forms."""

    suppressions: list[dict[str, object]] = []
    try:
        document = source.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise ClangTidyError(
            f"cannot read selected compile unit {source}: {error}"
        ) from error
    region_stack: list[tuple[int, tuple[str, ...]]] = []
    for line_number, comment in _comment_lines(document):
        if _NOLINT_TOKEN_PATTERN.search(comment) is None:
            continue
        match = _NOLINT_COMMENT_PATTERN.fullmatch(comment)
        if match is None:
            raise ClangTidyError(
                f"broad or malformed clang-tidy suppression in {source}:{line_number}; "
                "name exact checks and explain the boundary"
            )
        checks = tuple(
            check.strip() for check in match.group("checks").split(",")
        )
        if (
            not checks
            or any(not check for check in checks)
            or len(set(checks)) != len(checks)
            or any(
                "*" in check or _CHECK_NAME_PATTERN.fullmatch(check) is None
                for check in checks
            )
        ):
            raise ClangTidyError(
                f"clang-tidy suppression must name exact checks in "
                f"{source}:{line_number}: {checks}"
            )
        reason = match.group("reason")
        kind = match.group("kind")
        if kind == "NOLINTBEGIN":
            region_stack.append((line_number, checks))
        elif kind == "NOLINTEND":
            if not region_stack:
                raise ClangTidyError(
                    f"clang-tidy NOLINTEND has no matching NOLINTBEGIN in "
                    f"{source}:{line_number}"
                )
            begin_line, begin_checks = region_stack.pop()
            if checks != begin_checks:
                raise ClangTidyError(
                    "clang-tidy suppression region check lists differ in "
                    f"{source}:{begin_line} and {source}:{line_number}"
                )
        suppressions.append(
            {
                "line": line_number,
                "kind": kind,
                "checks": list(checks),
                "reason": reason,
            }
        )
    if region_stack:
        begin_line, _ = region_stack[-1]
        raise ClangTidyError(
            f"clang-tidy NOLINTBEGIN has no matching NOLINTEND in "
            f"{source}:{begin_line}"
        )
    return tuple(suppressions)


def _line_filter(source: Path) -> str:
    """Return an exact whole-file filter for one authored or generated unit."""

    try:
        with source.open("r", encoding="utf-8", errors="replace") as handle:
            line_count = sum(1 for _ in handle)
    except OSError as error:
        raise ClangTidyError(f"cannot read selected compile unit {source}: {error}") from error
    document = [{"name": str(source), "lines": [[1, max(1, line_count)]]}]
    return json.dumps(document, separators=(",", ":"))


def _clang_tidy_invocation(
    tidy: Path,
    command: CompileCommand,
    *,
    source_root: Path,
) -> tuple[str, ...]:
    """Construct one exact non-mutating clang-tidy invocation."""

    invocation = [
        str(tidy),
        str(command.source),
        f"--config-file={source_root / _CONFIG_NAME}",
        "--header-filter=^$",
        f"--line-filter={_line_filter(command.source)}",
        "--enable-module-headers-parsing",
        "--use-color=false",
    ]
    disabled = _disabled_checks(command)
    checks_overlay = ["clang-analyzer-core.*"]
    checks_overlay.extend(f"-{check}" for check in disabled)
    invocation.append(f"--checks={','.join(checks_overlay)}")
    invocation.extend(("--", *command.compiler_arguments))
    return tuple(invocation)


def _parse_diagnostics(output: str) -> tuple[dict[str, object], ...]:
    """Parse stable diagnostic coordinates while retaining unparsed tool output."""

    diagnostics: list[dict[str, object]] = []
    for line in output.splitlines():
        match = _DIAGNOSTIC_PATTERN.match(line)
        if match is None:
            continue
        diagnostics.append(
            {
                "path": match.group("path"),
                "line": int(match.group("line")),
                "column": int(match.group("column")),
                "severity": match.group("severity"),
                "message": match.group("message"),
                "check": match.group("check"),
            }
        )
    return tuple(diagnostics)


def _generator_hint(source: Path) -> str | None:
    """Read a bounded generated-file banner without treating it as authority."""

    try:
        with source.open("r", encoding="utf-8", errors="replace") as handle:
            for _ in range(80):
                line = handle.readline()
                if not line:
                    break
                if "Generated by " in line or "generated by " in line:
                    return line.strip().lstrip("/").strip()
    except OSError:
        return None
    return None


def _heuristic_owner_candidates(display_path: str) -> tuple[str, ...]:
    """Return explicitly identified semantic generator owners as heuristics."""

    if "highway-clang-canary/generated/" in display_path:
        return (
            "codegen/namespace_materialization/tests/integration/"
            "module_canary/CMakeLists.txt",
        )
    if "/highway/" in f"/{display_path}":
        return (
            "cmake/bioseq_highway_codegen.cmake",
            "codegen/highway/index.cppm",
        )
    if "/template_flatten/" in f"/{display_path}":
        return (
            "cmake/bioseq_template_flatten_codegen.cmake",
            "codegen/namespace_materialization/index.cppm",
        )
    if "/sql/" in f"/{display_path}":
        return ("cmake/bioseq_sql_codegen.cmake", "codegen/sql/index.cppm")
    if "/build_config/" in f"/{display_path}":
        return (
            "cmake/bioseq_build_config_codegen.cmake",
            "codegen/build_config/index.cppm",
        )
    if "namespace_materialization/tests/integration/module_canary/generated/" in display_path:
        return (
            "codegen/namespace_materialization/tests/integration/module_canary/CMakeLists.txt",
            "codegen/namespace_materialization/index.cppm",
        )
    if "namespace_materialization/tests/integration/owner_canary/generated/" in display_path:
        return (
            "codegen/namespace_materialization/tests/integration/owner_canary/CMakeLists.txt",
            "codegen/namespace_materialization/index.cppm",
        )
    if (
        display_path.startswith("codegen/generator-build")
        and "/generated/" in display_path
    ):
        return ("codegen/CMakeLists.txt",)
    if "/external/" in f"/{display_path}":
        return ("codegen/external/index.cppm",)
    return ()


def _ninja_build_directory(command: CompileCommand, build_dir: Path) -> Path:
    """Find the nearest nested Ninja graph that owns a compile command."""

    candidate = command.directory
    while _is_relative_to(candidate, build_dir):
        if (candidate / "build.ninja").is_file():
            return candidate
        if candidate == build_dir:
            break
        candidate = candidate.parent
    return build_dir


def _generated_owner_candidates(
    command: CompileCommand,
    *,
    source_root: Path,
    build_dir: Path,
) -> GeneratedOwnerRouting:
    """Resolve generated diagnostics to direct authored Ninja dependencies."""

    if not command.is_generated:
        return GeneratedOwnerRouting(
            ninja_candidates=(),
            heuristic_candidates=(),
            query_status="not-generated",
        )
    heuristic_candidates = _heuristic_owner_candidates(command.display_path)
    ninja_candidates: list[str] = []
    seen: set[str] = set()
    query_status = "not-run"
    try:
        ninja_build_dir = _ninja_build_directory(command, build_dir)
        relative_target = command.source.relative_to(ninja_build_dir).as_posix()
        completed = subprocess.run(
            ["ninja", "-C", str(ninja_build_dir), "-t", "query", relative_target],
            cwd=source_root,
            env=_subprocess_environment(),
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
        if completed.returncode == 0:
            authored_input_count = 0
            in_inputs = False
            for raw_line in completed.stdout.splitlines():
                stripped = raw_line.strip()
                if stripped == "input: CUSTOM_COMMAND":
                    in_inputs = True
                    continue
                if stripped == "outputs:":
                    break
                if not in_inputs or not stripped or stripped in {"CUSTOM_COMMAND", "||", "|"}:
                    continue
                if stripped.startswith(("|| ", "| ")):
                    stripped = stripped.split(maxsplit=1)[1]
                candidate = Path(stripped)
                if not candidate.is_absolute():
                    candidate = ninja_build_dir / candidate
                candidate = candidate.resolve(strict=False)
                if _is_relative_to(candidate, build_dir) or not _is_relative_to(
                    candidate, source_root
                ):
                    continue
                relative = candidate.relative_to(source_root).as_posix()
                if _is_excluded_source_path(relative):
                    continue
                authored_input_count += 1
                if relative not in seen:
                    ninja_candidates.append(relative)
                    seen.add(relative)
            query_status = (
                "ninja-authored-inputs"
                if authored_input_count
                else "ninja-no-authored-inputs"
            )
        else:
            query_status = f"ninja-exit-{completed.returncode}"
    except subprocess.TimeoutExpired:
        query_status = "ninja-timeout"
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        query_status = f"ninja-error-{type(error).__name__}"
    return GeneratedOwnerRouting(
        ninja_candidates=tuple(ninja_candidates[:_MAX_OWNER_CANDIDATES]),
        heuristic_candidates=heuristic_candidates,
        query_status=query_status,
    )


def _run_one(
    tidy: Path,
    command: CompileCommand,
    *,
    source_root: Path,
    build_dir: Path,
    timeout_seconds: int,
    source_suppressions: tuple[dict[str, object], ...],
) -> LintResult:
    """Run clang-tidy for one exact compilation and capture all output."""

    invocation = _clang_tidy_invocation(tidy, command, source_root=source_root)
    started_at = time.monotonic()
    try:
        completed = subprocess.run(
            invocation,
            cwd=command.directory,
            env=_subprocess_environment(),
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_seconds,
        )
        exit_code = completed.returncode
        output = completed.stdout + completed.stderr
    except subprocess.TimeoutExpired as error:
        exit_code = 124
        stdout = error.stdout if isinstance(error.stdout, str) else ""
        stderr = error.stderr if isinstance(error.stderr, str) else ""
        output = stdout + stderr + f"\nclang-tidy timed out after {timeout_seconds} seconds\n"
    duration = time.monotonic() - started_at
    diagnostics = _parse_diagnostics(output)
    needs_generated_routing = command.is_generated and (
        exit_code != 0 or bool(diagnostics)
    )
    owner_routing = (
        _generated_owner_candidates(
            command,
            source_root=source_root,
            build_dir=build_dir,
        )
        if needs_generated_routing
        else None
    )
    return LintResult(
        command=command,
        invocation=invocation,
        exit_code=exit_code,
        output=output,
        duration_seconds=duration,
        diagnostics=diagnostics,
        ninja_owner_candidates=(
            owner_routing.ninja_candidates if owner_routing else ()
        ),
        heuristic_owner_candidates=(
            owner_routing.heuristic_candidates if owner_routing else ()
        ),
        owner_query_status=(owner_routing.query_status if owner_routing else None),
        generator_hint=(
            _generator_hint(command.source) if needs_generated_routing else None
        ),
        source_suppressions=source_suppressions,
    )


def _result_document(result: LintResult) -> dict[str, object]:
    """Convert one immutable result into its JSON report representation."""

    return {
        "path": result.command.display_path,
        "variant": result.command.variant,
        "generated": result.command.is_generated,
        "working_directory": str(result.command.directory),
        "compiler": str(result.command.compiler),
        "compile_database": result.command.compile_database,
        "response_files": [
            {"path": path, "sha256": digest}
            for path, digest in result.command.response_files
        ],
        "suppression_profiles": list(_suppression_profiles(result.command)),
        "disabled_checks": list(_disabled_checks(result.command)),
        "exit_code": result.exit_code,
        "duration_seconds": round(result.duration_seconds, 6),
        "diagnostics": list(result.diagnostics),
        "diagnostic_count": len(result.diagnostics),
        "ninja_owner_candidates": list(result.ninja_owner_candidates),
        "heuristic_owner_candidates": list(result.heuristic_owner_candidates),
        "owner_query_status": result.owner_query_status,
        "generator_hint": result.generator_hint,
        "source_suppressions": list(result.source_suppressions),
        "invocation": list(result.invocation),
        "output": result.output,
    }


def _validate_full_inventory(
    *,
    database_evidence: list[dict[str, str]],
    selected: tuple[CompileCommand, ...],
    available_authored: tuple[CompileCommand, ...],
    available_generated: tuple[CompileCommand, ...],
) -> None:
    """Require complete authored/generated and database-provenance coverage."""

    if len(database_evidence) < 2:
        raise ClangTidyError(
            "full lint requires the primary and first-party generator compile databases"
        )
    if not available_authored or not available_generated:
        raise ClangTidyError(
            "full lint requires nonempty authored and generated compile-unit coverage"
        )
    evidence_paths = {evidence["path"] for evidence in database_evidence}
    generator_database_paths = {
        path
        for path in evidence_paths
        if _GENERATOR_COMPILE_DATABASE_PATTERN.fullmatch(path) is not None
    }
    selected_database_paths = {command.compile_database for command in selected}
    if (
        _COMPILE_DATABASE_NAME not in evidence_paths
        or _COMPILE_DATABASE_NAME not in selected_database_paths
        or not generator_database_paths.intersection(selected_database_paths)
    ):
        raise ClangTidyError(
            "full lint requires nonempty selected contribution from the primary "
            "and an authenticated first-party generator compile database"
        )


def run_clang_tidy(
    *,
    source_root: Path,
    build_dir: Path,
    scope: str,
    focused_paths: tuple[str, ...],
    report_path: Path,
    jobs: int,
    timeout_seconds: int,
    output: TextIO,
) -> int:
    """Run the requested lint scope and write a complete machine report.

    @param source_root Repository root containing the authoritative policy.
    @param build_dir Configured Clang build tree with module maps and BMIs.
    @param scope Either ``focused`` or ``full``.
    @param focused_paths Exact relative compile units for focused scope.
    @param report_path Build-tree JSON result destination.
    @param jobs Positive maximum concurrent clang-tidy processes.
    @param timeout_seconds Positive per-unit execution deadline.
    @param output Human-readable command and diagnostic stream.
    @return Zero only when every selected clang-tidy process succeeds.
    """

    source_root = source_root.resolve(strict=True)
    build_dir = build_dir.resolve(strict=True)
    report_path = report_path.resolve(strict=False)
    config_path = source_root / _CONFIG_NAME
    if not config_path.is_file():
        raise ClangTidyError(f"clang-tidy policy does not exist: {config_path}")
    if not _is_relative_to(build_dir, source_root / "build"):
        raise ClangTidyError(
            f"lint build directory is outside the repository build root: {build_dir}"
        )
    if not _is_relative_to(report_path, build_dir):
        raise ClangTidyError(
            f"clang-tidy report must remain inside the selected build tree: {report_path}"
        )
    if jobs <= 0 or timeout_seconds <= 0:
        raise ValueError("jobs and timeout_seconds must be positive")

    database_evidence = _compile_database_evidence(build_dir)
    commands = _load_compile_commands(source_root, build_dir)
    selected = _select_commands(commands, scope=scope, focused_paths=focused_paths)
    available_authored = tuple(command for command in commands if not command.is_generated)
    available_generated = tuple(command for command in commands if command.is_generated)
    if scope == "full":
        _validate_full_inventory(
            database_evidence=database_evidence,
            selected=selected,
            available_authored=available_authored,
            available_generated=available_generated,
        )
    source_suppression_inventory = {
        source: _source_suppressions(source)
        for source in sorted({command.source for command in selected})
    }
    tidy, toolchain = _matching_clang_tidy(selected)
    toolchain["config_verification"] = _verify_config(tidy, config_path)
    print(
        f"# clang-tidy scope={scope} selected={len(selected)} "
        f"authored={sum(not command.is_generated for command in selected)} "
        f"generated={sum(command.is_generated for command in selected)} "
        f"databases={len(database_evidence)} jobs={jobs}",
        file=output,
        flush=True,
    )
    results: list[LintResult] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = [
            executor.submit(
                _run_one,
                tidy,
                command,
                source_root=source_root,
                build_dir=build_dir,
                timeout_seconds=timeout_seconds,
                source_suppressions=source_suppression_inventory[command.source],
            )
            for command in selected
        ]
        for command, future in zip(selected, futures, strict=True):
            result = future.result()
            results.append(result)
            print(f"# cwd={result.command.directory}", file=output)
            print(f"$ {shlex.join(result.invocation)}", file=output)
            if result.output:
                print(result.output, end="" if result.output.endswith("\n") else "\n", file=output)
            if result.command.is_generated and result.ninja_owner_candidates:
                print(
                    f"generated-owner-ninja[{result.owner_query_status}]: "
                    f"{result.command.display_path} <- "
                    + ", ".join(result.ninja_owner_candidates),
                    file=output,
                )
            if result.command.is_generated and result.heuristic_owner_candidates:
                print(
                    f"generated-owner-heuristic: {result.command.display_path} <- "
                    + ", ".join(result.heuristic_owner_candidates),
                    file=output,
                )
            if (
                result.command.is_generated
                and result.owner_query_status is not None
                and not result.ninja_owner_candidates
                and not result.heuristic_owner_candidates
            ):
                print(
                    f"generated-owner-unresolved[{result.owner_query_status}]: "
                    f"{result.command.display_path}",
                    file=output,
                )
            print(
                f"# result path={command.display_path} variant={command.variant} "
                f"exit={result.exit_code} diagnostics={len(result.diagnostics)} "
                f"duration_seconds={result.duration_seconds:.3f}",
                file=output,
                flush=True,
            )

    failed = [result for result in results if result.exit_code != 0]
    config_sha256 = hashlib.sha256(config_path.read_bytes()).hexdigest()
    compile_database_contributions = {
        evidence["path"]: sum(
            command.compile_database == evidence["path"] for command in commands
        )
        for evidence in database_evidence
    }
    report = {
        "schema_version": 2,
        "scope": scope,
        "source_root": str(source_root),
        "build_dir": str(build_dir),
        "policy": str(config_path),
        "policy_sha256": config_sha256,
        "toolchain": toolchain,
        "compile_databases": database_evidence,
        "compile_database_contributions": compile_database_contributions,
        "available_count": len(commands),
        "available_path_count": len({command.display_path for command in commands}),
        "available_authored_count": len(available_authored),
        "available_authored_path_count": len(
            {command.display_path for command in available_authored}
        ),
        "available_generated_count": len(available_generated),
        "available_generated_path_count": len(
            {command.display_path for command in available_generated}
        ),
        "selected_count": len(results),
        "selected_path_count": len(
            {result.command.display_path for result in results}
        ),
        "authored_count": sum(not result.command.is_generated for result in results),
        "authored_path_count": len(
            {
                result.command.display_path
                for result in results
                if not result.command.is_generated
            }
        ),
        "generated_count": sum(result.command.is_generated for result in results),
        "generated_path_count": len(
            {
                result.command.display_path
                for result in results
                if result.command.is_generated
            }
        ),
        "optimization_verified_count": len(commands),
        "optimization_policy": {
            "optimization_flags": ["-O3"],
            "ndebug_required": True,
        },
        "source_suppression_count": len(
            {
                (
                    result.command.display_path,
                    json.dumps(suppression, sort_keys=True, separators=(",", ":")),
                )
                for result in results
                for suppression in result.source_suppressions
            }
        ),
        "diagnostic_count": sum(len(result.diagnostics) for result in results),
        "failure_count": len(failed),
        "results": [_result_document(result) for result in results],
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(
        f"# clang-tidy completed failures={len(failed)} "
        f"diagnostics={report['diagnostic_count']} report={report_path}",
        file=output,
        flush=True,
    )
    return 0 if not failed else 1


def build_argument_parser() -> argparse.ArgumentParser:
    """Create the closed command-line interface used by the build worker."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--scope", required=True, choices=("focused", "full"))
    parser.add_argument("--path", action="append", default=[], dest="focused_paths")
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=_DEFAULT_JOBS)
    parser.add_argument(
        "--timeout-seconds",
        type=int,
        default=_DEFAULT_UNIT_TIMEOUT_SECONDS,
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Run the closed clang-tidy CLI and report validation errors clearly."""

    parsed = build_argument_parser().parse_args(argv)
    try:
        return run_clang_tidy(
            source_root=parsed.source_root,
            build_dir=parsed.build_dir,
            scope=parsed.scope,
            focused_paths=tuple(parsed.focused_paths),
            report_path=parsed.report,
            jobs=parsed.jobs,
            timeout_seconds=parsed.timeout_seconds,
            output=sys.stdout,
        )
    except (ClangTidyError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"clang-tidy configuration error: {error}", file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
