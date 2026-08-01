#!/usr/bin/env python3
"""Focused contract for the manifest-derived PostgreSQL ELF export map."""
import json
import platform
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
GENERATOR = ROOT / "GeneratePostgreSQLExtensionElfVersionScript.cmake"


def invoke(manifest: Path, output: Path,
           abi_exports: str = "Pg_magic_func|_PG_init|worker_main") -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["cmake", f"-DINPUT_MANIFEST={manifest}", f"-DOUTPUT={output}",
         f"-DABI_EXPORTS_PIPE={abi_exports}", "-P",
         str(GENERATOR)],
        text=True, capture_output=True, check=False)


def manifest() -> dict[str, object]:
    symbols = ["owned_symbol"]
    return {
        "manifest_version": 1,
        "c_link_symbol_inventory_count": len(symbols),
        "c_finfo_symbol_inventory_count": len(symbols),
        "external_legacy_macro_symbol_count": 1,
        "c_link_symbols": symbols,
        "c_finfo_symbols": ["pg_finfo_owned_symbol"],
        "external_legacy_macro_symbols": ["bioseq_external_handler"],
        "c_symbol_owners": [{
            "symbol": "owned_symbol",
            "finfo_symbol": "pg_finfo_owned_symbol",
        }],
    }


def assert_ninja_build_graph() -> None:
    """Exercise the production helper's map target in a minimal Linux project."""
    if platform.system() != "Linux":
        return
    if shutil.which("ninja") is None:
        raise SystemExit("Ninja is required for the Linux ELF build-graph contract")
    helper = ROOT / "AddPostgreSQLExtension.cmake"
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        source = directory / "source"
        build = directory / "build"
        modules = source / "modules"
        modules.mkdir(parents=True)
        (modules / "FindPostgreSQL.cmake").write_text(
            "set(PostgreSQL_FOUND TRUE)\n", encoding="utf-8")
        (source / "fixture.c").write_text(
            "void owned_symbol(void) {}\n", encoding="utf-8")
        (source / "CMakeLists.txt").write_text(
            f'''cmake_minimum_required(VERSION 3.20)
project(elf_version_script_graph C)
list(PREPEND CMAKE_MODULE_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/modules")
include("{helper}")
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/manifest-one.json" [=[
{{"manifest_version":1,"c_link_symbol_inventory_count":1,
"c_finfo_symbol_inventory_count":1,"external_legacy_macro_symbol_count":0,
"c_link_symbols":["owned_symbol"],"c_finfo_symbols":["pg_finfo_owned_symbol"],
"external_legacy_macro_symbols":[],"c_symbol_owners":[{{"symbol":"owned_symbol",
"finfo_symbol":"pg_finfo_owned_symbol"}}]}}
]=])
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/manifest-two.json" [=[
{{"manifest_version":1,"c_link_symbol_inventory_count":1,
"c_finfo_symbol_inventory_count":1,"external_legacy_macro_symbol_count":0,
"c_link_symbols":["owned_symbol"],"c_finfo_symbols":["pg_finfo_owned_symbol"],
"external_legacy_macro_symbols":[],"c_symbol_owners":[{{"symbol":"owned_symbol",
"finfo_symbol":"pg_finfo_owned_symbol"}}]}}
]=])
add_library(fixture_extension MODULE fixture.c)
bioseq_attach_extension_dso_closure(fixture_extension
  EXTENSION_NAME fixture
  SQL_MANIFEST "${{CMAKE_CURRENT_BINARY_DIR}}/manifest-one.json"
  CONTRACT manifest
  ABI_EXPORTS Pg_magic_func)
add_library(fixture_extension_two MODULE fixture.c)
bioseq_attach_extension_dso_closure(fixture_extension_two
  EXTENSION_NAME fixture_two
  SQL_MANIFEST "${{CMAKE_CURRENT_BINARY_DIR}}/manifest-two.json"
  CONTRACT manifest
  ABI_EXPORTS Pg_magic_func)
get_property(map_target_one TARGET fixture_extension
  PROPERTY BIOSEQ_EXTENSION_DSO_ELF_VERSION_SCRIPT_TARGET)
get_property(map_target_two TARGET fixture_extension_two
  PROPERTY BIOSEQ_EXTENSION_DSO_ELF_VERSION_SCRIPT_TARGET)
get_property(map_output_one TARGET fixture_extension
  PROPERTY BIOSEQ_EXTENSION_DSO_ELF_VERSION_SCRIPT)
get_property(map_output_two TARGET fixture_extension_two
  PROPERTY BIOSEQ_EXTENSION_DSO_ELF_VERSION_SCRIPT)
get_property(extension_dependencies TARGET fixture_extension
  PROPERTY MANUALLY_ADDED_DEPENDENCIES)
list(FIND extension_dependencies "${{map_target_one}}" map_dependency_index)
if(map_dependency_index EQUAL -1)
  message(FATAL_ERROR "fixture extension lacks its ELF map target dependency")
endif()
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/map-targets.txt"
  "${{map_target_one}}\\n${{map_output_one}}\\n${{map_target_two}}\\n${{map_output_two}}")
''', encoding="utf-8")
        configured = subprocess.run(
            ["cmake", "-G", "Ninja", "-S", str(source), "-B", str(build)],
            text=True, capture_output=True, check=False)
        if configured.returncode != 0:
            raise SystemExit(configured.stderr)
        target_lines = (build / "map-targets.txt").read_text(
            encoding="utf-8").splitlines()
        if len(target_lines) != 4:
            raise SystemExit("production helper did not expose both ELF map targets")
        map_targets = target_lines[::2]
        map_outputs = [Path(path) for path in target_lines[1::2]]
        if len(set(map_targets)) != 2 or len(set(map_outputs)) != 2:
            raise SystemExit("ELF map targets or outputs collide across extensions")
        for map_target, map_output in zip(map_targets, map_outputs):
            built = subprocess.run(
                ["cmake", "--build", str(build), "--target", map_target],
                text=True, capture_output=True, check=False)
            if built.returncode != 0:
                raise SystemExit(built.stderr)
            if not map_output.exists():
                raise SystemExit("Ninja did not generate the ELF version script target")


with tempfile.TemporaryDirectory() as temporary:
    directory = Path(temporary)
    source = directory / "manifest.json"
    output = directory / "exports.map"
    source.write_text(json.dumps(manifest()), encoding="utf-8")
    completed = invoke(source, output)
    if completed.returncode != 0:
        raise SystemExit(completed.stderr)
    contents = output.read_text(encoding="utf-8")
    for symbol in (
            "owned_symbol;", "pg_finfo_owned_symbol;",
            "bioseq_external_handler;", "pg_finfo_bioseq_external_handler;",
            "Pg_magic_func;", "_PG_init;", "worker_main;", "local:\n    *;"):
        if symbol not in contents:
            raise SystemExit(f"ELF map misses required export {symbol!r}")
    malformed = manifest()
    malformed["c_finfo_symbols"] = ["pg_finfo_wrong_symbol"]
    source.write_text(json.dumps(malformed), encoding="utf-8")
    if invoke(source, output).returncode == 0:
        raise SystemExit("ELF map accepted malformed fmgr/finfo closure")
    unsupported = manifest()
    unsupported["manifest_version"] = 2
    source.write_text(json.dumps(unsupported), encoding="utf-8")
    if invoke(source, output).returncode == 0:
        raise SystemExit("ELF map accepted an unsupported manifest version")
    decimal_version = manifest()
    decimal_version["manifest_version"] = 1.0
    source.write_text(json.dumps(decimal_version), encoding="utf-8")
    if invoke(source, output).returncode == 0:
        raise SystemExit("ELF map accepted a decimal manifest version")
    string_version = manifest()
    string_version["manifest_version"] = "1"
    source.write_text(json.dumps(string_version), encoding="utf-8")
    if invoke(source, output).returncode == 0:
        raise SystemExit("ELF map accepted a string manifest version")
    string_count = manifest()
    string_count["c_link_symbol_inventory_count"] = "1"
    source.write_text(json.dumps(string_count), encoding="utf-8")
    if invoke(source, output).returncode == 0:
        raise SystemExit("ELF map accepted a string inventory count")
    decimal_count = manifest()
    decimal_count["c_link_symbol_inventory_count"] = 1.0
    source.write_text(json.dumps(decimal_count), encoding="utf-8")
    if invoke(source, output).returncode == 0:
        raise SystemExit("ELF map accepted a decimal inventory count")
    non_string_symbol = manifest()
    non_string_symbol["c_link_symbols"] = [1]
    source.write_text(json.dumps(non_string_symbol), encoding="utf-8")
    if invoke(source, output).returncode == 0:
        raise SystemExit("ELF map accepted a non-string symbol")
    non_string_owner = manifest()
    non_string_owner["c_symbol_owners"] = [{
        "symbol": 1,
        "finfo_symbol": "pg_finfo_owned_symbol",
    }]
    source.write_text(json.dumps(non_string_owner), encoding="utf-8")
    if invoke(source, output).returncode == 0:
        raise SystemExit("ELF map accepted a non-string owner field")
    source.write_text(json.dumps(manifest()), encoding="utf-8")
    if invoke(source, output, "worker_main|worker_main").returncode == 0:
        raise SystemExit("ELF map accepted duplicate ABI exports")
    if invoke(source, output, "owned_symbol").returncode == 0:
        raise SystemExit("ELF map accepted an ABI/fmgr export overlap")

assert_ninja_build_graph()
