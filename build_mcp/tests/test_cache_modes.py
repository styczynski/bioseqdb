from __future__ import annotations

import io
import json
import multiprocessing
import os
import platform as host_platform
import queue
import re
import secrets
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = BUILD_MCP_DIR.parent
sys.path.insert(0, str(BUILD_MCP_DIR))

import build_presets  # noqa: E402
import build_worker  # noqa: E402
import cache_modes  # noqa: E402
import integrity  # noqa: E402
import inspect_tool  # noqa: E402
import process_state  # noqa: E402
import protocol  # noqa: E402


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
    "production_dso_qualification_receipt",
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
})


class CachePresetContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        payload = json.loads((REPOSITORY_ROOT / "CMakePresets.json").read_text())
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

    def test_core_debug_preset_retains_inspection_artifact_flags(self) -> None:
        variables = self.presets["bench-opt-debug"]["cacheVariables"]
        flags = variables["CMAKE_CXX_FLAGS"]
        self.assertIn("-save-temps=obj", flags.split())
        self.assertIn("-g", flags.split())


class CacheModeResolutionTest(unittest.TestCase):
    def test_benchmark_filter_rejects_unknown_axes(self) -> None:
        arguments, error = build_presets._filter_to_cli_args({
            "unknown_axis": ["silently-dropping-this-is-unsafe"],
        })
        self.assertEqual(arguments, [])
        self.assertEqual(
            error,
            "filter contains unknown field(s): unknown_axis",
        )

    def _patch_toolchain(self, fast: str, debug: str):
        return mock.patch.multiple(
            build_presets,
            DEFAULT_BUILD_PRESET=fast,
            DEBUG_BUILD_PRESET=debug,
            TEST_BUILD_PRESET=build_presets._default_test_preset(fast),
            TEST_DEBUG_BUILD_PRESET=build_presets._default_test_preset(debug),
            CACHE_BUILD_PRESET_OVERRIDE=None,
            CACHE_DEBUG_BUILD_PRESET_OVERRIDE=None,
            CACHE_BIOSEQ_BUILD_PRESET_OVERRIDE=None,
            CACHE_BIOSEQ_DEBUG_BUILD_PRESET_OVERRIDE=None,
        )

    def test_all_modes_resolve_for_local_aws_clang_and_aws_gcc(self) -> None:
        for fast, debug_preset in (LOCAL, AWS_CLANG, AWS_GCC):
            with self.subTest(toolchain=fast), self._patch_toolchain(
                    fast, debug_preset):
                for mode, details in build_presets.MODES.items():
                    for debug in (False, True):
                        with self.subTest(mode=mode, debug=debug):
                            core_preset = debug_preset if debug else fast
                            if details.preset_override:
                                expected = details.preset_override
                            elif details.preset_family == "core":
                                expected = core_preset
                            elif details.preset_family == "test":
                                expected = (
                                    build_presets._default_test_preset(
                                        core_preset)
                                )
                            elif details.preset_family == "cache":
                                expected = build_presets._default_cache_preset(
                                    core_preset, combined=False)
                            elif details.preset_family == "cache-bioseq":
                                expected = build_presets._default_cache_preset(
                                    core_preset, combined=True)
                            elif details.preset_family == "website":
                                expected = (
                                    build_presets.WEBSITE_DEBUG_BUILD_PRESET
                                    if debug else build_presets.WEBSITE_BUILD_PRESET)
                            else:
                                self.fail(
                                    "unhandled preset family "
                                    f"{details.preset_family!r}")

                            self.assertEqual(
                                build_presets.preset_for(mode, debug), expected)
                            self.assertEqual(
                                build_presets.build_dir_for(mode, debug),
                                build_presets.REPOSITORY_ROOT / "build" / expected,
                            )
                            self.assertEqual(
                                build_presets.bin_dir_for(mode, debug),
                                build_presets.REPOSITORY_ROOT / "build" / expected
                                / details.binary_subdir,
                            )

    def test_cache_specific_environment_overrides_win(self) -> None:
        with mock.patch.multiple(
            build_presets,
            CACHE_BUILD_PRESET_OVERRIDE="custom-cache-fast",
            CACHE_DEBUG_BUILD_PRESET_OVERRIDE="custom-cache-debug",
            CACHE_BIOSEQ_BUILD_PRESET_OVERRIDE="custom-combined-fast",
            CACHE_BIOSEQ_DEBUG_BUILD_PRESET_OVERRIDE="custom-combined-debug",
        ):
            self.assertEqual(
                build_presets.preset_for("cache-unit", False), "custom-cache-fast")
            self.assertEqual(
                build_presets.preset_for("cache-unit", True), "custom-cache-debug")
            self.assertEqual(
                build_presets.preset_for("cache-bioseq-integration", False),
                "custom-combined-fast",
            )
            self.assertEqual(
                build_presets.preset_for("cache-bioseq-integration", True),
                "custom-combined-debug",
            )

    def test_custom_preset_fallback_keeps_debug_suffix_last(self) -> None:
        self.assertEqual(
            build_presets._default_cache_preset("toolchain-debug", combined=False),
            "toolchain-cache-debug",
        )
        self.assertEqual(
            build_presets._default_cache_preset("toolchain-debug", combined=True),
            "toolchain-cache-bioseq-debug",
        )


class CacheModeRoutingTest(unittest.TestCase):

    def test_cache_integration_reports_pin_the_cache_schema(self) -> None:
        runner = (
            REPOSITORY_ROOT
            / "bioseq_caching"
            / "tests"
            / "support"
            / "cache_catch_runner.cpp"
        ).read_text(encoding="utf-8")
        self.assertEqual(
            runner.count(".schema_revision = 2"),
            2,
        )
        report_model = (
            REPOSITORY_ROOT
            / "common"
            / "postgres"
            / "test_support"
            / "report.cppm"
        ).read_text(encoding="utf-8")
        self.assertIn("std::uint32_t schema_revision{1}", report_model)
        self.assertIn('key(output, "schema_revision")', report_model)
        self.assertNotIn("schema_version", report_model)
        for key in cache_modes._CACHE_INTEGRATION_REPORT_KEYS:
            with self.subTest(integration_report_key=key):
                self.assertIn(f'"{key}"', report_model)

    def test_native_report_contract_rejects_aliases_and_unknown_keys(
            self) -> None:
        payload = {
            key: None
            for key in cache_modes._CACHE_INTEGRATION_REPORT_KEYS
        }
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "report.json"
            for invalid_revision in (True, "2"):
                with self.subTest(revision=invalid_revision):
                    payload["schema_revision"] = invalid_revision
                    report.write_text(json.dumps(payload), encoding="utf-8")
                    with self.assertRaisesRegex(
                            RuntimeError, "schema_revision"):
                        cache_modes._cache_native_report_contract(report)

            payload["schema_revision"] = 2
            payload["schema_version"] = "retired-authored-identity"
            report.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(
                    RuntimeError, "unknown=.*schema_version"):
                cache_modes._cache_native_report_contract(report)

    def test_integration_case_contract_pins_unfiltered_cardinalities(
            self) -> None:
        expected_counts = {
            "bioseq_cache_integration_tests": 50,
            "bioseq_cache_bioseq_support_integration_tests": 8,
            "bioseq_cache_bioseq_large_input_e2e_tests": 1,
        }
        for executable, expected_count in expected_counts.items():
            cases = [
                {
                    "test_case": f"case-{index}",
                    "section": "",
                    "status": "passed",
                }
                for index in range(expected_count)
            ]
            with self.subTest(executable=executable):
                self.assertEqual(
                    cache_modes._cache_integration_case_contract(
                        cases, executable, focused=False),
                    expected_count,
                )
                with self.assertRaisesRegex(
                        RuntimeError, "unfiltered contract requires exactly"):
                    cache_modes._cache_integration_case_contract(
                        cases[:-1], executable, focused=False)
                self.assertEqual(
                    cache_modes._cache_integration_case_contract(
                        [], executable, focused=True),
                    0,
                )
                with self.assertRaisesRegex(
                        RuntimeError, "exceeds.*unfiltered closure"):
                    cache_modes._cache_integration_case_contract(
                        [
                            *cases,
                            {
                                "test_case": "unexpected-extra-case",
                                "section": "",
                                "status": "passed",
                            },
                        ],
                        executable,
                        focused=True,
                    )

    def test_integration_case_contract_rejects_duplicate_or_failed_cases(
            self) -> None:
        duplicate = {
            "test_case": "duplicate",
            "section": "",
            "status": "passed",
        }
        with self.assertRaisesRegex(RuntimeError, "repeats a top-level"):
            cache_modes._cache_integration_case_contract(
                [dict(duplicate), dict(duplicate)],
                "bioseq_cache_integration_tests",
                focused=True,
            )
        with self.assertRaisesRegex(RuntimeError, "non-passing"):
            cache_modes._cache_integration_case_contract(
                [{**duplicate, "status": "failed"}],
                "bioseq_cache_integration_tests",
                focused=True,
            )

    def test_integration_argv_contract_requires_exact_focused_suffix(
            self) -> None:
        runner_argv = [
            "/tmp/bioseq_cache_integration_tests",
            "--report", "/tmp/report.json",
            "--case-timeout-seconds", "480",
            "--reporter", "console::out=-::colour-mode=ansi",
        ]
        focused_flags = ["--allow-running-no-tests", "[cache-correctness]"]
        options = cache_modes._cache_integration_argv_contract(
            [*runner_argv, *focused_flags], focused_flags)
        self.assertEqual(options["--case-timeout-seconds"], "480")
        with self.assertRaisesRegex(RuntimeError, "exact focused"):
            cache_modes._cache_integration_argv_contract(
                [*runner_argv, "--allow-running-no-tests", "[other]"],
                focused_flags,
            )
        with self.assertRaisesRegex(
                RuntimeError, "missing option value|unrequested Catch2 filter"):
            cache_modes._cache_integration_argv_contract(
                [*runner_argv, "[unrequested]"], [])

    def test_focused_zero_match_report_is_a_valid_sibling_receipt(
            self) -> None:
        executable = "/tmp/bioseq_cache_integration_tests"
        focused_flags = ["--allow-running-no-tests", "[sibling-only]"]
        argv = [
            executable,
            "--report", "/tmp/report.json",
            "--case-timeout-seconds", "480",
            "--reporter", "console::out=-::colour-mode=ansi",
            *focused_flags,
        ]
        payload = {
            "schema_revision": 2,
            "executable": executable,
            "build_identity": "a" * 64,
            "arguments": [
                {
                    "name": f"argv[{index}]",
                    "value": argument,
                    "redacted": False,
                }
                for index, argument in enumerate(argv)
            ],
            "cluster_configuration": {
                "retained-supervision-state": "none",
            },
            "cases": [],
            "stage_timings": [],
            "host_qualifications": [],
            "sql_transcripts": [],
            "artifacts": [],
            "cleanup_observations": [],
            "resources": {},
            "cleanup": {"succeeded": True},
        }
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / "focused-empty.json"
            report.write_text(json.dumps(payload), encoding="utf-8")
            contract = cache_modes._cache_native_report_contract(
                report, test_flags=focused_flags)
        self.assertEqual(contract["selected_case_count"], 0)
        self.assertEqual(contract["arguments"], argv)
        self.assertEqual(contract["artifacts"], [])

    def test_native_benchmark_declares_revision_2_report_and_2gbp_contract(
            self) -> None:
        source = (
            REPOSITORY_ROOT
            / "bioseq_caching"
            / "bench"
            / "cache_benchmark.cppm"
        ).read_text(encoding="utf-8")
        for required in (
            "schema_revision",
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
        bench_dir = REPOSITORY_ROOT / "bioseq_caching" / "bench"
        source = (
            bench_dir / "cache_benchmark.cppm"
        ).read_text(encoding="utf-8")
        contract = json.loads(
            (bench_dir / "cache_benchmark_report.schema.json").read_text(
                encoding="utf-8"))

        self.assertEqual(
            contract["$id"],
            "https://bioseq.dev/schemas/cache-benchmark-report.schema.json",
        )
        self.assertEqual(contract["schema_revision"], 2)
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
            set(contract["required"]),
            cache_modes._CACHE_BENCHMARK_REPORT_KEYS,
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
        self.assertIn("postgres_test::cluster_resource_sampler", source)
        self.assertIn("peak_postgres_rss_bytes", source)
        self.assertIn("benchmark_resource_tracker", source)
        self.assertIn("rss_sample_interval", source)
        self.assertIn("temp_sample_interval", source)
        self.assertNotIn("RUSAGE_SELF", source)

        self.assertIn(
            "postgres_test::collect_artifact_observations("
            "\n        fixture.cluster().staged_artifacts())",
            source,
        )
        self.assertLess(
            source.index(
                "report.artifacts = "
                "postgres_test::collect_artifact_observations"
            ),
            source.index("benchmark_resource_tracker resource_tracker"),
        )
        self.assertIn(
            "benchmark_resource_report* destination_", source)
        self.assertIn(
            "write_failure_report(", source)
        self.assertIn(
            "postgres_test::hash_sha256_file(executable).render_hex()",
            source,
        )
        self.assertIn("::uname(&information)", source)
        self.assertIn("finalize_build_identity(report)", source)

    def test_cache_cmake_generates_artifact_manifest_from_target_properties(
            self) -> None:
        cmake = (
            REPOSITORY_ROOT
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
            cache_modes._CACHE_NATIVE_ARTIFACT_KEYS,
            EXPECTED_CACHE_NATIVE_ARTIFACT_KEYS,
        )
        for key in cache_modes._CACHE_NATIVE_ARTIFACT_KEYS:
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
        self.assertIn("Threads::Threads", cmake)
        self.assertIn(
            "bioseq_cache_benchmark_report_contract_tests", cmake)
        self.assertIn("cache_benchmark_report_test.cpp", cmake)
        self.assertNotIn("run_private_cluster_bench.py", cmake)
        cache_unit = build_presets.MODES["cache-unit"]
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
    def test_combined_mode_owns_host_native_dso_qualification(self) -> None:
        root_cmake = (REPOSITORY_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        helper = (
            REPOSITORY_ROOT / "cmake" / "bioseq_production_dso_qualification.cmake"
        ).read_text(encoding="utf-8")
        qualifier = (
            REPOSITORY_ROOT / "cmake" / "qualify_production_extension_dsos.py"
        ).read_text(encoding="utf-8")
        bench_cmake = (
            REPOSITORY_ROOT / "bioseq_caching" / "bench" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")

        target = "bioseq_cache_bioseq_production_dso_qualification"
        self.assertIn(
            "bioseq_add_production_dso_qualification(", root_cmake)
        self.assertIn("BIOSEQ_TARGET postgresql_bioseq", root_cmake)
        self.assertIn("CACHE_TARGET postgresql_bioseq_cache", root_cmake)
        self.assertIn(
            "cmake/tests/contract/test_production_dso_qualification.py",
            root_cmake)
        self.assertIn("LANGUAGES C CXX", root_cmake)
        self.assertIn(
            'set(CMAKE_C_COMPILER "${CMAKE_C_COMPILER}"', root_cmake)
        self.assertIn(
            'set(CMAKE_CXX_COMPILER "${CMAKE_CXX_COMPILER}"', root_cmake)
        for mode in ("cache-bioseq-integration", "cache-bioseq-benchmark"):
            with self.subTest(mode=mode):
                self.assertIn(target, build_presets.MODES[mode].targets)
        for expression in (
            "$<TARGET_FILE:${BIOSEQ_DSO_QUALIFY_BIOSEQ_TARGET}>",
            "$<TARGET_FILE:${BIOSEQ_DSO_QUALIFY_CACHE_TARGET}>",
            ".bioseq-build-mcp-configure.json",
            "CMakeCache.txt",
        ):
            self.assertIn(expression, helper)
        self.assertNotIn("test_support", helper)
        self.assertNotIn(".sql.in", helper)
        self.assertIn('CMAKE_SYSTEM_NAME STREQUAL "Darwin"', helper)
        self.assertIn("NAMES otool", helper)
        self.assertIn('CMAKE_SYSTEM_NAME STREQUAL "Linux"', helper)
        self.assertIn("NAMES readelf", helper)
        self.assertIn("CMAKE_${_language}_${_field}", helper)
        self.assertIn(
            "requires an identified ${_language}", helper)
        self.assertIn("BIOSEQ_PRODUCTION_DSO_QUALIFICATION_RECEIPT", helper)
        self.assertIn("_macho_dependencies", qualifier)
        self.assertIn("_macho_uuids", qualifier)
        self.assertIn("_elf_dependencies", qualifier)
        self.assertIn("_elf_build_ids", qualifier)
        self.assertIn("sibling_dependency_absent", qualifier)
        self.assertIn("content_sha256", qualifier)
        self.assertIn("host_platform.machine", qualifier)
        self.assertIn("identity_kind", qualifier)
        self.assertIn(
            '\\"production_dso_qualification_receipt\\"', bench_cmake)

    def test_cache_runtime_targets_use_profile_exact_postgres_stages(
            self) -> None:
        cache_cmake = (
            REPOSITORY_ROOT
            / "bioseq_caching"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        main_cmake = (
            REPOSITORY_ROOT
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        integration_cmake = (
            REPOSITORY_ROOT
            / "bioseq_caching"
            / "tests"
            / "integration"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        combined_cmake = (
            REPOSITORY_ROOT
            / "bioseq_caching"
            / "tests"
            / "integration"
            / "bioseq"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        production_e2e_cmake = (
            REPOSITORY_ROOT
            / "bioseq_caching"
            / "tests"
            / "e2e"
            / "bioseq"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        bench_cmake = (
            REPOSITORY_ROOT
            / "bioseq_caching"
            / "bench"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        helper_cmake = (
            REPOSITORY_ROOT
            / "cmake"
            / "bioseq_private_postgres_stage.cmake"
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
                production_e2e_cmake,
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
        for cmake in (
                integration_cmake, combined_cmake, production_e2e_cmake):
            self.assertNotIn(
                '--postgres-bin-dir "${PostgreSQL_BIN_DIR}"', cmake)
        self.assertNotIn("MODULE_PATHNAME_OVERRIDES", cache_cmake)
        self.assertNotIn("MODULE_PATHNAME_OVERRIDES", main_cmake)

    def test_cache_runner_uses_attested_preinstalled_stage_and_narrow_peer_override(
            self) -> None:
        support_dir = (
            REPOSITORY_ROOT
            / "bioseq_caching"
            / "tests"
            / "support"
        )
        context = (support_dir / "cache_test_context.cppm").read_text(
            encoding="utf-8")
        runner = (support_dir / "cache_catch_runner.cpp").read_text(
            encoding="utf-8")
        peer = (
            REPOSITORY_ROOT
            / "bioseq_caching"
            / "tests"
            / "integration"
            / "bioseq"
            / "provider_peer_failopen_test.cpp"
        ).read_text(encoding="utf-8")
        coexistence = (
            REPOSITORY_ROOT
            / "bioseq_caching"
            / "tests"
            / "integration"
            / "bioseq"
            / "coexistence_test.cpp"
        ).read_text(encoding="utf-8")
        acceptance = (
            REPOSITORY_ROOT
            / "bioseq_caching"
            / "tests"
            / "integration"
            / "bioseq"
            / "coexistence_acceptance_test.cpp"
        ).read_text(encoding="utf-8")

        for source_name, source in (
                ("context", context), ("runner", runner)):
            with self.subTest(source=source_name):
                self.assertIn("postgres_stage_root", source)
                self.assertIn("postgres_stage_manifest", source)
        self.assertIn("--postgres-stage-root", runner)
        self.assertIn("--postgres-stage-manifest", runner)
        for executable_seal in (
            "executed_executable_sha256",
            "sha256_file(",
            "hash_sha256_file(executed_executable).render_hex()",
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
            REPOSITORY_ROOT
            / "common"
            / "postgres"
            / "test_support"
            / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        checked_arithmetic_cmake = (
            REPOSITORY_ROOT
            / "common"
            / "checked_arithmetic"
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
        self.assertIn(
            "add_executable(\n"
            "    bioseq_checked_arithmetic_contract_tests",
            checked_arithmetic_cmake,
        )
        self.assertEqual(
            checked_arithmetic_cmake.count(runtime_directory), 1)

    def test_cache_runtime_modes_have_no_post_build_targets(
            self) -> None:
        for mode in (
            "cache-integration",
            "cache-benchmark",
            "cache-bioseq-integration",
            "cache-bioseq-benchmark",
        ):
            with self.subTest(mode=mode):
                self.assertEqual(build_presets.post_build_targets_for(mode, False), ())
                self.assertEqual(build_presets.post_build_targets_for(mode, True), ())

    def test_existing_install_modes_remain_unchanged(self) -> None:
        install_modes = {
            name for name, details in build_presets.MODES.items()
            if details.install_extension
        }
        self.assertEqual(
            install_modes, {"unit-integration", "benchmark-e2e"})
        for name in install_modes:
            with self.subTest(mode=name):
                self.assertIn(
                    "postgresql_bioseq_cache", build_presets.MODES[name].targets)
                self.assertIn("postgresql_bioseq", build_presets.MODES[name].targets)
        for name, details in build_presets.MODES.items():
            if name.startswith("cache-"):
                self.assertFalse(details.install_extension, name)
                if details.uses_private_cluster:
                    self.assertTrue(details.commands, name)

    def test_private_cluster_mode_cannot_install_shared_extension(self) -> None:
        with self.assertRaisesRegex(ValueError, "private-cluster modes"):
            build_presets.ModeDefinition(
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
        result = build_worker.tool_start_build(
            "cache-benchmark", bench_filter={"tags": ["cache"]})
        self.assertIn("require a benchmark CLI", result)
        self.assertIn("cache-benchmark", result)

    def test_native_cache_mode_commands_and_timeouts(self) -> None:
        primary_postgres_bin_dir_key_by_executable = {
            "bioseq_cache_integration_tests":
                "cache_test_support_postgres_bin_dir",
            "bioseq_cache_benchmark":
                "cache_production_postgres_bin_dir",
            "bioseq_cache_bioseq_support_integration_tests":
                "cache_bioseq_support_postgres_bin_dir",
            "bioseq_cache_bioseq_large_input_e2e_tests":
                "cache_bioseq_production_postgres_bin_dir",
            "bioseq_cache_bioseq_benchmark":
                "cache_bioseq_production_postgres_bin_dir",
        }
        all_stage_prefixes = {
            "cache_test_support_postgres",
            "cache_production_postgres",
            "cache_bioseq_support_postgres",
            "cache_bioseq_only_postgres",
            "cache_bioseq_production_postgres",
        }
        case_timeout_by_executable = {
            "bioseq_cache_integration_tests": "480",
            "bioseq_cache_bioseq_support_integration_tests": "480",
            "bioseq_cache_bioseq_large_input_e2e_tests": "6300",
        }
        scenario_by_executable = {
            "bioseq_cache_benchmark": "standalone",
            "bioseq_cache_bioseq_benchmark": "bioseq-2gbp",
        }
        expected = {
            "cache-integration": (
                ("bioseq_cache_integration_tests",),
                8.0,
            ),
            "cache-benchmark": (
                ("bioseq_cache_benchmark",),
                20.0,
            ),
            "cache-bioseq-integration": (
                (
                    "bioseq_cache_bioseq_support_integration_tests",
                    "bioseq_cache_bioseq_large_input_e2e_tests",
                ),
                140.0,
            ),
            "cache-bioseq-benchmark": (
                ("bioseq_cache_bioseq_benchmark",),
                180.0,
            ),
        }
        for mode, (executables, timeout) in expected.items():
            with self.subTest(mode=mode):
                details = build_presets.MODES[mode]
                self.assertEqual(details.expected_duration_min, timeout)
                if mode == "cache-bioseq-integration":
                    self.assertEqual(details.hard_timeout_min, 140.0)
                self.assertEqual(
                    tuple(command[0] for command in details.commands),
                    executables,
                )
                self.assertTrue(details.reporting_subdir_name)
                for command in details.commands:
                    executable = command[0]
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
                    case_timeout = case_timeout_by_executable.get(executable)
                    if case_timeout is not None:
                        timeout_index = command.index(
                            "--case-timeout-seconds")
                        self.assertEqual(
                            command[timeout_index + 1], case_timeout)
                    else:
                        self.assertNotIn("--case-timeout-seconds", command)
                    scenario = scenario_by_executable.get(executable)
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

    def test_cache_integration_resolves_exact_cmake_parity_argv(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary_dir = Path(tmp)
            manifest = {
                key: str(binary_dir / key)
                for key in cache_modes._CACHE_NATIVE_ARTIFACT_KEYS
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

            resolved = build_worker._run_command_argv(
                "cache-integration",
                binary_dir,
                build_presets.MODES["cache-integration"].commands[0],
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
                for key in cache_modes._CACHE_NATIVE_ARTIFACT_KEYS
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

            support_resolved = build_worker._run_command_argv(
                "cache-bioseq-integration",
                binary_dir,
                build_presets.MODES["cache-bioseq-integration"].commands[0],
                report_path=support_report,
            )
            production_resolved = build_worker._run_command_argv(
                "cache-bioseq-integration",
                binary_dir,
                build_presets.MODES["cache-bioseq-integration"].commands[1],
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
                    / "bioseq_cache_bioseq_large_input_e2e_tests"
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
                "--report", str(production_report),
                "--case-timeout-seconds", "6300",
                "--reporter", "console::out=-::colour-mode=ansi",
            ],
        )

    def test_cache_modes_contain_no_script_runner_invocations(self) -> None:
        forbidden = (".py", ".pl", ".pm", "python", "perl")
        for mode, details in build_presets.MODES.items():
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
                for key in cache_modes._CACHE_NATIVE_ARTIFACT_KEYS
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
            command = build_presets.MODES["cache-bioseq-benchmark"].commands[0]
            resolved = build_worker._run_command_argv(
                "cache-bioseq-benchmark",
                binary_dir,
                command,
                report_path=report,
            )
        self.assertEqual(
            resolved[0], str(binary_dir / "bioseq_cache_bioseq_benchmark"))
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
                for key in cache_modes._CACHE_NATIVE_ARTIFACT_KEYS
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
                build_worker._run_command_argv(
                    "cache-bioseq-benchmark",
                    binary_dir,
                    build_presets.MODES["cache-bioseq-benchmark"].commands[0],
                    report_path=binary_dir / "report_0.json",
                )

    @staticmethod
    def _seal_dso_document(
            document: dict[str, object]) -> dict[str, object]:
        result = dict(document)
        result.pop("content_sha256", None)
        result["content_sha256"] = integrity.canonical_json_sha256(result)
        return result

    @classmethod
    def _production_dso_fixture(
            cls, root: Path) -> dict[str, object]:
        root = root.resolve(strict=True)
        system = host_platform.system()
        if system not in {"Darwin", "Linux"}:
            raise unittest.SkipTest("DSO qualification supports Darwin/Linux")
        machine = cache_modes._qualification_architecture(
            host_platform.machine(), "fixture host architecture")
        suffix = ".dylib" if system == "Darwin" else ".so"
        binary_format = "Mach-O" if system == "Darwin" else "ELF"
        build_dir = root / "cache-bioseq-fixture"
        binary_dir = build_dir / "bioseq_caching"
        binary_dir.mkdir(parents=True)
        tool = root / ("otool" if system == "Darwin" else "readelf")
        c_compiler = root / "cc"
        cxx_compiler = root / "c++"
        for executable in (c_compiler, cxx_compiler):
            executable.write_bytes((executable.name + "\n").encode())
            executable.chmod(0o755)
        dsos = {
            "bioseq_dso": build_dir / f"bioseq{suffix}",
            "cache_dso": build_dir / f"bioseq_cache{suffix}",
        }
        for key, path in dsos.items():
            path.write_bytes((key + "\n").encode())
        if system == "Darwin":
            tool.write_text(
                "#!/bin/sh\n"
                "case \"$1\" in\n"
                "  -hv) printf '%s\\n' 'Mach header' "
                f"'MH_MAGIC_64 {machine.upper()} ALL 0x6 0 0 0 0' ;;\n"
                "  -L) printf '%s\\n' \"$2:\" "
                "'\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, "
                "current version 1.0.0)' ;;\n"
                "  -l) case \"$2\" in *bioseq_cache*) "
                "uuid=22222222-2222-2222-2222-222222222222 ;; "
                "*) uuid=11111111-1111-1111-1111-111111111111 ;; esac; "
                "printf '%s\\n' 'Load command 1' '      cmd LC_UUID' "
                "\"     uuid $uuid\" ;;\n"
                "  *) exit 2 ;;\n"
                "esac\n",
                encoding="utf-8",
            )
        else:
            elf_machine = (
                "AArch64" if machine == "arm64"
                else "Advanced Micro Devices X86-64")
            tool.write_text(
                "#!/bin/sh\n"
                "case \"$1\" in\n"
                "  -h) printf '%s\\n' 'ELF Header:' "
                "'  Class:                             ELF64' "
                "\"  Data:                              2's complement, "
                "little endian\" "
                f"'  Machine:                           {elf_machine}' ;;\n"
                "  -d) printf '%s\\n' 'Dynamic section:' "
                "' 0x0000000000000001 (NEEDED) Shared library: "
                "[libc.so.6]' ;;\n"
                "  -n) case \"$2\" in *bioseq_cache*) id=5678efab ;; "
                "*) id=1234abcd ;; esac; "
                "printf '    Build ID: %s\\n' \"$id\" ;;\n"
                "  *) exit 2 ;;\n"
                "esac\n",
                encoding="utf-8",
            )
        tool.chmod(0o755)
        cmake_cache = build_dir / "CMakeCache.txt"
        cmake_cache.write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n"
            f"CMAKE_C_COMPILER:FILEPATH={c_compiler}\n"
            f"CMAKE_CXX_COMPILER:FILEPATH={cxx_compiler}\n",
            encoding="utf-8",
        )
        configure_inputs = {"preset": "cache-bioseq-fixture"}
        configure_stamp = build_dir / build_presets._CMAKE_CONFIGURE_STAMP_NAME
        configure_stamp.write_text(json.dumps({
            "artifact_kind": build_presets._CMAKE_CONFIGURE_STAMP_KIND,
            "schema_revision": 1,
            "inputs": configure_inputs,
            "inputs_sha256":
                integrity.canonical_json_sha256(configure_inputs),
        }), encoding="utf-8")

        def identity(path: Path) -> dict[str, object]:
            return {
                "lexical_path": str(path),
                "resolved_path": str(path.resolve(strict=True)),
                "size_bytes": path.stat().st_size,
                "sha256": integrity.sha256_file(path),
            }

        compiler_context = {
            "c": {
                **identity(c_compiler),
                "compiler_id": "FixtureC",
                "compiler_version": "1.2.3",
            },
            "cxx": {
                **identity(cxx_compiler),
                "compiler_id": "FixtureCXX",
                "compiler_version": "4.5.6",
            },
        }
        binary = (
            {"format": "Mach-O", "architectures": [machine]}
            if system == "Darwin" else {
                "format": "ELF",
                "architectures": [machine],
                "class": "ELF64",
                "data": "2's complement, little endian",
                "machine": (
                    "AArch64" if machine == "arm64"
                    else "Advanced Micro Devices X86-64"),
                "architecture": machine,
            }
        )
        dso_rows = []
        for extension, key, identity_value in (
            (
                "bioseq", "bioseq_dso",
                "11111111-1111-1111-1111-111111111111"
                if system == "Darwin" else "1234abcd"),
            (
                "bioseq_cache", "cache_dso",
                "22222222-2222-2222-2222-222222222222"
                if system == "Darwin" else "5678efab"),
        ):
            sibling = "bioseq_cache" if extension == "bioseq" else "bioseq"
            if system == "Darwin":
                header_listing = (
                    "Mach header\n"
                    f"MH_MAGIC_64 {machine.upper()} ALL 0x6 0 0 0 0\n")
                dependency_listing = (
                    f"{dsos[key]}:\n"
                    "\t/usr/lib/libSystem.B.dylib (compatibility version "
                    "1.0.0, current version 1.0.0)\n")
                identity_listing = (
                    "Load command 1\n"
                    "      cmd LC_UUID\n"
                    f"     uuid {identity_value}\n")
            else:
                header_listing = (
                    "ELF Header:\n"
                    "  Class:                             ELF64\n"
                    "  Data:                              2's complement, "
                    "little endian\n"
                    f"  Machine:                           {binary['machine']}\n")
                dependency_listing = (
                    "Dynamic section:\n"
                    " 0x0000000000000001 (NEEDED) Shared library: "
                    "[libc.so.6]\n")
                identity_listing = f"    Build ID: {identity_value}\n"
            dso_rows.append({
                "extension": extension,
                "path": str(dsos[key]),
                "basename": dsos[key].name,
                "size_bytes": dsos[key].stat().st_size,
                "sha256": integrity.sha256_file(dsos[key]),
                "binary": dict(binary),
                "header_listing_sha256": integrity.sha256_bytes(
                    header_listing.encode("utf-8")),
                "build_identity": {
                    "kind": (
                        "mach-o-lc-uuid" if system == "Darwin"
                        else "elf-gnu-build-id"),
                    "values": [identity_value],
                },
                "direct_load_dependencies": [
                    "/usr/lib/libSystem.B.dylib"
                    if system == "Darwin" else "libc.so.6"],
                "dependency_listing_sha256": integrity.sha256_bytes(
                    dependency_listing.encode("utf-8")),
                "build_identity_listing_sha256": integrity.sha256_bytes(
                    identity_listing.encode("utf-8")),
                "sibling_extension": sibling,
                "sibling_basename": f"{sibling}{suffix}",
                "sibling_dependency_absent": True,
            })
        document = cls._seal_dso_document({
            "schema_revision": 1,
            "artifact_kind": "bioseq-production-dso-load-qualification",
            "platform": {
                "system_name": system,
                "system_release": host_platform.release(),
                "machine": machine,
                "binary_format": binary_format,
                "dso_suffix": suffix,
            },
            "inspection_tool": {
                **identity(tool),
                "identity_kind": "executable-sha256",
                "header_arguments": ["-hv"] if system == "Darwin" else ["-h"],
                "dependency_arguments": ["-L"] if system == "Darwin" else ["-d"],
                "build_identity_arguments": ["-l"] if system == "Darwin" else ["-n"],
            },
            "build_context": {
                "preset": "cache-bioseq-fixture",
                "configuration": "Release",
                "build_type": "Release",
                "generator": "Ninja",
                "build_directory": str(build_dir),
                "cmake_system": {"name": system, "processor": machine},
                "cmake_cache": {
                    "path": str(cmake_cache),
                    "size_bytes": cmake_cache.stat().st_size,
                    "sha256": integrity.sha256_file(cmake_cache),
                },
                "configure_input": {
                    "path": str(configure_stamp),
                    "size_bytes": configure_stamp.stat().st_size,
                    "sha256": integrity.sha256_file(configure_stamp),
                    "inputs_sha256":
                        integrity.canonical_json_sha256(configure_inputs),
                },
                "compilers": compiler_context,
            },
            "dsos": dso_rows,
        })
        receipt = binary_dir / "production-dso-qualification.json"
        receipt.write_text(json.dumps(document), encoding="utf-8")
        manifest = {
            key: str(binary_dir / key)
            for key in cache_modes._CACHE_NATIVE_ARTIFACT_KEYS
        }
        manifest.update({key: str(path) for key, path in dsos.items()})
        manifest["production_dso_qualification_receipt"] = str(receipt)
        (binary_dir / "cache_native_artifacts.json").write_text(
            json.dumps(manifest), encoding="utf-8")
        return {
            "binary_dir": binary_dir,
            "build_dir": build_dir,
            "receipt": receipt,
            "document": document,
            "tool": tool,
            "cmake_cache": cmake_cache,
            "configure_stamp": configure_stamp,
            "dsos": dsos,
        }

    def test_builder_rejects_dso_receipt_and_live_context_tampering(
            self) -> None:
        def mutate_schema(
                fixture: dict[str, object], mutation: str) -> None:
            document = dict(fixture["document"])
            document.pop("content_sha256", None)
            if mutation == "legacy":
                document["schema_version"] = document.pop("schema_revision")
            elif mutation == "legacy-kind":
                document["artifact_kind"] = (
                    "bioseq-production-dso-load-qualification-v1")
            elif mutation == "wrong":
                document["schema_revision"] = 2
            elif mutation == "boolean":
                document["schema_revision"] = True
            else:
                document["unexpected"] = True
            fixture["receipt"].write_text(
                json.dumps(self._seal_dso_document(document)), encoding="utf-8")

        mutations = {
            "receipt seal": lambda fixture: fixture["receipt"].write_text(
                json.dumps({**fixture["document"], "content_sha256": "0" * 64}),
                encoding="utf-8"),
            "inspection tool": lambda fixture: fixture["tool"].write_bytes(
                b"tampered tool\n"),
            "CMake cache": lambda fixture: fixture["cmake_cache"].write_text(
                "tampered cache\n", encoding="utf-8"),
            "configure input": lambda fixture: fixture[
                "configure_stamp"].write_text("{}", encoding="utf-8"),
            "production DSO": lambda fixture: fixture["dsos"][
                "bioseq_dso"].write_bytes(b"tampered DSO\n"),
            "legacy schema key": lambda fixture: mutate_schema(fixture, "legacy"),
            "legacy artifact kind": lambda fixture: mutate_schema(
                fixture, "legacy-kind"),
            "wrong schema revision": lambda fixture: mutate_schema(fixture, "wrong"),
            "boolean schema revision": lambda fixture: mutate_schema(
                fixture, "boolean"),
            "unknown receipt key": lambda fixture: mutate_schema(fixture, "unknown"),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as tmp:
                fixture = self._production_dso_fixture(Path(tmp))
                mutate(fixture)
                with self.assertRaises(RuntimeError):
                    cache_modes._cache_bioseq_dso_qualification_evidence(
                        fixture["binary_dir"],
                        expected_build_dir=fixture["build_dir"],
                        expected_preset="cache-bioseq-fixture",
                    )

    def test_builder_rejects_missing_compiler_cache_coordinate(self) -> None:
        for language in ("C", "CXX"):
            with self.subTest(language=language), \
                    tempfile.TemporaryDirectory() as tmp:
                fixture = self._production_dso_fixture(Path(tmp))
                cache = fixture["cmake_cache"]
                retained = [
                    line for line in cache.read_text(encoding="utf-8").splitlines()
                    if not line.startswith(f"CMAKE_{language}_COMPILER:")
                ]
                cache.write_text("\n".join(retained) + "\n", encoding="utf-8")
                document = fixture["document"]
                document["build_context"]["cmake_cache"].update({
                    "size_bytes": cache.stat().st_size,
                    "sha256": integrity.sha256_file(cache),
                })
                sealed = self._seal_dso_document(document)
                fixture["receipt"].write_text(
                    json.dumps(sealed), encoding="utf-8")
                with self.assertRaisesRegex(
                        RuntimeError,
                        "current CMake cache context differs"):
                    cache_modes._cache_bioseq_dso_qualification_evidence(
                        fixture["binary_dir"],
                        expected_build_dir=fixture["build_dir"],
                        expected_preset="cache-bioseq-fixture",
                    )

    def test_builder_rejects_platform_and_binary_architecture_tampering(
            self) -> None:
        for field in ("platform", "binary"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as tmp:
                fixture = self._production_dso_fixture(Path(tmp))
                document = fixture["document"]
                assert isinstance(document, dict)
                if field == "platform":
                    document["platform"]["machine"] = (
                        "x86_64"
                        if document["platform"]["machine"] == "arm64"
                        else "arm64")
                else:
                    document["dsos"][0]["binary"]["architectures"] = [
                        "x86_64"
                        if document["platform"]["machine"] == "arm64"
                        else "arm64"]
                sealed = self._seal_dso_document(document)
                fixture["receipt"].write_text(
                    json.dumps(sealed), encoding="utf-8")
                with self.assertRaises(RuntimeError):
                    cache_modes._cache_bioseq_dso_qualification_evidence(
                        fixture["binary_dir"],
                        expected_build_dir=fixture["build_dir"],
                        expected_preset="cache-bioseq-fixture",
                    )

    def test_builder_reruns_tool_and_rejects_resealed_dependency_lie(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fixture = self._production_dso_fixture(Path(tmp))
            document = fixture["document"]
            assert isinstance(document, dict)
            document["dsos"][0]["direct_load_dependencies"] = [
                "/usr/lib/libz.1.dylib"
                if host_platform.system() == "Darwin"
                else "libm.so.6"
            ]
            sealed = self._seal_dso_document(document)
            fixture["receipt"].write_text(
                json.dumps(sealed), encoding="utf-8")
            with self.assertRaisesRegex(
                    RuntimeError, "direct_load_dependencies differs"):
                cache_modes._cache_bioseq_dso_qualification_evidence(
                    fixture["binary_dir"],
                    expected_build_dir=fixture["build_dir"],
                    expected_preset="cache-bioseq-fixture",
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
                build_worker._next_report_path(reporting_dir),
                reporting_dir / "report_1.json",
            )
            self.assertEqual(
                cache_modes._report_index("report_build_2.json"),
                -1,
            )

    def test_split_cache_module_publishes_atomic_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            destination = Path(tmp) / "nested" / "evidence.json"
            cache_modes._write_json_atomic(
                destination,
                {"z-last": 2, "a-first": 1},
            )
            self.assertEqual(
                destination.read_bytes(),
                b'{\n  "a-first": 1,\n  "z-last": 2\n}\n',
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
            real_mkstemp = tempfile.mkstemp

            def update_from_worker(
                    mode: str, fields: dict[str, object],
                    nested_lock: bool) -> None:
                process_state.STATE_FILE = state_path
                process_state.LOCK_FILE = lock_path

                def tracked_mkstemp(*args, **kwargs):
                    descriptor, path = real_mkstemp(*args, **kwargs)
                    results.put(("temporary", path))
                    return descriptor, path

                tempfile.mkstemp = tracked_mkstemp
                try:
                    start_barrier.wait(timeout=5.0)
                    if nested_lock:
                        with process_state._global_lock():
                            process_state._update_entry(mode, **fields)
                    else:
                        process_state._update_entry(mode, **fields)
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
                        "cache-bioseq-benchmark",
                        {
                            "notes": "production-benchmark-ready",
                            "report_files": ["/reports/report_1.json"],
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
                {"cache-integration", "cache-bioseq-benchmark"},
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
                state["cache-bioseq-benchmark"]["notes"],
                "production-benchmark-ready",
            )
            self.assertEqual(
                state["cache-bioseq-benchmark"]["report_files"],
                ["/reports/report_1.json"],
            )

    def test_combined_commands_persist_two_ordered_report_receipts(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            reporting_root = Path(tmp) / "reporting"
            command_receipts: list[tuple[str, Path]] = []
            state_updates: list[dict[str, object]] = []
            focused_flags = [
                "--allow-running-no-tests",
                "[cache-correctness]",
            ]

            def resolve_command(
                    _mode, _active_bin_dir, command, *,
                    bench_flags=None, test_flags=None,
                    report_path=None):
                self.assertIsNone(bench_flags)
                self.assertEqual(test_flags, focused_flags)
                self.assertIsNotNone(report_path)
                command_receipts.append((command[0], report_path))
                watchdog_option = command.index(
                    "--case-timeout-seconds")
                return [
                    command[0],
                    "--report",
                    str(report_path),
                    "--case-timeout-seconds",
                    command[watchdog_option + 1],
                    *test_flags,
                ]

            def publish_report(argv, *_args, **_kwargs):
                report_option = argv.index("--report")
                Path(argv[report_option + 1]).write_text(
                    "{}", encoding="utf-8")
                return 0

            def record_update(_mode, **fields):
                state_updates.append(fields)

            with (
                mock.patch.object(
                    build_presets, "REPORTING_DIR", reporting_root),
                mock.patch.object(
                    build_worker,
                    "_run_command_argv",
                    side_effect=resolve_command,
                ),
                mock.patch.object(
                    build_worker,
                    "_stream_subprocess",
                    side_effect=publish_report,
                ),
                mock.patch.object(
                    build_worker,
                    "_update_entry",
                    side_effect=record_update,
                ),
                mock.patch.object(
                    build_worker,
                    "_cache_native_report_contract",
                    return_value={"cleanup_roots": []},
                ) as validate_report,
            ):
                (
                    report_file,
                    report_files,
                    report_invocations,
                    unit_command_records,
                ) = build_worker._run_mode_commands(
                    "cache-bioseq-integration",
                    Path(tmp) / "bin",
                    Path(tmp) / "build",
                    {},
                    io.StringIO(),
                    float("inf"),
                    set(),
                    test_flags=focused_flags,
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
            self.assertEqual(validate_report.call_count, 2)
            for call in validate_report.call_args_list:
                self.assertEqual(
                    call.kwargs["test_flags"], focused_flags)
            self.assertEqual(
                [Path(invocation[0]).name
                 for invocation in report_invocations],
                [
                    "bioseq_cache_bioseq_support_integration_tests",
                    "bioseq_cache_bioseq_large_input_e2e_tests",
                ],
            )
            for invocation in report_invocations:
                self.assertEqual(invocation[-2:], focused_flags)
            self.assertEqual(
                command_receipts,
                [
                    (
                        "bioseq_cache_bioseq_support_integration_tests",
                        expected_reports[0],
                    ),
                    (
                        "bioseq_cache_bioseq_large_input_e2e_tests",
                        expected_reports[1],
                    ),
                ],
            )
            self.assertEqual(
                state_updates,
                [
                    {
                        "report_files": [str(expected_reports[0])],
                    },
                    {
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
                mock.patch.object(
                    build_presets, "REPORTING_DIR", reporting_root),
                mock.patch.object(
                    build_worker,
                    "_run_command_argv",
                    side_effect=resolve_command,
                ),
                mock.patch.object(
                    build_worker,
                    "_stream_subprocess",
                    return_value=0,
                ) as stream,
                mock.patch.object(
                    build_worker, "_update_entry") as update,
            ):
                with self.assertRaisesRegex(
                        RuntimeError,
                        "support_integration_tests.*allocated regular "
                        "JSON report"):
                    build_worker._run_mode_commands(
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
                / f"ordinary-timeout-root-{secrets.token_hex(8)}"
            )

            def resolve_command(
                    _mode, _active_bin_dir, command, *,
                    bench_flags=None, test_flags=None,
                    report_path=None):
                self.assertIsNotNone(report_path)
                watchdog_option = command.index(
                    "--case-timeout-seconds")
                return [
                    command[0],
                    "--report",
                    str(report_path),
                    "--case-timeout-seconds",
                    command[watchdog_option + 1],
                ]

            def publish_unsafe_terminal_report(
                    argv, *_args, **_kwargs):
                report_option = argv.index("--report")
                Path(argv[report_option + 1]).write_text(json.dumps({
                    "schema_revision": 2,
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
                    build_presets, "REPORTING_DIR", reporting_root),
                mock.patch.object(
                    build_worker,
                    "_run_command_argv",
                    side_effect=resolve_command,
                ),
                mock.patch.object(
                    build_worker,
                    "_stream_subprocess",
                    side_effect=publish_unsafe_terminal_report,
                ),
                mock.patch.object(build_worker, "_update_entry"),
                self.assertRaisesRegex(
                    RuntimeError, "not marker-owned"),
            ):
                build_worker._run_mode_commands(
                    "cache-bioseq-integration",
                    root / "bin",
                    root / "build",
                    {},
                    io.StringIO(),
                    time.time()
                    + cache_modes.CACHE_BIOSEQ_RUNTIME_TOTAL_SEC,
                    set(),
                )

    def test_status_exposes_only_ordered_reports(
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
        }
        with mock.patch.object(
                build_worker,
                "_load_state",
                return_value={
                    "cache-bioseq-integration": {
                        **base_entry,
                        "report_files": [support, production],
                    },
                }):
            status = build_worker.tool_status("cache-bioseq-integration")
        self.assertIn(f"Reports: {support}, {production}", status)
        self.assertLess(status.index(support), status.index(production))

        with mock.patch.object(
                build_worker,
                "_load_state",
                return_value={
                    "cache-bioseq-integration": {
                        **base_entry,
                        "report_file": production,
                    },
                }):
            legacy_status = build_worker.tool_status(
                "cache-bioseq-integration")
        self.assertNotIn(production, legacy_status)
        self.assertNotIn("Report:", legacy_status)
        self.assertNotIn("Reports:", legacy_status)

    def test_mcp_schemas_expose_cache_modes_and_inspect_targets(self) -> None:
        definitions = {
            definition["name"]: definition
            for definition in protocol.TOOL_DEFINITIONS
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
            "cache-benchmark",
            "cache-bioseq-integration",
            "cache-bioseq-benchmark",
        ):
            self.assertIn(mode, modes)
            self.assertIn(mode, remote_modes)
        self.assertEqual(inspect_targets, sorted(build_worker.INSPECT_TARGETS))
        self.assertEqual(remote_targets, sorted(build_worker.INSPECT_TARGETS))

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

    def test_inspect_cache_dso_candidates_follow_configuration_and_platform(
            self) -> None:
        definition = build_worker.INSPECT_TARGETS["postgresql_bioseq_cache"]
        self.assertEqual(
            definition.profiled_artifact_directory,
            "artifacts/cache_production",
        )

        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp)
            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n",
                encoding="utf-8",
            )
            for system_name, suffix in (
                ("Darwin", ".dylib"),
                ("Linux", ".so"),
                ("Windows", ".dll"),
            ):
                with self.subTest(system_name=system_name):
                    candidates = build_worker._inspect_artifact_candidates(
                        definition,
                        build_dir,
                        debug=True,
                        system_name=system_name,
                    )
                    self.assertEqual(
                        candidates,
                        (
                            "artifacts/cache_production/Release/"
                            f"bioseq_cache{suffix}",
                            f"bioseq_cache{suffix}",
                        ),
                    )

            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n",
                encoding="utf-8",
            )
            candidates = build_worker._inspect_artifact_candidates(
                definition,
                build_dir,
                debug=False,
                system_name="Linux",
            )
            self.assertEqual(
                candidates[0],
                "artifacts/cache_production/RelWithDebInfo/"
                "bioseq_cache.so",
            )

            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=\n",
                encoding="utf-8",
            )
            with mock.patch.dict(
                    os.environ, {"CMAKE_BUILD_TYPE": "Release"}):
                candidates = build_worker._inspect_artifact_candidates(
                    definition,
                    build_dir,
                    debug=True,
                    system_name="Linux",
                )
            self.assertEqual(
                candidates[0],
                "artifacts/cache_production/Debug/bioseq_cache.so",
            )

            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=../../wrong\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "invalid CMake"):
                build_worker._inspect_artifact_candidates(
                    definition,
                    build_dir,
                    debug=False,
                    system_name="Linux",
                )

    def test_inspect_cache_dso_resolution_rejects_wrong_config_and_directory(
            self) -> None:
        definition = build_worker.INSPECT_TARGETS["postgresql_bioseq_cache"]
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp)
            binary_root = build_dir / "bioseq_caching"
            (build_dir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n",
                encoding="utf-8",
            )
            artifact_names = build_worker._inspect_artifact_candidates(
                definition,
                build_dir,
                debug=False,
                system_name="Linux",
            )
            requested = binary_root / artifact_names[0]
            stale_debug = (
                binary_root
                / "artifacts/cache_production/Debug/bioseq_cache.so"
            )
            fallback = binary_root / artifact_names[1]
            for path in (requested, stale_debug, fallback):
                path.parent.mkdir(parents=True, exist_ok=True)
            stale_debug.touch()
            fallback.touch()
            requested.mkdir()

            binary, candidates = inspect_tool.resolve_binary_candidate(
                binary_root,
                "postgresql_bioseq_cache",
                artifact_names,
            )
            self.assertEqual(binary, fallback)
            self.assertEqual(candidates, (requested, fallback))

            requested.rmdir()
            requested.touch()
            binary, candidates = inspect_tool.resolve_binary_candidate(
                binary_root,
                "postgresql_bioseq_cache",
                artifact_names,
            )

        self.assertEqual(binary, requested)
        self.assertEqual(candidates, (requested, fallback))

    def test_inspect_cache_dso_missing_diagnostic_lists_profile_and_fallback(
            self) -> None:
        definition = build_worker.INSPECT_TARGETS["postgresql_bioseq_cache"]
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp)
            artifact_names = build_worker._inspect_artifact_candidates(
                definition,
                build_dir,
                debug=True,
                system_name="Linux",
            )
            result = inspect_tool.run_inspect(
                function_name="provider_plan_matches",
                target="postgresql_bioseq_cache",
                build_dir=build_dir,
                binary_subdir="bioseq_caching",
                artifact_names=artifact_names,
                shared_target_names=definition.shared_target_names,
            )

        self.assertIn("error", result)
        self.assertIn(
            "bioseq_caching/artifacts/cache_production/Debug/"
            "bioseq_cache.so",
            result["error"],
        )
        self.assertIn(
            "bioseq_caching/bioseq_cache.so",
            result["error"],
        )

    def test_postgresql_bioseq_inspect_mapping_uses_benchmark_e2e_dso_candidates(
            self) -> None:
        definition = build_worker.INSPECT_TARGETS["postgresql_bioseq"]
        self.assertEqual(definition.mode, "benchmark-e2e")
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


class InspectAssemblyStreamingTest(unittest.TestCase):
    _SYMBOL = "_bioseq_customscan_methods"

    @classmethod
    def _matches(cls) -> list[dict[str, object]]:
        return [{
            "address": 0x1000,
            "size": 64,
            "type": "T",
            "mangled": cls._SYMBOL,
            "demangled": "bioseq_customscan_methods",
        }]

    def test_stream_scan_avoids_read_text_and_extracts_exact_evidence(
            self) -> None:
        listing_text = "\n".join((
            '.file 7 "/src/backend/pg/table" "custom_scan.cppm"',
            ".loc 7 10 0",
            f"{self._SYMBOL}: # @bioseq_customscan_methods",
            ".cfi_startproc",
            "mov x0, x1",
            ".loc 7 11 0",
            "#DEBUG_VALUE: bioseq_customscan_methods:value",
            ".cfi_endproc",
            "",
        ))
        with tempfile.TemporaryDirectory() as tmp:
            listing = Path(tmp) / "custom_scan.s"
            listing.write_text(listing_text, encoding="utf-8")
            warnings: list[str] = []
            bounded_lines = inspect_tool._bounded_binary_lines

            class NoSplitText(str):
                def splitlines(self, *args, **kwargs):
                    raise AssertionError(
                        "assembly scan must not call str.splitlines")

            def guarded_lines(stream, stats, phase_deadline=None):
                for line in bounded_lines(stream, stats, phase_deadline):
                    yield NoSplitText(line)

            with (
                mock.patch.object(
                    Path, "read_text",
                    side_effect=AssertionError(
                        "assembly scan must not call Path.read_text"),
                ),
                mock.patch.object(
                    inspect_tool,
                    "_bounded_binary_lines",
                    side_effect=guarded_lines,
                ),
            ):
                blocks, inline_sites, stats = (
                    inspect_tool.scan_assembly_listings(
                        [listing, listing],
                        self._matches(),
                        "custom_scan.cppm",
                        "bioseq_customscan_methods",
                        snippet_lines=3,
                        rg_path=None,
                        warnings=warnings,
                    )
                )

        self.assertEqual(warnings, [])
        self.assertEqual(stats["prefilter_method"], "streaming-fallback")
        self.assertEqual(stats["listings_discovered"], 1)
        self.assertEqual(stats["listings_candidates"], 1)
        self.assertEqual(stats["listings_scanned"], 1)
        block = blocks[self._SYMBOL]
        self.assertEqual(block["assembly_listing_lines"], [3, 8])
        self.assertEqual(
            block["assembly_listing_snippet_first_lines"],
            "\n".join((
                f"{self._SYMBOL}: # @bioseq_customscan_methods",
                ".cfi_startproc",
                "mov x0, x1",
            )),
        )
        self.assertEqual(len(inline_sites), 1)
        self.assertEqual(inline_sites[0]["loc_directive_count"], 2)
        self.assertEqual(inline_sites[0]["first_loc_line"], 2)
        self.assertEqual(inline_sites[0]["last_loc_line"], 6)
        self.assertEqual(inline_sites[0]["debug_value_hits"], 1)
        self.assertEqual(inline_sites[0]["resolved_file_indices"], [7])

    def test_ripgrep_prefilter_limits_and_deduplicates_stream_candidates(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            symbol_listing = root / "symbol.s"
            inline_listing = root / "inline.s"
            unrelated_listing = root / "unrelated.s"
            symbol_listing.write_text(
                f"{self._SYMBOL}:\n.cfi_startproc\nret\n.cfi_endproc\n",
                encoding="utf-8",
            )
            inline_listing.write_text(
                '.file 3 "/src" "custom_scan.cppm"\n.loc 3 44 0\n',
                encoding="utf-8",
            )
            unrelated_listing.write_text(
                f"{self._SYMBOL}_extra:\n.cfi_startproc\nret\n"
                ".cfi_endproc\n",
                encoding="utf-8",
            )
            rg_result = mock.Mock(
                returncode=0,
                stdout=(f"{symbol_listing}\n{inline_listing}\n"
                        f"{symbol_listing}\n"),
                stderr="",
            )
            warnings: list[str] = []
            with (
                mock.patch.object(
                    inspect_tool.subprocess, "run", return_value=rg_result,
                ) as rg_run,
                mock.patch.object(
                    inspect_tool,
                    "_stream_assembly_listing",
                    wraps=inspect_tool._stream_assembly_listing,
                ) as stream_listing,
            ):
                blocks, inline_sites, stats = (
                    inspect_tool.scan_assembly_listings(
                        [symbol_listing, inline_listing, unrelated_listing],
                        self._matches(),
                        "custom_scan.cppm",
                        "bioseq_customscan_methods",
                        snippet_lines=10,
                        rg_path="/usr/bin/rg",
                        warnings=warnings,
                    )
                )

        self.assertEqual(warnings, [])
        rg_run.assert_called_once()
        command = rg_run.call_args.args[0]
        pattern = command[command.index("--regexp") + 1]
        self.assertIsNotNone(re.search(pattern, f"{self._SYMBOL}:\n"))
        self.assertIsNone(re.search(pattern, f"{self._SYMBOL}_extra:\n"))
        self.assertEqual(
            [call.args[0] for call in stream_listing.call_args_list],
            [symbol_listing, inline_listing],
        )
        self.assertEqual(stats["prefilter_method"], "ripgrep")
        self.assertEqual(stats["listings_discovered"], 3)
        self.assertEqual(stats["listings_candidates"], 2)
        self.assertEqual(stats["listings_scanned"], 2)
        self.assertEqual(list(blocks), [self._SYMBOL])
        self.assertEqual(len(inline_sites), 1)

    def test_omitted_source_basename_skips_inline_sweep_with_warning(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp)
            binary_root = build_dir / "bioseq"
            target = "probe"
            binary_root.mkdir()
            (binary_root / target).write_bytes(b"not-an-object")
            listing_dir = (
                binary_root / "CMakeFiles" / f"{target}.dir")
            listing_dir.mkdir(parents=True)
            listing = listing_dir / "probe.s"
            listing.write_text(
                f"{self._SYMBOL}:\n.cfi_startproc\nret\n.cfi_endproc\n",
                encoding="utf-8",
            )
            scratch = build_dir / "scratch"

            def fake_disassemble(*args, **kwargs):
                output = scratch / "disassembly.txt"
                output.write_text("0000:\tret\n", encoding="utf-8")
                return output, []

            with (
                mock.patch.object(inspect_tool, "detect_tools", return_value={
                    "nm": "/usr/bin/nm",
                    "objdump": "/usr/bin/objdump",
                    "demangler": None,
                    "llc": None,
                    "rg": None,
                }),
                mock.patch.object(inspect_tool, "tool_supports", return_value=True),
                mock.patch.object(
                    inspect_tool, "list_symbols", return_value=self._matches()),
                mock.patch.object(
                    inspect_tool, "disassemble_symbol",
                    side_effect=fake_disassemble),
            ):
                result = inspect_tool.run_inspect(
                    function_name="bioseq_customscan_methods",
                    target=target,
                    build_dir=build_dir,
                    max_matches=999,
                    snippet_lines=0,
                    source_basename=None,
                    llc_on_bc=False,
                    scratch_dir=scratch,
                    shared_target_names=(),
                )

        self.assertNotIn("error", result)
        self.assertEqual(result["inline_sites"], [])
        self.assertEqual(
            result["query"]["max_matches"],
            inspect_tool.MAX_INSPECT_MATCHES,
        )
        self.assertEqual(
            result["query"]["snippet_lines"],
            inspect_tool.MIN_INSPECT_SNIPPET_LINES,
        )
        self.assertTrue(any(
            "source_basename was not supplied" in warning
            for warning in result["warnings"]
        ))
        self.assertTrue(any(
            "max_matches=999" in warning for warning in result["warnings"]
        ))
        self.assertTrue(any(
            "snippet_lines=0" in warning for warning in result["warnings"]
        ))
        self.assertEqual(
            result["matches"][0]["assembly_listing_file"], str(listing))
        self.assertEqual(result["summary"]["asm_listings_discovered"], 1)
        self.assertEqual(result["summary"]["asm_listings_candidates"], 1)
        self.assertEqual(result["summary"]["asm_listings_parsed"], 1)
        self.assertNotIn("asm_listings_scanned", result["summary"])
        self.assertFalse(result["summary"]["asm_listing_scan_incomplete"])

    def test_sparse_listing_read_and_retained_state_are_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            listing = Path(tmp) / "sparse.s"
            sparse_prefix_bytes = inspect_tool.ASSEMBLY_READ_CHUNK_BYTES * 8
            with listing.open("wb") as stream:
                stream.seek(sparse_prefix_bytes)
                stream.write(
                    (f"\n{self._SYMBOL}:\n.cfi_startproc\nret\n"
                     ".cfi_endproc\n").encode("utf-8"))

            blocks, inline_site, stats = (
                inspect_tool._stream_assembly_listing(
                    listing,
                    self._matches(),
                    source_basename=None,
                    function_name="bioseq_customscan_methods",
                    snippet_lines=2,
                )
            )

        self.assertIsNone(inline_site)
        self.assertEqual(stats["truncated_lines"], 1)
        self.assertLessEqual(
            stats["max_retained_line_bytes"],
            inspect_tool.ASSEMBLY_MAX_RETAINED_LINE_BYTES,
        )
        self.assertGreaterEqual(stats["bytes_streamed"], sparse_prefix_bytes)
        block = blocks[self._SYMBOL]
        self.assertEqual(block["assembly_listing_lines"], [2, 5])
        self.assertEqual(
            block["assembly_listing_snippet_first_lines"],
            f"{self._SYMBOL}:\n.cfi_startproc",
        )

    def test_macho_underscore_directions_and_dual_symbol_collision(
            self) -> None:
        symbols = ("_strip_me", "add_me", "_dual", "dual")
        matches = [{
            "address": 0x1000 + index,
            "size": 16,
            "type": "T",
            "mangled": symbol,
            "demangled": symbol,
        } for index, symbol in enumerate(symbols)]
        text = "\n".join((
            "strip_me:", ".cfi_startproc", "ret", ".cfi_endproc",
            "_add_me:", ".cfi_startproc", "ret", ".cfi_endproc",
            "_dual:", ".cfi_startproc", "ret", ".cfi_endproc", "",
            "dual:", ".cfi_startproc", "nop", ".cfi_endproc", "",
        ))
        with tempfile.TemporaryDirectory() as tmp:
            listing = Path(tmp) / "symbols.s"
            listing.write_text(text, encoding="utf-8")
            blocks, _, _ = inspect_tool._stream_assembly_listing(
                listing, matches, None, "unused", 10)
            ambiguous = Path(tmp) / "ambiguous.s"
            ambiguous.write_text(
                "_dual:\n.cfi_startproc\nret\n.cfi_endproc\n",
                encoding="utf-8",
            )
            ambiguous_again = Path(tmp) / "ambiguous_again.s"
            ambiguous_again.write_text(
                "_dual:\n.cfi_startproc\nnop\n.cfi_endproc\n",
                encoding="utf-8",
            )
            ambiguous_blocks, _, _ = inspect_tool.scan_assembly_listings(
                [ambiguous, ambiguous_again], matches[-2:], None, "unused",
                10, None, [])

        self.assertEqual(set(blocks), set(symbols))
        self.assertEqual(blocks["_strip_me"]["assembly_listing_lines"], [1, 4])
        self.assertEqual(blocks["add_me"]["assembly_listing_lines"], [5, 8])
        self.assertEqual(blocks["_dual"]["assembly_listing_lines"], [9, 12])
        self.assertEqual(blocks["dual"]["assembly_listing_lines"], [14, 17])
        self.assertEqual(set(ambiguous_blocks), {"_dual"})

    def test_first_matching_listing_wins_across_distinct_candidates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first = root / "first.s"
            second = root / "second.s"
            body = (f"{self._SYMBOL}:\n.cfi_startproc\nret\n"
                    ".cfi_endproc\n")
            first.write_text(body, encoding="utf-8")
            second.write_text(body, encoding="utf-8")
            blocks, _, stats = inspect_tool.scan_assembly_listings(
                [first, second], self._matches(), None,
                "bioseq_customscan_methods", 10, None, [])

        self.assertEqual(
            blocks[self._SYMBOL]["assembly_listing_file"], str(first))
        self.assertEqual(stats["listings_scanned"], 2)

    def test_chunk_split_label_file_loc_and_cfi_tokens_are_reassembled(
            self) -> None:
        text = "\n".join((
            '.file 17 "/source" "custom_scan.cppm"',
            ".loc 17 91 0",
            f"{self._SYMBOL}:",
            ".cfi_startproc",
            "ret",
            ".cfi_endproc",
            "",
        ))
        with tempfile.TemporaryDirectory() as tmp:
            listing = Path(tmp) / "split.s"
            listing.write_text(text, encoding="utf-8")
            with mock.patch.object(
                    inspect_tool, "ASSEMBLY_READ_CHUNK_BYTES", 5):
                blocks, inline_site, stats = (
                    inspect_tool._stream_assembly_listing(
                        listing,
                        self._matches(),
                        "custom_scan.cppm",
                        "bioseq_customscan_methods",
                        10,
                    )
                )

        self.assertEqual(blocks[self._SYMBOL]["assembly_listing_lines"], [3, 6])
        self.assertIsNotNone(inline_site)
        self.assertEqual(inline_site["loc_directive_count"], 1)
        self.assertEqual(inline_site["resolved_file_indices"], [17])
        self.assertEqual(stats["truncated_lines"], 0)

    def test_cfi_start_window_accepts_line_eight_and_rejects_line_nine(
            self) -> None:
        accepted = "_accepted"
        rejected = "_rejected"
        matches = [
            {"mangled": symbol, "demangled": symbol, "type": "T",
             "address": index, "size": 1}
            for index, symbol in enumerate((accepted, rejected))
        ]
        text = "\n".join((
            f"{accepted}:",
            "# 2", "# 3", "# 4", "# 5", "# 6", "# 7",
            ".cfi_startproc", "ret", ".cfi_endproc",
            f"{rejected}:",
            "# 2", "# 3", "# 4", "# 5", "# 6", "# 7", "# 8",
            ".cfi_startproc", "ret", ".cfi_endproc", "",
        ))
        with tempfile.TemporaryDirectory() as tmp:
            listing = Path(tmp) / "window.s"
            listing.write_text(text, encoding="utf-8")
            blocks, _, _ = inspect_tool._stream_assembly_listing(
                listing, matches, None, "unused", 20)

        self.assertIn(accepted, blocks)
        self.assertNotIn(rejected, blocks)
        self.assertEqual(blocks[accepted]["assembly_listing_lines"], [1, 10])

    def test_rg_error_and_timeout_use_streaming_fallback_single_deadline(
            self) -> None:
        paths = [Path("one.s"), Path("two.s")]
        for failure in (
            mock.Mock(returncode=2, stdout="", stderr="bad regex"),
            subprocess.TimeoutExpired("rg", 120),
        ):
            with self.subTest(failure=type(failure).__name__):
                warnings: list[str] = []
                side_effect = failure if isinstance(
                    failure, BaseException) else None
                with mock.patch.object(
                        inspect_tool.subprocess, "run",
                        return_value=None if side_effect else failure,
                        side_effect=side_effect):
                    candidates, method = inspect_tool.prefilter_assembly_files(
                        paths, self._matches(), None, "/usr/bin/rg", warnings)
                self.assertEqual(candidates, paths)
                self.assertEqual(method, "streaming-fallback")
                self.assertTrue(warnings)

        warnings = []
        first = mock.Mock(returncode=1, stdout="", stderr="")
        timeout = subprocess.TimeoutExpired("rg", 115)
        with (
            mock.patch.object(
                inspect_tool, "_rg_path_batches",
                return_value=[[paths[0]], [paths[1]]],
            ),
            mock.patch.object(
                inspect_tool.time, "monotonic",
                side_effect=[100.0, 100.0, 105.0],
            ),
            mock.patch.object(
                inspect_tool.subprocess, "run",
                side_effect=[first, timeout],
            ) as rg_run,
        ):
            candidates, method = inspect_tool.prefilter_assembly_files(
                paths, self._matches(), None, "/usr/bin/rg", warnings)
        self.assertEqual(candidates, paths)
        self.assertEqual(method, "streaming-fallback")
        self.assertEqual(
            [call.kwargs["timeout"] for call in rg_run.call_args_list],
            [120.0, 115.0],
        )

    def test_rg_batches_keep_each_argv_below_24_kib(self) -> None:
        base = [
            "/usr/bin/rg", "--files-with-matches", "--text",
            "--no-messages", "--regexp", r"^(?:_symbol):(?:\s|$)", "--",
        ]
        paths = [Path("segment" + ("x" * 300) + str(index) + ".s")
                 for index in range(200)]
        batches = inspect_tool._rg_path_batches(paths, base)
        self.assertIsNotNone(batches)
        self.assertGreater(len(batches), 1)
        self.assertEqual([path for batch in batches for path in batch], paths)
        for batch in batches:
            self.assertLessEqual(
                inspect_tool._argv_size_bytes(
                    [*base, *(str(path) for path in batch)]),
                inspect_tool.ASSEMBLY_RG_MAX_ARG_BYTES,
            )

    def test_one_assembly_deadline_covers_rg_and_streaming_chunks(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            listing = Path(tmp) / "deadline.s"
            listing.write_text(
                f"{self._SYMBOL}:\n.cfi_startproc\nret\n.cfi_endproc\n",
                encoding="utf-8",
            )

            rg_warnings: list[str] = []
            rg_error = mock.Mock(returncode=2, stdout="", stderr="failed")
            with (
                mock.patch.object(
                    inspect_tool, "ASSEMBLY_PHASE_TIMEOUT_SEC", 10,
                ),
                mock.patch.object(
                    inspect_tool.time, "monotonic",
                    side_effect=[0.0, 0.0, 11.0],
                ),
                mock.patch.object(
                    inspect_tool.subprocess, "run", return_value=rg_error,
                ),
            ):
                _, _, rg_stats = inspect_tool.scan_assembly_listings(
                    [listing], self._matches(), None,
                    "bioseq_customscan_methods", 10, "/usr/bin/rg",
                    rg_warnings)

            stream_warnings: list[str] = []
            with (
                mock.patch.object(
                    inspect_tool, "ASSEMBLY_PHASE_TIMEOUT_SEC", 10,
                ),
                mock.patch.object(
                    inspect_tool, "ASSEMBLY_READ_CHUNK_BYTES", 8,
                ),
                mock.patch.object(
                    inspect_tool.time, "monotonic",
                    side_effect=[0.0, 0.0, 0.0, 11.0],
                ),
            ):
                blocks, _, stream_stats = inspect_tool.scan_assembly_listings(
                    [listing], self._matches(), None,
                    "bioseq_customscan_methods", 10, None,
                    stream_warnings)

        self.assertTrue(rg_stats["scan_incomplete"])
        self.assertEqual(
            rg_stats["incomplete_reason"], "assembly-phase-deadline")
        self.assertEqual(rg_stats["listings_scanned"], 0)
        self.assertTrue(any("phase deadline" in warning
                            for warning in rg_warnings))
        self.assertEqual(blocks, {})
        self.assertTrue(stream_stats["scan_incomplete"])
        self.assertEqual(stream_stats["listings_scanned"], 0)
        self.assertEqual(stream_stats["listings_unparsed"], 1)
        self.assertTrue(any("evidence is incomplete" in warning
                            for warning in stream_warnings))

    def test_total_snippet_budget_warns_and_reports_truncation(self) -> None:
        matches = [
            {"mangled": symbol, "demangled": symbol, "type": "T",
             "address": index, "size": 1}
            for index, symbol in enumerate(("_first", "_second"))
        ]
        text = "\n".join((
            "_first:", ".cfi_startproc", "x" * 80, ".cfi_endproc",
            "_second:", ".cfi_startproc", "y" * 80, ".cfi_endproc", "",
        ))
        with tempfile.TemporaryDirectory() as tmp:
            listing = Path(tmp) / "budget.s"
            listing.write_text(text, encoding="utf-8")
            warnings: list[str] = []
            request_budget = inspect_tool._new_snippet_budget()
            with (
                mock.patch.object(
                    inspect_tool, "MAX_RETAINED_SNIPPET_BYTES", 48,
                ),
                mock.patch.object(
                    inspect_tool, "MAX_TOTAL_RETAINED_SNIPPET_BYTES", 64,
                ),
            ):
                blocks, _, stats = inspect_tool.scan_assembly_listings(
                    [listing], matches, None, "unused", 20, None, warnings,
                    snippet_budget=request_budget)
                _, _, objdump_truncated = inspect_tool._retain_text_snippet(
                    "post-lto-" + ("z" * 80), 20, request_budget)

        self.assertEqual(set(blocks), {"_first", "_second"})
        self.assertLessEqual(stats["snippet_bytes_retained"], 64)
        self.assertGreaterEqual(stats["snippets_byte_truncated"], 1)
        self.assertTrue(objdump_truncated)
        self.assertLessEqual(request_budget["bytes_retained"], 64)
        self.assertTrue(any("byte-truncated" in warning for warning in warnings))
        self.assertTrue(any(
            block["assembly_listing_snippet_budget_truncated"]
            for block in blocks.values()
        ))

    def test_source_state_budget_fails_closed_with_warning_and_stats(
            self) -> None:
        text = "\n".join((
            '.file 1 "/src" "custom_scan.cppm"',
            ".loc 1 10 0",
            '.file 2 "/src" "other.cppm"',
            ".loc 2 20 0",
            '.file 3 "/src" "third.cppm"',
            ".loc 3 30 0",
            "",
        ))
        with tempfile.TemporaryDirectory() as tmp:
            listing = Path(tmp) / "state.s"
            listing.write_text(text, encoding="utf-8")
            warnings: list[str] = []
            with mock.patch.object(
                    inspect_tool, "ASSEMBLY_MAX_SOURCE_STATE_ENTRIES", 2):
                _, inline_sites, stats = inspect_tool.scan_assembly_listings(
                    [listing], [], "custom_scan.cppm", "unused", 10,
                    None, warnings)

        self.assertEqual(inline_sites, [])
        self.assertEqual(stats["source_state_budget_exceeded_files"], 1)
        self.assertEqual(stats["source_state_overflow_events"], 1)
        self.assertEqual(stats["source_state_entries_cleared"], 2)
        self.assertEqual(stats["source_state_records_dropped"], 2)
        self.assertTrue(any("omitted fail-closed" in warning
                            for warning in warnings))

    def test_local_and_remote_inspect_schemas_document_bounded_contract(
            self) -> None:
        definitions = {
            definition["name"]: definition
            for definition in protocol.TOOL_DEFINITIONS
        }
        local = definitions["inspect"]
        local_properties = local["inputSchema"]["properties"]
        self.assertEqual(
            local_properties["max_matches"]["maximum"],
            inspect_tool.MAX_INSPECT_MATCHES,
        )
        self.assertEqual(
            local_properties["snippet_lines"]["maximum"],
            inspect_tool.MAX_INSPECT_SNIPPET_LINES,
        )
        self.assertIn(
            "When omitted", local_properties["source_basename"]["description"])
        self.assertIn("build tree", local["description"])
        self.assertIn("build_metadata.tools.rg", local["description"])
        self.assertIn("asm_listings_discovered", local["description"])
        self.assertIn("asm_listings_parsed", local["description"])
        self.assertNotIn("asm_listings_scanned", local["description"])
        self.assertIn("asm_listing_scan_incomplete", local["description"])
        self.assertIn("entries-cleared", local["description"])

        remote_shapes = definitions["remote-run"]["inputSchema"]["oneOf"]
        remote = next(shape for shape in remote_shapes
                      if shape.get("title") == "inspect")
        remote_properties = remote["properties"]
        self.assertEqual(
            remote_properties["max_matches"]["maximum"],
            inspect_tool.MAX_INSPECT_MATCHES,
        )
        self.assertEqual(
            remote_properties["snippet_lines"]["maximum"],
            inspect_tool.MAX_INSPECT_SNIPPET_LINES,
        )
        self.assertIn("omission skips", remote_properties[
            "source_basename"]["description"])
        self.assertIn("box-side paths", definitions[
            "remote-run"]["description"])


class CacheRuntimeIntegrityTest(unittest.TestCase):
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
                os.replace(replacement, executable)
                return 0

            with (
                mock.patch.object(
                    build_worker, "_run_command_argv",
                    side_effect=resolve_command),
                mock.patch.object(
                    build_worker, "_stream_subprocess",
                    side_effect=replace_executable),
                mock.patch.object(build_worker, "_update_entry"),
            ):
                with self.assertRaisesRegex(
                        RuntimeError,
                        "verified handle|identity changed during"):
                    build_worker._run_mode_commands(
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
            shutil.copyfile(sys.executable, logical)
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
                f"printf B > {shlex.quote(str(marker))}\n",
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
                with cache_modes._verified_native_command_execution(
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
                    os.replace(logical, displaced)
                    os.replace(replacement, logical)
                    try:
                        rc = process_state._stream_subprocess(
                            invocation,
                            io.StringIO(),
                            time.time() + 10,
                            cwd=root,
                            verified_executable=snapshot,
                        )
                    finally:
                        os.replace(logical, replacement)
                        os.replace(displaced, logical)

            self.assertEqual(rc, 0)
            marker_lines = marker.read_text(
                encoding="utf-8").splitlines()
            self.assertEqual(marker_lines, ["A", invocation[0]])
            self.assertFalse(snapshot.exists())
            self.assertEqual(
                cache_modes._stable_native_command_identity(invocation)[
                    "executable_sha256"],
                record["executable_sha256"],
            )

    def test_verified_snapshot_tampering_fails_closed(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            logical = root / "native-command"
            shutil.copyfile(sys.executable, logical)
            logical.chmod(0o700)
            invocation = [str(logical), "-c", "pass"]

            with self.assertRaisesRegex(
                    RuntimeError, "verified handle"):
                with cache_modes._verified_native_command_execution(
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
                shutil.copyfile(sys.executable, logical)
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
                            cache_modes._verified_native_command_execution(
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
            shutil.copyfile(sys.executable, logical)
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
                with cache_modes._verified_native_command_execution(
                        invocation) as (_record, snapshot):
                    process_state._stream_subprocess(
                        invocation,
                        output,
                        time.time() + 0.25,
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
                / f"ordinary-temp-root-{secrets.token_hex(8)}"
            )
            with self.assertRaisesRegex(
                    RuntimeError, "not marker-owned"):
                cache_modes._cache_cleanup_root_contracts(
                    [{"root": str(unmarked)}])
            wrong_parent = (
                Path(tmp).resolve(strict=True)
                / f"bioseq-pg-test-{secrets.token_hex(8)}"
            )
            with self.assertRaisesRegex(
                    RuntimeError, "canonical temporary directory"):
                cache_modes._cache_cleanup_root_contracts(
                    [{"root": str(wrong_parent)}])

    def test_cleanup_absence_rejects_existing_and_symlinked_paths(
            self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve(strict=True)
            existing = root / "bioseq-pg-test-existing"
            existing.mkdir()
            with self.assertRaisesRegex(
                    RuntimeError, "still exists"):
                cache_modes._require_absent_nofollow_path(
                    existing, "cleanup root")

            target = root / "target"
            target.mkdir()
            linked_parent = root / "linked-parent"
            linked_parent.symlink_to(target, target_is_directory=True)
            through_symlink = (
                linked_parent / "bioseq-pg-test-absent")
            with self.assertRaisesRegex(
                    RuntimeError, "traverses a symlink"):
                cache_modes._require_absent_nofollow_path(
                    through_symlink, "cleanup root")

            provider_root = root / "bioseq-pg-test-provider"
            runtime_library = provider_root / "runtime-library"
            runtime_library.mkdir(parents=True)
            provider_dso = runtime_library / "bioseq_cache.dylib"
            provider_dso.write_bytes(b"reappeared")
            with self.assertRaisesRegex(
                    RuntimeError, "still exists"):
                cache_modes._require_absent_nofollow_path(
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
                    nonce = secrets.token_hex(8)
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
                        "schema_revision": 2,
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
                            cache_modes._validate_failed_cache_terminal_report(
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
                + secrets.token_hex(8)
            )
        )
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "report.json"
            report.write_text(json.dumps({
                "schema_revision": 2,
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
                cache_modes._require_cache_cleanup_roots_absent)
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
                        cache_modes,
                        "_require_cache_cleanup_roots_absent",
                        side_effect=mutate_after_first_check,
                    ),
                    self.assertRaisesRegex(
                        RuntimeError, "still exists"),
                ):
                    cache_modes._validate_failed_cache_terminal_report(
                        report)
            finally:
                if cleanup_root.exists():
                    cleanup_root.rmdir()
            self.assertEqual(calls, 2)


    def test_combined_runtime_budget_is_exact_and_never_shortens_watchdogs(
            self) -> None:
        large_argv = [
            "/bin/bioseq_cache_bioseq_large_input_e2e_tests",
            "--case-timeout-seconds",
            "6300",
        ]
        self.assertEqual(
            cache_modes.CACHE_BIOSEQ_RUNTIME_TOTAL_SEC,
            cache_modes.CACHE_BIOSEQ_ORCHESTRATION_BUDGET_SEC
            + cache_modes.CACHE_BIOSEQ_SUPPORT_AGGREGATE_TIMEOUT_SEC
            + cache_modes.CACHE_BIOSEQ_LARGE_INPUT_TIMEOUT_SEC
            + cache_modes.CACHE_LARGE_INPUT_FINALIZATION_RESERVE_SEC,
        )
        self.assertEqual(cache_modes.CACHE_BIOSEQ_RUNTIME_TOTAL_SEC, 8400)
        self.assertEqual(
            cache_modes.CACHE_BIOSEQ_SUPPORT_AGGREGATE_TIMEOUT_SEC, 900)
        self.assertEqual(cache_modes.CACHE_BIOSEQ_LARGE_INPUT_TIMEOUT_SEC, 6300)
        self.assertEqual(
            cache_modes.CACHE_BIOSEQ_LARGE_LAUNCH_MIN_REMAINING_SEC, 6900)
        self.assertEqual(
            cache_modes.CACHE_LARGE_INPUT_FINALIZATION_RESERVE_SEC, 600)

        bounded = cache_modes._cache_apply_runtime_budget(
            "cache-bioseq-integration",
            large_argv,
            deadline=8_400.0,
            now=1_500.0,
        )
        self.assertEqual(bounded, large_argv)
        self.assertEqual(
            cache_modes._cache_apply_runtime_budget(
                "cache-bioseq-integration",
                large_argv,
                deadline=float("inf"),
                now=1_000.0,
            ),
            large_argv,
        )
        with self.assertRaisesRegex(
                TimeoutError, "fewer than 6900 seconds"):
            cache_modes._cache_apply_runtime_budget(
                "cache-bioseq-integration",
                large_argv,
                deadline=8_400.0,
                now=1_500.001,
            )
        with self.assertRaisesRegex(
                RuntimeError, "watchdog must remain exactly 6300"):
            cache_modes._cache_apply_runtime_budget(
                "cache-bioseq-integration",
                [*large_argv[:-1], "6299"],
                deadline=float("inf"),
                now=1_000.0,
            )

        support_argv = [
            "/bin/bioseq_cache_bioseq_support_integration_tests",
            "--case-timeout-seconds",
            "480",
        ]
        self.assertEqual(
            cache_modes._cache_command_deadline(
                "cache-bioseq-integration",
                support_argv,
                deadline=8_400.0,
                now=600.0,
            ),
            1_500.0,
        )
        with self.assertRaisesRegex(
                TimeoutError, "orchestration budget"):
            cache_modes._cache_command_deadline(
                "cache-bioseq-integration",
                support_argv,
                deadline=8_400.0,
                now=600.001,
            )

    def test_failed_large_input_report_must_prove_zero_retained_state(
            self) -> None:
        cleanup_root = (
            Path(tempfile.gettempdir()).resolve(strict=True)
            / f"bioseq-pg-test-{secrets.token_hex(8)}"
        )
        report = {
            "schema_revision": 2,
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
            cache_modes._validate_failed_cache_terminal_report(path)
            report["schema_version"] = "retired-authored-identity"
            path.write_text(json.dumps(report), encoding="utf-8")
            with self.assertRaisesRegex(
                    RuntimeError, "revision-2 terminal report"):
                cache_modes._validate_failed_cache_terminal_report(path)
            del report["schema_version"]
            report["cluster_configuration"][
                "retained-supervision-state"] = "watchdog.json"
            path.write_text(json.dumps(report), encoding="utf-8")
            with self.assertRaisesRegex(
                    RuntimeError, "zero retained supervision state"):
                cache_modes._validate_failed_cache_terminal_report(path)

if __name__ == "__main__":
    unittest.main()
