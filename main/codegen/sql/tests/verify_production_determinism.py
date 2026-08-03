#!/usr/bin/env python3
"""Prove production SQL code generation is independent of discovery order.

The harness stages the selected production C++ module compile closure into
three independent, symlink-free source/build/output roots, reruns PREPARE in
each root, and then runs the configured generator.  Raw directory/file/output,
response-file, module-cache, and separate/joined path-flag spellings are
relocated before PREPARE.  Output arguments use the same absolute
source-root-relative mirror as their ordinary production counterparts, so
generated path metadata is itself required to be reproducible; generated
output bytes are never normalized.  Cross-root PREPARE comparison strictly
parses each stage's PCM manifest and compares only its canonical relocation-
semantic projection, while every physical closure remains independently
validated and isolated.  A same-root repeat compares the complete raw PREPARE
snapshot byte-for-byte.  The canonical generated result must also match every
ordinary production output byte-for-byte.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass, replace
from typing import Callable, Iterable, Mapping, Sequence


REPORT_SCHEMA = "bioseq_sql_production_determinism/v1"
TARGET_DYNDEP_INVENTORY_SCHEMA = (
    "bioseq_sql_target_dyndep_inventory/v1"
)
VARIANTS = ("canonical", "reversed", "shuffled")
STAGES = ("support", "core", "final")
PIPELINE_STAGES = ("support", "test_support", "core", "final")
SUPPORT_FMGR_OWNERS = (
    "vector_aggregate_registry",
    "vector_comparison_registry",
)
STABLE_KEY = re.compile(r"^[a-z][a-z0-9_]*$")
NOOP_SENTINEL_NS = 946_684_800_000_000_000
REPOSITORY_MAIN_ROOTS = frozenset(
    {
        "bench",
        "bench_framework",
        "bioseq",
        "bioseq_caching",
        "cmake",
        "codegen",
        "common",
        "config",
        "data",
        "DOCS",
        "infra",
        "main",
        "overlays",
        "papers",
        "reporting",
        "vendor",
        "website",
    }
)
PATH_LEFT_DELIMITERS = frozenset(" \t\r\n\"'=:@,;([{")
PATH_RIGHT_DELIMITERS = frozenset("/\\ \t\r\n\"'=:@,;)]}")
JOINED_PATH_PREFIXES = (
    "--serialize-diagnostics=",
    "-fmodules-cache-path=",
    "-fprebuilt-module-path=",
    "-fmodule-map-file=",
    "-fmodule-cache-path=",
    "-fmodule-mapper=",
    "-working-directory=",
    "-resource-dir=",
    "-fdeps-target=",
    "-fdeps-file=",
    "-fmodule-output=",
    "-include-pch",
    "-iframeworkwithsysroot",
    "-iframework",
    "-ivfsoverlay",
    "-isystem-after",
    "-isystem",
    "-iquote",
    "-idirafter",
    "-imacros",
    "-include",
    "--sysroot=",
    "-isysroot",
    "-MF",
    "-MJ",
    "-F",
    "-I",
)
SEPARATE_INPUT_PATH_FLAGS = frozenset(
    {
        "-I",
        "-idirafter",
        "-imacros",
        "-include",
        "-include-pch",
        "-iframeworkwithsysroot",
        "-iframework",
        "-iquote",
        "-isysroot",
        "-isystem-after",
        "-isystem",
        "-ivfsoverlay",
        "-resource-dir",
        "--sysroot",
        "-fmodule-map-file",
        "-fprebuilt-module-path",
        "-fmodules-cache-path",
        "-fmodule-cache-path",
        "-fmodule-mapper",
        "-working-directory",
        "-F",
    }
)
SEPARATE_OUTPUT_PATH_FLAGS = frozenset(
    {
        "-o",
        "-fdeps-file",
        "-fdeps-target",
        "-fmodule-output",
        "-MF",
        "-MJ",
        "--serialize-diagnostics",
    }
)
JOINED_INPUT_PATH_PREFIXES = (
    "-fmodules-cache-path=",
    "-fprebuilt-module-path=",
    "-fmodule-map-file=",
    "-fmodule-cache-path=",
    "-fmodule-mapper=",
    "-working-directory=",
    "-resource-dir=",
    "-include-pch",
    "-iframeworkwithsysroot",
    "-iframework",
    "-ivfsoverlay",
    "-isystem-after",
    "-isystem",
    "-iquote",
    "-idirafter",
    "-imacros",
    "-include",
    "--sysroot=",
    "-isysroot",
    "-F",
    "-I",
)
JOINED_OUTPUT_PATH_PREFIXES = (
    "--serialize-diagnostics=",
    "-fmodule-output=",
    "-fdeps-file=",
    "-fdeps-target=",
    "-MF",
    "-MJ",
    "-o",
)
PREPARE_PCM_MANIFEST_SCHEMA = "bioseq_clang_pcm_manifest/v1"
PREPARE_COMPILER_MODULE_POLICY = (
    "selected-clang-libc++-std-and-std.compat-only"
)
PREPARE_ROOT_KEYS = frozenset(
    {
        "schema",
        "clang_version",
        "clang_binary",
        "clang_binary_sha256",
        "clang_scan_deps",
        "clang_scan_deps_version",
        "clang_scan_deps_sha256",
        "llvm_config",
        "llvm_config_sha256",
        "llvm_version",
        "llvm_prefix",
        "clang_resource_dir",
        "clang_resource_dir_sha256",
        "clang_full_version",
        "linked_clang_full_version",
        "linked_clang_cpp",
        "linked_clang_cpp_sha256",
        "linked_llvm_library",
        "linked_llvm_library_sha256",
        "compiler_module_policy",
        "compiler_module_inventory",
        "compiler_module_inventory_sha256",
        "relocation_root",
        "pcm_root",
        "owned_compile_database_sha256",
        "relocatable_owned_compile_database_sha256",
        "compiler_profile",
        "authority",
        "compilation_signature",
        "relocatable_compilation_signature",
        "modules",
    }
)
PREPARE_ROOT_HASH_KEYS = frozenset(
    {
        "clang_binary_sha256",
        "clang_scan_deps_sha256",
        "llvm_config_sha256",
        "clang_resource_dir_sha256",
        "linked_clang_cpp_sha256",
        "linked_llvm_library_sha256",
        "compiler_module_inventory_sha256",
        "owned_compile_database_sha256",
        "relocatable_owned_compile_database_sha256",
        "compilation_signature",
        "relocatable_compilation_signature",
    }
)
PREPARE_ROOT_SEMANTIC_KEYS = (
    "schema",
    "clang_version",
    "clang_binary_sha256",
    "clang_scan_deps_version",
    "clang_scan_deps_sha256",
    "llvm_config_sha256",
    "llvm_version",
    "clang_resource_dir_sha256",
    "clang_full_version",
    "linked_clang_full_version",
    "linked_clang_cpp_sha256",
    "linked_llvm_library_sha256",
    "compiler_module_policy",
    "compiler_module_inventory_sha256",
    "compiler_profile",
    "authority",
    "relocatable_owned_compile_database_sha256",
    "relocatable_compilation_signature",
)
PREPARE_MODULE_KEYS = frozenset(
    {
        "name",
        "source",
        "directory",
        "source_sha256",
        "relocatable_source_sha256",
        "arguments_sha256",
        "relocatable_arguments_sha256",
        "command_input_sha256",
        "relocatable_command_input_sha256",
        "command_inputs",
        "base_arguments",
        "active_import_scan_database",
        "active_import_scan_database_sha256",
        "relocatable_active_import_scan_database_sha256",
        "active_import_scan_result",
        "active_import_scan_result_sha256",
        "relocatable_active_import_scan_result_sha256",
        "active_import_scan_diagnostics",
        "active_import_scan_diagnostics_sha256",
        "relocatable_active_import_scan_diagnostics_sha256",
        "direct_imports",
        "depfile",
        "depfile_sha256",
        "relocatable_depfile_sha256",
        "dependency_content_sha256",
        "relocatable_dependency_content_sha256",
        "content_key",
        "relocatable_content_key",
        "pcm",
        "pcm_sha256",
        "pcm_provenance",
        "pcm_provenance_sha256",
    }
)
PREPARE_MODULE_HASH_KEYS = frozenset(
    key
    for key in PREPARE_MODULE_KEYS
    if key.endswith("_sha256") or key.endswith("content_key")
)
PREPARE_MODULE_SEMANTIC_KEYS = (
    "relocatable_source_sha256",
    "relocatable_arguments_sha256",
    "relocatable_command_input_sha256",
    "relocatable_active_import_scan_database_sha256",
    "relocatable_active_import_scan_result_sha256",
    "relocatable_active_import_scan_diagnostics_sha256",
    "relocatable_depfile_sha256",
    "relocatable_dependency_content_sha256",
    "relocatable_content_key",
)
PREPARE_COMMAND_INPUT_KEYS = frozenset(
    {"path", "sha256", "relocatable_sha256"}
)


class DeterminismError(RuntimeError):
    """A fail-closed production determinism diagnostic."""


@dataclass(frozen=True)
class FacetInventory:
    row_codecs: tuple[str, ...]
    entity_handles: tuple[str, ...]
    fmgr_entrypoints: tuple[str, ...]
    support_fmgr_entrypoints: tuple[str, ...]
    guc_owners: tuple[str, ...]
    test_row_codecs: tuple[str, ...]
    test_support_fmgr_entrypoints: tuple[str, ...]
    test_guc_owners: tuple[str, ...]
    annotation_free_result_rows: tuple[str, ...]
    legacy_fmgr_scans: tuple[str, ...]
    support_sources: tuple[str, ...]
    test_support_sources: tuple[str, ...]
    core_sources: tuple[str, ...]
    final_sources: tuple[str, ...]

    @property
    def final_fmgr_entrypoints(self) -> tuple[str, ...]:
        return tuple(
            owner
            for owner in self.fmgr_entrypoints
            if owner not in self.support_fmgr_entrypoints
        )


@dataclass(frozen=True)
class FileState:
    content: bytes
    sha256: str
    size: int
    mode: int
    inode: int
    mtime_ns: int
    ctime_ns: int


@dataclass(frozen=True)
class GenerationContext:
    compile_database: Path
    pcm_root: Path
    pcm_manifest: Path
    depfile: Path
    stamp: Path


@dataclass(frozen=True)
class StagedCompilation:
    compile_database: Path
    relocated_sources: Mapping[str, Path]
    physical_outputs: Mapping[str, Path]
    mirror_root: Path
    staged_files: tuple[Path, ...]
    selected_entry_count: int
    staged_file_count: int


class PathRewriter:
    """Apply one simultaneous, longest-match path-prefix rewrite pass."""

    def __init__(self, replacements: Mapping[str, str]):
        normalized: dict[str, str] = {}
        for old, new in replacements.items():
            if not old or not new:
                raise DeterminismError(
                    "path replacement endpoints must be non-empty"
                )
            if not Path(old).is_absolute() or not Path(new).is_absolute():
                raise DeterminismError(
                    "path replacement endpoints must be absolute: "
                    f"{old!r} -> {new!r}"
                )
            previous = normalized.setdefault(old, new)
            if previous != new:
                raise DeterminismError(
                    f"path replacement has conflicting targets: {old}"
                )
        self.replacements = tuple(
            sorted(
                normalized.items(),
                key=lambda pair: (-len(pair[0]), pair[0]),
            )
        )

    def rewrite(
        self,
        text: str,
        replacement_counts: dict[str, int] | None = None,
    ) -> str:
        if not text:
            return text
        output: list[str] = []
        position = 0
        while position < len(text):
            selected: tuple[str, str] | None = None
            for old, new in self.replacements:
                if not text.startswith(old, position):
                    continue
                if not _path_match_has_boundaries(text, position, len(old)):
                    continue
                selected = (old, new)
                break
            if selected is None:
                output.append(text[position])
                position += 1
                continue
            old, new = selected
            output.append(new)
            if replacement_counts is not None:
                replacement_counts[old] = (
                    replacement_counts.get(old, 0) + 1
                )
            position += len(old)
        return "".join(output)


def _sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def _path_match_has_boundaries(
    text: str, position: int, length: int
) -> bool:
    if position:
        previous = text[position - 1]
        if previous not in PATH_LEFT_DELIMITERS:
            token_begin = position - 1
            while (
                token_begin >= 0
                and not text[token_begin].isspace()
                and text[token_begin] not in "\"'"
            ):
                token_begin -= 1
            prefix = text[token_begin + 1 : position]
            if (
                prefix not in JOINED_PATH_PREFIXES
                and not prefix.startswith("-")
                and "=" not in prefix
            ):
                return False
    end = position + length
    return end == len(text) or text[end] in PATH_RIGHT_DELIMITERS


def _path_occurrence_count(text: str, needle: str) -> int:
    count = 0
    position = 0
    while True:
        position = text.find(needle, position)
        if position == -1:
            return count
        if _path_match_has_boundaries(text, position, len(needle)):
            count += 1
        position += max(1, len(needle))


def _atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.is_file() and path.read_bytes() == content:
        return
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(content)
    temporary.replace(path)


def _stat_identity(metadata: os.stat_result) -> tuple[int, ...]:
    return (
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mode,
        metadata.st_nlink,
        metadata.st_size,
        metadata.st_mtime_ns,
        metadata.st_ctime_ns,
    )


def _require_single_link(
    metadata: os.stat_result,
    path: Path,
    label: str,
) -> None:
    if metadata.st_nlink != 1:
        raise DeterminismError(
            f"{label} is hard-linked (nlink={metadata.st_nlink}): {path}"
        )


def _replace_with_isolated_bytes(
    path: Path,
    content: bytes,
    mode: int,
    label: str,
) -> None:
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
    )
    temporary = Path(temporary_name)
    descriptor_open = True
    try:
        with os.fdopen(descriptor, "wb") as output:
            descriptor_open = False
            output.write(content)
            output.flush()
            os.fchmod(output.fileno(), mode)
            temporary_metadata = os.fstat(output.fileno())
            if not stat.S_ISREG(temporary_metadata.st_mode):
                raise DeterminismError(
                    f"temporary staged {label} is not a regular file: "
                    f"{temporary}"
                )
            _require_single_link(
                temporary_metadata,
                temporary,
                f"temporary staged {label}",
            )
        os.replace(temporary, path)
        metadata = path.lstat()
        if not stat.S_ISREG(metadata.st_mode):
            raise DeterminismError(
                f"staged {label} is not a regular file: {path}"
            )
        _require_single_link(metadata, path, f"staged {label}")
        if stat.S_IMODE(metadata.st_mode) != mode:
            raise DeterminismError(
                f"staged {label} did not preserve its mode: {path}"
            )
        if path.read_bytes() != content:
            raise DeterminismError(
                f"staged {label} did not preserve its bytes: {path}"
            )
        metadata_after = path.lstat()
        _require_single_link(metadata_after, path, f"staged {label}")
        if _stat_identity(metadata) != _stat_identity(metadata_after):
            raise DeterminismError(
                f"staged {label} changed while it was verified: {path}"
            )
    finally:
        if descriptor_open:
            os.close(descriptor)
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _require_regular_file(path: Path, label: str, executable: bool = False) -> Path:
    resolved = path.expanduser().resolve(strict=True)
    metadata = resolved.stat()
    if not stat.S_ISREG(metadata.st_mode):
        raise DeterminismError(f"{label} is not a regular file: {resolved}")
    if executable and not os.access(resolved, os.X_OK):
        raise DeterminismError(f"{label} is not executable: {resolved}")
    return resolved


def _require_directory(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve(strict=True)
    if not resolved.is_dir():
        raise DeterminismError(f"{label} is not a directory: {resolved}")
    return resolved


def _read_source_inventory(path: Path, stage: str) -> tuple[str, ...]:
    source_list = _require_regular_file(path, f"{stage} source list")
    sources: list[str] = []
    for line_number, raw_line in enumerate(
        source_list.read_text(encoding="utf-8").splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        candidate = Path(line)
        if not candidate.is_absolute():
            candidate = source_list.parent / candidate
        resolved = _require_regular_file(
            candidate, f"{stage} source at line {line_number}"
        )
        if resolved.suffix != ".cppm":
            raise DeterminismError(
                f"{stage} source is not a .cppm owner: {resolved}"
            )
        sources.append(resolved.as_posix())

    canonical = tuple(sorted(sources))
    if len(canonical) < 3:
        raise DeterminismError(
            f"{stage} production inventory needs at least three owners to "
            "exercise canonical, reversed, and shuffled orders"
        )
    if len(set(canonical)) != len(canonical):
        raise DeterminismError(
            f"{stage} production inventory contains duplicate source identities"
        )
    return canonical


def _shuffled_inventory(
    canonical: Sequence[str], stage: str
) -> tuple[str, ...]:
    ranked = sorted(
        canonical,
        key=lambda source: hashlib.sha256(
            f"{REPORT_SCHEMA}\0{stage}\0{source}".encode("utf-8")
        ).digest(),
    )
    shuffled = tuple(ranked)
    reversed_inventory = tuple(reversed(canonical))
    if shuffled in (tuple(canonical), reversed_inventory):
        shuffled = tuple((*canonical[1:], canonical[0]))
    if shuffled in (tuple(canonical), reversed_inventory):
        raise DeterminismError(
            f"{stage} deterministic shuffle did not produce a third order"
        )
    return shuffled


def _inventory_variants(
    canonical: Sequence[str], stage: str
) -> Mapping[str, tuple[str, ...]]:
    result = {
        "canonical": tuple(canonical),
        "reversed": tuple(reversed(canonical)),
        "shuffled": _shuffled_inventory(canonical, stage),
    }
    if len(set(result.values())) != len(VARIANTS):
        raise DeterminismError(
            f"{stage} source-order variants are not pairwise distinct"
        )
    return result


def _parse_owner_registry(path: Path) -> FacetInventory:
    registry = _require_regular_file(path, "SQL owner registry metadata")
    expected_header = (
        "owner_key",
        "module",
        "profiles",
        "authorities",
        "facets",
        "source",
    )
    ownership: dict[str, dict[str, str]] = {
        "row_codecs": {},
        "entity_handles": {},
        "fmgr_entrypoints": {},
        "guc_owner": {},
        "annotation_free_result_row": {},
        "legacy_fmgr_scan": {},
    }
    test_support_ownership: dict[str, dict[str, str]] = {
        "row_codecs": {},
        "guc_owner": {},
    }
    production_sources: dict[str, set[str]] = {
        "support": set(),
        "core": set(),
        "final": set(),
    }
    test_indexes_support_sources: set[str] = set()
    with registry.open("r", encoding="utf-8", newline="") as stream:
        rows = csv.DictReader(stream, delimiter="\t")
        if tuple(rows.fieldnames or ()) != expected_header:
            raise DeterminismError(
                "SQL owner registry metadata has an unexpected header"
            )
        for row_number, row in enumerate(rows, start=2):
            profiles = {
                item for item in (row["profiles"] or "").split(",") if item
            }
            if not profiles.intersection({"production", "test_indexes"}):
                continue
            owner_key = row["owner_key"] or ""
            if not STABLE_KEY.fullmatch(owner_key):
                raise DeterminismError(
                    f"invalid production owner key at metadata line "
                    f"{row_number}: {owner_key!r}"
                )
            source = Path(row["source"] or "")
            if not source.is_absolute():
                raise DeterminismError(
                    f"production owner source is not absolute at metadata "
                    f"line {row_number}: {source}"
                )
            source_identity = source.resolve(strict=False).as_posix()
            authorities = {
                item
                for item in (row["authorities"] or "").split(",")
                if item
            }
            if (
                "test_indexes" in profiles
                and "SUPPORT" in authorities
            ):
                test_indexes_support_sources.add(source_identity)
            for facet in (row["facets"] or "").split(","):
                if not facet:
                    continue
                category, separator, key = facet.partition("=")
                if not separator or not key:
                    raise DeterminismError(
                        f"malformed owner facet at metadata line {row_number}: "
                        f"{facet!r}"
                    )
                if category not in ownership:
                    raise DeterminismError(
                        f"unknown production owner facet at metadata line "
                        f"{row_number}: {facet!r}"
                    )
                if not STABLE_KEY.fullmatch(key):
                    raise DeterminismError(
                        f"invalid production facet key at metadata line "
                        f"{row_number}: {facet!r}"
                    )
                if "production" in profiles:
                    previous = ownership[category].setdefault(
                        key, owner_key
                    )
                    if previous != owner_key:
                        raise DeterminismError(
                            "production facet "
                            f"{category}={key} has multiple owners"
                        )
                if (
                    "test_indexes" in profiles
                    and "SUPPORT" in authorities
                    and category in test_support_ownership
                ):
                    previous = test_support_ownership[
                        category
                    ].setdefault(key, owner_key)
                    if previous != owner_key:
                        raise DeterminismError(
                            "test_indexes SUPPORT facet "
                            f"{category}={key} has multiple owners"
                        )
            if "production" not in profiles:
                continue
            production_sources["final"].add(source_identity)
            if "SUPPORT" in authorities:
                production_sources["support"].add(source_identity)
            if "CORE" in authorities:
                production_sources["core"].add(source_identity)

    def keys(category: str) -> tuple[str, ...]:
        values = tuple(sorted(ownership[category]))
        if not values:
            raise DeterminismError(
                f"production owner registry has no {category} facets"
            )
        return values

    def test_keys(category: str) -> tuple[str, ...]:
        values = tuple(sorted(test_support_ownership[category]))
        if not values:
            raise DeterminismError(
                "test_indexes SUPPORT owner registry has no "
                f"{category} facets"
            )
        return values

    result = FacetInventory(
        row_codecs=keys("row_codecs"),
        entity_handles=keys("entity_handles"),
        fmgr_entrypoints=keys("fmgr_entrypoints"),
        support_fmgr_entrypoints=SUPPORT_FMGR_OWNERS,
        guc_owners=keys("guc_owner"),
        test_row_codecs=test_keys("row_codecs"),
        test_support_fmgr_entrypoints=SUPPORT_FMGR_OWNERS,
        test_guc_owners=test_keys("guc_owner"),
        annotation_free_result_rows=keys(
            "annotation_free_result_row"
        ),
        legacy_fmgr_scans=keys("legacy_fmgr_scan"),
        support_sources=tuple(sorted(production_sources["support"])),
        test_support_sources=tuple(
            sorted(test_indexes_support_sources)
        ),
        core_sources=tuple(sorted(production_sources["core"])),
        final_sources=tuple(sorted(production_sources["final"])),
    )
    for stage in STAGES:
        if not getattr(result, f"{stage}_sources"):
            raise DeterminismError(
                f"production owner registry has no {stage} sources"
            )
    if not result.test_support_sources:
        raise DeterminismError(
            "owner registry has no test_indexes SUPPORT sources"
        )
    return result


def _relative_outputs(facets: FacetInventory) -> set[PurePosixPath]:
    outputs = {
        PurePosixPath("artifacts/support/support-manifest.json"),
        PurePosixPath("artifacts/support/support.stamp"),
        PurePosixPath(
            "artifacts/test-support/support-manifest.json"
        ),
        PurePosixPath("artifacts/test-support/support.stamp"),
        PurePosixPath("artifacts/core/core-manifest.json"),
        PurePosixPath("artifacts/core/core-descriptor.sha256"),
        PurePosixPath("artifacts/core/core.stamp"),
        PurePosixPath("artifacts/final/bioseq--1.0.sql"),
        PurePosixPath("artifacts/final/bioseq-sql-manifest.json"),
        PurePosixPath("artifacts/final/bioseq-sql.stamp"),
        PurePosixPath("artifacts/gucs/production/guc_registry.cppm"),
    }
    outputs.update(
        PurePosixPath(f"artifacts/row-codecs/{owner}.row_codecs.cppm")
        for owner in facets.row_codecs
    )
    outputs.update(
        PurePosixPath(
            f"artifacts/test-row-codecs/{owner}.row_codecs.cppm"
        )
        for owner in facets.test_row_codecs
    )
    outputs.update(
        PurePosixPath(f"artifacts/handles/{owner}.entity_handles.cppm")
        for owner in facets.entity_handles
    )
    outputs.update(
        PurePosixPath(f"artifacts/fmgr/{owner}.entrypoints.cppm")
        for owner in facets.fmgr_entrypoints
    )
    outputs.update(
        PurePosixPath(
            f"artifacts/test-fmgr/{owner}.entrypoints.cppm"
        )
        for owner in facets.test_support_fmgr_entrypoints
    )
    outputs.update(
        PurePosixPath(f"artifacts/gucs/production/owners/{owner}.gucs.cppm")
        for owner in facets.guc_owners
    )
    outputs.add(
        PurePosixPath(
            "artifacts/gucs/test_indexes/guc_registry.cppm"
        )
    )
    outputs.update(
        PurePosixPath(
            f"artifacts/gucs/test_indexes/owners/{owner}.gucs.cppm"
        )
        for owner in facets.test_guc_owners
    )
    return outputs


def _parse_ordinary_outputs(
    bindings: Sequence[str],
    expected: set[PurePosixPath],
) -> dict[str, Path]:
    ordinary: dict[str, Path] = {}
    paths_seen: dict[Path, str] = {}
    for binding in bindings:
        relative_text, separator, path_text = binding.partition("=")
        if not separator or not relative_text or not path_text:
            raise DeterminismError(
                "ordinary output binding must be RELATIVE=ABSOLUTE: "
                f"{binding!r}"
            )
        relative = PurePosixPath(relative_text)
        if (
            relative.is_absolute()
            or relative.as_posix() != relative_text
            or any(part in {"", ".", ".."} for part in relative.parts)
        ):
            raise DeterminismError(
                f"ordinary output identity is not canonical: {relative_text!r}"
            )
        if relative not in expected:
            raise DeterminismError(
                f"ordinary output identity is outside the owner closure: "
                f"{relative_text}"
            )
        if relative_text in ordinary:
            raise DeterminismError(
                f"duplicate ordinary output identity: {relative_text}"
            )
        path = _require_regular_file(
            Path(path_text), f"ordinary production output {relative_text}"
        )
        previous = paths_seen.setdefault(path, relative_text)
        if previous != relative_text:
            raise DeterminismError(
                "ordinary production output path is bound to multiple "
                f"identities: {path}"
            )
        ordinary[relative_text] = path
    expected_strings = {path.as_posix() for path in expected}
    if set(ordinary) != expected_strings:
        missing = sorted(expected_strings - set(ordinary))
        unexpected = sorted(set(ordinary) - expected_strings)
        raise DeterminismError(
            "ordinary production output bindings are not the exact owner "
            f"closure; missing={missing}, unexpected={unexpected}"
        )
    return ordinary


def _with_support_fmgr_from_ordinary_manifest(
    facets: FacetInventory,
    ordinary_outputs: Mapping[str, Path],
) -> FacetInventory:
    def manifest_owners(identity: str, profile: str) -> tuple[str, ...]:
        manifest_path = ordinary_outputs.get(identity)
        if manifest_path is None:
            raise DeterminismError(
                f"ordinary outputs do not contain {profile} SUPPORT"
            )
        try:
            document = json.loads(
                manifest_path.read_text(encoding="utf-8")
            )
        except json.JSONDecodeError as error:
            raise DeterminismError(
                f"ordinary {profile} SUPPORT manifest is not valid "
                f"JSON: {error}"
            ) from error
        if (
            not isinstance(document, dict)
            or document.get("schema")
                != "bioseq_sql_support_manifest/v1"
            or document.get("compiler_profile") != profile
        ):
            raise DeterminismError(
                f"ordinary {profile} SUPPORT manifest has an "
                "incompatible schema or profile"
            )
        vector_registries = document.get("vector_registries")
        if (
            not isinstance(vector_registries, list)
            or not vector_registries
        ):
            raise DeterminismError(
                f"ordinary {profile} SUPPORT manifest has no vector "
                "registries"
            )
        owners: list[str] = []
        for index, entry in enumerate(vector_registries):
            if not isinstance(entry, dict):
                raise DeterminismError(
                    f"ordinary {profile} SUPPORT vector registry "
                    f"entry {index} is not an object"
                )
            owner = entry.get("owner")
            if (
                not isinstance(owner, str)
                or not STABLE_KEY.fullmatch(owner)
            ):
                raise DeterminismError(
                    f"ordinary {profile} SUPPORT vector registry "
                    f"entry {index} has an invalid owner"
                )
            owners.append(owner)
        result = tuple(sorted(owners))
        if len(result) != len(set(result)):
            raise DeterminismError(
                f"ordinary {profile} SUPPORT manifest duplicates "
                "vector owners"
            )
        return result

    production_owners = manifest_owners(
        "artifacts/support/support-manifest.json",
        "production",
    )
    test_owners = manifest_owners(
        "artifacts/test-support/support-manifest.json",
        "test_indexes",
    )
    if production_owners != SUPPORT_FMGR_OWNERS:
        raise DeterminismError(
            "ordinary production SUPPORT vector registry closure "
            "differs from the exact two-owner contract"
        )
    if not set(production_owners).issubset(facets.fmgr_entrypoints):
        raise DeterminismError(
            "ordinary production SUPPORT vector owners escape the "
            "production FMGR owner closure"
        )
    if test_owners != facets.test_support_fmgr_entrypoints:
        raise DeterminismError(
            "ordinary test_indexes SUPPORT vector owners differ from "
            "the test_indexes registry metadata"
        )
    return replace(
        facets,
        support_fmgr_entrypoints=production_owners,
        test_support_fmgr_entrypoints=test_owners,
    )


def _repository_path_identity(path: Path, label: str) -> PurePosixPath:
    resolved = path.resolve(strict=True)
    reviewed_root_raw = os.environ.get("BIOSEQ_REVIEWED_MAIN_SOURCE_ROOT", "")
    if reviewed_root_raw:
        reviewed_root = Path(reviewed_root_raw).resolve(strict=True)
        if not reviewed_root.is_dir():
            raise DeterminismError(
                "BIOSEQ_REVIEWED_MAIN_SOURCE_ROOT is not a directory: "
                f"{reviewed_root}"
            )
        try:
            relative = resolved.relative_to(reviewed_root)
        except ValueError:
            # Self-tests also exercise independently relocated /main trees;
            # retain the structural normalization below for those paths.
            pass
        else:
            if not relative.parts:
                return PurePosixPath("main")
            if relative.parts[0] not in REPOSITORY_MAIN_ROOTS:
                raise DeterminismError(
                    f"{label} is outside the reviewed main source closure: "
                    f"{resolved}"
                )
            return PurePosixPath("main", *relative.parts)
    parts = resolved.parts
    if parts and parts[-1] == "main":
        return PurePosixPath("main")
    selected: int | None = None
    for index in range(len(parts) - 1):
        if (
            parts[index] == "main"
            and parts[index + 1] in REPOSITORY_MAIN_ROOTS
        ):
            selected = index
    if selected is None:
        raise DeterminismError(
            f"{label} is outside the reviewed main source tree: {resolved}"
        )
    identity = PurePosixPath(*parts[selected:])
    if identity.is_absolute() or ".." in identity.parts:
        raise DeterminismError(
            f"{label} has an unsafe repository identity: {identity}"
        )
    return identity


def _lexical_absolute(path: Path, directory: Path) -> Path:
    candidate = path if path.is_absolute() else directory / path
    return Path(os.path.abspath(os.path.normpath(candidate)))


def _path_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def _variant_mirror_root(source_root: Path, variant_root: Path) -> Path:
    return (
        variant_root
        / "workspace"
        / _repository_path_identity(source_root, "source root")
    )


def _variant_mirror_path(
    path: Path, source_root: Path, variant_root: Path
) -> Path:
    absolute = _lexical_absolute(path, source_root)
    if not _path_within(absolute, source_root):
        raise DeterminismError(
            f"project path escapes the mirrored source root: {absolute}"
        )
    return _variant_mirror_root(
        source_root, variant_root
    ) / absolute.relative_to(source_root)


def _assert_source_path_has_no_symlinks(
    path: Path, root: Path, label: str
) -> None:
    if not _path_within(path, root):
        raise DeterminismError(f"{label} escapes staging root {root}: {path}")
    cursor = root
    if cursor.is_symlink():
        raise DeterminismError(f"{label} root is a symlink: {cursor}")
    for component in path.relative_to(root).parts:
        cursor = cursor / component
        if cursor.is_symlink():
            raise DeterminismError(f"{label} is or traverses a symlink: {cursor}")


class VariantInputStager:
    def __init__(
        self,
        source_root: Path,
        build_root: Path,
        variant_root: Path,
        ordinary_outputs: Mapping[str, Path],
    ):
        self.source_root = source_root
        self.build_root = build_root
        self.variant_root = variant_root
        if variant_root.is_symlink():
            raise DeterminismError(
                f"variant staging root is a symlink: {variant_root}"
            )
        variant_root.mkdir(parents=True, exist_ok=True)
        if not _path_within(build_root, source_root):
            raise DeterminismError(
                "build root must be inside the mirrored source root"
            )
        self.mirror_root = _variant_mirror_root(
            source_root, variant_root
        )
        self.ordinary_by_path = {
            path.as_posix(): _variant_mirror_path(
                path, source_root, variant_root
            ).as_posix()
            for _relative, path in ordinary_outputs.items()
        }
        mirror_replacements = dict(self.ordinary_by_path)
        mirror_replacements[source_root.as_posix()] = (
            self.mirror_root.as_posix()
        )
        self.input_rewriter = PathRewriter(mirror_replacements)
        self.output_rewriter = PathRewriter(mirror_replacements)
        self.original_root_spelling = source_root.as_posix()
        self.staged_files: set[Path] = set()
        self.staged_trees: set[Path] = set()
        self.staged_responses: dict[Path, tuple[Path, Path]] = {}
        self.active_responses: set[Path] = set()

    def _project_root_for(self, path: Path) -> Path | None:
        if _path_within(path, self.build_root):
            return self.build_root
        if _path_within(path, self.source_root):
            return self.source_root
        return None

    def _ordinary_destination(self, path: Path) -> Path | None:
        destination = self.ordinary_by_path.get(path.as_posix())
        return None if destination is None else Path(destination)

    def input_path(self, path: Path) -> Path:
        ordinary = self._ordinary_destination(path)
        if ordinary is not None:
            return ordinary
        return Path(self.input_rewriter.rewrite(path.as_posix()))

    def output_path(self, path: Path) -> Path:
        ordinary = self._ordinary_destination(path)
        if ordinary is not None:
            return ordinary
        return Path(self.output_rewriter.rewrite(path.as_posix()))

    def _prepare_destination(self, destination: Path) -> None:
        if not _path_within(destination, self.variant_root):
            raise DeterminismError(
                "staged project input escapes its variant root: "
                f"{destination}"
            )
        relative = destination.relative_to(self.variant_root)
        cursor = self.variant_root
        for component in relative.parts[:-1]:
            cursor = cursor / component
            if cursor.is_symlink():
                raise DeterminismError(
                    f"staged input parent is a symlink: {cursor}"
                )
            cursor.mkdir(exist_ok=True)

    def stage_regular(self, source: Path, label: str) -> Path:
        source = _lexical_absolute(source, Path.cwd())
        project_root = self._project_root_for(source)
        if project_root is None:
            return source
        ordinary = self._ordinary_destination(source)
        if ordinary is not None:
            return ordinary
        destination = self.input_path(source)
        if destination in self.staged_files:
            return destination
        _assert_source_path_has_no_symlinks(source, project_root, label)
        try:
            metadata = source.lstat()
        except FileNotFoundError as error:
            raise DeterminismError(
                f"{label} does not exist: {source}"
            ) from error
        if not stat.S_ISREG(metadata.st_mode):
            raise DeterminismError(
                f"{label} is not a regular file: {source}"
            )
        _require_single_link(metadata, source, f"configured {label}")
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(source, flags)
        except OSError as error:
            raise DeterminismError(
                f"cannot open configured {label} without following "
                f"symlinks: {source}"
            ) from error
        try:
            opened_metadata = os.fstat(descriptor)
            if not stat.S_ISREG(opened_metadata.st_mode):
                raise DeterminismError(
                    f"{label} is not a regular file: {source}"
                )
            _require_single_link(
                opened_metadata, source, f"configured {label}"
            )
            if _stat_identity(metadata) != _stat_identity(opened_metadata):
                raise DeterminismError(
                    f"configured {label} changed before it was read: "
                    f"{source}"
                )
            with os.fdopen(descriptor, "rb", closefd=False) as input_file:
                content = input_file.read()
            metadata_after_read = os.fstat(descriptor)
            _require_single_link(
                metadata_after_read, source, f"configured {label}"
            )
            if _stat_identity(opened_metadata) != _stat_identity(
                metadata_after_read
            ):
                raise DeterminismError(
                    f"configured {label} changed while it was read: "
                    f"{source}"
                )

            content_text = content.decode("latin-1")
            has_path = _path_occurrence_count(
                content_text,
                self.original_root_spelling.encode("utf-8").decode(
                    "latin-1"
                ),
            )
            normalized_content = content
            if has_path:
                try:
                    decoded = content.decode("utf-8")
                except UnicodeDecodeError as error:
                    raise DeterminismError(
                        f"{label} is binary and embeds an original project "
                        f"path: {source}"
                    ) from error
                normalized_content = self.input_rewriter.rewrite(
                    decoded
                ).encode("utf-8")
            self._prepare_destination(destination)
            if destination.exists() or destination.is_symlink():
                destination_metadata = destination.lstat()
                if not stat.S_ISREG(destination_metadata.st_mode):
                    raise DeterminismError(
                        f"staged {label} is not a regular file: "
                        f"{destination}"
                    )
                _require_single_link(
                    destination_metadata,
                    destination,
                    f"staged {label}",
                )
                if destination.read_bytes() != normalized_content:
                    raise DeterminismError(
                        f"staged {label} collides with different bytes: "
                        f"{destination}"
                    )
            _replace_with_isolated_bytes(
                destination,
                normalized_content,
                stat.S_IMODE(opened_metadata.st_mode),
                label,
            )
            metadata_after_copy = os.fstat(descriptor)
            _require_single_link(
                metadata_after_copy, source, f"configured {label}"
            )
            if _stat_identity(opened_metadata) != _stat_identity(
                metadata_after_copy
            ):
                raise DeterminismError(
                    f"configured {label} changed while it was staged: "
                    f"{source}"
                )
        finally:
            os.close(descriptor)
        metadata_after = source.lstat()
        _require_single_link(
            metadata_after, source, f"configured {label}"
        )
        if _stat_identity(metadata) != _stat_identity(metadata_after):
            raise DeterminismError(
                f"configured {label} changed while it was staged: {source}"
            )
        self.staged_files.add(destination)
        return destination

    def stage_tree(self, source: Path, label: str) -> Path:
        source = _lexical_absolute(source, Path.cwd())
        project_root = self._project_root_for(source)
        if project_root is None:
            return source
        _assert_source_path_has_no_symlinks(source, project_root, label)
        if not source.is_dir():
            return self.stage_regular(source, label)
        if _path_within(self.variant_root, source):
            raise DeterminismError(
                f"{label} would recursively stage its variant root: {source}"
            )
        destination = self.input_path(source)
        if any(
            source == staged or _path_within(source, staged)
            for staged in self.staged_trees
        ):
            return destination
        destination.mkdir(parents=True, exist_ok=True)
        for directory, names, filenames in os.walk(
            source, topdown=True, followlinks=False
        ):
            names.sort()
            filenames.sort()
            directory_path = Path(directory)
            for name in names:
                child = directory_path / name
                if child.is_symlink():
                    raise DeterminismError(
                        f"{label} contains a symlink directory: {child}"
                    )
            for filename in filenames:
                child = directory_path / filename
                if child.is_symlink():
                    raise DeterminismError(
                        f"{label} contains a symlink file: {child}"
                    )
                if not child.is_file():
                    raise DeterminismError(
                        f"{label} contains a non-regular input: {child}"
                    )
                self.stage_regular(child, label)
        self.staged_trees.add(source)
        return destination

    def _normalize_input_path(
        self,
        value: str,
        original_directory: Path,
        label: str,
        stage_content: bool = True,
    ) -> str:
        source = _lexical_absolute(Path(value), original_directory)
        if self._project_root_for(source) is None:
            return value
        destination = self.input_path(source)
        if stage_content:
            if source.is_dir():
                destination = self.stage_tree(source, label)
            elif source.exists() or self._ordinary_destination(source):
                destination = self.stage_regular(source, label)
            else:
                raise DeterminismError(
                    f"{label} references a missing project input: {source}"
                )
        return destination.as_posix()

    def _normalize_output_path(
        self, value: str, original_directory: Path
    ) -> str:
        path = _lexical_absolute(Path(value), original_directory)
        destination = self.output_path(path)
        if _path_within(destination, self.variant_root):
            destination.parent.mkdir(parents=True, exist_ok=True)
        return destination.as_posix()

    def _normalize_module_mapping(
        self,
        value: str,
        original_directory: Path,
        label: str,
        stage_content: bool,
    ) -> str:
        module, separator, path = value.partition("=")
        if not separator:
            return self._normalize_input_path(
                value,
                original_directory,
                label,
                stage_content=stage_content,
            )
        if not module or not path:
            raise DeterminismError(
                f"{label} has a malformed module mapping: {value!r}"
            )
        normalized = self._normalize_input_path(
            path,
            original_directory,
            label,
            stage_content=stage_content,
        )
        return f"{module}={normalized}"

    def stage_response(
        self,
        value: str,
        original_directory: Path,
    ) -> str:
        source = _lexical_absolute(Path(value), original_directory)
        if self._project_root_for(source) is None:
            return source.as_posix()
        _assert_source_path_has_no_symlinks(
            source,
            self._project_root_for(source) or self.source_root,
            "compile response file",
        )
        if source in self.active_responses:
            raise DeterminismError(
                f"compile response-file cycle at {source}"
            )
        if not source.is_file():
            raise DeterminismError(
                f"compile response file does not exist: {source}"
            )
        destination = self.input_path(source)
        previous = self.staged_responses.get(source)
        if previous is not None:
            previous_directory, previous_destination = previous
            if previous_directory != original_directory:
                raise DeterminismError(
                    "compile response file is referenced from multiple "
                    f"directories: {source}"
                )
            return previous_destination.as_posix()
        self.active_responses.add(source)
        try:
            try:
                tokens = shlex.split(
                    source.read_text(encoding="utf-8"), posix=True
                )
            except (UnicodeDecodeError, ValueError) as error:
                raise DeterminismError(
                    f"cannot parse compile response file {source}: {error}"
                ) from error
            generated_cmake_modmap = (
                source.name.endswith(".o.modmap")
                and "CMakeFiles" in source.parts
            )
            normalized = self.normalize_arguments(
                tokens,
                original_directory,
                stage_module_inputs=not generated_cmake_modmap,
            )
            self._prepare_destination(destination)
            _atomic_write(
                destination,
                (shlex.join(normalized) + "\n").encode("utf-8"),
            )
            self.staged_files.add(destination)
            self.staged_responses[source] = (
                original_directory,
                destination,
            )
        finally:
            self.active_responses.remove(source)
        return destination.as_posix()

    def normalize_arguments(
        self,
        arguments: Sequence[str],
        original_directory: Path,
        *,
        stage_module_inputs: bool = True,
    ) -> list[str]:
        normalized: list[str] = []
        index = 0
        while index < len(arguments):
            argument = arguments[index]
            if argument == "-fmodule-file":
                if index + 1 >= len(arguments):
                    raise DeterminismError(
                        "compile -fmodule-file flag has no value"
                    )
                normalized.extend(
                    [
                        argument,
                        self._normalize_module_mapping(
                            arguments[index + 1],
                            original_directory,
                            "compile module-cache input",
                            stage_content=stage_module_inputs,
                        ),
                    ]
                )
                index += 2
                continue
            if argument in SEPARATE_INPUT_PATH_FLAGS:
                if index + 1 >= len(arguments):
                    raise DeterminismError(
                        f"compile path flag has no value: {argument}"
                    )
                normalized.extend(
                    [
                        argument,
                        self._normalize_input_path(
                            arguments[index + 1],
                            original_directory,
                            f"compile {argument} input",
                        ),
                    ]
                )
                index += 2
                continue
            if argument in SEPARATE_OUTPUT_PATH_FLAGS:
                if index + 1 >= len(arguments):
                    raise DeterminismError(
                        f"compile output flag has no value: {argument}"
                    )
                normalized.extend(
                    [
                        argument,
                        self._normalize_output_path(
                            arguments[index + 1], original_directory
                        ),
                    ]
                )
                index += 2
                continue
            if argument.startswith("@") and len(argument) > 1:
                normalized.append(
                    "@"
                    + self.stage_response(
                        argument[1:], original_directory
                    )
                )
                index += 1
                continue
            if argument.startswith("-fmodule-file="):
                value = argument[len("-fmodule-file=") :]
                normalized.append(
                    "-fmodule-file="
                    + self._normalize_module_mapping(
                        value,
                        original_directory,
                        "compile module-cache input",
                        stage_content=stage_module_inputs,
                    )
                )
                index += 1
                continue
            joined = False
            for prefix in JOINED_INPUT_PATH_PREFIXES:
                if not argument.startswith(prefix) or argument == prefix:
                    continue
                normalized.append(
                    prefix
                    + self._normalize_input_path(
                        argument[len(prefix) :],
                        original_directory,
                        f"compile {prefix} input",
                        stage_content=stage_module_inputs,
                    )
                )
                joined = True
                break
            if joined:
                index += 1
                continue
            for prefix in JOINED_OUTPUT_PATH_PREFIXES:
                if not argument.startswith(prefix) or argument == prefix:
                    continue
                if prefix == "-o" and not (
                    "/" in argument[2:]
                    or argument[2:].startswith(".")
                    or "." in argument[2:]
                ):
                    continue
                normalized.append(
                    prefix
                    + self._normalize_output_path(
                        argument[len(prefix) :], original_directory
                    )
                )
                joined = True
                break
            if joined:
                index += 1
                continue
            if argument and argument[0] != "-":
                candidate = _lexical_absolute(
                    Path(argument), original_directory
                )
                if self._project_root_for(candidate) is not None:
                    if candidate.exists() or self._ordinary_destination(
                        candidate
                    ):
                        normalized.append(
                            self._normalize_input_path(
                                argument,
                                original_directory,
                                "compile positional input",
                            )
                        )
                        index += 1
                        continue
            normalized.append(self.input_rewriter.rewrite(argument))
            index += 1
        return normalized


def _compile_arguments(row: Mapping[str, object], row_number: int) -> list[str]:
    arguments = row.get("arguments")
    command = row.get("command")
    if arguments is not None:
        if (
            not isinstance(arguments, list)
            or not arguments
            or not all(isinstance(item, str) for item in arguments)
        ):
            raise DeterminismError(
                f"raw compile database row {row_number} has invalid arguments"
            )
        return list(arguments)
    if not isinstance(command, str) or not command:
        raise DeterminismError(
            f"raw compile database row {row_number} has no command"
        )
    try:
        parsed = shlex.split(command, posix=True)
    except ValueError as error:
        raise DeterminismError(
            f"cannot parse raw compile command at row {row_number}: {error}"
        ) from error
    if not parsed:
        raise DeterminismError(
            f"raw compile database row {row_number} has an empty command"
        )
    return parsed


def _compile_standard(arguments: Sequence[str]) -> tuple[str, ...]:
    """Return the exact language-standard selection from a compile command."""
    result: list[str] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "-std":
            if index + 1 >= len(arguments):
                raise DeterminismError("compile -std flag has no value")
            result.append(arguments[index + 1])
            index += 2
            continue
        if argument.startswith("-std="):
            result.append(argument.removeprefix("-std="))
        index += 1
    return tuple(result)


def _validate_owned_bmi_compile_database_payload(
    database_value: object,
    manifest: Mapping[str, object],
    expected_sources: Iterable[str],
    label: str,
) -> None:
    modules_value = manifest.get("modules")
    pcm_root_value = manifest.get("pcm_root")
    if (
        not isinstance(database_value, list)
        or not database_value
        or not isinstance(modules_value, list)
        or not modules_value
        or not isinstance(pcm_root_value, str)
        or not pcm_root_value
    ):
        raise DeterminismError(
            f"{label} has no owned command/module/PCM-root closure"
        )
    pcm_root = Path(os.path.normpath(pcm_root_value))
    modules_by_name: dict[str, Mapping[str, object]] = {}
    modules_by_source: dict[str, Mapping[str, object]] = {}
    for index, module_value in enumerate(modules_value):
        if not isinstance(module_value, dict):
            raise DeterminismError(
                f"{label} manifest module {index} is not an object"
            )
        name = module_value.get("name")
        source = module_value.get("source")
        if (
            not isinstance(name, str)
            or not name
            or not isinstance(source, str)
            or not source
            or name in modules_by_name
            or source in modules_by_source
        ):
            raise DeterminismError(
                f"{label} manifest module inventory is ambiguous"
            )
        modules_by_name[name] = module_value
        modules_by_source[source] = module_value

    def mapping_order(module: Mapping[str, object]) -> list[str]:
        direct = module.get("direct_imports")
        if not isinstance(direct, list) or any(
            not isinstance(item, str) or not item for item in direct
        ):
            raise DeterminismError(
                f"{label} manifest direct-import closure is malformed"
            )
        reached: set[str] = set()
        active: set[str] = set()
        ordered: list[str] = []

        def visit(name: str) -> None:
            if name in reached:
                return
            if name in active or name not in modules_by_name:
                raise DeterminismError(
                    f"{label} manifest import closure is cyclic or foreign"
                )
            active.add(name)
            dependency = modules_by_name[name]
            dependency_imports = dependency.get("direct_imports")
            if not isinstance(dependency_imports, list) or any(
                not isinstance(item, str) or not item
                for item in dependency_imports
            ):
                raise DeterminismError(
                    f"{label} manifest import closure is malformed"
                )
            for imported in dependency_imports:
                visit(imported)
            active.remove(name)
            reached.add(name)
            ordered.append(name)

        for imported in direct:
            visit(imported)
        return ordered

    expected_source_set = set(expected_sources)
    if not expected_source_set or not expected_source_set.issubset(
        modules_by_source
    ):
        raise DeterminismError(
            f"{label} stage-root authority is empty or foreign"
        )
    seen_sources: set[str] = set()
    for row_number, row_value in enumerate(database_value, start=1):
        if not isinstance(row_value, dict) or set(row_value) != {
            "arguments",
            "directory",
            "file",
        }:
            raise DeterminismError(
                f"{label} row {row_number} is not one exact owned command"
            )
        source = row_value["file"]
        directory = row_value["directory"]
        arguments = row_value["arguments"]
        if (
            not isinstance(source, str)
            or source not in modules_by_source
            or source in seen_sources
            or not isinstance(directory, str)
            or not isinstance(arguments, list)
            or not arguments
            or any(not isinstance(item, str) for item in arguments)
        ):
            raise DeterminismError(
                f"{label} row {row_number} has an ambiguous owner"
            )
        seen_sources.add(source)
        module = modules_by_source[source]
        pcm = module.get("pcm")
        base_arguments = module.get("base_arguments")
        module_directory = module.get("directory")
        if (
            not isinstance(pcm, str)
            or not pcm
            or not isinstance(base_arguments, list)
            or not base_arguments
            or any(not isinstance(item, str) for item in base_arguments)
            or directory != module_directory
        ):
            raise DeterminismError(
                f"{label} row {row_number} differs from its manifest owner"
            )
        reduced_outputs = [
            argument.removeprefix("-fmodule-output=")
            for argument in arguments
            if argument.startswith("-fmodule-output=")
        ]
        if len(reduced_outputs) != 1:
            raise DeterminismError(
                f"{label} row {row_number} has an ambiguous reduced "
                "BMI output"
            )
        reduced_output = reduced_outputs[0]
        if (
            reduced_output != pcm
            or not _path_within(
                Path(os.path.normpath(reduced_output)), pcm_root
            )
        ):
            raise DeterminismError(
                f"{label} row {row_number} has a foreign reduced BMI output"
            )
        full_pcm = str(
            Path(os.path.normpath(pcm)).with_suffix(".full.pcm")
        )
        output_indices = [
            index for index, argument in enumerate(arguments)
            if argument == "-o"
        ]
        if len(output_indices) != 1:
            raise DeterminismError(
                f"{label} row {row_number} has an ambiguous full BMI output"
            )
        output_index = output_indices[0]
        if output_index + 1 >= len(arguments):
            raise DeterminismError(
                f"{label} row {row_number} has a value-less BMI output"
            )
        output = arguments[output_index + 1]
        if (
            output != full_pcm
            or not _path_within(
                Path(os.path.normpath(output)), pcm_root
            )
        ):
            raise DeterminismError(
                f"{label} row {row_number} has a foreign full BMI output"
            )
        expected_mappings = [
            "-fmodule-file=" + imported + "="
            + str(modules_by_name[imported]["pcm"])
            for imported in mapping_order(module)
        ]
        expected_suffix = [
            "--precompile",
            "-fmodules-reduced-bmi",
            "-fmodule-output=" + pcm,
            source,
            "-o",
            full_pcm,
            "-MD",
            "-MF",
            pcm + ".d",
            "-MT",
            pcm,
        ]
        expected = [*base_arguments, *expected_mappings, *expected_suffix]
        if arguments != expected:
            raise DeterminismError(
                f"{label} row {row_number} is not the exact BMI-only "
                "--precompile command"
            )
    if seen_sources != expected_source_set:
        raise DeterminismError(
            f"{label} does not own every authenticated stage root "
            "exactly once"
        )


def _validate_owned_bmi_compile_database(
    compile_database: Path,
    pcm_manifest: Path,
    expected_sources: Iterable[str],
    label: str,
) -> None:
    try:
        database_value = json.loads(
            compile_database.read_text(encoding="utf-8")
        )
        manifest_value = json.loads(
            pcm_manifest.read_text(encoding="utf-8")
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise DeterminismError(
            f"{label} is not valid UTF-8 JSON: {error}"
        ) from error
    if not isinstance(manifest_value, dict):
        raise DeterminismError(f"{label} manifest is not an object")
    _validate_owned_bmi_compile_database_payload(
        database_value, manifest_value, expected_sources, label
    )


def _top_level_response_paths(
    arguments: Sequence[str],
    directory: Path,
) -> tuple[Path, ...]:
    return tuple(
        _lexical_absolute(Path(argument[1:]), directory)
        for argument in arguments
        if argument.startswith("@") and len(argument) > 1
    )


def _is_generated_cmake_modmap(path: Path) -> bool:
    return path.name.endswith(".o.modmap") and "CMakeFiles" in path.parts


def _cmake_target_for_modmap(
    path: Path,
    build_root: Path,
) -> tuple[str, Path]:
    if not _is_generated_cmake_modmap(path):
        raise DeterminismError(
            f"response path is not a generated CMake module map: {path}"
        )
    if not _path_within(path, build_root):
        raise DeterminismError(
            f"generated CMake module map escapes the build root: {path}"
        )
    target_directory = next(
        (
            parent
            for parent in path.parents
            if parent.name.endswith(".dir")
            and parent.parent.name == "CMakeFiles"
        ),
        None,
    )
    if target_directory is None:
        raise DeterminismError(
            f"generated CMake module map has no target directory: {path}"
        )
    target = target_directory.name.removesuffix(".dir")
    if (
        not target
        or any(character in target for character in "\t\r\n/\\")
    ):
        raise DeterminismError(
            f"generated CMake module map has an invalid target: {path}"
        )
    return target, target_directory / "CXX.dd"


def _read_required_target_dyndeps(
    inventory_path: Path,
    build_root: Path,
) -> dict[str, Path]:
    inventory = _require_regular_file(
        inventory_path, "required target dyndep inventory"
    )
    try:
        rows = list(
            csv.reader(
                inventory.read_text(encoding="utf-8").splitlines(),
                delimiter="\t",
                strict=True,
            )
        )
    except (csv.Error, UnicodeDecodeError) as error:
        raise DeterminismError(
            f"required target dyndep inventory is malformed: {error}"
        ) from error
    expected_prefix = [
        ["schema", TARGET_DYNDEP_INVENTORY_SCHEMA],
        ["target", "dyndep"],
    ]
    if rows[:2] != expected_prefix or len(rows) <= 2:
        raise DeterminismError(
            "required target dyndep inventory has an invalid schema/header"
        )

    result: dict[str, Path] = {}
    observed_paths: set[Path] = set()
    for line_number, row in enumerate(rows[2:], start=3):
        if len(row) != 2 or not row[0] or not row[1]:
            raise DeterminismError(
                "required target dyndep inventory row "
                f"{line_number} is not target<TAB>dyndep"
            )
        target, raw_path = row
        if any(character in target for character in "\t\r\n/\\"):
            raise DeterminismError(
                "required target dyndep inventory has an invalid target "
                f"at line {line_number}: {target!r}"
            )
        if target in result:
            raise DeterminismError(
                "required target dyndep inventory contains duplicate target "
                f"{target}"
            )
        path = Path(raw_path)
        if not path.is_absolute():
            raise DeterminismError(
                "required target dyndep path is not absolute at line "
                f"{line_number}: {path}"
            )
        path = _lexical_absolute(path, build_root)
        expected_suffix = Path("CMakeFiles") / f"{target}.dir" / "CXX.dd"
        if (
            not _path_within(path, build_root)
            or path.name != "CXX.dd"
            or path.parent.name != f"{target}.dir"
            or path.parent.parent.name != "CMakeFiles"
            or not path.as_posix().endswith(expected_suffix.as_posix())
        ):
            raise DeterminismError(
                "required target dyndep path does not match its target at "
                f"line {line_number}: {target} -> {path}"
            )
        if path in observed_paths:
            raise DeterminismError(
                "required target dyndep inventory contains duplicate path "
                f"{path}"
            )
        if not path.exists():
            raise DeterminismError(
                f"required target dyndep does not exist: {path}"
            )
        if path.is_symlink() or not path.is_file():
            raise DeterminismError(
                f"required target dyndep is not a regular non-symlink file: "
                f"{path}"
            )
        resolved = path.resolve(strict=True)
        if resolved != path:
            raise DeterminismError(
                f"required target dyndep path is not canonical: {path}"
            )
        result[target] = path
        observed_paths.add(path)
    if tuple(result) != tuple(sorted(result)):
        raise DeterminismError(
            "required target dyndep inventory targets are not sorted"
        )
    return result


def _validate_required_target_dyndep_coverage(
    inventory_path: Path,
    raw_compile_database: Path,
    build_root: Path,
) -> dict[str, Path]:
    required_dyndeps = _read_required_target_dyndeps(
        inventory_path, build_root
    )
    try:
        payload = json.loads(
            raw_compile_database.read_text(encoding="utf-8")
        )
    except json.JSONDecodeError as error:
        raise DeterminismError(
            f"raw compile database is not valid JSON: {error}"
        ) from error
    if not isinstance(payload, list) or not payload:
        raise DeterminismError(
            "raw compile database must be a non-empty JSON array"
        )

    compile_targets: set[str] = set()
    response_classes: dict[
        tuple[str, tuple[str, ...]],
        list[tuple[tuple[str, ...], tuple[Path, ...], bool]],
    ] = {}
    for row_number, raw_row in enumerate(payload, start=1):
        if not isinstance(raw_row, dict):
            raise DeterminismError(
                f"raw compile database row {row_number} is not an object"
            )
        directory_text = raw_row.get("directory")
        file_text = raw_row.get("file")
        if not isinstance(directory_text, str) or not directory_text:
            raise DeterminismError(
                f"raw compile database row {row_number} has no directory"
            )
        if not isinstance(file_text, str) or not file_text:
            raise DeterminismError(
                f"raw compile database row {row_number} has no file"
            )
        original_directory = _lexical_absolute(
            Path(directory_text), raw_compile_database.parent
        )
        original_file = _lexical_absolute(
            Path(file_text), original_directory
        )
        if original_file.suffix != ".cppm":
            continue
        arguments = _compile_arguments(raw_row, row_number)
        response_paths = _top_level_response_paths(
            arguments, original_directory
        )
        if not response_paths:
            continue

        row_targets: set[str] = set()
        generated_responses: set[Path] = set()
        for response_path in response_paths:
            if not _is_generated_cmake_modmap(response_path):
                continue
            target, target_dyndep = _cmake_target_for_modmap(
                response_path, build_root
            )
            row_targets.add(target)
            generated_responses.add(response_path)
            compile_targets.add(target)
            required = required_dyndeps.get(target)
            if required is not None and required != target_dyndep:
                raise DeterminismError(
                    "required target dyndep differs from raw compile "
                    f"database target path: {target}: "
                    f"{required} != {target_dyndep}"
                )

        missing_responses = tuple(
            path
            for path in response_paths
            if (
                not path.exists()
                or path.is_symlink()
                or not path.is_file()
            )
        )
        missing_authored = tuple(
            path
            for path in missing_responses
            if path not in generated_responses
        )
        if missing_authored:
            raise DeterminismError(
                "authored compile response file is unavailable at raw "
                f"row {row_number}: {missing_authored}"
            )
        response_key = (
            original_file.as_posix(),
            _compile_standard(arguments),
        )
        response_classes.setdefault(response_key, []).append(
            (
                tuple(sorted(row_targets)),
                missing_responses,
                not missing_responses,
            )
        )

    absent_targets = sorted(set(required_dyndeps) - compile_targets)
    if absent_targets:
        raise DeterminismError(
            "required target dyndep anchors are absent from the raw compile "
            f"database: {absent_targets}"
        )
    for (source, standard), rows in sorted(response_classes.items()):
        if any(viable for _, _, viable in rows):
            continue
        targets = sorted(
            {
                target
                for row_targets, _, _ in rows
                for target in row_targets
            }
        )
        missing = sorted(
            {
                path.as_posix()
                for _, missing_paths, _ in rows
                for path in missing_paths
            }
        )
        raise DeterminismError(
            "required target dyndep coverage has an uncovered compile "
            f"equivalence class: source={source}, standard={standard}, "
            f"targets={targets}, missing={missing}"
        )
    return required_dyndeps


def _stage_raw_compile_database(
    raw_compile_database: Path,
    ordinary_outputs: Mapping[str, Path],
    source_root: Path,
    build_root: Path,
    variant_root: Path,
    required_sources: Iterable[str],
) -> StagedCompilation:
    try:
        payload = json.loads(
            raw_compile_database.read_text(encoding="utf-8")
        )
    except json.JSONDecodeError as error:
        raise DeterminismError(
            f"raw compile database is not valid JSON: {error}"
        ) from error
    if not isinstance(payload, list) or not payload:
        raise DeterminismError(
            "raw compile database must be a non-empty JSON array"
        )
    required = {
        _lexical_absolute(Path(source), source_root).as_posix()
        for source in required_sources
    }

    # CMake may publish duplicate compile-database rows for the same libc++
    # standard-module source and language standard. A target that was never
    # built can leave its generated dyndep .o.modmap absent even though an
    # equivalent, materialized row is present. Keep the materialized row and
    # discard only that exact missing generated duplicate. Authored response
    # files and a generated modmap with no viable equivalent remain fail-closed.
    viable_response_rows: set[tuple[str, tuple[str, ...]]] = set()
    for row_number, raw_row in enumerate(payload, start=1):
        if not isinstance(raw_row, dict):
            continue
        directory_text = raw_row.get("directory")
        file_text = raw_row.get("file")
        if (
            not isinstance(directory_text, str)
            or not directory_text
            or not isinstance(file_text, str)
            or not file_text
        ):
            continue
        original_directory = _lexical_absolute(
            Path(directory_text), raw_compile_database.parent
        )
        original_file = _lexical_absolute(
            Path(file_text), original_directory
        )
        if original_file.suffix != ".cppm":
            continue
        arguments = _compile_arguments(raw_row, row_number)
        response_paths = _top_level_response_paths(
            arguments, original_directory
        )
        if all(path.is_file() for path in response_paths):
            viable_response_rows.add(
                (
                    original_file.as_posix(),
                    _compile_standard(arguments),
                )
            )

    stager = VariantInputStager(
        source_root,
        build_root,
        variant_root,
        ordinary_outputs,
    )
    destination = _variant_mirror_path(
        raw_compile_database, source_root, variant_root
    )
    selected: list[dict[str, object]] = []
    relocated_sources: dict[str, Path] = {}
    for row_number, raw_row in enumerate(payload, start=1):
        if not isinstance(raw_row, dict):
            raise DeterminismError(
                f"raw compile database row {row_number} is not an object"
            )
        directory_text = raw_row.get("directory")
        file_text = raw_row.get("file")
        if not isinstance(directory_text, str) or not directory_text:
            raise DeterminismError(
                f"raw compile database row {row_number} has no directory"
            )
        if not isinstance(file_text, str) or not file_text:
            raise DeterminismError(
                f"raw compile database row {row_number} has no file"
            )
        original_directory = _lexical_absolute(
            Path(directory_text), raw_compile_database.parent
        )
        original_file = _lexical_absolute(
            Path(file_text), original_directory
        )
        if original_file.suffix != ".cppm":
            continue
        arguments = _compile_arguments(raw_row, row_number)
        response_paths = _top_level_response_paths(
            arguments, original_directory
        )
        missing_responses = tuple(
            path for path in response_paths if not path.is_file()
        )
        response_key = (
            original_file.as_posix(),
            _compile_standard(arguments),
        )
        if (
            missing_responses
            and all(
                _is_generated_cmake_modmap(path)
                for path in missing_responses
            )
            and response_key in viable_response_rows
        ):
            continue
        relocated_file = stager.input_path(original_file)
        if stager._project_root_for(original_file) is not None:
            if stager._ordinary_destination(original_file) is None:
                relocated_file = stager.stage_regular(
                    original_file,
                    f"compile source at raw row {row_number}",
                )
            relocated_sources[original_file.as_posix()] = relocated_file
        relocated_directory = stager.input_path(original_directory)
        if _path_within(relocated_directory, variant_root):
            relocated_directory.mkdir(parents=True, exist_ok=True)
        arguments = stager.normalize_arguments(
            arguments,
            original_directory,
        )
        entry: dict[str, object] = {
            "arguments": arguments,
            "directory": relocated_directory.as_posix(),
            "file": relocated_file.as_posix(),
        }
        output = raw_row.get("output")
        if output is not None:
            if not isinstance(output, str) or not output:
                raise DeterminismError(
                    f"raw compile database row {row_number} has invalid output"
                )
            entry["output"] = stager._normalize_output_path(
                output, original_directory
            )
        selected.append(entry)
    missing_sources = sorted(required - set(relocated_sources))
    if missing_sources:
        raise DeterminismError(
            "raw compile database does not contain every production owner "
            f"source: {missing_sources}"
        )
    _atomic_write(
        destination,
        (
            json.dumps(selected, indent=2, sort_keys=True) + "\n"
        ).encode("utf-8"),
    )
    stager.staged_files.add(destination)
    return StagedCompilation(
        compile_database=destination.resolve(strict=True),
        relocated_sources={
            source: relocated_sources[source]
            for source in sorted(required)
        },
        physical_outputs={
            relative: _variant_mirror_path(
                path, source_root, variant_root
            )
            for relative, path in sorted(ordinary_outputs.items())
        },
        mirror_root=stager.mirror_root,
        staged_files=tuple(sorted(stager.staged_files)),
        selected_entry_count=len(selected),
        staged_file_count=len(stager.staged_files),
    )


def _variant_contexts(
    source_root: Path,
    variant_root: Path,
    ordinary_context_root: Path,
) -> dict[str, GenerationContext]:
    result: dict[str, GenerationContext] = {}
    for stage in STAGES:
        ordinary_stage_root = (
            ordinary_context_root / "production" / stage
            if stage in {"support", "final"}
            else ordinary_context_root / "core"
        )
        ordinary_pcm_root = (
            ordinary_context_root / "production" / "pcm"
            if stage in {"support", "final"}
            else ordinary_context_root / "core" / "pcm"
        )
        context_root = _variant_mirror_path(
            ordinary_stage_root, source_root, variant_root
        )
        pcm_root = _variant_mirror_path(
            ordinary_pcm_root, source_root, variant_root
        )
        context_root.mkdir(parents=True, exist_ok=True)
        pcm_root.mkdir(parents=True, exist_ok=True)
        result[stage] = GenerationContext(
            compile_database=context_root / "compile_commands.json",
            pcm_root=pcm_root,
            pcm_manifest=context_root / "pcm-manifest.json",
            depfile=context_root / "prepare.d",
            stamp=context_root / "prepare.stamp",
        )
    test_support_root = _variant_mirror_path(
        ordinary_context_root / "test_indexes" / "support",
        source_root,
        variant_root,
    )
    test_support_pcm_root = _variant_mirror_path(
        ordinary_context_root / "test_indexes" / "pcm",
        source_root,
        variant_root,
    )
    test_support_root.mkdir(parents=True, exist_ok=True)
    test_support_pcm_root.mkdir(parents=True, exist_ok=True)
    result["test_support"] = GenerationContext(
        compile_database=test_support_root / "compile_commands.json",
        pcm_root=test_support_pcm_root,
        pcm_manifest=test_support_root / "pcm-manifest.json",
        depfile=test_support_root / "prepare.d",
        stamp=test_support_root / "prepare.stamp",
    )
    return result


def _stamp_outputs() -> frozenset[str]:
    return frozenset(
        {
            "artifacts/support/support.stamp",
            "artifacts/test-support/support.stamp",
            "artifacts/core/core.stamp",
            "artifacts/final/bioseq-sql.stamp",
        }
    )


def _snapshot_tree(root: Path, label: str) -> dict[str, FileState]:
    if root.is_symlink() or not root.is_dir():
        raise DeterminismError(f"{label} root is not a real directory: {root}")
    snapshot: dict[str, FileState] = {}
    for path in root.rglob("*"):
        if path.is_symlink():
            raise DeterminismError(f"{label} output is a symlink: {path}")
        if path.is_dir():
            continue
        metadata = path.stat()
        if not stat.S_ISREG(metadata.st_mode):
            raise DeterminismError(
                f"{label} output is not a regular file: {path}"
            )
        _require_single_link(metadata, path, f"{label} output")
        content = path.read_bytes()
        metadata_after = path.stat()
        _require_single_link(
            metadata_after, path, f"{label} output"
        )
        if (
            metadata.st_ino,
            metadata.st_nlink,
            metadata.st_size,
            metadata.st_mode,
            metadata.st_mtime_ns,
            metadata.st_ctime_ns,
        ) != (
            metadata_after.st_ino,
            metadata_after.st_nlink,
            metadata_after.st_size,
            metadata_after.st_mode,
            metadata_after.st_mtime_ns,
            metadata_after.st_ctime_ns,
        ):
            raise DeterminismError(
                f"{label} output changed while it was read: {path}"
            )
        relative = path.relative_to(root.parent).as_posix()
        if relative in snapshot:
            raise DeterminismError(
                f"{label} output identity is duplicated: {relative}"
            )
        snapshot[relative] = FileState(
            content=content,
            sha256=_sha256_bytes(content),
            size=metadata.st_size,
            mode=stat.S_IMODE(metadata.st_mode),
            inode=metadata.st_ino,
            mtime_ns=metadata.st_mtime_ns,
            ctime_ns=metadata.st_ctime_ns,
        )
    if not snapshot:
        raise DeterminismError(f"{label} output closure is empty")
    return snapshot


def _read_file_state(path: Path, label: str) -> FileState:
    if path.is_symlink():
        raise DeterminismError(f"{label} is a symlink: {path}")
    regular = _require_regular_file(path, label)
    metadata = regular.stat()
    _require_single_link(metadata, regular, label)
    content = regular.read_bytes()
    metadata_after = regular.stat()
    _require_single_link(metadata_after, regular, label)
    if (
        metadata.st_ino,
        metadata.st_nlink,
        metadata.st_size,
        metadata.st_mode,
        metadata.st_mtime_ns,
        metadata.st_ctime_ns,
    ) != (
        metadata_after.st_ino,
        metadata_after.st_nlink,
        metadata_after.st_size,
        metadata_after.st_mode,
        metadata_after.st_mtime_ns,
        metadata_after.st_ctime_ns,
    ):
        raise DeterminismError(f"{label} changed while it was read: {regular}")
    return FileState(
        content=content,
        sha256=_sha256_bytes(content),
        size=metadata.st_size,
        mode=stat.S_IMODE(metadata.st_mode),
        inode=metadata.st_ino,
        mtime_ns=metadata.st_mtime_ns,
        ctime_ns=metadata.st_ctime_ns,
    )


def _snapshot_bound_outputs(
    paths: Mapping[str, Path],
    expected: set[PurePosixPath],
) -> dict[str, FileState]:
    expected_strings = {path.as_posix() for path in expected}
    if set(paths) != expected_strings:
        missing = sorted(expected_strings - set(paths))
        unexpected = sorted(set(paths) - expected_strings)
        raise DeterminismError(
            "physical production output bindings are not the exact owner "
            f"closure; missing={missing}, unexpected={unexpected}"
        )
    if len(set(paths.values())) != len(paths):
        raise DeterminismError(
            "physical production output bindings are not bijective"
        )
    symlinks = sorted(
        path.as_posix() for path in paths.values() if path.is_symlink()
    )
    if symlinks:
        raise DeterminismError(
            "generated production outputs contain symlinks: "
            + ", ".join(symlinks)
        )
    return {
        relative: _read_file_state(
            path, f"generated artifact {relative}"
        )
        for relative, path in sorted(paths.items())
    }


def _snapshot_exact_paths(
    paths: Iterable[Path],
    root: Path,
    label: str,
) -> dict[str, FileState]:
    snapshot: dict[str, FileState] = {}
    for path in sorted(set(paths)):
        if path.is_symlink():
            raise DeterminismError(f"{label} input is a symlink: {path}")
        if not _path_within(path, root):
            raise DeterminismError(
                f"{label} input escapes its variant root: {path}"
            )
        relative = path.relative_to(root).as_posix()
        snapshot[relative] = _read_file_state(path, f"{label} {relative}")
    if not snapshot:
        raise DeterminismError(f"{label} closure is empty")
    return snapshot


def _assert_byte_identical(
    reference: Mapping[str, FileState],
    candidate: Mapping[str, FileState],
    label: str,
) -> None:
    if set(reference) != set(candidate):
        raise DeterminismError(
            f"{label} artifact path set differs from canonical output"
        )
    differences = [
        path
        for path in sorted(reference)
        if reference[path].content != candidate[path].content
    ]
    if differences:
        raise DeterminismError(
            f"{label} generated bytes differ for: {', '.join(differences)}"
        )


def _assert_matches_ordinary_outputs(
    generated: Mapping[str, FileState],
    ordinary: Mapping[str, FileState],
) -> None:
    if set(generated) != set(ordinary):
        missing = sorted(set(generated) - set(ordinary))
        unexpected = sorted(set(ordinary) - set(generated))
        raise DeterminismError(
            "ordinary production output closure differs from generated "
            f"closure; missing={missing}, unexpected={unexpected}"
        )
    differences = [
        path
        for path in sorted(generated)
        if generated[path].content != ordinary[path].content
    ]
    if differences:
        raise DeterminismError(
            "ordinary production output bytes differ for: "
            + ", ".join(differences)
        )


def _assert_no_path_leaks(
    snapshot: Mapping[str, FileState],
    forbidden_paths: Iterable[Path],
    label: str,
    allowed_paths: Iterable[Path] = (),
) -> None:
    needles: dict[str, str] = {}
    minimal_forbidden: list[Path] = []
    for path in sorted(
        set(forbidden_paths),
        key=lambda item: (len(item.parts), item.as_posix()),
    ):
        if any(_path_within(path, root) for root in minimal_forbidden):
            continue
        minimal_forbidden.append(path)
    for path in minimal_forbidden:
        spelling = str(path)
        if not spelling or not Path(spelling).is_absolute():
            raise DeterminismError(
                f"{label} forbidden path is not absolute: {spelling!r}"
            )
        for candidate in {spelling, Path(spelling).as_posix()}:
            encoded = candidate.encode("utf-8").decode("latin-1")
            if encoded:
                needles.setdefault(encoded, candidate)
    leaks: list[str] = []
    allowed = sorted(
        set(allowed_paths), key=lambda item: item.as_posix()
    )
    allowed_rewriter = (
        PathRewriter(
            {
                path.as_posix(): f"/__bioseq_allowed_root_{index}__"
                for index, path in enumerate(allowed)
            }
        )
        if allowed
        else None
    )
    for relative, state in sorted(snapshot.items()):
        content = state.content.decode("latin-1")
        if allowed_rewriter is not None:
            content = allowed_rewriter.rewrite(content)
        matches = sorted(
            spelling
            for encoded, spelling in needles.items()
            if _path_occurrence_count(content, encoded)
        )
        if matches:
            leaks.append(f"{relative}: {matches}")
    if leaks:
        raise DeterminismError(
            f"{label} contains host/variant path leaks: " + "; ".join(leaks)
        )


def _assert_root_attestation(
    snapshot: Mapping[str, FileState],
    root: Path,
    label: str,
) -> None:
    spelling = root.as_posix().encode("utf-8")
    if not any(
        spelling in state.content for state in snapshot.values()
    ):
        raise DeterminismError(
            f"{label} does not attest its private root"
        )


def _require_exact_json_keys(
    value: object,
    expected: frozenset[str],
    label: str,
) -> dict[str, object]:
    if not isinstance(value, dict):
        raise DeterminismError(f"{label} is not a JSON object")
    actual = set(value)
    missing = sorted(expected - actual)
    unknown = sorted(actual - expected)
    if missing or unknown:
        raise DeterminismError(
            f"{label} keys differ from the exact contract; "
            f"missing={missing}, unknown={unknown}"
        )
    return value


def _require_json_string(
    value: Mapping[str, object],
    key: str,
    label: str,
) -> str:
    candidate = value[key]
    if not isinstance(candidate, str) or not candidate:
        raise DeterminismError(
            f"{label}.{key} is not a non-empty string"
        )
    return candidate


def _require_json_sha256(
    value: Mapping[str, object],
    key: str,
    label: str,
) -> str:
    candidate = _require_json_string(value, key, label)
    if len(candidate) != 64 or any(
        character not in "0123456789abcdef" for character in candidate
    ):
        raise DeterminismError(
            f"{label}.{key} is not a lowercase SHA-256"
        )
    return candidate


def _load_strict_json_object(content: bytes, label: str) -> dict[str, object]:
    def reject_duplicate_keys(
        pairs: list[tuple[str, object]],
    ) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise DeterminismError(
                    f"duplicate JSON key {key!r}"
                )
            result[key] = value
        return result

    try:
        parsed = json.loads(
            content.decode("utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except DeterminismError as error:
        raise DeterminismError(f"{label}: {error}") from error
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise DeterminismError(
            f"{label} is not canonical UTF-8 JSON: {error}"
        ) from error
    if not isinstance(parsed, dict):
        raise DeterminismError(f"{label} root is not a JSON object")
    return parsed


def _relocatable_manifest_path(
    spelling: str,
    relocation_root: str,
    label: str,
) -> str:
    path = Path(os.path.normpath(spelling))
    root = Path(os.path.normpath(relocation_root))
    if not path.is_absolute() or not root.is_absolute():
        raise DeterminismError(
            f"{label} path or relocation root is not absolute"
        )
    try:
        relative = path.relative_to(root)
    except ValueError:
        return path.as_posix()
    if relative == Path("."):
        return "$BIOSEQ_RELOCATION_ROOT"
    return (
        PurePosixPath("$BIOSEQ_RELOCATION_ROOT")
        / PurePosixPath(relative.as_posix())
    ).as_posix()


def _prepare_manifest_stage(relative: str) -> str | None:
    path = PurePosixPath(relative)
    if path.name != "pcm-manifest.json":
        return None
    if path.parts[-3:] == (
        "production",
        "support",
        "pcm-manifest.json",
    ):
        return "support"
    if path.parts[-3:] == (
        "production",
        "final",
        "pcm-manifest.json",
    ):
        return "final"
    if path.parts[-3:] == (
        "test_indexes",
        "support",
        "pcm-manifest.json",
    ):
        return "test_support"
    if path.parts[-2:] == ("core", "pcm-manifest.json"):
        return "core"
    raise DeterminismError(
        "PREPARE pcm-manifest.json appears outside a recognized stage: "
        f"{relative}"
    )


def _project_prepare_manifest(
    manifest: dict[str, object],
    stage: str,
    label: str,
) -> dict[str, object]:
    root = _require_exact_json_keys(
        manifest, PREPARE_ROOT_KEYS, label
    )
    for key in PREPARE_ROOT_KEYS - {"modules"}:
        _require_json_string(root, key, label)
    for key in PREPARE_ROOT_HASH_KEYS:
        _require_json_sha256(root, key, label)
    if root["schema"] != PREPARE_PCM_MANIFEST_SCHEMA:
        raise DeterminismError(
            f"{label}.schema is not {PREPARE_PCM_MANIFEST_SCHEMA!r}"
        )
    if root["compiler_module_policy"] != PREPARE_COMPILER_MODULE_POLICY:
        raise DeterminismError(
            f"{label}.compiler_module_policy is not the selected-Clang "
            "inventory contract"
        )
    expected_profile = (
        "test_indexes" if stage == "test_support" else "production"
    )
    expected_authority = (
        "support" if stage == "test_support" else stage
    )
    if root["compiler_profile"] != expected_profile:
        raise DeterminismError(
            f"{label}.compiler_profile is not {expected_profile!r}"
        )
    if root["authority"] != expected_authority:
        raise DeterminismError(
            f"{label}.authority is not {expected_authority!r}"
        )

    modules_value = root["modules"]
    if not isinstance(modules_value, list) or not modules_value:
        raise DeterminismError(f"{label}.modules is not a non-empty array")
    relocation_root = _require_json_string(
        root, "relocation_root", label
    )
    projected_modules: list[dict[str, object]] = []
    module_names: set[str] = set()
    for index, module_value in enumerate(modules_value):
        module_label = f"{label}.modules[{index}]"
        module = _require_exact_json_keys(
            module_value, PREPARE_MODULE_KEYS, module_label
        )
        for key in PREPARE_MODULE_KEYS - {
            "command_inputs",
            "base_arguments",
            "direct_imports",
        }:
            _require_json_string(module, key, module_label)
        for key in PREPARE_MODULE_HASH_KEYS:
            _require_json_sha256(module, key, module_label)
        module_name = _require_json_string(
            module, "name", module_label
        )
        if module_name in module_names:
            raise DeterminismError(
                f"{label}.modules duplicates module {module_name!r}"
            )
        module_names.add(module_name)

        command_inputs_value = module["command_inputs"]
        if not isinstance(command_inputs_value, list):
            raise DeterminismError(
                f"{module_label}.command_inputs is not an array"
            )
        projected_inputs: list[dict[str, str]] = []
        input_paths: set[str] = set()
        for input_index, input_value in enumerate(command_inputs_value):
            input_label = (
                f"{module_label}.command_inputs[{input_index}]"
            )
            command_input = _require_exact_json_keys(
                input_value,
                PREPARE_COMMAND_INPUT_KEYS,
                input_label,
            )
            path = _require_json_string(
                command_input, "path", input_label
            )
            _require_json_sha256(command_input, "sha256", input_label)
            relocatable_sha256 = _require_json_sha256(
                command_input, "relocatable_sha256", input_label
            )
            if path in input_paths:
                raise DeterminismError(
                    f"{module_label}.command_inputs duplicates {path!r}"
                )
            input_paths.add(path)
            projected_inputs.append(
                {
                    "relocatable_path": _relocatable_manifest_path(
                        path, relocation_root, input_label
                    ),
                    "relocatable_sha256": relocatable_sha256,
                }
            )
        projected_inputs.sort(
            key=lambda item: (
                item["relocatable_path"],
                item["relocatable_sha256"],
            )
        )
        base_arguments_value = module["base_arguments"]
        if (
            not isinstance(base_arguments_value, list)
            or not base_arguments_value
            or any(
                not isinstance(argument, str)
                for argument in base_arguments_value
            )
        ):
            raise DeterminismError(
                f"{module_label}.base_arguments is not a non-empty "
                "string array"
            )

        imports_value = module["direct_imports"]
        if not isinstance(imports_value, list) or any(
            not isinstance(imported, str) or not imported
            for imported in imports_value
        ):
            raise DeterminismError(
                f"{module_label}.direct_imports is not a string array"
            )
        imports = list(imports_value)
        if imports != sorted(set(imports)):
            raise DeterminismError(
                f"{module_label}.direct_imports is not canonical"
            )
        projected_module: dict[str, object] = {
            "name": module_name,
            "command_inputs": projected_inputs,
            "direct_imports": imports,
        }
        for key in PREPARE_MODULE_SEMANTIC_KEYS:
            projected_module[key] = module[key]
        projected_modules.append(projected_module)
    projected_modules.sort(key=lambda module: str(module["name"]))

    projection: dict[str, object] = {
        key: root[key] for key in PREPARE_ROOT_SEMANTIC_KEYS
    }
    projection["modules"] = projected_modules
    return projection


def _project_prepare_snapshot(
    snapshot: Mapping[str, FileState],
    label: str,
) -> dict[str, FileState]:
    manifests: dict[str, list[tuple[str, FileState]]] = {
        stage: [] for stage in PIPELINE_STAGES
    }
    for relative, state in sorted(snapshot.items()):
        stage = _prepare_manifest_stage(relative)
        if stage is not None:
            manifests[stage].append((relative, state))
    counts = {
        stage: len(manifests[stage])
        for stage in PIPELINE_STAGES
    }
    if any(count != 1 for count in counts.values()):
        raise DeterminismError(
            f"{label} must contain exactly one pcm-manifest.json per "
            f"PREPARE stage; counts={counts}"
        )

    projection: dict[str, FileState] = {}
    for stage in PIPELINE_STAGES:
        relative, state = manifests[stage][0]
        manifest_label = f"{label}/{stage} ({relative})"
        projected = _project_prepare_manifest(
            _load_strict_json_object(state.content, manifest_label),
            stage,
            manifest_label,
        )
        content = (
            json.dumps(
                projected,
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            )
            + "\n"
        ).encode("utf-8")
        projection[f"semantic/{stage}/pcm-manifest.json"] = FileState(
            content=content,
            sha256=_sha256_bytes(content),
            size=len(content),
            mode=0o644,
            inode=0,
            mtime_ns=0,
            ctime_ns=0,
        )
    return projection


def _assert_prepare_context_isolated(
    snapshot: Mapping[str, FileState],
    variant: str,
    variant_root: Path,
    all_variant_roots: Mapping[str, Path],
    forbidden_paths: Iterable[Path],
    stage_prefixes: Mapping[str, str] | None = None,
) -> None:
    _assert_root_attestation(
        snapshot, variant_root, f"{variant} PREPARE closure"
    )
    prefixes = (
        {
            stage: f"contexts/{stage}/"
            for stage in PIPELINE_STAGES
        }
        if stage_prefixes is None
        else stage_prefixes
    )
    if set(prefixes) != set(PIPELINE_STAGES):
        raise DeterminismError(
            f"{variant} PREPARE stage-prefix closure is incomplete"
        )
    for stage in PIPELINE_STAGES:
        prefix = prefixes[stage]
        stage_snapshot = {
            relative: state
            for relative, state in snapshot.items()
            if relative.startswith(prefix)
        }
        if not stage_snapshot:
            raise DeterminismError(
                f"{variant} PREPARE closure has no {stage} context"
            )
        _assert_root_attestation(
            stage_snapshot,
            variant_root,
            f"{variant}/{stage} PREPARE context",
        )
    forbidden = set(forbidden_paths)
    forbidden.update(
        root
        for other_variant, root in all_variant_roots.items()
        if other_variant != variant
    )
    _assert_no_path_leaks(
        snapshot,
        forbidden,
        f"{variant} PREPARE closure",
        allowed_paths={variant_root},
    )


def _assert_staged_inputs_isolated(
    snapshot: Mapping[str, FileState],
    variant: str,
    variant_root: Path,
    all_variant_roots: Mapping[str, Path],
    forbidden_paths: Iterable[Path],
) -> None:
    _assert_root_attestation(
        snapshot, variant_root, f"{variant} staged inputs"
    )
    forbidden = set(forbidden_paths)
    forbidden.update(
        root
        for other_variant, root in all_variant_roots.items()
        if other_variant != variant
    )
    _assert_no_path_leaks(
        snapshot,
        forbidden,
        f"{variant} staged inputs",
        allowed_paths={variant_root},
    )


def _assert_staged_inputs_unchanged(
    before: Mapping[str, FileState],
    after: Mapping[str, FileState],
    variant: str,
) -> None:
    if set(before) != set(after):
        raise DeterminismError(
            f"{variant} staged input path set changed during generation"
        )
    mutations = [
        relative
        for relative in sorted(before)
        if before[relative].sha256 != after[relative].sha256
        or before[relative].size != after[relative].size
        or before[relative].mode != after[relative].mode
    ]
    if mutations:
        raise DeterminismError(
            f"{variant} staged input hash/mode changed during generation: "
            + ", ".join(mutations)
        )


def _arm_noop_timestamps(
    output_paths: Mapping[str, Path],
    stamp_paths: frozenset[str],
) -> None:
    for relative, artifact in sorted(output_paths.items()):
        if relative in stamp_paths:
            continue
        if artifact.is_symlink() or not artifact.is_file():
            raise DeterminismError(
                f"cannot arm no-op timestamp for artifact: {artifact}"
            )
        os.utime(
            artifact,
            ns=(NOOP_SENTINEL_NS, NOOP_SENTINEL_NS),
            follow_symlinks=False,
        )


def _assert_noop_rewrite(
    before: Mapping[str, FileState],
    after: Mapping[str, FileState],
    stamp_paths: frozenset[str],
) -> None:
    _assert_byte_identical(before, after, "in-place repeat")
    unexpected_rewrites: list[str] = []
    for path in sorted(before):
        if path in stamp_paths:
            continue
        previous = before[path]
        current = after[path]
        if (
            previous.inode,
            previous.mode,
            previous.mtime_ns,
            previous.ctime_ns,
        ) != (
            current.inode,
            current.mode,
            current.mtime_ns,
            current.ctime_ns,
        ):
            unexpected_rewrites.append(path)
    if unexpected_rewrites:
        raise DeterminismError(
            "in-place repeat rewrote content-addressed artifacts: "
            + ", ".join(unexpected_rewrites)
        )


def _artifact_set_hash(snapshot: Mapping[str, FileState]) -> str:
    material = "".join(
        f"{path}\0{snapshot[path].size}\0{snapshot[path].sha256}\n"
        for path in sorted(snapshot)
    )
    return _sha256_bytes(material.encode("utf-8"))


def _mapping_args(
    option: str,
    owners: Iterable[str],
    directory: str,
    suffix: str,
    output_paths: Mapping[str, Path],
    cwd: Path,
) -> list[str]:
    result: list[str] = []
    for owner in sorted(owners):
        logical = f"artifacts/{directory}/{owner}.{suffix}.cppm"
        result.extend(
            [
                option,
                f"{owner}="
                + _configured_variant_path(output_paths[logical], cwd),
            ]
        )
    return result


def _configured_variant_path(path: Path, cwd: Path) -> str:
    if not _path_within(path, cwd):
        raise DeterminismError(
            f"configured variant path escapes its working root: {path}"
        )
    return path.as_posix()


def _common_generation_args(
    args: argparse.Namespace,
    source_list: Path,
    context: GenerationContext,
    profile: str = "production",
) -> list[str]:
    return [
        "--profile",
        profile,
        "--source-list",
        str(source_list),
        "--compile-database",
        str(context.compile_database),
        "--pcm-root",
        str(context.pcm_root),
        "--pcm-manifest",
        str(context.pcm_manifest),
        "--clang-binary",
        str(args.clang_binary),
        "--clang-scan-deps",
        str(args.clang_scan_deps),
        "--llvm-config",
        str(args.llvm_config),
        "--compiler-module-inventory",
        str(args.compiler_module_inventory),
        "--clang-sha256",
        args.clang_sha256,
        "--clang-scan-deps-sha256",
        args.clang_scan_deps_sha256,
        "--llvm-config-sha256",
        args.llvm_config_sha256,
        "--compiler-module-inventory-sha256",
        args.compiler_module_inventory_sha256,
    ]


def _run_generator(
    codegen: Path,
    cwd: Path,
    variant: str,
    stage: str,
    command_args: Sequence[str],
    profile: str = "production",
) -> None:
    command = [str(codegen), "sql", "generate", "--stage", stage, *command_args]
    environment = dict(os.environ)
    environment.update({"LC_ALL": "C", "LANG": "C", "TZ": "UTC"})
    result = subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        output = (result.stdout + result.stderr)[-16000:]
        raise DeterminismError(
            f"{variant}/{stage} {profile} generation failed with exit "
            f"{result.returncode}:\n{output}"
        )


def _run_prepare(
    args: argparse.Namespace,
    cwd: Path,
    variant: str,
    stage: str,
    source_list: Path,
    raw_compile_database: Path,
    context: GenerationContext,
    profile: str = "production",
) -> None:
    command = [
        str(args.codegen),
        "sql",
        "prepare",
        "--authority",
        stage,
        "--profile",
        profile,
        "--source-list",
        str(source_list),
        "--raw-compile-database",
        str(raw_compile_database),
        "--clang-binary",
        str(args.clang_binary),
        "--clang-scan-deps",
        str(args.clang_scan_deps),
        "--llvm-config",
        str(args.llvm_config),
        "--compiler-module-inventory",
        str(args.compiler_module_inventory),
        "--clang-sha256",
        args.clang_sha256,
        "--clang-scan-deps-sha256",
        args.clang_scan_deps_sha256,
        "--llvm-config-sha256",
        args.llvm_config_sha256,
        "--compiler-module-inventory-sha256",
        args.compiler_module_inventory_sha256,
        "--compile-database-output",
        str(context.compile_database),
        "--pcm-root",
        str(context.pcm_root),
        "--pcm-manifest",
        str(context.pcm_manifest),
        "--depfile",
        str(context.depfile),
        "--stamp",
        str(context.stamp),
    ]
    environment = dict(os.environ)
    environment.update({"LC_ALL": "C", "LANG": "C", "TZ": "UTC"})
    result = subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        output = (result.stdout + result.stderr)[-16000:]
        raise DeterminismError(
            f"{variant}/{stage} {profile} PREPARE failed with exit "
            f"{result.returncode}:\n{output}"
        )
    for path, label in (
        (context.compile_database, "compile database"),
        (context.pcm_manifest, "PCM manifest"),
        (context.depfile, "depfile"),
        (context.stamp, "stamp"),
    ):
        _require_regular_file(
            path, f"{variant}/{stage} PREPARE {label}"
        )
    _validate_owned_bmi_compile_database(
        context.compile_database,
        context.pcm_manifest,
        _read_source_inventory(source_list, stage),
        f"{variant}/{stage} {profile} PREPARE owned compile database",
    )


def _run_pipeline(
    args: argparse.Namespace,
    facets: FacetInventory,
    variant: str,
    variant_root: Path,
    source_lists: Mapping[str, Path],
    contexts: Mapping[str, GenerationContext],
    raw_compile_database: Path,
    output_paths: Mapping[str, Path],
) -> None:
    def configured(logical: str) -> str:
        return _configured_variant_path(
            output_paths[logical], variant_root
        )

    guc_output_root = output_paths[
        "artifacts/gucs/production/guc_registry.cppm"
    ].parents[1]
    _run_prepare(
        args,
        variant_root,
        variant,
        "support",
        source_lists["support"],
        raw_compile_database,
        contexts["support"],
    )
    support_args = _common_generation_args(
        args, source_lists["support"], contexts["support"]
    )
    support_args.extend(
        [
            "--manifest",
            configured("artifacts/support/support-manifest.json"),
            "--stamp",
            configured("artifacts/support/support.stamp"),
            "--guc-parity-ledger",
            str(args.guc_parity_ledger),
            "--guc-output-root",
            _configured_variant_path(guc_output_root, variant_root),
            "--guc-active-profile",
            "production",
            "--extension-schema",
            "public",
        ]
    )
    support_args.extend(
        _mapping_args(
            "--row-codecs-output",
            facets.row_codecs,
            "row-codecs",
            "row_codecs",
            output_paths,
            variant_root,
        )
    )
    support_args.extend(
        _mapping_args(
            "--fmgr-entrypoints-output",
            facets.support_fmgr_entrypoints,
            "fmgr",
            "entrypoints",
            output_paths,
            variant_root,
        )
    )
    _run_generator(
        args.codegen, variant_root, variant, "support", support_args
    )

    _run_prepare(
        args,
        variant_root,
        variant,
        "support",
        source_lists["test_support"],
        raw_compile_database,
        contexts["test_support"],
        profile="test_indexes",
    )
    test_support_args = _common_generation_args(
        args,
        source_lists["test_support"],
        contexts["test_support"],
        profile="test_indexes",
    )
    test_support_args.extend(
        [
            "--manifest",
            configured(
                "artifacts/test-support/support-manifest.json"
            ),
            "--stamp",
            configured("artifacts/test-support/support.stamp"),
            "--guc-parity-ledger",
            str(args.guc_parity_ledger),
            "--guc-output-root",
            _configured_variant_path(guc_output_root, variant_root),
            "--guc-active-profile",
            "test_indexes",
            "--extension-schema",
            "public",
        ]
    )
    test_support_args.extend(
        _mapping_args(
            "--row-codecs-output",
            facets.test_row_codecs,
            "test-row-codecs",
            "row_codecs",
            output_paths,
            variant_root,
        )
    )
    test_support_args.extend(
        _mapping_args(
            "--fmgr-entrypoints-output",
            facets.test_support_fmgr_entrypoints,
            "test-fmgr",
            "entrypoints",
            output_paths,
            variant_root,
        )
    )
    _run_generator(
        args.codegen,
        variant_root,
        variant,
        "support",
        test_support_args,
        profile="test_indexes",
    )

    _run_prepare(
        args,
        variant_root,
        variant,
        "core",
        source_lists["core"],
        raw_compile_database,
        contexts["core"],
    )
    core_args = _common_generation_args(
        args, source_lists["core"], contexts["core"]
    )
    core_args.extend(
        [
            "--core-hash-output",
            configured("artifacts/core/core-descriptor.sha256"),
            "--core-manifest",
            configured("artifacts/core/core-manifest.json"),
            "--stamp",
            configured("artifacts/core/core.stamp"),
            "--extension-schema",
            "public",
        ]
    )
    core_args.extend(
        _mapping_args(
            "--entity-handles-output",
            facets.entity_handles,
            "handles",
            "entity_handles",
            output_paths,
            variant_root,
        )
    )
    _run_generator(args.codegen, variant_root, variant, "core", core_args)

    _run_prepare(
        args,
        variant_root,
        variant,
        "final",
        source_lists["final"],
        raw_compile_database,
        contexts["final"],
    )
    final_args = _common_generation_args(
        args, source_lists["final"], contexts["final"]
    )
    final_args.extend(
        [
            "--output",
            configured("artifacts/final/bioseq--1.0.sql"),
            "--manifest",
            configured("artifacts/final/bioseq-sql-manifest.json"),
            "--physical-ledger",
            str(args.physical_ledger),
            "--require-checked-physical-ledger",
            "--stamp",
            configured("artifacts/final/bioseq-sql.stamp"),
            "--support-manifest",
            configured("artifacts/support/support-manifest.json"),
            "--support-seal",
            configured("artifacts/support/support.stamp"),
            "--production-support-manifest",
            configured("artifacts/support/support-manifest.json"),
            "--production-support-seal",
            configured("artifacts/support/support.stamp"),
            "--test-indexes-support-manifest",
            configured(
                "artifacts/test-support/support-manifest.json"
            ),
            "--test-indexes-support-seal",
            configured("artifacts/test-support/support.stamp"),
            "--expected-core-hash",
            configured("artifacts/core/core-descriptor.sha256"),
            "--core-manifest",
            configured("artifacts/core/core-manifest.json"),
            "--guc-parity-ledger",
            str(args.guc_parity_ledger),
            "--guc-output-root",
            _configured_variant_path(guc_output_root, variant_root),
            "--guc-active-profile",
            "production",
            "--require-complete-codegen",
            "--extension-schema",
            "public",
        ]
    )
    final_args.extend(
        _mapping_args(
            "--entity-handles-output",
            facets.entity_handles,
            "handles",
            "entity_handles",
            output_paths,
            variant_root,
        )
    )
    final_args.extend(
        _mapping_args(
            "--row-codecs-output",
            facets.row_codecs,
            "row-codecs",
            "row_codecs",
            output_paths,
            variant_root,
        )
    )
    final_args.extend(
        _mapping_args(
            "--fmgr-entrypoints-output",
            facets.fmgr_entrypoints,
            "fmgr",
            "entrypoints",
            output_paths,
            variant_root,
        )
    )
    _run_generator(args.codegen, variant_root, variant, "final", final_args)


def _resolve_run_arguments(args: argparse.Namespace) -> None:
    args.codegen = _require_regular_file(args.codegen, "bioseq_codegen", True)
    args.owner_registry_metadata = _require_regular_file(
        args.owner_registry_metadata, "SQL owner registry metadata"
    )
    args.physical_ledger = _require_regular_file(
        args.physical_ledger, "checked physical ledger"
    )
    args.guc_parity_ledger = _require_regular_file(
        args.guc_parity_ledger, "GUC parity ledger"
    )
    args.clang_binary = _require_regular_file(
        args.clang_binary, "configured Clang", True
    )
    args.clang_scan_deps = _require_regular_file(
        args.clang_scan_deps, "configured clang-scan-deps", True
    )
    args.llvm_config = _require_regular_file(
        args.llvm_config, "configured llvm-config", True
    )
    args.compiler_module_inventory = _require_regular_file(
        args.compiler_module_inventory, "compiler module inventory"
    )
    args.raw_compile_database = _require_regular_file(
        args.raw_compile_database, "raw CMake compile database"
    )
    args.required_target_dyndep_inventory = _require_regular_file(
        args.required_target_dyndep_inventory,
        "required target dyndep inventory",
    )
    args.source_root = _require_directory(args.source_root, "source root")
    args.build_root = _require_directory(args.build_root, "build root")
    args.ordinary_context_root = _require_directory(
        args.ordinary_context_root, "ordinary Clang context root"
    )
    for path, label in (
        (args.build_root, "build root"),
        (args.raw_compile_database, "raw compile database"),
        (
            args.required_target_dyndep_inventory,
            "required target dyndep inventory",
        ),
        (args.ordinary_context_root, "ordinary Clang context root"),
    ):
        if not _path_within(path, args.source_root):
            raise DeterminismError(
                f"{label} is outside the mirrored source root: {path}"
            )
    args.required_target_dyndeps = (
        _validate_required_target_dyndep_coverage(
            args.required_target_dyndep_inventory,
            args.raw_compile_database,
            args.build_root,
        )
    )
    for stage in PIPELINE_STAGES:
        setattr(
            args,
            f"{stage}_source_list",
            _require_regular_file(
                getattr(args, f"{stage}_source_list"),
                f"{stage} source list",
            ),
        )

    for label, value in (
        ("Clang SHA-256", args.clang_sha256),
        ("clang-scan-deps SHA-256", args.clang_scan_deps_sha256),
        ("llvm-config SHA-256", args.llvm_config_sha256),
        (
            "compiler module inventory SHA-256",
            args.compiler_module_inventory_sha256,
        ),
    ):
        if len(value) != 64 or any(
            character not in "0123456789abcdef" for character in value
        ):
            raise DeterminismError(f"{label} is not lowercase SHA-256")

    requested_output_root = args.output_root.expanduser().absolute()
    requested_report = args.report.expanduser().absolute()
    if requested_output_root.is_symlink():
        raise DeterminismError(
            f"determinism output root is a symlink: {requested_output_root}"
        )
    if requested_report.is_symlink():
        raise DeterminismError(
            f"determinism report is a symlink: {requested_report}"
        )
    args.output_root = requested_output_root.resolve()
    args.report = requested_report.resolve()
    args.output_root.mkdir(parents=True, exist_ok=True)
    if not args.output_root.is_dir():
        raise DeterminismError(
            f"determinism output root is not a directory: {args.output_root}"
        )
    if args.report.parent != args.output_root:
        raise DeterminismError(
            "determinism report must be a direct child of the output root"
        )


def run(args: argparse.Namespace) -> int:
    _resolve_run_arguments(args)
    runs_root = args.output_root / "runs"
    if runs_root.is_symlink():
        raise DeterminismError("determinism runs root is a symlink")
    runs_root.mkdir(parents=True, exist_ok=True)
    variant_roots = {
        variant: runs_root / variant for variant in VARIANTS
    }
    for variant in VARIANTS:
        variant_root = variant_roots[variant]
        if variant_root.is_symlink():
            raise DeterminismError(
                f"determinism variant root is a symlink: {variant_root}"
            )
        if variant_root.exists():
            if not variant_root.is_dir():
                raise DeterminismError(
                    f"determinism variant root is not a directory: "
                    f"{variant_root}"
                )
            shutil.rmtree(variant_root)
        variant_root.mkdir(parents=True)

    facets = _parse_owner_registry(args.owner_registry_metadata)
    expected_outputs = _relative_outputs(facets)
    ordinary_output_paths = _parse_ordinary_outputs(
        args.ordinary_output, expected_outputs
    )
    outside_mirror = sorted(
        path.as_posix()
        for path in ordinary_output_paths.values()
        if not _path_within(path, args.source_root)
    )
    if outside_mirror:
        raise DeterminismError(
            "ordinary production outputs escape the mirrored source root: "
            + ", ".join(outside_mirror)
        )
    facets = _with_support_fmgr_from_ordinary_manifest(
        facets, ordinary_output_paths
    )
    ordinary_outputs = {
        relative: _read_file_state(
            path, f"ordinary production output {relative}"
        )
        for relative, path in sorted(ordinary_output_paths.items())
    }
    canonical_by_stage = {
        stage: _read_source_inventory(
            getattr(args, f"{stage}_source_list"), stage
        )
        for stage in PIPELINE_STAGES
    }
    for stage in PIPELINE_STAGES:
        registered_sources = getattr(facets, f"{stage}_sources")
        if canonical_by_stage[stage] != registered_sources:
            missing = sorted(
                set(registered_sources) - set(canonical_by_stage[stage])
            )
            unexpected = sorted(
                set(canonical_by_stage[stage]) - set(registered_sources)
            )
            raise DeterminismError(
                f"{stage} source list is not the exact owner "
                f"registry closure; missing={missing}, unexpected={unexpected}"
            )
    variants_by_stage = {
        stage: _inventory_variants(canonical_by_stage[stage], stage)
        for stage in PIPELINE_STAGES
    }
    source_lists: dict[str, dict[str, Path]] = {}
    contexts: dict[str, dict[str, GenerationContext]] = {}
    raw_compile_databases: dict[str, Path] = {}
    context_snapshot_roots: dict[str, Path] = {}
    staged_compilations: dict[str, StagedCompilation] = {}
    input_snapshots: dict[str, dict[str, FileState]] = {}
    required_sources = {
        source
        for stage in PIPELINE_STAGES
        for source in canonical_by_stage[stage]
    }
    for variant in VARIANTS:
        variant_root = variant_roots[variant]
        staged_compilation = _stage_raw_compile_database(
            args.raw_compile_database,
            ordinary_output_paths,
            args.source_root,
            args.build_root,
            variant_root,
            required_sources,
        )
        staged_compilations[variant] = staged_compilation
        raw_compile_databases[variant] = (
            staged_compilation.compile_database
        )
        contexts[variant] = _variant_contexts(
            args.source_root,
            variant_root,
            args.ordinary_context_root,
        )
        context_snapshot_roots[variant] = _variant_mirror_path(
            args.ordinary_context_root,
            args.source_root,
            variant_root,
        )

        source_lists[variant] = {}
        staged_input_paths = list(staged_compilation.staged_files)
        for stage in PIPELINE_STAGES:
            inventory_path = _variant_mirror_path(
                getattr(args, f"{stage}_source_list"),
                args.source_root,
                variant_root,
            )
            relocated_inventory = [
                staged_compilation.relocated_sources[source].as_posix()
                for source in variants_by_stage[stage][variant]
            ]
            content = ("\n".join(relocated_inventory) + "\n").encode(
                "utf-8"
            )
            _atomic_write(inventory_path, content)
            source_lists[variant][stage] = inventory_path.resolve(strict=True)
            staged_input_paths.append(source_lists[variant][stage])
        input_snapshots[variant] = _snapshot_exact_paths(
            staged_input_paths,
            variant_root,
            f"{variant} staged inputs",
        )

    forbidden_paths = {
        args.source_root,
        args.build_root,
        args.raw_compile_database,
        args.ordinary_context_root,
        *(
            Path(source)
            for sources in canonical_by_stage.values()
            for source in sources
        ),
        *ordinary_output_paths.values(),
    }
    for variant in VARIANTS:
        _assert_staged_inputs_isolated(
            input_snapshots[variant],
            variant,
            variant_roots[variant],
            variant_roots,
            forbidden_paths,
        )

    snapshots: dict[str, dict[str, FileState]] = {}
    prepare_snapshots: dict[str, dict[str, FileState]] = {}
    for variant in VARIANTS:
        _run_pipeline(
            args,
            facets,
            variant,
            variant_roots[variant],
            source_lists[variant],
            contexts[variant],
            raw_compile_databases[variant],
            staged_compilations[variant].physical_outputs,
        )
        snapshots[variant] = _snapshot_bound_outputs(
            staged_compilations[variant].physical_outputs,
            expected_outputs,
        )
        prepare_snapshots[variant] = _snapshot_tree(
            context_snapshot_roots[variant],
            f"{variant} PREPARE",
        )
        after_inputs = _snapshot_exact_paths(
            (
                variant_roots[variant] / key
                for key in input_snapshots[variant]
            ),
            variant_roots[variant],
            f"{variant} staged inputs after generation",
        )
        _assert_staged_inputs_unchanged(
            input_snapshots[variant], after_inputs, variant
        )

    for variant in ("reversed", "shuffled"):
        _assert_byte_identical(
            snapshots["canonical"], snapshots[variant], variant
        )

    _assert_matches_ordinary_outputs(
        snapshots["canonical"], ordinary_outputs
    )
    generated_forbidden_paths = {
        *forbidden_paths,
        args.codegen,
        args.clang_binary,
        args.clang_scan_deps,
        args.llvm_config,
        args.compiler_module_inventory,
        *variant_roots.values(),
    }
    for variant in VARIANTS:
        _assert_prepare_context_isolated(
            prepare_snapshots[variant],
            variant,
            variant_roots[variant],
            variant_roots,
            forbidden_paths,
            {
                "support": (
                    f"{context_snapshot_roots[variant].name}/"
                    "production/support/"
                ),
                "test_support": (
                    f"{context_snapshot_roots[variant].name}/"
                    "test_indexes/support/"
                ),
                "core": (
                    f"{context_snapshot_roots[variant].name}/core/"
                ),
                "final": (
                    f"{context_snapshot_roots[variant].name}/"
                    "production/final/"
                ),
            },
        )
        _assert_no_path_leaks(
            snapshots[variant],
            generated_forbidden_paths,
            f"{variant} generated artifacts",
        )

    prepare_projections = {
        variant: _project_prepare_snapshot(
            prepare_snapshots[variant], f"{variant} PREPARE"
        )
        for variant in VARIANTS
    }
    for variant in ("reversed", "shuffled"):
        _assert_byte_identical(
            prepare_projections["canonical"],
            prepare_projections[variant],
            f"{variant} PREPARE semantic projection",
        )

    _arm_noop_timestamps(
        staged_compilations["canonical"].physical_outputs,
        _stamp_outputs(),
    )
    before_repeat = _snapshot_bound_outputs(
        staged_compilations["canonical"].physical_outputs,
        expected_outputs,
    )
    _run_pipeline(
        args,
        facets,
        "canonical-repeat",
        variant_roots["canonical"],
        source_lists["canonical"],
        contexts["canonical"],
        raw_compile_databases["canonical"],
        staged_compilations["canonical"].physical_outputs,
    )
    after_repeat = _snapshot_bound_outputs(
        staged_compilations["canonical"].physical_outputs,
        expected_outputs,
    )
    _assert_noop_rewrite(before_repeat, after_repeat, _stamp_outputs())
    after_repeat_prepare = _snapshot_tree(
        context_snapshot_roots["canonical"],
        "canonical-repeat PREPARE",
    )
    after_repeat_inputs = _snapshot_exact_paths(
        (
            variant_roots["canonical"] / key
            for key in input_snapshots["canonical"]
        ),
        variant_roots["canonical"],
        "canonical-repeat staged inputs",
    )
    _assert_staged_inputs_unchanged(
        input_snapshots["canonical"],
        after_repeat_inputs,
        "canonical-repeat",
    )
    _assert_byte_identical(
        prepare_snapshots["canonical"],
        after_repeat_prepare,
        "in-place raw PREPARE repeat",
    )

    source_report: dict[str, dict[str, object]] = {}
    for variant in VARIANTS:
        source_report[variant] = {}
        for stage in PIPELINE_STAGES:
            inventory = variants_by_stage[stage][variant]
            material = ("\n".join(inventory) + "\n").encode("utf-8")
            source_report[variant][stage] = {
                "entry_count": len(inventory),
                "sha256": _sha256_bytes(material),
            }

    artifact_hashes = {
        path: {
            "sha256": state.sha256,
            "size": state.size,
        }
        for path, state in sorted(after_repeat.items())
    }
    artifact_set_hash = _artifact_set_hash(after_repeat)
    ordinary_artifact_set_hash = _artifact_set_hash(ordinary_outputs)
    prepare_projection_hashes = {
        variant: _artifact_set_hash(prepare_projections[variant])
        for variant in VARIANTS
    }
    report = {
        "schema": REPORT_SCHEMA,
        "profile": "production",
        "source_inventories": source_report,
        "facet_counts": {
            "row_codecs": len(facets.row_codecs),
            "entity_handles": len(facets.entity_handles),
            "fmgr_entrypoints": len(facets.fmgr_entrypoints),
            "guc_owners": len(facets.guc_owners),
            "annotation_free_result_rows": len(
                facets.annotation_free_result_rows
            ),
            "legacy_fmgr_scans": len(facets.legacy_fmgr_scans),
        },
        "artifact_count": len(after_repeat),
        "artifact_set_sha256": artifact_set_hash,
        "ordinary_artifact_set_sha256": ordinary_artifact_set_hash,
        "artifacts": artifact_hashes,
        "prepare_closure": {
            "cross_root_comparison": (
                "exact PCM-manifest schemas are parsed once per stage and "
                "reduced to canonical relocation-semantic JSON; raw PCM, "
                "depfile, provenance, compile-database, stamp, and other "
                "physical PREPARE artifacts are not compared across roots"
            ),
            "semantic_projection_set_sha256": prepare_projection_hashes,
            "semantic_projection_file_count": len(
                prepare_projections["canonical"]
            ),
            "raw_physical_artifact_sets_compared_across_roots": False,
            "private_contexts": {
                variant: {
                    "raw_physical_file_count": len(
                        prepare_snapshots[variant]
                    ),
                    "attests_private_root": True,
                    "references_other_variant_or_ordinary_output": False,
                    "strict_stage_manifest_count": len(
                        PIPELINE_STAGES
                    ),
                }
                for variant in VARIANTS
            },
        },
        "staged_inputs": {
            "policy": (
                "selected C++ module compile entries and all referenced "
                "project source/build/include/response inputs and ordinary "
                "outputs preserve one source-root-relative workspace mirror "
                "without symlinks; rewrites are simultaneous, "
                "longest-match, non-chaining, and path-boundary-aware"
            ),
            "logical_output_aliases": {
                relative: (
                    staged_compilations["canonical"]
                    .physical_outputs[relative]
                    .relative_to(
                        staged_compilations["canonical"].mirror_root
                    )
                    .as_posix()
                )
                for relative in sorted(
                    staged_compilations["canonical"].physical_outputs
                )
            },
            "variants": {
                variant: {
                    "selected_compile_entry_count": (
                        staged_compilations[variant].selected_entry_count
                    ),
                    "staged_file_count": (
                        staged_compilations[variant].staged_file_count
                    ),
                    "snapshot_file_count": len(
                        input_snapshots[variant]
                    ),
                    "artifact_set_sha256": _artifact_set_hash(
                        input_snapshots[variant]
                    ),
                    "mirror_root_relative": (
                        staged_compilations[variant]
                        .mirror_root.relative_to(
                            variant_roots[variant]
                        )
                        .as_posix()
                    ),
                    "all_outputs_preserve_ordinary_relative_paths": True,
                    "attests_private_root": True,
                    "contains_original_or_other_variant_paths": False,
                }
                for variant in VARIANTS
            },
            "canonical_hash_and_mode_unchanged_after_repeat": True,
        },
        "comparisons": {
            "canonical_vs_reversed_byte_identical": True,
            "canonical_vs_shuffled_byte_identical": True,
            "canonical_vs_ordinary_production_byte_identical": True,
            "generated_artifacts_contain_no_host_or_variant_paths": True,
            "fresh_prepare_semantic_projections_identical": True,
            "fresh_prepare_raw_artifact_sets_compared": False,
            "staged_inputs_hash_and_mode_unchanged": True,
            "staged_inputs_contain_no_original_or_other_variant_paths": True,
            "source_orders_pairwise_distinct_for_every_stage": True,
            "all_variant_artifact_set_sha256": {
                variant: _artifact_set_hash(snapshots[variant])
                for variant in VARIANTS
            },
        },
        "generator_invocations": {
            "independent_variant_working_roots": len(VARIANTS),
            "fresh_stage_invocations": (
                len(VARIANTS) * len(PIPELINE_STAGES)
            ),
            "in_place_repeat_stage_invocations": len(
                PIPELINE_STAGES
            ),
            "total": (
                (len(VARIANTS) + 1) * len(PIPELINE_STAGES)
            ),
        },
        "prepare_invocations": {
            "independent_variant_context_roots": len(VARIANTS),
            "fresh_stage_invocations": (
                len(VARIANTS) * len(PIPELINE_STAGES)
            ),
            "in_place_repeat_stage_invocations": len(
                PIPELINE_STAGES
            ),
            "total": (
                (len(VARIANTS) + 1) * len(PIPELINE_STAGES)
            ),
        },
        "in_place_repeat": {
            "variant": "canonical",
            "artifact_hashes_unchanged": True,
            "raw_prepare_snapshot_paths_and_bytes_unchanged": True,
            "requested_non_stamp_sentinel_mtime_ns": NOOP_SENTINEL_NS,
            "non_stamp_inode_mode_mtime_ctime_unchanged": True,
            "completion_stamp_bytes_unchanged": True,
        },
    }
    _atomic_write(
        args.report,
        (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8"),
    )
    print(
        "DOC0388 production determinism passed: "
        f"{len(after_repeat)} artifacts, closure {artifact_set_hash}"
    )
    return 0


def _expect_failure(action: Callable[[], object], text: str) -> None:
    try:
        action()
    except DeterminismError as error:
        if text not in str(error):
            raise AssertionError(
                f"expected diagnostic containing {text!r}, got {error!r}"
            ) from error
    else:
        raise AssertionError(f"expected DeterminismError containing {text!r}")


def self_test() -> int:
    script_identity = _repository_path_identity(
        Path(__file__), "determinism self-test script"
    )
    if script_identity != PurePosixPath(
        "main/codegen/sql/tests/verify_production_determinism.py"
    ):
        raise AssertionError(
            "repository source identity normalization changed"
        )
    baseline = {
        "artifacts/final/bioseq.sql": FileState(
            b"0123456789", "a" * 64, 10, 0o644, 1, 100, 100
        ),
        "artifacts/final/bioseq.stamp": FileState(
            b"x" * 20, "b" * 64, 20, 0o644, 2, 100, 100
        ),
    }
    identical = dict(baseline)
    _assert_byte_identical(baseline, identical, "identical")

    mutated = dict(baseline)
    mutated["artifacts/final/bioseq.sql"] = FileState(
        b"abcdefghij", "a" * 64, 10, 0o644, 3, 200, 200
    )
    _expect_failure(
        lambda: _assert_byte_identical(baseline, mutated, "mutated"),
        "generated bytes differ",
    )

    missing = dict(baseline)
    del missing["artifacts/final/bioseq.sql"]
    _expect_failure(
        lambda: _assert_byte_identical(baseline, missing, "missing"),
        "artifact path set differs",
    )
    _assert_matches_ordinary_outputs(baseline, identical)
    _expect_failure(
        lambda: _assert_matches_ordinary_outputs(baseline, mutated),
        "ordinary production output bytes differ",
    )
    _expect_failure(
        lambda: _assert_matches_ordinary_outputs(baseline, missing),
        "ordinary production output closure differs",
    )

    rewritten = dict(baseline)
    rewritten["artifacts/final/bioseq.sql"] = FileState(
        b"0123456789", "a" * 64, 10, 0o644, 3, 200, 200
    )
    _expect_failure(
        lambda: _assert_noop_rewrite(
            baseline,
            rewritten,
            frozenset({"artifacts/final/bioseq.stamp"}),
        ),
        "rewrote content-addressed artifacts",
    )

    retouched_stamp = dict(baseline)
    retouched_stamp["artifacts/final/bioseq.stamp"] = FileState(
        b"x" * 20, "b" * 64, 20, 0o644, 2, 300, 300
    )
    _assert_noop_rewrite(
        baseline,
        retouched_stamp,
        frozenset({"artifacts/final/bioseq.stamp"}),
    )
    leaked = dict(baseline)
    leaked["artifacts/final/bioseq.sql"] = FileState(
        b"prefix /forbidden/root suffix",
        "c" * 64,
        29,
        0o644,
        1,
        100,
        100,
    )
    _expect_failure(
        lambda: _assert_no_path_leaks(
            leaked, {Path("/forbidden/root")}, "mutation"
        ),
        "path leaks",
    )
    _assert_no_path_leaks(
        baseline, {Path("/forbidden/root")}, "clean fixture"
    )

    def prepare_fixture_state(document: dict[str, object]) -> FileState:
        content = (
            json.dumps(document, indent=2, sort_keys=True) + "\n"
        ).encode("utf-8")
        return FileState(
            content,
            _sha256_bytes(content),
            len(content),
            0o644,
            1,
            100,
            100,
        )

    def prepare_projection_fixture(
        root: Path,
        raw_hash_character: str,
        semantic_mutation_stage: str | None = None,
    ) -> dict[str, FileState]:
        stage_paths = {
            "support": (
                "contexts/production/support/pcm-manifest.json"
            ),
            "test_support": (
                "contexts/test_indexes/support/pcm-manifest.json"
            ),
            "core": "contexts/core/pcm-manifest.json",
            "final": "contexts/production/final/pcm-manifest.json",
        }
        stage_hashes = {
            "support": "d" * 64,
            "test_support": "c" * 64,
            "core": "e" * 64,
            "final": "f" * 64,
        }
        snapshot: dict[str, FileState] = {}
        for stage in PIPELINE_STAGES:
            semantic_dependency = (
                "0" * 64
                if semantic_mutation_stage == stage
                else "8" * 64
            )
            module = {
                "name": f"fixture.{stage}",
                "source": f"{root}/src/{stage}.cppm",
                "directory": f"{root}/build/{stage}",
                "source_sha256": raw_hash_character * 64,
                "relocatable_source_sha256": "1" * 64,
                "arguments_sha256": raw_hash_character * 64,
                "relocatable_arguments_sha256": "2" * 64,
                "command_input_sha256": raw_hash_character * 64,
                "relocatable_command_input_sha256": "3" * 64,
                "command_inputs": [
                    {
                        "path": f"{root}/response/{stage}.rsp",
                        "sha256": raw_hash_character * 64,
                        "relocatable_sha256": "a" * 64,
                    }
                ],
                "base_arguments": [
                    f"{root}/toolchain/clang++",
                    "-std=c++26",
                ],
                "active_import_scan_database": (
                    f"{root}/pcm/.module-dependency-scan/{stage}."
                    "compile_commands.json"
                ),
                "active_import_scan_database_sha256": (
                    raw_hash_character * 64
                ),
                "relocatable_active_import_scan_database_sha256": (
                    "4" * 64
                ),
                "active_import_scan_result": (
                    f"{root}/pcm/.module-dependency-scan/"
                    f"{stage}.p1689.json"
                ),
                "active_import_scan_result_sha256": (
                    raw_hash_character * 64
                ),
                "relocatable_active_import_scan_result_sha256": (
                    "5" * 64
                ),
                "active_import_scan_diagnostics": (
                    f"{root}/pcm/.module-dependency-scan/{stage}."
                    "diagnostics.txt"
                ),
                "active_import_scan_diagnostics_sha256": (
                    raw_hash_character * 64
                ),
                "relocatable_active_import_scan_diagnostics_sha256": (
                    "6" * 64
                ),
                "direct_imports": [],
                "depfile": f"{root}/pcm/{stage}.d",
                "depfile_sha256": raw_hash_character * 64,
                "relocatable_depfile_sha256": "7" * 64,
                "dependency_content_sha256": raw_hash_character * 64,
                "relocatable_dependency_content_sha256": (
                    semantic_dependency
                ),
                "content_key": raw_hash_character * 64,
                "relocatable_content_key": "9" * 64,
                "pcm": f"{root}/pcm/{stage}.pcm",
                "pcm_sha256": raw_hash_character * 64,
                "pcm_provenance": (
                    f"{root}/pcm/{stage}.pcm.provenance"
                ),
                "pcm_provenance_sha256": raw_hash_character * 64,
            }
            manifest = {
                "schema": PREPARE_PCM_MANIFEST_SCHEMA,
                "clang_version": "21.1.0",
                "clang_binary": f"{root}/toolchain/clang++",
                "clang_binary_sha256": "1" * 64,
                "clang_scan_deps": (
                    f"{root}/toolchain/clang-scan-deps"
                ),
                "clang_scan_deps_version": "21.1.0",
                "clang_scan_deps_sha256": "2" * 64,
                "llvm_config": f"{root}/toolchain/llvm-config",
                "llvm_config_sha256": "3" * 64,
                "llvm_version": "21.1.0",
                "llvm_prefix": f"{root}/toolchain",
                "clang_resource_dir": f"{root}/toolchain/resource",
                "clang_resource_dir_sha256": "4" * 64,
                "clang_full_version": "clang version 21.1.0",
                "linked_clang_full_version": "clang version 21.1.0",
                "linked_clang_cpp": f"{root}/toolchain/libclang-cpp",
                "linked_clang_cpp_sha256": "5" * 64,
                "linked_llvm_library": f"{root}/toolchain/libLLVM",
                "linked_llvm_library_sha256": "6" * 64,
                "compiler_module_policy": (
                    PREPARE_COMPILER_MODULE_POLICY
                ),
                "compiler_module_inventory": (
                    f"{root}/toolchain/libc++.modules.json"
                ),
                "compiler_module_inventory_sha256": "7" * 64,
                "relocation_root": str(root),
                "pcm_root": f"{root}/pcm",
                "owned_compile_database_sha256": (
                    raw_hash_character * 64
                ),
                "relocatable_owned_compile_database_sha256": (
                    "b" * 64
                ),
                "compiler_profile": (
                    "test_indexes"
                    if stage == "test_support"
                    else "production"
                ),
                "authority": (
                    "support" if stage == "test_support" else stage
                ),
                "compilation_signature": raw_hash_character * 64,
                "relocatable_compilation_signature": (
                    stage_hashes[stage]
                ),
                "modules": [module],
            }
            snapshot[stage_paths[stage]] = prepare_fixture_state(
                manifest
            )
        return snapshot

    prepare_a = prepare_projection_fixture(Path("/private/a"), "a")
    prepare_b = prepare_projection_fixture(Path("/private/b"), "b")
    projection_a = _project_prepare_snapshot(
        prepare_a, "relocated PREPARE fixture A"
    )
    projection_b = _project_prepare_snapshot(
        prepare_b, "relocated PREPARE fixture B"
    )
    _assert_byte_identical(
        projection_a, projection_b, "relocated PREPARE projection"
    )
    prepare_mutated = prepare_projection_fixture(
        Path("/private/b"), "b", semantic_mutation_stage="final"
    )
    _expect_failure(
        lambda: _assert_byte_identical(
            projection_a,
            _project_prepare_snapshot(
                prepare_mutated, "mutated PREPARE fixture"
            ),
            "mutated PREPARE semantic projection",
        ),
        "generated bytes differ",
    )

    def mutate_prepare_manifest(
        snapshot: Mapping[str, FileState],
        relative: str,
        mutation: Callable[[dict[str, object]], None],
    ) -> dict[str, FileState]:
        mutated = dict(snapshot)
        document = _load_strict_json_object(
            mutated[relative].content, f"self-test {relative}"
        )
        mutation(document)
        mutated[relative] = prepare_fixture_state(document)
        return mutated

    support_manifest = (
        "contexts/production/support/pcm-manifest.json"
    )

    def add_unknown_root_key(document: dict[str, object]) -> None:
        document["unexpected_root_key"] = "rejected"

    _expect_failure(
        lambda: _project_prepare_snapshot(
            mutate_prepare_manifest(
                prepare_a, support_manifest, add_unknown_root_key
            ),
            "unknown-root-key PREPARE fixture",
        ),
        "unknown=['unexpected_root_key']",
    )

    def remove_module_key(document: dict[str, object]) -> None:
        modules = document["modules"]
        if not isinstance(modules, list) or not modules:
            raise AssertionError("valid PREPARE fixture lost its modules")
        module = modules[0]
        if not isinstance(module, dict):
            raise AssertionError("valid PREPARE fixture module changed")
        del module["relocatable_source_sha256"]

    _expect_failure(
        lambda: _project_prepare_snapshot(
            mutate_prepare_manifest(
                prepare_a, support_manifest, remove_module_key
            ),
            "missing-module-key PREPARE fixture",
        ),
        "missing=['relocatable_source_sha256']",
    )

    def add_unknown_command_input_key(
        document: dict[str, object],
    ) -> None:
        modules = document["modules"]
        if not isinstance(modules, list) or not modules:
            raise AssertionError("valid PREPARE fixture lost its modules")
        module = modules[0]
        if not isinstance(module, dict):
            raise AssertionError("valid PREPARE fixture module changed")
        inputs = module["command_inputs"]
        if not isinstance(inputs, list) or not inputs:
            raise AssertionError(
                "valid PREPARE fixture lost its command inputs"
            )
        command_input = inputs[0]
        if not isinstance(command_input, dict):
            raise AssertionError(
                "valid PREPARE fixture command input changed"
            )
        command_input["unexpected_command_input_key"] = "rejected"

    _expect_failure(
        lambda: _project_prepare_snapshot(
            mutate_prepare_manifest(
                prepare_a,
                support_manifest,
                add_unknown_command_input_key,
            ),
            "unknown-command-input-key PREPARE fixture",
        ),
        "unknown=['unexpected_command_input_key']",
    )

    bmi_manifest = _load_strict_json_object(
        prepare_a[support_manifest].content,
        "BMI-only owned-command fixture manifest",
    )
    bmi_modules = bmi_manifest["modules"]
    if not isinstance(bmi_modules, list) or len(bmi_modules) != 1:
        raise AssertionError("BMI-only fixture lost its single module")
    bmi_module = bmi_modules[0]
    if not isinstance(bmi_module, dict):
        raise AssertionError("BMI-only fixture module is not an object")
    bmi_source = str(bmi_module["source"])
    bmi_pcm = str(bmi_module["pcm"])
    bmi_full_pcm = str(Path(bmi_pcm).with_suffix(".full.pcm"))
    bmi_arguments = [
        *list(bmi_module["base_arguments"]),
        "--precompile",
        "-fmodules-reduced-bmi",
        "-fmodule-output=" + bmi_pcm,
        bmi_source,
        "-o",
        bmi_full_pcm,
        "-MD",
        "-MF",
        bmi_pcm + ".d",
        "-MT",
        bmi_pcm,
    ]
    bmi_database: list[dict[str, object]] = [
        {
            "arguments": bmi_arguments,
            "directory": bmi_module["directory"],
            "file": bmi_source,
        }
    ]
    _validate_owned_bmi_compile_database_payload(
        bmi_database,
        bmi_manifest,
        [bmi_source],
        "valid BMI-only fixture",
    )
    duplicate_bmi_database = json.loads(json.dumps(bmi_database))
    duplicate_arguments = duplicate_bmi_database[0]["arguments"]
    duplicate_output = duplicate_arguments.index("-o")
    duplicate_arguments[duplicate_output:duplicate_output] = [
        "-o",
        bmi_full_pcm,
    ]
    _expect_failure(
        lambda: _validate_owned_bmi_compile_database_payload(
            duplicate_bmi_database,
            bmi_manifest,
            [bmi_source],
            "duplicate BMI-only fixture",
        ),
        "ambiguous full BMI output",
    )
    foreign_bmi_database = json.loads(json.dumps(bmi_database))
    foreign_arguments = foreign_bmi_database[0]["arguments"]
    foreign_output = foreign_arguments.index("-o")
    foreign_arguments[foreign_output + 1] = "/foreign/output.full.pcm"
    _expect_failure(
        lambda: _validate_owned_bmi_compile_database_payload(
            foreign_bmi_database,
            bmi_manifest,
            [bmi_source],
            "foreign BMI-only fixture",
        ),
        "foreign full BMI output",
    )
    duplicate_reduced_database = json.loads(json.dumps(bmi_database))
    duplicate_reduced_arguments = duplicate_reduced_database[0]["arguments"]
    reduced_argument = "-fmodule-output=" + bmi_pcm
    reduced_index = duplicate_reduced_arguments.index(reduced_argument)
    duplicate_reduced_arguments.insert(reduced_index, reduced_argument)
    _expect_failure(
        lambda: _validate_owned_bmi_compile_database_payload(
            duplicate_reduced_database,
            bmi_manifest,
            [bmi_source],
            "duplicate reduced-BMI fixture",
        ),
        "ambiguous reduced BMI output",
    )
    foreign_reduced_database = json.loads(json.dumps(bmi_database))
    foreign_reduced_arguments = foreign_reduced_database[0]["arguments"]
    foreign_reduced_arguments[
        foreign_reduced_arguments.index(reduced_argument)
    ] = "-fmodule-output=/foreign/output.pcm"
    _expect_failure(
        lambda: _validate_owned_bmi_compile_database_payload(
            foreign_reduced_database,
            bmi_manifest,
            [bmi_source],
            "foreign reduced-BMI fixture",
        ),
        "foreign reduced BMI output",
    )
    object_codegen_database = json.loads(json.dumps(bmi_database))
    object_codegen_arguments = object_codegen_database[0]["arguments"]
    object_codegen_arguments[
        object_codegen_arguments.index("--precompile")
    ] = "-c"
    _expect_failure(
        lambda: _validate_owned_bmi_compile_database_payload(
            object_codegen_database,
            bmi_manifest,
            [bmi_source],
            "object-codegen fixture",
        ),
        "not the exact BMI-only --precompile command",
    )

    replacements = PathRewriter(
        {
            "/host/source": "/variant/host/build",
            "/host/source/deep": "/variant/deep",
            "/host/build": "/must-not-chain",
        }
    )
    replacement_counts: dict[str, int] = {}
    rewritten_paths = replacements.rewrite(
        "/host/source/deep/a /host/source/b "
        "/host/source-lookalike -I/host/source/c "
        "/prefix/host/source/d",
        replacement_counts,
    )
    if rewritten_paths != (
        "/variant/deep/a /variant/host/build/b "
        "/host/source-lookalike -I/variant/host/build/c "
        "/prefix/host/source/d"
    ):
        raise AssertionError(
            "simultaneous longest-match path rewriting changed"
        )
    if replacement_counts != {
        "/host/source/deep": 1,
        "/host/source": 2,
    }:
        raise AssertionError("path replacement counts changed")
    if "/must-not-chain" in rewritten_paths:
        raise AssertionError("path replacements unexpectedly chained")
    lookalike = {
        "fixture": FileState(
            (
                b"/forbidden/root-lookalike "
                b"/prefix/forbidden/root "
                b"prefix/forbidden/root"
            ),
            "0" * 64,
            49,
            0o644,
            1,
            1,
            1,
        )
    }
    _assert_no_path_leaks(
        lookalike, {Path("/forbidden/root")}, "lookalike fixture"
    )
    joined_leak = {
        "fixture": FileState(
            b"-I/forbidden/root/include",
            "1" * 64,
            25,
            0o644,
            1,
            1,
            1,
        )
    }
    _expect_failure(
        lambda: _assert_no_path_leaks(
            joined_leak,
            {Path("/forbidden/root")},
            "joined-path fixture",
        ),
        "path leaks",
    )

    source_fixture = ("/a.cppm", "/b.cppm", "/c.cppm", "/d.cppm")
    variants = _inventory_variants(source_fixture, "final")
    if len(set(variants.values())) != len(VARIANTS):
        raise AssertionError("source-order mutation fixture did not diverge")

    with tempfile.TemporaryDirectory(
        prefix="bioseq-sql-dyndep-self-test-"
    ) as temporary:
        temporary_root = Path(temporary).resolve()
        source_root = temporary_root / "main"
        build_root = source_root / "build"
        anchor_directory = (
            build_root / "CMakeFiles" / "fixture_anchor.dir"
        )
        future_directory = (
            build_root / "CMakeFiles" / "future_unique.dir"
        )
        anchor_directory.mkdir(parents=True)
        future_directory.mkdir(parents=True)
        anchor_source = source_root / "anchor.cppm"
        future_source = source_root / "future.cppm"
        anchor_source.write_text(
            "export module fixture.anchor;\n", encoding="utf-8"
        )
        future_source.write_text(
            "export module fixture.future;\n", encoding="utf-8"
        )
        anchor_dyndep = anchor_directory / "CXX.dd"
        anchor_modmap = anchor_directory / "anchor.cppm.o.modmap"
        future_modmap = future_directory / "future.cppm.o.modmap"
        inventory = build_root / "required-target-dyndeps.tsv"
        inventory.write_text(
            "schema\tbioseq_sql_target_dyndep_inventory/v1\n"
            "target\tdyndep\n"
            f"fixture_anchor\t{anchor_dyndep.as_posix()}\n",
            encoding="utf-8",
        )

        def dyndep_row(
            source: Path,
            modmap: Path,
        ) -> dict[str, object]:
            return {
                "directory": build_root.as_posix(),
                "arguments": [
                    "/usr/bin/clang++",
                    "-std=gnu++26",
                    f"@{modmap}",
                    "-c",
                    source.as_posix(),
                ],
                "file": source.as_posix(),
            }

        raw_database = build_root / "compile_commands.json"
        raw_database.write_text(
            json.dumps([dyndep_row(anchor_source, anchor_modmap)]),
            encoding="utf-8",
        )
        _expect_failure(
            lambda: _validate_required_target_dyndep_coverage(
                inventory, raw_database, build_root
            ),
            "required target dyndep does not exist",
        )

        anchor_dyndep.write_text("ninja_dyndep_version = 1\n")
        anchor_modmap.write_text(
            "-fmodule-file=fixture.anchor=anchor.pcm\n",
            encoding="utf-8",
        )
        _validate_required_target_dyndep_coverage(
            inventory, raw_database, build_root
        )

        raw_database.write_text(
            json.dumps(
                [
                    dyndep_row(anchor_source, anchor_modmap),
                    dyndep_row(future_source, future_modmap),
                ]
            ),
            encoding="utf-8",
        )
        _expect_failure(
            lambda: _validate_required_target_dyndep_coverage(
                inventory, raw_database, build_root
            ),
            "uncovered compile equivalence class",
        )

        # A missing duplicate row remains discardable only when an anchor
        # materialized the exact same source+standard response class.
        raw_database.write_text(
            json.dumps(
                [
                    dyndep_row(anchor_source, anchor_modmap),
                    dyndep_row(anchor_source, future_modmap),
                ]
            ),
            encoding="utf-8",
        )
        _validate_required_target_dyndep_coverage(
            inventory, raw_database, build_root
        )

    with tempfile.TemporaryDirectory(
        prefix="bioseq-sql-determinism-self-test-"
    ) as temporary:
        registry = Path(temporary) / "owners.tsv"
        registry.write_text(
            "owner_key\tmodule\tprofiles\tauthorities\tfacets\tsource\n"
            "rows\tfixture.rows\tproduction,test_indexes\tSUPPORT\t"
            "row_codecs=rows,guc_owner=bioseq\t/a.cppm\n"
            "handles\tfixture.handles\tproduction,test_indexes\tCORE\t"
            "entity_handles=handles\t/b.cppm\n"
            "fmgr_final\tfixture.fmgr_final\tproduction,test_indexes\t"
            "FINAL\tannotation_free_result_row=ordinary,"
            "fmgr_entrypoints=vector_comparison_registry,"
            "fmgr_entrypoints=vector_aggregate_registry,"
            "fmgr_entrypoints=ordinary,"
            "legacy_fmgr_scan=external\t/d.cppm\n",
            encoding="utf-8",
        )
        facets = _parse_owner_registry(registry)
        support_manifest = Path(temporary) / "support-manifest.json"
        support_manifest.write_text(
            json.dumps(
                {
                    "schema": "bioseq_sql_support_manifest/v1",
                    "compiler_profile": "production",
                    "vector_registries": [
                        {"owner": "vector_comparison_registry"},
                        {"owner": "vector_aggregate_registry"},
                    ],
                }
            ),
            encoding="utf-8",
        )
        test_support_manifest = (
            Path(temporary) / "test-support-manifest.json"
        )
        test_support_manifest.write_text(
            json.dumps(
                {
                    "schema": "bioseq_sql_support_manifest/v1",
                    "compiler_profile": "test_indexes",
                    "vector_registries": [
                        {"owner": "vector_comparison_registry"},
                        {"owner": "vector_aggregate_registry"},
                    ],
                }
            ),
            encoding="utf-8",
        )
        support_outputs = {
            "artifacts/support/support-manifest.json": support_manifest,
            "artifacts/test-support/support-manifest.json": (
                test_support_manifest
            ),
        }
        facets = _with_support_fmgr_from_ordinary_manifest(
            facets, support_outputs
        )
        if facets.support_fmgr_entrypoints != (
            "vector_aggregate_registry",
            "vector_comparison_registry",
        ):
            raise AssertionError(
                "SUPPORT fmgr facets were not derived from SUPPORT manifest"
            )
        if facets.final_fmgr_entrypoints != ("ordinary",):
            raise AssertionError("owner-registry facet fixture was misparsed")
        _expect_failure(
            lambda: _with_support_fmgr_from_ordinary_manifest(
                replace(facets, fmgr_entrypoints=("ordinary",)),
                support_outputs,
            ),
            "escape the production FMGR owner closure",
        )
        support_manifest.write_text(
            json.dumps(
                {
                    "schema": "bioseq_sql_support_manifest/v1",
                    "compiler_profile": "production",
                    "vector_registries": [
                        {"owner": "unregistered_vector_registry"}
                    ],
                }
            ),
            encoding="utf-8",
        )
        _expect_failure(
            lambda: _with_support_fmgr_from_ordinary_manifest(
                facets, support_outputs
            ),
            "differs from the exact two-owner contract",
        )
        unknown_registry = Path(temporary) / "owners-unknown.tsv"
        unknown_registry.write_text(
            registry.read_text(encoding="utf-8").replace(
                "row_codecs=rows,guc_owner=bioseq",
                "row_codecs=rows,guc_owner=bioseq,new_facet=escape",
            ),
            encoding="utf-8",
        )
        _expect_failure(
            lambda: _parse_owner_registry(unknown_registry),
            "unknown production owner facet",
        )

    with tempfile.TemporaryDirectory(
        prefix="bioseq-sql-staging-self-test-"
    ) as temporary:
        temporary_root = Path(temporary)
        source_root = temporary_root / "project" / "main"
        build_root = source_root / "build"
        variant_root = temporary_root / "variants" / "canonical"
        other_variant_root = temporary_root / "variants" / "reversed"
        include_root = source_root / "include"
        cache_root = build_root / "module-cache"
        response_root = build_root / "CMakeFiles" / "fixture.dir"
        for directory in (
            include_root,
            cache_root,
            response_root,
            variant_root,
        ):
            directory.mkdir(parents=True, exist_ok=True)
        source = source_root / "owner.cppm"
        header = include_root / "fixture.hpp"
        configured_header = include_root / "configured.hpp"
        forced_header = source_root / "forced.hpp"
        pcm = cache_root / "fixture.pcm"
        generated = build_root / "generated.cppm"
        response = response_root / "compile.rsp"
        nested_response = response_root / "nested.rsp"
        source.write_text("export module fixture;\n", encoding="utf-8")
        source.chmod(0o640)
        header.write_text("#pragma once\n", encoding="utf-8")
        configured_header.write_text(
            f'#define FIXTURE_ROOT "{source_root.as_posix()}"\n',
            encoding="utf-8",
        )
        forced_header.write_text("#pragma once\n", encoding="utf-8")
        pcm.write_bytes(b"pcm fixture")
        generated.write_text(
            "export module generated;\n", encoding="utf-8"
        )
        nested_response.write_text(
            f"-include {forced_header}\n", encoding="utf-8"
        )
        response.write_text(
            f"@{nested_response} "
            f"-fmodule-file=fixture={pcm}\n",
            encoding="utf-8",
        )
        stager = VariantInputStager(
            source_root,
            build_root,
            variant_root,
            {"artifacts/generated.cppm": generated},
        )
        normalized_arguments = stager.normalize_arguments(
            [
                "/usr/bin/clang++",
                f"-I{include_root}",
                "-isystem",
                include_root.as_posix(),
                f"--sysroot={source_root}",
                "-include",
                forced_header.as_posix(),
                f"-fmodule-file=fixture={pcm}",
                f"@{response}",
                "-o",
                (build_root / "owner.o").as_posix(),
                f"-MF{build_root / 'owner.d'}",
                source.as_posix(),
            ],
            build_root,
        )
        normalized_text = "\n".join(normalized_arguments)
        if source_root.as_posix() in normalized_text:
            raise AssertionError(
                "normalized path forms retained the source root"
            )
        if build_root.as_posix() in normalized_text:
            raise AssertionError(
                "normalized path forms retained the build root"
            )
        if variant_root.as_posix() not in normalized_text:
            raise AssertionError(
                "normalized path forms do not attest the variant root"
            )
        staged_configured_header = stager.input_path(configured_header)
        staged_configured_text = staged_configured_header.read_text(
            encoding="utf-8"
        )
        if source_root.as_posix() in staged_configured_text:
            raise AssertionError(
                "staged text input retained an original host path"
            )
        if variant_root.as_posix() not in staged_configured_text:
            raise AssertionError(
                "staged text input did not attest its private mirror"
            )
        if stager.input_path(generated) != (
            variant_root
            / "workspace"
            / "main"
            / generated.relative_to(source_root)
        ):
            raise AssertionError(
                "ordinary generated input did not preserve its mirror path"
            )
        if (variant_root / "workspace").is_symlink():
            raise AssertionError("staged input root became a symlink")
        staged_snapshot = _snapshot_exact_paths(
            stager.staged_files,
            variant_root,
            "staging self-test inputs",
        )
        _assert_staged_inputs_isolated(
            staged_snapshot,
            "canonical",
            variant_root,
            {
                "canonical": variant_root,
                "reversed": other_variant_root,
            },
            {source_root, build_root},
        )
        staged_source = stager.input_path(source)
        source_content = source.read_bytes()
        source_metadata = source.lstat()
        staged_source_metadata = staged_source.lstat()
        if (
            source_metadata.st_nlink != 1
            or staged_source_metadata.st_nlink != 1
        ):
            raise AssertionError(
                "isolated staging did not retain single-link files"
            )
        if (
            source_metadata.st_dev,
            source_metadata.st_ino,
        ) == (
            staged_source_metadata.st_dev,
            staged_source_metadata.st_ino,
        ):
            raise AssertionError(
                "isolated staging retained the configured source inode"
            )
        if stat.S_IMODE(source_metadata.st_mode) != stat.S_IMODE(
            staged_source_metadata.st_mode
        ):
            raise AssertionError(
                "isolated staging did not preserve the source mode"
            )
        if staged_source.read_bytes() != source_content:
            raise AssertionError(
                "isolated staging did not preserve the source bytes"
            )

        replay_hardlink = temporary_root / "replay-hardlink-alias"
        os.link(staged_source, replay_hardlink)
        try:
            _expect_failure(
                lambda: _snapshot_exact_paths(
                    {staged_source},
                    variant_root,
                    "replay hardlink mutation fixture",
                ),
                "is hard-linked",
            )
        finally:
            replay_hardlink.unlink()

        configured_hardlink_source = (
            source_root / "configured-hardlink.hpp"
        )
        configured_hardlink_source.write_text(
            "#pragma once\n", encoding="utf-8"
        )
        configured_hardlink_alias = (
            temporary_root / "configured-hardlink-alias"
        )
        os.link(
            configured_hardlink_source, configured_hardlink_alias
        )
        configured_hardlink_stager = VariantInputStager(
            source_root,
            build_root,
            temporary_root / "variants" / "configured-hardlink",
            {"artifacts/generated.cppm": generated},
        )
        try:
            _expect_failure(
                lambda: configured_hardlink_stager.stage_regular(
                    configured_hardlink_source,
                    "hardlink mutation fixture",
                ),
                "configured hardlink mutation fixture is hard-linked",
            )
        finally:
            configured_hardlink_alias.unlink()

        concurrent_source = source_root / "concurrent-hardlink.hpp"
        concurrent_source.write_text(
            "#pragma once\n", encoding="utf-8"
        )
        concurrent_alias = (
            temporary_root / "concurrent-hardlink-alias"
        )
        concurrent_stager = VariantInputStager(
            source_root,
            build_root,
            temporary_root / "variants" / "concurrent-hardlink",
            {"artifacts/generated.cppm": generated},
        )
        original_isolated_replace = _replace_with_isolated_bytes

        def inject_concurrent_hardlink(
            path: Path,
            content: bytes,
            mode: int,
            label: str,
        ) -> None:
            os.link(concurrent_source, concurrent_alias)
            original_isolated_replace(
                path, content, mode, label
            )

        globals()["_replace_with_isolated_bytes"] = (
            inject_concurrent_hardlink
        )
        try:
            _expect_failure(
                lambda: concurrent_stager.stage_regular(
                    concurrent_source,
                    "concurrent hardlink mutation fixture",
                ),
                (
                    "configured concurrent hardlink mutation fixture "
                    "is hard-linked"
                ),
            )
        finally:
            globals()["_replace_with_isolated_bytes"] = (
                original_isolated_replace
            )
            try:
                concurrent_alias.unlink()
            except FileNotFoundError:
                pass
        if concurrent_source.lstat().st_nlink != 1:
            raise AssertionError(
                "concurrent hardlink mutation fixture was not restored"
            )

        prepare_fixture = {
            f"contexts/{stage}/fixture.json": FileState(
                (
                    f'{{"root":"{variant_root.as_posix()}",'
                    f'"stage":"{stage}"}}'
                ).encode("utf-8"),
                str(index) * 64,
                1,
                0o644,
                index,
                1,
                1,
            )
            for index, stage in enumerate(
                PIPELINE_STAGES, start=1
            )
        }
        _assert_prepare_context_isolated(
            prepare_fixture,
            "canonical",
            variant_root,
            {
                "canonical": variant_root,
                "reversed": other_variant_root,
            },
            {source_root, build_root},
        )
        raw_database = build_root / "compile_commands.json"
        raw_database.write_text(
            json.dumps(
                [
                    {
                        "directory": build_root.as_posix(),
                        "arguments": [
                            "/usr/bin/clang++",
                            f"-I{include_root}",
                            f"@{response}",
                            "-o",
                            (build_root / "owner.o").as_posix(),
                            "-c",
                            source.as_posix(),
                        ],
                        "file": source.as_posix(),
                        "output": (build_root / "owner.o").as_posix(),
                    }
                ]
            ),
            encoding="utf-8",
        )
        database_variant_root = (
            temporary_root / "variants" / "database"
        )
        staged_database = _stage_raw_compile_database(
            raw_database,
            {"artifacts/generated.cppm": generated},
            source_root,
            build_root,
            database_variant_root,
            {source.as_posix()},
        )
        staged_rows = json.loads(
            staged_database.compile_database.read_text(encoding="utf-8")
        )
        if len(staged_rows) != 1:
            raise AssertionError(
                "staged compile database did not retain its selected entry"
            )
        staged_row = staged_rows[0]
        database_mirror = _variant_mirror_root(
            source_root, database_variant_root
        )
        expected_database_fields = {
            "directory": (
                database_mirror / build_root.relative_to(source_root)
            ).as_posix(),
            "file": (
                database_mirror / source.relative_to(source_root)
            ).as_posix(),
            "output": (
                database_mirror
                / (build_root / "owner.o").relative_to(source_root)
            ).as_posix(),
        }
        if any(
            staged_row.get(field) != expected
            for field, expected in expected_database_fields.items()
        ):
            raise AssertionError(
                "compile database directory/file/output mirror changed"
            )
        staged_database_text = json.dumps(staged_rows)
        if source_root.as_posix() in staged_database_text:
            raise AssertionError(
                "staged compile database retained its original source root"
            )

        missing_modmap = (
            build_root
            / "CMakeFiles"
            / "__cmake_cxx23.dir"
            / "owner.cppm.o.modmap"
        )
        viable_modmap = (
            build_root
            / "CMakeFiles"
            / "__cmake_cxx26.dir"
            / "owner.cppm.o.modmap"
        )
        viable_modmap.parent.mkdir(parents=True)
        viable_modmap.write_text(
            "-Wno-reserved-module-identifier\n",
            encoding="utf-8",
        )

        def duplicate_modmap_row(
            modmap: Path,
            output_name: str,
            standard: str = "gnu++26",
        ) -> dict[str, object]:
            output = build_root / output_name
            return {
                "directory": build_root.as_posix(),
                "arguments": [
                    "/usr/bin/clang++",
                    f"-std={standard}",
                    f"@{modmap}",
                    "-o",
                    output.as_posix(),
                    "-c",
                    source.as_posix(),
                ],
                "file": source.as_posix(),
                "output": output.as_posix(),
            }

        raw_database.write_text(
            json.dumps(
                [
                    duplicate_modmap_row(
                        missing_modmap, "missing-owner.o"
                    ),
                    duplicate_modmap_row(
                        viable_modmap, "viable-owner.o"
                    ),
                ]
            ),
            encoding="utf-8",
        )
        duplicate_variant_root = (
            temporary_root / "variants" / "duplicate-modmap"
        )
        duplicate_database = _stage_raw_compile_database(
            raw_database,
            {},
            source_root,
            build_root,
            duplicate_variant_root,
            {source.as_posix()},
        )
        duplicate_rows = json.loads(
            duplicate_database.compile_database.read_text(
                encoding="utf-8"
            )
        )
        if (
            len(duplicate_rows) != 1
            or duplicate_rows[0].get("output", "").endswith(
                "missing-owner.o"
            )
        ):
            raise AssertionError(
                "missing generated CMake modmap duplicate was not discarded"
            )

        raw_database.write_text(
            json.dumps(
                [
                    duplicate_modmap_row(
                        missing_modmap, "unmatched-owner.o"
                    ),
                    duplicate_modmap_row(
                        viable_modmap,
                        "wrong-standard-owner.o",
                        "gnu++23",
                    ),
                ]
            ),
            encoding="utf-8",
        )
        _expect_failure(
            lambda: _stage_raw_compile_database(
                raw_database,
                {},
                source_root,
                build_root,
                temporary_root
                / "variants"
                / "unmatched-modmap",
                {source.as_posix()},
            ),
            "compile response file does not exist",
        )

        raw_database.write_text(
            json.dumps(
                [
                    duplicate_modmap_row(
                        missing_modmap, "sole-missing-owner.o"
                    )
                ]
            ),
            encoding="utf-8",
        )
        _expect_failure(
            lambda: _stage_raw_compile_database(
                raw_database,
                {},
                source_root,
                build_root,
                temporary_root / "variants" / "sole-modmap",
                {source.as_posix()},
            ),
            "compile response file does not exist",
        )

        leaked_prepare = dict(prepare_fixture)
        leaked_prepare["contexts/final/fixture.json"] = FileState(
            (
                prepare_fixture["contexts/final/fixture.json"].content
                + b" "
                + other_variant_root.as_posix().encode("utf-8")
            ),
            "9" * 64,
            1,
            0o644,
            9,
            1,
            1,
        )
        _expect_failure(
            lambda: _assert_prepare_context_isolated(
                leaked_prepare,
                "canonical",
                variant_root,
                {
                    "canonical": variant_root,
                    "reversed": other_variant_root,
                },
                {source_root, build_root},
            ),
            "path leaks",
        )
        symlink_root = source_root / "symlink-input"
        symlink_root.mkdir()
        symlink_target = symlink_root / "target.hpp"
        symlink_target.write_text("#pragma once\n", encoding="utf-8")
        symlink = symlink_root / "escape.hpp"
        symlink.symlink_to(symlink_target)
        symlink_stager = VariantInputStager(
            source_root,
            build_root,
            temporary_root / "variants" / "symlink",
            {"artifacts/generated.cppm": generated},
        )
        _expect_failure(
            lambda: symlink_stager.stage_tree(
                symlink_root, "symlink mutation fixture"
            ),
            "symlink file",
        )
        before_mutation = _snapshot_exact_paths(
            stager.staged_files,
            variant_root,
            "hash mutation baseline",
        )
        staged_source.write_text(
            "export module fixture; // mutation\n", encoding="utf-8"
        )
        after_mutation = _snapshot_exact_paths(
            stager.staged_files,
            variant_root,
            "hash mutation candidate",
        )
        _expect_failure(
            lambda: _assert_staged_inputs_unchanged(
                before_mutation, after_mutation, "hash mutation"
            ),
            "hash/mode changed",
        )
        if source.read_bytes() != source_content:
            raise AssertionError(
                "staged input mutation changed its configured source"
            )
        if source.lstat().st_nlink != 1:
            raise AssertionError(
                "configured source link count changed after replay mutation"
            )

    print("DOC0388 production determinism self-tests passed")
    return 0


def _add_stage_inventory_argument(
    parser: argparse.ArgumentParser, stage: str
) -> None:
    parser.add_argument(
        f"--{stage}-source-list",
        type=Path,
        required=True,
    )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser(
        "self-test",
        help="run fail-closed comparison and inventory mutation tests",
    )
    run_parser = subparsers.add_parser(
        "run", help="run the real production-corpus determinism gate"
    )
    run_parser.add_argument("--codegen", type=Path, required=True)
    run_parser.add_argument(
        "--owner-registry-metadata", type=Path, required=True
    )
    for stage in STAGES:
        _add_stage_inventory_argument(run_parser, stage)
    run_parser.add_argument(
        "--test-indexes-support-source-list",
        dest="test_support_source_list",
        type=Path,
        required=True,
    )
    run_parser.add_argument(
        "--raw-compile-database", type=Path, required=True
    )
    run_parser.add_argument(
        "--required-target-dyndep-inventory",
        type=Path,
        required=True,
    )
    run_parser.add_argument("--source-root", type=Path, required=True)
    run_parser.add_argument("--build-root", type=Path, required=True)
    run_parser.add_argument(
        "--ordinary-context-root", type=Path, required=True
    )
    run_parser.add_argument(
        "--ordinary-output",
        action="append",
        default=[],
        metavar="RELATIVE=ABSOLUTE",
        help=(
            "bind one exact ordinary production artifact to its variant "
            "relative identity; repeat for the complete owner closure"
        ),
    )
    run_parser.add_argument("--physical-ledger", type=Path, required=True)
    run_parser.add_argument("--guc-parity-ledger", type=Path, required=True)
    run_parser.add_argument("--clang-binary", type=Path, required=True)
    run_parser.add_argument("--clang-scan-deps", type=Path, required=True)
    run_parser.add_argument("--llvm-config", type=Path, required=True)
    run_parser.add_argument(
        "--compiler-module-inventory", type=Path, required=True
    )
    run_parser.add_argument("--clang-sha256", required=True)
    run_parser.add_argument("--clang-scan-deps-sha256", required=True)
    run_parser.add_argument("--llvm-config-sha256", required=True)
    run_parser.add_argument(
        "--compiler-module-inventory-sha256", required=True
    )
    run_parser.add_argument("--output-root", type=Path, required=True)
    run_parser.add_argument("--report", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.command == "self-test":
            return self_test()
        return run(args)
    except (DeterminismError, OSError, ValueError) as error:
        print(f"verify_production_determinism: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
