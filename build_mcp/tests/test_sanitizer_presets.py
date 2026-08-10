from __future__ import annotations

import contextlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = BUILD_MCP_DIR.parent
sys.path.insert(0, str(BUILD_MCP_DIR))
sys.path.insert(0, str(REPOSITORY_ROOT / "cmake"))

import server  # noqa: E402
import private_postgres  # noqa: E402
from sanitizer_runtime import (  # noqa: E402
    ASAN_MIXED_LIBCXX_CONTAINER_COMPAT_OPTION,
    ASAN_OPTIONS_ENV,
    DYLD_INSERT_ENV,
    SANITIZER_RUNTIME_ENV,
    SanitizerRuntimeError,
    _force_colon_option,
    postgres_sanitizer_environment,
    sanitizer_descendant_environment,
)


SANITIZERS = {
    "test-opt-asan": "address",
    "test-opt-ubsan": "undefined",
    "test-opt-tsan": "thread",
}
SANITIZER_BASE = "test-opt-sanitizer"
COMMON_COMPILE_FLAGS = {
    "-O1",
    "-DNDEBUG",
    "-g",
    "-fno-omit-frame-pointer",
    "-fno-optimize-sibling-calls",
}
LINKER_KEYS = (
    "CMAKE_EXE_LINKER_FLAGS",
    "CMAKE_SHARED_LINKER_FLAGS",
    "CMAKE_MODULE_LINKER_FLAGS",
)


class SanitizerPresetContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        payload = json.loads((REPOSITORY_ROOT / "CMakePresets.json").read_text())
        cls.presets = {
            preset["name"]: preset
            for preset in payload["configurePresets"]
        }

    @classmethod
    def resolved_cache_variables(cls, name: str) -> dict[str, str]:
        preset = cls.presets[name]
        variables: dict[str, str] = {}
        inherited = preset.get("inherits", [])
        if isinstance(inherited, str):
            inherited = [inherited]
        for parent in inherited:
            variables.update(cls.resolved_cache_variables(parent))
        variables.update(preset.get("cacheVariables", {}))
        return variables

    def test_presets_are_isolated_test_trees_with_profile_zero(self) -> None:
        self.assertEqual(
            self.presets["test-opt"]["cacheVariables"]["BIOSEQ_TEST_INDEXES"],
            "ON",
        )
        base = self.presets[SANITIZER_BASE]
        self.assertTrue(base["hidden"])
        self.assertEqual(base["inherits"], "test-opt")
        self.assertEqual(
            base["cacheVariables"]["CMAKE_PREFIX_PATH"],
            "/opt/homebrew/opt/gettext",
        )
        for name in SANITIZERS:
            with self.subTest(name=name):
                preset = self.presets[name]
                variables = self.resolved_cache_variables(name)
                self.assertEqual(preset["inherits"], SANITIZER_BASE)
                self.assertEqual(
                    preset["binaryDir"], "${sourceDir}/build/${presetName}")
                self.assertEqual(
                    variables["BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE"], "0")
                self.assertEqual(
                    variables["PostgreSQL_CONFIG"],
                    "/opt/homebrew/opt/postgresql@17/bin/pg_config",
                )
                self.assertEqual(
                    variables["CMAKE_PREFIX_PATH"],
                    "/opt/homebrew/opt/gettext",
                )

    def test_c_cxx_and_every_link_product_use_the_same_sanitizer(self) -> None:
        for name, sanitizer in SANITIZERS.items():
            with self.subTest(name=name):
                variables = self.resolved_cache_variables(name)
                expected = f"-fsanitize={sanitizer}"
                for compile_key in ("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS"):
                    flags = set(variables[compile_key].split())
                    self.assertTrue(COMMON_COMPILE_FLAGS.issubset(flags))
                    self.assertIn(expected, flags)
                for linker_key in LINKER_KEYS:
                    self.assertEqual(variables[linker_key], expected)
                self.assertNotIn("CMAKE_LINKER_FLAGS", variables)

    def test_only_tsan_selects_instrumented_tbb_overlay(self) -> None:
        overlay_key = "VCPKG_OVERLAY_PORTS"
        self.assertEqual(
            self.resolved_cache_variables("test-opt-tsan")[overlay_key],
            "${sourceDir}/cmake/ports-tsan",
        )
        for name in ("test-opt", "test-opt-asan", "test-opt-ubsan"):
            with self.subTest(name=name):
                self.assertNotIn(
                    overlay_key, self.resolved_cache_variables(name))

        overlay = REPOSITORY_ROOT / "cmake" / "ports-tsan" / "tbb"
        portfile = (overlay / "portfile.cmake").read_text()
        self.assertEqual(
            (overlay / "bioseq-tsan-instrumented.txt").read_text(),
            "TBB_SANITIZE=thread\n"
            "CMAKE_C_COMPILER=/opt/homebrew/opt/llvm@22/bin/clang\n"
            "CMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@22/bin/clang++\n",
        )
        self.assertEqual(portfile.count("-DTBB_SANITIZE=thread"), 1)
        self.assertIn(
            '"-DCMAKE_C_COMPILER:FILEPATH=${_bioseq_tsan_clang}"',
            portfile,
        )
        self.assertIn(
            '"-DCMAKE_CXX_COMPILER:FILEPATH=${_bioseq_tsan_clangxx}"',
            portfile,
        )
        self.assertIn(
            "foreach(_bioseq_tbb_variant IN ITEMS dbg rel)", portfile)
        self.assertIn(
            '"${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-'
            '${_bioseq_tbb_variant}/CMakeCache.txt"',
            portfile,
        )
        self.assertIn(
            "list(FIND _bioseq_tbb_cache_lines", portfile)
        self.assertIn(
            '"TBB_SANITIZE:STRING=thread"', portfile)
        self.assertIn(
            "list(FILTER _bioseq_tbb_compiler_cache_entries INCLUDE REGEX",
            portfile,
        )
        self.assertIn(
            "_bioseq_tbb_compiler_cache_entry_count EQUAL 1", portfile)
        for cache_type in ("STRING", "FILEPATH"):
            with self.subTest(cache_type=cache_type):
                self.assertIn(
                    "CMAKE_${_bioseq_tbb_compiler_kind}_COMPILER:"
                    f"{cache_type}=${{_bioseq_tbb_expected_compiler}}",
                    portfile,
                )
        self.assertIn(
            "_bioseq_tbb_validated_cache_count EQUAL 0", portfile)
        configure_start = portfile.index("vcpkg_cmake_configure(")
        validation_start = portfile.index(
            "set(_bioseq_tbb_validated_cache_count")
        package_install = portfile.index("vcpkg_cmake_install()")
        marker_install = portfile.index(
            'file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/'
            'bioseq-tsan-instrumented.txt"')
        self.assertLess(configure_start, validation_start)
        self.assertLess(validation_start, package_install)
        self.assertLess(package_install, marker_install)

        bioseq_cmake = (REPOSITORY_ROOT / "bioseq" / "CMakeLists.txt").read_text()
        self.assertIn(
            "-fsanitize=([^ ;]*,)?thread(,[^ ;]*)?", bioseq_cmake)
        self.assertIn(
            "_bioseq_tbb_tsan_expected_marker", bioseq_cmake)

    def test_postgresql_nls_dependency_is_target_scoped(self) -> None:
        module = (REPOSITORY_ROOT / "cmake" / "FindPostgreSQL.cmake").read_text()
        self.assertIn(
            'file(STRINGS "${_PostgreSQL_CONFIG_HEADER}" '
            "_PostgreSQL_ENABLE_NLS",
            module,
        )
        self.assertIn("find_package(Intl REQUIRED)", module)
        self.assertIn(
            "list(APPEND PostgreSQL_INCLUDE_DIRS ${Intl_INCLUDE_DIRS})",
            module,
        )
        self.assertIn(
            "list(APPEND PostgreSQL_SERVER_LINK_LIBRARIES Intl::Intl)",
            module,
        )
        self.assertEqual(
            module.count(
                'INTERFACE_LINK_LIBRARIES '
                '"${PostgreSQL_SERVER_LINK_LIBRARIES}"'),
            2,
        )
        for name in SANITIZERS:
            variables = self.resolved_cache_variables(name)
            self.assertNotIn("-I/opt/homebrew/opt/gettext", " ".join(
                (variables["CMAKE_C_FLAGS"], variables["CMAKE_CXX_FLAGS"])))
            for linker_key in LINKER_KEYS:
                self.assertNotIn(
                    "-L/opt/homebrew/opt/gettext",
                    variables[linker_key],
                )

    def test_builder_override_routes_unit_integration_to_each_tree(self) -> None:
        for name in SANITIZERS:
            with self.subTest(name=name), mock.patch.multiple(
                    server,
                    DEFAULT_BUILD_PRESET="bench-opt",
                    TEST_BUILD_PRESET=name):
                self.assertEqual(
                    server.preset_for("unit+integration", debug=False), name)
                self.assertEqual(
                    server.build_dir_for("unit+integration", debug=False),
                    REPOSITORY_ROOT / "build" / name,
                )

    def test_neutral_runtime_is_mapped_only_at_postgres_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            runtime = (Path(temporary) /
                "libclang_rt.asan_osx_dynamic.dylib")
            runtime.touch()
            base = {
                "PATH": os.environ.get("PATH", ""),
                SANITIZER_RUNTIME_ENV: str(runtime),
                DYLD_INSERT_ENV: "/tmp/untrusted-interposer.dylib",
            }
            descendant = sanitizer_descendant_environment(
                base, allowed_paths=(runtime,))
            self.assertNotIn(DYLD_INSERT_ENV, descendant)
            self.assertEqual(
                descendant[SANITIZER_RUNTIME_ENV], str(runtime.resolve()))
            self.assertEqual(
                descendant[ASAN_OPTIONS_ENV],
                ASAN_MIXED_LIBCXX_CONTAINER_COMPAT_OPTION,
            )
            worker = server._build_worker_descendant_environment(
                base, allowed_paths=(runtime,))
            self.assertNotIn(DYLD_INSERT_ENV, worker)
            self.assertEqual(
                worker[SANITIZER_RUNTIME_ENV], str(runtime.resolve()))
            postgres = postgres_sanitizer_environment(
                base, allowed_paths=(runtime,))
            self.assertEqual(
                postgres[DYLD_INSERT_ENV], str(runtime.resolve()))

            untrusted = Path(temporary) / "untrusted.dylib"
            untrusted.touch()
            with self.assertRaises(SanitizerRuntimeError):
                sanitizer_descendant_environment(
                    {SANITIZER_RUNTIME_ENV: str(untrusted)},
                    allowed_paths=(runtime,))

    def test_mixed_libcxx_asan_lane_disables_entire_container_poison_category(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            runtime = (Path(temporary) /
                "libclang_rt.asan_osx_dynamic.dylib")
            runtime.touch()
            base = {
                SANITIZER_RUNTIME_ENV: str(runtime),
                ASAN_OPTIONS_ENV: (
                    "abort_on_error=1:detect_container_overflow=1:"
                    "detect_stack_use_after_return=1"),
            }
            descendant = sanitizer_descendant_environment(
                base, allowed_paths=(runtime,))
            options = descendant[ASAN_OPTIONS_ENV].split(":")
            self.assertIn("abort_on_error=1", options)
            self.assertIn("detect_stack_use_after_return=1", options)
            self.assertEqual(
                options.count(ASAN_MIXED_LIBCXX_CONTAINER_COMPAT_OPTION), 1)
            self.assertNotIn("detect_container_overflow=1", options)

            postgres = postgres_sanitizer_environment(
                base, allowed_paths=(runtime,))
            self.assertEqual(
                postgres[ASAN_OPTIONS_ENV], descendant[ASAN_OPTIONS_ENV])
            self.assertEqual(
                postgres[DYLD_INSERT_ENV], str(runtime.resolve()))

    def test_colon_option_parser_replaces_every_duplicate_target_key(
            self) -> None:
        observed = _force_colon_option(
            "abort_on_error=1:detect_container_overflow=1:"
            "detect_container_overflow=0:detect_container_overflow=maybe:"
            "symbolize=1",
            ASAN_MIXED_LIBCXX_CONTAINER_COMPAT_OPTION,
        )
        self.assertEqual(
            observed,
            "abort_on_error=1:symbolize=1:"
            + ASAN_MIXED_LIBCXX_CONTAINER_COMPAT_OPTION,
        )

    def test_colon_option_parser_drops_empty_fields_but_preserves_unrelated(
            self) -> None:
        observed = _force_colon_option(
            ":abort_on_error=1::malformed:=empty-key:other==value:"
            "trailing=::",
            ASAN_MIXED_LIBCXX_CONTAINER_COMPAT_OPTION,
        )
        self.assertEqual(
            observed,
            "abort_on_error=1:malformed:=empty-key:other==value:trailing=:"
            + ASAN_MIXED_LIBCXX_CONTAINER_COMPAT_OPTION,
        )

    def test_private_cluster_child_receives_normalized_asan_options(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = root / "libclang_rt.asan_osx_dynamic.dylib"
            runtime.touch()
            cluster = private_postgres.UniquePrivateCluster(
                private_postgres.ClusterInputs(
                    bindir=root,
                    database="sanitizer_contract",
                    config_template=root / "unused.conf.in",
                    output_root=root / "output",
                    run_label="sanitizer_child_env",
                ))
            base = {
                SANITIZER_RUNTIME_ENV: str(runtime),
                ASAN_OPTIONS_ENV: (
                    "abort_on_error=1:detect_container_overflow=1:"
                    "symbolize=1"),
            }

            def normalize(env):
                return sanitizer_descendant_environment(
                    env, allowed_paths=(runtime,))

            try:
                with mock.patch.dict(os.environ, base, clear=True), \
                        mock.patch.object(
                            private_postgres,
                            "sanitizer_descendant_environment",
                            side_effect=normalize):
                    child = cluster.child_environment()
            finally:
                if cluster.socket_dir.exists():
                    cluster.socket_dir.rmdir()

            self.assertEqual(
                child[ASAN_OPTIONS_ENV],
                "abort_on_error=1:symbolize=1:"
                + ASAN_MIXED_LIBCXX_CONTAINER_COMPAT_OPTION,
            )
            self.assertEqual(
                child[SANITIZER_RUNTIME_ENV], str(runtime.resolve()))

    def test_non_asan_runtime_does_not_rewrite_asan_options(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            runtime = (Path(temporary) /
                "libclang_rt.ubsan_osx_dynamic.dylib")
            runtime.touch()
            base = {
                SANITIZER_RUNTIME_ENV: str(runtime),
                ASAN_OPTIONS_ENV: "detect_container_overflow=1",
            }
            descendant = sanitizer_descendant_environment(
                base, allowed_paths=(runtime,))
            self.assertEqual(
                descendant[ASAN_OPTIONS_ENV],
                base[ASAN_OPTIONS_ENV],
            )

    def test_detached_worker_spawn_receives_no_raw_dyld_state(self) -> None:
        fake_process = mock.Mock(pid=4242)
        base = {
            "PATH": os.environ.get("PATH", ""),
            DYLD_INSERT_ENV: "/tmp/untrusted-interposer.dylib",
        }
        with mock.patch.dict(os.environ, base, clear=True), \
                mock.patch.object(
                    server, "_refuse_root_build_identity", return_value=None), \
                mock.patch.object(
                    server, "_ensure_build_worker_timesharing_policy"), \
                mock.patch.object(
                    server, "_global_lock",
                    return_value=contextlib.nullcontext()), \
                mock.patch.object(server, "_load_state", return_value={}), \
                mock.patch.object(
                    server, "_running_in_same_group",
                    return_value=(None, None)), \
                mock.patch.object(
                    server, "_git_commit", return_value="a" * 40), \
                mock.patch.object(
                    server, "_source_graph_fingerprint",
                    return_value="source-digest"), \
                mock.patch.object(
                    server, "_configured_local_build_rt_priority",
                    return_value=None), \
                mock.patch.object(server, "_preflight_local_clang_rt_launcher"), \
                mock.patch.object(
                    server, "_capture_process_identity", return_value=None), \
                mock.patch.object(server, "_save_state"), \
                mock.patch.object(server, "_log"), \
                mock.patch.object(
                    server.subprocess, "Popen", return_value=fake_process) as popen:
            result = server.tool_start_build("external-api", build_only=True)

        self.assertIn("scheduled", result)
        worker_env = popen.call_args.kwargs["env"]
        self.assertNotIn(DYLD_INSERT_ENV, worker_env)
        self.assertEqual(worker_env["PATH"], base["PATH"])

    def test_invalid_neutral_runtime_fails_before_worker_spawn(self) -> None:
        base = {
            "PATH": os.environ.get("PATH", ""),
            SANITIZER_RUNTIME_ENV: "/tmp/not-an-allowed-runtime.dylib",
        }
        with mock.patch.dict(os.environ, base, clear=True), \
                mock.patch.object(
                    server, "_refuse_root_build_identity", return_value=None), \
                mock.patch.object(
                    server, "_ensure_build_worker_timesharing_policy"), \
                mock.patch.object(
                    server, "_global_lock",
                    return_value=contextlib.nullcontext()), \
                mock.patch.object(server, "_load_state", return_value={}), \
                mock.patch.object(
                    server, "_running_in_same_group",
                    return_value=(None, None)), \
                mock.patch.object(
                    server, "_git_commit", return_value="a" * 40), \
                mock.patch.object(
                    server, "_source_graph_fingerprint",
                    return_value="source-digest"), \
                mock.patch.object(
                    server, "_configured_local_build_rt_priority",
                    return_value=None), \
                mock.patch.object(
                    server, "_preflight_local_clang_rt_launcher") as preflight, \
                mock.patch.object(server.subprocess, "Popen") as popen:
            result = server.tool_start_build("external-api", build_only=True)

        self.assertIn("Build worker environment error", result)
        preflight.assert_not_called()
        popen.assert_not_called()

    def test_private_postgres_loads_sibling_sanitizer_runtime_in_isolated_process(
            self) -> None:
        runner = REPOSITORY_ROOT / "cmake" / "private_postgres.py"
        probe = """
import importlib.util
from pathlib import Path
import sys

runner = Path(sys.argv[1]).resolve(strict=True)
spec = importlib.util.spec_from_file_location(
    "standalone_private_postgres_probe", runner)
if spec is None or spec.loader is None:
    raise RuntimeError(f"cannot load runner: {runner}")
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
if Path(module.__file__).resolve() != runner:
    raise RuntimeError("file-spec import loaded the wrong generic runner")
runtime = sys.modules.get("sanitizer_runtime")
if runtime is None:
    raise RuntimeError("generic runner did not load sanitizer_runtime")
if module._sanitizer_runtime is not runtime:
    raise RuntimeError("generic runner did not retain the loaded helper")
expected_helper = runner.with_name("sanitizer_runtime.py")
if Path(runtime.__file__).resolve() != expected_helper.resolve():
    raise RuntimeError("generic runner loaded a non-sibling sanitizer helper")
"""
        with tempfile.TemporaryDirectory() as temporary:
            completed = subprocess.run(
                [sys.executable, "-I", "-c", probe, str(runner)],
                cwd=temporary,
                capture_output=True,
                text=True,
                timeout=30,
            )
        self.assertEqual(
            completed.returncode,
            0,
            msg=(completed.stdout + completed.stderr),
        )

    def test_restart_argv_preserves_homebrew_dyld_environment(self) -> None:
        data_dir = "/tmp/private-pg-data"
        homebrew = "/opt/homebrew/opt/postgresql@17/bin/pg_ctl"
        postgres_app = (
            "/Applications/Postgres.app/Contents/Versions/17/bin/pg_ctl")
        self.assertEqual(
            server._darwin_postgresql_restart_argv(homebrew, data_dir),
            [homebrew, "-D", data_dir, "restart"],
        )
        self.assertEqual(
            server._darwin_postgresql_restart_argv(postgres_app, data_dir),
            ["arch", "-arm64", postgres_app,
             "-D", data_dir, "restart"],
        )

    def test_builder_restart_environment_strips_ambient_preload(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            runtime = (Path(temporary) /
                "libclang_rt.tsan_osx_dynamic.dylib")
            runtime.touch()
            with mock.patch.object(server.sys, "platform", "darwin"):
                restart_env = server._postgresql_restart_environment(
                    {
                        SANITIZER_RUNTIME_ENV: str(runtime),
                        DYLD_INSERT_ENV: "/tmp/untrusted-interposer.dylib",
                    },
                    allowed_paths=(runtime,),
                )
            self.assertEqual(
                restart_env[DYLD_INSERT_ENV], str(runtime.resolve()))
            self.assertEqual(
                restart_env[SANITIZER_RUNTIME_ENV], str(runtime.resolve()))


if __name__ == "__main__":
    unittest.main()
