"""Focused unit tests for the safe local bench-e2e config override."""

from pathlib import Path
import tempfile
import time
import unittest
from unittest import mock

import server


class BenchE2eConfigOverrideTests(unittest.TestCase):
    def test_accepts_workspace_relative_json_and_returns_canonical_path(self):
        resolved, error = server._validate_bench_e2e_config_override(
            "bench-e2e", "main/bench/bench_e2e_configuration.json")

        self.assertIsNone(error)
        self.assertEqual(
            resolved,
            str((server.WORKSPACE_ROOT
                 / "main/bench/bench_e2e_configuration.json").resolve()),
        )

    def test_rejects_override_for_every_other_mode(self):
        resolved, error = server._validate_bench_e2e_config_override(
            "bench", "main/bench/bench_e2e_configuration.json")

        self.assertIsNone(resolved)
        self.assertIn("only for mode='bench-e2e'", error)

    def test_rejects_existing_json_outside_workspace(self):
        outside_json = Path("/tmp/bioseq-build-e2e-outside.json")
        outside_json.write_text("{}", encoding="utf-8")
        self.addCleanup(outside_json.unlink, missing_ok=True)

        resolved, error = server._validate_bench_e2e_config_override(
            "bench-e2e", str(outside_json))

        self.assertIsNone(resolved)
        self.assertIn("inside the workspace", error)

    def test_rejects_missing_and_non_json_paths(self):
        missing, missing_error = server._validate_bench_e2e_config_override(
            "bench-e2e", "main/bench/does-not-exist.json")
        non_json, non_json_error = server._validate_bench_e2e_config_override(
            "bench-e2e", "main/build_mcp/server.py")

        self.assertIsNone(missing)
        self.assertIn("existing file", missing_error)
        self.assertIsNone(non_json)
        self.assertIn("must name a .json file", non_json_error)


class UnitIntegrationQualificationTests(unittest.TestCase):
    def test_builder_builds_and_runs_e2e_serializer_contract(self):
        mode = server.MODES["unit+integration"]

        self.assertIn("bioseq_bench_e2e_tests_exec", mode.targets)
        self.assertIn(
            "bioseq_bench_e2e_tests_exec",
            [command[0] for command in mode.commands],
        )

    def test_builder_builds_and_runs_benchmark_framework_contracts(self):
        mode = server.MODES["unit+integration"]

        self.assertIn(
            "bioseq_bench_framework_example_tests_exec", mode.targets)
        self.assertIn(
            "bioseq_bench_framework_example_tests_exec",
            [command[0] for command in mode.commands],
        )

    def test_focused_filter_allows_zero_matches_in_every_binary(self):
        test_flags, error = server._unit_test_filter_to_cli_args(
            {"tags": ["[format5]"]})

        self.assertIsNone(error)
        self.assertEqual(
            test_flags,
            ["--allow-running-no-tests", "[format5]"],
        )
        binary_dir = Path("/tmp/test-opt/bioseq")
        resolved_commands = [
            server._run_command_argv(
                "unit+integration",
                binary_dir,
                command,
                test_flags=test_flags,
            )
            for command in server.MODES["unit+integration"].commands
        ]
        self.assertEqual(len(resolved_commands), 4)
        for command in resolved_commands:
            self.assertEqual(command[-2:], test_flags)


class UnitIntegrationCacheIsolationTests(unittest.TestCase):
    def test_optional_component_mode_invariants_are_explicit(self):
        self.assertEqual(
            server.MODES["unit+integration"].required_cache_variables,
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
                "BIOSEQ_BUILD_WEBSITE": "OFF",
            },
        )
        self.assertEqual(
            server.MODES["external-api"].required_cache_variables,
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "ON",
                "BIOSEQ_BUILD_WEBSITE": "OFF",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "OFF",
            },
        )
        self.assertEqual(
            server.MODES["website"].required_cache_variables,
            {
                "BIOSEQ_BUILD_WEBSITE": "ON",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "ON",
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
            },
        )

    def _run_optional_transition(
            self, mode, preset, cache_entries):
        commands = []
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / preset
            build_dir.mkdir()
            (build_dir / "build.ninja").touch()
            cache = build_dir / "CMakeCache.txt"
            cache.write_text(
                "BIOSEQ_TEST_INDEXES:BOOL=OFF\n" + "".join(
                    f"{name}:BOOL={value}\n"
                    for name, value in cache_entries.items()),
                encoding="utf-8",
            )
            log_path = Path(temporary) / "worker.log"
            state = {
                mode: {
                    "started_at": time.time(),
                    "log_file": str(log_path),
                    "rt_priority": None,
                },
            }

            def run(command, *_args, **_kwargs):
                commands.append(list(command))
                return 0

            with mock.patch.multiple(
                    server,
                    _refuse_root_build_identity=mock.DEFAULT,
                    _ensure_build_worker_timesharing_policy=mock.DEFAULT,
                    _load_state=mock.DEFAULT,
                    _update_entry=mock.DEFAULT,
                    preset_for=mock.DEFAULT,
                    build_dir_for=mock.DEFAULT,
                    bin_dir_for=mock.DEFAULT,
                    _build_worker_descendant_environment=mock.DEFAULT,
                    _ensure_local_toolchain_environment=mock.DEFAULT,
                    _configured_local_build_rt_priority=mock.DEFAULT,
                    _local_clang_rt_is_enabled=mock.DEFAULT,
                    _preflight_local_clang_rt_launcher=mock.DEFAULT,
                    _stream_subprocess=mock.DEFAULT,
                    _clang_rt_cmake_cache_repairs=mock.DEFAULT,
            ) as patched:
                patched["_refuse_root_build_identity"].return_value = None
                patched["_load_state"].return_value = state
                patched["preset_for"].return_value = preset
                patched["build_dir_for"].return_value = build_dir
                patched["bin_dir_for"].return_value = build_dir / "bioseq"
                patched["_build_worker_descendant_environment"].return_value = {}
                patched["_configured_local_build_rt_priority"].return_value = None
                patched["_local_clang_rt_is_enabled"].return_value = False
                patched["_stream_subprocess"].side_effect = run
                patched["_clang_rt_cmake_cache_repairs"].return_value = ([], [])

                result = server._run_build_worker(mode, build_only=True)

            self.assertEqual(result, 0, log_path.read_text(encoding="utf-8"))
        return commands

    def test_website_to_external_transition_is_atomic(self):
        commands = self._run_optional_transition(
            "external-api",
            "shared-opt",
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
                "BIOSEQ_BUILD_WEBSITE": "ON",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "ON",
            },
        )
        self.assertEqual(commands[0][:3], ["cmake", "--preset", "shared-opt"])
        self.assertEqual(
            set(commands[0][3:]),
            {
                "-DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON",
                "-DBIOSEQ_BUILD_WEBSITE=OFF",
                "-DBIOSEQ_WEBSITE_BROWSER_TESTS=OFF",
            },
        )
        self.assertEqual(commands[1][:2], ["cmake", "--build"])

    def test_external_to_website_transition_is_atomic(self):
        commands = self._run_optional_transition(
            "website",
            "shared-opt",
            {
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "ON",
                "BIOSEQ_BUILD_WEBSITE": "OFF",
                "BIOSEQ_WEBSITE_BROWSER_TESTS": "OFF",
            },
        )
        self.assertEqual(commands[0][:3], ["cmake", "--preset", "shared-opt"])
        self.assertEqual(
            set(commands[0][3:]),
            {
                "-DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=OFF",
                "-DBIOSEQ_BUILD_WEBSITE=ON",
                "-DBIOSEQ_WEBSITE_BROWSER_TESTS=ON",
            },
        )
        self.assertEqual(commands[1][:2], ["cmake", "--build"])

    def test_external_and_website_cannot_share_a_live_build_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            shared_tree = Path(temporary) / "shared-opt"
            shared_tree.mkdir()
            shared_alias = Path(temporary) / "shared-alias"
            try:
                shared_alias.symlink_to(shared_tree, target_is_directory=True)
            except OSError:
                # Windows test hosts may deny unprivileged symlink creation;
                # exact path identity still pins the scheduler contract there.
                shared_alias = shared_tree

            cases = (
                ("external-api", "website", shared_alias),
                ("website", "external-api", shared_tree),
            )
            for requested_mode, running_mode, recorded_tree in cases:
                with self.subTest(
                        requested=requested_mode, running=running_mode):
                    state = {
                        running_mode: {
                            "status": "running",
                            "build_only": True,
                            "debug": False,
                            "build_dir": str(recorded_tree),
                        },
                    }
                    with mock.patch.object(
                            server, "_refresh",
                            side_effect=lambda current, candidate:
                            current.get(candidate)):
                        conflict_mode, conflict_entry = (
                            server._running_in_same_group(
                                state, requested_mode, True, shared_tree))

                    self.assertEqual(conflict_mode, running_mode)
                    self.assertIs(conflict_entry, state[running_mode])

    def test_legacy_running_entry_uses_its_recorded_debug_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            shared_debug_tree = Path(temporary) / "shared-debug"
            state = {
                "external-api": {
                    "status": "running",
                    "build_only": True,
                    "debug": True,
                },
            }

            def legacy_build_dir(mode, debug):
                self.assertEqual((mode, debug), ("external-api", True))
                return shared_debug_tree

            with mock.patch.object(
                    server, "_refresh",
                    side_effect=lambda current, candidate:
                    current.get(candidate)), mock.patch.object(
                        server, "build_dir_for",
                        side_effect=legacy_build_dir):
                conflict_mode, conflict_entry = server._running_in_same_group(
                    state, "website", True, shared_debug_tree)

            self.assertEqual(conflict_mode, "external-api")
            self.assertIs(conflict_entry, state["external-api"])

    def test_different_optional_mode_trees_remain_independent(self):
        with tempfile.TemporaryDirectory() as temporary:
            external_tree = Path(temporary) / "external-opt"
            website_tree = Path(temporary) / "website-opt"
            state = {
                "website": {
                    "status": "running",
                    "build_only": True,
                    "debug": False,
                    "build_dir": str(website_tree),
                },
            }
            with mock.patch.object(
                    server, "_refresh",
                    side_effect=lambda current, candidate:
                    current.get(candidate)):
                conflict = server._running_in_same_group(
                    state, "external-api", True, external_tree)

            self.assertEqual(conflict, (None, None))

    def test_doubly_polluted_tree_is_repaired_in_first_configure(self):
        commands = []
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "test-opt"
            build_dir.mkdir()
            (build_dir / "build.ninja").touch()
            cache = build_dir / "CMakeCache.txt"
            cache.write_text(
                "BIOSEQ_TEST_INDEXES:BOOL=OFF\n"
                "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS:BOOL=ON\n"
                "BIOSEQ_BUILD_WEBSITE:BOOL=ON\n",
                encoding="utf-8",
            )
            log_path = Path(temporary) / "worker.log"
            state = {
                "unit+integration": {
                    "started_at": time.time(),
                    "log_file": str(log_path),
                    "rt_priority": None,
                },
            }

            def run(command, *_args, **_kwargs):
                command = list(command)
                commands.append(command)
                if command[:3] == ["cmake", "--preset", "test-opt"]:
                    cache.write_text(
                        "BIOSEQ_TEST_INDEXES:BOOL=ON\n"
                        "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS:BOOL=OFF\n"
                        "BIOSEQ_BUILD_WEBSITE:BOOL=OFF\n",
                        encoding="utf-8",
                    )
                return 0

            with mock.patch.multiple(
                    server,
                    _refuse_root_build_identity=mock.DEFAULT,
                    _ensure_build_worker_timesharing_policy=mock.DEFAULT,
                    _load_state=mock.DEFAULT,
                    _update_entry=mock.DEFAULT,
                    preset_for=mock.DEFAULT,
                    build_dir_for=mock.DEFAULT,
                    bin_dir_for=mock.DEFAULT,
                    _build_worker_descendant_environment=mock.DEFAULT,
                    _ensure_local_toolchain_environment=mock.DEFAULT,
                    _configured_local_build_rt_priority=mock.DEFAULT,
                    _local_clang_rt_is_enabled=mock.DEFAULT,
                    _preflight_local_clang_rt_launcher=mock.DEFAULT,
                    _initial_clang_rt_cmake_args=mock.DEFAULT,
                    _stream_subprocess=mock.DEFAULT,
                    _clang_rt_cmake_cache_repairs=mock.DEFAULT,
            ) as patched:
                patched["_refuse_root_build_identity"].return_value = None
                patched["_load_state"].return_value = state
                patched["preset_for"].return_value = "test-opt"
                patched["build_dir_for"].return_value = build_dir
                patched["bin_dir_for"].return_value = build_dir / "bioseq"
                patched["_build_worker_descendant_environment"].return_value = {}
                patched["_configured_local_build_rt_priority"].return_value = None
                patched["_local_clang_rt_is_enabled"].return_value = False
                patched["_initial_clang_rt_cmake_args"].return_value = [
                    "-DCMAKE_C_COMPILER_LAUNCHER:FILEPATH=/trusted/launcher",
                    "-DCMAKE_CXX_COMPILER_LAUNCHER:FILEPATH=/trusted/launcher",
                ]
                patched["_stream_subprocess"].side_effect = run
                patched["_clang_rt_cmake_cache_repairs"].return_value = ([], [])

                result = server._run_build_worker(
                    "unit+integration", build_only=True)

            self.assertEqual(result, 0, log_path.read_text(encoding="utf-8"))

        repair = [
            "cmake", "--preset", "test-opt",
            "-DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=OFF",
            "-DBIOSEQ_BUILD_WEBSITE=OFF",
            "-DCMAKE_C_COMPILER_LAUNCHER:FILEPATH=/trusted/launcher",
            "-DCMAKE_CXX_COMPILER_LAUNCHER:FILEPATH=/trusted/launcher",
        ]
        self.assertEqual(commands[0], repair)
        first_build = next(
            index for index, command in enumerate(commands)
            if command[:2] == ["cmake", "--build"])
        self.assertLess(0, first_build)


class RemoteRunWebsiteRejectionTests(unittest.TestCase):
    """website is a local build-tree-only target (Doxygen + private PostgreSQL
    + pinned browser); remote-run must refuse it explicitly."""

    def test_website_is_a_valid_build_mode(self):
        # It must be a real mode, otherwise the rejection below is meaningless
        # (it would be caught by the generic invalid-mode guard instead).
        self.assertIn("website", server.VALID_MODES)

    def test_website_is_registered_as_rejected(self):
        self.assertIn("website", server.REMOTE_RUN_REJECTED_MODES)

    def test_remote_run_build_rejects_website(self):
        message = server.tool_remote_run({"operation": "build", "mode": "website"})

        self.assertIn("not supported by remote-run", message)
        self.assertIn("start_build mode=website", message)

    def test_remote_run_does_not_reject_ordinary_modes(self):
        # Every other build mode stays eligible for remote-run.
        self.assertEqual(
            server.REMOTE_RUN_REJECTED_MODES & server.VALID_MODES,
            {"website"},
        )


if __name__ == "__main__":
    unittest.main()
