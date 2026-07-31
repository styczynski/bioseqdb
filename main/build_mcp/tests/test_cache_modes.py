from __future__ import annotations

import contextlib
import csv
import io
import json
import multiprocessing
import queue
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
MAIN_DIR = BUILD_MCP_DIR.parent
sys.path.insert(0, str(BUILD_MCP_DIR))

import inspect_tool  # noqa: E402
import server  # noqa: E402


LOCAL = ("bench-opt", "bench-opt-debug")
AWS_CLANG = ("aws-x64-linux", "aws-x64-linux-debug")
AWS_GCC = ("aws-x64-linux-gcc161", "aws-x64-linux-gcc161-debug")

EXPECTED_CACHE_NATIVE_ARTIFACT_KEYS = frozenset({
    "cache_test_support_postgres_bin_dir",
    "cache_test_support_postgres_stage_root",
    "cache_test_support_postgres_stage_manifest",
    "cache_production_postgres_bin_dir",
    "cache_production_postgres_stage_root",
    "cache_production_postgres_stage_manifest",
    "cache_bioseq_support_postgres_bin_dir",
    "cache_bioseq_support_postgres_stage_root",
    "cache_bioseq_support_postgres_stage_manifest",
    "cache_bioseq_only_postgres_bin_dir",
    "cache_bioseq_only_postgres_stage_root",
    "cache_bioseq_only_postgres_stage_manifest",
    "cache_bioseq_production_postgres_bin_dir",
    "cache_bioseq_production_postgres_stage_root",
    "cache_bioseq_production_postgres_stage_manifest",
    "cache_control",
    "cache_install_sql",
    "cache_dso",
    "cache_test_control",
    "cache_test_install_sql",
    "cache_test_dso",
    "bioseq_control",
    "bioseq_install_sql",
    "bioseq_dso",
    "provider_peer_test_dso",
    "migration_ledger",
    "legacy_inventory",
    "migration_catch_cases",
    "legacy_retirement_gate",
})


class CachePresetContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        payload = json.loads((MAIN_DIR / "CMakePresets.json").read_text())
        cls.presets = {
            preset["name"]: preset
            for preset in payload["configurePresets"]
        }

    def test_inherited_cache_presets_preserve_toolchain_families(self) -> None:
        expected = {
            "cache-opt": ("bench-opt", False, False),
            "cache-opt-debug": ("bench-opt-debug", False, False),
            "cache-bioseq-opt": ("bench-opt", True, False),
            "cache-bioseq-opt-debug": ("bench-opt-debug", True, False),
            "aws-x64-linux-cache": ("aws-x64-linux", False, True),
            "aws-x64-linux-cache-debug": (
                "aws-x64-linux-debug", False, True),
            "aws-x64-linux-cache-bioseq": (
                "aws-x64-linux", True, True),
            "aws-x64-linux-cache-bioseq-debug": (
                "aws-x64-linux-debug", True, True),
            "aws-x64-linux-gcc161-cache": (
                "aws-x64-linux-gcc161", False, True),
            "aws-x64-linux-gcc161-cache-debug": (
                "aws-x64-linux-gcc161-debug", False, True),
            "aws-x64-linux-gcc161-cache-bioseq": (
                "aws-x64-linux-gcc161", True, True),
            "aws-x64-linux-gcc161-cache-bioseq-debug": (
                "aws-x64-linux-gcc161-debug", True, True),
        }

        for name, (parent, combined, _aws) in expected.items():
            with self.subTest(name=name):
                preset = self.presets[name]
                variables = preset["cacheVariables"]
                self.assertEqual(preset["inherits"], parent)
                self.assertEqual(
                    preset["binaryDir"], "${sourceDir}/build/${presetName}")
                self.assertEqual(
                    variables["BIOSEQ_BUILD_CORE"],
                    "ON" if combined else "OFF",
                )
                self.assertEqual(variables["BIOSEQ_BUILD_CACHE"], "ON")
                self.assertEqual(variables["BIOSEQ_BUILD_PAPERS"], "OFF")
                self.assertEqual(variables["BIOSEQ_CACHE_BUILD_TESTS"], "ON")
                self.assertEqual(variables["BIOSEQ_CACHE_TESTING"], "ON")
                if combined:
                    self.assertNotIn("VCPKG_MANIFEST_DIR", variables)
                else:
                    self.assertEqual(
                        variables["VCPKG_MANIFEST_DIR"],
                        "${sourceDir}/bioseq_caching",
                    )


class CacheModeResolutionTest(unittest.TestCase):
    def _patch_toolchain(self, fast: str, debug: str):
        return mock.patch.multiple(
            server,
            DEFAULT_BUILD_PRESET=fast,
            DEBUG_BUILD_PRESET=debug,
            TEST_BUILD_PRESET=server._default_test_preset(fast),
            TEST_DEBUG_BUILD_PRESET=server._default_test_preset(debug),
            CACHE_BUILD_PRESET_OVERRIDE=None,
            CACHE_DEBUG_BUILD_PRESET_OVERRIDE=None,
            CACHE_BIOSEQ_BUILD_PRESET_OVERRIDE=None,
            CACHE_BIOSEQ_DEBUG_BUILD_PRESET_OVERRIDE=None,
        )

    def test_all_modes_resolve_for_local_aws_clang_and_aws_gcc(self) -> None:
        for fast, debug_preset in (LOCAL, AWS_CLANG, AWS_GCC):
            with self.subTest(toolchain=fast), self._patch_toolchain(
                    fast, debug_preset):
                for mode, details in server.MODES.items():
                    for debug in (False, True):
                        with self.subTest(mode=mode, debug=debug):
                            core_preset = debug_preset if debug else fast
                            if details.preset_override:
                                expected = details.preset_override
                            elif details.preset_family == "core":
                                expected = core_preset
                            elif details.preset_family == "test":
                                expected = server._default_test_preset(core_preset)
                            elif details.preset_family == "cache":
                                expected = server._default_cache_preset(
                                    core_preset, combined=False)
                            elif details.preset_family == "cache-bioseq":
                                expected = server._default_cache_preset(
                                    core_preset, combined=True)
                            elif details.preset_family == "website":
                                expected = (
                                    server.WEBSITE_DEBUG_BUILD_PRESET
                                    if debug else server.WEBSITE_BUILD_PRESET)
                            else:
                                self.fail(
                                    "unhandled preset family "
                                    f"{details.preset_family!r}")

                            self.assertEqual(
                                server.preset_for(mode, debug), expected)
                            self.assertEqual(
                                server.build_dir_for(mode, debug),
                                server.PROJECT_ROOT / "build" / expected,
                            )
                            self.assertEqual(
                                server.bin_dir_for(mode, debug),
                                server.PROJECT_ROOT / "build" / expected
                                / details.binary_subdir,
                            )

    def test_cache_specific_environment_overrides_win(self) -> None:
        with mock.patch.multiple(
            server,
            CACHE_BUILD_PRESET_OVERRIDE="custom-cache-fast",
            CACHE_DEBUG_BUILD_PRESET_OVERRIDE="custom-cache-debug",
            CACHE_BIOSEQ_BUILD_PRESET_OVERRIDE="custom-combined-fast",
            CACHE_BIOSEQ_DEBUG_BUILD_PRESET_OVERRIDE="custom-combined-debug",
        ):
            self.assertEqual(
                server.preset_for("cache-unit", False), "custom-cache-fast")
            self.assertEqual(
                server.preset_for("cache-unit", True), "custom-cache-debug")
            self.assertEqual(
                server.preset_for("cache-bioseq-integration", False),
                "custom-combined-fast",
            )
            self.assertEqual(
                server.preset_for("cache-bioseq-integration", True),
                "custom-combined-debug",
            )

    def test_custom_preset_fallback_keeps_debug_suffix_last(self) -> None:
        self.assertEqual(
            server._default_cache_preset("toolchain-debug", combined=False),
            "toolchain-cache-debug",
        )
        self.assertEqual(
            server._default_cache_preset("toolchain-debug", combined=True),
            "toolchain-cache-bioseq-debug",
        )


class CacheModeRoutingTest(unittest.TestCase):
    def test_legacy_python_benchmark_inventory_is_complete_and_checked(
            self) -> None:
        inventory_path = (
            MAIN_DIR
            / "bioseq_caching"
            / "bench"
            / "legacy_python_behavior_inventory.tsv"
        )
        with inventory_path.open(encoding="utf-8", newline="") as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
        self.assertGreaterEqual(len(rows), 100)
        self.assertEqual(
            set(rows[0]),
            {
                "legacy_asset_sha256",
                "legacy_symbol",
                "legacy_behavior",
                "disposition",
                "native_evidence",
            },
        )
        expected_hashes = {
            "b7f8378f3f3cdfa4a0ef88b20ba34273587f1a7ddced698ce7ef9cc8c19e659b",
            "2f255c8f83d7782d3c1aaf8d7d4025eeb8b5c35ff1b4ce50b855b0b9b3627585",
            "1d3045a0b41a5b42f851a3db9fe0f7518aef2f8d6325da648a92a096e714b472",
        }
        self.assertEqual(
            {row["legacy_asset_sha256"] for row in rows},
            expected_hashes,
        )
        identities = [
            (row["legacy_asset_sha256"], row["legacy_symbol"])
            for row in rows
        ]
        self.assertEqual(len(identities), len(set(identities)))
        for row in rows:
            with self.subTest(symbol=row["legacy_symbol"]):
                self.assertTrue(row["legacy_behavior"])
                self.assertRegex(
                    row["disposition"],
                    r"^(native|shared|retired)_[a-z0-9_]+$",
                )
                evidence_paths = row["native_evidence"].split(";")
                self.assertTrue(evidence_paths)
                for relative_path in evidence_paths:
                    self.assertTrue(
                        (MAIN_DIR.parent / relative_path).is_file(),
                        relative_path,
                    )

    def test_native_benchmark_declares_v2_report_and_2gbp_contract(
            self) -> None:
        source = (
            MAIN_DIR
            / "bioseq_caching"
            / "bench"
            / "cache_benchmark.cppm"
        ).read_text(encoding="utf-8")
        for required in (
            "bioseq-cache-benchmark-v2",
            "metadata",
            "scenario",
            "fixture",
            "correctness_digest",
            "cold_producer",
            "samples",
            "summary",
            "p50",
            "p95",
            "cache_counter_deltas",
            "logical_bytes",
            "stored_bytes",
            "peak_rss_bytes",
            "peak_temp_bytes",
            "build_identity",
            "host_identity",
            "staged_control_file",
            "staged_install_sql",
            "staged_dso",
            "staged_control_sha256",
            "2'000'000'000ULL",
            "5'000'000ULL",
            "reference_bases",
            "query_bases",
            "target_bases",
            "aggregate_input_bases",
            "bioseq.inspect_locator(sample.query)",
            "bioseq.inspect_locator(sample.target)",
            "PQsetSingleRowMode first PGRES_SINGLE_TUPLE arrival",
            "--postgres-stage-root",
            "--postgres-stage-manifest",
            ".preinstalled_stage_root = options.postgres_stage_root",
            ".preinstalled_stage_manifest = "
            "options.postgres_stage_manifest",
            ".allowed_runtime_dso_overrides = {}",
        ):
            with self.subTest(required=required):
                self.assertIn(required, source)

    def test_native_benchmark_report_schema_and_measurement_contract(
            self) -> None:
        bench_dir = MAIN_DIR / "bioseq_caching" / "bench"
        source = (
            bench_dir / "cache_benchmark.cppm"
        ).read_text(encoding="utf-8")
        contract = json.loads(
            (bench_dir / "report_schema_v2.json").read_text(
                encoding="utf-8"))

        self.assertEqual(
            contract["schema_version"], "bioseq-cache-benchmark-v2")
        self.assertEqual(
            contract["$schema"],
            "https://json-schema.org/draft/2020-12/schema",
        )
        self.assertEqual(contract["type"], "object")
        self.assertFalse(contract["additionalProperties"])
        self.assertEqual(
            set(contract["required"]),
            set(contract["required_fields"]["top_level"]),
        )
        self.assertIn(
            "at least three",
            contract["semantics"]["samples"],
        )
        self.assertIn(
            "measurement_cohort",
            contract["semantics"]["samples"],
        )
        self.assertIn(
            "measured after storage",
            contract["semantics"]["fixture_base_counts"],
        )
        self.assertIn(
            "exactly 2000000000",
            contract["semantics"]["fixture_base_counts"],
        )
        production_fixture_contract = contract["allOf"][1]["then"][
            "properties"]["fixture"]["properties"]
        self.assertEqual(
            production_fixture_contract[
                "aggregate_input_bases"]["const"],
            2_000_000_000,
        )
        self.assertEqual(
            production_fixture_contract["reference_bases"]["const"],
            1_995_000_000,
        )
        self.assertEqual(
            production_fixture_contract["query_bases"]["const"],
            5_000_000,
        )
        self.assertEqual(
            production_fixture_contract["target_bases"]["const"],
            1_995_000_000,
        )
        self.assertIn(
            "PostgreSQL postmaster and all descendant processes",
            contract["semantics"]["peak_rss_bytes"],
        )
        self.assertIn(
            "maximum sampled regular-file bytes",
            contract["semantics"]["peak_temp_bytes"],
        )
        schema_sections = {
            "metadata": "metadata",
            "build_identity": "build_identity",
            "host_identity": "host_identity",
            "resource_sampling": "resource_sampling",
            "artifact": "artifact",
            "fixture": "fixture",
            "sample": "sample",
            "summary": "summary",
            "counter": "counter",
        }
        for section, fields in contract["required_fields"].items():
            with self.subTest(section=section):
                self.assertTrue(fields)
                self.assertEqual(len(fields), len(set(fields)))
                if section in schema_sections:
                    self.assertEqual(
                        set(fields),
                        set(contract["$defs"][
                            schema_sections[section]]["required"]),
                    )
            for field in fields:
                with self.subTest(section=section, field=field):
                    self.assertIn(f'"{field}"', source)

        self.assertIn(
            "std::optional<benchmark_sample> cold_producer", source)
        self.assertIn("report.cold_producer = std::move(producer)", source)
        self.assertNotIn(
            "report.samples.push_back(std::move(producer))", source)
        self.assertIn(
            "report.samples.push_back(std::move(warm))", source)
        self.assertIn(
            "wall_samples.push_back(warm_sample.wall_time_ns)", source)
        self.assertIn('"warm samples only"', source)

        self.assertNotIn("postgres_cluster_rss_bytes", source)
        self.assertIn(
            "peak_postmaster_process_tree_rss_bytes", source)
        self.assertIn("postgres_test::resource_sampler", source)
        self.assertIn("peak_postgres_rss_bytes", source)
        self.assertIn("benchmark_resource_tracker", source)
        self.assertIn("rss_sample_interval", source)
        self.assertIn("temp_sample_interval", source)
        self.assertNotIn("RUSAGE_SELF", source)

        self.assertIn(
            "postgres_test::artifact_reports("
            "\n        fixture.cluster().staged_artifacts())",
            source,
        )
        self.assertLess(
            source.index("report.artifacts = postgres_test::artifact_reports"),
            source.index("benchmark_resource_tracker resource_tracker"),
        )
        self.assertIn(
            "benchmark_resource_report* destination_", source)
        self.assertIn(
            "write_failure_report(", source)
        self.assertIn(
            "postgres_test::sha256_file(executable).hex()", source)
        self.assertIn("::uname(&information)", source)
        self.assertIn("finalize_build_identity(report)", source)

    def test_cache_cmake_generates_artifact_manifest_from_target_properties(
            self) -> None:
        cmake = (
            MAIN_DIR
            / "bioseq_caching"
            / "bench"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertIn("cache_native_artifacts.json", cmake)
        self.assertIn("BIOSEQ_EXTENSION_CONTROL_FILE", cmake)
        self.assertIn("BIOSEQ_EXTENSION_INSTALL_SQL", cmake)
        self.assertIn("BIOSEQ_EXTENSION_DSO", cmake)
        self.assertIn(
            '\\"cache_control\\": '
            '\\"$<SHELL_PATH:$<TARGET_PROPERTY:'
            'postgresql_bioseq_cache,BIOSEQ_EXTENSION_CONTROL_FILE>>\\"',
            cmake,
        )
        self.assertIn(
            '\\"cache_install_sql\\": '
            '\\"$<SHELL_PATH:$<TARGET_PROPERTY:'
            'postgresql_bioseq_cache,BIOSEQ_EXTENSION_INSTALL_SQL>>\\"',
            cmake,
        )
        self.assertIn(
            "$<TARGET_GENEX_EVAL:postgresql_bioseq_cache,"
            "$<TARGET_PROPERTY:postgresql_bioseq_cache,"
            "BIOSEQ_EXTENSION_DSO>>",
            cmake,
        )
        self.assertIn(
            "$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,"
            "BIOSEQ_EXTENSION_CONTROL_FILE>",
            cmake,
        )
        self.assertIn(
            "$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,"
            "BIOSEQ_EXTENSION_TEST_INSTALL_SQL>",
            cmake,
        )
        self.assertIn(
            "$<TARGET_GENEX_EVAL:postgresql_bioseq_cache_test_support,"
            "$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,"
            "BIOSEQ_EXTENSION_DSO>>",
            cmake,
        )
        self.assertEqual(
            server._CACHE_NATIVE_ARTIFACT_KEYS,
            EXPECTED_CACHE_NATIVE_ARTIFACT_KEYS,
        )
        for key in server._CACHE_NATIVE_ARTIFACT_KEYS:
            with self.subTest(native_artifact_key=key):
                self.assertIn(f'\\"{key}\\"', cmake)
        for stage_target in (
            "bioseq_cache_test_support_postgres_stage",
            "bioseq_cache_production_postgres_stage",
            "bioseq_cache_bioseq_support_postgres_stage",
            "bioseq_cache_bioseq_only_postgres_stage",
            "bioseq_cache_bioseq_production_postgres_stage",
        ):
            with self.subTest(stage_target=stage_target):
                for stage_property in (
                    "BIOSEQ_POSTGRES_BIN_DIR",
                    "BIOSEQ_POSTGRES_STAGE_ROOT",
                    "BIOSEQ_POSTGRES_STAGE_MANIFEST",
                ):
                    self.assertIn(
                        f"$<TARGET_PROPERTY:{stage_target},"
                        f"{stage_property}>",
                        cmake,
                    )
        self.assertNotIn('\\"postgres_bin_dir\\"', cmake)
        self.assertIn(
            "$<TARGET_FILE:bioseq_cache_provider_peer_test>", cmake)
        self.assertIn("../test/legacy_migration_ledger.tsv", cmake)
        self.assertIn("Threads::Threads", cmake)
        self.assertIn(
            "bioseq_cache_benchmark_report_contract_tests", cmake)
        self.assertIn("cache_benchmark_report_tests.cpp", cmake)
        self.assertNotIn("run_private_cluster_bench.py", cmake)
        cache_unit = server.MODES["cache-unit"]
        self.assertIn(
            "bioseq_postgres_test_support_contract_tests",
            cache_unit.targets,
        )
        self.assertIn(
            [
                "bioseq_postgres_test_support_contract_tests",
                "--reporter",
                "console::out=-::colour-mode=ansi",
            ],
            cache_unit.commands,
        )
        self.assertIn(
            "bioseq_cache_benchmark_report_contract_tests",
            cache_unit.targets,
        )
        self.assertIn(
            [
                "bioseq_cache_benchmark_report_contract_tests",
                "--reporter",
                "console::out=-::colour-mode=ansi",
            ],
            cache_unit.commands,
        )
        self.assertIn(
            "bioseq_durable_file_io_contract_tests",
            cache_unit.targets,
        )
        self.assertIn(
            [
                "bioseq_durable_file_io_contract_tests",
                "--reporter",
                "console::out=-::colour-mode=ansi",
            ],
            cache_unit.commands,
        )
        self.assertIn(
            "bioseq_cache_legacy_inventory_tracking",
            cache_unit.targets,
        )
        self.assertIn(
            [
                "bioseq_cache_legacy_inventory_tracking",
                "--legacy-root",
                str(server.PROJECT_ROOT / "bioseq_caching"),
                "--legacy-inventory",
                "{legacy_inventory}",
                "--phase",
                "current",
                "--inventory-only",
            ],
            cache_unit.commands,
        )

    def test_cache_runtime_targets_use_profile_exact_postgres_stages(
            self) -> None:
        cache_cmake = (
            MAIN_DIR
            / "bioseq_caching"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        main_cmake = (
            MAIN_DIR
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        integration_cmake = (
            MAIN_DIR
            / "bioseq_caching"
            / "test"
            / "integration"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        combined_cmake = (
            MAIN_DIR
            / "bioseq_caching"
            / "test"
            / "bioseq"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        bench_cmake = (
            MAIN_DIR
            / "bioseq_caching"
            / "bench"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        helper_cmake = (
            MAIN_DIR
            / "cmake"
            / "BioseqPrivatePostgresStage.cmake"
        ).read_text(encoding="utf-8")

        keywords = {
            "TARGET",
            "PG_CONFIG",
            "STAGE_ROOT",
            "EXTENSION_NAMES",
            "EXTENSION_TARGETS",
            "CONTROL_PROPERTIES",
            "INSTALL_SQL_PROPERTIES",
            "MODULE_PATHNAME_OVERRIDES",
        }

        def parse_stage_calls(source: str) -> dict[str, dict[str, list[str]]]:
            parsed: dict[str, dict[str, list[str]]] = {}
            call_bodies = re.findall(
                r"bioseq_add_private_postgres_stage\s*\((.*?)\)",
                source,
                flags=re.DOTALL,
            )
            for body in call_bodies:
                fields: dict[str, list[str]] = {}
                current: str | None = None
                for raw_token in re.findall(r'"[^"]*"|[^\s]+', body):
                    token = raw_token.strip('"')
                    if token in keywords:
                        current = token
                        fields.setdefault(current, [])
                    else:
                        self.assertIsNotNone(
                            current,
                            f"unkeyed stage token {token!r}",
                        )
                        fields[current].append(token)
                self.assertEqual(len(fields.get("TARGET", [])), 1)
                target = fields.pop("TARGET")[0]
                self.assertNotIn(target, parsed)
                parsed[target] = fields
            return parsed

        cache_stage_calls = parse_stage_calls(cache_cmake)
        main_stage_calls = parse_stage_calls(main_cmake)
        self.assertTrue(
            set(cache_stage_calls).isdisjoint(main_stage_calls),
            "cache-owned private-stage target names must be globally unique",
        )
        stage_calls = {**cache_stage_calls, **main_stage_calls}
        expected_profiles = {
            "bioseq_cache_test_support_postgres_stage": {
                "PG_CONFIG": ["${PostgreSQL_CONFIG}"],
                "EXTENSION_NAMES": ["bioseq_cache"],
                "EXTENSION_TARGETS": [
                    "postgresql_bioseq_cache_test_support"],
                "CONTROL_PROPERTIES": [
                    "BIOSEQ_EXTENSION_CONTROL_FILE"],
                "INSTALL_SQL_PROPERTIES": [
                    "BIOSEQ_EXTENSION_TEST_INSTALL_SQL"],
            },
            "bioseq_cache_production_postgres_stage": {
                "PG_CONFIG": ["${PostgreSQL_CONFIG}"],
                "EXTENSION_NAMES": ["bioseq_cache"],
                "EXTENSION_TARGETS": ["postgresql_bioseq_cache"],
                "CONTROL_PROPERTIES": [
                    "BIOSEQ_EXTENSION_CONTROL_FILE"],
                "INSTALL_SQL_PROPERTIES": [
                    "BIOSEQ_EXTENSION_INSTALL_SQL"],
            },
            "bioseq_cache_bioseq_support_postgres_stage": {
                "PG_CONFIG": ["${PostgreSQL_CONFIG}"],
                "EXTENSION_NAMES": ["bioseq_cache", "bioseq"],
                "EXTENSION_TARGETS": [
                    "postgresql_bioseq_cache_test_support",
                    "postgresql_bioseq",
                ],
                "CONTROL_PROPERTIES": [
                    "BIOSEQ_EXTENSION_CONTROL_FILE",
                    "BIOSEQ_EXTENSION_CONTROL_FILE",
                ],
                "INSTALL_SQL_PROPERTIES": [
                    "BIOSEQ_EXTENSION_TEST_INSTALL_SQL",
                    "BIOSEQ_EXTENSION_INSTALL_SQL",
                ],
            },
            "bioseq_cache_bioseq_production_postgres_stage": {
                "PG_CONFIG": ["${PostgreSQL_CONFIG}"],
                "EXTENSION_NAMES": ["bioseq_cache", "bioseq"],
                "EXTENSION_TARGETS": [
                    "postgresql_bioseq_cache",
                    "postgresql_bioseq",
                ],
                "CONTROL_PROPERTIES": [
                    "BIOSEQ_EXTENSION_CONTROL_FILE",
                    "BIOSEQ_EXTENSION_CONTROL_FILE",
                ],
                "INSTALL_SQL_PROPERTIES": [
                    "BIOSEQ_EXTENSION_INSTALL_SQL",
                    "BIOSEQ_EXTENSION_INSTALL_SQL",
                ],
            },
            "bioseq_cache_bioseq_only_postgres_stage": {
                "PG_CONFIG": ["${PostgreSQL_CONFIG}"],
                "EXTENSION_NAMES": ["bioseq"],
                "EXTENSION_TARGETS": ["postgresql_bioseq"],
                "CONTROL_PROPERTIES": [
                    "BIOSEQ_EXTENSION_CONTROL_FILE"],
                "INSTALL_SQL_PROPERTIES": [
                    "BIOSEQ_EXTENSION_INSTALL_SQL"],
            },
        }
        self.assertEqual(stage_calls, expected_profiles)
        self.assertEqual(len(stage_calls), 5)
        self.assertEqual(len(stage_calls), len(set(stage_calls)))
        for target, profile in stage_calls.items():
            with self.subTest(stage_target=target):
                names = profile["EXTENSION_NAMES"]
                self.assertEqual(len(names), len(set(names)))
                vector_lengths = {
                    len(profile[field])
                    for field in (
                        "EXTENSION_NAMES",
                        "EXTENSION_TARGETS",
                        "CONTROL_PROPERTIES",
                        "INSTALL_SQL_PROPERTIES",
                    )
                }
                self.assertEqual(vector_lengths, {len(names)})
                self.assertNotIn("STAGE_ROOT", profile)
                self.assertNotIn("MODULE_PATHNAME_OVERRIDES", profile)

        self.assertIn(
            '"${_safe_stage_parent}/${BIOSEQ_STAGE_TARGET}"',
            helper_cmake,
        )
        self.assertRegex(
            helper_cmake,
            re.compile(
                r"string\(CONCAT\s+_metadata_root\s+"
                r'"\$\{CMAKE_CURRENT_BINARY_DIR\}/generated/'
                r'private_postgres/"\s+'
                r'"\$\{BIOSEQ_STAGE_TARGET\}"\s*\)',
                flags=re.DOTALL,
            ),
        )
        self.assertIn(
            'BIOSEQ_POSTGRES_STAGE_ROOT "${_stage_root}"',
            helper_cmake,
        )
        self.assertIn(
            'BIOSEQ_POSTGRES_STAGE_MANIFEST "${_stage_manifest}"',
            helper_cmake,
        )
        self.assertIn(
            'COMMENT\n'
            '      "Staging immutable PostgreSQL 17 runtime for '
            '${BIOSEQ_STAGE_TARGET}"\n'
            '    VERBATIM)',
            helper_cmake,
        )

        inventory_targets = {
            "cache_test_support_postgres":
                "bioseq_cache_test_support_postgres_stage",
            "cache_production_postgres":
                "bioseq_cache_production_postgres_stage",
            "cache_bioseq_support_postgres":
                "bioseq_cache_bioseq_support_postgres_stage",
            "cache_bioseq_only_postgres":
                "bioseq_cache_bioseq_only_postgres_stage",
            "cache_bioseq_production_postgres":
                "bioseq_cache_bioseq_production_postgres_stage",
        }
        self.assertEqual(
            len(inventory_targets.values()),
            len(set(inventory_targets.values())),
        )
        inventory_properties = {
            "bin_dir": "BIOSEQ_POSTGRES_BIN_DIR",
            "stage_root": "BIOSEQ_POSTGRES_STAGE_ROOT",
            "stage_manifest": "BIOSEQ_POSTGRES_STAGE_MANIFEST",
        }
        for prefix, stage_target in inventory_targets.items():
            for suffix, stage_property in inventory_properties.items():
                key = f"{prefix}_{suffix}"
                variable = f"_bioseq_{key}"
                with self.subTest(
                        inventory_key=key, stage_target=stage_target):
                    expected_expression = (
                        '"$<SHELL_PATH:$<TARGET_GENEX_EVAL:'
                        f"{stage_target},"
                        f"$<TARGET_PROPERTY:{stage_target},"
                        f'{stage_property}>>>"'
                    )
                    self.assertRegex(
                        bench_cmake,
                        re.compile(
                            r"set\(\s*"
                            + re.escape(variable)
                            + r"\s+"
                            + re.escape(expected_expression)
                            + r"\s*\)",
                            flags=re.DOTALL,
                        ),
                    )
                    self.assertIn(
                        f'\\"{key}\\": \\"${{{variable}}}\\"',
                        bench_cmake,
                    )

        consumer_sources = {
            "bioseq_cache_test_support_postgres_stage":
                integration_cmake,
            "bioseq_cache_production_postgres_stage": bench_cmake,
            "bioseq_cache_bioseq_support_postgres_stage":
                combined_cmake,
            "bioseq_cache_bioseq_only_postgres_stage": combined_cmake,
            "bioseq_cache_bioseq_production_postgres_stage":
                combined_cmake,
        }
        for stage_target, source in consumer_sources.items():
            for stage_property in inventory_properties.values():
                with self.subTest(
                        consumer_stage=stage_target,
                        stage_property=stage_property):
                    self.assertIn(
                        f"$<TARGET_PROPERTY:{stage_target},"
                        f"{stage_property}>",
                        source,
                    )
        for cmake in (integration_cmake, combined_cmake):
            self.assertNotIn(
                '--postgres-bin-dir "${PostgreSQL_BIN_DIR}"', cmake)
        self.assertNotIn("MODULE_PATHNAME_OVERRIDES", cache_cmake)
        self.assertNotIn("MODULE_PATHNAME_OVERRIDES", main_cmake)

    def test_cache_runner_uses_attested_preinstalled_stage_and_narrow_peer_override(
            self) -> None:
        support_dir = (
            MAIN_DIR
            / "bioseq_caching"
            / "test"
            / "support"
        )
        context = (support_dir / "cache_test_context.cppm").read_text(
            encoding="utf-8")
        runner = (support_dir / "cache_catch_runner.cpp").read_text(
            encoding="utf-8")
        peer = (
            MAIN_DIR
            / "bioseq_caching"
            / "test"
            / "bioseq"
            / "test_provider_peer_failopen.cpp"
        ).read_text(encoding="utf-8")
        coexistence = (
            MAIN_DIR
            / "bioseq_caching"
            / "test"
            / "bioseq"
            / "test_coexistence.cpp"
        ).read_text(encoding="utf-8")
        acceptance = (
            MAIN_DIR
            / "bioseq_caching"
            / "test"
            / "bioseq"
            / "test_coexistence_acceptance.cpp"
        ).read_text(encoding="utf-8")

        for source_name, source in (
                ("context", context), ("runner", runner)):
            with self.subTest(source=source_name):
                self.assertIn("postgres_stage_root", source)
                self.assertIn("postgres_stage_manifest", source)
        self.assertIn("--postgres-stage-root", runner)
        self.assertIn("--postgres-stage-manifest", runner)
        self.assertIn(
            "executable_allows_migration_role", runner)
        for executable_seal in (
            "executed_executable_sha256",
            "sha256_file(",
            "executed_executable).hex()",
            "watchdog_launch_configuration",
            "executable_sha256",
            "file_sha256_unchanged",
            "integration executable changed before report publication",
            "integration executable changed during report publication",
            ".build_identity = std::string(executable_sha256)",
            ".build_identity = executable_sha256",
        ):
            with self.subTest(executable_seal=executable_seal):
                self.assertIn(executable_seal, runner)
        for executable, role in (
            (
                "bioseq_cache_integration_tests",
                "cache_integration",
            ),
            (
                "bioseq_cache_bioseq_support_integration_tests",
                "cache_bioseq_support_integration",
            ),
            (
                "bioseq_cache_bioseq_production_integration_tests",
                "cache_bioseq_production_integration",
            ),
        ):
            with self.subTest(
                    migration_executable=executable,
                    migration_role=role):
                self.assertIn(f'"{executable}"', runner)
                self.assertIn(
                    f"evidence_role::{role}", runner)
        for option in (
            "--bioseq-only-postgres-bin-dir",
            "--bioseq-only-postgres-stage-root",
            "--bioseq-only-postgres-stage-manifest",
        ):
            self.assertEqual(runner.count(option), 1)
        for field in (
            "bioseq_only_postgres_bin_dir",
            "bioseq_only_postgres_stage_root",
            "bioseq_only_postgres_stage_manifest",
        ):
            self.assertIn(field, context)
            self.assertIn(field, runner)
        self.assertIn(
            ".preinstalled_stage_root = value.postgres_stage_root",
            context,
        )
        self.assertIn(
            ".preinstalled_stage_manifest = "
            "value.postgres_stage_manifest",
            context,
        )
        self.assertIn(".allowed_runtime_dso_overrides = {}", context)
        self.assertEqual(
            peer.count(
                'allowed_runtime_dso_overrides.insert("bioseq_cache")'),
            1,
        )
        self.assertIn("bioseq_only_cluster_options", context)
        self.assertIn(
            "canonical_cache_stage_artifact_paths", context)
        self.assertIn(
            'stage.control_directory / "bioseq_cache.control"', context)
        self.assertIn(
            'stage.control_directory / "bioseq_cache--1.0.sql"', context)
        self.assertIn(
            "stage.library_directory / "
            "configuration().cache_dso.filename()",
            context,
        )
        for source in (coexistence, acceptance):
            self.assertIn("bioseq_only_cluster_options", source)
            self.assertIn("staged_artifacts()", source)
            self.assertIn("stage.extensions.size() == 1", source)
            self.assertIn('stage.extensions.front().name == "bioseq"', source)
            self.assertIn(
                "canonical_cache_stage_artifact_paths(stage)", source)
            self.assertEqual(
                source.count(
                    "CHECK_FALSE(std::filesystem::exists(cache_paths["),
                3,
            )

    def test_cache_unit_common_contract_binaries_share_runtime_directory(
            self) -> None:
        cmake = (
            MAIN_DIR
            / "common"
            / "postgres_test_support"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        runtime_directory = (
            'RUNTIME_OUTPUT_DIRECTORY '
            '"${CMAKE_BINARY_DIR}/bioseq_caching"'
        )
        self.assertEqual(cmake.count(runtime_directory), 2)
        self.assertIn(
            "add_executable(bioseq_postgres_test_process_contract_child",
            cmake,
        )
        self.assertIn(
            "add_executable(bioseq_postgres_test_support_contract_tests",
            cmake,
        )

    def test_cache_runtime_modes_have_no_legacy_post_build_targets(
            self) -> None:
        for mode in (
            "cache-integration",
            "cache-bench",
            "cache-bioseq-integration",
            "cache-bioseq-bench",
        ):
            with self.subTest(mode=mode):
                self.assertEqual(server.post_build_targets_for(mode, False), ())
                self.assertEqual(server.post_build_targets_for(mode, True), ())

    def test_existing_install_modes_remain_unchanged(self) -> None:
        install_modes = {
            name for name, details in server.MODES.items()
            if details.install_extension
        }
        self.assertEqual(
            install_modes, {"unit+integration", "bench-e2e"})
        for name in install_modes:
            with self.subTest(mode=name):
                self.assertIn(
                    "postgresql_bioseq_cache", server.MODES[name].targets)
                self.assertIn("postgresql_bioseq", server.MODES[name].targets)
        for name, details in server.MODES.items():
            if name.startswith("cache-"):
                self.assertFalse(details.install_extension, name)
                if details.uses_private_cluster:
                    self.assertTrue(details.commands, name)

    def test_private_cluster_mode_cannot_install_shared_extension(self) -> None:
        with self.assertRaisesRegex(ValueError, "private-cluster modes"):
            server.ModeDefinition(
                lock_group="invalid",
                expected_duration_min=1.0,
                build_only_expected_duration_min=1.0,
                label="invalid",
                is_verb_plular=False,
                targets=[],
                commands=[],
                docs="invalid",
                install_extension=True,
                uses_private_cluster=True,
            )

    def test_native_cache_bench_rejects_unimplemented_cli_filters(self) -> None:
        result = server.tool_start_build(
            "cache-bench", bench_filter={"tags": ["cache"]})
        self.assertIn("require a benchmark CLI", result)
        self.assertIn("cache-bench", result)

    def test_native_cache_mode_commands_and_timeouts(self) -> None:
        primary_postgres_bin_dir_key_by_executable = {
            "bioseq_cache_integration_tests":
                "cache_test_support_postgres_bin_dir",
            "bioseq_cache_bench":
                "cache_production_postgres_bin_dir",
            "bioseq_cache_bioseq_support_integration_tests":
                "cache_bioseq_support_postgres_bin_dir",
            "bioseq_cache_bioseq_production_integration_tests":
                "cache_bioseq_production_postgres_bin_dir",
            "bioseq_cache_bioseq_bench":
                "cache_bioseq_production_postgres_bin_dir",
        }
        all_stage_prefixes = {
            "cache_test_support_postgres",
            "cache_production_postgres",
            "cache_bioseq_support_postgres",
            "cache_bioseq_only_postgres",
            "cache_bioseq_production_postgres",
        }
        expected = {
            "cache-integration": (
                ("bioseq_cache_integration_tests",),
                8.0,
                "480",
                None,
            ),
            "cache-bench": (
                ("bioseq_cache_bench",),
                20.0,
                None,
                "standalone",
            ),
            "cache-bioseq-integration": (
                (
                    "bioseq_cache_bioseq_support_integration_tests",
                    "bioseq_cache_bioseq_production_integration_tests",
                ),
                120.0,
                "480",
                None,
            ),
            "cache-bioseq-bench": (
                ("bioseq_cache_bioseq_bench",),
                180.0,
                None,
                "bioseq-2gbp",
            ),
        }
        for mode, (executables, timeout, case_timeout, scenario) in (
                expected.items()):
            with self.subTest(mode=mode):
                details = server.MODES[mode]
                self.assertEqual(details.expected_duration_min, timeout)
                self.assertEqual(
                    tuple(command[0] for command in details.commands),
                    executables,
                )
                self.assertTrue(details.reporting_subdir_name)
                for command in details.commands:
                    primary_bin_key = (
                        primary_postgres_bin_dir_key_by_executable[
                            command[0]])
                    primary_prefix = primary_bin_key.removesuffix("_bin_dir")
                    expected_stage_prefixes = {primary_prefix}
                    is_combined_support = (
                        command[0]
                        == "bioseq_cache_bioseq_support_integration_tests"
                    )
                    if is_combined_support:
                        expected_stage_prefixes.add(
                            "cache_bioseq_only_postgres")

                    expected_stage_placeholders = {
                        f"{prefix}_{suffix}"
                        for prefix in expected_stage_prefixes
                        for suffix in (
                            "bin_dir",
                            "stage_root",
                            "stage_manifest",
                        )
                    }
                    actual_stage_placeholders = {
                        argument[1:-1]
                        for argument in command
                        if (
                            argument.startswith("{")
                            and argument.endswith("}")
                            and "_postgres_" in argument
                        )
                    }
                    self.assertEqual(
                        actual_stage_placeholders,
                        expected_stage_placeholders,
                    )

                    primary_flags = {
                        "--postgres-bin-dir":
                            f"{{{primary_prefix}_bin_dir}}",
                        "--postgres-stage-root":
                            f"{{{primary_prefix}_stage_root}}",
                        "--postgres-stage-manifest":
                            f"{{{primary_prefix}_stage_manifest}}",
                    }
                    for flag, placeholder in primary_flags.items():
                        self.assertEqual(command.count(flag), 1)
                        flag_index = command.index(flag)
                        self.assertEqual(
                            command[flag_index + 1], placeholder)

                    bioseq_only_flags = {
                        "--bioseq-only-postgres-bin-dir":
                            "{cache_bioseq_only_postgres_bin_dir}",
                        "--bioseq-only-postgres-stage-root":
                            "{cache_bioseq_only_postgres_stage_root}",
                        "--bioseq-only-postgres-stage-manifest":
                            "{cache_bioseq_only_postgres_stage_manifest}",
                    }
                    for flag, placeholder in bioseq_only_flags.items():
                        self.assertEqual(
                            command.count(flag),
                            1 if is_combined_support else 0,
                        )
                        if is_combined_support:
                            flag_index = command.index(flag)
                            self.assertEqual(
                                command[flag_index + 1], placeholder)
                        else:
                            self.assertNotIn(placeholder, command)
                    self.assertEqual(
                        expected_stage_prefixes - all_stage_prefixes,
                        set(),
                    )
                    self.assertIn("--cache-control", command)
                    self.assertIn("--cache-install-sql", command)
                    self.assertIn("--cache-dso", command)
                    self.assertIn("--report", command)
                    self.assertEqual(command.count("{report}"), 1)
                    if case_timeout is not None:
                        timeout_index = command.index(
                            "--case-timeout-seconds")
                        self.assertEqual(
                            command[timeout_index + 1], case_timeout)
                    if scenario is not None:
                        scenario_index = command.index("--scenario")
                        self.assertEqual(
                            command[scenario_index + 1], scenario)
                        self.assertIn("--samples", command)
                        self.assertIn("--warmup-samples", command)
                    if "bioseq" in mode:
                        self.assertIn("--bioseq-control", command)
                        self.assertIn("--bioseq-install-sql", command)
                        self.assertIn("--bioseq-dso", command)
                if mode == "cache-bioseq-integration":
                    support_command, production_command = details.commands
                    self.assertNotIn(
                        "--large-case-timeout-seconds",
                        support_command,
                    )
                    timeout_index = production_command.index(
                        "--large-case-timeout-seconds")
                    self.assertEqual(
                        production_command[timeout_index + 1], "6300")

    def test_cache_unit_inventory_resolves_current_state_argv(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary_dir = Path(tmp)
            inventory = binary_dir / "legacy_inventory"
            inventory.touch()
            manifest = {
                key: str(binary_dir / key)
                for key in server._CACHE_NATIVE_ARTIFACT_KEYS
            }
            manifest["legacy_inventory"] = str(inventory)
            (binary_dir / "cache_native_artifacts.json").write_text(
                json.dumps(manifest), encoding="utf-8")
            command = server.MODES["cache-unit"].commands[-1]
            resolved = server._run_command_argv(
                "cache-unit",
                binary_dir,
                command,
            )

        self.assertEqual(
            resolved,
            [
                str(
                    binary_dir
                    / "bioseq_cache_legacy_inventory_tracking"
                ),
                "--legacy-root",
                str(server.PROJECT_ROOT / "bioseq_caching"),
                "--legacy-inventory",
                str(inventory),
                "--phase",
                "current",
                "--inventory-only",
            ],
        )

    def test_cache_integration_resolves_exact_cmake_parity_argv(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary_dir = Path(tmp)
            manifest = {
                key: str(binary_dir / key)
                for key in server._CACHE_NATIVE_ARTIFACT_KEYS
            }
            manifest["provider_peer_test_dso"] = ""
            for key, value in manifest.items():
                if not value:
                    continue
                path = Path(value)
                if key.endswith(
                        ("_postgres_bin_dir", "_postgres_stage_root")):
                    path.mkdir()
                else:
                    path.touch()
            (binary_dir / "cache_native_artifacts.json").write_text(
                json.dumps(manifest), encoding="utf-8")
            report = binary_dir / "reports" / "report_0.json"

            resolved = server._run_command_argv(
                "cache-integration",
                binary_dir,
                server.MODES["cache-integration"].commands[0],
                report_path=report,
            )

        self.assertEqual(
            resolved,
            [
                str(binary_dir / "bioseq_cache_integration_tests"),
                "--postgres-bin-dir",
                manifest["cache_test_support_postgres_bin_dir"],
                "--postgres-stage-root",
                manifest["cache_test_support_postgres_stage_root"],
                "--postgres-stage-manifest",
                manifest["cache_test_support_postgres_stage_manifest"],
                "--cache-control", manifest["cache_test_control"],
                "--cache-install-sql", manifest["cache_test_install_sql"],
                "--cache-dso", manifest["cache_test_dso"],
                "--migration-ledger", manifest["migration_ledger"],
                "--migration-harness", "cache-integration",
                "--migration-report-role", "cache-integration",
                "--report", str(report),
                "--case-timeout-seconds", "480",
                "--reporter", "console::out=-::colour-mode=ansi",
            ],
        )

    def test_combined_integration_resolves_profile_exact_argv(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary_dir = Path(tmp)
            manifest = {
                key: str(binary_dir / key)
                for key in server._CACHE_NATIVE_ARTIFACT_KEYS
            }
            for key, value in manifest.items():
                path = Path(value)
                if key.endswith(
                        ("_postgres_bin_dir", "_postgres_stage_root")):
                    path.mkdir()
                else:
                    path.touch()
            (binary_dir / "cache_native_artifacts.json").write_text(
                json.dumps(manifest), encoding="utf-8")
            support_report = (
                binary_dir / "reports" / "report_0.json")
            production_report = (
                binary_dir / "reports" / "report_1.json")

            support_resolved = server._run_command_argv(
                "cache-bioseq-integration",
                binary_dir,
                server.MODES["cache-bioseq-integration"].commands[0],
                report_path=support_report,
            )
            production_resolved = server._run_command_argv(
                "cache-bioseq-integration",
                binary_dir,
                server.MODES["cache-bioseq-integration"].commands[1],
                report_path=production_report,
            )

        self.assertEqual(
            support_resolved,
            [
                str(
                    binary_dir
                    / "bioseq_cache_bioseq_support_integration_tests"
                ),
                "--postgres-bin-dir",
                manifest["cache_bioseq_support_postgres_bin_dir"],
                "--postgres-stage-root",
                manifest["cache_bioseq_support_postgres_stage_root"],
                "--postgres-stage-manifest",
                manifest[
                    "cache_bioseq_support_postgres_stage_manifest"],
                "--bioseq-only-postgres-bin-dir",
                manifest["cache_bioseq_only_postgres_bin_dir"],
                "--bioseq-only-postgres-stage-root",
                manifest["cache_bioseq_only_postgres_stage_root"],
                "--bioseq-only-postgres-stage-manifest",
                manifest[
                    "cache_bioseq_only_postgres_stage_manifest"],
                "--cache-control", manifest["cache_test_control"],
                "--cache-install-sql", manifest["cache_test_install_sql"],
                "--cache-dso", manifest["cache_test_dso"],
                "--bioseq-control", manifest["bioseq_control"],
                "--bioseq-install-sql", manifest["bioseq_install_sql"],
                "--bioseq-dso", manifest["bioseq_dso"],
                "--provider-peer-test-dso",
                manifest["provider_peer_test_dso"],
                "--migration-ledger", manifest["migration_ledger"],
                "--migration-harness", "cache-bioseq-integration",
                "--migration-report-role",
                "cache-bioseq-support-integration",
                "--report", str(support_report),
                "--case-timeout-seconds", "480",
                "--reporter", "console::out=-::colour-mode=ansi",
            ],
        )
        self.assertEqual(
            production_resolved,
            [
                str(
                    binary_dir
                    / "bioseq_cache_bioseq_production_integration_tests"
                ),
                "--postgres-bin-dir",
                manifest["cache_bioseq_production_postgres_bin_dir"],
                "--postgres-stage-root",
                manifest["cache_bioseq_production_postgres_stage_root"],
                "--postgres-stage-manifest",
                manifest[
                    "cache_bioseq_production_postgres_stage_manifest"],
                "--cache-control", manifest["cache_control"],
                "--cache-install-sql", manifest["cache_install_sql"],
                "--cache-dso", manifest["cache_dso"],
                "--bioseq-control", manifest["bioseq_control"],
                "--bioseq-install-sql", manifest["bioseq_install_sql"],
                "--bioseq-dso", manifest["bioseq_dso"],
                "--migration-ledger", manifest["migration_ledger"],
                "--migration-harness", "cache-bioseq-integration",
                "--migration-report-role",
                "cache-bioseq-production-integration",
                "--report", str(production_report),
                "--case-timeout-seconds", "480",
                "--large-case-timeout-seconds", "6300",
                "--reporter", "console::out=-::colour-mode=ansi",
            ],
        )

    def test_cache_modes_contain_no_script_runner_invocations(self) -> None:
        forbidden = (".py", ".pl", ".pm", "python", "perl")
        for mode, details in server.MODES.items():
            if not mode.startswith("cache-"):
                continue
            for command in details.commands:
                rendered = " ".join(command).lower()
                with self.subTest(mode=mode, command=rendered):
                    self.assertFalse(
                        any(token in rendered for token in forbidden))

    def test_native_artifact_placeholders_resolve_from_generated_manifest(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary_dir = Path(tmp)
            manifest = {
                key: str(binary_dir / key)
                for key in server._CACHE_NATIVE_ARTIFACT_KEYS
            }
            for key, value in manifest.items():
                path = Path(value)
                if key.endswith(
                        ("_postgres_bin_dir", "_postgres_stage_root")):
                    path.mkdir()
                else:
                    path.touch()
            (binary_dir / "cache_native_artifacts.json").write_text(
                json.dumps(manifest), encoding="utf-8")
            report = binary_dir / "reports" / "report_0.json"
            command = server.MODES["cache-bioseq-bench"].commands[0]
            resolved = server._run_command_argv(
                "cache-bioseq-bench",
                binary_dir,
                command,
                report_path=report,
            )
        self.assertEqual(
            resolved[0], str(binary_dir / "bioseq_cache_bioseq_bench"))
        self.assertIn(
            str(binary_dir / "cache_bioseq_production_postgres_bin_dir"),
            resolved,
        )
        self.assertIn(
            str(
                binary_dir
                / "cache_bioseq_production_postgres_stage_root"
            ),
            resolved,
        )
        self.assertIn(
            str(
                binary_dir
                / "cache_bioseq_production_postgres_stage_manifest"
            ),
            resolved,
        )
        self.assertIn(str(binary_dir / "cache_control"), resolved)
        self.assertIn(str(binary_dir / "bioseq_dso"), resolved)
        self.assertIn(str(binary_dir / "migration_ledger"), resolved)
        self.assertIn(str(report), resolved)
        self.assertNotIn("{report}", resolved)

    def test_native_artifact_resolution_rejects_missing_combined_path(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary_dir = Path(tmp)
            manifest = {
                key: str(binary_dir / key)
                for key in server._CACHE_NATIVE_ARTIFACT_KEYS
            }
            manifest["bioseq_dso"] = ""
            for key, value in manifest.items():
                if not value:
                    continue
                path = Path(value)
                if key.endswith(
                        ("_postgres_bin_dir", "_postgres_stage_root")):
                    path.mkdir()
                else:
                    path.touch()
            (binary_dir / "cache_native_artifacts.json").write_text(
                json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(
                    RuntimeError, "bioseq_dso.*unavailable"):
                server._run_command_argv(
                    "cache-bioseq-bench",
                    binary_dir,
                    server.MODES["cache-bioseq-bench"].commands[0],
                    report_path=binary_dir / "report_0.json",
                )

    def test_report_path_allocator_preserves_prior_native_reports(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            reporting_dir = Path(tmp)
            (reporting_dir / "report_0.json").write_text(
                "{}", encoding="utf-8")
            (reporting_dir / "report_debug_9.jsonl").write_text(
                "{}", encoding="utf-8")
            self.assertEqual(
                server._next_report_path(reporting_dir),
                reporting_dir / "report_1.json",
            )

    def test_concurrent_worker_state_updates_are_lossless_and_collision_safe(
            self) -> None:
        if "fork" not in multiprocessing.get_all_start_methods():
            self.skipTest("state locking contract requires POSIX fork/flock")

        with tempfile.TemporaryDirectory() as tmp:
            state_path = Path(tmp) / "state.json"
            lock_path = Path(tmp) / "state.lock"
            context = multiprocessing.get_context("fork")
            start_barrier = context.Barrier(3)
            results = context.Queue()
            real_named_temporary_file = (
                server.tempfile.NamedTemporaryFile)

            def update_from_worker(
                    mode: str, fields: dict[str, object],
                    nested_lock: bool) -> None:
                server.STATE_FILE = state_path
                server.LOCK_FILE = lock_path

                def tracked_temporary_file(*args, **kwargs):
                    handle = real_named_temporary_file(*args, **kwargs)
                    results.put(("temporary", handle.name))
                    return handle

                server.tempfile.NamedTemporaryFile = tracked_temporary_file
                try:
                    start_barrier.wait(timeout=5.0)
                    if nested_lock:
                        with server._global_lock():
                            server._update_entry(mode, **fields)
                    else:
                        server._update_entry(mode, **fields)
                    results.put(("completed", mode))
                except BaseException as exc:
                    results.put(
                        ("error", f"{type(exc).__name__}: {exc}"))

            workers = [
                context.Process(
                    target=update_from_worker,
                    args=(
                        "cache-integration",
                        {
                            "step": "support-report-published",
                            "report_files": ["/reports/report_0.json"],
                        },
                        True,
                    ),
                ),
                context.Process(
                    target=update_from_worker,
                    args=(
                        "cache-bioseq-bench",
                        {
                            "notes": "production-benchmark-ready",
                            "report_file": "/reports/report_1.json",
                        },
                        False,
                    ),
                ),
            ]
            for worker in workers:
                worker.start()
            try:
                start_barrier.wait(timeout=5.0)
                for worker in workers:
                    worker.join(timeout=10.0)
            finally:
                timed_out = [
                    worker.pid for worker in workers if worker.is_alive()
                ]
                for worker in workers:
                    if worker.is_alive():
                        worker.terminate()
                    worker.join(timeout=2.0)

            self.assertEqual(timed_out, [], "state update deadlocked")
            self.assertEqual(
                [worker.exitcode for worker in workers], [0, 0])

            messages: list[tuple[str, str]] = []
            while True:
                try:
                    messages.append(results.get(timeout=0.1))
                except queue.Empty:
                    break
            results.close()
            results.join_thread()

            errors = [
                detail for kind, detail in messages if kind == "error"
            ]
            self.assertEqual(errors, [])
            self.assertEqual(
                {
                    detail
                    for kind, detail in messages
                    if kind == "completed"
                },
                {"cache-integration", "cache-bioseq-bench"},
            )
            temporary_paths = [
                Path(detail)
                for kind, detail in messages
                if kind == "temporary"
            ]
            self.assertEqual(len(temporary_paths), 2)
            self.assertEqual(len(set(temporary_paths)), 2)
            self.assertTrue(
                all(path.parent == state_path.parent
                    for path in temporary_paths))
            self.assertTrue(
                all(not path.exists() for path in temporary_paths))
            self.assertFalse(
                state_path.with_suffix(".json.tmp").exists())

            state = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual(
                state["cache-integration"]["step"],
                "support-report-published",
            )
            self.assertEqual(
                state["cache-integration"]["report_files"],
                ["/reports/report_0.json"],
            )
            self.assertEqual(
                state["cache-bioseq-bench"]["notes"],
                "production-benchmark-ready",
            )
            self.assertEqual(
                state["cache-bioseq-bench"]["report_file"],
                "/reports/report_1.json",
            )

    def test_combined_commands_persist_two_ordered_report_receipts(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            reporting_root = Path(tmp) / "reporting"
            command_receipts: list[tuple[str, Path]] = []
            state_updates: list[dict[str, object]] = []

            def resolve_command(
                    _mode, _active_bin_dir, command, *,
                    bench_flags=None, test_flags=None,
                    report_path=None):
                self.assertIsNone(bench_flags)
                self.assertIsNone(test_flags)
                self.assertIsNotNone(report_path)
                command_receipts.append((command[0], report_path))
                return [command[0], str(report_path)]

            def publish_report(argv, *_args, **_kwargs):
                Path(argv[-1]).write_text("{}", encoding="utf-8")
                return 0

            def record_update(_mode, **fields):
                state_updates.append(fields)

            with (
                mock.patch.object(server, "REPORTING_DIR", reporting_root),
                mock.patch.object(
                    server,
                    "_run_command_argv",
                    side_effect=resolve_command,
                ),
                mock.patch.object(
                    server,
                    "_stream_subprocess",
                    side_effect=publish_report,
                ),
                mock.patch.object(
                    server,
                    "_update_entry",
                    side_effect=record_update,
                ),
            ):
                (
                    report_file,
                    report_files,
                    report_invocations,
                    unit_command_records,
                ) = server._run_mode_commands(
                    "cache-bioseq-integration",
                    Path(tmp) / "bin",
                    Path(tmp) / "build",
                    {},
                    io.StringIO(),
                    float("inf"),
                    set(),
                )

            report_dir = (
                reporting_root / "bioseq_cache_bioseq_integration")
            expected_reports = [
                report_dir / "report_0.json",
                report_dir / "report_1.json",
            ]
            self.assertEqual(report_files, expected_reports)
            self.assertEqual(report_file, expected_reports[-1])
            self.assertEqual(unit_command_records, [])
            self.assertEqual(
                [Path(invocation[0]).name
                 for invocation in report_invocations],
                [
                    "bioseq_cache_bioseq_support_integration_tests",
                    "bioseq_cache_bioseq_production_integration_tests",
                ],
            )
            self.assertEqual(
                command_receipts,
                [
                    (
                        "bioseq_cache_bioseq_support_integration_tests",
                        expected_reports[0],
                    ),
                    (
                        "bioseq_cache_bioseq_production_integration_tests",
                        expected_reports[1],
                    ),
                ],
            )
            self.assertEqual(
                state_updates,
                [
                    {
                        "report_file": str(expected_reports[0]),
                        "report_files": [str(expected_reports[0])],
                    },
                    {
                        "report_file": str(expected_reports[1]),
                        "report_files": [
                            str(expected_reports[0]),
                            str(expected_reports[1]),
                        ],
                    },
                ],
            )

    def test_missing_support_report_fails_before_production_command(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            reporting_root = Path(tmp) / "reporting"
            resolved_commands: list[str] = []

            def resolve_command(
                    _mode, _active_bin_dir, command, *,
                    bench_flags=None, test_flags=None,
                    report_path=None):
                self.assertIsNone(bench_flags)
                self.assertIsNone(test_flags)
                self.assertIsNotNone(report_path)
                resolved_commands.append(command[0])
                return [command[0], str(report_path)]

            with (
                mock.patch.object(server, "REPORTING_DIR", reporting_root),
                mock.patch.object(
                    server,
                    "_run_command_argv",
                    side_effect=resolve_command,
                ),
                mock.patch.object(
                    server,
                    "_stream_subprocess",
                    return_value=0,
                ) as stream,
                mock.patch.object(server, "_update_entry") as update,
            ):
                with self.assertRaisesRegex(
                        RuntimeError,
                        "support_integration_tests.*allocated regular "
                        "JSON report"):
                    server._run_mode_commands(
                        "cache-bioseq-integration",
                        Path(tmp) / "bin",
                        Path(tmp) / "build",
                        {},
                        io.StringIO(),
                        float("inf"),
                        set(),
                    )

            self.assertEqual(
                resolved_commands,
                ["bioseq_cache_bioseq_support_integration_tests"],
            )
            self.assertEqual(stream.call_count, 1)
            update.assert_not_called()
            self.assertFalse(
                (
                    reporting_root
                    / "bioseq_cache_bioseq_integration"
                    / "report_1.json"
                ).exists()
            )

    def test_integration_timeout_validates_terminal_cleanup_report(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve(strict=True)
            reporting_root = root / "reporting"
            unsafe_cleanup_root = (
                Path(tempfile.gettempdir()).resolve(strict=True)
                / f"ordinary-timeout-root-{server.secrets.token_hex(8)}"
            )

            def resolve_command(
                    _mode, _active_bin_dir, command, *,
                    bench_flags=None, test_flags=None,
                    report_path=None):
                self.assertIsNotNone(report_path)
                return [command[0], str(report_path)]

            def publish_unsafe_terminal_report(
                    argv, *_args, **_kwargs):
                Path(argv[-1]).write_text(json.dumps({
                    "schema_version": "bioseq-cache-test-v2",
                    "cleanup": {"succeeded": True},
                    "cleanup_observations": [{
                        "succeeded": True,
                        "root": str(unsafe_cleanup_root),
                    }],
                    "cluster_configuration": {
                        "retained-supervision-state": "none",
                    },
                }), encoding="utf-8")
                raise TimeoutError("injected mode timeout")

            with (
                mock.patch.object(
                    server, "REPORTING_DIR", reporting_root),
                mock.patch.object(
                    server,
                    "_run_command_argv",
                    side_effect=resolve_command,
                ),
                mock.patch.object(
                    server,
                    "_stream_subprocess",
                    side_effect=publish_unsafe_terminal_report,
                ),
                mock.patch.object(server, "_update_entry"),
                self.assertRaisesRegex(
                    RuntimeError, "not marker-owned"),
            ):
                server._run_mode_commands(
                    "cache-bioseq-integration",
                    root / "bin",
                    root / "build",
                    {},
                    io.StringIO(),
                    server.time.time() + 10,
                    set(),
                )

    def test_status_exposes_ordered_reports_and_legacy_singular_fallback(
            self) -> None:
        now = 1_000_000.0
        support = "/reports/report_0.json"
        production = "/reports/report_1.json"
        base_entry = {
            "status": "completed",
            "started_at": now - 60.0,
            "completed_at": now,
            "build_only": False,
            "debug": False,
            "preset": "cache-bioseq-opt",
            "uses_private_cluster": True,
            "focused_test_args": [],
            "log_file": "/logs/cache.log",
            "report_file": production,
        }
        with mock.patch.object(
                server,
                "_load_state",
                return_value={
                    "cache-bioseq-integration": {
                        **base_entry,
                        "report_files": [support, production],
                    },
                }):
            status = server.tool_status("cache-bioseq-integration")
        self.assertIn(f"Reports: {support}, {production}", status)
        self.assertLess(status.index(support), status.index(production))

        with mock.patch.object(
                server,
                "_load_state",
                return_value={
                    "cache-bioseq-integration": base_entry,
                }):
            legacy_status = server.tool_status(
                "cache-bioseq-integration")
        self.assertIn(f"Report: {production}", legacy_status)
        self.assertNotIn("Reports:", legacy_status)

    def test_mcp_schemas_expose_cache_modes_and_inspect_targets(self) -> None:
        definitions = {
            definition["name"]: definition
            for definition in server.TOOL_DEFINITIONS
        }
        modes = definitions["start_build"]["inputSchema"]["properties"][
            "mode"]["enum"]
        inspect_targets = definitions["inspect"]["inputSchema"][
            "properties"]["target"]["enum"]
        remote_shapes = definitions["remote-run"]["inputSchema"]["oneOf"]
        remote_modes = remote_shapes[0]["properties"]["mode"]["enum"]
        remote_targets = remote_shapes[1]["properties"]["target"]["enum"]
        for mode in (
            "cache-unit",
            "cache-integration",
            "cache-bench",
            "cache-bioseq-integration",
            "cache-bioseq-bench",
        ):
            self.assertIn(mode, modes)
            self.assertIn(mode, remote_modes)
        self.assertEqual(inspect_targets, sorted(server.INSPECT_TARGETS))
        self.assertEqual(remote_targets, sorted(server.INSPECT_TARGETS))

    def test_inspect_missing_artifact_uses_cache_subdirectory_and_candidates(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = inspect_tool.run_inspect(
                function_name="bioseq_cache_version",
                target="postgresql_bioseq_cache",
                build_dir=Path(tmp),
                binary_subdir="bioseq_caching",
                artifact_names=("bioseq_cache.dylib", "bioseq_cache.so"),
                shared_target_names=("bioseq_cache_core",),
            )
        self.assertIn("error", result)
        self.assertIn("bioseq_caching/bioseq_cache.dylib", result["error"])
        self.assertIn("bioseq_caching/bioseq_cache.so", result["error"])
        self.assertEqual(result["binary_subdir"], "bioseq_caching")

    def test_postgresql_bioseq_inspect_mapping_uses_bench_e2e_dso_candidates(
            self) -> None:
        definition = server.INSPECT_TARGETS["postgresql_bioseq"]
        self.assertEqual(definition.mode, "bench-e2e")
        self.assertEqual(
            definition.artifact_names,
            ("bioseq.dylib", "bioseq.so", "bioseq.dll"),
        )
        self.assertEqual(
            definition.shared_target_names,
            ("bioseq_modules_nopg",),
        )

        with tempfile.TemporaryDirectory() as tmp:
            result = inspect_tool.run_inspect(
                function_name="PreparedShapeV5FragmentQueryReader",
                target="postgresql_bioseq",
                build_dir=Path(tmp),
                binary_subdir="",
                artifact_names=definition.artifact_names,
                shared_target_names=definition.shared_target_names,
            )
        self.assertIn("error", result)
        self.assertIn("bioseq.dylib", result["error"])
        self.assertIn("bioseq.so", result["error"])
        self.assertIn("bioseq.dll", result["error"])


class CacheRetirementProvenanceTest(unittest.TestCase):
    _ROLES = (
        "cache-integration",
        "cache-bioseq-support-integration",
        "cache-bioseq-production-integration",
        "cache-bench",
        "cache-bioseq-bench",
    )

    @staticmethod
    def _provenance(phase: str = "passing") -> dict[str, object]:
        return {
            "source_revision": "a" * 40,
            "source_scope_sha256": "b" * 64,
            "source_scoped_clean": True,
            "migration_ledger_sha256": "c" * 64,
            "evidence_contract_sha256": "d" * 64,
            "migration_phase": phase,
        }

    @staticmethod
    def _write_json(path: Path, value: object) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(value, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    @classmethod
    def _historical_chain(
            cls, root: Path) -> dict[str, object]:
        unit_dir = root / "unit"
        evidence_dir = root / "evidence"
        native_dir = root / "native"
        historical = cls._provenance("passing")
        historical["source_revision"] = "1" * 40
        historical["source_scope_sha256"] = "2" * 64
        historical["migration_ledger_sha256"] = "3" * 64

        unit_receipt = unit_dir / "receipt_historical.json"
        cls._write_json(unit_receipt, {
            "schema_version": "bioseq-cache-mcp-unit-receipt-v1",
            "mode": "cache-unit",
            "status": "passed",
            **historical,
            "commands": [],
        })

        evidence_receipts: dict[str, Path] = {}
        native_reports: dict[str, Path] = {}
        report_seals: list[dict[str, str]] = []
        for ordinal, role in enumerate(cls._ROLES):
            native_report = native_dir / f"report_{ordinal}.json"
            cls._write_json(native_report, {
                "schema_version": "historical-native-probe-v1",
                "report_role": role,
                "status": "passed",
            })
            receipt = evidence_dir / f"receipt_historical_{ordinal}.json"
            cls._write_json(receipt, {
                "schema_version":
                    "bioseq-cache-mcp-evidence-receipt-v1",
                "mode": server._CACHE_EVIDENCE_MODE_BY_ROLE[role],
                "report_role": role,
                "native_report": str(native_report),
                "native_report_sha256":
                    server._sha256_path(native_report),
                "executable": str(
                    Path("/fixed")
                    / server._CACHE_EVIDENCE_EXECUTABLE_BY_ROLE[role]),
                "executable_sha256": "4" * 64,
                "scenario":
                    server._CACHE_EVIDENCE_SCENARIO_BY_ROLE[role],
                **historical,
            })
            evidence_receipts[role] = receipt
            native_reports[role] = native_report
            report_seals.append({
                "report_role": role,
                "receipt_sha256": server._sha256_path(receipt),
            })

        proof = {
            "schema_version": "bioseq-cache-retirement-proof-v1",
            "proof_source_revision": historical["source_revision"],
            "proof_source_scope_sha256":
                historical["source_scope_sha256"],
            "evidence_contract_sha256":
                historical["evidence_contract_sha256"],
            "unit_receipt_sha256": server._sha256_path(unit_receipt),
            "reports": report_seals,
        }
        return {
            "unit_dir": unit_dir,
            "evidence_dir": evidence_dir,
            "historical": historical,
            "unit_receipt": unit_receipt,
            "evidence_receipts": evidence_receipts,
            "native_reports": native_reports,
            "proof": proof,
        }

    @classmethod
    def _add_current_chain(
            cls, fixture: dict[str, object],
            provenance: dict[str, object]) -> tuple[Path, list[Path]]:
        unit_dir = fixture["unit_dir"]
        evidence_dir = fixture["evidence_dir"]
        assert isinstance(unit_dir, Path)
        assert isinstance(evidence_dir, Path)
        unit_receipt = unit_dir / "receipt_current.json"
        cls._write_json(unit_receipt, {
            "schema_version": "bioseq-cache-mcp-unit-receipt-v1",
            "mode": "cache-unit",
            "status": "passed",
            **provenance,
            "commands": [],
        })
        receipts = []
        for ordinal, role in enumerate(cls._ROLES):
            path = evidence_dir / f"receipt_current_{ordinal}.json"
            cls._write_json(path, {
                "schema_version":
                    "bioseq-cache-mcp-evidence-receipt-v1",
                "mode": server._CACHE_EVIDENCE_MODE_BY_ROLE[role],
                "report_role": role,
                "executable": str(
                    Path("/fixed")
                    / server._CACHE_EVIDENCE_EXECUTABLE_BY_ROLE[role]),
                "executable_sha256": "e" * 64,
                "scenario":
                    server._CACHE_EVIDENCE_SCENARIO_BY_ROLE[role],
                **provenance,
            })
            receipts.append(path)
        return unit_receipt, receipts

    @classmethod
    def _retired_lifecycle_fixture(
            cls, root: Path) -> dict[str, object]:
        fixture = cls._historical_chain(root)
        current = cls._provenance("retired_with_proof")
        current_unit, current_receipts = cls._add_current_chain(
            fixture, current)
        legacy_root = root / "legacy-root"
        active_bin_dir = root / "bin"
        active_build_dir = root / "build"
        retirement_dir = root / "retirement"
        legacy_root.mkdir()
        active_bin_dir.mkdir()
        active_build_dir.mkdir()
        retirement_dir.mkdir()
        ledger = root / "legacy_migration_ledger.tsv"
        inventory = root / "legacy_asset_inventory.txt"
        catch_cases = root / "migration_catch_cases.tsv"
        gate = active_bin_dir / "bioseq_cache_legacy_retirement_gate"
        proof_path = root / "legacy_retirement_receipt.json"
        ledger.write_text("fixture ledger\n", encoding="utf-8")
        inventory.write_text("fixture inventory\n", encoding="utf-8")
        catch_cases.write_text("fixture catch cases\n", encoding="utf-8")
        gate.write_bytes(b"fixture gate")
        cls._write_json(proof_path, fixture["proof"])
        fixture.update({
            "current": current,
            "current_unit": current_unit,
            "current_receipts": current_receipts,
            "legacy_root": legacy_root,
            "ledger": ledger,
            "inventory": inventory,
            "catch_cases": catch_cases,
            "gate": gate,
            "proof_path": proof_path,
            "active_bin_dir": active_bin_dir,
            "active_build_dir": active_build_dir,
            "retirement_dir": retirement_dir,
        })
        return fixture

    @staticmethod
    def _patch_retired_lifecycle(
            stack: contextlib.ExitStack,
            fixture: dict[str, object],
            transition_sha256: str,
            stream_side_effect) -> mock.Mock:
        def patch(name: str, value: object):
            return stack.enter_context(
                mock.patch.object(server, name, value))

        patch("_CACHE_UNIT_REPORTING_DIR", fixture["unit_dir"])
        patch(
            "_CACHE_EVIDENCE_REPORTING_DIRS",
            (fixture["evidence_dir"],),
        )
        patch("_CACHE_MIGRATION_LEDGER", fixture["ledger"])
        patch("_CACHE_LEGACY_ROOT", fixture["legacy_root"])
        patch("_CACHE_LEGACY_INVENTORY", fixture["inventory"])
        patch("_CACHE_RETIREMENT_PROOF", fixture["proof_path"])
        patch(
            "_CACHE_RETIREMENT_REPORTING_DIR",
            fixture["retirement_dir"],
        )
        stack.enter_context(mock.patch.object(
            server,
            "_cache_native_artifacts",
            return_value={
                "migration_catch_cases": str(
                    fixture["catch_cases"]),
                "legacy_retirement_gate": str(fixture["gate"]),
            },
        ))
        stack.enter_context(mock.patch.object(
            server,
            "_cache_retirement_transition_sha256",
            return_value=transition_sha256,
        ))
        stream_patch = (
            {"side_effect": stream_side_effect}
            if callable(stream_side_effect)
            else {"return_value": stream_side_effect}
        )
        stream = stack.enter_context(mock.patch.object(
            server, "_stream_subprocess", **stream_patch))
        stack.enter_context(mock.patch.object(server, "_update_entry"))
        return stream

    def test_status_only_ledger_transition_preserves_evidence_digest(
            self) -> None:
        header = (
            "legacy_path\tlegacy_line_or_assertion\tstable_id\t"
            "evidence_harness\tevidence_role\tsource_kind\t"
            "scenario_or_permutation\texpected_sqlstate_or_result\t"
            "expected_state_invariant\tcatch2_case\tcatch2_section\t"
            "catch2_tags\tmigration_status\n"
        )
        semantic_fields = (
            "test/regress/a.sql\tSELECT 1\tlegacy-a\t"
            "cache-integration\tcache-integration\tsql\tdefault\t"
            "1\tunchanged\tcase a\tsection a\t[cache]"
        )
        with tempfile.TemporaryDirectory() as tmp:
            passing = Path(tmp) / "passing.tsv"
            retired = Path(tmp) / "retired.tsv"
            changed = Path(tmp) / "changed.tsv"
            passing.write_text(
                header + semantic_fields + "\tpassing\n",
                encoding="utf-8",
            )
            retired.write_text(
                header + semantic_fields + "\tretired_with_proof\n",
                encoding="utf-8",
            )
            changed.write_text(
                header
                + semantic_fields.replace(
                    "cache-integration\tcache-integration",
                    "cache-bench\tcache-bench",
                )
                + "\tretired_with_proof\n",
                encoding="utf-8",
            )

            passing_contract = server._cache_migration_contract(passing)
            retired_contract = server._cache_migration_contract(retired)
            changed_contract = server._cache_migration_contract(changed)

        self.assertNotEqual(
            passing_contract["migration_ledger_sha256"],
            retired_contract["migration_ledger_sha256"],
        )
        self.assertEqual(
            passing_contract["evidence_contract_sha256"],
            retired_contract["evidence_contract_sha256"],
        )
        self.assertNotEqual(
            retired_contract["evidence_contract_sha256"],
            changed_contract["evidence_contract_sha256"],
        )
        self.assertEqual(
            retired_contract["migration_phase"],
            "retired_with_proof",
        )

    def test_current_passing_receipt_roster_is_exact_and_ordered(
            self) -> None:
        provenance = self._provenance()
        roles = (
            "cache-integration",
            "cache-bioseq-support-integration",
            "cache-bioseq-production-integration",
            "cache-bench",
            "cache-bioseq-bench",
        )
        with tempfile.TemporaryDirectory() as tmp:
            reporting = Path(tmp) / "receipts"
            reporting.mkdir()
            for ordinal, role in enumerate(reversed(roles)):
                receipt = {
                    "schema_version":
                        "bioseq-cache-mcp-evidence-receipt-v1",
                    "mode": server._CACHE_EVIDENCE_MODE_BY_ROLE[role],
                    "report_role": role,
                    "executable": str(
                        Path("/fixed")
                        / server._CACHE_EVIDENCE_EXECUTABLE_BY_ROLE[
                            role]),
                    "executable_sha256": "f" * 64,
                    "scenario":
                        server._CACHE_EVIDENCE_SCENARIO_BY_ROLE[role],
                    **provenance,
                }
                (reporting / f"receipt_{ordinal}.json").write_text(
                    json.dumps(receipt), encoding="utf-8")
            with mock.patch.object(
                    server,
                    "_CACHE_EVIDENCE_REPORTING_DIRS",
                    (reporting,)):
                selected = (
                    server._find_current_cache_evidence_receipts(
                        provenance)
                )
                observed = [
                    json.loads(path.read_text(encoding="utf-8"))[
                        "report_role"]
                    for path in selected
                ]
                self.assertEqual(observed, list(roles))

                stale = json.loads(
                    selected[0].read_text(encoding="utf-8"))
                stale["migration_ledger_sha256"] = "e" * 64
                selected[0].write_text(
                    json.dumps(stale), encoding="utf-8")
                with self.assertRaisesRegex(
                        RuntimeError, "missing=.*cache-integration"):
                    server._find_current_cache_evidence_receipts(
                        provenance)

    def test_current_only_roster_cannot_satisfy_historical_proof(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fixture = self._historical_chain(Path(tmp))
            current = self._provenance("retired_with_proof")
            self._add_current_chain(fixture, current)
            unit_receipt = fixture["unit_receipt"]
            evidence_receipts = fixture["evidence_receipts"]
            assert isinstance(unit_receipt, Path)
            assert isinstance(evidence_receipts, dict)
            unit_receipt.unlink()
            for receipt in evidence_receipts.values():
                assert isinstance(receipt, Path)
                receipt.unlink()

            with (
                mock.patch.object(
                    server,
                    "_CACHE_UNIT_REPORTING_DIR",
                    fixture["unit_dir"],
                ),
                mock.patch.object(
                    server,
                    "_CACHE_EVIDENCE_REPORTING_DIRS",
                    (fixture["evidence_dir"],),
                ),
                self.assertRaisesRegex(
                    RuntimeError,
                    "proof-ready native report receipts are incomplete",
                ),
            ):
                server._find_proof_cache_receipts(fixture["proof"])

    def test_historical_unit_receipt_is_hash_sealed(
            self) -> None:
        for mutation in ("missing", "altered"):
            with self.subTest(mutation=mutation), \
                    tempfile.TemporaryDirectory() as tmp:
                fixture = self._historical_chain(Path(tmp))
                unit_receipt = fixture["unit_receipt"]
                assert isinstance(unit_receipt, Path)
                if mutation == "missing":
                    unit_receipt.unlink()
                else:
                    unit_receipt.write_text(
                        unit_receipt.read_text(encoding="utf-8")
                        + " ",
                        encoding="utf-8",
                    )
                with (
                    mock.patch.object(
                        server,
                        "_CACHE_UNIT_REPORTING_DIR",
                        fixture["unit_dir"],
                    ),
                    mock.patch.object(
                        server,
                        "_CACHE_EVIDENCE_REPORTING_DIRS",
                        (fixture["evidence_dir"],),
                    ),
                    self.assertRaisesRegex(
                        RuntimeError,
                        "cache-unit receipt is unavailable or ambiguous",
                    ),
                ):
                    server._find_proof_cache_receipts(
                        fixture["proof"])

    def test_each_historical_role_receipt_is_hash_sealed(
            self) -> None:
        for role in self._ROLES:
            for mutation in ("missing", "altered"):
                with self.subTest(role=role, mutation=mutation), \
                        tempfile.TemporaryDirectory() as tmp:
                    fixture = self._historical_chain(Path(tmp))
                    receipts = fixture["evidence_receipts"]
                    assert isinstance(receipts, dict)
                    receipt = receipts[role]
                    assert isinstance(receipt, Path)
                    if mutation == "missing":
                        receipt.unlink()
                    else:
                        receipt.write_text(
                            receipt.read_text(encoding="utf-8")
                            + " ",
                            encoding="utf-8",
                        )
                    with (
                        mock.patch.object(
                            server,
                            "_CACHE_UNIT_REPORTING_DIR",
                            fixture["unit_dir"],
                        ),
                        mock.patch.object(
                            server,
                            "_CACHE_EVIDENCE_REPORTING_DIRS",
                            (fixture["evidence_dir"],),
                        ),
                        self.assertRaisesRegex(
                            RuntimeError,
                            "proof-ready native report receipts "
                            "are incomplete",
                        ),
                    ):
                        server._find_proof_cache_receipts(
                            fixture["proof"])

    def test_retired_final_bundle_contains_current_and_historical_rosters(
            self) -> None:
        transition_sha256 = "7" * 64
        with tempfile.TemporaryDirectory() as tmp:
            fixture = self._retired_lifecycle_fixture(Path(tmp))
            current = fixture["current"]
            assert isinstance(current, dict)
            transition = (
                fixture["retirement_dir"]
                / f"transition_{current['source_revision']}.json"
            )
            self._write_json(transition, {
                "schema_version":
                    "bioseq-cache-retirement-transition-v1",
                "retired_source_revision":
                    current["source_revision"],
                "retirement_transition_sha256": transition_sha256,
            })
            with contextlib.ExitStack() as stack:
                stream = self._patch_retired_lifecycle(
                    stack, fixture, transition_sha256, 0)
                result = server._run_cache_retirement_lifecycle_gate(
                    "cache-bioseq-bench",
                    fixture["active_bin_dir"],
                    fixture["active_build_dir"],
                    {},
                    current,
                    io.StringIO(),
                    float("inf"),
                )

            assert result is not None
            bundle = json.loads(
                Path(result["bundle"]).read_text(encoding="utf-8"))
            current_receipts = fixture["current_receipts"]
            historical_receipts = fixture["evidence_receipts"]
            assert isinstance(current_receipts, list)
            assert isinstance(historical_receipts, dict)
            self.assertEqual(bundle["phase"], "retired-final")
            self.assertEqual(
                bundle["unit_receipt"],
                str(fixture["current_unit"].resolve(strict=True)),
            )
            self.assertEqual(
                bundle["evidence_receipts"],
                [
                    str(path.resolve(strict=True))
                    for path in current_receipts
                ],
            )
            self.assertEqual(
                bundle["proof_unit_receipt"],
                str(fixture["unit_receipt"].resolve(strict=True)),
            )
            self.assertEqual(
                bundle["proof_evidence_receipts"],
                [
                    str(
                        historical_receipts[role].resolve(
                            strict=True))
                    for role in self._ROLES
                ],
            )
            self.assertEqual(
                bundle["transition_receipt"],
                str(transition.resolve(strict=True)),
            )
            self.assertEqual(
                bundle["retirement_transition_sha256"],
                transition_sha256,
            )
            gate_command = stream.call_args.args[0]
            gate_snapshot = stream.call_args.kwargs[
                "verified_executable"]
            self.assertEqual(
                gate_command[0], str(fixture["gate"]))
            self.assertEqual(
                gate_snapshot.parent,
                fixture["gate"].parent.resolve(strict=True),
            )
            self.assertNotEqual(
                gate_snapshot,
                fixture["gate"].resolve(strict=True),
            )
            self.assertFalse(gate_snapshot.exists())

    def test_retired_final_requires_revision_pinned_transition_receipt(
            self) -> None:
        transition_sha256 = "7" * 64
        for mutation in ("missing", "wrong-revision-name"):
            with self.subTest(mutation=mutation), \
                    tempfile.TemporaryDirectory() as tmp:
                fixture = self._retired_lifecycle_fixture(Path(tmp))
                current = fixture["current"]
                assert isinstance(current, dict)
                if mutation == "wrong-revision-name":
                    wrong = (
                        fixture["retirement_dir"]
                        / f"transition_{'9' * 40}.json"
                    )
                    self._write_json(wrong, {
                        "schema_version":
                            "bioseq-cache-retirement-transition-v1",
                    })
                with contextlib.ExitStack() as stack:
                    stream = self._patch_retired_lifecycle(
                        stack, fixture, transition_sha256, 0)
                    with self.assertRaises(FileNotFoundError):
                        server._run_cache_retirement_lifecycle_gate(
                            "cache-bioseq-bench",
                            fixture["active_bin_dir"],
                            fixture["active_build_dir"],
                            {},
                            current,
                            io.StringIO(),
                            float("inf"),
                        )
                    stream.assert_not_called()

    def test_retired_final_propagates_transition_semantic_rejection(
            self) -> None:
        transition_sha256 = "7" * 64
        mutations = {
            "missing-revision": (
                "retired_source_revision", None),
            "altered-revision": (
                "retired_source_revision", "9" * 40),
            "missing-digest": (
                "retirement_transition_sha256", None),
            "altered-digest": (
                "retirement_transition_sha256", "8" * 64),
        }
        for mutation, (field, replacement) in mutations.items():
            with self.subTest(mutation=mutation), \
                    tempfile.TemporaryDirectory() as tmp:
                fixture = self._retired_lifecycle_fixture(Path(tmp))
                current = fixture["current"]
                assert isinstance(current, dict)
                transition = (
                    fixture["retirement_dir"]
                    / f"transition_{current['source_revision']}.json"
                )
                receipt = {
                    "schema_version":
                        "bioseq-cache-retirement-transition-v1",
                    "retired_source_revision":
                        current["source_revision"],
                    "retirement_transition_sha256":
                        transition_sha256,
                }
                if replacement is None:
                    receipt.pop(field)
                else:
                    receipt[field] = replacement
                self._write_json(transition, receipt)

                def native_gate(
                        argv, *_args, **_kwargs) -> int:
                    bundle = json.loads(
                        Path(argv[2]).read_text(encoding="utf-8"))
                    observed = json.loads(
                        Path(bundle["transition_receipt"]).read_text(
                            encoding="utf-8"))
                    if (
                        observed.get("retired_source_revision")
                            != current["source_revision"]
                        or observed.get(
                            "retirement_transition_sha256")
                            != transition_sha256
                    ):
                        return 73
                    return 0

                with contextlib.ExitStack() as stack:
                    self._patch_retired_lifecycle(
                        stack, fixture, transition_sha256, native_gate)
                    with self.assertRaisesRegex(
                            RuntimeError, "exited with code 73"):
                        server._run_cache_retirement_lifecycle_gate(
                            "cache-bioseq-bench",
                            fixture["active_bin_dir"],
                            fixture["active_build_dir"],
                            {},
                            current,
                            io.StringIO(),
                            float("inf"),
                        )

    def test_cache_unit_receipt_seals_five_executables_and_exact_argv(
            self) -> None:
        provenance = self._provenance()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            reporting = root / "reporting"
            bin_dir.mkdir()
            resolved_commands: list[list[str]] = []
            for command in server.MODES["cache-unit"].commands:
                executable = bin_dir / command[0]
                executable.write_bytes(command[0].encode("utf-8"))
                arguments = [
                    argument.replace(
                        "{legacy_inventory}",
                        str(root / "legacy_asset_inventory.txt"),
                    )
                    for argument in command[1:]
                ]
                resolved_commands.append(
                    [str(executable), *arguments])

            with (
                mock.patch.object(
                    server, "_CACHE_UNIT_REPORTING_DIR", reporting),
                mock.patch.object(
                    server,
                    "_run_command_argv",
                    side_effect=resolved_commands,
                ),
            ):
                command_records = [
                    server._stable_native_command_identity(command)
                    for command in resolved_commands
                ]
                receipt_path = server._seal_cache_unit_receipt(
                    bin_dir, provenance, command_records)

            receipt = json.loads(
                receipt_path.read_text(encoding="utf-8"))
        self.assertEqual(len(receipt["commands"]), 5)
        self.assertEqual(
            {
                Path(command["executable"]).name
                for command in receipt["commands"]
            },
            {
                "bioseq_cache_unit_tests",
                "bioseq_postgres_test_support_contract_tests",
                "bioseq_cache_benchmark_report_contract_tests",
                "bioseq_durable_file_io_contract_tests",
                "bioseq_cache_legacy_inventory_tracking",
            },
        )
        self.assertEqual(
            receipt["commands"][-1]["arguments"][-3:],
            ["--phase", "current", "--inventory-only"],
        )

    def test_cache_unit_rejects_executable_replacement_during_command(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            executable = root / "bioseq_cache_unit_tests"
            executable.write_bytes(b"before")

            def resolve_command(
                    _mode, _active_bin_dir, _command, **_kwargs):
                return [str(executable)]

            def replace_executable(*_args, **_kwargs):
                replacement = root / "replacement"
                replacement.write_bytes(b"after")
                server.os.replace(replacement, executable)
                return 0

            with (
                mock.patch.object(
                    server, "_run_command_argv",
                    side_effect=resolve_command),
                mock.patch.object(
                    server, "_stream_subprocess",
                    side_effect=replace_executable),
                mock.patch.object(server, "_update_entry"),
            ):
                with self.assertRaisesRegex(
                        RuntimeError,
                        "verified handle|identity changed during"):
                    server._run_mode_commands(
                        "cache-unit",
                        root,
                        root,
                        {},
                        io.StringIO(),
                        float("inf"),
                        set(),
                    )

    def test_verified_snapshot_executes_a_when_logical_path_is_swapped_to_b(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            logical = root / "native-command"
            replacement = root / "replacement"
            displaced = root / "displaced"
            marker = root / "executed"
            observer = root / "observe_argv0.py"
            server.shutil.copyfile(sys.executable, logical)
            observer.write_text(
                "import ctypes\n"
                "import sys\n"
                "from pathlib import Path\n"
                "if sys.platform == 'darwin':\n"
                "    libc = ctypes.CDLL(None)\n"
                "    pointer = ctypes.POINTER\n"
                "    libc._NSGetArgv.restype = pointer(pointer("
                "ctypes.c_char_p))\n"
                "    native_argv0 = libc._NSGetArgv().contents[0]"
                ".decode()\n"
                "else:\n"
                "    native_argv0 = Path('/proc/self/cmdline')"
                ".read_bytes().split(b'\\0', 1)[0].decode()\n"
                "Path(sys.argv[1]).write_text("
                "'A\\n' + native_argv0)\n",
                encoding="utf-8",
            )
            replacement.write_text(
                "#!/bin/sh\n"
                f"printf B > {server.shlex.quote(str(marker))}\n",
                encoding="utf-8",
            )
            logical.chmod(0o700)
            replacement.chmod(0o700)
            invocation = [
                str(logical),
                str(observer),
                str(marker),
            ]

            with self.assertRaisesRegex(
                    RuntimeError, "verified handle"):
                with server._verified_native_command_execution(
                        invocation) as (record, snapshot):
                    self.assertEqual(
                        record["executable"],
                        str(logical.resolve(strict=True)),
                    )
                    self.assertEqual(
                        snapshot.parent,
                        logical.parent.resolve(strict=True),
                    )
                    self.assertNotEqual(
                        snapshot, logical.resolve(strict=True))
                    server.os.replace(logical, displaced)
                    server.os.replace(replacement, logical)
                    try:
                        rc = server._stream_subprocess(
                            invocation,
                            io.StringIO(),
                            server.time.time() + 10,
                            cwd=root,
                            verified_executable=snapshot,
                        )
                    finally:
                        server.os.replace(logical, replacement)
                        server.os.replace(displaced, logical)

            self.assertEqual(rc, 0)
            marker_lines = marker.read_text(
                encoding="utf-8").splitlines()
            self.assertEqual(marker_lines, ["A", invocation[0]])
            self.assertFalse(snapshot.exists())
            self.assertEqual(
                server._stable_native_command_identity(invocation)[
                    "executable_sha256"],
                record["executable_sha256"],
            )

    def test_verified_snapshot_tampering_fails_closed(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            logical = root / "native-command"
            server.shutil.copyfile(sys.executable, logical)
            logical.chmod(0o700)
            invocation = [str(logical), "-c", "pass"]

            with self.assertRaisesRegex(
                    RuntimeError, "verified handle"):
                with server._verified_native_command_execution(
                        invocation) as (_record, snapshot):
                    snapshot.chmod(0o700)
                    snapshot.write_bytes(b"tampered")

            self.assertFalse(snapshot.exists())

    def test_verified_snapshot_cleanup_failures_are_not_suppressed(
            self) -> None:
        path_type = type(Path("."))
        for mutation, expected in (
                ("unlink-error", "cannot remove"),
                ("leftover", "still exists")):
            with self.subTest(mutation=mutation), \
                    tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                logical = root / "native-command"
                server.shutil.copyfile(sys.executable, logical)
                logical.chmod(0o700)
                invocation = [str(logical), "-c", "pass"]
                snapshot: Path | None = None

                def fail_unlink(
                        _path: Path, *_args, **_kwargs) -> None:
                    raise PermissionError("injected unlink failure")

                unlink_patch = (
                    {"side_effect": fail_unlink}
                    if mutation == "unlink-error"
                    else {"return_value": None}
                )
                try:
                    with (
                        self.assertRaisesRegex(
                            RuntimeError, expected),
                        mock.patch.object(
                            path_type,
                            "unlink",
                            autospec=True,
                            **unlink_patch,
                        ),
                    ):
                        with (
                            server._verified_native_command_execution(
                                invocation)
                        ) as (_record, snapshot):
                            pass
                finally:
                    if snapshot is not None and snapshot.exists():
                        snapshot.unlink()

    def test_verified_snapshot_preserves_streaming_and_timeout(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            logical = root / "native-command"
            server.shutil.copyfile(sys.executable, logical)
            logical.chmod(0o700)
            invocation = [
                str(logical),
                "-c",
                "import time; print('snapshot-stream', flush=True); "
                "time.sleep(30)",
            ]
            output = io.StringIO()

            with self.assertRaisesRegex(
                    TimeoutError, "hard timeout"):
                with server._verified_native_command_execution(
                        invocation) as (_record, snapshot):
                    server._stream_subprocess(
                        invocation,
                        output,
                        server.time.time() + 0.25,
                        cwd=root,
                        verified_executable=snapshot,
                    )

            self.assertIn("snapshot-stream", output.getvalue())
            self.assertFalse(snapshot.exists())

    def test_cleanup_root_contract_rejects_unmarked_path(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            canonical_temp = Path(
                tempfile.gettempdir()).resolve(strict=True)
            unmarked = (
                canonical_temp
                / f"ordinary-temp-root-{server.secrets.token_hex(8)}"
            )
            with self.assertRaisesRegex(
                    RuntimeError, "not marker-owned"):
                server._cache_cleanup_root_contracts(
                    [{"root": str(unmarked)}])
            wrong_parent = (
                Path(tmp).resolve(strict=True)
                / f"bioseq-pg-test-{server.secrets.token_hex(8)}"
            )
            with self.assertRaisesRegex(
                    RuntimeError, "canonical temporary directory"):
                server._cache_cleanup_root_contracts(
                    [{"root": str(wrong_parent)}])

    def test_cleanup_absence_rejects_existing_and_symlinked_paths(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve(strict=True)
            existing = root / "bioseq-pg-test-existing"
            existing.mkdir()
            with self.assertRaisesRegex(
                    RuntimeError, "still exists"):
                server._require_absent_nofollow_path(
                    existing, "cleanup root")

            target = root / "target"
            target.mkdir()
            linked_parent = root / "linked-parent"
            linked_parent.symlink_to(target, target_is_directory=True)
            through_symlink = (
                linked_parent / "bioseq-pg-test-absent")
            with self.assertRaisesRegex(
                    RuntimeError, "traverses a symlink"):
                server._require_absent_nofollow_path(
                    through_symlink, "cleanup root")

            provider_root = root / "bioseq-pg-test-provider"
            runtime_library = provider_root / "runtime-library"
            runtime_library.mkdir(parents=True)
            provider_dso = runtime_library / "bioseq_cache.dylib"
            provider_dso.write_bytes(b"reappeared")
            with self.assertRaisesRegex(
                    RuntimeError, "still exists"):
                server._require_absent_nofollow_path(
                    provider_dso, "provider staged DSO")

    def test_failed_terminal_report_rejects_unsafe_cleanup_roots(
            self) -> None:
        canonical_temp = Path(
            tempfile.gettempdir()).resolve(strict=True)
        with tempfile.TemporaryDirectory() as tmp:
            fixture = Path(tmp).resolve(strict=True)
            report = fixture / "report.json"
            mutations = ("existing", "symlink", "unmarked", "wrong-parent")
            for mutation in mutations:
                with self.subTest(mutation=mutation):
                    nonce = server.secrets.token_hex(8)
                    if mutation == "unmarked":
                        cleanup_root = (
                            canonical_temp / f"ordinary-{nonce}")
                    elif mutation == "wrong-parent":
                        cleanup_root = (
                            fixture / f"bioseq-pg-test-{nonce}")
                    else:
                        cleanup_root = (
                            canonical_temp
                            / f"bioseq-pg-test-{mutation}-{nonce}"
                        )
                    if mutation == "existing":
                        cleanup_root.mkdir()
                    elif mutation == "symlink":
                        cleanup_root.symlink_to(
                            fixture / f"absent-target-{nonce}",
                            target_is_directory=True,
                        )
                    report.write_text(json.dumps({
                        "schema_version": "bioseq-cache-test-v2",
                        "cleanup": {"succeeded": True},
                        "cleanup_observations": [{
                            "succeeded": True,
                            "root": str(cleanup_root),
                        }],
                        "cluster_configuration": {
                            "retained-supervision-state": "none",
                        },
                    }), encoding="utf-8")
                    try:
                        with self.assertRaises(RuntimeError):
                            server._validate_failed_cache_terminal_report(
                                report)
                    finally:
                        if cleanup_root.is_symlink():
                            cleanup_root.unlink()
                        elif cleanup_root.exists():
                            cleanup_root.rmdir()

    def test_failed_terminal_report_rechecks_cleanup_root_before_acceptance(
            self) -> None:
        canonical_temp = Path(
            tempfile.gettempdir()).resolve(strict=True)
        cleanup_root = (
            canonical_temp
            / (
                "bioseq-pg-test-terminal-window-"
                + server.secrets.token_hex(8)
            )
        )
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "report.json"
            report.write_text(json.dumps({
                "schema_version": "bioseq-cache-test-v2",
                "cleanup": {"succeeded": True},
                "cleanup_observations": [{
                    "succeeded": True,
                    "root": str(cleanup_root),
                }],
                "cluster_configuration": {
                    "retained-supervision-state": "none",
                },
            }), encoding="utf-8")
            real_check = (
                server._require_cache_cleanup_roots_absent)
            calls = 0

            def mutate_after_first_check(
                    roots: list[Path], context: str) -> None:
                nonlocal calls
                calls += 1
                real_check(roots, context)
                if calls == 1:
                    roots[0].mkdir()

            try:
                with (
                    mock.patch.object(
                        server,
                        "_require_cache_cleanup_roots_absent",
                        side_effect=mutate_after_first_check,
                    ),
                    self.assertRaisesRegex(
                        RuntimeError, "still exists"),
                ):
                    server._validate_failed_cache_terminal_report(
                        report)
            finally:
                if cleanup_root.exists():
                    cleanup_root.rmdir()
            self.assertEqual(calls, 2)

    def test_retired_cache_unit_gate_failure_precedes_receipt_publication(
            self) -> None:
        provenance = self._provenance("retired_with_proof")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build_dir = root / "build"
            bin_dir = build_dir / "bioseq_caching"
            build_dir.mkdir()
            bin_dir.mkdir()
            (build_dir / "build.ninja").write_text(
                "# test fixture\n", encoding="utf-8")
            state = {
                "cache-unit": {
                    "started_at": server.time.time(),
                    "log_file": str(root / "worker.log"),
                    "rt_priority": None,
                },
            }
            gate_failure = RuntimeError(
                "historical transition proof rejected")

            with contextlib.ExitStack() as stack:
                def patch(
                        name: str, **kwargs: object) -> object:
                    return stack.enter_context(
                        mock.patch.object(server, name, **kwargs))

                patch(
                    "_refuse_root_build_identity", return_value=None)
                patch("_ensure_build_worker_timesharing_policy")
                patch("_load_state", return_value=state)
                patch("preset_for", return_value="cache-opt")
                patch("build_dir_for", return_value=build_dir)
                patch("bin_dir_for", return_value=bin_dir)
                patch(
                    "_cache_qualification_provenance",
                    return_value=provenance)
                patch(
                    "_build_worker_descendant_environment",
                    return_value={})
                patch("_ensure_local_toolchain_environment")
                patch("_is_aws_clang_preset", return_value=False)
                patch("_is_aws_gcc_preset", return_value=False)
                patch("_is_aws_preset", return_value=False)
                patch(
                    "_configured_local_build_rt_priority",
                    return_value=None)
                patch(
                    "_local_clang_rt_is_enabled",
                    return_value=False)
                patch("_preflight_local_clang_rt_launcher")
                patch(
                    "_cmake_preset_cache_contract", return_value={})
                patch(
                    "_initial_clang_rt_cmake_args", return_value=[])
                patch("_cmake_cache_bool", return_value=False)
                patch(
                    "_cmake_preset_cache_repairs", return_value={})
                patch(
                    "_postgresql_discovery_cache_reset_args",
                    return_value=[])
                patch(
                    "_clang_rt_cmake_cache_repairs",
                    return_value=({}, []))
                patch("_stream_subprocess", return_value=0)
                patch(
                    "_run_mode_commands",
                    return_value=(None, [], [], []))
                patch(
                    "_cache_run_provenance", return_value=provenance)
                lifecycle_gate = patch(
                    "_run_cache_retirement_lifecycle_gate",
                    side_effect=gate_failure)
                seal_mode = patch("_seal_cache_mode_receipts")
                seal_unit = patch("_seal_cache_unit_receipt")
                patch("_update_entry")
                result = server._run_build_worker("cache-unit")

        self.assertEqual(result, 1)
        lifecycle_gate.assert_called_once()
        seal_mode.assert_not_called()
        seal_unit.assert_not_called()

    def test_receipt_sealing_rejects_captured_benchmark_policy_override(
            self) -> None:
        provenance = self._provenance()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            executable = bin_dir / "bioseq_cache_bench"
            executable.write_bytes(b"bench")
            report = root / "report_0.json"
            report.write_text("{}", encoding="utf-8")
            policy = [
                str(executable),
                "--scenario", "standalone",
                "--samples", "7",
            ]
            captured = [*policy, "--samples", "1"]
            with (
                mock.patch.object(
                    server, "_run_command_argv", return_value=policy),
                mock.patch.object(
                    server, "_seal_cache_native_report") as seal,
            ):
                with self.assertRaisesRegex(
                        RuntimeError, "fixed mode policy"):
                    server._seal_cache_mode_receipts(
                        "cache-bench",
                        bin_dir,
                        [report],
                        [captured],
                        [],
                        provenance,
                    )
                seal.assert_not_called()

    def test_native_report_must_match_frozen_concrete_invocation(
            self) -> None:
        provenance = self._provenance()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            executable = bin_dir / "bioseq_cache_integration_tests"
            executable.write_bytes(b"integration")
            report = root / "report_0.json"
            report.write_text("{}", encoding="utf-8")
            invocation = [
                str(executable),
                "--migration-harness", "cache-integration",
                "--migration-report-role", "cache-integration",
                "--report", str(report),
            ]
            native = {
                "report_role": "cache-integration",
                "scenario": "cache-integration",
                "arguments": [
                    *invocation[:-1],
                    str(root / "wrong-report.json"),
                ],
                "executable": str(executable),
                "executable_sha256":
                    server._sha256_path(executable),
                "migration_ledger_sha256":
                    provenance["migration_ledger_sha256"],
                "evidence_contract_sha256":
                    provenance["evidence_contract_sha256"],
                "migration_phase": provenance["migration_phase"],
            }
            with mock.patch.object(
                    server,
                    "_cache_native_report_contract",
                    return_value=native):
                with self.assertRaisesRegex(
                        RuntimeError, "exact MCP command"):
                    server._seal_cache_native_report(
                        "cache-integration",
                        bin_dir,
                        invocation,
                        report,
                        provenance,
                    )

    def test_support_report_seal_requires_exact_five_artifact_roster(
            self) -> None:
        provenance = self._provenance()

        def seal(mutation: str | None) -> dict[str, object]:
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp).resolve(strict=True)
                bin_dir = root / "bin"
                source_dir = root / "source"
                primary_stage = root / "primary-stage"
                bioseq_only_stage = root / "bioseq-only-stage"
                for directory in (
                        bin_dir,
                        source_dir,
                        primary_stage,
                        bioseq_only_stage):
                    directory.mkdir()

                def write(
                        path: Path, contents: bytes) -> Path:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(contents)
                    return path.resolve(strict=True)

                executable = write(
                    bin_dir
                    / "bioseq_cache_bioseq_support_integration_tests",
                    b"support executable",
                )
                report = write(root / "report_0.json", b"{}\n")
                primary_manifest = write(
                    primary_stage / "stage-manifest.json",
                    b"primary manifest",
                )
                bioseq_only_manifest = write(
                    bioseq_only_stage / "stage-manifest.json",
                    b"bioseq-only manifest",
                )
                cache_control = write(
                    source_dir / "bioseq_cache.control",
                    b"cache control source",
                )
                cache_sql = write(
                    source_dir / "bioseq_cache--1.0.sql",
                    b"cache install sql",
                )
                cache_dso = write(
                    source_dir / "bioseq_cache.dylib",
                    b"cache dso",
                )
                bioseq_control = write(
                    source_dir / "bioseq.control",
                    b"bioseq control source",
                )
                bioseq_sql = write(
                    source_dir / "bioseq--1.0.sql",
                    b"bioseq install sql",
                )
                bioseq_dso = write(
                    source_dir / "bioseq.dylib",
                    b"bioseq dso",
                )
                provider_dso = write(
                    source_dir / "provider-peer.dylib",
                    b"provider peer dso",
                )
                cache_staged_control = write(
                    primary_stage
                    / "share/extension/bioseq_cache.control",
                    b"cache staged control",
                )
                cache_staged_sql = write(
                    primary_stage
                    / "share/extension/bioseq_cache--1.0.sql",
                    cache_sql.read_bytes(),
                )
                cache_staged_dso = write(
                    primary_stage / "lib/bioseq_cache.dylib",
                    cache_dso.read_bytes(),
                )
                bioseq_staged_control = write(
                    primary_stage / "share/extension/bioseq.control",
                    b"bioseq staged control",
                )
                bioseq_staged_sql = write(
                    primary_stage
                    / "share/extension/bioseq--1.0.sql",
                    bioseq_sql.read_bytes(),
                )
                bioseq_staged_dso = write(
                    primary_stage / "lib/bioseq.dylib",
                    bioseq_dso.read_bytes(),
                )
                bioseq_only_staged_control = write(
                    bioseq_only_stage
                    / "share/extension/bioseq.control",
                    b"bioseq-only staged control",
                )
                bioseq_only_staged_sql = write(
                    bioseq_only_stage
                    / "share/extension/bioseq--1.0.sql",
                    bioseq_sql.read_bytes(),
                )
                bioseq_only_staged_dso = write(
                    bioseq_only_stage / "lib/bioseq.dylib",
                    bioseq_dso.read_bytes(),
                )
                canonical_temp = Path(
                    tempfile.gettempdir()).resolve(strict=True)
                cleanup_roots = [
                    canonical_temp
                    / (
                        "bioseq-pg-test-provider-"
                        + server.secrets.token_hex(8)
                    )
                    for _ in range(2)
                ]
                provider_staged_dsos = [
                    cleanup_root
                    / "runtime-library"
                    / cache_staged_dso.name
                    for cleanup_root in cleanup_roots
                ]

                def artifact(
                        name: str,
                        stage_root: Path,
                        stage_manifest: Path,
                        control: Path,
                        install_sql: Path,
                        dso: Path,
                        staged_control: Path,
                        staged_install_sql: Path,
                        staged_dso: Path,
                ) -> dict[str, object]:
                    return {
                        "name": name,
                        "preinstalled_stage_root":
                            str(stage_root.resolve(strict=True)),
                        "stage_manifest": str(stage_manifest),
                        "stage_manifest_sha256":
                            server._sha256_path(stage_manifest),
                        "control_file": str(control),
                        "install_sql": str(install_sql),
                        "dso": str(dso),
                        "staged_control_file": str(staged_control),
                        "staged_install_sql":
                            str(staged_install_sql),
                        "staged_dso": str(staged_dso),
                        "control_sha256":
                            server._sha256_path(control),
                        "install_sql_sha256":
                            server._sha256_path(install_sql),
                        "dso_sha256": server._sha256_path(dso),
                        "staged_control_sha256":
                            server._sha256_path(staged_control),
                    }

                canonical_cache = artifact(
                    "bioseq_cache",
                    primary_stage,
                    primary_manifest,
                    cache_control,
                    cache_sql,
                    cache_dso,
                    cache_staged_control,
                    cache_staged_sql,
                    cache_staged_dso,
                )
                artifacts = [
                    canonical_cache,
                    artifact(
                        "bioseq",
                        primary_stage,
                        primary_manifest,
                        bioseq_control,
                        bioseq_sql,
                        bioseq_dso,
                        bioseq_staged_control,
                        bioseq_staged_sql,
                        bioseq_staged_dso,
                    ),
                    artifact(
                        "bioseq",
                        bioseq_only_stage,
                        bioseq_only_manifest,
                        bioseq_control,
                        bioseq_sql,
                        bioseq_dso,
                        bioseq_only_staged_control,
                        bioseq_only_staged_sql,
                        bioseq_only_staged_dso,
                    ),
                    *[
                        artifact(
                            "bioseq_cache",
                            primary_stage,
                            primary_manifest,
                            cache_control,
                            cache_sql,
                            provider_dso,
                            cache_staged_control,
                            cache_staged_sql,
                            staged_dso,
                        )
                        for staged_dso in provider_staged_dsos
                    ],
                ]
                if mutation == "missing-provider":
                    artifacts.pop()
                elif mutation == "duplicate-provider-runtime":
                    artifacts[-1]["staged_dso"] = artifacts[-2][
                        "staged_dso"]
                elif mutation == "wrong-provider-shared-path":
                    alternate = write(
                        primary_stage
                        / "alternate/bioseq_cache.control",
                        b"alternate staged control",
                    )
                    artifacts[-1]["staged_control_file"] = str(
                        alternate)
                    artifacts[-1]["staged_control_sha256"] = (
                        server._sha256_path(alternate)
                    )
                elif mutation == "wrong-provider-hash":
                    artifacts[-1]["dso_sha256"] = "0" * 64
                artifacts = server._cache_native_artifact_contracts(
                    artifacts)

                invocation = [
                    str(executable),
                    "--postgres-stage-root", str(primary_stage),
                    "--postgres-stage-manifest",
                    str(primary_manifest),
                    "--bioseq-only-postgres-stage-root",
                    str(bioseq_only_stage),
                    "--bioseq-only-postgres-stage-manifest",
                    str(bioseq_only_manifest),
                    "--cache-control", str(cache_control),
                    "--cache-install-sql", str(cache_sql),
                    "--cache-dso", str(cache_dso),
                    "--bioseq-control", str(bioseq_control),
                    "--bioseq-install-sql", str(bioseq_sql),
                    "--bioseq-dso", str(bioseq_dso),
                    "--provider-peer-test-dso", str(provider_dso),
                    "--migration-harness",
                    "cache-bioseq-integration",
                    "--migration-report-role",
                    "cache-bioseq-support-integration",
                    "--report", str(report),
                ]
                native = {
                    "report_role":
                        "cache-bioseq-support-integration",
                    "scenario": "cache-bioseq-support",
                    "arguments": invocation,
                    "executable": str(executable),
                    "executable_sha256":
                        server._sha256_path(executable),
                    "migration_ledger_sha256":
                        provenance["migration_ledger_sha256"],
                    "evidence_contract_sha256":
                        provenance["evidence_contract_sha256"],
                    "migration_phase":
                        provenance["migration_phase"],
                    "artifacts": artifacts,
                    "cleanup_roots": [
                        str(path) for path in cleanup_roots
                    ],
                }
                original_receipt_path = (
                    server._cache_report_receipt_path)

                def recreate_cleanup_root(report_path: Path) -> Path:
                    output = original_receipt_path(report_path)
                    cleanup_roots[0].mkdir()
                    return output

                try:
                    with contextlib.ExitStack() as stack:
                        stack.enter_context(mock.patch.object(
                            server,
                            "_cache_native_report_contract",
                            return_value=native,
                        ))
                        if (
                            mutation
                            == "recreate-cleanup-before-publication"
                        ):
                            stack.enter_context(mock.patch.object(
                                server,
                                "_cache_report_receipt_path",
                                side_effect=recreate_cleanup_root,
                            ))
                        receipt_path = (
                            server._seal_cache_native_report(
                                "cache-bioseq-integration",
                                bin_dir,
                                invocation,
                                report,
                                provenance,
                            )
                        )
                    return json.loads(
                        receipt_path.read_text(encoding="utf-8"))
                finally:
                    if cleanup_roots[0].exists():
                        cleanup_roots[0].rmdir()

        receipt = seal(None)
        self.assertEqual(
            receipt["report_role"],
            "cache-bioseq-support-integration",
        )
        for mutation in (
                "missing-provider",
                "duplicate-provider-runtime",
                "wrong-provider-shared-path",
                "wrong-provider-hash",
                "recreate-cleanup-before-publication"):
            with self.subTest(mutation=mutation), \
                    self.assertRaises(RuntimeError):
                seal(mutation)

    def test_exact_2gbp_runtime_budget_reserves_finalization_window(
            self) -> None:
        argv = [
            "/bin/bioseq_cache_bioseq_production_integration_tests",
            "--large-case-timeout-seconds",
            "6300",
        ]
        bounded = server._cache_apply_runtime_budget(
            "cache-bioseq-integration",
            argv,
            deadline=2_000.0,
            now=1_000.0,
        )
        self.assertEqual(
            bounded[-1],
            str(int(
                1_000
                - server.CACHE_EXACT2G_FINALIZATION_RESERVE_SEC)),
        )
        self.assertEqual(
            server._cache_apply_runtime_budget(
                "cache-bioseq-integration",
                argv,
                deadline=float("inf"),
                now=1_000.0,
            ),
            argv,
        )
        with self.assertRaisesRegex(
                TimeoutError, "cleanup/report reserve"):
            server._cache_apply_runtime_budget(
                "cache-bioseq-integration",
                argv,
                deadline=1_500.0,
                now=1_000.0,
            )

    def test_failed_exact_2gbp_report_must_prove_zero_retained_state(
            self) -> None:
        cleanup_root = (
            Path(tempfile.gettempdir()).resolve(strict=True)
            / f"bioseq-pg-test-{server.secrets.token_hex(8)}"
        )
        report = {
            "schema_version": "bioseq-cache-test-v2",
            "cleanup": {"succeeded": True},
            "cleanup_observations": [{
                "succeeded": True,
                "root": str(cleanup_root),
            }],
            "cluster_configuration": {
                "retained-supervision-state": "none"},
        }
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "report.json"
            path.write_text(json.dumps(report), encoding="utf-8")
            server._validate_failed_cache_terminal_report(path)
            report["cluster_configuration"][
                "retained-supervision-state"] = "watchdog.json"
            path.write_text(json.dumps(report), encoding="utf-8")
            with self.assertRaisesRegex(
                    RuntimeError, "zero retained supervision state"):
                server._validate_failed_cache_terminal_report(path)

    def test_combined_integration_has_two_hour_mode_wide_timeout(
            self) -> None:
        self.assertEqual(
            server.MODES[
                "cache-bioseq-integration"].hard_timeout_min,
            120.0,
        )
        self.assertIn("main/bioseq", server._CACHE_SOURCE_SCOPES)
        self.assertIn("main/bioseq_caching", server._CACHE_SOURCE_SCOPES)
        self.assertIn("main/build_mcp", server._CACHE_SOURCE_SCOPES)
        self.assertIn("main/common", server._CACHE_SOURCE_SCOPES)
        self.assertIn("main/cmake", server._CACHE_SOURCE_SCOPES)
        self.assertIn("main/codegen", server._CACHE_SOURCE_SCOPES)
        self.assertIn("main/overlays", server._CACHE_SOURCE_SCOPES)
        self.assertIn("main/vendor", server._CACHE_SOURCE_SCOPES)
        self.assertIn("main/vcpkg.json", server._CACHE_SOURCE_SCOPES)
        self.assertIn(
            "main/vcpkg-configuration.json",
            server._CACHE_SOURCE_SCOPES,
        )
        self.assertIn(
            ":(top,exclude,glob)main/cmake/**/__pycache__/**",
            server._CACHE_SOURCE_SCOPES,
        )
        self.assertNotIn(".codex/config.toml", server._CACHE_SOURCE_SCOPES)
        self.assertNotIn(
            "main/cmake/__pycache__/sanitizer_runtime.cpython-310.pyc",
            server._CACHE_SOURCE_SCOPES,
        )

    def test_only_release_phases_start_clean_source_qualification(
            self) -> None:
        with (
            mock.patch.object(
                server,
                "_cache_migration_contract",
                return_value={"migration_phase": "mapped"},
            ) as contract,
            mock.patch.object(
                server, "_cache_run_provenance") as provenance,
        ):
            self.assertIsNone(
                server._cache_qualification_provenance(
                    "cache-unit", False))
            provenance.assert_not_called()
            self.assertIsNone(
                server._cache_qualification_provenance(
                    "cache-unit", True))
            self.assertEqual(contract.call_count, 1)

        expected = self._provenance()
        with (
            mock.patch.object(
                server,
                "_cache_migration_contract",
                return_value={"migration_phase": "passing"},
            ),
            mock.patch.object(
                server,
                "_cache_run_provenance",
                return_value=expected,
            ) as provenance,
        ):
            self.assertEqual(
                server._cache_qualification_provenance(
                    "cache-bioseq-bench", False),
                expected,
            )
            provenance.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
