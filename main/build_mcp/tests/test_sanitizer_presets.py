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
MAIN_DIR = BUILD_MCP_DIR.parent
sys.path.insert(0, str(BUILD_MCP_DIR))
sys.path.insert(0, str(MAIN_DIR / "cmake"))

import server  # noqa: E402
from sanitizer_runtime import (  # noqa: E402
    DYLD_INSERT_ENV,
    SANITIZER_RUNTIME_ENV,
    SanitizerRuntimeError,
    postgres_sanitizer_environment,
    sanitizer_descendant_environment,
)


SANITIZERS = {
    "test-opt-asan": "address",
    "test-opt-ubsan": "undefined",
    "test-opt-tsan": "thread",
}
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
        payload = json.loads((MAIN_DIR / "CMakePresets.json").read_text())
        cls.presets = {
            preset["name"]: preset
            for preset in payload["configurePresets"]
        }

    def test_presets_are_isolated_test_trees_with_profile_zero(self) -> None:
        self.assertEqual(
            self.presets["test-opt"]["cacheVariables"]["BIOSEQ_TEST_INDEXES"],
            "ON",
        )
        for name in SANITIZERS:
            with self.subTest(name=name):
                preset = self.presets[name]
                variables = preset["cacheVariables"]
                self.assertEqual(preset["inherits"], "test-opt")
                self.assertEqual(
                    preset["binaryDir"], "${sourceDir}/build/${presetName}")
                self.assertEqual(
                    variables["BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE"], "0")
                self.assertEqual(
                    variables["PostgreSQL_CONFIG"],
                    "/opt/homebrew/opt/postgresql@17/bin/pg_config",
                )

    def test_c_cxx_and_every_link_product_use_the_same_sanitizer(self) -> None:
        for name, sanitizer in SANITIZERS.items():
            with self.subTest(name=name):
                variables = self.presets[name]["cacheVariables"]
                expected = f"-fsanitize={sanitizer}"
                for compile_key in ("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS"):
                    flags = set(variables[compile_key].split())
                    self.assertTrue(COMMON_COMPILE_FLAGS.issubset(flags))
                    self.assertIn(expected, flags)
                for linker_key in LINKER_KEYS:
                    self.assertEqual(variables[linker_key], expected)
                self.assertNotIn("CMAKE_LINKER_FLAGS", variables)

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
                    MAIN_DIR / "build" / name,
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
                    server, "_configured_local_build_rt_priority",
                    return_value=None), \
                mock.patch.object(server, "_preflight_local_clang_rt_launcher"), \
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
                    server, "_configured_local_build_rt_priority",
                    return_value=None), \
                mock.patch.object(
                    server, "_preflight_local_clang_rt_launcher") as preflight, \
                mock.patch.object(server.subprocess, "Popen") as popen:
            result = server.tool_start_build("external-api", build_only=True)

        self.assertIn("Build worker environment error", result)
        preflight.assert_not_called()
        popen.assert_not_called()

    def test_cache_adapter_loads_generic_runner_in_isolated_process(self) -> None:
        adapter = (
            MAIN_DIR / "bioseq_caching" / "test" /
            "run_private_provider_tests.py")
        probe = """
import importlib.util
from pathlib import Path
import sys

adapter = Path(sys.argv[1]).resolve(strict=True)
spec = importlib.util.spec_from_file_location(
    "standalone_cache_adapter_probe", adapter)
if spec is None or spec.loader is None:
    raise RuntimeError(f"cannot load adapter: {adapter}")
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
expected_runner = adapter.parents[2] / "cmake" / "private_postgres.py"
expected_helper = adapter.parents[2] / "cmake" / "sanitizer_runtime.py"
if Path(module._generic.__file__).resolve() != expected_runner.resolve():
    raise RuntimeError("cache adapter loaded the wrong generic runner")
runtime = sys.modules.get("sanitizer_runtime")
if runtime is None:
    raise RuntimeError("generic runner did not load sanitizer_runtime")
if Path(runtime.__file__).resolve() != expected_helper.resolve():
    raise RuntimeError("generic runner loaded a non-sibling sanitizer helper")
"""
        with tempfile.TemporaryDirectory() as temporary:
            completed = subprocess.run(
                [sys.executable, "-I", "-c", probe, str(adapter)],
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
