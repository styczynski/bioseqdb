from __future__ import annotations

import hashlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


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

    @staticmethod
    def _runtime_file_identity(
            path: str,
            *,
            inode: int,
            size: int,
            digest_character: str,
    ) -> postgres_runtime._PostgreSQLFileIdentity:
        return postgres_runtime._PostgreSQLFileIdentity(
            path=path,
            device=1,
            inode=inode,
            size=size,
            sha256=digest_character * 64,
        )

    @staticmethod
    def _postmaster_process_identity() -> process_state._ProcessIdentity:
        return process_state._ProcessIdentity(
            pid=42,
            ppid=1,
            pgid=42,
            session=42,
            started="postgres-start-identity",
            command="/toolchain/17/bin/postgres",
        )

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
        owners = (
            ("cmake/bioseq_sql_generation_target_graph.cmake", (
                '"${CMAKE_CURRENT_BINARY_DIR}/generated/sql"',
            )),
            ("cmake/bioseq_sql_contract_target_graph.cmake", (
                '"${BIOSEQ_SQL_CODEGEN_DIR}/safety"',
            )),
            ("cmake/bioseq_postgres_error_safety_target_graph.cmake", (
                '"${BIOSEQ_SQL_SAFETY_ARTIFACT_ROOT}/runtime"',
                '"${_bioseq_sql_runtime_root}/candidate"',
                '"${_bioseq_sql_runtime_candidate_root}/runtime-validation.json"',
            )),
        )
        previous_owner_offset = -1
        for owner_path, fragments in owners:
            include_fragment = (
                'include(\n  "${CMAKE_CURRENT_LIST_DIR}/../'
                + owner_path + '")'
            )
            owner_include_offset = cmake.find(include_fragment)
            with self.subTest(cmake_owner=owner_path):
                self.assertEqual(cmake.count(include_fragment), 1)
                self.assertGreater(owner_include_offset, previous_owner_offset)
                owner = (BUILD_MCP_DIR.parent / owner_path).read_text(
                    encoding="utf-8")
                for fragment in fragments:
                    with self.subTest(cmake_path_fragment=fragment):
                        self.assertTrue(
                            fragment in owner,
                            f"{fragment!r} missing from {owner_path}",
                        )
            previous_owner_offset = owner_include_offset

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
            expected = {
                "artifact_kind": build_presets._CMAKE_CONFIGURE_STAMP_KIND,
                "schema_revision": 1,
                "inputs": {},
                "inputs_sha256": hashlib.sha256(b"{}").hexdigest(),
            }
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

    def test_local_restart_argv_pins_selected_postgres_for_normal_and_e2e(
            self) -> None:
        pg_ctl = "/toolchain/17/bin/pg_ctl"
        postgres = "/toolchain/17/bin/postgres"
        data_dir = "/var/lib/postgresql/17/main"
        with mock.patch.object(
            postgres_runtime.sys, "platform", "linux",
        ), mock.patch.object(
            postgres_runtime, "_is_aws_preset", return_value=False,
        ):
            normal = postgres_runtime._postgresql_restart_argv(
                pg_ctl, postgres, data_dir, "test-opt")
            e2e = postgres_runtime._postgresql_restart_argv(
                pg_ctl,
                postgres,
                data_dir,
                "bench-e2e-opt",
                fast_wait=True,
            )

        self.assertEqual(
            normal,
            [pg_ctl, "-D", data_dir, "restart", "-p", postgres],
        )
        self.assertEqual(
            e2e,
            [pg_ctl, "-D", data_dir, "restart", "-p", postgres,
             "-m", "fast", "-w"],
        )

    def test_runtime_mapping_evidence_excludes_ordinary_open_descriptors(self) -> None:
        listing = (
            b"p42\0\nftxt\0D0x1000011\0i20\0n/lib/bioseq.dylib\0\n"
            b"fmem\0D0x1000011\0i10\0n/bin/postgres\0\n"
            b"f3\0D0x1000011\0i99\0n/old/bioseq.dylib\0\n"
        )
        with mock.patch.object(
            postgres_runtime.shutil, "which", return_value="/usr/sbin/lsof",
        ), mock.patch.object(
            postgres_runtime.subprocess, "run",
            return_value=mock.Mock(returncode=0, stdout=listing, stderr=b""),
        ):
            mappings = postgres_runtime._lsof_postgresql_mapped_files(42)
        self.assertEqual({mapping.inode for mapping in mappings}, {10, 20})

    def test_postmaster_pid_reuses_aws_uid_boundary_for_private_data_dir(self) -> None:
        with mock.patch.object(
            Path, "open", side_effect=PermissionError("private cluster"),
        ), mock.patch.object(
            postgres_runtime, "_is_aws_preset", return_value=True,
        ), mock.patch.object(
            postgres_runtime, "_postgresql_aws_read_process_file",
            return_value="42\n/var/lib/postgresql/17/main\n",
        ) as read_as_postgres:
            pid = postgres_runtime._postgresql_postmaster_pid(
                "/var/lib/postgresql/17/main", "aws-test-opt")
        self.assertEqual(pid, 42)
        read_as_postgres.assert_called_once_with(
            Path("/var/lib/postgresql/17/main/postmaster.pid"), None)

    def test_runtime_identity_rejects_wrong_executable_before_sql(
            self) -> None:
        expected_postgres = self._runtime_file_identity(
            "/toolchain/17/bin/postgres",
            inode=10, size=100, digest_character="a")
        expected_dso = self._runtime_file_identity(
            "/toolchain/17/lib/bioseq.so",
            inode=20, size=200, digest_character="b")
        postmaster_identity = self._postmaster_process_identity()
        with mock.patch.object(
            postgres_runtime,
            "_seal_postgresql_file_identity",
            side_effect=(expected_postgres, expected_dso),
        ), mock.patch.object(
            postgres_runtime, "_postgresql_postmaster_pid", return_value=42,
        ), mock.patch.object(
            postgres_runtime,
            "_process_snapshot",
            autospec=True,
            return_value={42: postmaster_identity},
        ), mock.patch.object(
            postgres_runtime,
            "_postgresql_mapped_files",
            return_value=(postgres_runtime._PostgreSQLMappedFile(
                path="/other/17/bin/postgres",
                device=1,
                inode=30,
            ),),
        ):
            with self.assertRaisesRegex(
                    RuntimeError, "does not map the sealed selected server"):
                postgres_runtime._verify_postgresql_runtime_identity(
                    "/var/lib/postgresql/17/main",
                    "test-opt",
                    expected_postgres,
                    expected_dso,
                    io.StringIO(),
                )

    def test_runtime_identity_accepts_exact_executable_and_mapped_dso(
            self) -> None:
        expected_postgres = self._runtime_file_identity(
            "/toolchain/17/bin/postgres",
            inode=10, size=100, digest_character="a")
        expected_dso = self._runtime_file_identity(
            "/toolchain/17/lib/bioseq.so",
            inode=20, size=200, digest_character="b")
        postmaster_identity = self._postmaster_process_identity()
        runtime_log = io.StringIO()
        with mock.patch.object(
            postgres_runtime,
            "_seal_postgresql_file_identity",
            side_effect=(expected_postgres, expected_dso),
        ), mock.patch.object(
            postgres_runtime,
            "_postgresql_postmaster_pid",
            side_effect=(42, 42),
        ), mock.patch.object(
            postgres_runtime,
            "_process_snapshot",
            autospec=True,
            return_value={42: postmaster_identity},
        ), mock.patch.object(
            postgres_runtime,
            "_postgresql_mapped_files",
            return_value=(
                postgres_runtime._PostgreSQLMappedFile(
                    path=expected_postgres.path,
                    device=expected_postgres.device,
                    inode=expected_postgres.inode,
                ),
                postgres_runtime._PostgreSQLMappedFile(
                    path=expected_dso.path,
                    device=expected_dso.device,
                    inode=expected_dso.inode,
                ),
            ),
        ):
            postgres_runtime._verify_postgresql_runtime_identity(
                "/var/lib/postgresql/17/main",
                "test-opt",
                expected_postgres,
                expected_dso,
                runtime_log,
            )

        self.assertIn("pid=42", runtime_log.getvalue())
        self.assertIn(expected_dso.sha256, runtime_log.getvalue())

    def test_runtime_identity_rejects_unmapped_dso_before_sql(self) -> None:
        expected_postgres = self._runtime_file_identity(
            "/toolchain/17/bin/postgres",
            inode=10, size=100, digest_character="a")
        expected_dso = self._runtime_file_identity(
            "/toolchain/17/lib/bioseq.so",
            inode=20, size=200, digest_character="b")
        postmaster_identity = self._postmaster_process_identity()
        with mock.patch.object(
            postgres_runtime,
            "_seal_postgresql_file_identity",
            side_effect=(expected_postgres, expected_dso),
        ), mock.patch.object(
            postgres_runtime, "_postgresql_postmaster_pid", return_value=42,
        ), mock.patch.object(
            postgres_runtime,
            "_process_snapshot",
            autospec=True,
            return_value={42: postmaster_identity},
        ), mock.patch.object(
            postgres_runtime,
            "_postgresql_mapped_files",
            return_value=(
                postgres_runtime._PostgreSQLMappedFile(
                    path=expected_postgres.path,
                    device=expected_postgres.device,
                    inode=expected_postgres.inode,
                ),
                postgres_runtime._PostgreSQLMappedFile(
                    path="/old/lib/bioseq.so",
                    device=1,
                    inode=99,
                ),
            ),
        ):
            with self.assertRaisesRegex(
                    RuntimeError, "does not map the sealed installed bioseq"):
                postgres_runtime._verify_postgresql_runtime_identity(
                    "/var/lib/postgresql/17/main",
                    "test-opt",
                    expected_postgres,
                    expected_dso,
                    io.StringIO(),
                )

    def test_e2e_restart_shell_pins_and_gates_runtime_identity(self) -> None:
        pg_config = "/toolchain/17/bin/pg_config"
        bindir = "/toolchain/17/bin"
        pkglibdir = "/toolchain/17/lib"
        data_dir = "/var/lib/postgresql/17/main"
        expected_postgres = self._runtime_file_identity(
            f"{bindir}/postgres",
            inode=10, size=100, digest_character="a")
        expected_dso = self._runtime_file_identity(
            f"{pkglibdir}/bioseq.so",
            inode=20, size=200, digest_character="b")
        with mock.patch.object(
            postgres_runtime, "_pg_config_from_cache", return_value=pg_config,
        ), mock.patch.object(
            postgres_runtime,
            "_postgresql_config_path",
            side_effect=(bindir, pkglibdir),
        ), mock.patch.object(
            postgres_runtime.os.path, "isfile", return_value=True,
        ), mock.patch.object(
            postgres_runtime.os.path, "isdir", return_value=True,
        ), mock.patch.object(
            postgres_runtime,
            "_query_postgresql_data_directory",
            return_value=data_dir,
        ), mock.patch.object(
            postgres_runtime.sys, "platform", "linux",
        ), mock.patch.object(
            postgres_runtime, "_is_aws_preset", return_value=False,
        ), mock.patch.object(
            postgres_runtime,
            "_seal_postgresql_file_identity",
            side_effect=(expected_postgres, expected_dso),
        ):
            command = postgres_runtime._postgresql_e2e_restart_shell_command(
                Path("/unused-build"), "bench-e2e-opt")

        self.assertIn(
            f"restart -p {bindir}/postgres -m fast -w &&", command)
        self.assertIn("verify-postgresql-runtime-identity", command)
        self.assertTrue(command.startswith("( "))
        self.assertTrue(command.endswith(
            " ) > /tmp/bioseq_e2e_pg_restart.log 2>&1"))

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
