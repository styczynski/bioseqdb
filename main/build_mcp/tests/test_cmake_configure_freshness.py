from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BUILD_MCP_DIR))

import server  # noqa: E402


class CMakeConfigureFreshnessTest(unittest.TestCase):
    def _write_project_presets(
            self,
            root: Path,
            *,
            include: list[str] | None = None) -> Path:
        presets = root / "CMakePresets.json"
        payload: dict[str, object] = {
            "version": 4,
            "configurePresets": [
                {
                    "name": "selected",
                    "inherits": "base",
                    "cacheVariables": {
                        "SELECTED": "yes",
                    },
                },
            ],
        }
        if include is not None:
            payload["include"] = include
        presets.write_text(json.dumps(payload), encoding="utf-8")
        return presets

    def _write_base_include(
            self,
            path: Path,
            *,
            value: object = "one",
            include: list[str] | None = None) -> None:
        payload: dict[str, object] = {
            "version": 4,
            "configurePresets": [
                {
                    "name": "base",
                    "cacheVariables": {
                        "INCLUDED": value,
                        "TOOLCHAIN":
                            "$env{CONFIGURE_FRESHNESS_SECRET}",
                    },
                },
            ],
        }
        if include is not None:
            payload["include"] = include
        path.write_text(json.dumps(payload), encoding="utf-8")

    def _document(
            self,
            presets: Path,
            *,
            secret: str = "secret-one",
            extra_environment: dict[str, str] | None = None,
    ) -> dict[str, object]:
        environment = {
            "CONFIGURE_FRESHNESS_SECRET": secret,
            "PATH": "/usr/bin",
        }
        if extra_environment is not None:
            environment.update(extra_environment)
        return server._cmake_configure_stamp_document(
            "selected",
            mode_cache_variables={"MODE": "cache"},
            managed_arguments=["-DMANAGED=one"],
            environment=environment,
            cmake_executable_identity={
                "resolved_path": "/tools/cmake",
                "sha256": "1" * 64,
                "version_line": "cmake version test",
            },
            presets_file=presets,
        )

    def test_transitive_include_changes_stamp_and_resolves_parent(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            nested = root / "nested.json"
            parent = root / "parent.json"
            nested.write_text(json.dumps({
                "version": 4,
                "configurePresets": [],
            }), encoding="utf-8")
            self._write_base_include(
                parent,
                include=["nested.json", "nested.json"],
            )
            presets = self._write_project_presets(
                root,
                include=["parent.json"],
            )
            first = self._document(presets)
            self.assertEqual(
                server._resolved_cmake_preset_cache_variables(
                    "selected",
                    presets_file=presets,
                    environment={
                        "CONFIGURE_FRESHNESS_SECRET": "secret-one",
                    },
                )["INCLUDED"],
                "one",
            )

            nested.write_text(json.dumps({
                "version": 4,
                "configurePresets": [],
                "vendor": {"test": "nested-content-changed"},
            }), encoding="utf-8")
            second = self._document(presets)

        self.assertNotEqual(
            first["inputs_sha256"],
            second["inputs_sha256"],
        )
        parent_record = next(
            record
            for record in first["inputs"]["preset_files"]
            if record["resolved_path"] == str(parent.resolve())
        )
        self.assertEqual(
            len(parent_record["ordered_include_targets"]),
            2,
        )

    def test_cmake_user_presets_are_part_of_the_implicit_graph(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            presets = root / "CMakePresets.json"
            presets.write_text(json.dumps({
                "version": 4,
                "configurePresets": [{
                    "name": "base",
                    "cacheVariables": {"BASE": "one"},
                }],
            }), encoding="utf-8")
            user = root / "CMakeUserPresets.json"
            user.write_text(json.dumps({
                "version": 4,
                "configurePresets": [{
                    "name": "selected",
                    "inherits": "base",
                    "cacheVariables": {"USER": "one"},
                }],
            }), encoding="utf-8")
            first = self._document(presets)
            user.write_text(json.dumps({
                "version": 4,
                "configurePresets": [{
                    "name": "selected",
                    "inherits": "base",
                    "cacheVariables": {"USER": "two"},
                }],
            }), encoding="utf-8")
            second = self._document(presets)

        self.assertEqual(
            first["inputs"]["resolved_cache_variables"]["USER"],
            "one",
        )
        self.assertNotEqual(
            first["inputs_sha256"],
            second["inputs_sha256"],
        )

    def test_include_failures_are_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            missing = self._write_project_presets(
                root,
                include=["missing.json"],
            )
            with self.assertRaisesRegex(
                    RuntimeError, "cannot resolve CMake preset include"):
                self._document(missing)

            cyclic = root / "cyclic.json"
            self._write_base_include(
                cyclic,
                include=["CMakePresets.json"],
            )
            self._write_project_presets(
                root,
                include=["cyclic.json"],
            )
            with self.assertRaisesRegex(
                    RuntimeError, "include cycle"):
                self._document(root / "CMakePresets.json")

            self._write_project_presets(
                root,
                include=["${unknownMacro}/presets.json"],
            )
            with self.assertRaisesRegex(
                    RuntimeError, "unsupported dynamic macro"):
                self._document(root / "CMakePresets.json")

            real_parent = root / "real_parent.json"
            self._write_base_include(real_parent)
            alias_parent = root / "alias_parent.json"
            alias_parent.symlink_to(real_parent)
            self._write_project_presets(
                root,
                include=["alias_parent.json"],
            )
            with self.assertRaisesRegex(
                    RuntimeError, "regular non-symlink file"):
                self._document(root / "CMakePresets.json")

    def test_mtime_only_change_keeps_stamp_stable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parent = root / "parent.json"
            self._write_base_include(parent)
            presets = self._write_project_presets(
                root,
                include=["parent.json"],
            )
            first = self._document(presets)
            stat_before = parent.stat()
            os.utime(
                parent,
                ns=(
                    stat_before.st_atime_ns,
                    stat_before.st_mtime_ns + 1_000_000,
                ),
            )
            second = self._document(presets)

        self.assertEqual(
            first["inputs_sha256"],
            second["inputs_sha256"],
        )

    def test_environment_is_content_sealed_without_plaintext(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parent = root / "parent.json"
            self._write_base_include(parent)
            presets = self._write_project_presets(
                root,
                include=["parent.json"],
            )
            first = self._document(presets, secret="do-not-persist")
            second = self._document(presets, secret="changed-secret")

        self.assertNotIn("do-not-persist", json.dumps(first))
        self.assertNotEqual(
            first["inputs_sha256"],
            second["inputs_sha256"],
        )

    def test_direct_cmake_environment_selector_is_content_sealed(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parent = root / "parent.json"
            self._write_base_include(parent)
            presets = self._write_project_presets(
                root,
                include=["parent.json"],
            )
            first = self._document(
                presets,
                extra_environment={
                    "BIOSEQ_LLVM_ROOT": "/private/llvm-one",
                },
            )
            second = self._document(
                presets,
                extra_environment={
                    "BIOSEQ_LLVM_ROOT": "/private/llvm-two",
                },
            )

        self.assertNotIn("/private/llvm-one", json.dumps(first))
        self.assertNotEqual(
            first["inputs_sha256"],
            second["inputs_sha256"],
        )

    def test_cmake_executable_identity_seals_binary_and_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "cmake"
            executable.write_text(
                "#!/bin/sh\nprintf 'cmake version 9.9-test\\n'\n",
                encoding="utf-8",
            )
            executable.chmod(0o755)
            with mock.patch.object(
                    server.shutil,
                    "which",
                    return_value=str(executable)):
                identity = server._cmake_executable_identity(
                    {"PATH": "/irrelevant"},
                )

        self.assertEqual(
            identity["resolved_path"],
            str(executable.resolve()),
        )
        self.assertEqual(
            identity["version_line"],
            "cmake version 9.9-test",
        )
        self.assertRegex(identity["sha256"], r"^[0-9a-f]{64}$")

    def test_matching_stamp_skips_existing_tree_configure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary)
            (build_dir / "build.ninja").write_text(
                "# configured\n",
                encoding="utf-8",
            )
            document = {
                "artifact_kind": server._CMAKE_CONFIGURE_STAMP_KIND,
                "schema_version": 1,
                "inputs": {},
                "inputs_sha256": "a" * 64,
            }
            server._publish_cmake_configure_stamp(
                build_dir,
                document,
            )
            with mock.patch.object(
                    server,
                    "_run_cmake_preset_configure") as configure:
                changed = server._ensure_cmake_preset_tree_fresh(
                    build_dir=build_dir,
                    preset="selected",
                    configure_stamp=document,
                    configure_arguments=[
                        "cmake",
                        "--preset",
                        "selected",
                    ],
                    log_handle=io.StringIO(),
                    deadline=1.0,
                    environment={},
                )

        self.assertFalse(changed)
        configure.assert_not_called()

    def test_missing_stamp_clears_cache_and_reconfigures_once(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary)
            build_ninja = build_dir / "build.ninja"
            build_ninja.write_text("# stale\n", encoding="utf-8")
            cache = build_dir / "CMakeCache.txt"
            cache.write_text(
                "REMOVED_PRESET_VALUE:STRING=must-not-survive\n",
                encoding="utf-8",
            )
            cmake_files = build_dir / "CMakeFiles"
            cmake_files.mkdir()
            (cmake_files / "stale").write_text(
                "stale",
                encoding="utf-8",
            )
            document = {
                "artifact_kind": server._CMAKE_CONFIGURE_STAMP_KIND,
                "schema_version": 1,
                "inputs": {},
                "inputs_sha256": "b" * 64,
            }

            def configure(*_args: object, **_kwargs: object) -> int:
                self.assertFalse(cache.exists())
                self.assertFalse(cmake_files.exists())
                build_ninja.write_text("# fresh\n", encoding="utf-8")
                cache.write_text(
                    "EXPECTED:STRING=fresh\n",
                    encoding="utf-8",
                )
                return 0

            with mock.patch.object(
                    server,
                    "_run_cmake_preset_configure",
                    side_effect=configure) as configure_mock:
                changed = server._ensure_cmake_preset_tree_fresh(
                    build_dir=build_dir,
                    preset="selected",
                    configure_stamp=document,
                    configure_arguments=[
                        "cmake",
                        "--preset",
                        "selected",
                    ],
                    log_handle=io.StringIO(),
                    deadline=1.0,
                    environment={},
                )

            self.assertTrue(changed)
            configure_mock.assert_called_once()
            self.assertNotIn(
                "REMOVED_PRESET_VALUE",
                cache.read_text(encoding="utf-8"),
            )

    def test_value_to_null_transition_cannot_reuse_old_cache(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parent = root / "parent.json"
            self._write_base_include(parent, value="one")
            presets = self._write_project_presets(
                root,
                include=["parent.json"],
            )
            before = self._document(presets)
            build_dir = root / "build"
            build_dir.mkdir()
            build_ninja = build_dir / "build.ninja"
            build_ninja.write_text("# old\n", encoding="utf-8")
            cache = build_dir / "CMakeCache.txt"
            cache.write_text(
                "INCLUDED:STRING=one\n",
                encoding="utf-8",
            )
            server._publish_cmake_configure_stamp(build_dir, before)

            self._write_base_include(parent, value=None)
            after = self._document(presets)
            self.assertIsNone(
                after["inputs"]["resolved_cache_variables"]["INCLUDED"],
            )

            def configure(*_args: object, **_kwargs: object) -> int:
                self.assertFalse(cache.exists())
                build_ninja.write_text("# refreshed\n", encoding="utf-8")
                cache.write_text(
                    "OTHER:STRING=present\n",
                    encoding="utf-8",
                )
                return 0

            with mock.patch.object(
                    server,
                    "_run_cmake_preset_configure",
                    side_effect=configure):
                server._ensure_cmake_preset_tree_fresh(
                    build_dir=build_dir,
                    preset="selected",
                    configure_stamp=after,
                    configure_arguments=[
                        "cmake",
                        "--preset",
                        "selected",
                    ],
                    log_handle=io.StringIO(),
                    deadline=1.0,
                    environment={},
                )

            self.assertNotIn(
                "INCLUDED",
                cache.read_text(encoding="utf-8"),
            )

    def test_every_repair_configure_invalidates_existing_stamp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary)
            document = {
                "artifact_kind": server._CMAKE_CONFIGURE_STAMP_KIND,
                "schema_version": 1,
                "inputs": {},
                "inputs_sha256": "9" * 64,
            }
            server._publish_cmake_configure_stamp(
                build_dir,
                document,
            )
            stamp = build_dir / server._CMAKE_CONFIGURE_STAMP_NAME

            def run(*_args: object, **_kwargs: object) -> int:
                self.assertFalse(stamp.exists())
                return 0

            with mock.patch.object(
                    server,
                    "_stream_subprocess",
                    side_effect=run) as subprocess_mock:
                result = server._run_cmake_preset_configure(
                    ["cmake", "--preset", "selected", "-DREPAIR=one"],
                    build_dir=build_dir,
                    log_handle=io.StringIO(),
                    deadline=1.0,
                    environment={},
                )

            self.assertEqual(result, 0)
            subprocess_mock.assert_called_once()

    def test_failed_reconfigure_invalidates_old_stamp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary)
            (build_dir / "build.ninja").write_text(
                "# stale\n",
                encoding="utf-8",
            )
            old = {
                "artifact_kind": server._CMAKE_CONFIGURE_STAMP_KIND,
                "schema_version": 1,
                "inputs": {"old": True},
                "inputs_sha256": "c" * 64,
            }
            new = {
                "artifact_kind": server._CMAKE_CONFIGURE_STAMP_KIND,
                "schema_version": 1,
                "inputs": {"new": True},
                "inputs_sha256": "d" * 64,
            }
            server._publish_cmake_configure_stamp(build_dir, old)
            with (
                mock.patch.object(
                    server,
                    "_run_cmake_preset_configure",
                    return_value=1,
                ),
                self.assertRaisesRegex(
                    RuntimeError,
                    "cmake configure exited with code 1",
                ),
            ):
                server._ensure_cmake_preset_tree_fresh(
                    build_dir=build_dir,
                    preset="selected",
                    configure_stamp=new,
                    configure_arguments=[
                        "cmake",
                        "--preset",
                        "selected",
                    ],
                    log_handle=io.StringIO(),
                    deadline=1.0,
                    environment={},
                )

            self.assertFalse(
                (build_dir / server._CMAKE_CONFIGURE_STAMP_NAME).exists(),
            )

    def test_mutation_during_configure_fails_before_resealing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary)
            before = {
                "artifact_kind": server._CMAKE_CONFIGURE_STAMP_KIND,
                "schema_version": 1,
                "inputs": {"value": "before"},
                "inputs_sha256": "e" * 64,
            }
            after = {
                **before,
                "inputs": {"value": "after"},
                "inputs_sha256": "f" * 64,
            }
            server._publish_cmake_configure_stamp(build_dir, before)
            with self.assertRaisesRegex(
                    RuntimeError, "refusing a mixed configuration"):
                server._finalize_cmake_configure_stamp(
                    build_dir,
                    before,
                    after,
                    context="test preset",
                )

            self.assertFalse(
                (build_dir / server._CMAKE_CONFIGURE_STAMP_NAME).exists(),
            )

    def test_failed_directory_fsync_leaves_no_published_stamp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary)
            document = {
                "artifact_kind": server._CMAKE_CONFIGURE_STAMP_KIND,
                "schema_version": 1,
                "inputs": {},
                "inputs_sha256": "8" * 64,
            }
            with (
                mock.patch.object(
                    server,
                    "_fsync_cmake_configure_stamp_directory",
                    side_effect=RuntimeError("fsync failed"),
                ),
                self.assertRaisesRegex(RuntimeError, "fsync failed"),
            ):
                server._publish_cmake_configure_stamp(
                    build_dir,
                    document,
                )

            self.assertFalse(
                (build_dir / server._CMAKE_CONFIGURE_STAMP_NAME).exists(),
            )


if __name__ == "__main__":
    unittest.main()
