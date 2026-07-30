from __future__ import annotations

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

    def test_native_benchmark_declares_v1_report_and_2gbp_contract(
            self) -> None:
        source = (
            MAIN_DIR
            / "bioseq_caching"
            / "bench"
            / "cache_benchmark.cppm"
        ).read_text(encoding="utf-8")
        for required in (
            "bioseq-cache-benchmark-v1",
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
            (bench_dir / "report_schema_v1.json").read_text(
                encoding="utf-8"))

        self.assertEqual(
            contract["schema_version"], "bioseq-cache-benchmark-v1")
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
        self.assertEqual(
            contract["semantics"]["samples"],
            "measured warm cache-hit samples only",
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

        self.assertIn("postgres_cluster_rss_bytes", source)
        self.assertIn("descendants{postmaster_pid}", source)
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
                report_file, report_files = server._run_mode_commands(
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


if __name__ == "__main__":
    unittest.main()
