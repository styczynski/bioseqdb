"""Focused unit contracts for the local package-linux qualification worker."""

from __future__ import annotations

import argparse
import inspect
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_linux as subject  # noqa: E402
import server  # noqa: E402


class PackageLinuxPureContractTests(unittest.TestCase):
    def test_sibling_final_codegen_binds_configured_main_source_root(self) -> None:
        helper = (
            Path(__file__).resolve().parents[2]
            / "cmake"
            / "BioseqSqlCodegen.cmake"
        ).read_text(encoding="utf-8")
        self.assertIn(
            '''      COMMAND "${CMAKE_COMMAND}" -E env
        "BIOSEQ_REVIEWED_MAIN_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
        "$<TARGET_FILE:bioseq_codegen>" sql generate
        --stage final
''',
            helper,
        )

    def test_mode_has_no_arbitrary_package_input(self) -> None:
        parameters = inspect.signature(subject.run_package_linux).parameters
        self.assertNotIn("package", parameters)
        self.assertEqual(subject.ARCHITECTURE, "arm64")
        self.assertEqual(subject.IMAGE_TAG, "debian:trixie")
        self.assertEqual(subject.POSTGRES_MAJOR, 17)

    def test_only_generated_output_and_bytecode_noise_are_ignored(self) -> None:
        status = (
            " M main/cmake/__pycache__/sanitizer_runtime.cpython-310.pyc\n"
            " M main/build_mcp/package_linux.py\n"
            "?? main/build/package-linux/partial\n"
            "?? main/reporting/package-linux/result.json\n"
            "?? main/untracked.txt\n"
        )
        self.assertEqual(
            subject._material_dirty_rows(status),
            [" M main/build_mcp/package_linux.py", "?? main/untracked.txt"],
        )
        self.assertEqual(
            subject._material_dirty_rows(
                " M main/a/b/__pycache__/module.cpython-313.pyc\n"),
            [],
        )

    def test_porcelain_capture_preserves_modified_worktree_column(self) -> None:
        status = " M main/cmake/__pycache__/sanitizer_runtime.cpython-310.pyc\n"
        with mock.patch.object(subject, "_run", return_value=status) as run:
            captured = subject._git_porcelain_status(
                Path("/repository"), log=io.StringIO(), deadline=time.time() + 10)
        self.assertEqual(captured, status.rstrip("\n"))
        self.assertEqual(subject._material_dirty_rows(captured), [])
        self.assertEqual(run.call_args.args[0][-5:], [
            "main",
            ":(top,exclude,literal)main/papers",
            ":(top,exclude,glob)main/papers/**",
            ":(top,exclude,literal)papers",
            ":(top,exclude,glob)papers/**",
        ])

    def test_native_x86_is_rejected(self) -> None:
        with mock.patch.object(subject.platform, "machine", return_value="x86_64"):
            with self.assertRaisesRegex(subject.PackageLinuxError, "native ARM64"):
                subject._native_arm64()

    def test_free_space_gate_accepts_exact_boundary_and_rejects_one_byte_below(
            self) -> None:
        for available, accepted in (
            (subject.MINIMUM_FREE_BYTES, True),
            (subject.MINIMUM_FREE_BYTES - 1, False),
        ):
            with self.subTest(available=available), mock.patch.object(
                subject.shutil, "disk_usage",
                return_value=SimpleNamespace(free=available),
            ):
                if accepted:
                    self.assertEqual(
                        subject._require_free_space(Path("/")), available)
                else:
                    with self.assertRaisesRegex(
                        subject.PackageLinuxError, "80 GiB"):
                        subject._require_free_space(Path("/"))

    def test_non_unix_docker_endpoint_is_rejected(self) -> None:
        for value in ("tcp://127.0.0.1:2375", "ssh://builder", "unix://"):
            with self.subTest(value=value):
                with self.assertRaises(subject.PackageLinuxError):
                    subject._docker_socket({"DOCKER_HOST": value})

    def test_cross_architecture_docker_flag_is_rejected_before_spawn(self) -> None:
        with mock.patch.object(subject.subprocess, "Popen") as spawn:
            with self.assertRaisesRegex(subject.PackageLinuxError, "cross-architecture"):
                subject._run(
                    ["docker", "run", "--platform=linux/amd64", "image"],
                    log=io.StringIO(), deadline=time.time() + 10,
                )
        spawn.assert_not_called()

    def test_content_seal_detects_tampering(self) -> None:
        value = subject._sealed({"schema_version": 1, "value": "original"})
        subject._validate_seal(value, "test")
        value["value"] = "changed"
        with self.assertRaisesRegex(subject.PackageLinuxError, "seal changed"):
            subject._validate_seal(value, "test")

    def test_build_identity_changes_with_image_or_tree(self) -> None:
        base = {"tree": "a" * 40, "image": "sha256:" + "b" * 64}
        first = subject._canonical_sha256(base)
        self.assertNotEqual(
            first, subject._canonical_sha256({**base, "tree": "c" * 40}))
        self.assertNotEqual(
            first,
            subject._canonical_sha256(
                {**base, "image": "sha256:" + "d" * 64}),
        )

    def test_extension_dso_producer_contract_uses_committed_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "main"
            helper = source / "cmake/AddPostgreSQLExtension.cmake"
            helper.parent.mkdir(parents=True)
            valid = '''elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # endif()
  if(TARGET fixture_map)
    message(STATUS "fixture map already exists")
  endif()
  set(_bioseq_dso_elf_build_id_link_option
    "LINKER:--build-id=sha1")
  target_link_options(fixture PRIVATE
    "${_bioseq_dso_elf_build_id_link_option}")
  endif()
'''
            helper.write_text(valid, encoding="utf-8")
            subject._validate_committed_extension_dso_contract(source)
            harmless_decoys = (
                valid + '\n# elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")\n'
                'message(STATUS "elseif(CMAKE_SYSTEM_NAME STREQUAL '
                '\\"Linux\\")")\n'
            )
            helper.write_text(harmless_decoys, encoding="utf-8")
            subject._validate_committed_extension_dso_contract(source)
            for invalid in (
                valid.replace('"LINKER:--build-id=sha1"', ""),
                valid.replace(
                    '"${_bioseq_dso_elf_build_id_link_option}"', ""),
                valid.replace(
                    '"LINKER:--build-id=sha1"',
                    '"LINKER:--build-id=sha1"\n'
                    '    "LINKER:--build-id=sha1"'),
                valid + '\nelseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")\n'
                '  endif()\n',
                valid.replace(
                    '"LINKER:--build-id=sha1"',
                    '# "LINKER:--build-id=sha1"'),
                valid.replace(
                    '"${_bioseq_dso_elf_build_id_link_option}"',
                    '# "${_bioseq_dso_elf_build_id_link_option}"'),
            ):
                helper.write_text(invalid, encoding="utf-8")
                with self.assertRaisesRegex(
                    subject.PackageLinuxError, "exactly one"):
                    subject._validate_committed_extension_dso_contract(source)


class PackageLinuxServerModeTests(unittest.TestCase):
    def test_mode_is_fixed_local_only_and_remote_rejected(self) -> None:
        self.assertIn(subject.MODE, server.MODES)
        self.assertIn(subject.MODE, server.REMOTE_RUN_REJECTED_MODES)
        self.assertNotIn(subject.MODE, server.REMOTE_RUN_BUILD_MODES)
        definition = server.MODES[subject.MODE]
        self.assertEqual(definition.preset_override, subject.MODE)
        self.assertEqual(definition.targets, [])
        self.assertEqual(definition.commands, [])
        self.assertFalse(definition.install_extension)
        self.assertEqual(
            definition.reporting_dir,
            server.REPORTING_DIR / subject.REPORTING_SUBDIR,
        )
        self.assertNotEqual(
            definition.reporting_dir,
            server.build_dir_for(subject.MODE, False),
        )

    def test_start_build_rejects_variable_package_inputs_before_spawn(self) -> None:
        with (
            mock.patch.object(server, "_refuse_root_build_identity", return_value=None),
            mock.patch.object(server, "_ensure_build_worker_timesharing_policy"),
            mock.patch.object(server.subprocess, "Popen") as spawn,
        ):
            result = server.tool_start_build(subject.MODE, build_only=True)
        self.assertIn("accepts no", result)
        spawn.assert_not_called()

    def test_detached_worker_routes_directly_to_package_qualifier(self) -> None:
        entry = {"started_at": time.time(), "log_file": "/tmp/package-linux.log"}
        with (
            mock.patch.object(server, "_refuse_root_build_identity", return_value=None),
            mock.patch.object(server, "_ensure_build_worker_timesharing_policy"),
            mock.patch.object(server, "_load_state", return_value={subject.MODE: entry}),
            mock.patch.object(server, "_run_package_linux_worker", return_value=0) as run,
        ):
            self.assertEqual(server._run_build_worker(subject.MODE), 0)
        run.assert_called_once_with(entry)

    def test_remote_run_rejects_before_provisioning(self) -> None:
        with mock.patch.object(server, "_start_or_poll_remote") as remote:
            result = server.tool_remote_run({
                "operation": "build", "mode": subject.MODE,
                "branch": "main",
                "sync": False, "auto_provision": False,
            })
        self.assertIn("local-only", result)
        remote.assert_not_called()


class DockerHealthTests(unittest.TestCase):
    def _serve_once(self, path: Path, response: bytes) -> threading.Thread:
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(path))
        server.listen(1)

        def serve() -> None:
            try:
                connection, _ = server.accept()
                with connection:
                    connection.recv(4096)
                    connection.sendall(response)
            finally:
                server.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        return thread

    def test_direct_ping_accepts_preexisting_healthy_socket(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "docker.sock"
            thread = self._serve_once(
                path, b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK")
            endpoint, resolved = subject._require_healthy_docker(
                {"DOCKER_HOST": f"unix://{path}"})
            expected = path.resolve(strict=True)
            thread.join(timeout=2)
        self.assertEqual(endpoint, f"unix://{path}")
        self.assertEqual(resolved, expected)

    def test_direct_ping_rejects_unhealthy_daemon_without_cli(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "docker.sock"
            thread = self._serve_once(
                path,
                b"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 2\r\n\r\nNO",
            )
            with mock.patch.object(subject.subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(subject.PackageLinuxError, "/_ping"):
                    subject._require_healthy_docker(
                        {"DOCKER_HOST": f"unix://{path}"})
            thread.join(timeout=2)
        spawn.assert_not_called()


class DockerCliIdentityTests(unittest.TestCase):
    @staticmethod
    def _entry(root: Path, target_name: str = "docker-tools") -> tuple[Path, Path]:
        target = root / target_name
        target.write_bytes(b"multicall docker executable\n")
        target.chmod(0o755)
        entry = root / "docker"
        entry.symlink_to(target)
        return entry, target

    def test_symlink_multicall_invocation_preserves_docker_argv0(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            entry, target = self._entry(Path(temporary))
            identity = subject._docker_cli_identity(entry)
            resolved_target = str(target.resolve(strict=True))
            inspect_result = json.dumps([{
                "Id": "sha256:" + "a" * 64,
                "Architecture": "arm64",
                "Os": "linux",
                "RepoDigests": ["debian@sha256:" + "b" * 64],
            }])
            with mock.patch.object(
                subject, "_run", side_effect=["", inspect_result]
            ) as run:
                subject._docker_image(
                    identity, {}, log=io.StringIO(), deadline=time.time() + 10)
            invocations = [call.args[0][0] for call in run.call_args_list]
        self.assertEqual(invocations, [str(entry), str(entry)])
        self.assertTrue(all(Path(value).name == "docker" for value in invocations))
        self.assertNotIn(str(target), invocations)
        self.assertEqual(
            identity["target_resolved_path"], resolved_target)
        self.assertRegex(identity["entry_identity_sha256"], r"^[0-9a-f]{64}$")
        self.assertRegex(identity["target_sha256"], r"^[0-9a-f]{64}$")

    def test_symlink_entry_substitution_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entry, _target = self._entry(root)
            identity = subject._docker_cli_identity(entry)
            replacement = root / "replacement-tools"
            replacement.write_bytes(b"replacement\n")
            replacement.chmod(0o755)
            entry.unlink()
            entry.symlink_to(replacement)
            with self.assertRaisesRegex(subject.PackageLinuxError, "identity changed"):
                subject._validated_docker_entry(identity)

    def test_resolved_multicall_target_tampering_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            entry, target = self._entry(Path(temporary))
            identity = subject._docker_cli_identity(entry)
            original_size = target.stat().st_size
            target.write_bytes(b"X" * original_size)
            target.chmod(0o755)
            self.assertEqual(target.stat().st_size, identity["target_size_bytes"])
            self.assertNotEqual(
                subject._sha256_file(target), identity["target_sha256"])
            with self.assertRaisesRegex(subject.PackageLinuxError, "identity changed"):
                subject._validated_docker_entry(identity)


class PackagePayloadTests(unittest.TestCase):
    def test_staging_manifest_requires_exact_six_unique_basenames(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            manifest = Path(temporary) / "package-files.list"
            manifest.write_text(
                "".join(f"/usr/share/{name}\n" for name in subject.EXPECTED_BASENAMES),
                encoding="utf-8",
            )
            evidence = subject._package_file_list(manifest)
            self.assertEqual(len(evidence["paths"]), 6)
            manifest.write_text(
                manifest.read_text(encoding="utf-8") + "/usr/share/extra.sql\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(subject.PackageLinuxError, "exact six"):
                subject._package_file_list(manifest)

    def _payload(self, root: Path) -> argparse.Namespace:
        extracted = root / "extracted"
        extracted.mkdir()
        for name in subject.EXPECTED_BASENAMES:
            (extracted / name).write_bytes((name + "\n").encode("ascii"))
        validator = root / "validator.json"
        validator.write_text(
            json.dumps({"passed": True, "six_artifacts": True}), encoding="utf-8")
        tool = root / "readelf"
        tool.write_bytes(b"readelf identity")
        self.tool = tool
        return argparse.Namespace(
            extracted=str(extracted), validator=str(validator),
            output=str(root / "payload.json"),
        )

    @staticmethod
    def _valid_readelf(_path: Path, arguments: list[str]) -> str:
        if arguments == ["-h"]:
            return "  Machine:                           AArch64\n"
        if arguments == ["-d"]:
            return (
                " 0x0000000000000001 (NEEDED) Shared library: [libc.so.6]\n"
                " 0x0000000000000001 (NEEDED) Shared library: [libm.so.6]\n")
        return "    Build ID: aabbccdd\n"

    def test_extraction_receipt_seals_six_hashes_and_elf_dependencies(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            arguments = self._payload(Path(temporary))
            with (
                mock.patch.object(subject, "_readelf", side_effect=self._valid_readelf),
                mock.patch.object(subject.shutil, "which", return_value=str(self.tool)),
            ):
                self.assertEqual(subject._internal_inspect(arguments), 0)
            receipt = subject._load_json(
                Path(arguments.output), "payload", sealed=True)
        self.assertEqual(
            {row["basename"] for row in receipt["artifacts"]},
            subject.EXPECTED_BASENAMES,
        )
        self.assertEqual(len(receipt["elf"]), 2)
        self.assertTrue(all(row["direct_load_dependencies"] for row in receipt["elf"]))
        self.assertTrue(all(row["gnu_build_ids"] for row in receipt["elf"]))

    def test_extraction_rejects_extra_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            arguments = self._payload(Path(temporary))
            (Path(arguments.extracted) / "extra").write_bytes(b"extra")
            with self.assertRaisesRegex(subject.PackageLinuxError, "exactly the six"):
                subject._internal_inspect(arguments)

    def test_extraction_rejects_hardlinked_payload_entries(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            arguments = self._payload(Path(temporary))
            source = Path(arguments.extracted) / "bioseq.so"
            alias = Path(arguments.extracted) / "bioseq_cache.so"
            alias.unlink()
            os.link(source, alias)
            with self.assertRaisesRegex(subject.PackageLinuxError, "single-link"):
                subject._internal_inspect(arguments)

    def test_extraction_rejects_duplicate_dependencies(self) -> None:
        def duplicate(path: Path, arguments: list[str]) -> str:
            value = self._valid_readelf(path, arguments)
            return value + value if arguments == ["-d"] else value

        with tempfile.TemporaryDirectory() as temporary:
            arguments = self._payload(Path(temporary))
            with mock.patch.object(subject, "_readelf", side_effect=duplicate):
                with self.assertRaisesRegex(subject.PackageLinuxError, "duplicate DT_NEEDED"):
                    subject._internal_inspect(arguments)

    def test_extraction_rejects_missing_build_id(self) -> None:
        def missing(path: Path, arguments: list[str]) -> str:
            return "no build identity\n" if arguments == ["-n"] else self._valid_readelf(
                path, arguments)

        with tempfile.TemporaryDirectory() as temporary:
            arguments = self._payload(Path(temporary))
            with mock.patch.object(subject, "_readelf", side_effect=missing):
                with self.assertRaisesRegex(subject.PackageLinuxError, "GNU build ID"):
                    subject._internal_inspect(arguments)


class ProvenanceAndStorageTests(unittest.TestCase):
    def test_source_snapshot_uses_one_captured_revision_when_head_moves(self) -> None:
        revision = "a" * 40
        tree = "b" * 40
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repository = root / "repository"
            repository.mkdir()
            state = root / "state"
            state.mkdir()

            def git_result(
                _repository: Path, arguments: list[str], **_kwargs: object,
            ) -> str:
                if arguments == ["rev-parse", "HEAD"]:
                    return revision
                if arguments == ["rev-parse", f"{revision}:main"]:
                    return tree
                raise AssertionError(f"mutable or unexpected Git ref: {arguments}")

            def archive_result(argv: list[str], **_kwargs: object) -> str:
                output = next(value for value in argv if value.startswith("--output="))
                Path(output.removeprefix("--output=")).write_bytes(b"archive")
                self.assertEqual(argv[-7:], [
                    revision,
                    "--",
                    "main",
                    ":(top,exclude,literal)main/papers",
                    ":(top,exclude,glob)main/papers/**",
                    ":(top,exclude,literal)papers",
                    ":(top,exclude,glob)papers/**",
                ])
                self.assertNotIn("--prefix=main/", argv)
                return ""

            def extract_result(_archive: Path, destination: Path) -> Path:
                source = destination / "main"
                source.mkdir()
                (source / "CMakeLists.txt").write_text("project(test)\n")
                for relative in subject._INPUT_FILES:
                    path = source / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(relative + "\n", encoding="utf-8")
                return source

            with (
                mock.patch.object(subject, "_git", side_effect=git_result) as git,
                mock.patch.object(
                    subject, "_git_porcelain_status",
                    return_value=(
                        " M main/cmake/__pycache__/"
                        "sanitizer_runtime.cpython-310.pyc")),
                mock.patch.object(subject, "_run", side_effect=archive_result),
                mock.patch.object(subject, "_safe_extract", side_effect=extract_result),
            ):
                snapshot = subject._source_snapshot(
                    repository, state, log=io.StringIO(), deadline=time.time() + 10)

        self.assertEqual(snapshot["revision"], revision)
        self.assertEqual(snapshot["main_tree"], tree)
        self.assertEqual(
            snapshot["ignored_generated_status_rows"],
            [" M main/cmake/__pycache__/sanitizer_runtime.cpython-310.pyc"],
        )
        self.assertEqual(
            [call.args[1] for call in git.call_args_list if call.args[1][0] == "rev-parse"],
            [["rev-parse", "HEAD"], ["rev-parse", f"{revision}:main"]],
        )

    def test_commit_archive_is_stable_and_excludes_paths_outside_main(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repository = root / "repository"
            repository.mkdir()
            source = repository / "main"
            source.mkdir()
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 4.2)\n", encoding="utf-8")
            for relative in subject._INPUT_FILES:
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(relative + "\n", encoding="utf-8")
            (repository / "outside.txt").write_text(
                "must not enter the source archive\n", encoding="utf-8")

            def git(*arguments: str, environment: dict[str, str] | None = None) -> None:
                subprocess.run(
                    ["git", "-C", str(repository), *arguments],
                    check=True,
                    capture_output=True,
                    text=True,
                    env=environment,
                )

            git("init", "-q")
            git("add", "--", "main", "outside.txt")
            commit_environment = {
                **os.environ,
                "GIT_AUTHOR_DATE": "2001-02-03T04:05:06+00:00",
                "GIT_COMMITTER_DATE": "2001-02-03T04:05:06+00:00",
            }
            git(
                "-c", "user.name=BioSeq Test",
                "-c", "user.email=bioseq-test@example.invalid",
                "-c", "commit.gpgsign=false",
                "commit", "-q", "-m", "fixture",
                environment=commit_environment,
            )
            state = root / "state"
            state.mkdir()
            with (root / "snapshot.log").open(
                "w+", encoding="utf-8"
            ) as snapshot_log:
                first = subject._source_snapshot(
                    repository, state, log=snapshot_log,
                    deadline=time.time() + 30)
                time.sleep(1.1)
                second = subject._source_snapshot(
                    repository, state, log=snapshot_log,
                    deadline=time.time() + 30)

            identity_fields = (
                "revision", "main_tree", "archive_sha256", "input_sha256")
            self.assertEqual(
                {field: first[field] for field in identity_fields},
                {field: second[field] for field in identity_fields},
            )
            for snapshot in (first, second):
                with tarfile.open(Path(snapshot["archive"]), "r:") as bundle:
                    names = {member.name for member in bundle.getmembers()}
                self.assertIn("main/CMakeLists.txt", names)
                self.assertTrue(
                    all(name == "main" or name.startswith("main/") for name in names))
                self.assertNotIn("outside.txt", names)

    def test_existing_source_snapshot_root_accepts_only_its_archive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source-test"
            root.mkdir()
            archive = root / "main.tar"
            body = b"cmake_minimum_required(VERSION 4.2)\n"
            with tarfile.open(archive, "w") as bundle:
                member = tarfile.TarInfo("main/CMakeLists.txt")
                member.size = len(body)
                bundle.addfile(member, io.BytesIO(body))
            source = subject._safe_extract(archive, root)
            self.assertEqual(source, root / "main")
            self.assertEqual((source / "CMakeLists.txt").read_bytes(), body)

    def test_source_archive_rejects_links(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "source.tar"
            with tarfile.open(archive, "w") as bundle:
                member = tarfile.TarInfo("main/link")
                member.type = tarfile.SYMTYPE
                member.linkname = "../outside"
                bundle.addfile(member)
            with self.assertRaisesRegex(subject.PackageLinuxError, "unsafe member"):
                subject._safe_extract(archive, root / "out")

    def test_source_archive_rejects_sealed_boundary_before_extraction(self) -> None:
        for member_name in ("main/papers", "main/papers/hidden.md", "papers"):
            with self.subTest(member_name=member_name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive = root / "source.tar"
                with tarfile.open(archive, "w") as bundle:
                    member = tarfile.TarInfo(member_name)
                    member.size = 0
                    bundle.addfile(member, io.BytesIO())
                destination = root / "out"
                with self.assertRaisesRegex(
                    subject.PackageLinuxError, "sealed source boundary"
                ):
                    subject._safe_extract(archive, destination)
                self.assertEqual(list(destination.iterdir()), [])

    def test_content_addressed_objects_are_not_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.write_bytes(b"first")
            first = subject._store_content_addressed(
                source, root / "objects", suffix=".deb")
            source.write_bytes(b"second")
            second = subject._store_content_addressed(
                source, root / "objects", suffix=".deb")
            self.assertNotEqual(first, second)
            self.assertEqual(first.read_bytes(), b"first")
            self.assertEqual(second.read_bytes(), b"second")

    def test_atomic_receipt_is_regular_and_sealed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "receipt.json"
            value = subject._sealed({"schema_version": 1, "passed": True})
            subject._write_json_atomic(path, value)
            self.assertFalse(path.is_symlink())
            self.assertEqual(subject._load_json(path, "receipt", sealed=True), value)

    def test_docker_health_failure_precedes_any_docker_cli(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary)
            project = repository / "main"
            project.mkdir()
            state = project / "build/package-linux"
            state.mkdir(parents=True)
            source_root = state / "source-test"
            source = source_root / "main"
            source.mkdir(parents=True)
            snapshot = {
                "ephemeral_root": source_root,
                "source": source,
                "archive": source_root / "main.tar",
                "revision": "a" * 40,
                "main_tree": "b" * 40,
                "archive_sha256": "c" * 64,
                "input_sha256": {},
            }
            with (
                mock.patch.object(subject, "_native_arm64", return_value="arm64"),
                mock.patch.object(
                    subject, "_require_free_space",
                    return_value=subject.MINIMUM_FREE_BYTES) as free_space,
                mock.patch.object(subject, "_source_snapshot", return_value=snapshot),
                mock.patch.object(
                    subject, "_validate_committed_extension_dso_contract"),
                mock.patch.object(
                    subject, "_require_healthy_docker",
                    side_effect=subject.PackageLinuxError("daemon unhealthy")),
                mock.patch.object(subject, "_docker_image") as image,
            ):
                with self.assertRaisesRegex(subject.PackageLinuxError, "daemon unhealthy"):
                    subject.run_package_linux(
                        project_root=project, workspace_root=repository,
                        log_handle=io.StringIO(), deadline=time.time() + 30,
                        environment={},
                    )
            self.assertEqual(free_space.call_count, 2)
            image.assert_not_called()

    def test_producer_contract_failure_cleans_snapshot_before_docker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary)
            project = repository / "main"
            project.mkdir()
            state = project / "build/package-linux"
            state.mkdir(parents=True)
            source_root = state / "source-test"
            source = source_root / "main"
            source.mkdir(parents=True)
            snapshot = {
                "ephemeral_root": source_root,
                "source": source,
                "archive": source_root / "main.tar",
                "revision": "a" * 40,
                "main_tree": "b" * 40,
                "archive_sha256": "c" * 64,
                "input_sha256": {},
            }
            with (
                mock.patch.object(subject, "_native_arm64", return_value="arm64"),
                mock.patch.object(
                    subject, "_require_free_space",
                    return_value=subject.MINIMUM_FREE_BYTES),
                mock.patch.object(subject, "_source_snapshot", return_value=snapshot),
                mock.patch.object(
                    subject, "_validate_committed_extension_dso_contract",
                    side_effect=subject.PackageLinuxError("producer drift")),
                mock.patch.object(subject, "_require_healthy_docker") as docker,
            ):
                with self.assertRaisesRegex(
                    subject.PackageLinuxError, "producer drift"):
                    subject.run_package_linux(
                        project_root=project,
                        workspace_root=repository,
                        log_handle=io.StringIO(),
                        deadline=time.time() + 30,
                        environment={},
                    )
            docker.assert_not_called()
            self.assertFalse(source_root.exists())

    def test_post_snapshot_free_space_gate_fails_before_docker_and_cleans_source(
            self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary)
            project = repository / "main"
            project.mkdir()
            state = project / "build/package-linux"
            state.mkdir(parents=True)
            source_root = state / "source-test"
            source = source_root / "main"
            source.mkdir(parents=True)
            snapshot = {
                "ephemeral_root": source_root,
                "source": source,
                "archive": source_root / "main.tar",
                "revision": "a" * 40,
                "main_tree": "b" * 40,
                "archive_sha256": "c" * 64,
                "input_sha256": {},
            }
            initial = subject.MINIMUM_FREE_BYTES + 200 * 1024**2
            after_snapshot = subject.MINIMUM_FREE_BYTES - 600 * 1024**2
            with (
                mock.patch.object(subject, "_native_arm64", return_value="arm64"),
                mock.patch.object(
                    subject.shutil,
                    "disk_usage",
                    side_effect=(
                        SimpleNamespace(free=initial),
                        SimpleNamespace(free=after_snapshot),
                    ),
                ) as disk_usage,
                mock.patch.object(subject, "_source_snapshot", return_value=snapshot),
                mock.patch.object(subject, "_require_healthy_docker") as docker,
            ):
                with self.assertRaisesRegex(subject.PackageLinuxError, "80 GiB"):
                    subject.run_package_linux(
                        project_root=project,
                        workspace_root=repository,
                        log_handle=io.StringIO(),
                        deadline=time.time() + 30,
                        environment={},
                    )
            self.assertEqual(disk_usage.call_count, 2)
            docker.assert_not_called()
            self.assertFalse(source_root.exists())


if __name__ == "__main__":
    unittest.main()
