from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BUILD_MCP_DIR))

import build_presets  # noqa: E402
import build_worker  # noqa: E402
import postgres_runtime  # noqa: E402
import postgres_safety  # noqa: E402
import process_state  # noqa: E402


class PostgreSQLConfigPresetInvariantTest(unittest.TestCase):
    _CMAKE_IDENTITY = {
        "resolved_path": "/tools/cmake",
        "sha256": "1" * 64,
        "version_line": "cmake version test",
    }

    def test_split_process_state_loads_sanitizer_environment_helpers(
            self) -> None:
        environment = process_state._build_worker_descendant_environment({
            "DYLD_INSERT_LIBRARIES": "/untrusted/preload.dylib",
            "BIOSEQ_TEST_SENTINEL": "preserved",
        })
        self.assertNotIn("DYLD_INSERT_LIBRARIES", environment)
        self.assertEqual(environment["BIOSEQ_TEST_SENTINEL"], "preserved")

    def test_error_safety_report_uses_live_generated_artifact_path(
            self) -> None:
        self.assertEqual(
            postgres_safety.POSTGRES_ERROR_SAFETY_RUNTIME_REPORT_RELATIVE_PATH,
            Path(
                "bioseq/generated/sql/safety/runtime/candidate/"
                "runtime-validation.json"
            ),
        )
        cmake = (
            BUILD_MCP_DIR.parent / "bioseq" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        for fragment in (
            '"${CMAKE_CURRENT_BINARY_DIR}/generated/sql"',
            '"${BIOSEQ_SQL_CODEGEN_DIR}/safety"',
            '"${BIOSEQ_SQL_SAFETY_ARTIFACT_ROOT}/runtime"',
            '"${_bioseq_sql_runtime_root}/candidate"',
            '"${_bioseq_sql_runtime_candidate_root}/runtime-validation.json"',
        ):
            with self.subTest(cmake_path_fragment=fragment):
                self.assertIn(fragment, cmake)

    def _write_presets(self, root: Path) -> Path:
        presets_file = root / "CMakePresets.json"
        presets_file.write_text(json.dumps({
            "version": 3,
            "configurePresets": [
                {
                    "name": "base",
                    "cacheVariables": {
                        "CMAKE_TOOLCHAIN_FILE":
                            "$env{VCPKG_ROOT}/vcpkg.cmake",
                        "PostgreSQL_CONFIG": "/toolchain/17/bin/pg_config",
                    },
                },
                {
                    "name": "fallback",
                    "cacheVariables": {
                        "PostgreSQL_CONFIG":
                            "/fallback/17/bin/pg_config",
                    },
                },
                {
                    "name": "selected",
                    "inherits": ["base", "fallback"],
                },
            ],
        }), encoding="utf-8")
        return presets_file

    def test_inherited_preset_value_uses_earlier_parent_priority(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            presets_file = self._write_presets(Path(temporary))
            contract = build_presets._cmake_preset_cache_contract(
                "selected",
                ("PostgreSQL_CONFIG",),
                presets_file=presets_file,
            )

        self.assertEqual(
            contract,
            {"PostgreSQL_CONFIG": "/toolchain/17/bin/pg_config"},
        )

    def test_configure_stamp_tracks_preset_content_and_resolved_values(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            presets_file = self._write_presets(root)
            build_dir = root / "build"
            first = build_worker._cmake_configure_stamp_document(
                "selected",
                mode_cache_variables={"BIOSEQ_BUILD_CACHE": "ON"},
                managed_arguments=["-DBIOSEQ_CLANG_RT_PRIORITY=3"],
                environment={"VCPKG_ROOT": "/vcpkg/one"},
                cmake_executable_identity=self._CMAKE_IDENTITY,
                presets_file=presets_file,
            )
            build_worker._publish_cmake_configure_stamp(build_dir, first)
            self.assertEqual(
                build_worker._cmake_configure_stamp_status(build_dir, first),
                (True, "configure inputs match"),
            )

            payload = json.loads(presets_file.read_text(encoding="utf-8"))
            payload["configurePresets"][0]["cacheVariables"][
                "PostgreSQL_CONFIG"
            ] = "/toolchain/18/bin/pg_config"
            presets_file.write_text(
                json.dumps(payload),
                encoding="utf-8",
            )
            second = build_worker._cmake_configure_stamp_document(
                "selected",
                mode_cache_variables={"BIOSEQ_BUILD_CACHE": "ON"},
                managed_arguments=["-DBIOSEQ_CLANG_RT_PRIORITY=3"],
                environment={"VCPKG_ROOT": "/vcpkg/one"},
                cmake_executable_identity=self._CMAKE_IDENTITY,
                presets_file=presets_file,
            )
            self.assertNotEqual(
                first["inputs_sha256"],
                second["inputs_sha256"],
            )
            self.assertEqual(
                build_worker._cmake_configure_stamp_status(build_dir, second),
                (False, "configure inputs changed"),
            )

    def test_configure_stamp_tracks_referenced_environment_and_mode_args(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            presets_file = self._write_presets(Path(temporary))
            common = {
                "preset": "selected",
                "mode_cache_variables": {"BIOSEQ_BUILD_CACHE": "ON"},
                "presets_file": presets_file,
            }
            first = build_worker._cmake_configure_stamp_document(
                managed_arguments=["-DFLAG=one"],
                environment={"VCPKG_ROOT": "/vcpkg/one"},
                cmake_executable_identity=self._CMAKE_IDENTITY,
                **common,
            )
            changed_environment = build_worker._cmake_configure_stamp_document(
                managed_arguments=["-DFLAG=one"],
                environment={"VCPKG_ROOT": "/vcpkg/two"},
                cmake_executable_identity=self._CMAKE_IDENTITY,
                **common,
            )
            changed_argument = build_worker._cmake_configure_stamp_document(
                managed_arguments=["-DFLAG=two"],
                environment={"VCPKG_ROOT": "/vcpkg/one"},
                cmake_executable_identity=self._CMAKE_IDENTITY,
                **common,
            )

        self.assertNotEqual(
            first["inputs_sha256"],
            changed_environment["inputs_sha256"],
        )
        self.assertNotEqual(
            first["inputs_sha256"],
            changed_argument["inputs_sha256"],
        )

    def test_configure_stamp_rejects_malformed_or_symlink_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build_dir = root / "build"
            build_dir.mkdir()
            expected = {"artifact_kind": "expected"}
            stamp = build_dir / build_presets._CMAKE_CONFIGURE_STAMP_NAME
            stamp.write_text("{", encoding="utf-8")
            matches, reason = build_worker._cmake_configure_stamp_status(
                build_dir,
                expected,
            )
            self.assertFalse(matches)
            self.assertIn("unreadable", reason)

            stamp.unlink()
            target = root / "elsewhere.json"
            target.write_text(json.dumps(expected), encoding="utf-8")
            stamp.symlink_to(target)
            self.assertEqual(
                build_worker._cmake_configure_stamp_status(
                    build_dir,
                    expected,
                ),
                (
                    False,
                    "configure stamp is not a regular non-symlink file",
                ),
            )

    def test_stale_cache_repair_is_planned_until_exact_value_is_cached(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            presets_file = self._write_presets(root)
            build_dir = root / "build"
            build_dir.mkdir()
            cache = build_dir / "CMakeCache.txt"
            cache.write_text(
                "PostgreSQL_CONFIG:FILEPATH="
                "/Applications/Postgres.app/Contents/Versions/17.5/bin/"
                "pg_config\n",
                encoding="utf-8",
            )

            repairs = build_presets._cmake_preset_cache_repairs(
                build_dir,
                "selected",
                presets_file=presets_file,
            )
            self.assertEqual(
                repairs,
                {"PostgreSQL_CONFIG": "/toolchain/17/bin/pg_config"},
            )

            cache.write_text(
                "PostgreSQL_CONFIG:FILEPATH="
                "/toolchain/17/bin/pg_config\n",
                encoding="utf-8",
            )
            self.assertEqual(
                build_presets._cmake_preset_cache_repairs(
                    build_dir,
                    "selected",
                    presets_file=presets_file,
                ),
                {},
            )

    def test_stale_derived_server_path_resets_postgresql_discovery(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            presets_file = self._write_presets(root)
            build_dir = root / "build"
            build_dir.mkdir()
            cache = build_dir / "CMakeCache.txt"
            cache.write_text(
                "PostgreSQL_CONFIG:FILEPATH="
                "/toolchain/17/bin/pg_config\n"
                "PostgreSQL_EXECUTABLE:FILEPATH="
                "/Applications/Postgres.app/Contents/Versions/17.5/bin/"
                "postgres\n",
                encoding="utf-8",
            )

            self.assertEqual(
                postgres_runtime._postgresql_discovery_cache_reset_args(
                    build_dir,
                    "selected",
                    presets_file=presets_file,
                ),
                postgres_runtime._POSTGRESQL_DISCOVERY_CACHE_UNSET_ARGS,
            )

            cache.write_text(
                "PostgreSQL_CONFIG:FILEPATH="
                "/toolchain/17/bin/pg_config\n"
                "PostgreSQL_EXECUTABLE:FILEPATH="
                "/toolchain/17/bin/postgres\n",
                encoding="utf-8",
            )
            self.assertEqual(
                postgres_runtime._postgresql_discovery_cache_reset_args(
                    build_dir,
                    "selected",
                    presets_file=presets_file,
                ),
                (),
            )

    def test_checked_in_safety_presets_inherit_local_pin(self) -> None:
        expected = "/opt/homebrew/opt/postgresql@17/bin/pg_config"
        for preset in (
            build_presets.POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
            build_presets.POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET,
        ):
            with self.subTest(preset=preset):
                self.assertEqual(
                    build_presets._cmake_preset_cache_contract(
                        preset, ("PostgreSQL_CONFIG",)),
                    {"PostgreSQL_CONFIG": expected},
                )
                self.assertEqual(
                    postgres_safety._postgres_error_safety_cache_contract(preset)[
                        "PostgreSQL_CONFIG"],
                    expected,
                )

    def test_aws_presets_without_a_pin_are_not_given_a_local_value(
            self) -> None:
        for preset in (
            "aws-x64-linux",
            "test-aws-x64-linux",
            "aws-x64-linux-gcc161",
        ):
            with self.subTest(preset=preset):
                self.assertEqual(
                    build_presets._cmake_preset_cache_contract(
                        preset, ("PostgreSQL_CONFIG",)),
                    {},
                )


if __name__ == "__main__":
    unittest.main()
