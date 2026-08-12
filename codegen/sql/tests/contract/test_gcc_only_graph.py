#!/usr/bin/env python3
"""Static and cryptographic contracts for the GCC-only SQL graph."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


GENERATED_MODULE = re.compile(
    r"\bexport\s+module\s+"
    r"(bioseq\.postgres\.sql\.generated\."
    r"(?:core|production|test_indexes)\."
    r"[A-Za-z0-9_.]+)\s*;"
)
SHA256 = re.compile(r"^[0-9a-f]{64}$")
GENERATED_SQL_MANIFEST_FIELDS = frozenset(
    {
        "schema_revision",
        "authority_mode",
        "compiler_profile",
        "clang_version",
        "compilation_signature",
        "extension_name",
        "extension_schema",
        "generated_module_prefix",
        "fmgr_boundary_module",
        "script_sha256",
        "statement_count",
        "statements",
        "entity_count",
        "public_entity_count",
        "public_stable_identities",
        "public_installed_identities",
        "routine_entity_count",
        "c_routine_identity_count",
        "c_link_symbol_inventory_count",
        "c_finfo_symbol_inventory_count",
        "c_wrapper_entity_count",
        "generated_wrapper_symbol_count",
        "handwritten_fmgr_export_symbol_count",
        "handwritten_fmgr_export_external_symbol_count",
        "handwritten_fmgr_export_owner_count",
        "c_symbol_sharing_group_count",
        "generated_cpp_profile_invariant",
        "entity_handle_count",
        "entity_handle_module_count",
        "entity_handle_modules",
        "row_codec_count",
        "row_codec_module_count",
        "row_codec_modules",
        "routine_boundary_contract_count",
        "routine_boundary_contracts",
        "result_shape_contract_count",
        "result_shape_contracts",
        "row_codec_contract_count",
        "row_codec_contracts",
        "c_routine_identities",
        "c_link_symbols",
        "c_finfo_symbols",
        "generated_wrapper_symbols",
        "handwritten_fmgr_export_symbols",
        "handwritten_fmgr_export_external_symbols",
        "fmgr_entrypoint_module_count",
        "fmgr_entrypoint_modules",
        "handwritten_fmgr_export_owners",
        "c_symbol_owners",
        "entity_counts_by_kind",
        "entities",
        "guc_generation",
    }
)
GUC_REGISTRATION_KEYS = frozenset(
    {
        "registration_ordinal",
        "name",
        "owner",
        "member",
        "kind",
        "cpp_storage",
        "storage_kind",
        "source_module",
        "source_path",
        "source_sha256",
        "default_expression",
        "minimum_expression",
        "maximum_expression",
        "context",
        "flags",
        "unit",
        "hooks",
        "feature_predicate",
        "enum_default",
        "enum_rows_name_wire_condition_alias_hidden_compiled",
        "generated_live_authority",
    }
)
GUC_GENERATED_LIVE_KEYS = frozenset(
    {
        "vartype",
        "context",
        "unit",
        "boot_value",
        "reset_value",
        "current_value",
        "minimum",
        "maximum",
        "visible_enum_values",
        "short_description",
        "extra_description",
        "raw_flags",
        "visible_in_show_all",
        "included_in_sample",
        "hooks",
    }
)
GUC_GENERATED_HOOK_KEYS = frozenset(
    {"symbol", "present", "behavior"}
)
GUC_PROFILE_DELTA_KEYS = frozenset(
    {
        "production_registry_content_sha256",
        "test_indexes_registry_content_sha256",
        "added_in_test_indexes",
        "removed_from_test_indexes",
        "changed_in_test_indexes",
        "owner_descriptor_profile_invariant",
        "owner_artifacts",
    }
)
GUC_PROFILE_OWNER_ARTIFACT_KEYS = frozenset(
    {
        "production_content_sha256",
        "test_indexes_content_sha256",
    }
)
GUC_KIND_STORAGE_KINDS = {
    "bool": "bool",
    "integer": "int32",
    "real": "double",
    "string": "string",
    "enum": "int32_enum",
}
GUC_TEST_ONLY_NAMES = (
    "bioseq.testing.chunked_scanner.enabled",
    "bioseq.testing.in_memory_scanner.enabled",
    "bioseq.testing.persisted_index.failure_mode",
)
GUC_SOURCE_MODULES = {
    "bioseq": "bioseq.postgres.configuration.core",
    "scheduler": "bioseq.postgres.configuration.scheduler",
    "storage": "bioseq.postgres.configuration.storage",
    "aligner": "bioseq.postgres.configuration.aligner",
}
GUC_SOURCE_PATHS = {
    "bioseq": "bioseq/backend/pg/configuration/core.cppm",
    "scheduler": "bioseq/backend/pg/configuration/scheduler.cppm",
    "storage": "bioseq/backend/pg/configuration/storage.cppm",
    "aligner": "bioseq/backend/pg/configuration/aligner.cppm",
}
GUC_PRODUCTION_OWNER_COUNTS = {
    "bioseq": 26,
    "scheduler": 3,
    "storage": 1,
    "aligner": 43,
}
GUC_PROFILE_TOTALS = {"production": 73, "test_indexes": 76}
PROVENANCE_IDENTIFIERS = {
    "relation_entity_handle": "BIOSEQ_SQL_E_CATALOG_HANDLE_PROVENANCE",
    "postgres_enum_registration_access": "BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE",
    "postgres_enum_registration":
        "BIOSEQ_SQL_E_GUC_REGISTRATION_PROVENANCE",
}
NEGATIVE_DIAGNOSTICS = {
    "invalid_nested_annotation_namespace.cpp":
        "BIOSEQ_SQL_E_ATTRIBUTE_NAMESPACE",
    "invalid_class_head_placement.cppm":
        "BIOSEQ_SQL_E_CLASS_HEAD_PLACEMENT",
    "invalid_guc_enumerator_placement.cppm":
        "BIOSEQ_SQL_E_GUC_ENUMERATOR_PLACEMENT",
    "invalid_mixed_guc_alias_initialization.cppm":
        "BIOSEQ_SQL_E_MIXED_INITIALIZER",
    "authored_catalog_handle_specialization.cppm":
        "BIOSEQ_SQL_E_CATALOG_HANDLE_PROVENANCE",
    "authored_guc_registration_access.cppm":
        "BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE",
    "authored_catalog_handle_namespace_alias.cppm":
        "BIOSEQ_SQL_E_CATALOG_HANDLE_PROVENANCE",
    "authored_guc_access_using_declaration.cppm":
        "BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE",
    "authored_guc_registration_type_alias.cppm":
        "BIOSEQ_SQL_E_GUC_REGISTRATION_PROVENANCE",
}


def fail(message: str) -> None:
    raise RuntimeError(message)


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def read_bytes(path: Path) -> bytes:
    if not path.is_file():
        fail(f"required GCC-only contract artifact is absent: {path}")
    return path.read_bytes()


def read_text(path: Path) -> str:
    return read_bytes(path).decode("utf-8")


def load_json_bytes(path: Path, raw: bytes) -> dict[str, Any]:
    try:
        value = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"invalid JSON in {path}: {error}")
    if not isinstance(value, dict):
        fail(f"expected a JSON object in {path}")
    return value


def load_json(path: Path) -> tuple[bytes, dict[str, Any]]:
    raw = read_bytes(path)
    return raw, load_json_bytes(path, raw)


def require_sha256(value: Any, label: str) -> str:
    rendered = str(value)
    if not SHA256.fullmatch(rendered):
        fail(f"{label} is not a lowercase SHA-256 digest")
    return rendered


def require_exact_object(
    value: Any,
    expected_keys: frozenset[str],
    label: str,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        fail(f"{label} is not an object")
    actual_keys = set(value)
    missing = sorted(expected_keys - actual_keys)
    unknown = sorted(actual_keys - expected_keys)
    if missing or unknown:
        fail(
            f"{label} schema differs; missing={missing}, unknown={unknown}"
        )
    return value


def require_nonempty_string(
    value: dict[str, Any],
    key: str,
    label: str,
) -> str:
    rendered = value[key]
    if not isinstance(rendered, str) or not rendered:
        fail(f"{label}.{key} is not a non-empty string")
    return rendered


def require_optional_string(value: Any, label: str) -> None:
    if value is not None and not isinstance(value, str):
        fail(f"{label} is neither null nor a string")


def require_string_array(
    value: Any,
    label: str,
    *,
    nonempty: bool = False,
) -> list[str]:
    if (
        not isinstance(value, list)
        or (nonempty and not value)
        or any(not isinstance(item, str) or not item for item in value)
        or len(value) != len(set(value))
    ):
        fail(f"{label} is not a unique non-empty-string array")
    return value


def canonical_regular(path: Path, root: Path | None = None) -> Path:
    absolute = path.absolute()
    try:
        info = absolute.lstat()
    except FileNotFoundError:
        fail(f"configured generated artifact is absent: {path}")
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        fail(f"configured generated artifact is not a regular file: {path}")
    if info.st_nlink != 1:
        fail(f"configured generated artifact is hard-linked: {path}")
    canonical = absolute.resolve(strict=True)
    if canonical != absolute:
        fail(f"generated artifact path is not canonical: {path}")
    if root is not None:
        canonical_root = root.resolve(strict=True)
        try:
            canonical.relative_to(canonical_root)
        except ValueError:
            fail(
                f"generated artifact escapes the configured root: {canonical}"
            )
    return canonical


def canonical_repository_source(
    repository_root: Path,
    source_path: Any,
    label: str,
) -> Path:
    if (
        not isinstance(source_path, str)
        or not source_path
        or "\x00" in source_path
    ):
        fail(f"{label} source path is empty or malformed")
    relative = Path(source_path)
    if (
        relative.is_absolute()
        or source_path != relative.as_posix()
        or "\\" in source_path
        or relative == Path(".")
        or ".." in relative.parts
    ):
        fail(f"{label} source path is not canonical repository-relative")
    source = canonical_regular(repository_root / relative, repository_root)
    if source.relative_to(repository_root).as_posix() != source_path:
        fail(f"{label} source path differs from its canonical repository path")
    return source


def exported_generated_module(path: Path, text: str) -> str:
    found = GENERATED_MODULE.search(text)
    if found is None:
        fail(f"generated artifact has no accepted module declaration: {path}")
    return found.group(1)


def cpp_terminal_identifiers(text: str) -> set[str]:
    """Return use-site identifiers outside C++ non-code and module names."""
    identifiers: set[str] = set()
    index = 0
    size = len(text)
    module_or_import_name = False
    raw_prefix = re.compile(
        r'(?:u8|u|U|L)?R"(?P<delimiter>[^\s()\\]{0,16})\('
    )
    while index < size:
        if text.startswith("//", index):
            newline = text.find("\n", index + 2)
            index = size if newline < 0 else newline + 1
            continue
        if text.startswith("/*", index):
            close = text.find("*/", index + 2)
            index = size if close < 0 else close + 2
            continue
        raw = raw_prefix.match(text, index)
        if raw is not None:
            terminator = ")" + raw.group("delimiter") + '"'
            close = text.find(terminator, raw.end())
            index = size if close < 0 else close + len(terminator)
            continue
        if text[index] in {'"', "'"}:
            quote = text[index]
            index += 1
            while index < size:
                if text[index] == "\\":
                    index += 2
                elif text[index] == quote:
                    index += 1
                    break
                else:
                    index += 1
            continue
        if module_or_import_name:
            if text[index] == ";":
                module_or_import_name = False
            index += 1
            continue
        if text[index].isalpha() or text[index] == "_":
            end = index + 1
            while (
                end < size
                and (text[end].isalnum() or text[end] == "_")
            ):
                end += 1
            identifier = text[index:end]
            if identifier in {"module", "import"}:
                module_or_import_name = True
            else:
                identifiers.add(identifier)
            index = end
            continue
        index += 1
    return identifiers


def require_provenance_fixture_exercise(
    label: str,
    text: str,
    diagnostic: str,
) -> None:
    matching = [
        identifier
        for identifier, identifier_diagnostic
        in PROVENANCE_IDENTIFIERS.items()
        if identifier_diagnostic == diagnostic
    ]
    if not matching:
        return
    identifiers = cpp_terminal_identifiers(text)
    if not any(identifier in identifiers for identifier in matching):
        fail(
            f"provenance negative fixture {label} does not exercise "
            f"the reserved terminal identifier for {diagnostic}"
        )


def verify_negative_fixture(path: Path, expected_diagnostic: str) -> None:
    text = read_text(path)
    marker = f"// expected-failure: {expected_diagnostic}"
    if not text.startswith(marker):
        fail(f"negative fixture lacks exact marker {marker!r}: {path}")
    if "import bioseq.codegen.sql.annotation_model;" not in text:
        fail(f"negative fixture bypasses the real annotation model: {path}")


def verify_only_imports(
    path: Path,
    text: str,
    expected_modules: tuple[str, ...],
) -> None:
    imported_modules = tuple(
        re.findall(
            r"^\s*(?:export\s+)?import\s+([^;]+);",
            text,
            flags=re.MULTILINE,
        )
    )
    if imported_modules != expected_modules:
        fail(
            f"{path} imports {imported_modules!r}; expected exactly "
            f"{expected_modules!r}"
        )


def require_tokens(path: Path, text: str, tokens: tuple[str, ...]) -> None:
    for token in tokens:
        if token not in text:
            fail(f"{path} lacks compile-contract token {token!r}")


def verify_gcc_toolchain_wiring_documents(
    root_cmake_path: Path,
    root_cmake: str,
    helper_path: Path,
    helper: str,
    codegen_helper_path: Path,
    codegen_helper: str,
    codegen_cmake_path: Path,
    codegen_cmake: str,
    launcher_path: Path,
    launcher: str,
) -> None:
    require_tokens(
        root_cmake_path,
        root_cmake,
        (
            "OUTPUT_C_SHA256 _BIOSEQ_AUTHENTICATED_GCC16_C_SHA256",
            "OUTPUT_CXX_SHA256 _BIOSEQ_AUTHENTICATED_GCC16_CXX_SHA256",
            "OUTPUT_MODULES_SHA256",
            "OUTPUT_MODULE_SOURCE_CLOSURE_SHA256",
            "BIOSEQ_GCC16_C_SHA256",
            "BIOSEQ_GCC16_CXX_SHA256",
            "BIOSEQ_GCC16_MODULES_SHA256",
            "set(BIOSEQ_GCC16_MODULE_SOURCE_CLOSURE_SHA256",
            "BIOSEQ_GCC16_C_COMPILER_LAUNCHER",
            "BIOSEQ_GCC16_CXX_COMPILER_LAUNCHER",
            "set(CMAKE_C_COMPILER_LAUNCHER",
            "set(CMAKE_CXX_COMPILER_LAUNCHER",
            "--bioseq-expected-compiler-sha256=",
            "--bioseq-expected-module-inventory-sha256=",
            "--bioseq-expected-module-source-closure-sha256=",
            "bioseq_verify_gcc16_toolchain_identity(",
            'CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1"',
        ),
    )
    require_tokens(
        helper_path,
        helper,
        (
            "_bioseq_query_gcc_version",
            "_bioseq_gcc_modules_json_is_authentic",
            '"bioseq_gcc_module_source_closure\\n"',
            '"revision=1\\n"',
            "file(SHA256 \"${_selected_c}\" _selected_c_sha256)",
            "file(SHA256 \"${_selected_cxx}\" _selected_cxx_sha256)",
            "OUTPUT_MODULE_SOURCE_CLOSURE_SHA256",
            "bioseq_verify_gcc16_toolchain_identity",
            "authenticated GCC C compiler SHA-256 changed",
            "authenticated GCC C++ compiler SHA-256 changed",
            "authenticated libstdc++ module inventory SHA-256 changed",
            "authenticated libstdc++ module source closure SHA-256 changed",
            'VERSION_LESS "16.1"',
        ),
    )
    require_tokens(
        codegen_helper_path,
        codegen_helper,
        (
            "_bioseq_codegen_authenticated_c_sha256",
            "_bioseq_codegen_authenticated_cxx_sha256",
            "_bioseq_codegen_authenticated_modules_sha256",
            "_bioseq_codegen_authenticated_module_source_closure_sha256",
            "BIOSEQ_GCC16_C_SHA256",
            "BIOSEQ_GCC16_CXX_SHA256",
            "BIOSEQ_GCC16_MODULES_SHA256",
            "BIOSEQ_GCC16_MODULE_SOURCE_CLOSURE_SHA256",
            "bioseq_verify_gcc16_toolchain_identity(",
            "BIOSEQ_CODEGEN_C_COMPILER_LAUNCHER",
            "BIOSEQ_CODEGEN_CXX_COMPILER_LAUNCHER",
            "-DCMAKE_C_COMPILER:FILEPATH=${BIOSEQ_CODEGEN_C_COMPILER}",
            "-DCMAKE_CXX_COMPILER:FILEPATH=${BIOSEQ_CODEGEN_CXX_COMPILER}",
            "-DCMAKE_CXX_STDLIB_MODULES_JSON:FILEPATH="
            "${BIOSEQ_CODEGEN_STDLIB_MODULES_JSON}",
            "-DCMAKE_C_COMPILER_LAUNCHER:STRING="
            "${_bioseq_codegen_c_launcher_argument}",
            "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING="
            "${_bioseq_codegen_cxx_launcher_argument}",
            "-DBIOSEQ_EXPECTED_GCC_C_SHA256:STRING=",
            "-DBIOSEQ_EXPECTED_GCC_CXX_SHA256:STRING=",
            "-DBIOSEQ_EXPECTED_GCC_MODULES_SHA256:STRING=",
            "-DBIOSEQ_EXPECTED_GCC_MODULE_SOURCE_CLOSURE_SHA256:STRING=",
            'if(_llvm_version VERSION_LESS "22")',
            'OR _bioseq_codegen_clang_version VERSION_LESS "22"',
            "-DCLANG_BINARY:FILEPATH=${BIOSEQ_CODEGEN_CLANG_BINARY}",
        ),
    )
    require_tokens(
        codegen_cmake_path,
        codegen_cmake,
        (
            "BIOSEQ_EXPECTED_GCC_ROOT",
            "BIOSEQ_EXPECTED_GCC_C",
            "BIOSEQ_EXPECTED_GCC_CXX",
            "BIOSEQ_EXPECTED_GCC_MODULES",
            "BIOSEQ_EXPECTED_GCC_C_SHA256",
            "BIOSEQ_EXPECTED_GCC_CXX_SHA256",
            "BIOSEQ_EXPECTED_GCC_MODULES_SHA256",
            "BIOSEQ_EXPECTED_GCC_MODULE_SOURCE_CLOSURE_SHA256",
            "bioseq_verify_gcc16_toolchain_identity(",
            'NOT CMAKE_C_COMPILER_ID STREQUAL "GNU"',
            'NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"',
            'CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1"',
            'if(_bioseq_llvm_version VERSION_LESS "22")',
            'OR _bioseq_clang_version VERSION_LESS "22"',
        ),
    )
    require_tokens(
        launcher_path,
        launcher,
        (
            "--bioseq-expected-compiler-sha256",
            "--bioseq-gcc-root",
            "--bioseq-gcc-module-inventory",
            "--bioseq-expected-module-inventory-sha256",
            "--bioseq-expected-module-source-closure-sha256",
            '"bioseq_gcc_module_source_closure\\n"',
            '"revision=1\\n"',
            "verify_compiler_identity(compiler, identity)",
            "authenticated GCC compiler SHA-256 changed",
            "authenticated libstdc++ module inventory SHA-256 changed",
            "authenticated libstdc++ module-source closure SHA-256 changed",
        ),
    )

    gcc_branch_begin = root_cmake.find(
        'elseif(${CMAKE_NAMED_ENV_TYPE} STREQUAL '
        '"AWS_MACHINE_X64_GCC16")'
    )
    gcc_branch_end = root_cmake.find("\nelse()", gcc_branch_begin)
    if gcc_branch_begin < 0 or gcc_branch_end < 0:
        fail("root CMake has no closed GCC-only environment branch")
    gcc_branch = root_cmake[gcc_branch_begin:gcc_branch_end]
    if re.search(
        r"set\s*\(\s*CMAKE_C(?:XX)?_COMPILER\b[^)]*clang",
        gcc_branch,
        flags=re.IGNORECASE | re.DOTALL,
    ):
        fail("GCC-only root branch assigns Clang to a compiler slot")

    clang_command = re.compile(
        r"COMMAND\s+"
        r"(?:\"?\$\{(?:BIOSEQ_CODEGEN_CLANG_BINARY|CLANG_BINARY)\}\"?"
        r"|\"?[^\"\s)]*clang\+\+(?:-[0-9]+)?\"?)"
        r"(?:(?!\bCOMMAND\b).){0,600}?"
        r"(?:[\s\"]-c(?:[\s\"]|$)|-emit-obj)",
        flags=re.IGNORECASE | re.DOTALL,
    )
    for path, text in (
        (root_cmake_path, root_cmake),
        (codegen_helper_path, codegen_helper),
        (codegen_cmake_path, codegen_cmake),
    ):
        if clang_command.search(text):
            fail(f"{path} uses Clang to produce a C++ object")


def run_gcc_wiring_mutation_self_tests(root: Path) -> None:
    paths = {
        "root": root / "CMakeLists.txt",
        "helper": root / "cmake/bioseq_gcc_toolchain.cmake",
        "codegen_helper": root / "cmake/bioseq_codegen.cmake",
        "codegen": root / "codegen/CMakeLists.txt",
        "launcher": root / "cmake/gcc_hwy_header_unit_launcher.py",
    }
    original = {name: read_text(path) for name, path in paths.items()}

    def verify(documents: dict[str, str]) -> None:
        verify_gcc_toolchain_wiring_documents(
            paths["root"],
            documents["root"],
            paths["helper"],
            documents["helper"],
            paths["codegen_helper"],
            documents["codegen_helper"],
            paths["codegen"],
            documents["codegen"],
            paths["launcher"],
            documents["launcher"],
        )

    verify(original)

    def expect_rejected(
        label: str,
        document: str,
        old: str,
        new: str,
    ) -> None:
        if original[document].count(old) < 1:
            fail(f"GCC wiring mutation source is absent: {label}")
        mutated = dict(original)
        mutated[document] = mutated[document].replace(old, new, 1)
        try:
            verify(mutated)
        except RuntimeError:
            return
        fail(f"GCC wiring verifier accepted mutation: {label}")

    expect_rejected(
        "root C++ compiler content seal removed",
        "root",
        "OUTPUT_CXX_SHA256 _BIOSEQ_AUTHENTICATED_GCC16_CXX_SHA256",
        "OUTPUT_CXX_SHA256 _BIOSEQ_AUTHENTICATED_GCC16_C_SHA256",
    )
    expect_rejected(
        "isolated generator compiled with Clang",
        "codegen_helper",
        "-DCMAKE_CXX_COMPILER:FILEPATH=${BIOSEQ_CODEGEN_CXX_COMPILER}",
        "-DCMAKE_CXX_COMPILER:FILEPATH=${BIOSEQ_CODEGEN_CLANG_BINARY}",
    )
    expect_rejected(
        "isolated generator C/C++ pair diverged",
        "codegen_helper",
        "-DCMAKE_C_COMPILER:FILEPATH=${BIOSEQ_CODEGEN_C_COMPILER}",
        "-DCMAKE_C_COMPILER:FILEPATH=${BIOSEQ_CODEGEN_CXX_COMPILER}",
    )
    expect_rejected(
        "isolated generator compiler launcher removed",
        "codegen_helper",
        "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING="
        "${_bioseq_codegen_cxx_launcher_argument}",
        "-DBIOSEQ_REMOVED_CXX_LAUNCHER:STRING=1",
    )
    expect_rejected(
        "root module-source closure join removed",
        "root",
        "set(BIOSEQ_GCC16_MODULE_SOURCE_CLOSURE_SHA256",
        "set(BIOSEQ_GCC16_REMOVED_MODULE_SOURCE_CLOSURE_SHA256",
    )
    expect_rejected(
        "parent LLVM floor downgraded",
        "codegen_helper",
        'if(_llvm_version VERSION_LESS "22")',
        'if(_llvm_version VERSION_LESS "21")',
    )
    expect_rejected(
        "child Clang floor downgraded",
        "codegen",
        'OR _bioseq_clang_version VERSION_LESS "22"',
        'OR _bioseq_clang_version VERSION_LESS "21"',
    )
    injected = dict(original)
    injected["codegen_helper"] += (
        "\nadd_custom_command(OUTPUT injected.o\n"
        '  COMMAND "${BIOSEQ_CODEGEN_CLANG_BINARY}" '
        "-std=c++26 -c injected.cpp -o injected.o)\n"
    )
    try:
        verify(injected)
    except RuntimeError:
        pass
    else:
        fail(
            "GCC wiring verifier accepted injected Clang object compilation"
        )


def cmake_target_call(
    text: str,
    command: str,
    target: str,
) -> str:
    match = re.search(
        rf"{re.escape(command)}\(\s*{re.escape(target)}\b(.*?)\)",
        text,
        flags=re.DOTALL,
    )
    if match is None:
        fail(f"CMake target {target} has no {command} call")
    return match.group(0)


def require_scoped_annotation_options(label: str, cmake_call: str) -> None:
    required = (
        "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>",
        "$<$<CXX_COMPILER_ID:Clang,AppleClang>:"
        "-Wno-unknown-attributes>",
    )
    for option in required:
        if option not in cmake_call:
            fail(f"annotation-owning {label} lacks scoped option {option}")


def verify_annotation_fixtures(root: Path) -> None:
    positive_module_path = (
        root / "codegen/sql/tests/contract/fixtures/annotation_probe.cppm"
    )
    positive_tu_path = (
        root / "codegen/sql/tests/contract/fixtures/annotation_probe.cpp"
    )
    combined = (
        read_text(positive_module_path) + "\n" + read_text(positive_tu_path)
    )
    required_positive = (
        "[[bioseq_sql::",
        "struct [[bioseq_sql::type({",
        "using sequence_alias [[bioseq_sql::type]]",
        "[[bioseq_sql::column({})]]",
        "[[bioseq_sql::column(reusable_column_options)]]",
        "debug2 [[bioseq_sql::guc_value({",
        "static bool letter_eq(",
        "inline constexpr bool rank_less(",
        "static inline bool enabled = true;",
    )
    require_tokens(positive_module_path, combined, required_positive)

    negative_root = root / "codegen/sql/tests/contract/fixtures/negative"
    for filename, diagnostic in NEGATIVE_DIAGNOSTICS.items():
        verify_negative_fixture(negative_root / filename, diagnostic)

    contract_path = root / "codegen/sql/tests/contract/index.cppm"
    contract = read_text(contract_path)
    require_tokens(
        contract_path,
        contract,
        (
            "import bioseq.codegen.structural_analysis.tests.contract;",
            "run_preparation_contract_self_tests",
            "run_guc_generator_contract_self_tests",
            "run_fmgr_renderer_contract_tests",
            "run_compact_fmgr_contract_tests",
            "validate_codegen_completion_policy",
            "bioseq.postgres.sql.generated",
            "bioseq.postgres.support.sql_boundary",
        ),
    )
    collector_path = root / "codegen/sql/collector/index.cppm"
    collector = read_text(collector_path)
    require_tokens(
        collector_path,
        collector,
        (
            "structural_analysis::scan_translation_unit(source_path)",
            "contains_structural_terminal_identifier",
            "relation_entity_handle",
            "postgres_enum_registration_access",
            "postgres_enum_registration",
            "BIOSEQ_SQL_E_CATALOG_HANDLE_PROVENANCE",
            "BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE",
            "BIOSEQ_SQL_E_GUC_REGISTRATION_PROVENANCE",
        ),
    )
    provenance_validator = collector[
        collector.find("collect_provenance_checked_fixture") :
    ]
    if "source.contains(" in provenance_validator:
        fail("provenance validator falls back to raw source substrings")
    options_path = root / "codegen/sql/collector/options.cppm"
    require_tokens(
        options_path,
        read_text(options_path),
        (
            'attribute.name == "guc_value"',
            "detail::split_top_level",
            "BIOSEQ_SQL_E_MIXED_INITIALIZER",
        ),
    )
def verify_compile_contract_fixtures(root: Path) -> None:
    fixture_root = (
        root / "bioseq/tests/contract/postgres/sql"
    )
    catalog_producer = fixture_root / (
        "catalog_entity_handle_producer_test.cppm"
    )
    catalog_producer_text = read_text(catalog_producer)
    verify_only_imports(
        catalog_producer,
        catalog_producer_text,
        ("bioseq.postgres.support.sql_catalog_entities",),
    )
    require_tokens(
        catalog_producer,
        catalog_producer_text,
        (
            "RELKIND_SEQUENCE",
            "sql_schema_binding::fixed",
            "sql_schema_binding::extension",
            'stable_schema_name() == "$extension"',
            "core_descriptor",
        ),
    )

    catalog_consumer = fixture_root / (
        "catalog_entity_handle_consumer_test.cpp"
    )
    catalog_consumer_text = read_text(catalog_consumer)
    verify_only_imports(
        catalog_consumer,
        catalog_consumer_text,
        (
            "bioseq.postgres.sql.generated.core."
            "catalog_entity_handle_producer_test",
        ),
    )
    require_tokens(
        catalog_consumer,
        catalog_consumer_text,
        (
            "fixed_handle::stable_qualified_name()",
            "fixed_handle::installed_schema_name(policy)",
            "fixed_handle::installed_qualified_name(policy)",
            "fixed_handle::resolve_oid(policy)",
            "extension_handle::stable_qualified_name()",
            "extension_handle::installed_schema_name(policy)",
            "extension_handle::installed_qualified_name(policy)",
            "extension_handle::resolve_oid(policy)",
        ),
    )

    guc_producer = fixture_root / "generated_guc_producer_test.cppm"
    guc_producer_text = read_text(guc_producer)
    verify_only_imports(
        guc_producer,
        guc_producer_text,
        ("bioseq.postgres.support.enum_storage",),
    )
    require_tokens(
        guc_producer,
        guc_producer_text,
        (
            "postgres_enum_storage<wire_mode>",
            "postgres_enum_registration_access::initialize(",
            "register_storage(",
        ),
    )

    guc_consumer = fixture_root / "generated_guc_consumer_test.cpp"
    guc_consumer_text = read_text(guc_consumer)
    verify_only_imports(
        guc_consumer,
        guc_consumer_text,
        ("bioseq.postgres.sql.generated.core.generated_guc_producer_test",),
    )
    require_tokens(
        guc_consumer,
        guc_consumer_text,
        (
            "!std::is_default_constructible_v<contract::storage>",
            "!std::is_copy_constructible_v<contract::storage>",
            "!std::is_move_constructible_v<contract::storage>",
            "!exposes_member_init<",
            "!exposes_wire_cell<contract::storage>",
            "contract::register_storage(storage, registration)",
        ),
    )

    boundary_producer = fixture_root / (
        "sql_boundary_template_producer_test.cppm"
    )
    boundary_producer_text = read_text(boundary_producer)
    verify_only_imports(
        boundary_producer,
        boundary_producer_text,
        (
            "bioseq.postgres.support.sql_boundary",
            "bioseq.postgres.storage.indexing.sql_row_codecs",
            "bioseq.postgres.storage.manager.sql_row_codecs",
            "bioseq.postgres.storage.metadata.sql_row_codecs",
        ),
    )
    require_tokens(
        boundary_producer,
        boundary_producer_text,
        (
            "bioseq::postgres::support::set_of<",
            "bioseq::postgres::support::table_rows<",
            "bioseq::postgres::support::named_output_set<",
            "bioseq::postgres::support::named_output<",
            "&bioseq::postgres::support::"
            "invoke_postgres_callback<boundary_callable>",
            "chunk_group_codec",
            "minimizer_generation_storage_codec",
        ),
    )

    boundary_consumer = fixture_root / (
        "sql_boundary_template_consumer_test.cpp"
    )
    boundary_consumer_text = read_text(boundary_consumer)
    verify_only_imports(
        boundary_consumer,
        boundary_consumer_text,
        (
            "bioseq.postgres.sql.generated.core."
            "sql_boundary_template_producer_test",
        ),
    )
    require_tokens(
        boundary_consumer,
        boundary_consumer_text,
        (
            "std::optional<FullTransactionId>",
            "std::optional<bytea*>",
            "NameData",
            "TimestampTz",
            "text*>",
            "!registered_codec_member<",
            "!publicly_constructible_encoded_row<",
            "update.template set<",
            "update.template set_null<",
            "::scan_key<",
            "::deform(",
            "contract::boundary_instantiation(",
        ),
    )

    materialization = fixture_root / "result_materialization_test.cppm"
    materialization_text = read_text(materialization)
    verify_only_imports(
        materialization,
        materialization_text,
        (
            "bioseq.postgres.support.sql_boundary",
            "bioseq.postgres.storage.manager.sql_rows",
            "bioseq.postgres.table.sql_rows",
        ),
    )
    require_tokens(
        materialization,
        materialization_text,
        (
            "using bioseq::postgres::support::table_rows;",
            "using bioseq::postgres::support::named_output_set;",
            "using bioseq::postgres::support::named_output;",
            "table_rows<reaper_event>",
            "named_output_set<teardown_diagnostics_row>",
            "named_output<teardown_diagnostics_row>",
            "bioseq::postgres::support::encode_result_row(",
            "bioseq::postgres::support::encode_result(",
            "encoded_reaper_event::field_count",
        ),
    )

    entity_handles = fixture_root / "sql_entity_handles_test.cppm"
    entity_handles_text = read_text(entity_handles)
    require_tokens(
        entity_handles,
        entity_handles_text,
        (
            "bioseq.postgres.sql.generated.core.storage_metadata_core.",
            "bioseq.postgres.sql.generated.core."
            "storage_indexing_pg_chunked_storage.entity_handles",
            "bioseq.postgres.sql.generated.core."
            "storage_manager_sequence_storage_manager.entity_handles",
            "bioseq.postgres.storage.indexing.sql_row_codecs",
            "bioseq.postgres.storage.io.sql_row_codecs",
            "bioseq.postgres.storage.manager.sql_row_codecs",
            "bioseq.postgres.storage.metadata.sql_row_codecs",
            "bioseq.postgres.table.sql_row_codecs",
        ),
    )

    active_guc = fixture_root / "active_generated_guc_consumer_test.cpp"
    active_guc_text = read_text(active_guc)
    require_tokens(
        active_guc,
        active_guc_text,
        (
            "import bioseq.postgres.sql.generated.production.bioseq.gucs;",
            "import bioseq.postgres.sql.generated.production.guc_registry;",
            "import bioseq.postgres.sql.generated.test_indexes.bioseq.gucs;",
            "import bioseq.postgres.sql.generated.test_indexes.guc_registry;",
            "active_guc_owner::registration_count == 26U",
            "active_guc_owner::registration_count == 29U",
            "registration_count == 73U",
            "registration_count == 76U",
            "&active_guc_owner::register_diagnostics_logging_enabled",
            "&active_guc_registry::register_gucs",
        ),
    )


def verify_provenance_fixtures(root: Path) -> None:
    lexical_decoys = r'''
// relation_entity_handle postgres_enum_registration_access
/* postgres_enum_registration */
export module decoy.postgres_enum_registration_access;
import decoy.relation_entity_handle;
export import decoy.postgres_enum_registration;
constexpr auto a = "relation_entity_handle";
constexpr auto b = R"guard(postgres_enum_registration_access
postgres_enum_registration)guard";
constexpr char c = 'x';
'''
    if (
        cpp_terminal_identifiers(lexical_decoys)
        & set(PROVENANCE_IDENTIFIERS)
    ):
        fail("static provenance lexer accepts comment/string decoys")

    fixture_root = (
        root / "bioseq/tests/contract/postgres/sql"
    )
    producer = fixture_root / "catalog_entity_handle_producer_test.cppm"
    guc_producer = fixture_root / "generated_guc_producer_test.cppm"
    if (
        "relation_entity_handle"
        not in cpp_terminal_identifiers(read_text(producer))
    ):
        fail("generated-like catalog producer has no handle specialization")
    if (
        "postgres_enum_registration_access"
        not in cpp_terminal_identifiers(read_text(guc_producer))
    ):
        fail("generated-like GUC producer does not instantiate access")

    allowed_relative: dict[str, dict[str, tuple[str, ...]]] = {
        "relation_entity_handle": {
            "definitions": (
                "bioseq/backend/pg/support/"
                "sql_catalog_entities.cppm",
            ),
            "infrastructure": (),
            "tests": (
                "bioseq/tests/contract/postgres/sql/"
                "catalog_entity_handle_producer_test.cppm",
            ),
        },
        "postgres_enum_registration_access": {
            "definitions": (
                "common/postgres/support/enum_storage.cppm",
            ),
            "infrastructure": (
                "bioseq/backend/pg/support/sql_boundary/"
                "guc_storage.cppm",
            ),
            "tests": (
                "bioseq/tests/contract/postgres/sql/"
                "generated_guc_producer_test.cppm",
                "common/postgres/test_support/tests/contract/enum_storage_test.cpp",
            ),
        },
        "postgres_enum_registration": {
            "definitions": (
                "bioseq/backend/pg/support/sql_boundary/"
                "guc_storage.cppm",
            ),
            "infrastructure": (),
            "tests": (),
        },
    }
    allowed: dict[str, set[Path]] = {}
    for identifier, categories in allowed_relative.items():
        resolved_paths: set[Path] = set()
        if set(categories) != {"definitions", "infrastructure", "tests"}:
            fail(f"provenance allowlist categories changed for {identifier}")
        for relative_paths in categories.values():
            for relative_path in relative_paths:
                path = root / relative_path
                if not path.is_file() or path.is_symlink():
                    fail(
                        "provenance allowlist path is not a canonical regular "
                        f"source: {path}"
                    )
                canonical = path.resolve(strict=True)
                if canonical != path.absolute():
                    fail(
                        f"provenance allowlist path is not canonical: {path}"
                    )
                resolved_paths.add(canonical)
        allowed[identifier] = resolved_paths

    for source_root in (
        root / "bioseq/backend",
        root / "common/postgres",
    ):
        for path in sorted(source_root.rglob("*")):
            if path.suffix not in {".cpp", ".cppm"}:
                continue
            identifiers = cpp_terminal_identifiers(read_text(path))
            canonical = path.resolve(strict=True)
            for identifier, diagnostic in PROVENANCE_IDENTIFIERS.items():
                if (
                    identifier in identifiers
                    and canonical not in allowed[identifier]
                ):
                    fail(
                        f"{diagnostic}: reserved terminal identifier "
                        f"{identifier!r} in unauthorized source {path}"
                    )

    negative_root = root / "codegen/sql/tests/contract/fixtures/negative"
    for filename, diagnostic in NEGATIVE_DIAGNOSTICS.items():
        require_provenance_fixture_exercise(
            filename,
            read_text(negative_root / filename),
            diagnostic,
        )

    access_fixture = negative_root / "authored_guc_registration_access.cppm"
    access_text = read_text(access_fixture)
    actual_access = (
        "    bioseq::postgres::support::postgres_enum_registration_access::initialize(\n"
        "        storage, authored_registrar{});\n"
    )
    if access_text.count(actual_access) != 1:
        fail("GUC access fixture lacks one exact forbidden use expression")
    access_without_use = access_text.replace(actual_access, "", 1)
    try:
        require_provenance_fixture_exercise(
            "authored_guc_registration_access.cppm without access use",
            access_without_use,
            "BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE",
        )
    except RuntimeError as error:
        if "does not exercise" not in str(error):
            raise
    else:
        fail(
            "provenance source mutation accepted deletion of the actual "
            "GUC access expression"
        )


def verify_proof_vs_scan_wiring(cmake_path: Path, cmake: str) -> None:
    forbidden_harness_path = (
        cmake_path.parent.parent
        / "codegen/sql/tests/integration/test_production_determinism.py"
    )
    if forbidden_harness_path.exists() or forbidden_harness_path.is_symlink():
        fail(
            "forbidden production-determinism migration harness source "
            f"was retained: {forbidden_harness_path}"
        )
    require_tokens(
        cmake_path,
        cmake,
        (
            "BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY",
            "function(_bioseq_finalize_sql_target_dyndep_anchors)",
            "get_target_property(_bioseq_sql_target_binary_dir",
            '"${_bioseq_sql_target}" BINARY_DIR)',
            '"${_bioseq_sql_target_binary_dir}/CMakeFiles/'
            '${_bioseq_sql_target}.dir/CXX.dd"',
            'file(CONFIGURE\n'
            '    OUTPUT "${BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY}"',
            "cmake_language(DEFER CALL\n"
            "  _bioseq_finalize_sql_target_dyndep_anchors)",
            "BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP",
            "add_custom_target(bioseq_sql_postgresql_link_proof\n"
            "  DEPENDS "
            '"${BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP}")',
        ),
    )
    forbidden_migration_harness_tokens = (
        "BIOSEQ_SQL_PRODUCTION_DETERMINISM_SCRIPT",
        "BIOSEQ_SQL_PRODUCTION_DETERMINISM_ROOT",
        "BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT",
        "BIOSEQ_SQL_PRODUCTION_DETERMINISM_SELF_TEST_STAMP",
        "bioseq_sql_production_determinism_self_test",
        "bioseq_sql_production_determinism",
        "codegen/sql/tests/integration/test_production_determinism.py",
        "production-determinism",
        "--required-target-dyndep-inventory",
        "--ordinary-output",
    )
    retained_harness_tokens = tuple(
        token
        for token in forbidden_migration_harness_tokens
        if token in cmake
    )
    if retained_harness_tokens:
        fail(
            "forbidden production-determinism migration harness wiring "
            f"was retained: {retained_harness_tokens}"
        )
    if "BIOSEQ_SQL_POSTGRES_EXTENSION_CXX_DYNDEP" in cmake:
        fail("extension-only SQL PREPARE dyndep wiring was retained")

    active_support_selection = (
        "set(_bioseq_typed_sql_support_profile production)\n"
        "if(BIOSEQ_TEST_INDEXES)\n"
        "  set(_bioseq_typed_sql_support_profile test_indexes)\n"
        "endif()\n"
        "set(BIOSEQ_TYPED_SQL_SUPPORT_STAMP\n"
        "  \"${CMAKE_CURRENT_BINARY_DIR}/generated/sql/profiles/"
        "${_bioseq_typed_sql_support_profile}/support.stamp\")\n"
        "unset(_bioseq_typed_sql_support_profile)"
    )
    if cmake.count(active_support_selection) != 1:
        fail(
            "template-flatten PREPARE does not select exactly one active-"
            "profile SQL SUPPORT dependency"
        )
    if cmake.count(
        'PREPARE_DEPENDS "${BIOSEQ_TYPED_SQL_SUPPORT_STAMP}"'
    ) != 1:
        fail(
            "template-flatten PREPARE does not consume exactly one active-"
            "profile SQL SUPPORT stamp"
        )
    active_support_guard = (
        "if(BIOSEQ_TEST_INDEXES)\n"
        "  set(_bioseq_expected_typed_sql_support_stamp\n"
        "    \"${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_STAMP}\")\n"
        "else()\n"
        "  set(_bioseq_expected_typed_sql_support_stamp\n"
        "    \"${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}\")\n"
        "endif()\n"
        "if(NOT \"${BIOSEQ_TYPED_SQL_SUPPORT_STAMP}\" STREQUAL\n"
        "    \"${_bioseq_expected_typed_sql_support_stamp}\")"
    )
    if cmake.count(active_support_guard) != 1:
        fail(
            "template-flatten SQL SUPPORT dependency is not guarded against "
            "both configured profiles"
        )

    typed_prepare_ordering = re.search(
        r"add_custom_command\(\s*"
        r"OUTPUT\s*"
        r'"\$\{BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT\}'
        r'/pcm/metadata/prepare_compile_commands\.json"\s*'
        r"APPEND\s*"
        r"DEPENDS\s*"
        r'"\$\{BIOSEQ_SQL_CORE_GENERATED_STAMP\}"\s*'
        r'"\$\{BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP\}"\s*'
        r"\)",
        cmake,
        flags=re.DOTALL,
    )
    if typed_prepare_ordering is None:
        fail(
            "typed-template PREPARE is not ordered after the exact CORE and "
            "production SQL SUPPORT stamps"
        )

    anchor_definition = re.search(
        r"set\(_bioseq_sql_target_dyndep_anchors\s+"
        r"(.*?)\)\s*"
        r"if\(TARGET "
        r"bioseq_sql_row_codec_runtime_test_support_compile_donor\)\s*"
        r"list\(APPEND _bioseq_sql_target_dyndep_anchors\s+"
        r"(.*?)\)\s*"
        r"endif\(\)\s*"
        r"if\(BIOSEQ_BUILD_CACHE\)\s*"
        r"list\(APPEND _bioseq_sql_target_dyndep_anchors\s+"
        r"(.*?)\)\s*"
        r"endif\(\)",
        cmake,
        flags=re.DOTALL,
    )
    if anchor_definition is None:
        fail("required SQL PREPARE dyndep anchor declaration is absent")
    observed_base_anchors = set(anchor_definition.group(1).split())
    expected_base_anchors = {
        "bioseq_benchmark",
        "bioseq_benchmark_e2e_contract_tests",
        "bioseq_sql_annotation_probe",
        "bioseq_sql_gcc_compile_contract_objects",
        "postgresql_bioseq",
    }
    if observed_base_anchors != expected_base_anchors:
        fail(
            "required SQL PREPARE base dyndep anchors differ: "
            f"expected={sorted(expected_base_anchors)}, "
            f"observed={sorted(observed_base_anchors)}"
        )
    observed_profile_donor_anchors = set(
        anchor_definition.group(2).split()
    )
    expected_profile_donor_anchors = {
        "bioseq_sql_row_codec_runtime_test_support_compile_donor",
    }
    if observed_profile_donor_anchors != expected_profile_donor_anchors:
        fail(
            "test-profile SQL PREPARE dyndep donor anchor is not exact: "
            f"{sorted(observed_profile_donor_anchors)}"
        )
    observed_cache_anchors = set(anchor_definition.group(3).split())
    if observed_cache_anchors != {"postgresql_bioseq_cache"}:
        fail(
            "cache-enabled SQL PREPARE dyndep anchor is not exact: "
            f"{sorted(observed_cache_anchors)}"
        )

    finalizer = re.search(
        r"function\(_bioseq_finalize_sql_target_dyndep_anchors\)"
        r"(.*?)"
        r"endfunction\(\)",
        cmake,
        flags=re.DOTALL,
    )
    finalizer_body = "" if finalizer is None else finalizer.group(1)
    output_loop = re.search(
        r"foreach\(_bioseq_sql_prepare_output IN ITEMS\s+"
        r"(.*?)\)\s*"
        r".*?"
        r"add_custom_command\(\s*"
        r'OUTPUT "\$\{_bioseq_sql_prepare_output\}"\s*'
        r"APPEND\s+"
        r"DEPENDS\s+"
        r'"\$\{BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY\}"\s+'
        r"\$\{_bioseq_sql_target_dyndep_paths\}\s*"
        r"\)\s*"
        r"endforeach\(\)",
        finalizer_body,
        flags=re.DOTALL,
    )
    if output_loop is None:
        fail(
            "SQL PREPARE outputs do not consume the configure-derived "
            "target dyndep inventory and anchor paths"
        )
    observed_outputs = set(output_loop.group(1).split())
    expected_outputs = {
        '"${BIOSEQ_SQL_SAFETY_PRODUCTION_PREPARE_STAMP}"',
        '"${BIOSEQ_SQL_SAFETY_TEST_INDEXES_PREPARE_STAMP}"',
    }
    if observed_outputs != expected_outputs:
        fail(
            "target dyndep anchors are not attached to the exact "
            "two safety PREPARE outputs"
        )

    proof_command = re.search(
        r"add_custom_command\(\s*"
        r'OUTPUT\s+"\$\{BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP\}"'
        r"(.*?)VERBATIM\s*\)",
        cmake,
        flags=re.DOTALL,
    )
    proof_body = "" if proof_command is None else proof_command.group(1)
    for target in (
        "bioseq_sql_generation_contract",
        "bioseq_sql_gcc_compile_contracts",
    ):
        if proof_body.count(target) != 1:
            fail(
                "PostgreSQL extension proof stamp does not depend exactly "
                f"once on {target}"
            )
    if "bioseq_sql_postgres_error_safety" in proof_body:
        fail(
            "PostgreSQL extension link proof depends on the opt-in "
            "PostgreSQL compiler-facts/proof closure"
        )

    dependency_blocks = re.findall(
        r"add_dependencies\(\s*([A-Za-z0-9_]+)\s+(.*?)\)",
        cmake,
        flags=re.DOTALL,
    )
    dependency_tokens: dict[str, set[str]] = {}
    for owner, body in dependency_blocks:
        dependency_tokens.setdefault(owner, set()).update(
            re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", body)
        )
    external_dependencies = dependency_tokens.get(
        "bioseq_sql_external_feature_contract", set()
    )
    if external_dependencies:
        fail(
            "external feature contract acquired unexpected target-level "
            f"prerequisites: {sorted(external_dependencies)}"
        )
    generation_contract_dependencies = dependency_tokens.get(
        "bioseq_sql_generation_contract", set()
    )
    required_generation_contract_dependencies = {
        "bioseq_sql_external_feature_contract",
        "bioseq_sql_validator_self_test",
    }
    if generation_contract_dependencies != (
        required_generation_contract_dependencies
    ):
        fail(
            "SQL generation contract prerequisites differ from the "
            "current feature and validator closure"
        )
    if (
        "bioseq_sql_postgres_error_safety"
        in generation_contract_dependencies
    ):
        fail(
            "SQL generation contract depends on the opt-in PostgreSQL "
            "compiler-facts/proof closure"
        )
    require_tokens(
        cmake_path,
        cmake,
        (
            "set(_bioseq_sql_generation_contract_outputs\n"
            "  ${_bioseq_sql_final_stamps}\n"
            "  ${_bioseq_sql_external_feature_contract_stamps})",
            "add_custom_target("
            "bioseq_sql_postgres_error_safety_static_evidence\n"
            '    DEPENDS "${BIOSEQ_SQL_SAFETY_VALIDATION_REPORT}")',
        ),
    )
    if re.search(
        r"list\(APPEND\s+_bioseq_sql_generation_contract_outputs\b"
        r".*?BIOSEQ_SQL_SAFETY_VALIDATION_REPORT",
        cmake,
        flags=re.DOTALL,
    ):
        fail(
            "SQL generation outputs include the opt-in PostgreSQL "
            "safety evidence report"
        )

    safety_pair = re.search(
        r"bioseq_add_postgres_error_safety_pair\(\s*"
        r"TARGET\s+bioseq_sql_postgres_error_safety\s+"
        r"PRODUCTION_TARGET\s+"
        r"bioseq_sql_postgres_error_safety_production\s+"
        r"TEST_INDEXES_TARGET\s+"
        r"bioseq_sql_postgres_error_safety_test_indexes\s*\)",
        cmake,
        flags=re.DOTALL,
    )
    if safety_pair is None:
        fail(
            "opt-in PostgreSQL safety target does not retain both "
            "profile compiler-facts/proof closures"
        )
    safety_dependencies = dependency_tokens.get(
        "bioseq_sql_postgres_error_safety", set()
    )
    if safety_dependencies != {
        "bioseq_sql_postgres_error_safety_inventory_production",
        "bioseq_sql_postgres_error_safety_inventory_test_indexes",
    }:
        fail(
            "opt-in PostgreSQL safety target does not retain its exact "
            "two profile inventory closures"
        )
    link_gate = re.search(
        r"set_property\(TARGET\s+postgresql_bioseq\s+APPEND\s+"
        r"PROPERTY\s+LINK_DEPENDS\s+"
        r'"\$\{BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP\}"\s*\)',
        cmake,
        flags=re.DOTALL,
    )
    if link_gate is None:
        fail("PostgreSQL extension link does not consume the proof stamp")

    forbidden_target_wide = re.search(
        r"add_dependencies\(\s*"
        r"(?:postgresql_bioseq|bioseq_benchmark)\s+"
        r"(?:bioseq_sql_generation_contract"
        r"|bioseq_sql_gcc_compile_contracts"
        r"|bioseq_sql_postgres_error_safety"
        r"|bioseq_sql_postgresql_link_proof)\b",
        cmake,
        flags=re.DOTALL,
    )
    if forbidden_target_wide is not None:
        fail(
            "SQL proof target re-enters an extension or benchmark .ddi "
            "object-order closure"
        )


def run_proof_vs_scan_wiring_mutation_self_tests(root: Path) -> None:
    cmake_path = root / "bioseq/CMakeLists.txt"
    original = read_text(cmake_path)
    verify_proof_vs_scan_wiring(cmake_path, original)

    def expect_rejected(label: str, old: str, new: str) -> None:
        if original.count(old) != 1:
            fail(f"proof-vs-scan mutation source is ambiguous: {label}")
        mutated = original.replace(old, new, 1)
        try:
            verify_proof_vs_scan_wiring(cmake_path, mutated)
        except RuntimeError:
            return
        fail(f"proof-vs-scan verifier accepted mutation: {label}")

    expect_rejected(
        "unique benchmark dyndep anchor removed",
        "  set(_bioseq_sql_target_dyndep_anchors\n"
        "    bioseq_benchmark\n"
        "    bioseq_benchmark_e2e_contract_tests\n"
        "    bioseq_sql_annotation_probe\n"
        "    bioseq_sql_gcc_compile_contract_objects\n"
        "    postgresql_bioseq)",
        "  set(_bioseq_sql_target_dyndep_anchors\n"
        "    bioseq_benchmark_e2e_contract_tests\n"
        "    bioseq_sql_annotation_probe\n"
        "    bioseq_sql_gcc_compile_contract_objects\n"
        "    postgresql_bioseq)",
    )
    expect_rejected(
        "row-codec test-profile donor dyndep anchor removed",
        "  if(TARGET "
        "bioseq_sql_row_codec_runtime_test_support_compile_donor)\n"
        "    list(APPEND _bioseq_sql_target_dyndep_anchors\n"
        "      bioseq_sql_row_codec_runtime_test_support_compile_donor)\n"
        "  endif()\n",
        "",
    )
    expect_rejected(
        "test-indexes active SQL SUPPORT selection removed",
        "if(BIOSEQ_TEST_INDEXES)\n"
        "  set(_bioseq_typed_sql_support_profile test_indexes)\n"
        "endif()\n",
        "",
    )
    expect_rejected(
        "active SQL SUPPORT PREPARE dependency bypassed",
        '  PREPARE_DEPENDS "${BIOSEQ_TYPED_SQL_SUPPORT_STAMP}"\n',
        '  PREPARE_DEPENDS "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}"\n',
    )
    expect_rejected(
        "production row-codec SUPPORT ordering removed",
        "  DEPENDS\n"
        '    "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"\n'
        '    "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}")',
        '  DEPENDS "${BIOSEQ_SQL_CORE_GENERATED_STAMP}")',
    )
    expect_rejected(
        "production row-codec SUPPORT ordering substituted by test indexes",
        "  DEPENDS\n"
        '    "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"\n'
        '    "${BIOSEQ_SQL_PRODUCTION_SUPPORT_STAMP}")',
        "  DEPENDS\n"
        '    "${BIOSEQ_SQL_CORE_GENERATED_STAMP}"\n'
        '    "${BIOSEQ_SQL_TEST_INDEXES_SUPPORT_STAMP}")',
    )
    expect_rejected(
        "test-index safety PREPARE dyndep dependency removed",
        '      "${BIOSEQ_SQL_SAFETY_PRODUCTION_PREPARE_STAMP}"\n'
        '      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_PREPARE_STAMP}")',
        '      "${BIOSEQ_SQL_SAFETY_PRODUCTION_PREPARE_STAMP}")',
    )
    expect_rejected(
        "forbidden migration harness target restored",
        "  bioseq_sql_generation_contract\n"
        "  bioseq_sql_validator_self_test\n"
        "  bioseq_sql_external_feature_contract)",
        "  bioseq_sql_generation_contract\n"
        "  bioseq_sql_validator_self_test\n"
        "  bioseq_sql_external_feature_contract\n"
        "  bioseq_sql_production_determinism)",
    )
    expect_rejected(
        "forbidden migration harness self-test target restored",
        "  bioseq_sql_generation_contract\n"
        "  bioseq_sql_validator_self_test\n"
        "  bioseq_sql_external_feature_contract)",
        "  bioseq_sql_generation_contract\n"
        "  bioseq_sql_validator_self_test\n"
        "  bioseq_sql_external_feature_contract\n"
        "  bioseq_sql_production_determinism_self_test)",
    )
    expect_rejected(
        "forbidden migration harness script wiring restored",
        "set(BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY\n"
        '  "${BIOSEQ_SQL_CODEGEN_DIR}/required-target-dyndeps.tsv")',
        "set(BIOSEQ_SQL_PRODUCTION_DETERMINISM_SCRIPT\n"
        '  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/'
        'integration/test_production_determinism.py")\n'
        "set(BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY\n"
        '  "${BIOSEQ_SQL_CODEGEN_DIR}/required-target-dyndeps.tsv")',
    )
    expect_rejected(
        "PostgreSQL safety proof restored to the default DSO graph",
        "  bioseq_sql_generation_contract\n"
        "  bioseq_sql_validator_self_test\n"
        "  bioseq_sql_external_feature_contract)",
        "  bioseq_sql_generation_contract\n"
        "  bioseq_sql_validator_self_test\n"
        "  bioseq_sql_external_feature_contract\n"
        "  bioseq_sql_postgres_error_safety)",
    )
    expect_rejected(
        "production safety profile removed from the opt-in target",
        "  PRODUCTION_TARGET "
        "bioseq_sql_postgres_error_safety_production\n",
        "  PRODUCTION_TARGET "
        "bioseq_sql_postgres_error_safety_test_indexes\n",
    )
    expect_rejected(
        "link proof changed into target-wide proof",
        "set_property(TARGET postgresql_bioseq APPEND PROPERTY LINK_DEPENDS\n"
        '  "${BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP}")',
        "add_dependencies(postgresql_bioseq "
        "bioseq_sql_generation_contract)",
    )


def verify_sql_codegen_ast_proof_pool_wiring(
    root: Path,
    *,
    root_cmake: str | None = None,
    bioseq_cmake: str | None = None,
    safety_cmake: str | None = None,
) -> None:
    root_cmake_path = root / "CMakeLists.txt"
    bioseq_cmake_path = root / "bioseq/CMakeLists.txt"
    safety_cmake_path = (
        root / "codegen/safety/postgres_error_safety.cmake"
    )
    if root_cmake is None:
        root_cmake = read_text(root_cmake_path)
    if bioseq_cmake is None:
        bioseq_cmake = read_text(bioseq_cmake_path)
    if safety_cmake is None:
        safety_cmake = read_text(safety_cmake_path)

    pool_depths = re.findall(
        r"set_property\(\s*GLOBAL\s+APPEND\s+PROPERTY\s+"
        r"JOB_POOLS\s+sql_codegen_ast_proof=([0-9]+)\s*\)",
        root_cmake,
        flags=re.DOTALL,
    )
    if pool_depths != ["1"]:
        fail(
            "SQL codegen AST proof pool must be declared exactly once "
            f"at depth 1, observed depths={pool_depths}"
        )

    def require_command_pool(
        path: Path,
        source: str,
        output_marker: str,
        label: str,
    ) -> None:
        marker_index = source.find(output_marker)
        if marker_index < 0:
            fail(f"{label} custom-command output is absent from {path}")
        command_start = source.rfind(
            "add_custom_command(", 0, marker_index
        )
        if command_start < 0:
            fail(f"{label} output is not owned by a custom command")
        following_commands = [
            index
            for marker in (
                "\nadd_custom_command(",
                "\n    add_custom_command(",
                "\nadd_custom_target(",
                "\n    add_custom_target(",
            )
            if (index := source.find(marker, marker_index + 1)) >= 0
        ]
        command_end = (
            min(following_commands) if following_commands else len(source)
        )
        command = source[command_start:command_end]
        if (
            len(
                re.findall(
                    r"\bJOB_POOL\s+sql_codegen_ast_proof\b",
                    command,
                )
            )
            != 1
        ):
            fail(
                f"{label} must use the depth-1 SQL codegen AST proof "
                "pool exactly once"
            )

    require_command_pool(
        safety_cmake_path,
        safety_cmake,
        'OUTPUT "${ARG_OUTPUT_PREPARE_STAMP}"',
        "PostgreSQL safety PREPARE",
    )
    require_command_pool(
        safety_cmake_path,
        safety_cmake,
        'OUTPUT\n'
        '            "${ARG_OUTPUT_COMPILE_GRAPH}"\n'
        '            "${ARG_OUTPUT_FACTS}"\n'
        '            "${ARG_OUTPUT_FACTS_STAMP}"',
        "PostgreSQL safety emit-facts",
    )
    if safety_cmake.count("JOB_POOL sql_codegen_ast_proof") != 2:
        fail(
            "PostgreSQL safety must place only PREPARE and emit-facts "
            "in the SQL codegen AST proof pool"
        )
    if bioseq_cmake.count("JOB_POOL sql_codegen_ast_proof") != 0:
        fail(
            "bioseq CMake retains a removed SQL codegen AST proof-pool "
            "command"
        )
    all_cmake = root_cmake + bioseq_cmake + safety_cmake
    if re.search(
        r"\b(?:JOB_POOL_COMPILE|CMAKE_JOB_POOL_COMPILE)\s+"
        r"sql_codegen_ast_proof\b",
        all_cmake,
    ):
        fail(
            "SQL codegen AST proof pool leaked into ordinary target "
            "compilation"
        )


def run_sql_codegen_ast_proof_pool_mutation_self_tests(
    root: Path,
) -> None:
    root_cmake = read_text(root / "CMakeLists.txt")
    bioseq_cmake = read_text(root / "bioseq/CMakeLists.txt")
    safety_cmake = read_text(
        root / "codegen/safety/postgres_error_safety.cmake"
    )
    verify_sql_codegen_ast_proof_pool_wiring(
        root,
        root_cmake=root_cmake,
        bioseq_cmake=bioseq_cmake,
        safety_cmake=safety_cmake,
    )

    def expect_rejected(
        label: str,
        *,
        mutated_root: str = root_cmake,
        mutated_bioseq: str = bioseq_cmake,
        mutated_safety: str = safety_cmake,
    ) -> None:
        try:
            verify_sql_codegen_ast_proof_pool_wiring(
                root,
                root_cmake=mutated_root,
                bioseq_cmake=mutated_bioseq,
                safety_cmake=mutated_safety,
            )
        except RuntimeError:
            return
        fail(f"SQL codegen AST proof-pool verifier accepted {label}")

    expect_rejected(
        "a depth-2 pool mutation",
        mutated_root=root_cmake.replace(
            "    sql_codegen_ast_proof=1)",
            "    sql_codegen_ast_proof=2)",
            1,
        ),
    )
    expect_rejected(
        "a safety PREPARE pool deletion",
        mutated_safety=safety_cmake.replace(
            '        DEPFILE "${ARG_OUTPUT_PREPARE_DEPFILE}"\n'
            "        JOB_POOL sql_codegen_ast_proof\n",
            '        DEPFILE "${ARG_OUTPUT_PREPARE_DEPFILE}"\n',
            1,
        ),
    )
    expect_rejected(
        "a safety emit-facts pool deletion",
        mutated_safety=safety_cmake.replace(
            "            ${ARG_SOURCES}\n"
            "        JOB_POOL sql_codegen_ast_proof\n"
            "        VERBATIM\n"
            '        COMMENT "Emitting current',
            "            ${ARG_SOURCES}\n"
            "        VERBATIM\n"
            '        COMMENT "Emitting current',
            1,
        ),
    )
    expect_rejected(
        "a bioseq AST proof-pool command restoration",
        mutated_bioseq=(
            bioseq_cmake
            + "\nadd_custom_command(\n"
            '  OUTPUT "${BIOSEQ_SQL_CODEGEN_DIR}/'
            'unexpected-ast-proof.stamp"\n'
            '  COMMAND "${CMAKE_COMMAND}" -E touch "<STAMP>"\n'
            "  JOB_POOL sql_codegen_ast_proof)\n"
        ),
    )
    expect_rejected(
        "ordinary extension compile-pool contamination",
        mutated_bioseq=(
            bioseq_cmake
            + "\nset_target_properties(postgresql_bioseq PROPERTIES\n"
            "  JOB_POOL_COMPILE sql_codegen_ast_proof)\n"
        ),
    )


def verify_fmgr_owner_authority_wiring(
    root: Path,
    bioseq_cmake: str,
) -> None:
    registry_path = root / "cmake/bioseq_sql_codegen.cmake"
    registry = read_text(registry_path)
    require_tokens(
        registry_path,
        registry,
        (
            "FMGR_OWNER_MODULES",
            "_natural_fmgr_entrypoints",
            "_resolved_fmgr_owner_records",
            "_owner_declared_modules",
            "_FMGR_OWNER_RECORDS",
            "synthetic SUPPORT FMGR facet",
            "multi-facet FMGR owner",
        ),
    )
    if re.search(
        r"foreach\(_fmgr_owner\s+IN LISTS ARG_FMGR_ENTRYPOINTS\)",
        registry,
    ):
        fail(
            "SQL registry still maps every FMGR output facet to the "
            "owner root module"
        )

    registration_blocks = re.findall(
        r"bioseq_register_sql_owner\(\s*(.*?)\)",
        bioseq_cmake,
        flags=re.DOTALL,
    )
    support_owned = {
        "vector_comparison_registry",
        "vector_aggregate_registry",
    }
    observed_by_profile: dict[str, dict[str, str]] = {
        "production": {},
        "test_indexes": {},
    }
    vector_facets: set[str] = set()
    vector_overrides: dict[str, str] = {}
    for block in registration_blocks:
        owner_match = re.search(r"\bOWNER_KEY\s+([a-z][a-z0-9_]*)", block)
        module_match = re.search(
            r"\bMODULE\s+([A-Za-z_][A-Za-z0-9_.:]*)", block
        )
        profiles_match = re.search(
            r"\bPROFILES\s+(.*?)\s+AUTHORITIES\b",
            block,
            flags=re.DOTALL,
        )
        facets_match = re.search(
            r"\bFMGR_ENTRYPOINTS\s+(.*?)(?:\s+FMGR_OWNER_MODULES\b|"
            r"\s+GUC_OWNERS\b|\s+ANNOTATION_FREE_RESULT_ROWS\b|"
            r"\s+HANDWRITTEN_FMGR_EXPORTS\b|$)",
            block,
            flags=re.DOTALL,
        )
        if facets_match is None:
            continue
        if owner_match is None or module_match is None or profiles_match is None:
            fail("FMGR owner registration lacks owner/module/profile authority")
        facets = re.findall(r"\b[a-z][a-z0-9_]*\b", facets_match.group(1))
        profiles = re.findall(
            r"\b(?:production|test_indexes)\b", profiles_match.group(1)
        )
        overrides_match = re.search(
            r"\bFMGR_OWNER_MODULES\s+(.*?)(?:\s+GUC_OWNERS\b|"
            r"\s+ANNOTATION_FREE_RESULT_ROWS\b|"
            r"\s+HANDWRITTEN_FMGR_EXPORTS\b|$)",
            block,
            flags=re.DOTALL,
        )
        overrides = {
            key: module
            for key, module in re.findall(
                r"\b([a-z][a-z0-9_]*)="
                r"([A-Za-z_][A-Za-z0-9_.:]*)\b",
                "" if overrides_match is None else overrides_match.group(1),
            )
        }
        natural_facets = [
            facet for facet in facets if facet not in support_owned
        ]
        if len(natural_facets) > 1 and set(overrides) != set(natural_facets):
            fail(
                f"multi-facet FMGR owner {owner_match.group(1)} lacks an "
                "exact per-facet source-module map"
            )
        for facet in natural_facets:
            module = overrides.get(facet, module_match.group(1))
            for profile in profiles:
                if facet in observed_by_profile[profile]:
                    fail(f"duplicate FMGR facet authority for {facet}")
                observed_by_profile[profile][facet] = module
        if owner_match.group(1) == "vector_types":
            vector_facets = set(facets)
            vector_overrides = overrides

    production_expected = {
        "mod": "bioseq.postgres.extension",
        "highway_target_test_support": "bioseq.highway_target_test_support",
        "storage_manager_sequence_storage_manager":
            "bioseq.postgres.storage.manager:sequence_storage",
        "table_access_hooks": "bioseq.postgres.table",
        "storage_load_pg": "bioseq.postgres.storage.io",
        "sequences_sample_table_udf":
            "bioseq.postgres.sequences.sample_table",
        "postgres_types_compact_sequence":
            "bioseq.postgres.types.compact_sequence",
        "vector_types_integer":
            "bioseq.postgres.vectorization.types.integer",
        "vector_types_date": "bioseq.postgres.vectorization.types.date",
        "vector_types_aggregates":
            "bioseq.postgres.vectorization.types.aggregates",
    }
    test_expected = dict(production_expected)
    test_expected["highway_ksw_target_test_support"] = (
        "bioseq.highway_ksw_target_test_support"
    )
    if observed_by_profile["production"] != production_expected:
        fail("production FMGR facet/source-module authority map drifted")
    if observed_by_profile["test_indexes"] != test_expected:
        fail("test_indexes FMGR facet/source-module authority map drifted")
    if not support_owned.issubset(vector_facets):
        fail("synthetic vector registries are absent from FMGR output facets")
    if support_owned.intersection(vector_overrides):
        fail("synthetic vector registries claim bogus source-module owners")

    require_tokens(
        root / "bioseq/CMakeLists.txt",
        bioseq_cmake,
        (
            "BIOSEQ_SQL_BIOSEQ_${_bioseq_sql_profile_key}_"
            "FMGR_OWNER_RECORDS",
            "foreach(_bioseq_sql_fmgr_owner_record IN LISTS",
            "--fmgr-owner-module \"${_bioseq_sql_fmgr_owner_record}\"",
            "${_bioseq_sql_fmgr_owner_args}",
        ),
    )


def verify_row_codec_owner_authority_wiring(
    root: Path,
    bioseq_cmake: str,
) -> None:
    registry_path = root / "cmake/bioseq_sql_codegen.cmake"
    registry = read_text(registry_path)
    require_tokens(
        registry_path,
        registry,
        (
            "_ROW_CODEC_OWNER_RECORDS",
            "_row_codec_owner_record",
            "row-codec owner ${ARG_OWNER_KEY} must register exactly one facet",
            "exactly name its source module",
        ),
    )

    observed_by_profile: dict[str, dict[str, str]] = {
        "production": {},
        "test_indexes": {},
    }
    registration_blocks = re.findall(
        r"bioseq_register_sql_owner\(\s*(.*?)\)",
        bioseq_cmake,
        flags=re.DOTALL,
    )
    for block in registration_blocks:
        facets_match = re.search(
            r"\bROW_CODECS\s+(.*?)(?:\s+ENTITY_HANDLES\b|"
            r"\s+FMGR_ENTRYPOINTS\b|\s+FMGR_OWNER_MODULES\b|"
            r"\s+GUC_OWNERS\b|\s+ANNOTATION_FREE_RESULT_ROWS\b|"
            r"\s+HANDWRITTEN_FMGR_EXPORTS\b|$)",
            block,
            flags=re.DOTALL,
        )
        if facets_match is None:
            continue
        module_match = re.search(
            r"\bMODULE\s+([A-Za-z_][A-Za-z0-9_.:]*)", block
        )
        profiles_match = re.search(
            r"\bPROFILES\s+(.*?)\s+AUTHORITIES\b",
            block,
            flags=re.DOTALL,
        )
        if module_match is None or profiles_match is None:
            fail("row-codec registration lacks module/profile authority")
        facets = re.findall(
            r"\b[a-z][a-z0-9_]*\b", facets_match.group(1)
        )
        if len(facets) != 1:
            fail("row-codec registration is not one facet per source module")
        for profile in re.findall(
            r"\b(?:production|test_indexes)\b", profiles_match.group(1)
        ):
            facet = facets[0]
            if facet in observed_by_profile[profile]:
                fail(f"duplicate row-codec facet authority for {facet}")
            observed_by_profile[profile][facet] = module_match.group(1)

    expected = {
        "storage_metadata": "bioseq.postgres.storage.metadata.sql_rows",
        "storage_indexing_pg":
            "bioseq.postgres.storage.indexing.sql_rows",
        "storage_load_pg": "bioseq.postgres.storage.io.sql_rows",
        "storage_manager": "bioseq.postgres.storage.manager.sql_rows",
        "table": "bioseq.postgres.table.sql_rows",
    }
    for profile, observed in observed_by_profile.items():
        if observed != expected:
            fail(
                f"{profile} row-codec facet/source-module authority map "
                "drifted"
            )

    cmake_path = root / "bioseq/CMakeLists.txt"
    require_tokens(
        cmake_path,
        bioseq_cmake,
        (
            "BIOSEQ_SQL_BIOSEQ_${_bioseq_sql_profile_key}_"
            "ROW_CODEC_OWNER_RECORDS",
            "foreach(_bioseq_sql_row_codec_owner_record IN LISTS",
            "--row-codec-owner-module",
            '"${_bioseq_sql_row_codec_owner_record}"',
            "${_bioseq_sql_row_codec_owner_args}",
        ),
    )
    if bioseq_cmake.count("${_bioseq_sql_row_codec_owner_args}") != 2:
        fail(
            "row-codec owner authority is not transported exactly once "
            "through SUPPORT and FINAL"
        )
    output_variables = {
        "storage_metadata": "STORAGE_METADATA",
        "storage_indexing_pg": "STORAGE_INDEXING_PG",
        "storage_load_pg": "STORAGE_LOAD_PG",
        "storage_manager": "STORAGE_MANAGER",
        "table": "TABLE",
    }
    for facet, variable_suffix in output_variables.items():
        configured_output = (
            f'"{facet}=${{BIOSEQ_SQL_${{_bioseq_sql_profile_key}}_'
            f'ROW_CODECS_{variable_suffix}}}"'
        )
        if bioseq_cmake.count(configured_output) != 2:
            fail(
                f"row-codec output {facet} is not configured exactly once "
                "in SUPPORT and FINAL"
            )

    index_path = root / "codegen/sql/index.cppm"
    index = read_text(index_path)
    require_tokens(
        index_path,
        index,
        (
            "--row-codec-owner-module",
            "validate_row_codec_owner_modules",
            "has no configured source-module owner",
            "has no configured output",
            "--row-codec-owner-module has invalid C++ module name "
            "'a::b'",
        ),
    )
    facets_path = root / "codegen/sql/renderer/cpp_facets.cppm"
    facets = read_text(facets_path)
    manifest_path = root / "codegen/sql/renderer/manifest.cppm"
    manifest = read_text(manifest_path)
    if "row_codec_owner_key" in facets:
        fail("row-codec renderer retains module-derived owner authority")
    if re.search(r"render_row_codec_modules\s*\(\s*graph\s*\)", facets + index + manifest):
        fail("row-codec renderer retains a one-argument fallback")
    require_tokens(
        manifest_path,
        manifest,
        (
            "row_codec_owners",
            "render_row_codec_modules(\n            graph, row_codec_owners)",
        ),
    )


def verify_cmake_wiring(root: Path) -> None:
    cmake_path = root / "bioseq/CMakeLists.txt"
    cmake = read_text(cmake_path)
    required = (
        "bioseq_sql_compile_contract_providers",
        "bioseq_sql_gcc_compile_contract_objects",
        "bioseq_sql_gcc_compile_contracts",
        "bioseq_sql_negative_contracts",
        "PUBLIC FILE_SET CXX_MODULES",
        "active_generated_guc_consumer_test.cpp",
        "--core-manifest",
        "--generated-root",
        "--allowed-core-generated-source",
        "--allowed-production-guc-generated-source",
        "--allowed-test-indexes-guc-generated-source",
        "if(BIOSEQ_SQL_RUN_NEGATIVE_CONTRACT_SELF_TEST)",
        '"$<TARGET_FILE:bioseq_codegen>" self_test sql',
        '    COMMAND "${CMAKE_COMMAND}" -E make_directory\n'
        '      "${BIOSEQ_SQL_CODEGEN_DIR}"\n'
        '    COMMAND "$<TARGET_FILE:bioseq_codegen>" self_test sql',
        "Skipping disabled maintainer SQL negative/provenance fixtures",
        "OBJECT EXCLUDE_FROM_ALL",
        "BIOSEQ_SQL_GCC_CONTRACT_SOURCE_INPUTS",
        '"${CMAKE_SOURCE_DIR}/CMakeLists.txt"',
        '"${CMAKE_CURRENT_LIST_DIR}/../codegen/CMakeLists.txt"',
        (
            '"${CMAKE_CURRENT_LIST_DIR}/../codegen/safety/'
            'postgres_error_safety.cmake"'
        ),
        '"${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_codegen.cmake"',
        '"${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_sql_codegen.cmake"',
        '"${CMAKE_CURRENT_LIST_DIR}/../cmake/bioseq_gcc_toolchain.cmake"',
        '"${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/collector/index.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/collector/options.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/tests/contract/index.cppm"',
        "BIOSEQ_SQL_GCC_CONTRACT_PROVENANCE_SOURCES",
        "${BIOSEQ_SQL_GCC_CONTRACT_SOURCE_INPUTS}",
        "list(REMOVE_ITEM SRC_MOD_FILES "
        "${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES})",
        "list(REMOVE_ITEM SRC_FILES "
        "${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES})",
    )
    require_tokens(cmake_path, cmake, required)
    verify_row_codec_owner_authority_wiring(root, cmake)
    verify_fmgr_owner_authority_wiring(root, cmake)
    verify_proof_vs_scan_wiring(cmake_path, cmake)
    verify_sql_codegen_ast_proof_pool_wiring(root)
    authored_provider_closure = re.search(
        r"set\(BIOSEQ_SQL_COMPILE_CONTRACT_AUTHORED_PROVIDERS\b"
        r"(.*?)\)\s*list\(REMOVE_ITEM\s+SRC_MOD_FILES",
        cmake,
        flags=re.DOTALL,
    )
    required_authored_providers = {
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/'
        'alignment_sql_rows.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/column/metadata/'
        'sql_rows.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/extension/sql_rows.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/datums.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/'
        'runtime_diagnostics.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/'
        'sql_catalog_entities.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/sql_boundary/'
        'aggregate.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/sql_boundary/'
        'guc_storage.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/sql_boundary/'
        'index.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/sql_boundary/'
        'row_codec.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/support/sql_boundary/'
        'vector.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/indexing/'
        'sql_rows.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/io/sql_rows.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage/manager/'
        'sql_rows.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/sequences/sample_table/'
        'sql_rows.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/table/sql_rows.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/vectorization/'
        'vector_types.cppm"',
    }
    observed_authored_providers = (
        ""
        if authored_provider_closure is None
        else authored_provider_closure.group(1)
    )
    missing_authored_providers = sorted(
        provider
        for provider in required_authored_providers
        if provider not in observed_authored_providers
    )
    if missing_authored_providers:
        fail(
            "SQL provider closure does not own every authored module "
            "needed by generated row contracts: "
            + ", ".join(missing_authored_providers)
        )
    if "BIOSEQ_SQL_NATIVE_OWNER_MODULES" in cmake:
        fail(
            "SQL native owner modules retain a second extension-owned "
            "source registry"
        )
    for source_variable, category_argument in (
        (
            "BIOSEQ_SQL_ENTITY_HANDLE_MODULES",
            "--allowed-core-generated-source",
        ),
        (
            "BIOSEQ_SQL_GUC_production_MODULES",
            "--allowed-production-guc-generated-source",
        ),
        (
            "BIOSEQ_SQL_GUC_test_indexes_MODULES",
            "--allowed-test-indexes-guc-generated-source",
        ),
    ):
        mapping = re.search(
            rf"foreach\([^)]*IN LISTS\s+{re.escape(source_variable)}\s*\)"
            rf"(.*?)endforeach\(\)",
            cmake,
            flags=re.DOTALL,
        )
        mapped_body = "" if mapping is None else mapping.group(1)
        category_arguments = {
            "--allowed-core-generated-source",
            "--allowed-production-guc-generated-source",
            "--allowed-test-indexes-guc-generated-source",
        }
        observed_arguments = {
            argument
            for argument in category_arguments
            if argument in mapped_body
        }
        if observed_arguments != {category_argument}:
            fail(
                f"CMake does not map {source_variable} exclusively to "
                f"{category_argument}"
            )
    production_begin = cmake.find(
        "bioseq_add_postgresql_extension(\n  postgresql_bioseq"
    )
    if production_begin < 0:
        fail("PostgreSQL extension target call is absent")
    production_call = cmake[production_begin:]
    production_call = production_call[:production_call.find("\n)")]
    if "bioseq_sql_compile_contract_providers" not in production_call:
        fail("PostgreSQL extension does not link the SQL provider closure")
    aligner_begin = cmake.find(
        "bioseq_add_postgresql_extension(\n  bioseq_aligner"
    )
    aligner_call = cmake[aligner_begin:production_begin]
    aligner_call = aligner_call[:aligner_call.find("\n)")]
    if "bioseq_sql_compile_contract_providers" in aligner_call:
        fail("non-PostgreSQL CLI target links the SQL provider closure")
    for fixture in (
        "catalog_entity_handle_producer_test.cppm",
        "generated_guc_producer_test.cppm",
        "sql_boundary_template_producer_test.cppm",
        "catalog_entity_handle_consumer_test.cpp",
        "generated_guc_consumer_test.cpp",
        "sql_boundary_template_consumer_test.cpp",
        "active_generated_guc_consumer_test.cpp",
        "result_materialization_test.cppm",
        "sql_entity_handles_test.cppm",
    ):
        if fixture in production_call:
            fail(f"production extension directly owns contract fixture {fixture}")

    codegen_cmake_path = root / "codegen/CMakeLists.txt"
    codegen_cmake = read_text(codegen_cmake_path)
    for path, text in ((cmake_path, cmake), (codegen_cmake_path, codegen_cmake)):
        if "-Wno-attributes=bioseq_sql::" not in text:
            fail(f"{path} lacks namespace-scoped GCC attribute suppression")
        if re.search(r"-Wno-attributes(?![=A-Za-z0-9_:-])", text):
            fail(f"{path} contains broad GCC -Wno-attributes")

    for target in (
        "bioseq_modules_nopg",
        "bioseq_sql_annotation_probe",
        "bioseq_sql_compile_contract_providers",
        "postgresql_bioseq",
    ):
        require_scoped_annotation_options(
            target,
            cmake_target_call(
                cmake, "target_compile_options", target
            ),
        )
    annotation_source_calls = re.findall(
        r"set_source_files_properties\(.*?\)",
        codegen_cmake,
        flags=re.DOTALL,
    )
    annotation_source_call = next(
        (
            call
            for call in annotation_source_calls
            if "sql/tests/contract/fixtures/annotation_probe.cppm" in call
            and "sql/tests/contract/fixtures/annotation_probe.cpp" in call
        ),
        None,
    )
    if annotation_source_call is None:
        fail("isolated codegen has no annotation-probe source property map")
    require_scoped_annotation_options(
        "isolated codegen annotation probes",
        annotation_source_call,
    )

    provider_begin = cmake.find(
        "set(BIOSEQ_SQL_COMPILE_CONTRACT_AUTHORED_PROVIDERS"
    )
    provider_end = cmake.find(")\n", provider_begin)
    if provider_begin < 0 or provider_end < 0:
        fail("SQL provider source registry is absent")
    provider_registry = cmake[provider_begin:provider_end]
    provider_sources = set(
        re.findall(
            r'"\$\{CMAKE_CURRENT_LIST_DIR\}/([^"]+)"',
            provider_registry,
        )
    )
    if not provider_sources:
        fail("SQL provider source registry is empty")
    for relative in provider_sources:
        if not relative.startswith("backend/pg/"):
            fail(
                "SQL provider registry duplicates a non-PostgreSQL module "
                f"owned by bioseq_modules_nopg: {relative}"
            )
        if not (root / "bioseq" / relative).is_file():
            fail(f"SQL provider registry names an absent source: {relative}")
    annotation_owners: dict[str, list[str]] = {
        "bioseq_modules_nopg": [],
        "bioseq_sql_compile_contract_providers": [],
        "postgresql_bioseq": [],
    }
    backend_root = root / "bioseq/backend"
    for source in sorted(backend_root.rglob("*")):
        if source.suffix not in {".cpp", ".cppm"}:
            continue
        if "[[bioseq_sql::" not in read_text(source):
            continue
        relative = source.relative_to(
            root / "bioseq"
        ).as_posix()
        if not relative.startswith("backend/pg/"):
            owner = "bioseq_modules_nopg"
        elif relative in provider_sources:
            owner = "bioseq_sql_compile_contract_providers"
        else:
            owner = "postgresql_bioseq"
        annotation_owners[owner].append(relative)
    required_annotation_owners = (
        "bioseq_sql_compile_contract_providers",
        "postgresql_bioseq",
    )
    if not all(
        annotation_owners[owner]
        for owner in required_annotation_owners
    ):
        fail(
            "annotation source ownership map did not cover provider and "
            "PostgreSQL targets"
        )

    root_cmake_path = root / "CMakeLists.txt"
    root_cmake = read_text(root_cmake_path)
    helper_path = root / "cmake/bioseq_gcc_toolchain.cmake"
    helper = read_text(helper_path)
    codegen_helper_path = root / "cmake/bioseq_codegen.cmake"
    codegen_helper = read_text(codegen_helper_path)
    codegen_cmake_path = root / "codegen/CMakeLists.txt"
    codegen_cmake = read_text(codegen_cmake_path)
    launcher_path = (
        root / "cmake/gcc_hwy_header_unit_launcher.py"
    )
    launcher = read_text(launcher_path)
    verify_gcc_toolchain_wiring_documents(
        root_cmake_path,
        root_cmake,
        helper_path,
        helper,
        codegen_helper_path,
        codegen_helper,
        codegen_cmake_path,
        codegen_cmake,
        launcher_path,
        launcher,
    )


def core_descriptor_hash(modules: list[dict[str, Any]]) -> str:
    rows = sorted(
        "\t".join(
            (
                str(module["owner"]),
                str(module["source_module"]),
                str(module["generated_module"]),
                str(module["profile"]),
                str(module["content_sha256"]),
            )
        )
        for module in modules
    )
    material = "bioseq_sql_core_descriptor\nrevision=1\n"
    material += "".join(row + "\n" for row in rows)
    return sha256_bytes(material.encode())


def core_entity_handle_logical_path(owner: str) -> str:
    return f"core/owners/{owner}.entity_handles.cppm"


def core_artifact_set_hash(modules: list[dict[str, Any]]) -> str:
    rows = sorted(
        "\t".join(
            (
                str(module["owner"]),
                str(module["source_module"]),
                str(module["generated_module"]),
                str(module["profile"]),
                str(module["configured_path"]),
                str(module["content_sha256"]),
            )
        )
        for module in modules
    )
    material = "bioseq_sql_core_artifact_set\nrevision=1\n"
    material += "".join(row + "\n" for row in rows)
    return sha256_bytes(material.encode())


def profile_core_records(manifest: dict[str, Any], label: str) -> list[dict[str, Any]]:
    unknown = sorted(set(manifest) - GENERATED_SQL_MANIFEST_FIELDS)
    if unknown:
        fail(f"{label} manifest has unknown fields: {unknown}")
    if (
        type(manifest.get("schema_revision")) is not int
        or manifest["schema_revision"] != 1
    ):
        fail(f"{label} manifest has an incompatible schema revision")
    if manifest.get("compiler_profile") != label:
        fail(f"{label} manifest has a mismatched compiler profile")
    require_sha256(
        manifest.get("compilation_signature"),
        f"{label} compilation signature",
    )
    if not isinstance(manifest.get("clang_version"), str):
        fail(f"{label} manifest has no compiler version")
    modules = manifest.get("entity_handle_modules")
    if not isinstance(modules, list) or not modules:
        fail(f"{label} manifest has no entity-handle modules")
    if manifest.get("generated_cpp_profile_invariant") is not True:
        fail(f"{label} manifest does not claim generated C++ invariance")
    identity_count = 0
    records: list[dict[str, Any]] = []
    seen_owners: set[str] = set()
    seen_modules: set[str] = set()
    for module in modules:
        if not isinstance(module, dict):
            fail(f"{label} manifest has malformed handle module")
        owner = module.get("owner_key")
        source_module = module.get("source_module")
        generated_module = module.get("module_name")
        cpp_namespace = module.get("cpp_namespace")
        for value, field in (
            (owner, "owner"),
            (source_module, "source module"),
            (generated_module, "generated module"),
            (cpp_namespace, "C++ namespace"),
        ):
            if not isinstance(value, str) or not value:
                fail(f"{label} CORE {field} is empty or malformed")
        if owner in seen_owners:
            fail(f"{label} CORE owner is duplicated: {owner}")
        if generated_module in seen_modules:
            fail(
                f"{label} CORE generated module is duplicated: "
                f"{generated_module}"
            )
        seen_owners.add(owner)
        seen_modules.add(generated_module)
        identities = module.get("entity_identities")
        if (
            not isinstance(identities, list)
            or not identities
            or not all(
                isinstance(identity, str) and identity
                for identity in identities
            )
        ):
            fail(f"{label} manifest has malformed handle identities")
        normalized_identities = sorted(identities)
        if len(set(normalized_identities)) != len(normalized_identities):
            fail(f"{label} CORE entity identities are duplicated")
        identity_count += len(identities)
        if module.get("profile_invariant") is not True:
            fail(f"{label} manifest contains a variant CORE module")
        records.append(
            {
                "owner": owner,
                "source_module": source_module,
                "generated_module": generated_module,
                "cpp_namespace": cpp_namespace,
                "entity_identities": normalized_identities,
            }
        )
    if manifest.get("entity_handle_count") != identity_count:
        fail(
            f"{label} entity_handle_count differs from identity cardinality"
        )
    if manifest.get("entity_handle_module_count") != len(records):
        fail(f"{label} entity_handle_module_count differs from records")
    return sorted(
        records,
        key=lambda record: (
            record["owner"],
            record["source_module"],
            record["generated_module"],
        ),
    )


def validate_guc_registration(
    registration: Any,
    profile: str,
    index: int,
) -> tuple[int, str, str, str, str, str]:
    label = f"{profile} GUC registration {index}"
    record = require_exact_object(
        registration, GUC_REGISTRATION_KEYS, label
    )
    live = require_exact_object(
        record["generated_live_authority"],
        GUC_GENERATED_LIVE_KEYS,
        f"{label}.generated_live_authority",
    )

    ordinal = record["registration_ordinal"]
    if type(ordinal) is not int or ordinal < 0:
        fail(f"{label}.registration_ordinal is not non-negative")

    strings = {
        key: require_nonempty_string(
            record, key, label
        )
        for key in (
            "name",
            "owner",
            "member",
            "kind",
            "cpp_storage",
            "storage_kind",
            "source_module",
            "source_path",
            "default_expression",
            "context",
            "feature_predicate",
        )
    }
    owner = strings["owner"]
    expected_prefix = (
        "bioseq." if owner == "bioseq" else f"bioseq.{owner}."
    )
    if (
        owner not in GUC_SOURCE_MODULES
        or strings["source_module"] != GUC_SOURCE_MODULES[owner]
        or strings["source_path"] != GUC_SOURCE_PATHS[owner]
        or not strings["name"].startswith(expected_prefix)
        or strings["name"] == expected_prefix
        or strings["member"]
        != strings["name"].removeprefix(expected_prefix).replace(".", "_")
    ):
        fail(f"{label} has inconsistent current owner/source provenance")
    expected_storage_kind = GUC_KIND_STORAGE_KINDS.get(
        strings["kind"]
    )
    if expected_storage_kind != strings["storage_kind"]:
        fail(f"{label} has an invalid kind/storage authority pair")

    minimum = record["minimum_expression"]
    maximum = record["maximum_expression"]
    bounded = strings["kind"] in {"integer", "real"}
    if (
        bounded
        and (
            not isinstance(minimum, str)
            or not minimum
            or not isinstance(maximum, str)
            or not maximum
        )
    ) or (not bounded and (minimum is not None or maximum is not None)):
        fail(f"{label} bound authority differs from its kind")
    unit = record["unit"]
    if unit is not None and (unit != "mb" or strings["kind"] != "integer"):
        fail(f"{label} unit authority differs from its kind")
    flags = require_string_array(
        record["flags"], f"{label}.flags"
    )
    if any(flag not in {"no_show_all", "not_in_sample"} for flag in flags):
        fail(f"{label}.flags contains an unsupported value")
    test_only = strings["name"] in GUC_TEST_ONLY_NAMES
    if (
        (test_only and profile != "test_indexes")
        or record["feature_predicate"]
        != (
            "defined(BIOSEQ_TEST_INDEXES)" if test_only else "true"
        )
    ):
        fail(f"{label} has mismatched profile provenance")

    hooks = record["hooks"]
    if not isinstance(hooks, list) or len(hooks) != 3:
        fail(f"{label}.hooks is not the three-hook array")
    for hook_index, hook in enumerate(hooks):
        if hook is not None and (not isinstance(hook, str) or not hook):
            fail(f"{label}.hooks[{hook_index}] is malformed")

    expected_context = {
        "sighup": "sighup",
        "superuser_set": "superuser",
        "user_set": "user",
    }.get(strings["context"])
    if expected_context is None:
        fail(f"{label}.context is not a current PostgreSQL context")
    expected_unit = "MB" if record["unit"] == "mb" else record["unit"]
    if (
        live["vartype"] != strings["kind"]
        or live["context"] != expected_context
        or live["unit"] != expected_unit
        or not isinstance(live["boot_value"], str)
        or live["reset_value"] != live["boot_value"]
        or live["current_value"] != live["boot_value"]
        or not isinstance(live["short_description"], str)
        or not live["short_description"]
        or (
            live["extra_description"] is not None
            and not isinstance(live["extra_description"], str)
        )
        or type(live["raw_flags"]) is not int
        or live["raw_flags"] < 0
        or type(live["visible_in_show_all"]) is not bool
        or type(live["included_in_sample"]) is not bool
    ):
        fail(f"{label}.generated_live_authority is malformed")
    live_minimum = live["minimum"]
    live_maximum = live["maximum"]
    bounded = strings["kind"] in {"integer", "real"}
    if (
        bounded
        and (
            not isinstance(live_minimum, str)
            or not isinstance(live_maximum, str)
        )
    ) or (
        not bounded
        and (live_minimum is not None or live_maximum is not None)
    ):
        fail(f"{label}.generated_live_authority bounds differ by kind")
    visible_enum_values = live["visible_enum_values"]
    if (
        not isinstance(visible_enum_values, list)
        or any(
            not isinstance(value, str) or not value
            for value in visible_enum_values
        )
        or len(set(visible_enum_values)) != len(visible_enum_values)
        or (
            strings["kind"] == "enum"
            and not visible_enum_values
        )
        or (
            strings["kind"] != "enum"
            and visible_enum_values
        )
    ):
        fail(f"{label}.generated_live_authority enum values differ")
    if (
        strings["kind"] == "enum"
        and live["boot_value"] not in visible_enum_values
    ) or (
        strings["kind"] == "bool"
        and live["boot_value"] not in {"on", "off"}
    ):
        fail(f"{label}.generated_live_authority boot value differs")
    expected_raw_flags = 0
    if "no_show_all" in record["flags"]:
        expected_raw_flags |= 0x000004
    if "not_in_sample" in record["flags"]:
        expected_raw_flags |= 0x000080
    expected_raw_flags |= {
        None: 0,
        "kb": 0x01000000,
        "blocks": 0x02000000,
        "mb": 0x04000000,
        "ms": 0x10000000,
        "s": 0x20000000,
        "min": 0x30000000,
    }.get(record["unit"], -1)
    if (
        live["raw_flags"] != expected_raw_flags
        or live["visible_in_show_all"]
        != ("no_show_all" not in record["flags"])
        or live["included_in_sample"]
        != ("not_in_sample" not in record["flags"])
    ):
        fail(f"{label}.generated_live_authority flag behavior differs")
    live_hooks = require_exact_object(
        live["hooks"],
        frozenset({"check", "assign", "show"}),
        f"{label}.generated_live_authority.hooks",
    )
    for hook_index, hook_name in enumerate(("check", "assign", "show")):
        live_hook = require_exact_object(
            live_hooks[hook_name],
            GUC_GENERATED_HOOK_KEYS,
            f"{label}.generated_live_authority.hooks.{hook_name}",
        )
        symbol = record["hooks"][hook_index]
        if (
            live_hook["symbol"] != symbol
            or live_hook["present"] is not (symbol is not None)
            or live_hook["behavior"]
            != (
                f"custom_{hook_name}"
                if symbol is not None
                else "none"
            )
        ):
            fail(
                f"{label}.generated_live_authority hook behavior differs"
            )

    enum_rows = record[
        "enum_rows_name_wire_condition_alias_hidden_compiled"
    ]
    if not isinstance(enum_rows, list):
        fail(f"{label} enum rows are not an array")
    enum_names: set[str] = set()
    visible_enum_names: list[str] = []
    for row_index, row in enumerate(enum_rows):
        row_label = f"{label}.enum_rows[{row_index}]"
        if not isinstance(row, list) or len(row) != 6:
            fail(f"{row_label} is not a six-field row")
        if (
            any(
                not isinstance(row[field], str) or not row[field]
                for field in (0, 1)
            )
            or not isinstance(row[2], str)
        ):
            fail(f"{row_label} has malformed string provenance")
        if row[3] is not None and (
            not isinstance(row[3], str) or not row[3]
        ):
            fail(f"{row_label}.alias_of is malformed")
        if (
            type(row[4]) is not bool
            or type(row[5]) is not bool
            or row[0] in enum_names
        ):
            fail(f"{row_label} has malformed hidden/compiled flags")
        enum_names.add(row[0])
        if row[5] and not row[4]:
            visible_enum_names.append(row[0])
    enum_default = record["enum_default"]
    require_optional_string(enum_default, f"{label}.enum_default")
    if (
        (strings["kind"] == "enum" and enum_default is None)
        or (strings["kind"] != "enum" and enum_default is not None)
        or (strings["kind"] == "enum" and not enum_rows)
        or (strings["kind"] != "enum" and enum_rows)
    ):
        fail(f"{label} enum authority differs from its kind")
    if strings["kind"] == "enum" and (
        enum_default not in visible_enum_names
        or visible_enum_values != visible_enum_names
    ):
        fail(f"{label} visible enum authority differs from its rows")

    source_sha256 = record["source_sha256"]
    if not isinstance(source_sha256, str):
        fail(f"{label}.source_sha256 is not a string")
    require_sha256(
        source_sha256,
        f"{label}.source_sha256",
    )
    return (
        ordinal,
        strings["name"],
        owner,
        strings["source_module"],
        strings["source_path"],
        source_sha256,
    )


def profile_guc_records(
    manifest: dict[str, Any],
    label: str,
    repository_root: Path,
) -> tuple[list[dict[str, Any]], set[str]]:
    generation = manifest.get("guc_generation")
    if not isinstance(generation, dict):
        fail(f"{label} manifest has no GUC generation record")
    if generation.get("profile") != label:
        fail(f"{label} GUC profile label differs")
    registrations = generation.get("registrations")
    owners = generation.get("owner_facets")
    if not isinstance(registrations, list) or not isinstance(owners, list):
        fail(f"{label} GUC inventory is malformed")
    if generation.get("registration_count") != len(registrations):
        fail(f"{label} GUC registration count differs from inventory")
    entities = manifest.get("entities")
    if not isinstance(entities, list):
        fail(f"{label} manifest has no entity inventory")
    guc_entities: dict[str, tuple[str, str, str]] = {}
    for index, entity in enumerate(entities):
        if not isinstance(entity, dict) or entity.get("kind") != "guc":
            continue
        stable_identity = entity.get("stable_identity")
        if (
            not isinstance(stable_identity, str)
            or not stable_identity.startswith("sql:guc:")
            or entity.get("identity") != stable_identity
        ):
            fail(f"{label} GUC entity {index} has a malformed identity")
        name = stable_identity.removeprefix("sql:guc:")
        source_module = entity.get("source_module")
        if not isinstance(source_module, str) or not source_module:
            fail(f"{label} GUC entity {name} has no source module")
        raw_source_sha256 = entity.get("source_sha256")
        if not isinstance(raw_source_sha256, str):
            fail(f"{label} GUC entity {name} source hash is not a string")
        source_sha256 = require_sha256(
            raw_source_sha256,
            f"{label} GUC entity {name} source hash",
        )
        source_path = entity.get("source_path")
        source = canonical_repository_source(
            repository_root,
            source_path,
            f"{label} GUC entity {name}",
        )
        if sha256_bytes(read_bytes(source)) != source_sha256:
            fail(
                f"{label} GUC entity {name} source hash differs from "
                "repository bytes"
            )
        if not name or name in guc_entities:
            fail(f"{label} GUC entity identities are duplicated")
        guc_entities[name] = (
            source_module,
            str(source_path),
            source_sha256,
        )
    try:
        owner_count = sum(
            (
                int(owner.get("registration_count", -1))
                if isinstance(owner, dict)
                else -1
            )
            for owner in owners
        )
    except (TypeError, ValueError):
        fail(f"{label} GUC owner registration count is malformed")
    if owner_count != len(registrations):
        fail(f"{label} GUC owner counts differ from registry inventory")
    records: list[dict[str, Any]] = []
    seen_modules: set[str] = set()
    seen_owners: set[str] = set()
    owner_source_modules: dict[str, str] = {}
    owner_registration_counts: dict[str, int] = {}
    for owner in owners:
        if not isinstance(owner, dict):
            fail(f"{label} GUC owner record is malformed")
        owner_key = owner.get("owner")
        if (
            not isinstance(owner_key, str)
            or not owner_key
            or owner_key in seen_owners
        ):
            fail(f"{label} GUC owner keys are empty or duplicated")
        seen_owners.add(owner_key)
        source_module = owner.get("source_module")
        if source_module != GUC_SOURCE_MODULES.get(owner_key):
            fail(f"{label} GUC owner source module is not canonical")
        registration_count = owner.get("registration_count")
        if type(registration_count) is not int or registration_count < 0:
            fail(f"{label} GUC owner registration count is malformed")
        owner_source_modules[owner_key] = source_module
        owner_registration_counts[owner_key] = registration_count
        module_name = owner.get("generated_module")
        if (
            not isinstance(module_name, str)
            or not module_name
            or module_name in seen_modules
        ):
            fail(f"{label} GUC owner modules are empty or duplicated")
        seen_modules.add(module_name)
        records.append(
            {
                "kind": "owner",
                "owner": owner_key,
                "module": module_name,
                "sha256": require_sha256(
                    owner.get("content_sha256"),
                    f"{label} GUC owner content hash",
                ),
            }
        )
    if seen_owners != set(GUC_SOURCE_MODULES):
        fail(f"{label} GUC owner set is not the exact current closure")
    registry_module = generation.get("registry_module")
    if (
        not isinstance(registry_module, str)
        or not registry_module
        or registry_module in seen_modules
    ):
        fail(f"{label} GUC registry module is empty or duplicated")
    records.append(
        {
            "kind": "registry",
            "owner": None,
            "module": registry_module,
            "sha256": require_sha256(
                generation.get("registry_content_sha256"),
                f"{label} GUC registry content hash",
            ),
        }
    )
    names: set[str] = set()
    ordinals: list[int] = []
    observed_owner_counts = {
        owner_key: 0 for owner_key in owner_source_modules
    }
    for index, registration in enumerate(registrations):
        (
            ordinal,
            name,
            registration_owner,
            registration_source_module,
            registration_source_path,
            registration_source_sha256,
        ) = validate_guc_registration(
            registration, label, index
        )
        if name in names:
            fail(f"{label} GUC names are duplicated or malformed")
        names.add(name)
        ordinals.append(ordinal)
        owner_key = registration_owner
        if owner_key not in GUC_SOURCE_MODULES:
            fail(f"{label} GUC owner provenance is malformed")
        if owner_source_modules.get(owner_key) != registration_source_module:
            fail(
                f"{label} GUC source module is not owned by "
                f"{owner_key}"
            )
        if guc_entities.get(name) != (
            registration_source_module,
            registration_source_path,
            registration_source_sha256,
        ):
            fail(
                f"{label} GUC source provenance does not join the "
                f"manifest entity for {name}"
            )
        observed_owner_counts[owner_key] += 1
    if len(names) != len(registrations):
        fail(f"{label} GUC names are duplicated or malformed")
    if ordinals != sorted(set(ordinals)):
        fail(f"{label} GUC registration ordinals are not unique and ordered")
    if observed_owner_counts != owner_registration_counts:
        fail(f"{label} GUC per-owner provenance counts differ")
    expected_owner_counts = dict(GUC_PRODUCTION_OWNER_COUNTS)
    if label == "test_indexes":
        expected_owner_counts["bioseq"] += len(GUC_TEST_ONLY_NAMES)
    if (
        len(registrations) != GUC_PROFILE_TOTALS[label]
        or owner_registration_counts != expected_owner_counts
    ):
        fail(f"{label} GUC cardinality differs from the current closure")
    if set(guc_entities) != names:
        fail(f"{label} GUC entity and registration inventories differ")
    return records, names


def verify_guc_profile_delta_validation(
    production: dict[str, Any],
    test_indexes: dict[str, Any],
) -> None:
    production_generation = production.get("guc_generation")
    test_generation = test_indexes.get("guc_generation")
    if not isinstance(production_generation, dict) or not isinstance(
        test_generation, dict
    ):
        fail("profile manifests have no GUC generation records")

    production_validation = require_exact_object(
        production_generation.get("profile_delta_validation"),
        GUC_PROFILE_DELTA_KEYS,
        "production GUC profile delta validation",
    )
    test_validation = require_exact_object(
        test_generation.get("profile_delta_validation"),
        GUC_PROFILE_DELTA_KEYS,
        "test_indexes GUC profile delta validation",
    )
    if production_validation != test_validation:
        fail("GUC profile delta validation differs between FINAL manifests")

    production_registry = require_sha256(
        production_generation.get("registry_content_sha256"),
        "production GUC registry content hash",
    )
    test_registry = require_sha256(
        test_generation.get("registry_content_sha256"),
        "test_indexes GUC registry content hash",
    )
    if require_sha256(
        production_validation["production_registry_content_sha256"],
        "profile delta production registry content hash",
    ) != production_registry:
        fail("profile delta production registry hash does not join FINAL")
    if require_sha256(
        production_validation["test_indexes_registry_content_sha256"],
        "profile delta test_indexes registry content hash",
    ) != test_registry:
        fail("profile delta test_indexes registry hash does not join FINAL")

    if require_string_array(
        production_validation["added_in_test_indexes"],
        "profile delta added registrations",
    ) != list(GUC_TEST_ONLY_NAMES):
        fail("profile delta added registrations differ from current settings")
    if require_string_array(
        production_validation["removed_from_test_indexes"],
        "profile delta removed registrations",
    ):
        fail("profile delta removed registrations is not empty")
    if require_string_array(
        production_validation["changed_in_test_indexes"],
        "profile delta changed registrations",
    ):
        fail("profile delta changed registrations is not empty")

    expected_owners = frozenset(
        {"aligner", "bioseq", "scheduler", "storage"}
    )

    def owner_hashes(
        generation: dict[str, Any],
        profile: str,
    ) -> dict[str, str]:
        facets = generation.get("owner_facets")
        if not isinstance(facets, list):
            fail(f"{profile} GUC owner facets are malformed")
        result: dict[str, str] = {}
        for index, facet in enumerate(facets):
            if not isinstance(facet, dict):
                fail(f"{profile} GUC owner facet {index} is malformed")
            owner = facet.get("owner")
            if (
                not isinstance(owner, str)
                or not owner
                or owner in result
            ):
                fail(f"{profile} GUC owner facets are duplicated")
            result[owner] = require_sha256(
                facet.get("content_sha256"),
                f"{profile} GUC owner {owner} content hash",
            )
        if set(result) != expected_owners:
            fail(
                f"{profile} GUC owner facet set is not the exact "
                "four-owner closure"
            )
        return result

    production_owner_hashes = owner_hashes(
        production_generation, "production"
    )
    test_owner_hashes = owner_hashes(
        test_generation, "test_indexes"
    )
    invariants = require_exact_object(
        production_validation["owner_descriptor_profile_invariant"],
        expected_owners,
        "profile delta owner invariance",
    )
    expected_invariants = {
        owner: owner != "bioseq" for owner in expected_owners
    }
    if (
        any(type(value) is not bool for value in invariants.values())
        or invariants != expected_invariants
    ):
        fail("profile delta owner invariance map is not exact")

    owner_artifacts = require_exact_object(
        production_validation["owner_artifacts"],
        expected_owners,
        "profile delta owner artifacts",
    )
    for owner in sorted(expected_owners):
        artifact = require_exact_object(
            owner_artifacts[owner],
            GUC_PROFILE_OWNER_ARTIFACT_KEYS,
            f"profile delta owner artifact {owner}",
        )
        if require_sha256(
            artifact["production_content_sha256"],
            f"profile delta production owner {owner} hash",
        ) != production_owner_hashes[owner]:
            fail(
                f"profile delta production owner hash does not join "
                f"{owner}"
            )
        if require_sha256(
            artifact["test_indexes_content_sha256"],
            f"profile delta test_indexes owner {owner} hash",
        ) != test_owner_hashes[owner]:
            fail(
                f"profile delta test_indexes owner hash does not join "
                f"{owner}"
            )

    def registrations_by_name(
        generation: dict[str, Any],
        profile: str,
    ) -> dict[str, dict[str, Any]]:
        registrations = generation.get("registrations")
        if not isinstance(registrations, list):
            fail(f"{profile} GUC registrations are malformed")
        result: dict[str, dict[str, Any]] = {}
        for index, registration in enumerate(registrations):
            record = require_exact_object(
                registration,
                GUC_REGISTRATION_KEYS,
                f"{profile} GUC registration {index}",
            )
            name = record.get("name")
            if (
                not isinstance(name, str)
                or not name
                or name in result
            ):
                fail(f"{profile} GUC registration names are duplicated")
            result[name] = record
        return result

    production_rows = registrations_by_name(
        production_generation, "production"
    )
    test_rows = registrations_by_name(
        test_generation, "test_indexes"
    )
    for name in sorted(set(production_rows) & set(test_rows)):
        production_row = dict(production_rows[name])
        test_row = dict(test_rows[name])
        production_row.pop("registration_ordinal", None)
        test_row.pop("registration_ordinal", None)
        if production_row != test_row:
            fail(
                "shared GUC registration differs "
                f"between profiles: {name}"
            )


def authorized_generated_category(
    sources: list[Path],
    canonical_root: Path,
    label: str,
    module_prefix: str,
    relative_prefix: tuple[str, ...],
) -> tuple[
    dict[Path, tuple[str, bytes]],
    dict[str, tuple[Path, bytes]],
]:
    if not sources:
        fail(f"{label} generated allowlist is empty")
    by_path: dict[Path, tuple[str, bytes]] = {}
    by_module: dict[str, tuple[Path, bytes]] = {}
    for source in sources:
        canonical = canonical_regular(source, canonical_root)
        relative = canonical.relative_to(canonical_root)
        if relative.parts[:len(relative_prefix)] != relative_prefix:
            fail(
                f"{label} generated artifact has the wrong canonical "
                f"category path: {canonical}"
            )
        if canonical in by_path:
            fail(f"{label} generated allowlist repeats {canonical}")
        content = read_bytes(canonical)
        module = exported_generated_module(
            canonical, content.decode("utf-8")
        )
        if not module.startswith(module_prefix):
            fail(
                f"{label} generated module has the wrong prefix: {module}"
            )
        if module in by_module:
            fail(f"{label} generated allowlist duplicates module {module}")
        by_path[canonical] = (module, content)
        by_module[module] = (canonical, content)
    return by_path, by_module


def verify_artifact_bundle(
    repository_root: Path,
    production_manifest_path: Path,
    test_indexes_manifest_path: Path,
    core_manifest_path: Path,
    core_seal_path: Path,
    generated_root: Path,
    allowed_core_sources: list[Path],
    allowed_production_guc_sources: list[Path],
    allowed_test_indexes_guc_sources: list[Path],
) -> None:
    canonical_repository_root = repository_root.resolve(strict=True)
    if not canonical_repository_root.is_dir():
        fail("configured repository root is not a directory")
    canonical_root = generated_root.resolve(strict=True)
    production_manifest_path = canonical_regular(
        production_manifest_path
    )
    test_indexes_manifest_path = canonical_regular(
        test_indexes_manifest_path
    )
    core_manifest_path = canonical_regular(core_manifest_path)
    core_seal_path = canonical_regular(core_seal_path)
    core_allowed, core_by_module = authorized_generated_category(
        allowed_core_sources,
        canonical_root,
        "CORE",
        "bioseq.postgres.sql.generated.core.",
        ("core", "owners"),
    )
    production_allowed, production_by_module = (
        authorized_generated_category(
            allowed_production_guc_sources,
            canonical_root,
            "production GUC",
            "bioseq.postgres.sql.generated.production.",
            ("gucs", "production"),
        )
    )
    test_allowed, test_by_module = authorized_generated_category(
        allowed_test_indexes_guc_sources,
        canonical_root,
        "test_indexes GUC",
        "bioseq.postgres.sql.generated.test_indexes.",
        ("gucs", "test_indexes"),
    )
    category_paths = (
        set(core_allowed),
        set(production_allowed),
        set(test_allowed),
    )
    if (
        category_paths[0] & category_paths[1]
        or category_paths[0] & category_paths[2]
        or category_paths[1] & category_paths[2]
    ):
        fail("generated category allowlists overlap")
    category_modules = (
        set(core_by_module),
        set(production_by_module),
        set(test_by_module),
    )
    if (
        category_modules[0] & category_modules[1]
        or category_modules[0] & category_modules[2]
        or category_modules[1] & category_modules[2]
    ):
        fail("generated module categories overlap")

    core_raw, core = load_json(core_manifest_path)
    _, seal = load_json(core_seal_path)
    if (
        core.get("schema") != "bioseq_sql_core_manifest"
        or type(core.get("schema_revision")) is not int
        or core["schema_revision"] != 1
    ):
        fail("CORE manifest does not use the current schema revision")
    if (
        seal.get("schema") != "bioseq_sql_core_seal"
        or type(seal.get("schema_revision")) is not int
        or seal["schema_revision"] != 1
    ):
        fail("CORE seal does not use the current schema revision")
    signature = require_sha256(
        core.get("compilation_signature"), "CORE compilation signature"
    )
    if seal.get("compilation_signature") != signature:
        fail("CORE compilation signature is not sealed")
    descriptor = require_sha256(
        core.get("core_descriptor_sha256"), "CORE descriptor"
    )
    if seal.get("core_descriptor_sha256") != descriptor:
        fail("CORE descriptor differs from its seal")
    if seal.get("core_manifest_sha256") != sha256_bytes(core_raw):
        fail("raw CORE manifest bytes differ from the sealed hash")
    if core.get("compiler_profile") != "production":
        fail("CORE manifest was not collected under production profile")
    if not isinstance(core.get("clang_version"), str):
        fail("CORE manifest has no compiler version")

    modules = core.get("modules")
    if not isinstance(modules, list) or not modules:
        fail("CORE manifest has no module records")
    try:
        module_count = int(str(core.get("module_count")))
    except (TypeError, ValueError):
        fail("CORE module_count is not an integer")
    if module_count != len(modules):
        fail("CORE module_count differs from records")
    seen_owners: set[str] = set()
    seen_modules: set[str] = set()
    core_paths: set[Path] = set()
    for module in modules:
        if not isinstance(module, dict):
            fail("CORE module record is malformed")
        owner = module.get("owner")
        source_module = module.get("source_module")
        module_name = module.get("generated_module")
        if (
            not isinstance(owner, str)
            or not owner
            or owner in seen_owners
        ):
            fail("CORE owner records are empty or duplicated")
        if (
            not isinstance(module_name, str)
            or not module_name
            or module_name in seen_modules
        ):
            fail("CORE generated modules are empty or duplicated")
        if not isinstance(source_module, str) or not source_module:
            fail("CORE source module is empty or malformed")
        expected_module = (
            f"bioseq.postgres.sql.generated.core.{owner}.entity_handles"
        )
        if module_name != expected_module:
            fail("CORE owner and generated-module identity differ")
        seen_owners.add(owner)
        seen_modules.add(module_name)
        if module.get("profile") != "invariant":
            fail("CORE module is not profile invariant")
        logical_path = module.get("configured_path")
        expected_logical_path = core_entity_handle_logical_path(owner)
        if logical_path != expected_logical_path:
            fail(
                "CORE configured path is not the stable owner-relative "
                "logical identity"
            )
        configured = canonical_regular(
            canonical_root
            / Path(expected_logical_path),
            canonical_root,
        )
        if configured not in core_allowed:
            fail(
                "CORE module is outside the explicit CMake allowlist: "
                f"{configured}"
            )
        allowed_module, content = core_allowed[configured]
        if allowed_module != module_name:
            fail("CORE record module differs from allowed file declaration")
        content_hash = require_sha256(
            module.get("content_sha256"), "CORE module content hash"
        )
        if sha256_bytes(content) != content_hash:
            fail(f"CORE module bytes were tampered: {configured}")
        core_paths.add(configured)
    if set(core_allowed) != core_paths:
        fail("CORE CMake allowlist has unrecorded or missing artifacts")
    if core_descriptor_hash(modules) != descriptor:
        fail("CORE descriptor cannot be recomputed from module records")
    artifact_set = require_sha256(
        core.get("core_artifact_set_sha256"), "CORE artifact-set hash"
    )
    if core_artifact_set_hash(modules) != artifact_set:
        fail("CORE artifact-set hash cannot be recomputed")

    _, production = load_json(production_manifest_path)
    _, test_indexes = load_json(test_indexes_manifest_path)
    production_core = profile_core_records(production, "production")
    test_core = profile_core_records(test_indexes, "test_indexes")
    if production_core != test_core:
        fail(
            "CORE owner/source/module/namespace/entity identities differ "
            "between profiles"
        )
    core_join = sorted(
        [
        {
            "owner": module["owner"],
            "source_module": module["source_module"],
            "generated_module": module["generated_module"],
        }
        for module in modules
        ],
        key=lambda record: (
            record["owner"],
            record["source_module"],
            record["generated_module"],
        ),
    )
    profile_core_join = [
        {
            "owner": record["owner"],
            "source_module": record["source_module"],
            "generated_module": record["generated_module"],
        }
        for record in production_core
    ]
    if profile_core_join != core_join:
        fail("CORE owner/source/module records do not join both profiles")

    production_gucs, production_names = profile_guc_records(
        production, "production", canonical_repository_root
    )
    test_gucs, test_names = profile_guc_records(
        test_indexes, "test_indexes", canonical_repository_root
    )
    verify_guc_profile_delta_validation(production, test_indexes)

    def verify_guc_category(
        label: str,
        records: list[dict[str, Any]],
        allowed_paths: dict[Path, tuple[str, bytes]],
        allowed_modules: dict[str, tuple[Path, bytes]],
    ) -> None:
        expected_paths: set[Path] = set()
        for record in records:
            if record["kind"] == "registry":
                expected_module = (
                    f"bioseq.postgres.sql.generated.{label}.guc_registry"
                )
                expected_path = (
                    canonical_root
                    / "gucs"
                    / label
                    / "guc_registry.cppm"
                )
            else:
                owner = record["owner"]
                expected_module = (
                    f"bioseq.postgres.sql.generated.{label}.{owner}.gucs"
                )
                expected_path = (
                    canonical_root
                    / "gucs"
                    / label
                    / "owners"
                    / f"{owner}.gucs.cppm"
                )
            module_name = record["module"]
            if module_name != expected_module:
                fail(
                    f"{label} GUC record has a mismatched owner/module "
                    f"identity: {module_name}"
                )
            found = allowed_modules.get(module_name)
            if found is None:
                fail(
                    f"{label} GUC module is absent from its category "
                    f"allowlist: {module_name}"
                )
            path, content = found
            if path != expected_path or path not in allowed_paths:
                fail(
                    f"{label} GUC module is outside its canonical profile "
                    f"path: {path}"
                )
            if sha256_bytes(content) != record["sha256"]:
                fail(f"{label} GUC manifest hash differs for {module_name}")
            expected_paths.add(path)
        if set(allowed_paths) != expected_paths:
            fail(
                f"{label} GUC CMake allowlist has unrecorded or missing "
                "artifacts"
            )

    verify_guc_category(
        "production",
        production_gucs,
        production_allowed,
        production_by_module,
    )
    verify_guc_category(
        "test_indexes",
        test_gucs,
        test_allowed,
        test_by_module,
    )
    if production_names - test_names:
        fail("test-index GUC profile removed production registrations")
    if test_names - production_names != set(GUC_TEST_ONLY_NAMES):
        fail("test-index GUC profile delta differs from current settings")


def render_json(value: dict[str, Any]) -> bytes:
    return (json.dumps(value, indent=2) + "\n").encode()


def run_tamper_self_test() -> None:
    with tempfile.TemporaryDirectory(
        prefix="bioseq-gcc-contract-tamper-"
    ) as directory:
        root = Path(directory).resolve()
        generated = root / "generated"
        generated.mkdir()
        guc_source_relatives = {
            owner: Path(source_path)
            for owner, source_path in GUC_SOURCE_PATHS.items()
        }
        guc_source_sha256: dict[str, str] = {}
        for owner, source_relative in guc_source_relatives.items():
            source = root / source_relative
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(
                f"export module {GUC_SOURCE_MODULES[owner]};\n",
                encoding="utf-8",
            )
            guc_source_sha256[owner] = sha256_bytes(source.read_bytes())
        guc_source = root / guc_source_relatives["bioseq"]
        guc_source_content = guc_source.read_text(encoding="utf-8")
        core_source = (
            generated / "core/owners/owner.entity_handles.cppm"
        )
        guc_owners = ("aligner", "bioseq", "scheduler", "storage")
        production_owner_paths = {
            owner: (
                generated
                / f"gucs/production/owners/{owner}.gucs.cppm"
            )
            for owner in guc_owners
        }
        production_owner = production_owner_paths["bioseq"]
        production_registry = (
            generated / "gucs/production/guc_registry.cppm"
        )
        test_owner_paths = {
            owner: (
                generated
                / f"gucs/test_indexes/owners/{owner}.gucs.cppm"
            )
            for owner in guc_owners
        }
        test_owner = test_owner_paths["bioseq"]
        test_registry = (
            generated / "gucs/test_indexes/guc_registry.cppm"
        )
        contents = {
            core_source: (
                "export module bioseq.postgres.sql.generated.core.owner."
                "entity_handles;\n"
            ),
            production_registry: (
                "export module bioseq.postgres.sql.generated.production.guc_registry;\n"
            ),
            test_registry: (
                "export module bioseq.postgres.sql.generated.test_indexes.guc_registry;\n"
            ),
        }
        for owner in guc_owners:
            contents[production_owner_paths[owner]] = (
                "export module "
                f"bioseq.postgres.sql.generated.production.{owner}.gucs;\n"
            )
            contents[test_owner_paths[owner]] = (
                "export module "
                f"bioseq.postgres.sql.generated.test_indexes.{owner}.gucs;\n"
            )
        for path, content in contents.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        core_modules = [
            {
                "owner": "owner",
                "source_module": "bioseq.owner",
                "generated_module": (
                    "bioseq.postgres.sql.generated.core.owner.entity_handles"
                ),
                "configured_path": core_entity_handle_logical_path(
                    "owner"
                ),
                "profile": "invariant",
                "content_sha256": sha256_bytes(
                    contents[core_source].encode()
                ),
            }
        ]
        signature = "1" * 64
        core_manifest = {
            "schema": "bioseq_sql_core_manifest",
            "schema_revision": 1,
            "compiler_profile": "production",
            "clang_version": "self-test",
            "compilation_signature": signature,
            "core_descriptor_sha256": core_descriptor_hash(core_modules),
            "core_artifact_set_sha256": core_artifact_set_hash(core_modules),
            "module_count": "1",
            "modules": core_modules,
        }
        core_path = root / "core-manifest.json"
        seal_path = root / "core-seal.json"
        production_path = root / "production.json"
        test_path = root / "test.json"
        core_raw = render_json(core_manifest)
        core_path.write_bytes(core_raw)
        seal_path.write_bytes(
            render_json(
                {
                    "schema": "bioseq_sql_core_seal",
                    "schema_revision": 1,
                    "core_descriptor_sha256":
                        core_manifest["core_descriptor_sha256"],
                    "compilation_signature": signature,
                    "core_manifest_sha256": sha256_bytes(core_raw),
                }
            )
        )

        def profile(
            label: str,
            owner_paths: dict[str, Path],
            registry_path: Path,
            settings: list[tuple[str, str]],
        ) -> dict[str, Any]:
            def registration(
                ordinal: int,
                owner: str,
                name: str,
            ) -> dict[str, Any]:
                group, separator, member = name.rpartition(".")
                if not separator or not group or not member:
                    fail("tamper self-test GUC identity is malformed")
                test_only = name in GUC_TEST_ONLY_NAMES
                enum_setting = (
                    name
                    == "bioseq.testing.persisted_index.failure_mode"
                )
                kind = "enum" if enum_setting else "bool"
                storage_kind = "int32_enum" if enum_setting else "bool"
                enum_values = (
                    ["none", "unsupported_vector_shape"]
                    if enum_setting
                    else []
                )
                boot_value = "none" if enum_setting else "off"
                hooks = [None, None, None]
                flags = (
                    ["no_show_all", "not_in_sample"] if test_only else []
                )
                return {
                    "registration_ordinal": ordinal,
                    "name": name,
                    "owner": owner,
                    "member": name.removeprefix(
                        "bioseq."
                        if owner == "bioseq"
                        else f"bioseq.{owner}."
                    ).replace(".", "_"),
                    "kind": kind,
                    "cpp_storage": f"fixture::{member}",
                    "storage_kind": storage_kind,
                    "source_module": GUC_SOURCE_MODULES[owner],
                    "source_path": guc_source_relatives[owner].as_posix(),
                    "source_sha256": guc_source_sha256[owner],
                    "default_expression": (
                        "0" if enum_setting else "false"
                    ),
                    "minimum_expression": None,
                    "maximum_expression": None,
                    "context": "superuser_set" if test_only else "user_set",
                    "flags": flags,
                    "unit": None,
                    "hooks": hooks,
                    "feature_predicate": (
                        "defined(BIOSEQ_TEST_INDEXES)"
                        if test_only
                        else "true"
                    ),
                    "enum_default": "none" if enum_setting else None,
                    (
                        "enum_rows_name_wire_condition_alias_hidden_"
                        "compiled"
                    ): (
                        [
                            [
                                "none",
                                "0",
                                "defined(BIOSEQ_TEST_INDEXES)",
                                None,
                                False,
                                True,
                            ],
                            [
                                "unsupported_vector_shape",
                                "1",
                                "defined(BIOSEQ_TEST_INDEXES)",
                                None,
                                False,
                                True,
                            ],
                        ]
                        if enum_setting
                        else []
                    ),
                    "generated_live_authority": {
                        "vartype": kind,
                        "context": "superuser" if test_only else "user",
                        "unit": None,
                        "boot_value": boot_value,
                        "reset_value": boot_value,
                        "current_value": boot_value,
                        "minimum": None,
                        "maximum": None,
                        "visible_enum_values": enum_values,
                        "short_description": "Fixture setting.",
                        "extra_description": None,
                        "raw_flags": 0x000084 if test_only else 0,
                        "visible_in_show_all": not test_only,
                        "included_in_sample": not test_only,
                        "hooks": {
                            hook_name: {
                                "symbol": hooks[hook_index],
                                "present": False,
                                "behavior": "none",
                            }
                            for hook_index, hook_name in enumerate(
                                ("check", "assign", "show")
                            )
                        },
                    },
                }

            return {
                "schema_revision": 1,
                "compiler_profile": label,
                "compilation_signature": (
                    "3" * 64 if label == "production" else "4" * 64
                ),
                "clang_version": "self-test",
                "generated_cpp_profile_invariant": True,
                "entity_handle_count": 1,
                "entity_handle_module_count": 1,
                "entity_handle_modules": [
                    {
                        "owner_key": "owner",
                        "source_module": "bioseq.owner",
                        "module_name": (
                            "bioseq.postgres.sql.generated.core.owner.entity_handles"
                        ),
                        "cpp_namespace": "bioseq::generated::owner",
                        "profile_invariant": True,
                        "entity_identities": ["sql:sequence:owner.seq"],
                    }
                ],
                "entities": [
                    {
                        "kind": "guc",
                        "identity": f"sql:guc:{name}",
                        "stable_identity": f"sql:guc:{name}",
                        "source_module": GUC_SOURCE_MODULES[owner],
                        "source_path": guc_source_relatives[owner].as_posix(),
                        "source_sha256": guc_source_sha256[owner],
                    }
                    for owner, name in settings
                ],
                "guc_generation": {
                    "profile": label,
                    "registration_count": len(settings),
                    "registry_module": exported_generated_module(
                        registry_path, contents[registry_path]
                    ),
                    "registry_content_sha256": sha256_bytes(
                        contents[registry_path].encode()
                    ),
                    "owner_facets": [
                        {
                            "owner": owner,
                            "source_module": GUC_SOURCE_MODULES[owner],
                            "generated_module": exported_generated_module(
                                owner_paths[owner],
                                contents[owner_paths[owner]],
                            ),
                            "content_sha256": sha256_bytes(
                                contents[owner_paths[owner]].encode()
                            ),
                            "registration_count": sum(
                                setting_owner == owner
                                for setting_owner, _ in settings
                            ),
                        }
                        for owner in guc_owners
                    ],
                    "registrations": [
                        registration(index, owner, name)
                        for index, (owner, name) in enumerate(settings)
                    ],
                },
            }

        production_settings = [
            ("bioseq", "bioseq.stable"),
            *(
                ("bioseq", f"bioseq.fixture_bioseq_{index:02d}")
                for index in range(25)
            ),
            *(
                (
                    "scheduler",
                    f"bioseq.scheduler.fixture_scheduler_{index:02d}",
                )
                for index in range(3)
            ),
            ("storage", "bioseq.storage.fixture_storage_00"),
            *(
                (
                    "aligner",
                    f"bioseq.aligner.fixture_aligner_{index:02d}",
                )
                for index in range(43)
            ),
        ]
        test_settings = [
            *production_settings,
            *(("bioseq", name) for name in GUC_TEST_ONLY_NAMES),
        ]
        production_manifest = profile(
            "production",
            production_owner_paths,
            production_registry,
            production_settings,
        )
        test_manifest = profile(
            "test_indexes",
            test_owner_paths,
            test_registry,
            test_settings,
        )

        def facet_hashes(
            manifest: dict[str, Any],
        ) -> dict[str, str]:
            return {
                str(facet["owner"]): str(facet["content_sha256"])
                for facet in manifest["guc_generation"]["owner_facets"]
            }

        production_hashes = facet_hashes(production_manifest)
        test_hashes = facet_hashes(test_manifest)
        profile_delta_validation = {
            "production_registry_content_sha256": (
                production_manifest["guc_generation"][
                    "registry_content_sha256"
                ]
            ),
            "test_indexes_registry_content_sha256": (
                test_manifest["guc_generation"][
                    "registry_content_sha256"
                ]
            ),
            "added_in_test_indexes": list(GUC_TEST_ONLY_NAMES),
            "removed_from_test_indexes": [],
            "changed_in_test_indexes": [],
            "owner_descriptor_profile_invariant": {
                owner: owner != "bioseq" for owner in guc_owners
            },
            "owner_artifacts": {
                owner: {
                    "production_content_sha256": (
                        production_hashes[owner]
                    ),
                    "test_indexes_content_sha256": test_hashes[owner],
                }
                for owner in guc_owners
            },
        }
        production_manifest["guc_generation"][
            "profile_delta_validation"
        ] = copy.deepcopy(profile_delta_validation)
        test_manifest["guc_generation"]["profile_delta_validation"] = (
            copy.deepcopy(profile_delta_validation)
        )
        production_path.write_bytes(render_json(production_manifest))
        test_path.write_bytes(render_json(test_manifest))
        core_allowlist = [core_source]
        production_guc_allowlist = [
            *production_owner_paths.values(),
            production_registry,
        ]
        test_guc_allowlist = [
            *test_owner_paths.values(),
            test_registry,
        ]

        def verify() -> None:
            verify_artifact_bundle(
                root,
                production_path,
                test_path,
                core_path,
                seal_path,
                generated,
                core_allowlist,
                production_guc_allowlist,
                test_guc_allowlist,
            )

        verify()

        def expect_failure(operation: Any, label: str) -> None:
            try:
                operation()
            except RuntimeError:
                return
            fail(f"tamper self-test accepted mutation: {label}")

        hardlink = root / "hard-linked-core.cppm"
        hardlink.hardlink_to(core_source)
        expect_failure(verify, "hard-linked generated module")
        hardlink.unlink()

        core_source.write_text(
            contents[core_source] + "// tampered\n", encoding="utf-8"
        )
        expect_failure(verify, "module bytes")
        core_source.write_text(contents[core_source], encoding="utf-8")

        original_core_raw = core_path.read_bytes()
        original_seal_raw = seal_path.read_bytes()
        core_path.write_bytes(original_core_raw + b" ")
        expect_failure(verify, "raw manifest bytes")
        core_path.write_bytes(original_core_raw)

        mutated_core = copy.deepcopy(core_manifest)
        mutated_core["core_descriptor_sha256"] = "2" * 64
        mutated_raw = render_json(mutated_core)
        core_path.write_bytes(mutated_raw)
        mutated_seal = {
            "schema": "bioseq_sql_core_seal",
            "schema_revision": 1,
            "core_descriptor_sha256": "2" * 64,
            "compilation_signature": signature,
            "core_manifest_sha256": sha256_bytes(mutated_raw),
        }
        seal_path.write_bytes(render_json(mutated_seal))
        expect_failure(verify, "descriptor")
        core_path.write_bytes(original_core_raw)
        seal_path.write_bytes(original_seal_raw)

        for configured_path, label in (
            (
                core_source.as_posix(),
                "absolute CORE configured path",
            ),
            (
                "core/owners/../owners/owner.entity_handles.cppm",
                "non-normal CORE configured path",
            ),
            (
                "core/owners/other.entity_handles.cppm",
                "wrong-owner CORE configured path",
            ),
        ):
            path_mutation = copy.deepcopy(core_manifest)
            path_mutation["modules"][0][
                "configured_path"
            ] = configured_path
            path_mutation["core_artifact_set_sha256"] = (
                core_artifact_set_hash(path_mutation["modules"])
            )
            path_mutation_raw = render_json(path_mutation)
            core_path.write_bytes(path_mutation_raw)
            seal_path.write_bytes(
                render_json(
                    {
                        "schema": "bioseq_sql_core_seal",
                        "schema_revision": 1,
                        "core_descriptor_sha256": path_mutation[
                            "core_descriptor_sha256"
                        ],
                        "compilation_signature": signature,
                        "core_manifest_sha256": sha256_bytes(
                            path_mutation_raw
                        ),
                    }
                )
            )
            expect_failure(verify, label)
        core_path.write_bytes(original_core_raw)
        seal_path.write_bytes(original_seal_raw)

        core_allowlist.remove(core_source)
        expect_failure(verify, "allowlist removal")
        core_allowlist.append(core_source)

        mutated_profile = copy.deepcopy(production_manifest)
        mutated_profile["entity_handle_count"] = 2
        production_path.write_bytes(render_json(mutated_profile))
        expect_failure(verify, "profile identity count")
        production_path.write_bytes(render_json(production_manifest))

        mismatched_entity_identity = copy.deepcopy(production_manifest)
        mismatched_entity_identity["entities"][0]["identity"] = (
            "sql:guc:bioseq.other"
        )
        production_path.write_bytes(render_json(mismatched_entity_identity))
        expect_failure(verify, "mismatched GUC entity identity")
        production_path.write_bytes(render_json(production_manifest))

        absolute_source_path = copy.deepcopy(production_manifest)
        absolute_source_path["entities"][0]["source_path"] = (
            guc_source.as_posix()
        )
        production_path.write_bytes(render_json(absolute_source_path))
        expect_failure(verify, "absolute GUC entity source path")
        production_path.write_bytes(render_json(production_manifest))

        noncanonical_source_path = copy.deepcopy(production_manifest)
        noncanonical_source_path["entities"][0]["source_path"] = (
            "bioseq/backend/pg/configuration/../configuration/core.cppm"
        )
        production_path.write_bytes(render_json(noncanonical_source_path))
        expect_failure(verify, "noncanonical GUC entity source path")
        production_path.write_bytes(render_json(production_manifest))

        guc_source.write_text(
            guc_source_content + "// tampered\n",
            encoding="utf-8",
        )
        expect_failure(verify, "GUC repository source bytes")
        guc_source.write_text(guc_source_content, encoding="utf-8")

        flat_registration = copy.deepcopy(production_manifest)
        flat_registration["guc_generation"]["registrations"][0] = {
            "ordinal": 0,
            "name": "bioseq.stable",
        }
        production_path.write_bytes(render_json(flat_registration))
        expect_failure(verify, "truncated GUC registration schema")
        production_path.write_bytes(render_json(production_manifest))

        malformed_registration = copy.deepcopy(production_manifest)
        malformed_registration["guc_generation"]["registrations"][0][
            "unexpected_provenance"
        ] = "rejected"
        production_path.write_bytes(render_json(malformed_registration))
        expect_failure(verify, "unknown GUC registration field")
        production_path.write_bytes(render_json(production_manifest))

        malformed_source_hash = copy.deepcopy(production_manifest)
        malformed_source_hash["guc_generation"]["registrations"][0][
            "source_sha256"
        ] = "not-a-sha256"
        production_path.write_bytes(render_json(malformed_source_hash))
        expect_failure(verify, "malformed GUC source provenance")
        production_path.write_bytes(render_json(production_manifest))

        mismatched_live_flags = copy.deepcopy(production_manifest)
        mismatched_live_flags["guc_generation"]["registrations"][0][
            "generated_live_authority"
        ]["raw_flags"] = 1
        production_path.write_bytes(render_json(mismatched_live_flags))
        expect_failure(verify, "mismatched generated-live GUC flags")
        production_path.write_bytes(render_json(production_manifest))

        mismatched_source_hash = copy.deepcopy(production_manifest)
        mismatched_source_hash["guc_generation"]["registrations"][0][
            "source_sha256"
        ] = "6" * 64
        production_path.write_bytes(render_json(mismatched_source_hash))
        expect_failure(verify, "GUC entity source hash mismatch")
        production_path.write_bytes(render_json(production_manifest))

        resealed_source_hash = copy.deepcopy(production_manifest)
        resealed_source_hash["entities"][0]["source_sha256"] = "6" * 64
        resealed_source_hash["guc_generation"]["registrations"][0][
            "source_sha256"
        ] = "6" * 64
        production_path.write_bytes(render_json(resealed_source_hash))
        expect_failure(verify, "GUC source hash differs from repository bytes")
        production_path.write_bytes(render_json(production_manifest))

        mismatched_owner = copy.deepcopy(production_manifest)
        mismatched_owner["guc_generation"]["registrations"][0][
            "owner"
        ] = "other"
        production_path.write_bytes(render_json(mismatched_owner))
        expect_failure(verify, "mismatched GUC owner provenance")
        production_path.write_bytes(render_json(production_manifest))

        missing_validation = copy.deepcopy(production_manifest)
        del missing_validation["guc_generation"][
            "profile_delta_validation"
        ]
        production_path.write_bytes(render_json(missing_validation))
        expect_failure(verify, "missing GUC profile delta validation")
        production_path.write_bytes(render_json(production_manifest))

        swapped_registry_validation = copy.deepcopy(production_manifest)
        swapped_registry_validation["guc_generation"][
            "profile_delta_validation"
        ]["production_registry_content_sha256"] = "9" * 64
        production_path.write_bytes(
            render_json(swapped_registry_validation)
        )
        expect_failure(verify, "swapped profile registry hash")
        production_path.write_bytes(render_json(production_manifest))

        swapped_owner_validation = copy.deepcopy(production_manifest)
        swapped_owner_validation["guc_generation"][
            "profile_delta_validation"
        ]["owner_artifacts"]["bioseq"][
            "production_content_sha256"
        ] = "8" * 64
        production_path.write_bytes(
            render_json(swapped_owner_validation)
        )
        expect_failure(verify, "swapped profile owner hash")
        production_path.write_bytes(render_json(production_manifest))

        integer_invariance = copy.deepcopy(production_manifest)
        integer_invariance["guc_generation"]["profile_delta_validation"][
            "owner_descriptor_profile_invariant"
        ]["aligner"] = 1
        production_path.write_bytes(render_json(integer_invariance))
        expect_failure(verify, "non-boolean owner invariance")
        production_path.write_bytes(render_json(production_manifest))

        shared_row_drift = copy.deepcopy(test_manifest)
        shared_row_drift["guc_generation"]["registrations"][0][
            "default_expression"
        ] = "true"
        test_path.write_bytes(render_json(shared_row_drift))
        expect_failure(verify, "shared GUC registration drift")
        test_path.write_bytes(render_json(test_manifest))

        cross_profile = copy.deepcopy(production_manifest)
        cross_profile_owner = (
            cross_profile["guc_generation"]["owner_facets"][0]
        )
        test_owner_record = (
            test_manifest["guc_generation"]["owner_facets"][0]
        )
        cross_profile_owner["generated_module"] = (
            test_owner_record["generated_module"]
        )
        cross_profile_owner["content_sha256"] = (
            test_owner_record["content_sha256"]
        )
        production_path.write_bytes(render_json(cross_profile))
        expect_failure(verify, "cross-profile GUC module swap")
        production_path.write_bytes(render_json(production_manifest))

        identity_drift = copy.deepcopy(test_manifest)
        identity_drift["entity_handle_modules"][0][
            "entity_identities"
        ][0] = "sql:sequence:owner.same_count_drift"
        test_path.write_bytes(render_json(identity_drift))
        expect_failure(verify, "same-count CORE identity drift")
        test_path.write_bytes(render_json(test_manifest))


def run_fake_gcc_toolchain_self_tests(root: Path) -> None:
    cmake = shutil.which("cmake")
    if cmake is None:
        fail("fake GCC toolchain self-test requires cmake")
    helper = root / "cmake/bioseq_gcc_toolchain.cmake"
    launcher = root / "cmake/gcc_hwy_header_unit_launcher.py"
    with tempfile.TemporaryDirectory(
        prefix="bioseq-gcc-identity-"
    ) as directory:
        temporary = Path(directory).resolve()
        prefix = temporary / "gcc"
        binary = prefix / "bin"
        inventory_directory = (
            prefix / "lib/gcc/x86_64-test/16.1.0"
        )
        binary.mkdir(parents=True)
        inventory_directory.mkdir(parents=True)
        gcc = binary / "gcc-16"
        gxx = binary / "g++-16"
        inventory = inventory_directory / "libstdc++.modules.json"
        std_source = inventory_directory / "std.cc"
        compat_source = inventory_directory / "std.compat.cc"
        outside_source = temporary / "outside.cc"
        std_source.write_text(
            "export module std;\n",
            encoding="utf-8",
        )
        compat_source.write_text(
            "export module std.compat;\n",
            encoding="utf-8",
        )
        outside_source.write_text(
            "export module escaped;\n",
            encoding="utf-8",
        )

        def compiler_script(version: str, modules: Path) -> str:
            return (
                "#!/bin/sh\n"
                'case "$1" in\n'
                "  -dumpfullversion|-dumpversion) "
                f"printf '%s\\n' '{version}' ;;\n"
                "  -print-file-name=libstdc++.modules.json) "
                f"printf '%s\\n' '{modules.as_posix()}' ;;\n"
                f"  --version) printf '%s\\n' 'g++ {version}' ;;\n"
                "  *) exit 2 ;;\n"
                "esac\n"
            )

        def write_compilers(
            c_version: str = "16.1.0",
            cxx_version: str = "16.1.0",
        ) -> tuple[str, str]:
            gcc_text = compiler_script(c_version, inventory)
            gxx_text = compiler_script(cxx_version, inventory)
            gcc.write_text(gcc_text, encoding="utf-8")
            gxx.write_text(gxx_text, encoding="utf-8")
            gcc.chmod(0o755)
            gxx.chmod(0o755)
            return gcc_text, gxx_text

        def inventory_document(
            std_path: str = "std.cc",
            compat_path: str = "std.compat.cc",
        ) -> bytes:
            return (
                json.dumps(
                    {
                        "version": 1,
                        "modules": [
                            {
                                "logical-name": "std",
                                "source-path": std_path,
                                "is-std-library": True,
                            },
                            {
                                "logical-name": "std.compat",
                                "source-path": compat_path,
                                "is-std-library": True,
                            },
                        ],
                    },
                    indent=2,
                )
                + "\n"
            ).encode()

        positive_inventory = inventory_document()
        gcc_text, gxx_text = write_compilers()
        inventory.write_bytes(positive_inventory)
        seal = temporary / "seal.cmake"
        selection_script = temporary / "select.cmake"
        selection_script.write_text(
            """
cmake_minimum_required(VERSION 3.20)
include("${HELPER}")
bioseq_select_gcc16_toolchain(
  REQUESTED_ROOT "${PREFIX}"
  REQUESTED_MODULES "${MODULES}"
  OUTPUT_ROOT SELECTED_ROOT
  OUTPUT_C SELECTED_C
  OUTPUT_CXX SELECTED_CXX
  OUTPUT_MODULES SELECTED_MODULES
  OUTPUT_VERSION SELECTED_VERSION
  OUTPUT_C_SHA256 SELECTED_C_SHA256
  OUTPUT_CXX_SHA256 SELECTED_CXX_SHA256
  OUTPUT_MODULES_SHA256 SELECTED_MODULES_SHA256
  OUTPUT_MODULE_SOURCE_CLOSURE_SHA256 SELECTED_CLOSURE_SHA256)
file(WRITE "${SEAL}"
  "set(EXPECTED_ROOT \\"${SELECTED_ROOT}\\")\\n"
  "set(EXPECTED_C \\"${SELECTED_C}\\")\\n"
  "set(EXPECTED_CXX \\"${SELECTED_CXX}\\")\\n"
  "set(EXPECTED_MODULES \\"${SELECTED_MODULES}\\")\\n"
  "set(EXPECTED_VERSION \\"${SELECTED_VERSION}\\")\\n"
  "set(EXPECTED_C_SHA256 \\"${SELECTED_C_SHA256}\\")\\n"
  "set(EXPECTED_CXX_SHA256 \\"${SELECTED_CXX_SHA256}\\")\\n"
  "set(EXPECTED_MODULES_SHA256 \\"${SELECTED_MODULES_SHA256}\\")\\n"
  "set(EXPECTED_CLOSURE_SHA256 \\"${SELECTED_CLOSURE_SHA256}\\")\\n")
""".strip()
            + "\n",
            encoding="utf-8",
        )
        verification_script = temporary / "verify.cmake"
        verification_script.write_text(
            """
cmake_minimum_required(VERSION 3.20)
include("${HELPER}")
include("${SEAL}")
bioseq_verify_gcc16_toolchain_identity(
  ROOT "${EXPECTED_ROOT}"
  C "${EXPECTED_C}"
  CXX "${EXPECTED_CXX}"
  MODULES "${EXPECTED_MODULES}"
  VERSION "${EXPECTED_VERSION}"
  C_SHA256 "${EXPECTED_C_SHA256}"
  CXX_SHA256 "${EXPECTED_CXX_SHA256}"
  MODULES_SHA256 "${EXPECTED_MODULES_SHA256}"
  MODULE_SOURCE_CLOSURE_SHA256 "${EXPECTED_CLOSURE_SHA256}")
""".strip()
            + "\n",
            encoding="utf-8",
        )

        def run_cmake(
            script: Path,
            *,
            expect_success: bool,
            expected_diagnostic: str = "",
        ) -> None:
            result = subprocess.run(
                [
                    cmake,
                    f"-DHELPER={helper}",
                    f"-DPREFIX={prefix}",
                    f"-DMODULES={inventory}",
                    f"-DSEAL={seal}",
                    "-P",
                    str(script),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=False,
            )
            combined = result.stdout + result.stderr
            if expect_success and result.returncode != 0:
                fail(
                    "fake GCC toolchain positive self-test failed: "
                    + combined
                )
            if not expect_success:
                if result.returncode == 0:
                    fail(
                        "fake GCC toolchain accepted mutation: "
                        + expected_diagnostic
                    )
                if (
                    expected_diagnostic
                    and expected_diagnostic not in combined
                ):
                    fail(
                        "fake GCC toolchain mutation produced the wrong "
                        f"diagnostic; expected {expected_diagnostic!r}: "
                        + combined
                    )

        run_cmake(selection_script, expect_success=True)
        run_cmake(verification_script, expect_success=True)

        gxx.write_text(gxx_text + "# same-path replacement\n", encoding="utf-8")
        run_cmake(
            verification_script,
            expect_success=False,
            expected_diagnostic="C++ compiler SHA-256 changed",
        )
        gxx.write_text(gxx_text, encoding="utf-8")
        gxx.chmod(0o755)

        inventory.write_bytes(positive_inventory + b" ")
        run_cmake(
            verification_script,
            expect_success=False,
            expected_diagnostic="module inventory SHA-256 changed",
        )
        inventory.write_bytes(positive_inventory)

        original_std = std_source.read_bytes()
        std_source.write_bytes(original_std + b"// replacement\n")
        run_cmake(
            verification_script,
            expect_success=False,
            expected_diagnostic="module source closure SHA-256 changed",
        )
        std_source.write_bytes(original_std)

        write_compilers("16.0.0", "16.0.0")
        run_cmake(
            selection_script,
            expect_success=False,
            expected_diagnostic="requires g++ >= 16.1",
        )
        write_compilers("16.1.0", "16.2.0")
        run_cmake(
            selection_script,
            expect_success=False,
            expected_diagnostic="version-mismatched gcc/g++ pair",
        )
        write_compilers()
        inventory.write_bytes(
            inventory_document(outside_source.as_posix())
        )
        run_cmake(
            selection_script,
            expect_success=False,
            expected_diagnostic="not an authenticated",
        )
        inventory.write_bytes(positive_inventory)

        metadata_sha256 = sha256_bytes(positive_inventory)
        source_rows = sorted(
            (
                f"std\t{std_source.resolve().as_posix()}\t"
                f"{sha256_bytes(std_source.read_bytes())}",
                f"std.compat\t{compat_source.resolve().as_posix()}\t"
                f"{sha256_bytes(compat_source.read_bytes())}",
            )
        )
        closure_material = (
            "bioseq_gcc_module_source_closure\n"
            "revision=1\n"
            f"{metadata_sha256}\n"
            + "".join(f"{row}\n" for row in source_rows)
        )
        closure_sha256 = sha256_bytes(closure_material.encode())

        def run_launcher(
            *,
            expect_success: bool,
            expected_diagnostic: str = "",
        ) -> None:
            result = subprocess.run(
                [
                    sys.executable,
                    str(launcher),
                    "--bioseq-expected-compiler-sha256="
                    + sha256_bytes(gxx_text.encode()),
                    f"--bioseq-gcc-root={prefix}",
                    f"--bioseq-gcc-module-inventory={inventory}",
                    "--bioseq-expected-module-inventory-sha256="
                    + metadata_sha256,
                    "--bioseq-expected-module-source-closure-sha256="
                    + closure_sha256,
                    str(gxx),
                    "-dumpfullversion",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=False,
            )
            combined = result.stdout + result.stderr
            if expect_success and result.returncode != 0:
                fail("GCC launcher positive self-test failed: " + combined)
            if not expect_success:
                if result.returncode == 0:
                    fail(
                        "GCC launcher accepted identity mutation: "
                        + expected_diagnostic
                    )
                if expected_diagnostic not in combined:
                    fail(
                        "GCC launcher mutation produced wrong diagnostic: "
                        + combined
                    )

        run_launcher(expect_success=True)
        gxx.write_text(gxx_text + "# launcher replacement\n", encoding="utf-8")
        run_launcher(
            expect_success=False,
            expected_diagnostic="compiler SHA-256 changed",
        )
        gxx.write_text(gxx_text, encoding="utf-8")
        gxx.chmod(0o755)
        inventory.write_bytes(positive_inventory + b" ")
        run_launcher(
            expect_success=False,
            expected_diagnostic="inventory SHA-256 changed",
        )
        inventory.write_bytes(positive_inventory)
        std_source.write_bytes(original_std + b"// launcher replacement\n")
        run_launcher(
            expect_success=False,
            expected_diagnostic="module-source closure SHA-256 changed",
        )
        std_source.write_bytes(original_std)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", type=Path, required=True)
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--production-manifest", type=Path)
    parser.add_argument("--test-indexes-manifest", type=Path)
    parser.add_argument("--core-manifest", type=Path)
    parser.add_argument("--core-seal", type=Path)
    parser.add_argument("--generated-root", type=Path)
    parser.add_argument(
        "--allowed-core-generated-source",
        action="append",
        default=[],
        type=Path,
    )
    parser.add_argument(
        "--allowed-production-guc-generated-source",
        action="append",
        default=[],
        type=Path,
    )
    parser.add_argument(
        "--allowed-test-indexes-guc-generated-source",
        action="append",
        default=[],
        type=Path,
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.repository_root.resolve()
    verify_annotation_fixtures(root)
    verify_compile_contract_fixtures(root)
    verify_provenance_fixtures(root)
    verify_cmake_wiring(root)
    run_proof_vs_scan_wiring_mutation_self_tests(root)
    run_sql_codegen_ast_proof_pool_mutation_self_tests(root)
    run_gcc_wiring_mutation_self_tests(root)
    run_fake_gcc_toolchain_self_tests(root)
    run_tamper_self_test()

    artifact_args = (
        args.production_manifest,
        args.test_indexes_manifest,
        args.core_manifest,
        args.core_seal,
        args.generated_root,
    )
    if args.source_only:
        if (
            any(artifact_args)
            or args.allowed_core_generated_source
            or args.allowed_production_guc_generated_source
            or args.allowed_test_indexes_guc_generated_source
        ):
            fail("--source-only cannot receive generated-artifact arguments")
    else:
        if (
            not all(artifact_args)
            or not args.allowed_core_generated_source
            or not args.allowed_production_guc_generated_source
            or not args.allowed_test_indexes_guc_generated_source
        ):
            fail(
                "generated verification requires both profile manifests, "
                "CORE manifest/seal, generated root, and three explicit "
                "category allowlists"
            )
        verify_artifact_bundle(
            root,
            args.production_manifest.resolve(),
            args.test_indexes_manifest.resolve(),
            args.core_manifest.resolve(),
            args.core_seal.resolve(),
            args.generated_root.resolve(),
            args.allowed_core_generated_source,
            args.allowed_production_guc_generated_source,
            args.allowed_test_indexes_guc_generated_source,
        )

    print(
        "GCC-only SQL contracts: source/negative/provenance/tamper"
        + ("" if args.source_only else "/cryptographic-profile")
        + " verification passed"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
