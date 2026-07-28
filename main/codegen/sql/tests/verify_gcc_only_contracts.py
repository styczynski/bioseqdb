#!/usr/bin/env python3
"""Static and cryptographic contracts for the DOC0388 GCC-only graph."""

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
    r"(bioseq\.sql\.generated\.(?:core|production|test_indexes)\."
    r"[A-Za-z0-9_.]+)\s*;"
)
SHA256 = re.compile(r"^[0-9a-f]{64}$")
GUC_REGISTRATION_KEYS = frozenset(
    {"authority", "candidate", "generated_live_authority"}
)
GUC_AUTHORITY_KEYS = frozenset(
    {
        "registration_ordinal",
        "source_owner",
        "name",
        "group",
        "member",
        "kind",
        "storage_kind",
        "storage",
        "default_expression",
        "minimum_expression",
        "maximum_expression",
        "context",
        "flags",
        "unit",
        "hooks",
        "feature_predicate",
        "profiles",
        "default_semantic",
        "enum_rows_name_wire_hidden_feature_alias_of",
        "enum_omissions_name_wire",
    }
)
GUC_CANDIDATE_KEYS = frozenset(
    {
        "owner",
        "cpp_storage",
        "storage_kind",
        "source_module",
        "source_sha256",
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
GUC_SOURCE_OWNERS = frozenset(
    {"column", "custom_scan", "batch_sort_scan"}
)
PROVENANCE_IDENTIFIERS = {
    "relation_entity_handle": "BIOSEQ_SQL_E_CATALOG_HANDLE_PROVENANCE",
    "guc_registration_access": "BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE",
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
    "authored_migration_install_owner.cppm":
        "BIOSEQ_SQL_E_MIGRATION_OWNER_PROVENANCE",
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
            "bioseq_gcc_module_source_closure/v1",
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
            "bioseq_gcc_module_source_closure/v1",
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
        "root": root / "main/CMakeLists.txt",
        "helper": root / "main/cmake/BioseqGccToolchain.cmake",
        "codegen_helper": root / "main/cmake/BioseqCodegen.cmake",
        "codegen": root / "main/codegen/CMakeLists.txt",
        "launcher": root / "main/cmake/gcc_hwy_header_unit_launcher.py",
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
        root / "main/codegen/sql/tests/annotation_probe.cppm"
    )
    positive_tu_path = root / "main/codegen/sql/tests/annotation_probe.cpp"
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

    negative_root = root / "main/codegen/sql/tests/negative"
    for filename, diagnostic in NEGATIVE_DIAGNOSTICS.items():
        verify_negative_fixture(negative_root / filename, diagnostic)

    contract_path = root / "main/codegen/sql/contract_tests.cppm"
    contract = read_text(contract_path)
    require_tokens(
        contract_path,
        contract,
        tuple(NEGATIVE_DIAGNOSTICS)
        + tuple(NEGATIVE_DIAGNOSTICS.values())
        + (
            "collect_fixture_sources",
            "collect_provenance_checked_fixture",
        ),
    )
    collector_path = root / "main/codegen/sql/collector/index.cppm"
    collector = read_text(collector_path)
    require_tokens(
        collector_path,
        collector,
        (
            "clang_structural::scan_translation_unit(source_path)",
            "contains_structural_terminal_identifier",
            "relation_entity_handle",
            "guc_registration_access",
            "postgres_enum_registration",
            "BIOSEQ_SQL_E_CATALOG_HANDLE_PROVENANCE",
            "BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE",
            "BIOSEQ_SQL_E_GUC_REGISTRATION_PROVENANCE",
            "BIOSEQ_SQL_E_MIGRATION_OWNER_PROVENANCE",
        ),
    )
    provenance_validator = collector[
        collector.find("collect_provenance_checked_fixture") :
    ]
    if "source.contains(" in provenance_validator:
        fail("provenance validator falls back to raw source substrings")
    options_path = root / "main/codegen/sql/collector/options.cppm"
    require_tokens(
        options_path,
        read_text(options_path),
        (
            'attribute.name == "guc_value"',
            "detail::split_top_level",
            "BIOSEQ_SQL_E_MIXED_INITIALIZER",
        ),
    )
    public_annotation_paths = (
        root / "main/codegen/sql/annotation_model/entities.cppm",
        root / "main/codegen/sql/annotation_model/options.cppm",
        root / "main/codegen/sql/annotation_model/validation.cppm",
        root / "main/codegen/sql/annotation_model/index.cppm",
    )
    migration_only_tokens = (
        "migration_install_action",
        "migration_statement_kind",
        "typed_install_statement",
        "install_action_options",
        "typed_install_options",
    )
    for path in public_annotation_paths:
        text = read_text(path)
        for token in migration_only_tokens:
            if token in text:
                fail(
                    f"public annotation model {path} exposes "
                    f"migration-only token {token}"
                )
    migration_model_path = (
        root / "main/codegen/sql/annotation_model/migration.cppm"
    )
    require_tokens(
        migration_model_path,
        read_text(migration_model_path),
        (
            "export module bioseq.codegen.sql.annotation_model.migration;",
            "export import bioseq.codegen.sql.annotation_model;",
        ) + migration_only_tokens,
    )


def verify_compile_contract_fixtures(root: Path) -> None:
    fixture_root = (
        root / "main/bioseq/backend/pg/pg_utils/compile_contracts"
    )
    catalog_producer = fixture_root / (
        "catalog_entity_handle_producer_test.cppm"
    )
    catalog_producer_text = read_text(catalog_producer)
    verify_only_imports(
        catalog_producer,
        catalog_producer_text,
        ("bioseq.pg_utils.sql_catalog_entities",),
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
            "bioseq.sql.generated.core."
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
        ("bioseq.config.guc_storage",),
    )
    require_tokens(
        guc_producer,
        guc_producer_text,
        (
            "postgres_enum_storage<wire_mode>",
            "guc_registration_access::init(",
            "register_storage(",
        ),
    )

    guc_consumer = fixture_root / "generated_guc_consumer_test.cpp"
    guc_consumer_text = read_text(guc_consumer)
    verify_only_imports(
        guc_consumer,
        guc_consumer_text,
        ("bioseq.sql.generated.core.generated_guc_producer_test",),
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
            "bioseq.pg_utils.sql_boundary",
            "bioseq.storage.indexing_pg.sql_row_codecs",
            "bioseq.storage.manager.sql_row_codecs",
            "bioseq.storage.metadata.sql_row_codecs",
        ),
    )
    require_tokens(
        boundary_producer,
        boundary_producer_text,
        (
            "bioseq::pg_utils::setof<",
            "bioseq::pg_utils::returns_table<",
            "bioseq::pg_utils::setof_named_out<",
            "bioseq::pg_utils::named_out<",
            "&bioseq::pg_utils::invoke_sql_boundary<boundary_callable>",
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
            "bioseq.sql.generated.core."
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

    active_guc = fixture_root / "active_generated_guc_consumer_test.cpp"
    active_guc_text = read_text(active_guc)
    require_tokens(
        active_guc,
        active_guc_text,
        (
            "import bioseq.sql.generated.production.bioseq.gucs;",
            "import bioseq.sql.generated.production.guc_registry;",
            "import bioseq.sql.generated.test_indexes.bioseq.gucs;",
            "import bioseq.sql.generated.test_indexes.guc_registry;",
            "active_guc_owner::registration_count == 29U",
            "active_guc_owner::registration_count == 30U",
            "registration_count == 76U",
            "registration_count == 77U",
            "&active_guc_owner::register_logging",
            "&active_guc_registry::register_gucs",
        ),
    )


def verify_provenance_fixtures(root: Path) -> None:
    lexical_decoys = r'''
// relation_entity_handle guc_registration_access
/* postgres_enum_registration */
export module decoy.guc_registration_access;
import decoy.relation_entity_handle;
export import decoy.postgres_enum_registration;
constexpr auto a = "relation_entity_handle";
constexpr auto b = R"guard(guc_registration_access
postgres_enum_registration)guard";
constexpr char c = 'x';
'''
    if (
        cpp_terminal_identifiers(lexical_decoys)
        & set(PROVENANCE_IDENTIFIERS)
    ):
        fail("static provenance lexer accepts comment/string decoys")

    fixture_root = (
        root / "main/bioseq/backend/pg/pg_utils/compile_contracts"
    )
    producer = fixture_root / "catalog_entity_handle_producer_test.cppm"
    guc_producer = fixture_root / "generated_guc_producer_test.cppm"
    if (
        "relation_entity_handle"
        not in cpp_terminal_identifiers(read_text(producer))
    ):
        fail("generated-like catalog producer has no handle specialization")
    if (
        "guc_registration_access"
        not in cpp_terminal_identifiers(read_text(guc_producer))
    ):
        fail("generated-like GUC producer does not instantiate access")

    allowed_relative: dict[str, dict[str, tuple[str, ...]]] = {
        "relation_entity_handle": {
            "definitions": (
                "main/bioseq/backend/pg/pg_utils/"
                "sql_catalog_entities.cppm",
            ),
            "infrastructure": (),
            "tests": (
                "main/bioseq/backend/pg/pg_utils/compile_contracts/"
                "catalog_entity_handle_producer_test.cppm",
            ),
        },
        "guc_registration_access": {
            "definitions": (
                "main/bioseq/backend/config/guc_storage.cppm",
            ),
            "infrastructure": (
                "main/bioseq/backend/pg/pg_utils/sql_boundary/"
                "guc_storage.cppm",
            ),
            "tests": (
                "main/bioseq/backend/pg/pg_utils/compile_contracts/"
                "generated_guc_producer_test.cppm",
                "main/bioseq/backend/common/guc_storage_test.cpp",
            ),
        },
        "postgres_enum_registration": {
            "definitions": (
                "main/bioseq/backend/pg/pg_utils/sql_boundary/"
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

    backend_root = root / "main/bioseq/backend"
    for path in sorted(backend_root.rglob("*")):
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

    negative_root = root / "main/codegen/sql/tests/negative"
    for filename, diagnostic in NEGATIVE_DIAGNOSTICS.items():
        require_provenance_fixture_exercise(
            filename,
            read_text(negative_root / filename),
            diagnostic,
        )

    access_fixture = negative_root / "authored_guc_registration_access.cppm"
    access_text = read_text(access_fixture)
    actual_access = (
        "    bioseq::pg_utils::guc_registration_access::init(\n"
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
    require_tokens(
        cmake_path,
        cmake,
        (
            "BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY",
            "--required-target-dyndep-inventory",
            "function(_bioseq_finalize_sql_target_dyndep_anchors)",
            "get_target_property(_bioseq_sql_target_binary_dir",
            '"${_bioseq_sql_target}" BINARY_DIR)',
            '"${_bioseq_sql_target_binary_dir}/CMakeFiles/'
            '${_bioseq_sql_target}.dir/CXX.dd"',
            'file(CONFIGURE\n'
            '    OUTPUT "${BIOSEQ_SQL_REQUIRED_TARGET_DYNDEP_INVENTORY}"',
            "cmake_language(DEFER CALL\n"
            "  _bioseq_finalize_sql_target_dyndep_anchors)",
            "BIOSEQ_SQL_POSTGRES_EXTENSION_PROOF_STAMP",
            "add_custom_target(bioseq_sql_postgresql_link_proof\n"
            "  DEPENDS "
            '"${BIOSEQ_SQL_POSTGRES_EXTENSION_PROOF_STAMP}")',
            "BIOSEQ_SQL_PRODUCTION_DETERMINISM_SELF_TEST_STAMP",
            "add_custom_target("
            "bioseq_sql_production_determinism_self_test\n",
            "add_custom_target(bioseq_sql_production_determinism\n",
        ),
    )
    if "BIOSEQ_SQL_POSTGRES_EXTENSION_CXX_DYNDEP" in cmake:
        fail("extension-only SQL PREPARE dyndep wiring was retained")

    anchor_definition = re.search(
        r"set\(_bioseq_sql_target_dyndep_anchors\s+"
        r"(.*?)\)\s*"
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
        "bioseq_bench",
        "bioseq_bench_e2e_tests_exec",
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
    observed_cache_anchors = set(anchor_definition.group(2).split())
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
        '"${BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT}"',
        '"${BIOSEQ_SQL_SAFETY_PRODUCTION_PREPARE_STAMP}"',
        '"${BIOSEQ_SQL_SAFETY_TEST_INDEXES_PREPARE_STAMP}"',
    }
    if observed_outputs != expected_outputs:
        fail(
            "target dyndep anchors are not attached to the exact "
            "determinism and two safety PREPARE outputs"
        )

    proof_command = re.search(
        r"add_custom_command\(\s*"
        r'OUTPUT\s+"\$\{BIOSEQ_SQL_POSTGRES_EXTENSION_PROOF_STAMP\}"'
        r"(.*?)VERBATIM\s*\)",
        cmake,
        flags=re.DOTALL,
    )
    proof_body = "" if proof_command is None else proof_command.group(1)
    for target in (
        "bioseq_sql_check_golden",
        "bioseq_sql_gcc_compile_contracts",
    ):
        if proof_body.count(target) != 1:
            fail(
                "PostgreSQL extension proof stamp does not depend exactly "
                f"once on {target}"
            )
    if (
        "bioseq_sql_production_determinism" in proof_body
        or "BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT" in proof_body
    ):
        fail(
            "PostgreSQL extension link proof depends on full production "
            "determinism replay"
        )
    if "bioseq_sql_postgres_error_safety" in proof_body:
        fail(
            "PostgreSQL extension link proof depends on the opt-in "
            "PostgreSQL compiler-facts/proof closure"
        )

    optional_target = re.search(
        r"add_custom_target\(\s*"
        r"bioseq_sql_production_determinism\s+"
        r'DEPENDS\s+"\$\{BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT\}"'
        r"\s*\)",
        cmake,
        flags=re.DOTALL,
    )
    if optional_target is None:
        fail(
            "full production determinism replay is not retained as an "
            "explicit opt-in target"
        )
    self_test_target = re.search(
        r"add_custom_target\(\s*"
        r"bioseq_sql_production_determinism_self_test\s+"
        r"DEPENDS\s+"
        r'"\$\{BIOSEQ_SQL_PRODUCTION_DETERMINISM_SELF_TEST_STAMP\}"'
        r"\s*\)",
        cmake,
        flags=re.DOTALL,
    )
    if self_test_target is None:
        fail(
            "cheap production determinism self-tests are not retained "
            "as a standalone target"
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
    optional_dependencies = dependency_tokens.get(
        "bioseq_sql_production_determinism", set()
    )
    if optional_dependencies != {
        "bioseq_sql_final_production",
        "bioseq_sql_production_determinism_self_test",
    }:
        fail(
            "opt-in production determinism target does not have its "
            "exact final-generation and self-test prerequisites"
        )
    for owner, body in dependency_blocks:
        if (
            owner != "bioseq_sql_production_determinism"
            and re.search(
                r"\bbioseq_sql_production_determinism\b", body
            )
        ):
            fail(
                f"default-capable target {owner} depends on full "
                "production determinism replay"
            )

    external_dependencies = dependency_tokens.get(
        "bioseq_sql_external_feature_contract", set()
    )
    if external_dependencies != {
        "bioseq_sql_production_determinism_self_test"
    }:
        fail(
            "external feature contract does not depend exactly on the "
            "cheap production determinism self-tests"
        )
    check_golden_dependencies = dependency_tokens.get(
        "bioseq_sql_check_golden", set()
    )
    required_check_golden_dependencies = {
        "bioseq_doc0388_ledgers",
        "bioseq_sql_external_feature_contract",
        "bioseq_sql_production_determinism_self_test",
        "bioseq_sql_validator_self_test",
    }
    if not required_check_golden_dependencies.issubset(
        check_golden_dependencies
    ):
        fail(
            "default SQL golden gate lost a ledger, feature, validator, "
            "or cheap determinism prerequisite"
        )
    if "bioseq_sql_postgres_error_safety" in check_golden_dependencies:
        fail(
            "default SQL golden gate depends on the opt-in PostgreSQL "
            "compiler-facts/proof closure"
        )
    require_tokens(
        cmake_path,
        cmake,
        (
            "set(_bioseq_sql_check_golden_outputs\n"
            "  ${_bioseq_sql_final_stamps}\n"
            "  ${_bioseq_sql_external_feature_contract_stamps})",
            "add_custom_target("
            "bioseq_sql_postgres_error_safety_static_evidence\n"
            '    DEPENDS "${BIOSEQ_SQL_SAFETY_EVIDENCE_REPORT}")',
        ),
    )
    if re.search(
        r"list\(APPEND\s+_bioseq_sql_check_golden_outputs\b"
        r".*?BIOSEQ_SQL_SAFETY_EVIDENCE_REPORT",
        cmake,
        flags=re.DOTALL,
    ):
        fail(
            "default SQL golden outputs include the opt-in PostgreSQL "
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
        r'"\$\{BIOSEQ_SQL_POSTGRES_EXTENSION_PROOF_STAMP\}"\s*\)',
        cmake,
        flags=re.DOTALL,
    )
    if link_gate is None:
        fail("PostgreSQL extension link does not consume the proof stamp")

    forbidden_target_wide = re.search(
        r"add_dependencies\(\s*"
        r"(?:postgresql_bioseq|bioseq_bench)\s+"
        r"(?:bioseq_sql_check_golden"
        r"|bioseq_sql_gcc_compile_contracts"
        r"|bioseq_sql_postgres_error_safety"
        r"|bioseq_sql_production_determinism"
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
    cmake_path = root / "main/bioseq/CMakeLists.txt"
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
        "    bioseq_bench\n"
        "    bioseq_bench_e2e_tests_exec\n"
        "    bioseq_sql_annotation_probe\n"
        "    bioseq_sql_gcc_compile_contract_objects\n"
        "    postgresql_bioseq)",
        "  set(_bioseq_sql_target_dyndep_anchors\n"
        "    bioseq_bench_e2e_tests_exec\n"
        "    bioseq_sql_annotation_probe\n"
        "    bioseq_sql_gcc_compile_contract_objects\n"
        "    postgresql_bioseq)",
    )
    expect_rejected(
        "test-index safety PREPARE dyndep dependency removed",
        '      "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT}"\n'
        '      "${BIOSEQ_SQL_SAFETY_PRODUCTION_PREPARE_STAMP}"\n'
        '      "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_PREPARE_STAMP}")',
        '      "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT}"\n'
        '      "${BIOSEQ_SQL_SAFETY_PRODUCTION_PREPARE_STAMP}")',
    )
    expect_rejected(
        "full replay restored to the default golden gate",
        "  bioseq_sql_check_golden\n"
        "  bioseq_sql_validator_self_test\n"
        "  bioseq_sql_external_feature_contract\n"
        "  bioseq_sql_production_determinism_self_test)",
        "  bioseq_sql_check_golden\n"
        "  bioseq_sql_validator_self_test\n"
        "  bioseq_sql_external_feature_contract\n"
        "  bioseq_sql_production_determinism)",
    )
    expect_rejected(
        "PostgreSQL safety proof restored to the default DSO graph",
        "  bioseq_sql_check_golden\n"
        "  bioseq_sql_validator_self_test\n"
        "  bioseq_sql_external_feature_contract\n"
        "  bioseq_sql_production_determinism_self_test)",
        "  bioseq_sql_check_golden\n"
        "  bioseq_sql_validator_self_test\n"
        "  bioseq_sql_external_feature_contract\n"
        "  bioseq_sql_postgres_error_safety\n"
        "  bioseq_sql_production_determinism_self_test)",
    )
    expect_rejected(
        "production safety profile removed from the opt-in target",
        "  PRODUCTION_TARGET "
        "bioseq_sql_postgres_error_safety_production\n",
        "  PRODUCTION_TARGET "
        "bioseq_sql_postgres_error_safety_test_indexes\n",
    )
    expect_rejected(
        "opt-in production determinism target removed",
        "add_custom_target(bioseq_sql_production_determinism\n"
        '  DEPENDS "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT}")',
        "add_custom_target(bioseq_sql_production_determinism_removed\n"
        '  DEPENDS "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT}")',
    )
    expect_rejected(
        "link proof changed into target-wide proof",
        "set_property(TARGET postgresql_bioseq APPEND PROPERTY LINK_DEPENDS\n"
        '  "${BIOSEQ_SQL_POSTGRES_EXTENSION_PROOF_STAMP}")',
        "add_dependencies(postgresql_bioseq bioseq_sql_check_golden)",
    )


def verify_sql_codegen_ast_proof_pool_wiring(
    root: Path,
    *,
    root_cmake: str | None = None,
    bioseq_cmake: str | None = None,
    safety_cmake: str | None = None,
) -> None:
    root_cmake_path = root / "main/CMakeLists.txt"
    bioseq_cmake_path = root / "main/bioseq/CMakeLists.txt"
    safety_cmake_path = (
        root / "main/codegen/safety/PostgresErrorSafety.cmake"
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
    require_command_pool(
        bioseq_cmake_path,
        bioseq_cmake,
        'OUTPUT "${BIOSEQ_SQL_PRODUCTION_DETERMINISM_REPORT}"',
        "production determinism",
    )

    if safety_cmake.count("JOB_POOL sql_codegen_ast_proof") != 2:
        fail(
            "PostgreSQL safety must place only PREPARE and emit-facts "
            "in the SQL codegen AST proof pool"
        )
    if bioseq_cmake.count("JOB_POOL sql_codegen_ast_proof") != 1:
        fail(
            "bioseq CMake must place only production determinism in "
            "the SQL codegen AST proof pool"
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
    root_cmake = read_text(root / "main/CMakeLists.txt")
    bioseq_cmake = read_text(root / "main/bioseq/CMakeLists.txt")
    safety_cmake = read_text(
        root / "main/codegen/safety/PostgresErrorSafety.cmake"
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
            '        COMMENT "Emitting schema-v2',
            "            ${ARG_SOURCES}\n"
            "        VERBATIM\n"
            '        COMMENT "Emitting schema-v2',
            1,
        ),
    )
    expect_rejected(
        "a production determinism pool deletion",
        mutated_bioseq=bioseq_cmake.replace(
            "  JOB_POOL sql_codegen_ast_proof\n"
            "  COMMENT\n"
            '    "Proving production SQL generation',
            "  COMMENT\n"
            '    "Proving production SQL generation',
            1,
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


def verify_cmake_wiring(root: Path) -> None:
    cmake_path = root / "main/bioseq/CMakeLists.txt"
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
            'PostgresErrorSafety.cmake"'
        ),
        '"${CMAKE_CURRENT_LIST_DIR}/../cmake/BioseqCodegen.cmake"',
        '"${CMAKE_CURRENT_LIST_DIR}/../cmake/BioseqGccToolchain.cmake"',
        '"${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/collector/index.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/collector/options.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/contract_tests.cppm"',
        "BIOSEQ_SQL_GCC_CONTRACT_PROVENANCE_SOURCES",
        "${BIOSEQ_SQL_GCC_CONTRACT_SOURCE_INPUTS}",
        "list(REMOVE_ITEM SRC_MOD_FILES "
        "${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_MODULES})",
        "list(REMOVE_ITEM SRC_FILES "
        "${BIOSEQ_SQL_GCC_COMPILE_CONTRACT_SOURCES})",
    )
    require_tokens(cmake_path, cmake, required)
    verify_proof_vs_scan_wiring(cmake_path, cmake)
    verify_sql_codegen_ast_proof_pool_wiring(root)
    authored_provider_closure = re.search(
        r"set\(BIOSEQ_SQL_COMPILE_CONTRACT_AUTHORED_PROVIDERS\b"
        r"(.*?)\)\s*list\(REMOVE_ITEM\s+SRC_MOD_FILES",
        cmake,
        flags=re.DOTALL,
    )
    required_authored_providers = {
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/alignment_sql_rows.cppm"',
        '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/mod_sql_rows.cppm"',
        (
            '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/sequences_pg/'
            'sample_table_udf_sql_rows.cppm"'
        ),
        (
            '"${CMAKE_CURRENT_LIST_DIR}/backend/pg/storage_pg/io/'
            'sql_rows.cppm"'
        ),
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
    production_begin = cmake.find("AddCommonTargetX(\n  postgresql_bioseq")
    if production_begin < 0:
        fail("PostgreSQL extension target call is absent")
    production_call = cmake[production_begin:]
    production_call = production_call[:production_call.find("\n)")]
    if "bioseq_sql_compile_contract_providers" not in production_call:
        fail("PostgreSQL extension does not link the SQL provider closure")
    aligner_begin = cmake.find("AddCommonTargetX(\n  bioseq_aligner")
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
        "sql_entity_handles_contract.cpp",
    ):
        if fixture in production_call:
            fail(f"production extension directly owns contract fixture {fixture}")

    codegen_cmake_path = root / "main/codegen/CMakeLists.txt"
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
            if "sql/tests/annotation_probe.cppm" in call
            and "sql/tests/annotation_probe.cpp" in call
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
        if not (root / "main/bioseq" / relative).is_file():
            fail(f"SQL provider registry names an absent source: {relative}")
    annotation_owners: dict[str, list[str]] = {
        "bioseq_modules_nopg": [],
        "bioseq_sql_compile_contract_providers": [],
        "postgresql_bioseq": [],
    }
    backend_root = root / "main/bioseq/backend"
    for source in sorted(backend_root.rglob("*")):
        if source.suffix not in {".cpp", ".cppm"}:
            continue
        if "[[bioseq_sql::" not in read_text(source):
            continue
        relative = source.relative_to(
            root / "main/bioseq"
        ).as_posix()
        if not relative.startswith("backend/pg/"):
            owner = "bioseq_modules_nopg"
        elif relative in provider_sources:
            owner = "bioseq_sql_compile_contract_providers"
        else:
            owner = "postgresql_bioseq"
        annotation_owners[owner].append(relative)
    if not all(annotation_owners.values()):
        fail(
            "annotation source ownership map did not cover nopg, provider, "
            "and PostgreSQL targets"
        )

    root_cmake_path = root / "main/CMakeLists.txt"
    root_cmake = read_text(root_cmake_path)
    helper_path = root / "main/cmake/BioseqGccToolchain.cmake"
    helper = read_text(helper_path)
    codegen_helper_path = root / "main/cmake/BioseqCodegen.cmake"
    codegen_helper = read_text(codegen_helper_path)
    codegen_cmake_path = root / "main/codegen/CMakeLists.txt"
    codegen_cmake = read_text(codegen_cmake_path)
    launcher_path = (
        root / "main/cmake/gcc_hwy_header_unit_launcher.py"
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
    material = "bioseq_sql_core_descriptor/v1\n"
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
    material = "bioseq_sql_core_artifact_set/v1\n"
    material += "".join(row + "\n" for row in rows)
    return sha256_bytes(material.encode())


def profile_core_records(manifest: dict[str, Any], label: str) -> list[dict[str, Any]]:
    if manifest.get("manifest_version") != 1:
        fail(f"{label} manifest has an incompatible version")
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
) -> tuple[int, str, str, str, str]:
    label = f"{profile} GUC registration {index}"
    record = require_exact_object(
        registration, GUC_REGISTRATION_KEYS, label
    )
    authority = require_exact_object(
        record["authority"], GUC_AUTHORITY_KEYS, f"{label}.authority"
    )
    candidate = require_exact_object(
        record["candidate"], GUC_CANDIDATE_KEYS, f"{label}.candidate"
    )
    live = require_exact_object(
        record["generated_live_authority"],
        GUC_GENERATED_LIVE_KEYS,
        f"{label}.generated_live_authority",
    )

    ordinal = authority["registration_ordinal"]
    if type(ordinal) is not int or ordinal < 0:
        fail(f"{label}.authority.registration_ordinal is not non-negative")

    authority_strings = {
        key: require_nonempty_string(
            authority, key, f"{label}.authority"
        )
        for key in (
            "source_owner",
            "name",
            "group",
            "member",
            "kind",
            "storage_kind",
            "storage",
            "default_expression",
            "context",
            "feature_predicate",
        )
    }
    if authority_strings["source_owner"] not in GUC_SOURCE_OWNERS:
        fail(f"{label} has an unknown source-owner provenance")
    expected_storage_kind = GUC_KIND_STORAGE_KINDS.get(
        authority_strings["kind"]
    )
    if expected_storage_kind != authority_strings["storage_kind"]:
        fail(f"{label} has an invalid kind/storage authority pair")
    if authority_strings["name"] != (
        authority_strings["group"]
        + "."
        + authority_strings["member"]
    ):
        fail(f"{label} has inconsistent name/group/member authority")

    for key in (
        "minimum_expression",
        "maximum_expression",
        "unit",
        "default_semantic",
    ):
        require_optional_string(
            authority[key], f"{label}.authority.{key}"
        )
    require_string_array(
        authority["flags"], f"{label}.authority.flags"
    )
    profiles = require_string_array(
        authority["profiles"],
        f"{label}.authority.profiles",
        nonempty=True,
    )
    expected_profiles = (
        ["test_indexes"]
        if authority_strings["name"]
        == "bioseq.test_force_paper_index_failure"
        else ["production", "test_indexes"]
    )
    if profiles != expected_profiles or profile not in profiles:
        fail(f"{label} has mismatched profile provenance")

    hooks = authority["hooks"]
    if not isinstance(hooks, list) or len(hooks) != 3:
        fail(f"{label}.authority.hooks is not the three-hook array")
    for hook_index, hook in enumerate(hooks):
        require_optional_string(
            hook, f"{label}.authority.hooks[{hook_index}]"
        )

    expected_context = {
        "sighup": "sighup",
        "superuser_set": "superuser",
        "user_set": "user",
    }.get(authority_strings["context"])
    expected_unit = "MB" if authority["unit"] == "mb" else authority["unit"]
    if (
        live["vartype"] != authority_strings["kind"]
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
    bounded = authority_strings["kind"] in {"integer", "real"}
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
            authority_strings["kind"] == "enum"
            and not visible_enum_values
        )
        or (
            authority_strings["kind"] != "enum"
            and visible_enum_values
        )
    ):
        fail(f"{label}.generated_live_authority enum values differ")
    if (
        authority_strings["kind"] == "enum"
        and live["boot_value"] not in visible_enum_values
    ) or (
        authority_strings["kind"] == "bool"
        and live["boot_value"] not in {"on", "off"}
    ):
        fail(f"{label}.generated_live_authority boot value differs")
    expected_raw_flags = 0
    if "no_show_all" in authority["flags"]:
        expected_raw_flags |= 0x000004
    if "not_in_sample" in authority["flags"]:
        expected_raw_flags |= 0x000080
    expected_raw_flags |= {
        None: 0,
        "kb": 0x01000000,
        "blocks": 0x02000000,
        "mb": 0x04000000,
        "ms": 0x10000000,
        "s": 0x20000000,
        "min": 0x30000000,
    }.get(authority["unit"], -1)
    if (
        live["raw_flags"] != expected_raw_flags
        or live["visible_in_show_all"]
        != ("no_show_all" not in authority["flags"])
        or live["included_in_sample"]
        != ("not_in_sample" not in authority["flags"])
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
        symbol = authority["hooks"][hook_index]
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

    enum_rows = authority[
        "enum_rows_name_wire_hidden_feature_alias_of"
    ]
    if not isinstance(enum_rows, list):
        fail(f"{label}.authority enum rows are not an array")
    for row_index, row in enumerate(enum_rows):
        row_label = f"{label}.authority.enum_rows[{row_index}]"
        if not isinstance(row, list) or len(row) != 5:
            fail(f"{row_label} is not a five-field row")
        if any(
            not isinstance(row[field], str) or not row[field]
            for field in (0, 1, 3)
        ):
            fail(f"{row_label} has malformed string provenance")
        if type(row[2]) is not int or row[2] not in (0, 1):
            fail(f"{row_label} has a malformed hidden flag")
        require_optional_string(row[4], f"{row_label}.alias_of")

    omissions = authority["enum_omissions_name_wire"]
    if not isinstance(omissions, list):
        fail(f"{label}.authority enum omissions are not an array")
    for omission_index, omission in enumerate(omissions):
        omission_label = (
            f"{label}.authority.enum_omissions[{omission_index}]"
        )
        if (
            not isinstance(omission, list)
            or len(omission) != 2
            or any(
                not isinstance(item, str) or not item
                for item in omission
            )
        ):
            fail(f"{omission_label} is not a two-string row")

    candidate_strings = {
        key: require_nonempty_string(
            candidate, key, f"{label}.candidate"
        )
        for key in (
            "owner",
            "cpp_storage",
            "storage_kind",
            "source_module",
        )
    }
    candidate_source_sha256 = candidate["source_sha256"]
    if not isinstance(candidate_source_sha256, str):
        fail(f"{label}.candidate.source_sha256 is not a string")
    require_sha256(
        candidate_source_sha256,
        f"{label}.candidate.source_sha256",
    )
    if candidate_strings["owner"] != authority_strings["group"]:
        fail(f"{label} candidate owner differs from authority")
    if (
        candidate_strings["storage_kind"]
        != authority_strings["storage_kind"]
    ):
        fail(f"{label} candidate storage kind differs from authority")
    return (
        ordinal,
        authority_strings["name"],
        candidate_strings["owner"],
        candidate_strings["source_module"],
        candidate_source_sha256,
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
    guc_entities: dict[str, tuple[str, str]] = {}
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
        source = canonical_repository_source(
            repository_root,
            entity.get("source_path"),
            f"{label} GUC entity {name}",
        )
        if sha256_bytes(read_bytes(source)) != source_sha256:
            fail(
                f"{label} GUC entity {name} source hash differs from "
                "repository bytes"
            )
        if not name or name in guc_entities:
            fail(f"{label} GUC entity identities are duplicated")
        guc_entities[name] = (source_module, source_sha256)
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
        if not isinstance(source_module, str) or not source_module:
            fail(f"{label} GUC owner source module is malformed")
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
            candidate_owner,
            candidate_source_module,
            candidate_source_sha256,
        ) = validate_guc_registration(
            registration, label, index
        )
        if name in names:
            fail(f"{label} GUC names are duplicated or malformed")
        names.add(name)
        ordinals.append(ordinal)
        if candidate_owner == "bioseq":
            owner_key = candidate_owner
        elif (
            candidate_owner.startswith("bioseq.")
            and candidate_owner.count(".") == 1
        ):
            owner_key = candidate_owner.removeprefix("bioseq.")
        else:
            fail(f"{label} GUC candidate owner provenance is malformed")
        if owner_source_modules.get(owner_key) != candidate_source_module:
            fail(
                f"{label} GUC candidate source module is not owned by "
                f"{owner_key}"
            )
        if guc_entities.get(name) != (
            candidate_source_module,
            candidate_source_sha256,
        ):
            fail(
                f"{label} GUC candidate provenance does not join the "
                f"manifest entity for {name}"
            )
        observed_owner_counts[owner_key] += 1
    if len(names) != len(registrations):
        fail(f"{label} GUC names are duplicated or malformed")
    if ordinals != sorted(set(ordinals)):
        fail(f"{label} GUC registration ordinals are not unique and ordered")
    if observed_owner_counts != owner_registration_counts:
        fail(f"{label} GUC per-owner provenance counts differ")
    if set(guc_entities) != names:
        fail(f"{label} GUC entity and registration inventories differ")
    return records, names


def verify_guc_profile_delta_proof(
    production: dict[str, Any],
    test_indexes: dict[str, Any],
) -> None:
    production_generation = production.get("guc_generation")
    test_generation = test_indexes.get("guc_generation")
    if not isinstance(production_generation, dict) or not isinstance(
        test_generation, dict
    ):
        fail("profile manifests have no GUC generation records")

    production_proof = require_exact_object(
        production_generation.get("profile_delta_proof"),
        GUC_PROFILE_DELTA_KEYS,
        "production GUC profile delta proof",
    )
    test_proof = require_exact_object(
        test_generation.get("profile_delta_proof"),
        GUC_PROFILE_DELTA_KEYS,
        "test_indexes GUC profile delta proof",
    )
    if production_proof != test_proof:
        fail("GUC profile delta proof differs between FINAL manifests")

    production_registry = require_sha256(
        production_generation.get("registry_content_sha256"),
        "production GUC registry content hash",
    )
    test_registry = require_sha256(
        test_generation.get("registry_content_sha256"),
        "test_indexes GUC registry content hash",
    )
    if require_sha256(
        production_proof["production_registry_content_sha256"],
        "profile delta production registry content hash",
    ) != production_registry:
        fail("profile delta production registry hash does not join FINAL")
    if require_sha256(
        production_proof["test_indexes_registry_content_sha256"],
        "profile delta test_indexes registry content hash",
    ) != test_registry:
        fail("profile delta test_indexes registry hash does not join FINAL")

    if require_string_array(
        production_proof["added_in_test_indexes"],
        "profile delta added registrations",
    ) != ["bioseq.test_force_paper_index_failure"]:
        fail("profile delta added registrations differ from the fixture")
    if require_string_array(
        production_proof["removed_from_test_indexes"],
        "profile delta removed registrations",
    ):
        fail("profile delta removed registrations is not empty")
    if require_string_array(
        production_proof["changed_in_test_indexes"],
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
        production_proof["owner_descriptor_profile_invariant"],
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
        production_proof["owner_artifacts"],
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
            authority = record.get("authority")
            if not isinstance(authority, dict):
                fail(f"{profile} GUC authority is malformed")
            name = authority.get("name")
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
        if production_rows[name] != test_rows[name]:
            fail(
                "shared GUC registration authority/candidate differs "
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
        "bioseq.sql.generated.core.",
        ("core", "owners"),
    )
    production_allowed, production_by_module = (
        authorized_generated_category(
            allowed_production_guc_sources,
            canonical_root,
            "production GUC",
            "bioseq.sql.generated.production.",
            ("gucs", "production"),
        )
    )
    test_allowed, test_by_module = authorized_generated_category(
        allowed_test_indexes_guc_sources,
        canonical_root,
        "test_indexes GUC",
        "bioseq.sql.generated.test_indexes.",
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
    if core.get("schema") != "bioseq_sql_core_manifest/v1":
        fail("CORE manifest has an incompatible schema")
    if seal.get("schema") != "bioseq_sql_core_seal/v1":
        fail("CORE seal has an incompatible schema")
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
            f"bioseq.sql.generated.core.{owner}.entity_handles"
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
    verify_guc_profile_delta_proof(production, test_indexes)

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
                    f"bioseq.sql.generated.{label}.guc_registry"
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
                    f"bioseq.sql.generated.{label}.{owner}.gucs"
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
    if test_names - production_names != {
        "bioseq.test_force_paper_index_failure"
    }:
        fail("test-index GUC profile delta differs from the single fixture")


def render_json(value: dict[str, Any]) -> bytes:
    return (json.dumps(value, indent=2) + "\n").encode()


def run_tamper_self_test() -> None:
    with tempfile.TemporaryDirectory(
        prefix="bioseq-gcc-contract-tamper-"
    ) as directory:
        root = Path(directory).resolve()
        generated = root / "generated"
        generated.mkdir()
        guc_source_relative = Path(
            "main/bioseq/backend/config/sql_guc_settings.cppm"
        )
        guc_source = root / guc_source_relative
        guc_source.parent.mkdir(parents=True)
        guc_source_content = "export module bioseq.owner;\n"
        guc_source.write_text(guc_source_content, encoding="utf-8")
        guc_source_sha256 = sha256_bytes(guc_source.read_bytes())
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
                "export module bioseq.sql.generated.core.owner."
                "entity_handles;\n"
            ),
            production_registry: (
                "export module bioseq.sql.generated.production.guc_registry;\n"
            ),
            test_registry: (
                "export module bioseq.sql.generated.test_indexes.guc_registry;\n"
            ),
        }
        for owner in guc_owners:
            contents[production_owner_paths[owner]] = (
                "export module "
                f"bioseq.sql.generated.production.{owner}.gucs;\n"
            )
            contents[test_owner_paths[owner]] = (
                "export module "
                f"bioseq.sql.generated.test_indexes.{owner}.gucs;\n"
            )
        for path, content in contents.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        core_modules = [
            {
                "owner": "owner",
                "source_module": "bioseq.owner",
                "generated_module": (
                    "bioseq.sql.generated.core.owner.entity_handles"
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
            "schema": "bioseq_sql_core_manifest/v1",
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
                    "schema": "bioseq_sql_core_seal/v1",
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
            names: list[str],
        ) -> dict[str, Any]:
            def registration(
                ordinal: int,
                name: str,
            ) -> dict[str, Any]:
                group, separator, member = name.rpartition(".")
                if not separator or not group or not member:
                    fail("tamper self-test GUC identity is malformed")
                test_only = (
                    name
                    == "bioseq.test_force_paper_index_failure"
                )
                profiles = (
                    ["test_indexes"]
                    if test_only
                    else ["production", "test_indexes"]
                )
                kind = "enum" if test_only else "bool"
                storage_kind = "int32_enum" if test_only else "bool"
                enum_values = (
                    ["none", "unsupported_vector_shape"]
                    if test_only
                    else []
                )
                boot_value = "none" if test_only else "off"
                hooks = [None, None, None]
                return {
                    "authority": {
                        "registration_ordinal": ordinal,
                        "source_owner": "column",
                        "name": name,
                        "group": group,
                        "member": member,
                        "kind": kind,
                        "storage_kind": storage_kind,
                        "storage": f"fixture_{member}",
                        "default_expression": "0" if test_only else "false",
                        "minimum_expression": None,
                        "maximum_expression": None,
                        "context": "user_set",
                        "flags": [],
                        "unit": None,
                        "hooks": hooks,
                        "feature_predicate": (
                            "defined(BIOSEQ_TEST_INDEXES)"
                            if test_only
                            else "true"
                        ),
                        "profiles": profiles,
                        "default_semantic": (
                            "none" if test_only else None
                        ),
                        (
                            "enum_rows_name_wire_hidden_feature_"
                            "alias_of"
                        ): (
                            [
                                [
                                    "none",
                                    "0",
                                    0,
                                    "defined(BIOSEQ_TEST_INDEXES)",
                                    None,
                                ],
                                [
                                    "unsupported_vector_shape",
                                    "1",
                                    0,
                                    "defined(BIOSEQ_TEST_INDEXES)",
                                    None,
                                ],
                            ]
                            if test_only
                            else []
                        ),
                        "enum_omissions_name_wire": [],
                    },
                    "candidate": {
                        "owner": group,
                        "cpp_storage": f"fixture::{member}",
                        "storage_kind": storage_kind,
                        "source_module": "bioseq.owner",
                        "source_sha256": guc_source_sha256,
                    },
                    "generated_live_authority": {
                        "vartype": kind,
                        "context": "user",
                        "unit": None,
                        "boot_value": boot_value,
                        "reset_value": boot_value,
                        "current_value": boot_value,
                        "minimum": None,
                        "maximum": None,
                        "visible_enum_values": enum_values,
                        "short_description": "Fixture setting.",
                        "extra_description": None,
                        "raw_flags": 0,
                        "visible_in_show_all": True,
                        "included_in_sample": True,
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
                "manifest_version": 1,
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
                            "bioseq.sql.generated.core.owner.entity_handles"
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
                        "source_module": "bioseq.owner",
                        "source_path": guc_source_relative.as_posix(),
                        "source_sha256": guc_source_sha256,
                    }
                    for name in names
                ],
                "guc_generation": {
                    "profile": label,
                    "registration_count": len(names),
                    "registry_module": exported_generated_module(
                        registry_path, contents[registry_path]
                    ),
                    "registry_content_sha256": sha256_bytes(
                        contents[registry_path].encode()
                    ),
                    "owner_facets": [
                        {
                            "owner": owner,
                            "source_module": "bioseq.owner",
                            "generated_module": exported_generated_module(
                                owner_paths[owner],
                                contents[owner_paths[owner]],
                            ),
                            "content_sha256": sha256_bytes(
                                contents[owner_paths[owner]].encode()
                            ),
                            "registration_count": (
                                len(names) if owner == "bioseq" else 0
                            ),
                        }
                        for owner in guc_owners
                    ],
                    "registrations": [
                        registration(index, name)
                        for index, name in enumerate(names)
                    ],
                },
            }

        production_manifest = profile(
            "production",
            production_owner_paths,
            production_registry,
            ["bioseq.stable"],
        )
        test_manifest = profile(
            "test_indexes",
            test_owner_paths,
            test_registry,
            [
                "bioseq.stable",
                "bioseq.test_force_paper_index_failure",
            ],
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
        profile_delta_proof = {
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
            "added_in_test_indexes": [
                "bioseq.test_force_paper_index_failure"
            ],
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
            "profile_delta_proof"
        ] = copy.deepcopy(profile_delta_proof)
        test_manifest["guc_generation"]["profile_delta_proof"] = (
            copy.deepcopy(profile_delta_proof)
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
            "schema": "bioseq_sql_core_seal/v1",
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
                        "schema": "bioseq_sql_core_seal/v1",
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
            "main/bioseq/backend/config/../config/sql_guc_settings.cppm"
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
        expect_failure(verify, "flat GUC registration schema")
        production_path.write_bytes(render_json(production_manifest))

        malformed_authority = copy.deepcopy(production_manifest)
        malformed_authority["guc_generation"]["registrations"][0][
            "authority"
        ]["unexpected_provenance"] = "rejected"
        production_path.write_bytes(render_json(malformed_authority))
        expect_failure(verify, "unknown GUC authority field")
        production_path.write_bytes(render_json(production_manifest))

        malformed_candidate_hash = copy.deepcopy(production_manifest)
        malformed_candidate_hash["guc_generation"]["registrations"][0][
            "candidate"
        ]["source_sha256"] = "not-a-sha256"
        production_path.write_bytes(render_json(malformed_candidate_hash))
        expect_failure(verify, "malformed GUC candidate source provenance")
        production_path.write_bytes(render_json(production_manifest))

        mismatched_live_flags = copy.deepcopy(production_manifest)
        mismatched_live_flags["guc_generation"]["registrations"][0][
            "generated_live_authority"
        ]["raw_flags"] = 1
        production_path.write_bytes(render_json(mismatched_live_flags))
        expect_failure(verify, "mismatched generated-live GUC flags")
        production_path.write_bytes(render_json(production_manifest))

        mismatched_candidate_hash = copy.deepcopy(production_manifest)
        mismatched_candidate_hash["guc_generation"]["registrations"][0][
            "candidate"
        ]["source_sha256"] = "6" * 64
        production_path.write_bytes(render_json(mismatched_candidate_hash))
        expect_failure(verify, "GUC candidate/entity source hash mismatch")
        production_path.write_bytes(render_json(production_manifest))

        resealed_source_hash = copy.deepcopy(production_manifest)
        resealed_source_hash["entities"][0]["source_sha256"] = "6" * 64
        resealed_source_hash["guc_generation"]["registrations"][0][
            "candidate"
        ]["source_sha256"] = "6" * 64
        production_path.write_bytes(render_json(resealed_source_hash))
        expect_failure(verify, "GUC source hash differs from repository bytes")
        production_path.write_bytes(render_json(production_manifest))

        mismatched_candidate = copy.deepcopy(production_manifest)
        mismatched_candidate["guc_generation"]["registrations"][0][
            "candidate"
        ]["owner"] = "bioseq.other"
        production_path.write_bytes(render_json(mismatched_candidate))
        expect_failure(verify, "mismatched GUC candidate owner provenance")
        production_path.write_bytes(render_json(production_manifest))

        missing_proof = copy.deepcopy(production_manifest)
        del missing_proof["guc_generation"]["profile_delta_proof"]
        production_path.write_bytes(render_json(missing_proof))
        expect_failure(verify, "missing GUC profile delta proof")
        production_path.write_bytes(render_json(production_manifest))

        swapped_registry_proof = copy.deepcopy(production_manifest)
        swapped_registry_proof["guc_generation"][
            "profile_delta_proof"
        ]["production_registry_content_sha256"] = "9" * 64
        production_path.write_bytes(render_json(swapped_registry_proof))
        expect_failure(verify, "swapped profile registry hash")
        production_path.write_bytes(render_json(production_manifest))

        swapped_owner_proof = copy.deepcopy(production_manifest)
        swapped_owner_proof["guc_generation"]["profile_delta_proof"][
            "owner_artifacts"
        ]["bioseq"]["production_content_sha256"] = "8" * 64
        production_path.write_bytes(render_json(swapped_owner_proof))
        expect_failure(verify, "swapped profile owner hash")
        production_path.write_bytes(render_json(production_manifest))

        integer_invariance = copy.deepcopy(production_manifest)
        integer_invariance["guc_generation"]["profile_delta_proof"][
            "owner_descriptor_profile_invariant"
        ]["aligner"] = 1
        production_path.write_bytes(render_json(integer_invariance))
        expect_failure(verify, "non-boolean owner invariance")
        production_path.write_bytes(render_json(production_manifest))

        shared_row_drift = copy.deepcopy(test_manifest)
        shared_row_drift["guc_generation"]["registrations"][0][
            "authority"
        ]["default_expression"] = "true"
        test_path.write_bytes(render_json(shared_row_drift))
        expect_failure(verify, "shared GUC authority drift")
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
    helper = root / "main/cmake/BioseqGccToolchain.cmake"
    launcher = root / "main/cmake/gcc_hwy_header_unit_launcher.py"
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
            "bioseq_gcc_module_source_closure/v1\n"
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
        "DOC0388 GCC-only contracts: source/negative/provenance/tamper"
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
