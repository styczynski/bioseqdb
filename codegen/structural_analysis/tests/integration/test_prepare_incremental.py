#!/usr/bin/env python3
"""Bounded Ninja regression for PREPARE depfiles and content seals."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time
from typing import Any


CODEGEN_TIMEOUT_SECONDS = 120
NINJA_STEP_TIMEOUT_SECONDS = 180
PCM_MANIFEST_SCHEMA = "bioseq_clang_pcm_manifest"
PCM_MANIFEST_SCHEMA_REVISION = 1
PCM_MANIFEST_KEYS = frozenset({
    "schema",
    "schema_revision",
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
})


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _write_if_different(path: Path, content: bytes) -> None:
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(content)
    os.replace(temporary, path)


def _append_counter(config: dict[str, Any], operation: str) -> None:
    with Path(config["counter"]).open("a", encoding="utf-8") as output:
        output.write(operation + "\n")


def _worker_prepare(config: dict[str, Any]) -> int:
    _append_counter(config, "prepare")
    command = [
        config["codegen"],
        "sql",
        "prepare",
        "--authority",
        "support",
        "--profile",
        "production",
        "--source",
        config["source"],
        "--raw-compile-database",
        config["raw_database"],
        "--clang-binary",
        config["clang_binary"],
        "--clang-scan-deps",
        config["clang_scan_deps"],
        "--llvm-config",
        config["llvm_config"],
        "--compiler-module-inventory",
        config["compiler_inventory"],
        "--clang-sha256",
        config["clang_sha256"],
        "--clang-scan-deps-sha256",
        config["clang_scan_deps_sha256"],
        "--llvm-config-sha256",
        config["llvm_config_sha256"],
        "--compiler-module-inventory-sha256",
        config["compiler_inventory_sha256"],
        "--compile-database-output",
        config["owned_database"],
        "--pcm-root",
        config["pcm_root"],
        "--pcm-manifest",
        config["manifest"],
        "--depfile",
        config["depfile"],
        "--stamp",
        config["prepare_stamp"],
    ]
    result = subprocess.run(
        command,
        cwd=config["root"],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=CODEGEN_TIMEOUT_SECONDS,
    )
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    return result.returncode


def _stamp_fields(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key] = value
    return result


def _worker_verify(config: dict[str, Any]) -> int:
    _append_counter(config, "verify")
    manifest_path = Path(config["manifest"])
    owned_database = Path(config["owned_database"])
    prepare_stamp = Path(config["prepare_stamp"])
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    stamp = _stamp_fields(prepare_stamp)
    if not isinstance(manifest, dict) or set(manifest) != PCM_MANIFEST_KEYS:
        raise RuntimeError("incremental fixture received an invalid manifest root")
    if (
        manifest.get("schema") != PCM_MANIFEST_SCHEMA
        or type(manifest.get("schema_revision")) is not int
        or manifest["schema_revision"] != PCM_MANIFEST_SCHEMA_REVISION
    ):
        raise RuntimeError("incremental fixture received an invalid manifest schema")
    if stamp.get("pcm_manifest_sha256") != _sha256(manifest_path):
        raise RuntimeError("incremental fixture detected a stale PCM manifest")
    if stamp.get("compile_database_sha256") != _sha256(owned_database):
        raise RuntimeError("incremental fixture detected a stale compile database")
    modules = manifest.get("modules")
    if not isinstance(modules, list) or not modules:
        raise RuntimeError("incremental fixture received an empty module closure")
    sealed_paths = {
        "source": "source_sha256",
        "depfile": "depfile_sha256",
        "pcm": "pcm_sha256",
        "pcm_provenance": "pcm_provenance_sha256",
    }
    for module in modules:
        if not isinstance(module, dict):
            raise RuntimeError("incremental fixture received a malformed module")
        for path_field, hash_field in sealed_paths.items():
            artifact = Path(module[path_field])
            if not artifact.is_file() or _sha256(artifact) != module[hash_field]:
                raise RuntimeError(
                    f"incremental fixture detected stale {path_field}"
                )
    verification = (
        "bioseq_prepare_incremental_verification\n"
        "revision=1\n"
        f"prepare_stamp_sha256={_sha256(prepare_stamp)}\n"
        f"pcm_manifest_sha256={_sha256(manifest_path)}\n"
        f"compile_database_sha256={_sha256(owned_database)}\n"
    ).encode()
    verification_stamp = Path(config["verification_stamp"])
    _write_if_different(verification_stamp, verification)
    verification_stamp.touch()
    return 0


def _ninja_path(path: Path) -> str:
    return (
        str(path)
        .replace("$", "$$")
        .replace(" ", "$ ")
        .replace(":", "$:")
    )


def _command_argument(value: str) -> str:
    return shlex.quote(value).replace("$", "$$")


def _counter_counts(counter: Path) -> tuple[int, int]:
    if not counter.exists():
        return (0, 0)
    operations = counter.read_text(encoding="utf-8").splitlines()
    return (
        operations.count("prepare"),
        operations.count("verify"),
    )


def _require_counts(
    counter: Path,
    expected: tuple[int, int],
    context: str,
    diagnostic: str = "",
) -> None:
    actual = _counter_counts(counter)
    if actual != expected:
        details = f"\nNinja diagnostics:\n{diagnostic}" if diagnostic else ""
        raise RuntimeError(
            f"{context}: expected prepare/verify counts {expected}, got {actual}"
            f"{details}"
        )


def _run_ninja(
    config: dict[str, Any],
    expect_success: bool,
    expected_diagnostic: str | None = None,
) -> str:
    result = subprocess.run(
        [
            config["ninja"],
            "-d",
            "explain",
            "-f",
            config["build_file"],
            "-C",
            config["root"],
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=NINJA_STEP_TIMEOUT_SECONDS,
    )
    if (result.returncode == 0) != expect_success:
        expectation = "success" if expect_success else "failure"
        raise RuntimeError(
            f"incremental Ninja fixture expected {expectation}, "
            f"got exit {result.returncode}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    if (
        not expect_success
        and expected_diagnostic is not None
        and expected_diagnostic not in result.stdout + result.stderr
    ):
        raise RuntimeError(
            "incremental Ninja fixture received the wrong failure; "
            f"expected diagnostic containing {expected_diagnostic!r}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    return result.stdout + result.stderr


def _mtime_snapshot(paths: list[Path]) -> str:
    rows = ["mtime snapshot (nanoseconds):"]
    for path in paths:
        if path.exists():
            rows.append(f"  {path}: {path.stat().st_mtime_ns}")
        else:
            rows.append(f"  {path}: missing")
    return "\n".join(rows)


def _touch_newer(path: Path, reference: Path) -> None:
    for _ in range(500):
        path.touch()
        if path.stat().st_mtime_ns > reference.stat().st_mtime_ns:
            return
        time.sleep(0.01)
    raise RuntimeError(f"cannot make {path} newer than {reference}")


def _run_fixture(arguments: argparse.Namespace) -> int:
    with tempfile.TemporaryDirectory(
        prefix="bioseq-prepare-ninja-"
    ) as temporary:
        root = Path(temporary).resolve()
        source = root / "fixture.cppm"
        raw_database = root / "raw-compile-commands.json"
        owned_database = root / "owned-compile-commands.json"
        pcm_root = root / "pcm"
        manifest = root / "pcm-manifest.json"
        depfile = root / "prepare.d"
        prepare_stamp = root / "prepare.stamp"
        verification_stamp = root / "verify.stamp"
        counter = root / "counter.log"
        config_path = root / "config.json"
        build_file = root / "build.ninja"
        source.write_text(
            "export module fixture.incremental;\n"
            "export int incremental_value();\n",
            encoding="utf-8",
        )
        raw_database.write_text(
            json.dumps(
                [
                    {
                        "directory": str(root),
                        "file": str(source),
                        "arguments": [
                            arguments.clang_binary,
                            "-std=c++26",
                            "-x",
                            "c++-module",
                            str(source),
                        ],
                    }
                ],
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        config = {
            "root": str(root),
            "codegen": str(Path(arguments.codegen).resolve()),
            "ninja": str(Path(arguments.ninja).resolve()),
            "source": str(source),
            "raw_database": str(raw_database),
            "owned_database": str(owned_database),
            "pcm_root": str(pcm_root),
            "manifest": str(manifest),
            "depfile": str(depfile),
            "prepare_stamp": str(prepare_stamp),
            "verification_stamp": str(verification_stamp),
            "counter": str(counter),
            "build_file": str(build_file),
            "clang_binary": str(Path(arguments.clang_binary).resolve()),
            "clang_scan_deps": str(Path(arguments.clang_scan_deps).resolve()),
            "llvm_config": str(Path(arguments.llvm_config).resolve()),
            "compiler_inventory": str(
                Path(arguments.compiler_inventory).resolve()
            ),
            "clang_sha256": arguments.clang_sha256,
            "clang_scan_deps_sha256": arguments.clang_scan_deps_sha256,
            "llvm_config_sha256": arguments.llvm_config_sha256,
            "compiler_inventory_sha256": (
                arguments.compiler_inventory_sha256
            ),
        }
        config_path.write_text(
            json.dumps(config, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        worker_prefix = (
            f"{_command_argument(sys.executable)} "
            f"{_command_argument(str(Path(__file__).resolve()))}"
        )
        build_file.write_text(
            "ninja_required_version = 1.10\n\n"
            "rule prepare\n"
            f"  command = {worker_prefix} worker-prepare "
            f"--config {_command_argument(str(config_path))}\n"
            f"  depfile = {_ninja_path(depfile)}\n"
            "  deps = gcc\n"
            "  restat = 1\n\n"
            "rule verify\n"
            f"  command = {worker_prefix} worker-verify "
            f"--config {_command_argument(str(config_path))}\n"
            "  restat = 1\n\n"
            f"build {_ninja_path(prepare_stamp)}"
            f" | {_ninja_path(owned_database)}"
            f" {_ninja_path(manifest)}"
            f": prepare"
            f" {_ninja_path(source)}"
            f" {_ninja_path(raw_database)}"
            f" {_ninja_path(Path(config['codegen']))}"
            f" {_ninja_path(Path(config['clang_binary']))}"
            f" {_ninja_path(Path(config['clang_scan_deps']))}"
            f" {_ninja_path(Path(config['llvm_config']))}"
            f" {_ninja_path(Path(config['compiler_inventory']))}\n"
            f"build {_ninja_path(verification_stamp)}: verify"
            f" {_ninja_path(prepare_stamp)}"
            f" {_ninja_path(manifest)}"
            f" {_ninja_path(owned_database)}\n\n"
            f"default {_ninja_path(verification_stamp)}\n",
            encoding="utf-8",
        )

        _run_ninja(config, expect_success=True)
        _require_counts(counter, (1, 1), "initial build")
        baseline_manifest = manifest.read_bytes()
        baseline_database = owned_database.read_bytes()
        manifest_document = json.loads(baseline_manifest)
        pcm = Path(manifest_document["modules"][0]["pcm"])
        baseline_pcm = pcm.read_bytes()
        ninja_state_paths = [
            source,
            pcm,
            owned_database,
            manifest,
            depfile,
            prepare_stamp,
            verification_stamp,
        ]

        first_noop_explain = _run_ninja(config, expect_success=True)
        _require_counts(
            counter,
            (1, 1),
            "first no-op build",
            first_noop_explain
            + "\n"
            + _mtime_snapshot(ninja_state_paths),
        )

        pcm.unlink()
        _run_ninja(config, expect_success=True)
        _require_counts(counter, (2, 2), "deleted PCM rebuild")
        if (
            manifest.read_bytes() != baseline_manifest
            or owned_database.read_bytes() != baseline_database
            or pcm.read_bytes() != baseline_pcm
        ):
            raise RuntimeError(
                "deleted PCM rebuild changed deterministic PREPARE bytes"
            )

        pcm.write_bytes(baseline_pcm + b"corrupt-nonzero-pcm")
        _run_ninja(
            config,
            expect_success=False,
            expected_diagnostic="PCM is missing, foreign, or corrupt",
        )
        _require_counts(counter, (3, 2), "corrupt PCM failure")
        pcm.write_bytes(baseline_pcm)
        _run_ninja(config, expect_success=True)
        _require_counts(counter, (4, 3), "corrupt PCM recovery")

        manifest.write_bytes(baseline_manifest + b"\n")
        _run_ninja(
            config,
            expect_success=False,
            expected_diagnostic=(
                "incremental fixture detected a stale PCM manifest"
            ),
        )
        _require_counts(counter, (4, 4), "edited manifest failure")
        manifest.write_bytes(baseline_manifest)
        _run_ninja(config, expect_success=True)
        _require_counts(counter, (4, 5), "edited manifest recovery")

        _touch_newer(source, prepare_stamp)
        _run_ninja(config, expect_success=True)
        _require_counts(counter, (5, 6), "content-equivalent source touch")
        if (
            manifest.read_bytes() != baseline_manifest
            or owned_database.read_bytes() != baseline_database
            or pcm.read_bytes() != baseline_pcm
        ):
            raise RuntimeError(
                "content-equivalent source touch changed PREPARE bytes"
            )

        first_final_noop_explain = _run_ninja(config, expect_success=True)
        second_final_noop_explain = _run_ninja(config, expect_success=True)
        _require_counts(
            counter,
            (5, 6),
            "two consecutive no-op builds",
            "first invocation:\n"
            + first_final_noop_explain
            + "\nsecond invocation:\n"
            + second_final_noop_explain
            + "\n"
            + _mtime_snapshot(ninja_state_paths),
        )
    return 0


def _load_config(path: str) -> dict[str, Any]:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="operation", required=True)
    run = subparsers.add_parser("run")
    run.add_argument("--codegen", required=True)
    run.add_argument("--ninja", required=True)
    run.add_argument("--clang-binary", required=True)
    run.add_argument("--clang-scan-deps", required=True)
    run.add_argument("--llvm-config", required=True)
    run.add_argument("--compiler-inventory", required=True)
    run.add_argument("--clang-sha256", required=True)
    run.add_argument("--clang-scan-deps-sha256", required=True)
    run.add_argument("--llvm-config-sha256", required=True)
    run.add_argument("--compiler-inventory-sha256", required=True)
    for operation in ("worker-prepare", "worker-verify"):
        worker = subparsers.add_parser(operation)
        worker.add_argument("--config", required=True)
    return parser


def main() -> int:
    arguments = _parser().parse_args()
    try:
        if arguments.operation == "run":
            return _run_fixture(arguments)
        config = _load_config(arguments.config)
        if arguments.operation == "worker-prepare":
            return _worker_prepare(config)
        return _worker_verify(config)
    except Exception as error:  # noqa: BLE001 - self-test diagnostic boundary
        print(f"bioseq PREPARE incremental fixture: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
