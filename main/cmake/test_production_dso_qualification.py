#!/usr/bin/env python3
"""Focused contracts for host-native production DSO qualification."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))
import qualify_production_extension_dsos as qualifier  # noqa: E402


class ProductionDsoQualificationTest(unittest.TestCase):
    def _fixture(
            self, root: Path, system: str, machine: str) -> tuple[
                argparse.Namespace, dict[tuple[str, ...], str]]:
        suffix = ".dylib" if system == "Darwin" else ".so"
        build = root / "build"
        build.mkdir()
        tool = root / ("otool" if system == "Darwin" else "readelf")
        c_compiler = root / "cc"
        cxx_compiler = root / "c++"
        for executable in (tool, c_compiler, cxx_compiler):
            executable.write_bytes((executable.name + "\n").encode())
            executable.chmod(0o755)
        bioseq = build / f"bioseq{suffix}"
        cache = build / f"bioseq_cache{suffix}"
        bioseq.write_bytes(b"bioseq-production-dso\n")
        cache.write_bytes(b"bioseq-cache-production-dso\n")
        cmake_cache = build / "CMakeCache.txt"
        cmake_cache.write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n"
            f"CMAKE_C_COMPILER:FILEPATH={c_compiler}\n"
            f"CMAKE_CXX_COMPILER:FILEPATH={cxx_compiler}\n",
            encoding="utf-8",
        )
        inputs = {"preset": "cache-bioseq-fixture"}
        configure_stamp = build / ".bioseq-build-mcp-configure.json"
        configure_stamp.write_text(json.dumps({
            "artifact_kind": "bioseq-build-mcp-configure-v1",
            "schema_version": 1,
            "inputs": inputs,
            "inputs_sha256": qualifier._canonical_json_sha256(inputs),
        }), encoding="utf-8")
        arguments = argparse.Namespace(
            platform=system,
            system_processor=machine,
            inspection_tool=str(tool),
            bioseq_dso=str(bioseq),
            cache_dso=str(cache),
            build_directory=str(build),
            build_config="Release",
            build_type="Release",
            generator="Ninja",
            c_compiler=str(c_compiler),
            c_compiler_id="FixtureC",
            c_compiler_version="1.2.3",
            cxx_compiler=str(cxx_compiler),
            cxx_compiler_id="FixtureCXX",
            cxx_compiler_version="4.5.6",
            cmake_cache=str(cmake_cache),
            configure_input_stamp=str(configure_stamp),
            receipt=str(build / "receipt.json"),
        )
        if system == "Darwin":
            listings = {
                ("-hv",): (
                    "Mach header\n"
                    f"MH_MAGIC_64 {machine.upper()} ALL 0x6 0 0 0 0\n"
                ),
                ("-L",): (
                    "fixture:\n"
                    "\t/usr/lib/libSystem.B.dylib "
                    "(compatibility version 1.0.0, current version 1.0.0)\n"
                ),
                ("-l",): (
                    "Load command 1\n"
                    "      cmd LC_UUID\n"
                    "     uuid 12345678-1234-1234-1234-123456789abc\n"
                ),
            }
        else:
            listings = {
                ("-h",): (
                    "ELF Header:\n"
                    "  Class:                             ELF64\n"
                    "  Data:                              2's complement, little endian\n"
                    "  Machine:                           Advanced Micro Devices X86-64\n"
                ),
                ("-d",): (
                    "Dynamic section:\n"
                    " 0x0000000000000001 (NEEDED) Shared library: [libc.so.6]\n"
                ),
                ("-n",): "    Build ID: 0123456789abcdef\n",
            }
        return arguments, listings

    @staticmethod
    def _run_with(
            arguments: argparse.Namespace,
            listings: dict[tuple[str, ...], str],
            machine: str) -> dict[str, object]:
        def inspect(_tool: Path, command: list[str]) -> str:
            return listings[(command[0],)]

        with (
            mock.patch.object(
                qualifier.host_platform, "system",
                return_value=arguments.platform),
            mock.patch.object(
                qualifier.host_platform, "machine", return_value=machine),
            mock.patch.object(
                qualifier.host_platform, "release", return_value="fixture-release"),
            mock.patch.object(qualifier, "_run_tool", side_effect=inspect),
        ):
            return qualifier.qualify(arguments)

    def test_darwin_receipt_seals_actual_architecture_and_context(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            arguments, listings = self._fixture(
                Path(tmp), "Darwin", "arm64")
            receipt = self._run_with(arguments, listings, "arm64")
        self.assertEqual(
            receipt["artifact_kind"],
            "bioseq-production-dso-load-qualification",
        )
        self.assertEqual(receipt["platform"], {
            "system_name": "Darwin",
            "system_release": "fixture-release",
            "machine": "arm64",
            "binary_format": "Mach-O",
            "dso_suffix": ".dylib",
        })
        self.assertEqual(
            receipt["inspection_tool"]["identity_kind"],
            "executable-sha256",
        )
        self.assertEqual(
            receipt["build_context"]["preset"],
            "cache-bioseq-fixture",
        )
        self.assertEqual(
            {row["extension"] for row in receipt["dsos"]},
            {"bioseq", "bioseq_cache"},
        )
        unsealed = dict(receipt)
        seal = unsealed.pop("content_sha256")
        self.assertEqual(seal, qualifier._canonical_json_sha256(unsealed))

    def test_linux_receipt_records_elf_machine_and_build_id(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            arguments, listings = self._fixture(
                Path(tmp), "Linux", "x86_64")
            receipt = self._run_with(arguments, listings, "AMD64")
        self.assertEqual(receipt["platform"]["binary_format"], "ELF")
        for row in receipt["dsos"]:
            self.assertEqual(row["binary"]["architecture"], "x86_64")
            self.assertEqual(
                row["build_identity"]["kind"], "elf-gnu-build-id")

    def test_actual_binary_architecture_mismatch_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            arguments, listings = self._fixture(
                Path(tmp), "Darwin", "arm64")
            listings[("-hv",)] = (
                "Mach header\nMH_MAGIC_64 X86_64 ALL 0x6 0 0 0 0\n")
            with self.assertRaisesRegex(
                    qualifier.QualificationError, "differs from native"):
                self._run_with(arguments, listings, "arm64")

    def test_direct_sibling_dependency_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            arguments, listings = self._fixture(
                Path(tmp), "Linux", "x86_64")
            listings[("-d",)] += (
                " 0x0000000000000001 (NEEDED) "
                "Shared library: [bioseq_cache.so]\n")
            with self.assertRaisesRegex(
                    qualifier.QualificationError, "directly depends"):
                self._run_with(arguments, listings, "x86_64")

    def test_configure_input_tamper_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            arguments, listings = self._fixture(
                Path(tmp), "Linux", "x86_64")
            stamp = Path(arguments.configure_input_stamp)
            value = json.loads(stamp.read_text(encoding="utf-8"))
            value["inputs"]["preset"] = "tampered"
            stamp.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(
                    qualifier.QualificationError, "invalid seal"):
                self._run_with(arguments, listings, "x86_64")

    def test_atomic_writer_replaces_regular_receipt_without_partial_file(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "nested" / "receipt.json"
            qualifier._write_json_atomic(output, {"value": 1})
            qualifier._write_json_atomic(output, {"value": 2})
            self.assertEqual(
                json.loads(output.read_text(encoding="utf-8")), {"value": 2})
            self.assertEqual(list(output.parent.glob("*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
