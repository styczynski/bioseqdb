#!/usr/bin/env python3
"""Focused contracts for the manifest-derived PostgreSQL Mach-O allowlist."""

from __future__ import annotations

import json
import platform
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
GENERATOR = ROOT / "GeneratePostgreSQLExtensionMachOExportList.cmake"
HELPER = ROOT / "AddPostgreSQLExtension.cmake"


def manifest() -> dict[str, object]:
    return {
        "manifest_version": 1,
        "c_routine_identity_count": 1,
        "c_link_symbol_inventory_count": 1,
        "c_finfo_symbol_inventory_count": 1,
        "c_wrapper_entity_count": 1,
        "retired_c_symbol_count": 0,
        "generated_wrapper_symbol_count": 1,
        "legacy_macro_symbol_count": 0,
        "external_legacy_macro_symbol_count": 1,
        "legacy_fmgr_macro_owner_count": 1,
        "c_symbol_sharing_group_count": 0,
        "c_routine_identities": ["fixture.owned_symbol(text)"],
        "c_link_symbols": ["owned_symbol"],
        "c_finfo_symbols": ["pg_finfo_owned_symbol"],
        "c_symbol_owners": [{
            "symbol": "owned_symbol",
            "finfo_symbol": "pg_finfo_owned_symbol",
            "generated_wrapper": True,
            "routine_identities": ["fixture.owned_symbol(text)"],
            "implementation_declarations": ["fixture::owned_symbol"],
        }],
        "retired_c_symbols": [],
        "generated_wrapper_symbols": ["owned_symbol"],
        "legacy_macro_symbols": [],
        "external_legacy_macro_symbols": ["external_handler"],
        "legacy_fmgr_macro_owners": [{
            "symbol": "external_handler",
            "macro": "CXX_PG_FUNCTION_INFO_V1",
            "source_module": "fixture.external",
            "source_path": "fixture/external.cppm",
            "source_line": 1,
        }],
    }


def invoke(
    source: Path,
    output: Path,
    abi_exports: str = "Pg_magic_func|_PG_init",
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "cmake",
            f"-DINPUT_MANIFEST={source}",
            f"-DOUTPUT={output}",
            f"-DABI_EXPORTS_PIPE={abi_exports}",
            "-P",
            str(GENERATOR),
        ],
        text=True,
        capture_output=True,
        check=False,
    )


def assert_generator_contract() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        source = directory / "manifest.json"
        output = directory / "exports.list"
        source.write_text(json.dumps(manifest()), encoding="utf-8")
        completed = invoke(source, output)
        if completed.returncode != 0:
            raise SystemExit(completed.stderr)
        expected = sorted({
            "_Pg_magic_func",
            "__PG_init",
            "_external_handler",
            "_owned_symbol",
            "_pg_finfo_external_handler",
            "_pg_finfo_owned_symbol",
        })
        actual = output.read_text(encoding="utf-8").splitlines()
        if actual != expected:
            raise SystemExit(
                f"Mach-O export list differs: expected={expected}, actual={actual}")
        if any("*" in symbol for symbol in actual):
            raise SystemExit("Mach-O exact export list must not contain wildcards")

        malformed = manifest()
        malformed["c_link_symbols"] = ["bad-symbol"]
        source.write_text(json.dumps(malformed), encoding="utf-8")
        if invoke(source, output).returncode == 0:
            raise SystemExit("Mach-O export list accepted a malformed symbol")

        source.write_text(json.dumps(manifest()), encoding="utf-8")
        if invoke(
            source, output, "Pg_magic_func|Pg_magic_func"
        ).returncode == 0:
            raise SystemExit("Mach-O export list accepted duplicate ABI exports")
        if invoke(source, output, "owned_symbol").returncode == 0:
            raise SystemExit("Mach-O export list accepted ABI/fmgr overlap")


def assert_darwin_ninja_build_graph() -> None:
    if platform.system() != "Darwin":
        return
    if shutil.which("ninja") is None:
        raise SystemExit("Ninja is required for the Mach-O build-graph contract")
    nm = shutil.which("nm")
    if nm is None:
        raise SystemExit("nm is required for the Mach-O exact-export contract")
    helper_text = HELPER.read_text(encoding="utf-8")
    if "-unexported_symbols_list" in helper_text:
        raise SystemExit("production helper still uses the obsolete Mach-O denylist")

    with tempfile.TemporaryDirectory() as temporary:
        # macOS exposes /var as a symlink to /private/var.  The production
        # evidence validator intentionally rejects symlinked path components,
        # so configure the fixture through its canonical physical path.
        directory = Path(temporary).resolve()
        source = directory / "source"
        build = directory / "build"
        modules = source / "modules"
        modules.mkdir(parents=True)
        (modules / "FindPostgreSQL.cmake").write_text(
            "set(PostgreSQL_FOUND TRUE)\n", encoding="utf-8")
        (source / "fixture.c").write_text(
            "void owned_symbol(void) {}\n"
            "void pg_finfo_owned_symbol(void) {}\n"
            "void external_handler(void) {}\n"
            "void pg_finfo_external_handler(void) {}\n"
            "void Pg_magic_func(void) {}\n"
            "void _PG_init(void) {}\n"
            "void unreviewed_global(void) {}\n",
            encoding="utf-8",
        )
        fixture_manifest = json.dumps(
            manifest(), sort_keys=True, separators=(",", ":"))
        (source / "CMakeLists.txt").write_text(
            f'''cmake_minimum_required(VERSION 3.20)
project(macho_export_list_graph C)
list(PREPEND CMAKE_MODULE_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/modules")
include("{HELPER}")
set(fixture_manifest [==[{fixture_manifest}]==])
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/manifest-one.json"
  "${{fixture_manifest}}")
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/manifest-two.json"
  "${{fixture_manifest}}")
add_library(fixture_extension MODULE fixture.c)
bioseq_attach_extension_dso_closure(fixture_extension
  EXTENSION_NAME fixture
  SQL_MANIFEST "${{CMAKE_CURRENT_BINARY_DIR}}/manifest-one.json"
  REPORT_PATH "${{CMAKE_CURRENT_BINARY_DIR}}/fixture-one-dso-report.json"
  CONTRACT manifest
  ABI_EXPORTS Pg_magic_func _PG_init)
add_library(fixture_extension_two MODULE fixture.c)
bioseq_attach_extension_dso_closure(fixture_extension_two
  EXTENSION_NAME fixture_two
  SQL_MANIFEST "${{CMAKE_CURRENT_BINARY_DIR}}/manifest-two.json"
  REPORT_PATH "${{CMAKE_CURRENT_BINARY_DIR}}/fixture-two-dso-report.json"
  CONTRACT manifest
  ABI_EXPORTS Pg_magic_func _PG_init)
get_property(map_target_one TARGET fixture_extension
  PROPERTY BIOSEQ_EXTENSION_DSO_MACHO_EXPORT_LIST_TARGET)
get_property(map_target_two TARGET fixture_extension_two
  PROPERTY BIOSEQ_EXTENSION_DSO_MACHO_EXPORT_LIST_TARGET)
get_property(map_output_one TARGET fixture_extension
  PROPERTY BIOSEQ_EXTENSION_DSO_MACHO_EXPORT_LIST)
get_property(map_output_two TARGET fixture_extension_two
  PROPERTY BIOSEQ_EXTENSION_DSO_MACHO_EXPORT_LIST)
get_property(extension_dependencies TARGET fixture_extension
  PROPERTY MANUALLY_ADDED_DEPENDENCIES)
list(FIND extension_dependencies "${{map_target_one}}" map_dependency_index)
if(map_dependency_index EQUAL -1)
  message(FATAL_ERROR "fixture extension lacks its Mach-O map dependency")
endif()
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/map-targets.txt"
  "${{map_target_one}}\n${{map_output_one}}\n${{map_target_two}}\n${{map_output_two}}")
file(GENERATE OUTPUT "${{CMAKE_CURRENT_BINARY_DIR}}/dso-paths.txt"
  CONTENT "$<TARGET_FILE:fixture_extension>\n$<TARGET_FILE:fixture_extension_two>\n")
''',
            encoding="utf-8",
        )
        configured = subprocess.run(
            ["cmake", "-G", "Ninja", "-S", str(source), "-B", str(build)],
            text=True,
            capture_output=True,
            check=False,
        )
        if configured.returncode != 0:
            raise SystemExit(configured.stderr)
        target_lines = (build / "map-targets.txt").read_text(
            encoding="utf-8").splitlines()
        if len(target_lines) != 4:
            raise SystemExit("production helper did not expose both Mach-O maps")
        map_targets = target_lines[::2]
        map_outputs = [Path(path) for path in target_lines[1::2]]
        if len(set(map_targets)) != 2 or len(set(map_outputs)) != 2:
            raise SystemExit("Mach-O map targets or outputs collide")

        built = subprocess.run(
            [
                "cmake",
                "--build",
                str(build),
                "--target",
                "fixture_extension",
                "fixture_extension_two",
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        if built.returncode != 0:
            raise SystemExit(built.stdout + built.stderr)
        for map_output in map_outputs:
            if not map_output.is_file():
                raise SystemExit("Ninja did not generate a Mach-O export list")

        expected_exports = {
            "_Pg_magic_func",
            "__PG_init",
            "_external_handler",
            "_owned_symbol",
            "_pg_finfo_external_handler",
            "_pg_finfo_owned_symbol",
        }
        dso_paths = (build / "dso-paths.txt").read_text(
            encoding="utf-8").splitlines()
        if len(dso_paths) != 2:
            raise SystemExit("fixture did not publish both target paths")
        for dso_path in dso_paths:
            inspected = subprocess.run(
                [nm, "-gUj", dso_path],
                text=True,
                capture_output=True,
                check=False,
            )
            if inspected.returncode != 0:
                raise SystemExit(inspected.stderr)
            actual_exports = {
                row.strip() for row in inspected.stdout.splitlines() if row.strip()
            }
            if actual_exports != expected_exports:
                raise SystemExit(
                    f"{Path(dso_path).name} exports {sorted(actual_exports)}, "
                    f"expected {sorted(expected_exports)}")
            if "_unreviewed_global" in actual_exports:
                raise SystemExit("Mach-O linker leaked the intentional extra global")


assert_generator_contract()
assert_darwin_ninja_build_graph()
