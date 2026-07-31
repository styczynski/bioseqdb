#!/usr/bin/env python3
"""Focused contract for the manifest-derived PostgreSQL ELF export map."""
import json
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
