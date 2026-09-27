from __future__ import annotations

import argparse
import hashlib
import importlib.util
import io
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import textwrap
import threading
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "package_builder.py"
WORKFLOW_PATH = (
    Path(__file__).resolve().parents[2]
    / ".github"
    / "workflows"
    / "package-release.yml"
)
SPEC = importlib.util.spec_from_file_location("package_builder", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
package_builder = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = package_builder
SPEC.loader.exec_module(package_builder)


def compiler_optimization_evidence(
    architecture: str,
    *,
    distros: tuple[str, ...] | None = None,
    postgres: tuple[int, ...] = (17,),
) -> dict[str, object]:
    if distros is None:
        distros = tuple(package_builder.load_matrix().distributions)
    cache_paths = sorted(
        f"{distro}/postgresql{major}/build-repository-root/CMakeCache.txt"
        for distro in distros
        for major in postgres
    )
    database_paths = [
        path.removesuffix("CMakeCache.txt") + "compile_commands.json"
        for path in cache_paths
    ]
    database_sha256 = "c" * 64
    database_records = [
        f"{path}\0{database_sha256}\0{2}" for path in database_paths
    ]
    build_paths = [
        path.removesuffix("CMakeCache.txt") + "build.ninja"
        for path in cache_paths
    ]
    build_sha256 = "d" * 64
    compiler_commands_sha256 = "e" * 64
    build_records = [
        f"{path}\0{build_sha256}\0{2}\0{1}\0{compiler_commands_sha256}"
        for path in build_paths
    ]
    compile_databases = [
        {
            "path": path,
            "sha256": database_sha256,
            "command_count": 2,
            "language_compile_commands": {"C": 1, "CXX": 1},
        }
        for path in database_paths
    ]
    database_count = len(database_paths)
    return {
        "policy_revision": package_builder.OPTIMIZATION_POLICY_REVISION,
        "requested": package_builder.PRODUCTION_OPTIMIZATION,
        "architecture": architecture,
        "build_type": "Release",
        "c_compiler": "/opt/bioseq-package-toolchain/llvm-22/bin/clang",
        "cxx_compiler": "/opt/bioseq-package-toolchain/llvm-22/bin/clang++",
        "effective_c_flags": ["-O3", "-DNDEBUG", "-fno-omit-frame-pointer"],
        "effective_cxx_flags": [
            "-stdlib=libc++",
            "-O3",
            "-DNDEBUG",
            "-fno-omit-frame-pointer",
        ],
        "optimization_flags": ["-O3"],
        "forbidden_optimization_flags": [],
        "cmake_cache_paths": cache_paths,
        "compile_database_count": database_count,
        "compile_databases": compile_databases,
        "compile_command_count": 2 * database_count,
        "language_compile_commands": {
            "C": database_count,
            "CXX": database_count,
        },
        "compile_databases_sha256": hashlib.sha256(
            "\n".join(database_records).encode("utf-8")
        ).hexdigest(),
        "ninja_build_file_count": len(build_paths),
        "ninja_build_files": [
            {
                "path": path,
                "sha256": build_sha256,
                "compile_command_count": 2,
                "link_command_count": 1,
                "compiler_commands_sha256": compiler_commands_sha256,
            }
            for path in build_paths
        ],
        "ninja_compile_command_count": 2 * len(build_paths),
        "link_command_count": len(build_paths),
        "ninja_commands_sha256": hashlib.sha256(
            "\n".join(build_records).encode("utf-8")
        ).hexdigest(),
    }


class SourceArchiveContractTests(unittest.TestCase):
    def test_remote_source_tree_hash_detects_content_and_extra_entries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "CMakeLists.txt"
            source.write_text("project(bioseq)\n", encoding="utf-8")

            def digest() -> str:
                result = subprocess.run(
                    [sys.executable, "-c", package_builder.SOURCE_TREE_HASH_SCRIPT, str(root)],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                return result.stdout.strip()

            baseline = digest()
            self.assertRegex(baseline, r"^[0-9a-f]{64}$")
            source.write_text("project(changed)\n", encoding="utf-8")
            self.assertNotEqual(digest(), baseline)
            source.write_text("project(bioseq)\n", encoding="utf-8")
            (root / "unexpected.txt").write_text("extra\n", encoding="utf-8")
            self.assertNotEqual(digest(), baseline)

    def test_committed_source_tree_hash_binds_git_modes_objects_and_exclusions(self) -> None:
        records = (
            "100755 blob " + "2" * 40 + "\ttools/build\0"
            "100644 blob " + "1" * 40 + "\tCMakeLists.txt\0"
            "100644 blob " + "3" * 40 + "\tmain/papers/secret.tex\0"
            "100644 blob " + "4" * 40 + "\tpapers/secret.tex\0"
        )
        included = sorted(
            (
                (b"tools/build", ("100755 blob " + "2" * 40 + "\ttools/build\0").encode()),
                (b"CMakeLists.txt", ("100644 blob " + "1" * 40 + "\tCMakeLists.txt\0").encode()),
            )
        )
        expected = hashlib.sha256(
            b"".join(record for _path, record in included)
        ).hexdigest()
        with mock.patch.object(
            package_builder,
            "run_command",
            return_value=subprocess.CompletedProcess([], 0, records, ""),
        ) as run:
            observed = package_builder.committed_source_tree_sha256("a" * 40)
        self.assertEqual(observed, expected)
        self.assertIn("ls-tree", run.call_args.args[0])
        self.assertIn("-z", run.call_args.args[0])

    def test_committed_source_archive_excludes_sealed_paths_before_streaming(self) -> None:
        builder = package_builder.Builder(
            "arm64",
            "i-0aaabbbcccdddeee1",
            "c7g.xlarge",
            "arm64",
            "203.0.113.1",
            "10.0.0.1",
            "ami-0123456789abcdef0",
        )
        revision = "a" * 40
        expected_remote_source = (
            "/opt/bioseq-package-builder/workspace/"
            f"source-{package_builder.SOURCE_ARCHIVE_IDENTITY}-revision-"
            f"{package_builder.SOURCE_ARCHIVE_REVISION}-{revision}"
        )
        token = "1" * 32
        tree_sha256 = "e" * 64
        staging = expected_remote_source + f".tmp-{token}"
        archive_process = mock.Mock(
            stdout=io.BytesIO(), returncode=0)
        archive_process.wait.return_value = 0
        extract_process = mock.Mock()
        extract_process.wait.return_value = 0
        ssh_responses = iter((
            subprocess.CompletedProcess([], 4, "", ""),
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "", ""),
        ))
        with mock.patch.object(
            package_builder.secrets, "token_hex", return_value=token,
        ), mock.patch.object(
            package_builder,
            "committed_source_tree_sha256",
            return_value=tree_sha256,
        ), mock.patch.object(
            package_builder, "ssh_run",
            side_effect=lambda *_args, **_kwargs: next(ssh_responses),
        ) as ssh_run, mock.patch.object(
            package_builder, "ssh_argv", return_value=["ssh"],
        ), mock.patch.object(
            package_builder.subprocess,
            "Popen",
            side_effect=(archive_process, extract_process),
        ) as popen:
            remote_source = package_builder.sync_committed_source(
                builder, Path("/tmp/key"), revision)

        self.assertEqual(remote_source, expected_remote_source)
        archive_argv = popen.call_args_list[0].args[0]
        self.assertEqual(archive_argv, [
            "git",
            "archive",
            "--format=tar",
            revision,
            "--",
            ".",
            ":(top,exclude,literal)papers",
            ":(top,exclude,glob)papers/**",
            ":(top,exclude,literal)main/papers",
            ":(top,exclude,glob)main/papers/**",
        ])
        self.assertEqual(
            popen.call_args_list[0].kwargs["env"]["GIT_LFS_SKIP_SMUDGE"], "1"
        )
        extract_argv = popen.call_args_list[1].args[0]
        self.assertEqual(extract_argv, ["ssh", f"tar -xf - -C {staging}"])
        commands = [call.args[2] for call in ssh_run.call_args_list]
        self.assertNotIn("install -d", "\n".join(commands))
        self.assertEqual(commands[1], f"mkdir -m 0755 -- {staging}")
        self.assertIn(f"staging={staging}", commands[2])
        self.assertIn(f"destination={expected_remote_source}", commands[2])
        self.assertIn(
            f"quarantine={expected_remote_source}.invalid-{token}", commands[2]
        )
        self.assertIn('mv -T -- "$destination" "$quarantine"', commands[2])
        expected_marker = hashlib.sha256(
            (
                f"{package_builder.SOURCE_ARCHIVE_IDENTITY}\n"
                f"revision={package_builder.SOURCE_ARCHIVE_REVISION}\n"
                f"source_revision={revision}\n"
                f"source_tree_sha256={tree_sha256}\n"
            ).encode("ascii")
        ).hexdigest()
        self.assertIn(f"marker={expected_marker}", commands[2])
        self.assertIn('mv -T -- "$staging" "$destination"', commands[2])
        self.assertLess(
            commands[2].index(".bioseq-source-marker"),
            commands[2].index("flock -x 9"),
        )
        self.assertLess(
            commands[2].index("flock -x 9"),
            commands[2].index("valid_destination"),
        )
        self.assertIn(".bioseq-source-marker", commands[0])
        self.assertIn(".bioseq-source-tree-sha256", commands[0])
        self.assertIn("hashlib.sha1", commands[0])
        self.assertIn(f"tree_sha256={tree_sha256}", commands[0])
        self.assertIn("flock -x 9", commands[0])
        self.assertIn("cmp -s", commands[0])
        self.assertEqual(commands[0], commands[3])
        self.assertEqual(commands[4], f"rm -rf -- {staging}")
        self.assertTrue(all("main/papers" not in command for command in commands))
        self.assertEqual(
            package_builder.SOURCE_ARCHIVE_IDENTITY,
            "repository-root-sealed-papers-excluded",
        )
        self.assertEqual(package_builder.SOURCE_ARCHIVE_REVISION, 4)
        self.assertEqual(
            package_builder._remote_committed_source_path(revision),
            expected_remote_source,
        )

    def test_committed_source_reuses_only_validated_publication(self) -> None:
        builder = package_builder.Builder(
            "arm64", "i-0aaabbbcccdddeee1", "c7g.xlarge", "arm64",
            "203.0.113.1", "10.0.0.1", "ami-0123456789abcdef0",
        )
        with mock.patch.object(
            package_builder,
            "committed_source_tree_sha256",
            return_value="e" * 64,
        ), mock.patch.object(
            package_builder, "ssh_run",
            return_value=subprocess.CompletedProcess([], 0, "", ""),
        ) as ssh_run, mock.patch.object(
            package_builder.subprocess, "Popen",
        ) as popen:
            remote_source = package_builder.sync_committed_source(
                builder, Path("/tmp/key"), "b" * 40)

        self.assertIn(package_builder.SOURCE_ARCHIVE_IDENTITY, remote_source)
        self.assertIn(
            f"revision-{package_builder.SOURCE_ARCHIVE_REVISION}", remote_source)
        self.assertEqual(ssh_run.call_count, 1)
        self.assertIn(".bioseq-source-marker", ssh_run.call_args.args[2])
        self.assertIn("flock -x 9", ssh_run.call_args.args[2])
        self.assertIn("cmp -s", ssh_run.call_args.args[2])
        self.assertIn("hashlib.sha1", ssh_run.call_args.args[2])
        popen.assert_not_called()

    def test_committed_source_rejects_symbolic_revision_before_io(self) -> None:
        builder = package_builder.Builder(
            "arm64", "i-0aaabbbcccdddeee1", "c7g.xlarge", "arm64",
            "203.0.113.1", "10.0.0.1", "ami-0123456789abcdef0",
        )
        with mock.patch.object(
            package_builder, "ssh_run",
        ) as ssh_run, mock.patch.object(
            package_builder, "ssh_argv",
        ) as ssh_argv, mock.patch.object(
            package_builder.subprocess, "Popen",
        ) as popen, self.assertRaisesRegex(
            package_builder.PackageBuilderError, "full Git revision",
        ):
            package_builder.sync_committed_source(
                builder, Path("/tmp/key"), "HEAD")

        ssh_run.assert_not_called()
        ssh_argv.assert_not_called()
        popen.assert_not_called()

    def test_committed_source_transfer_failures_never_publish(self) -> None:
        builder = package_builder.Builder(
            "arm64", "i-0aaabbbcccdddeee1", "c7g.xlarge", "arm64",
            "203.0.113.1", "10.0.0.1", "ami-0123456789abcdef0",
        )
        revision = "c" * 40
        for git_return, extract_return in ((1, 0), (0, 1)):
            archive_process = mock.Mock(stdout=io.BytesIO())
            archive_process.wait.return_value = git_return
            archive_process.poll.return_value = git_return
            extract_process = mock.Mock()
            extract_process.wait.return_value = extract_return
            extract_process.poll.return_value = extract_return
            responses = iter((
                subprocess.CompletedProcess([], 3, "", ""),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "", ""),
            ))
            with self.subTest(
                git_return=git_return, extract_return=extract_return,
            ), mock.patch.object(
                package_builder,
                "committed_source_tree_sha256",
                return_value="e" * 64,
            ), mock.patch.object(
                package_builder.secrets, "token_hex", return_value="2" * 32,
            ), mock.patch.object(
                package_builder, "ssh_run",
                side_effect=lambda *_args, **_kwargs: next(responses),
            ) as ssh_run, mock.patch.object(
                package_builder, "ssh_argv", return_value=["ssh"],
            ), mock.patch.object(
                package_builder.subprocess,
                "Popen",
                side_effect=(archive_process, extract_process),
            ), self.assertRaisesRegex(
                package_builder.PackageBuilderError, "source transfer",
            ):
                package_builder.sync_committed_source(
                    builder, Path("/tmp/key"), revision)

            commands = [call.args[2] for call in ssh_run.call_args_list]
            self.assertEqual(len(commands), 3)
            self.assertTrue(commands[-1].startswith("rm -rf -- "))
            self.assertTrue(all("mv -T" not in command for command in commands))

    def test_committed_source_interrupt_reaps_both_transfer_children(self) -> None:
        builder = package_builder.Builder(
            "arm64", "i-0aaabbbcccdddeee1", "c7g.xlarge", "arm64",
            "203.0.113.1", "10.0.0.1", "ami-0123456789abcdef0",
        )
        archive_process = mock.Mock(stdout=io.BytesIO())
        archive_process.poll.side_effect = (None, 0)
        archive_process.wait.return_value = 0
        extract_process = mock.Mock()
        extract_process.poll.side_effect = (None, 0)
        extract_process.wait.side_effect = (KeyboardInterrupt(), 0)
        responses = iter((
            subprocess.CompletedProcess([], 3, "", ""),
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "", ""),
        ))
        with mock.patch.object(
            package_builder,
            "committed_source_tree_sha256",
            return_value="e" * 64,
        ), mock.patch.object(
            package_builder.secrets, "token_hex", return_value="3" * 32,
        ), mock.patch.object(
            package_builder, "ssh_run",
            side_effect=lambda *_args, **_kwargs: next(responses),
        ), mock.patch.object(
            package_builder, "ssh_argv", return_value=["ssh"],
        ), mock.patch.object(
            package_builder.subprocess,
            "Popen",
            side_effect=(archive_process, extract_process),
        ), self.assertRaises(KeyboardInterrupt):
            package_builder.sync_committed_source(
                builder, Path("/tmp/key"), "d" * 40)

        archive_process.terminate.assert_called_once_with()
        extract_process.terminate.assert_called_once_with()
        self.assertNotIn(archive_process, package_builder.ACTIVE_CHILDREN)
        self.assertNotIn(extract_process, package_builder.ACTIVE_CHILDREN)


class OptimizationPolicyTests(unittest.TestCase):
    O3_FLAGS = ["-O3", "-DNDEBUG", "-fno-omit-frame-pointer"]

    def optimization_root(
        self,
        root: Path,
        flags: list[str],
        distro: str = "fedora_44",
    ) -> Path:
        build_root = (
            root / distro / "postgresql17" / "build-repository-root"
        )
        build_root.mkdir(parents=True, exist_ok=True)
        cache = "\n".join(
            (
                "CMAKE_BUILD_TYPE:STRING=Release",
                "CMAKE_C_FLAGS:STRING=",
                "CMAKE_CXX_FLAGS:STRING=-stdlib=libc++",
                (
                    "CMAKE_C_FLAGS_RELEASE:STRING="
                    "-O3 -DNDEBUG -fno-omit-frame-pointer"
                ),
                (
                    "CMAKE_CXX_FLAGS_RELEASE:STRING="
                    "-O3 -DNDEBUG -fno-omit-frame-pointer"
                ),
                (
                    "CMAKE_C_COMPILER:FILEPATH="
                    "/opt/bioseq-package-toolchain/llvm-22/bin/clang"
                ),
                (
                    "CMAKE_CXX_COMPILER:FILEPATH="
                    "/opt/bioseq-package-toolchain/llvm-22/bin/clang++"
                ),
            )
        )
        (build_root / "CMakeCache.txt").write_text(
            cache + "\n", encoding="utf-8"
        )
        entries = [
            {
                "arguments": [
                    "/opt/bioseq-package-toolchain/llvm-22/bin/clang",
                    *flags,
                    "-c",
                    "source.c",
                ],
                "file": "source.c",
            },
            {
                "arguments": [
                    "/opt/bioseq-package-toolchain/llvm-22/bin/clang++",
                    "-stdlib=libc++",
                    *flags,
                    "-c",
                    "source.cpp",
                ],
                "file": "source.cpp",
            },
        ]
        (build_root / "compile_commands.json").write_text(
            json.dumps(entries), encoding="utf-8"
        )
        rendered_flags = " ".join(flags)
        (build_root / "build.ninja").write_text(
            textwrap.dedent(
                f"""
                rule cc
                  command = /opt/bioseq-package-toolchain/llvm-22/bin/clang {rendered_flags} -c source.c -o source.c.o
                rule cxx
                  command = /opt/bioseq-package-toolchain/llvm-22/bin/clang++ -stdlib=libc++ {rendered_flags} -c source.cpp -o source.cpp.o
                rule link
                  command = /opt/bioseq-package-toolchain/llvm-22/bin/clang++ -stdlib=libc++ {rendered_flags} source.c.o source.cpp.o -o package-test
                build source.c.o: cc
                build source.cpp.o: cxx
                build package-test: link source.c.o source.cpp.o
                default package-test
                """
            ).lstrip(),
            encoding="utf-8",
        )
        return root

    def run_evidence(
        self,
        root: Path,
        expected_cache_paths: list[str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        if expected_cache_paths is None:
            expected_cache_paths = [
                "fedora_44/postgresql17/build-repository-root/CMakeCache.txt"
            ]
        return subprocess.run(
            [
                sys.executable,
                "-c",
                package_builder.REMOTE_OPTIMIZATION_EVIDENCE_SCRIPT,
                str(root),
                "arm64",
                json.dumps(expected_cache_paths, separators=(",", ":")),
            ],
            capture_output=True,
            text=True,
        )

    def test_remote_compiler_evidence_records_effective_o3_flags(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = self.optimization_root(Path(tmp), self.O3_FLAGS)
            self.optimization_root(root, self.O3_FLAGS, "ubuntu_26.04")
            completed = self.run_evidence(
                root,
                [
                    "fedora_44/postgresql17/build-repository-root/"
                    "CMakeCache.txt",
                    "ubuntu_26.04/postgresql17/build-repository-root/"
                    "CMakeCache.txt",
                ],
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        evidence = json.loads(completed.stdout)
        package_builder.require_compiler_optimization_evidence(
            evidence,
            architecture="arm64",
            label="test optimization evidence",
        )
        self.assertEqual(evidence["optimization_flags"], ["-O3"])
        self.assertEqual(evidence["compile_database_count"], 2)
        self.assertEqual(evidence["compile_command_count"], 4)
        self.assertEqual(evidence["ninja_build_file_count"], 2)
        self.assertEqual(evidence["ninja_compile_command_count"], 4)
        self.assertEqual(evidence["link_command_count"], 2)
        self.assertEqual(
            evidence["language_compile_commands"], {"C": 2, "CXX": 2}
        )
        self.assertEqual(
            evidence["effective_cxx_flags"],
            ["-stdlib=libc++", "-O3", "-DNDEBUG", "-fno-omit-frame-pointer"],
        )

    def test_remote_compiler_evidence_requires_every_selected_cell_cache(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = self.optimization_root(Path(tmp), self.O3_FLAGS)
            completed = self.run_evidence(
                root,
                [
                    "fedora_44/postgresql17/build-repository-root/"
                    "CMakeCache.txt",
                    "ubuntu_26.04/postgresql17/build-repository-root/"
                    "CMakeCache.txt",
                ],
            )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("selected production package cache is missing", completed.stderr)

    def test_remote_compiler_evidence_requires_both_languages_per_selected_cache(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = self.optimization_root(Path(tmp), self.O3_FLAGS)
            database = (
                root
                / "fedora_44"
                / "postgresql17"
                / "build-repository-root"
                / "compile_commands.json"
            )
            entries = json.loads(database.read_text(encoding="utf-8"))
            database.write_text(
                json.dumps([entry for entry in entries if entry["file"] == "source.cpp"]),
                encoding="utf-8",
            )
            completed = self.run_evidence(root)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("selected production package cache lacks C/C++ coverage", completed.stderr)

    def test_remote_compiler_evidence_allows_language_specific_nested_databases(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = self.optimization_root(Path(tmp), self.O3_FLAGS)
            build_root = (
                root / "fedora_44" / "postgresql17" / "build-repository-root"
            )
            entries = json.loads(
                (build_root / "compile_commands.json").read_text(encoding="utf-8")
            )
            (build_root / "compile_commands.json").write_text(
                json.dumps([entry for entry in entries if entry["file"] == "source.c"]),
                encoding="utf-8",
            )
            nested = build_root / "generated" / "compile_commands.json"
            nested.parent.mkdir(parents=True)
            nested.write_text(
                json.dumps([entry for entry in entries if entry["file"] == "source.cpp"]),
                encoding="utf-8",
            )
            completed = self.run_evidence(root)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        evidence = json.loads(completed.stdout)
        package_builder.require_compiler_optimization_evidence(
            evidence,
            architecture="arm64",
            label="language-specific nested database evidence",
        )
        self.assertEqual(evidence["compile_database_count"], 2)
        self.assertEqual(
            [
                value["language_compile_commands"]
                for value in evidence["compile_databases"]
            ],
            [{"C": 1, "CXX": 0}, {"C": 0, "CXX": 1}],
        )

    def test_remote_compiler_evidence_ignores_unselected_preserved_cache(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = self.optimization_root(Path(tmp), self.O3_FLAGS)
            self.optimization_root(
                root, [*self.O3_FLAGS, "-O0"], "arch"
            )
            legacy = (
                root
                / "fedora_44"
                / "postgresql17"
                / "build-rooted"
            )
            legacy.mkdir(parents=True)
            (legacy / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "arguments": ["clang++", "-O0", "-c", "legacy.cpp"],
                            "file": "legacy.cpp",
                        }
                    ]
                ),
                encoding="utf-8",
            )
            completed = self.run_evidence(root)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        evidence = json.loads(completed.stdout)
        self.assertEqual(evidence["compile_database_count"], 1)
        self.assertTrue(
            all(
                value["path"].startswith(
                    "fedora_44/postgresql17/build-repository-root/"
                )
                for value in evidence["compile_databases"]
            )
        )

    def test_remote_compiler_evidence_rejects_every_lower_optimization(self) -> None:
        for forbidden in ("-O0", "-O1", "-O2", "-Os", "-Og"):
            with self.subTest(forbidden=forbidden), tempfile.TemporaryDirectory() as tmp:
                completed = self.run_evidence(
                    self.optimization_root(
                        Path(tmp), [*self.O3_FLAGS, forbidden]
                    )
                )
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn(
                    "production compiler command must contain only -O3",
                    completed.stderr,
                )

    def test_remote_compiler_evidence_requires_actual_ndebug(self) -> None:
        for invalid in (
            ["-O3", "-fno-omit-frame-pointer"],
            [*self.O3_FLAGS, "-UNDEBUG"],
        ):
            with self.subTest(flags=invalid), tempfile.TemporaryDirectory() as tmp:
                completed = self.run_evidence(
                    self.optimization_root(Path(tmp), invalid)
                )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("ndebug=", completed.stderr)

    def test_remote_compiler_evidence_rejects_link_only_lower_optimization(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = self.optimization_root(Path(tmp), self.O3_FLAGS)
            build_file = (
                root
                / "fedora_44"
                / "postgresql17"
                / "build-repository-root"
                / "build.ninja"
            )
            contents = build_file.read_text(encoding="utf-8")
            build_file.write_text(
                contents.replace(
                    "-O3 -DNDEBUG -fno-omit-frame-pointer source.c.o",
                    "-O3 -O2 -DNDEBUG -fno-omit-frame-pointer source.c.o",
                    1,
                ),
                encoding="utf-8",
            )
            completed = self.run_evidence(root)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn(":link", completed.stderr)
        self.assertIn("-O2", completed.stderr)

    def test_run_cli_accepts_only_explicit_o3_policy(self) -> None:
        parser = package_builder.build_parser()
        self.assertEqual(
            parser.parse_args(["run", "--optimization", "O3"]).optimization,
            "O3",
        )
        with mock.patch("sys.stderr", new=io.StringIO()), self.assertRaises(
            SystemExit
        ):
            parser.parse_args(["run", "--optimization", "O2"])
        with mock.patch("sys.stderr", new=io.StringIO()), self.assertRaises(
            SystemExit
        ):
            parser.parse_args(["run"])

    def test_controller_preserves_cache_but_forces_flag_reconfiguration(self) -> None:
        builder = package_builder.Builder(
            "arm64",
            "i-0aaabbbcccdddeee1",
            "c7g.xlarge",
            "arm64",
            "203.0.113.1",
            "10.0.0.1",
            "ami-0123456789abcdef0",
        )
        with mock.patch.object(
            package_builder.secrets, "token_hex", return_value="1" * 16
        ), mock.patch.object(package_builder, "ssh_run") as ssh:
            package_builder.force_remote_package_reconfigure(
                builder, Path("/tmp/key")
            )
        command = ssh.call_args.args[2]
        self.assertIn("caches/package-cmake", command)
        self.assertIn(".controller-reconfigure.lock", command)
        self.assertIn("flock -x 9", command)
        self.assertIn("*/build-repository-root/CMakeCache.txt", command)
        self.assertIn("CMakeCache.txt", command)
        self.assertIn(".pre-o3-${token}", command)
        self.assertIn('sudo -- mv -- "$cache" "$backup"', command)

    def test_controller_reconfigure_is_noop_before_first_package_cache(self) -> None:
        builder = package_builder.Builder(
            "x64",
            "i-0aaabbbcccdddeee2",
            "m4.xlarge",
            "x86_64",
            "203.0.113.2",
            "10.0.0.2",
            "ami-0123456789abcdef1",
        )
        with tempfile.TemporaryDirectory() as tmp:
            absent_root = Path(tmp) / "package-cmake"

            def local_ssh(
                _builder: package_builder.Builder,
                _key: Path,
                command: str,
                **_kwargs: object,
            ) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    command,
                    shell=True,
                    executable="/bin/bash",
                    text=True,
                    capture_output=True,
                    check=True,
                )

            with mock.patch.object(
                package_builder,
                "REMOTE_PACKAGE_CMAKE_CACHE_ROOT",
                str(absent_root),
            ), mock.patch.object(package_builder, "ssh_run", side_effect=local_ssh):
                package_builder.force_remote_package_reconfigure(
                    builder, Path("/tmp/key")
                )
            self.assertFalse(absent_root.exists())


class MatrixTests(unittest.TestCase):
    def test_default_matrix_is_the_complete_current_matrix(self) -> None:
        matrix = package_builder.load_matrix()
        self.assertEqual(set(matrix.architectures), {"arm64", "x64"})
        self.assertEqual(
            {key: value.instance_type for key, value in matrix.architectures.items()},
            {"arm64": "c7g.xlarge", "x64": "m4.xlarge"},
        )
        self.assertEqual(
            {key: value.package_format for key, value in matrix.distributions.items()},
            {
                "opensuse_16.0": "rpm",
                "fedora_44": "rpm",
                "debian_13": "deb",
                "ubuntu_26.04": "deb",
                "arch": "pacman",
            },
        )
        self.assertEqual(matrix.postgresql_majors, (17,))

    def test_matrix_rejects_legacy_wrong_revision_and_unknown_keys(self) -> None:
        current = json.loads(package_builder.MATRIX_FILE.read_text(encoding="utf-8"))
        mutations = {
            "legacy-key": lambda value: value.update(
                schema_version=value.pop("schema_revision")),
            "wrong-revision": lambda value: value.update(schema_revision=2),
            "boolean-revision": lambda value: value.update(schema_revision=True),
            "unknown-root": lambda value: value.update(unexpected=True),
            "unknown-row": lambda value: value["architectures"]["arm64"].update(
                unexpected=True),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as raw:
                candidate = json.loads(json.dumps(current))
                mutate(candidate)
                path = Path(raw) / "matrix.json"
                path.write_text(json.dumps(candidate), encoding="utf-8")
                with self.assertRaises(package_builder.PackageBuilderError):
                    package_builder.load_matrix(path)

    def test_package_run_result_rejects_legacy_wrong_and_unknown_identity(self) -> None:
        current = {
            "schema_revision": package_builder.PACKAGE_RUN_SCHEMA_REVISION,
            "status": "success",
            "run_id": "20260810T000000Z-aaaaaaaaaaaa",
            "revision": "a" * 40,
            "source_revision": "a" * 40,
            "poetry_lock_sha256": "b" * 64,
            "worker_generation": "python-poetry",
            "worker_generation_revision": 1,
            "completed_at": "2026-08-10T00:00:00+00:00",
            "selection": {},
            "bucket": "bucket",
            "s3_prefix": "bioseq/packages",
            "artifact_count": 0,
            "python_environments": {},
            "worker_units": {},
            "worker_launch_acknowledged": {},
            "manifests": {},
            "optimization": {
                "policy_revision": package_builder.OPTIMIZATION_POLICY_REVISION,
                "requested": package_builder.PRODUCTION_OPTIMIZATION,
                "architectures": {},
            },
            "builders_left_running": {},
        }
        self.assertEqual(
            package_builder.require_current_package_run_result(
                current, label="test package run result"),
            current,
        )
        mutations = {
            "legacy-key": lambda value: value.update(
                schema_version=value.pop("schema_revision")),
            "wrong-revision": lambda value: value.update(schema_revision=2),
            "boolean-revision": lambda value: value.update(schema_revision=True),
            "legacy-worker": lambda value: value.update(
                worker_generation="python-poetry-v1"),
            "wrong-worker-revision": lambda value: value.update(
                worker_generation_revision=2),
            "boolean-worker-revision": lambda value: value.update(
                worker_generation_revision=True),
            "unknown-root": lambda value: value.update(unexpected=True),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                candidate = json.loads(json.dumps(current))
                mutate(candidate)
                with self.assertRaises(package_builder.PackageBuilderError):
                    package_builder.require_current_package_run_result(
                        candidate, label="test package run result")

    def test_selection_rejects_unknown_or_duplicate_values(self) -> None:
        matrix = package_builder.load_matrix()
        base = {"architectures": None, "distros": None, "postgres": None}
        with self.assertRaisesRegex(package_builder.PackageBuilderError, "unknown distributions"):
            package_builder.select_matrix(
                argparse.Namespace(**{**base, "distros": ["debian_13,unknown"]}),
                matrix,
            )
        with self.assertRaisesRegex(package_builder.PackageBuilderError, "must not be repeated"):
            package_builder.select_matrix(
                argparse.Namespace(**{**base, "architectures": ["arm64,arm64"]}),
                matrix,
            )

    def test_s3_prefix_is_validated_before_it_reaches_opentofu(self) -> None:
        self.assertEqual(
            package_builder.normalize_s3_prefix("bioseq/packages"), "bioseq/packages"
        )
        for invalid in ("", "/bioseq", "bioseq/", "bioseq//packages", "bioseq packages"):
            with self.subTest(invalid=invalid), self.assertRaises(
                package_builder.PackageBuilderError
            ):
                package_builder.normalize_s3_prefix(invalid)


class OpenTofuOnlyTests(unittest.TestCase):
    def test_there_is_no_alternate_cli_fallback(self) -> None:
        with mock.patch.dict(package_builder.os.environ, {}, clear=True), mock.patch.object(
            package_builder.shutil, "which", return_value=None
        ) as which:
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "`tofu`"):
                package_builder.tofu_executable()
        which.assert_called_once_with("tofu")

    def test_configured_binary_must_be_named_tofu(self) -> None:
        with mock.patch.dict(
            package_builder.os.environ,
            {"BIOSEQ_TOFU_BIN": "/usr/bin/not-tofu"},
            clear=True,
        ):
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "basename"):
                package_builder.tofu_executable()

    def test_plan_rejects_every_ec2_instance_delete_or_replacement(self) -> None:
        for actions in (["delete"], ["delete", "create"], ["create", "delete"]):
            with self.subTest(actions=actions), self.assertRaisesRegex(
                package_builder.PackageBuilderError, "delete|replace|destroy"
            ):
                package_builder.validate_tofu_plan(
                    {
                        "resource_changes": [
                            {
                                "address": 'aws_instance.builder["arm64"]',
                                "type": "aws_instance",
                                "change": {"actions": actions},
                            }
                        ]
                    }
                )

    def test_plan_allows_non_destructive_instance_and_non_instance_changes(self) -> None:
        package_builder.validate_tofu_plan(
            {
                "resource_changes": [
                    {
                        "address": 'aws_instance.builder["arm64"]',
                        "type": "aws_instance",
                        "change": {"actions": ["update"]},
                    },
                    {
                        "address": "aws_s3_object.metadata",
                        "type": "aws_s3_object",
                        "change": {"actions": ["delete", "create"]},
                    },
                ]
            }
        )


class InstalledRuntimeContractTests(unittest.TestCase):
    def test_cache_dso_does_not_reserve_the_shared_bioseq_guc_prefix(self) -> None:
        cache_source = (
            package_builder.REPOSITORY_ROOT
            / "bioseq_caching"
            / "pg"
            / "guc.cppm"
        ).read_text(encoding="utf-8")
        self.assertIn('"bioseq_cache.query_cache.mode"', cache_source)
        self.assertIn(
            '"bioseq_cache.query_cache.completed_result_ttl_ms"', cache_source
        )
        self.assertIn('"exact_query"', cache_source)
        self.assertIn('"reusable_fragments"', cache_source)
        self.assertNotIn('MarkGUCPrefixReserved("bioseq")', cache_source)


class LifecycleSafetyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.matrix = package_builder.load_matrix()

    def outputs(self, instance_id: str) -> dict:
        return {
            "builders": {
                "arm64": {
                    "instance_id": instance_id,
                    "instance_type": "c7g.xlarge",
                    "architecture": "arm64",
                    "public_ip": "203.0.113.1",
                    "private_ip": "10.0.0.1",
                    "source_ami_id": "ami-0123456789abcdef0",
                },
                "x64": {
                    "instance_id": "i-0123456789abcdef0",
                    "instance_type": "m4.xlarge",
                    "architecture": "x86_64",
                    "public_ip": "203.0.113.2",
                    "private_ip": "10.0.0.2",
                    "source_ami_id": "ami-0123456789abcdef1",
                },
            }
        }

    def test_protected_instance_is_rejected_before_any_aws_call(self) -> None:
        with mock.patch.object(package_builder, "aws_json") as aws:
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "protected"):
                package_builder.output_builders(
                    self.outputs("i-05b18218430e9fecb"), self.matrix
                )
        aws.assert_not_called()

    def test_poisoned_local_state_is_rejected_before_apply(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(
            package_builder, "STACK_DIR", Path(tmp)
        ):
            (Path(tmp) / "state.tfstate").write_text(
                '{"instance":"i-05d1fa02e86288637"}', encoding="utf-8"
            )
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "protected"):
                package_builder.guard_local_state_against_protected_instances()

    def test_run_leaves_builders_running_after_build_failure(self) -> None:
        builders = package_builder.output_builders(
            self.outputs("i-0aaabbbcccdddeee1"), self.matrix
        )
        args = argparse.Namespace(
            architectures=None,
            distros=None,
            postgres=None,
            region="us-east-1",
            s3_prefix="bioseq/packages",
            no_snapshot=True,
        )
        with mock.patch.object(
            package_builder,
            "provision_and_start",
            return_value=({}, builders, {}, {}),
        ), mock.patch.object(
            package_builder,
            "execute_build",
            side_effect=package_builder.PackageBuilderError("expected failure"),
        ), mock.patch.object(package_builder, "aws_json") as aws_json, mock.patch.object(
            package_builder, "aws_no_output"
        ) as aws_no_output:
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "expected failure"):
                package_builder.cmd_run(args)
        aws_json.assert_not_called()
        aws_no_output.assert_not_called()

    def test_partial_apply_recovers_exact_state_instance_without_stopping_it(self) -> None:
        state = {
            "values": {
                "root_module": {
                    "resources": [
                        {
                            "address": 'aws_instance.builder["x64"]',
                            "values": {
                                "id": "i-0123456789abcdef0",
                                "ami": "ami-0123456789abcdef0",
                                "public_ip": "203.0.113.2",
                                "private_ip": "10.0.0.2",
                            },
                        },
                        {
                            "address": "aws_s3_bucket.artifacts",
                            "values": {"id": "unrelated"},
                        },
                    ]
                }
            }
        }
        completed = subprocess.CompletedProcess([], 0, json.dumps(state), "")
        with mock.patch.object(package_builder, "run_command", return_value=completed):
            recovered = package_builder.tofu_state_builders(self.matrix)
        self.assertEqual(set(recovered), {"x64"})
        self.assertEqual(recovered["x64"].instance_id, "i-0123456789abcdef0")
        self.assertEqual(recovered["x64"].instance_type, "m4.xlarge")

        args = argparse.Namespace(
            architectures=None,
            distros=None,
            postgres=None,
            region="us-east-1",
            s3_prefix="bioseq/packages",
            no_snapshot=True,
        )
        with mock.patch.object(
            package_builder,
            "provision_and_start",
            side_effect=package_builder.PackageBuilderError("partial apply"),
        ), mock.patch.object(
            package_builder, "recover_managed_builders", return_value=recovered
        ), mock.patch.object(package_builder, "aws_json") as aws_json, mock.patch.object(
            package_builder, "aws_no_output"
        ) as aws_no_output:
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "partial apply"):
                package_builder.cmd_run(args)
        aws_json.assert_not_called()
        aws_no_output.assert_not_called()

    def test_state_recovery_failure_is_not_treated_as_no_resources(self) -> None:
        failed = subprocess.CompletedProcess([], 1, "", "provider failure")
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(
            package_builder, "STACK_DIR", Path(tmp)
        ), mock.patch.object(package_builder, "run_command", return_value=failed):
            (Path(tmp) / "partial.tfstate").write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "cannot inspect non-empty"
            ):
                package_builder.tofu_state_builders(self.matrix)

    def test_controller_contains_no_ec2_stop_or_terminate_operation(self) -> None:
        source = MODULE_PATH.read_text(encoding="utf-8")
        self.assertNotIn('"stop-instances"', source)
        self.assertNotIn('"terminate-instances"', source)

    def test_large_cached_ami_has_explicit_long_polling(self) -> None:
        pending = {"Images": [{"ImageId": "ami-0123456789abcdef0", "State": "pending"}]}
        available = {
            "Images": [{"ImageId": "ami-0123456789abcdef0", "State": "available"}]
        }
        with mock.patch.object(
            package_builder, "aws_json", side_effect=[pending, available]
        ), mock.patch.object(package_builder.time, "sleep") as sleep:
            image = package_builder.wait_for_cached_ami(
                "us-east-1", "ami-0123456789abcdef0"
            )
        self.assertEqual(image["State"], "available")
        sleep.assert_called_once_with(15)

    def test_ready_bootstrap_skips_stale_cloud_init_error(self) -> None:
        builder = package_builder.Builder(
            architecture="arm64",
            instance_id="i-0aaabbbcccdddeee1",
            instance_type="c7g.xlarge",
            aws_architecture="arm64",
            public_ip="203.0.113.1",
            private_ip="10.0.0.1",
            source_ami_id="ami-0123456789abcdef0",
        )
        ready = subprocess.CompletedProcess([], 0, "", "")
        verified = subprocess.CompletedProcess([], 0, "", "")
        with mock.patch.object(package_builder, "wait_for_ssh"), mock.patch.object(
            package_builder, "ssh_run", side_effect=[ready, verified]
        ) as ssh:
            package_builder.wait_for_bootstrap(builder, Path("/tmp/key"))
        self.assertEqual(ssh.call_count, 2)
        self.assertNotIn("cloud-init status", ssh.call_args_list[1].args[2])

    def test_bootstrap_reconcile_appends_missing_schema_assignment(self) -> None:
        builder = package_builder.Builder(
            architecture="arm64",
            instance_id="i-0aaabbbcccdddeee1",
            instance_type="c7g.xlarge",
            aws_architecture="arm64",
            public_ip="203.0.113.1",
            private_ip="10.0.0.1",
            source_ami_id="ami-0123456789abcdef0",
        )
        success = subprocess.CompletedProcess([], 0, "", "")
        ready_mismatch = subprocess.CompletedProcess([], 1, "", "")
        with mock.patch.object(
            package_builder, "wait_for_ssh"
        ), mock.patch.object(
            package_builder,
            "ssh_run",
            side_effect=(success, ready_mismatch, success),
        ) as ssh, mock.patch.object(package_builder, "wait_for_bootstrap"):
            package_builder.reconcile_remote_bootstrap(
                builder,
                Path("/tmp/key"),
                "/opt/bioseq-package-builder/workspace/source",
            )
        command = ssh.call_args_list[2].args[2]
        self.assertIn("if grep -q", command)
        self.assertIn("^BIOSEQ_AMI_SCHEMA_REVISION=", command)
        self.assertIn("sudo tee -a /etc/bioseq-package-builder.env", command)
        self.assertIn("grep -Fqx", command)

    def test_external_interrupt_is_forwarded_to_active_child(self) -> None:
        child = mock.Mock()
        child.poll.return_value = None
        with package_builder.ACTIVE_CHILDREN_LOCK:
            package_builder.ACTIVE_CHILDREN.add(child)
        try:
            package_builder.interrupt_active_children()
        finally:
            with package_builder.ACTIVE_CHILDREN_LOCK:
                package_builder.ACTIVE_CHILDREN.discard(child)
        child.send_signal.assert_called_once_with(signal.SIGINT)


class InstanceConnectRetryTests(unittest.TestCase):
    def builder(self) -> package_builder.Builder:
        return package_builder.Builder(
            "x64",
            "i-0aaabbbcccdddeee2",
            "m4.xlarge",
            "x86_64",
            "203.0.113.2",
            "10.0.0.2",
            "ami-0123456789abcdef1",
            availability_zone="us-east-1c",
            region="us-east-1",
            ssh_access="ec2-instance-connect",
        )

    def test_capture_retries_only_instance_connect_public_key_race(self) -> None:
        denied = subprocess.CompletedProcess(
            ["ssh", "true"],
            255,
            "",
            "ubuntu@example: Permission denied (publickey).\n",
        )
        master_success = subprocess.CompletedProcess(
            ["ssh", "-fN"], 0, "", ""
        )
        command_success = subprocess.CompletedProcess(
            ["ssh", "true"], 0, "ready\n", ""
        )
        master_exit = subprocess.CompletedProcess(
            ["ssh", "-O", "exit"], 0, "", ""
        )
        with mock.patch.object(
            package_builder, "refresh_instance_connect_key"
        ) as refresh, mock.patch.object(
            package_builder,
            "ssh_argv",
            return_value=["ssh", "ubuntu@example"],
        ), mock.patch.object(
            package_builder.secrets, "token_hex", return_value="1" * 16
        ), mock.patch.object(
            package_builder,
            "run_command",
            side_effect=[denied, master_success, command_success, master_exit],
        ) as run, mock.patch.object(package_builder.time, "sleep") as sleep:
            completed = package_builder.ssh_run(
                self.builder(), Path("/tmp/key"), "true", capture=True
            )

        self.assertIs(completed, command_success)
        self.assertEqual(refresh.call_count, 2)
        self.assertEqual(run.call_count, 4)
        self.assertEqual(
            sum(call.args[0][-1:] == ["true"] for call in run.call_args_list),
            1,
        )
        command = run.call_args_list[2].args[0]
        self.assertEqual(command[-2:], ["ubuntu@example", "true"])
        self.assertIn("ProxyCommand=/usr/bin/false", command)
        self.assertIn("PreferredAuthentications=none", command)
        self.assertIn("PubkeyAuthentication=no", command)
        self.assertIn("ControlMaster=no", command)
        self.assertEqual(run.call_args_list[3].args[0][-3:-1], ["-O", "exit"])
        sleep.assert_called_once_with(
            package_builder.INSTANCE_CONNECT_AUTH_RETRY_SECONDS
        )

    def test_authentication_race_has_bounded_command_free_retries(self) -> None:
        denied = subprocess.CompletedProcess(
            ["ssh", "-fN"],
            255,
            "",
            "ubuntu@example: Permission denied (publickey).\n",
        )
        master_exit = subprocess.CompletedProcess(
            ["ssh", "-O", "exit"], 255, "", "missing control socket"
        )

        def fake_run(argv, **_kwargs):
            if "-O" in argv:
                return master_exit
            return denied

        with mock.patch.object(
            package_builder, "refresh_instance_connect_key"
        ) as refresh, mock.patch.object(
            package_builder,
            "ssh_argv",
            return_value=["ssh", "ubuntu@example"],
        ), mock.patch.object(
            package_builder.secrets, "token_hex", return_value="6" * 16
        ), mock.patch.object(
            package_builder, "run_command", side_effect=fake_run
        ) as run, mock.patch.object(package_builder.time, "sleep") as sleep:
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError,
                "SSH authentication master failed with exit 255",
            ):
                package_builder.ssh_run(
                    self.builder(), Path("/tmp/key"), "work", capture=True
                )

        attempts = package_builder.INSTANCE_CONNECT_AUTH_ATTEMPTS
        self.assertEqual(refresh.call_count, attempts)
        self.assertEqual(sleep.call_count, attempts - 1)
        self.assertEqual(run.call_count, attempts + 1)
        self.assertFalse(
            any(call.args[0][-1:] == ["work"] for call in run.call_args_list)
        )

    def test_capture_does_not_retry_other_remote_failures(self) -> None:
        with mock.patch.object(
            package_builder, "refresh_instance_connect_key"
        ) as refresh, mock.patch.object(
            package_builder,
            "ssh_argv",
            return_value=["ssh", "ubuntu@example"],
        ), mock.patch.object(
            package_builder.secrets, "token_hex", return_value="2" * 16
        ), mock.patch.object(
            package_builder,
            "run_command",
            side_effect=[
                subprocess.CompletedProcess(["ssh", "-fN"], 0, "", ""),
                package_builder.PackageBuilderError("remote command failed"),
                subprocess.CompletedProcess(["ssh", "-O", "exit"], 0, "", ""),
            ],
        ) as run, mock.patch.object(package_builder.time, "sleep") as sleep:
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "remote command failed"
            ):
                package_builder.ssh_run(
                    self.builder(), Path("/tmp/key"), "false", capture=True
                )

        refresh.assert_called_once()
        self.assertEqual(run.call_count, 3)
        sleep.assert_not_called()

    def test_cleanup_failure_does_not_replace_requested_command_result(self) -> None:
        master_success = subprocess.CompletedProcess(
            ["ssh", "-fN"], 0, "", ""
        )
        command_success = subprocess.CompletedProcess(
            ["ssh", "work"], 0, "done\n", ""
        )
        with mock.patch.object(
            package_builder, "refresh_instance_connect_key"
        ), mock.patch.object(
            package_builder,
            "ssh_argv",
            return_value=["ssh", "ubuntu@example"],
        ), mock.patch.object(
            package_builder.secrets, "token_hex", return_value="4" * 16
        ), mock.patch.object(
            package_builder,
            "run_command",
            side_effect=[
                master_success,
                command_success,
                package_builder.PackageBuilderError("cleanup failed"),
            ],
        ) as run, mock.patch.object(
            package_builder,
            "info",
            side_effect=BrokenPipeError("output closed"),
        ), mock.patch.object(Path, "unlink") as unlink:
            completed = package_builder.ssh_run(
                self.builder(), Path("/tmp/key"), "work", capture=True
            )

        self.assertIs(completed, command_success)
        self.assertEqual(run.call_count, 3)
        unlink.assert_called_once_with(missing_ok=True)

    def test_interrupted_master_spawn_still_arms_teardown(self) -> None:
        master_exit = subprocess.CompletedProcess(
            ["ssh", "-O", "exit"], 0, "", ""
        )
        with mock.patch.object(
            package_builder, "refresh_instance_connect_key"
        ) as refresh, mock.patch.object(
            package_builder,
            "ssh_argv",
            return_value=["ssh", "ubuntu@example"],
        ), mock.patch.object(
            package_builder.secrets, "token_hex", return_value="5" * 16
        ), mock.patch.object(
            package_builder,
            "run_command",
            side_effect=[KeyboardInterrupt(), master_exit],
        ) as run, mock.patch.object(Path, "unlink") as unlink:
            with self.assertRaises(KeyboardInterrupt):
                package_builder.ssh_run(
                    self.builder(), Path("/tmp/key"), "work", capture=True
                )

        refresh.assert_called_once()
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args_list[1].args[0][-3:-1], ["-O", "exit"])
        unlink.assert_called_once_with(missing_ok=True)

    def test_concurrent_calls_use_distinct_fail_closed_control_paths(self) -> None:
        arm64 = package_builder.Builder(
            "arm64",
            "i-0aaabbbcccdddeee1",
            "c7g.xlarge",
            "arm64",
            "203.0.113.1",
            "10.0.0.1",
            "ami-0123456789abcdef0",
            availability_zone="us-east-1c",
            region="us-east-1",
            ssh_access="ec2-instance-connect",
        )
        builders = (arm64, self.builder())
        master_barrier = threading.Barrier(len(builders))
        observed_paths: list[str] = []
        observed_commands: list[list[str]] = []
        failures: list[BaseException] = []
        observation_lock = threading.Lock()

        def fake_ssh_argv(
            builder: package_builder.Builder, _key_path: Path
        ) -> list[str]:
            return ["ssh", f"ubuntu@{builder.architecture}"]

        def fake_run(argv, **_kwargs):
            command = list(argv)
            if "-M" in command:
                control_path = command[command.index("-S") + 1]
                with observation_lock:
                    observed_paths.append(control_path)
                master_barrier.wait(timeout=5)
            elif "-O" not in command:
                with observation_lock:
                    observed_commands.append(command)
            return subprocess.CompletedProcess(command, 0, "ready\n", "")

        def invoke(builder: package_builder.Builder) -> None:
            try:
                package_builder.ssh_run(
                    builder,
                    Path("/tmp/key"),
                    "true",
                    capture=True,
                )
            except BaseException as error:  # pragma: no cover - assertion aid
                with observation_lock:
                    failures.append(error)

        with mock.patch.object(
            package_builder, "refresh_instance_connect_key"
        ), mock.patch.object(
            package_builder, "ssh_argv", side_effect=fake_ssh_argv
        ), mock.patch.object(package_builder, "run_command", side_effect=fake_run):
            threads = [
                threading.Thread(target=invoke, args=(builder,))
                for builder in builders
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=10)

        self.assertFalse(failures)
        self.assertEqual(len(observed_paths), 2)
        self.assertEqual(len(set(observed_paths)), 2)
        self.assertTrue(any("-arm64-" in path for path in observed_paths))
        self.assertTrue(any("-x64-" in path for path in observed_paths))
        self.assertEqual(len(observed_commands), 2)
        for command in observed_commands:
            self.assertIn("ProxyCommand=/usr/bin/false", command)

    def test_capture_never_replays_executed_exit_255_command(self) -> None:
        master_success = subprocess.CompletedProcess(
            ["ssh", "-fN"], 0, "", ""
        )
        executed_failure = subprocess.CompletedProcess(
            ["ssh", "work"],
            255,
            "",
            "remote printed: Permission denied (publickey).\n",
        )
        with mock.patch.object(
            package_builder, "refresh_instance_connect_key"
        ) as refresh, mock.patch.object(
            package_builder,
            "ssh_argv",
            return_value=["ssh", "ubuntu@example"],
        ), mock.patch.object(
            package_builder.secrets, "token_hex", return_value="3" * 16
        ), mock.patch.object(
            package_builder,
            "run_command",
            side_effect=[
                master_success,
                executed_failure,
                subprocess.CompletedProcess(["ssh", "-O", "exit"], 0, "", ""),
            ],
        ) as run, mock.patch.object(package_builder.time, "sleep") as sleep:
            completed = package_builder.ssh_run(
                self.builder(),
                Path("/tmp/key"),
                "work",
                capture=True,
                check=False,
            )

        self.assertIs(completed, executed_failure)
        refresh.assert_called_once()
        self.assertEqual(run.call_count, 3)
        command = run.call_args_list[1].args[0]
        self.assertEqual(command[-2:], ["ubuntu@example", "work"])
        self.assertEqual(
            sum(call.args[0][-1:] == ["work"] for call in run.call_args_list),
            1,
        )
        sleep.assert_not_called()


class PoetryEnvironmentTests(unittest.TestCase):
    def setUp(self) -> None:
        self.builder = package_builder.Builder(
            architecture="arm64",
            instance_id="i-0123456789abcdef0",
            instance_type="c7g.xlarge",
            aws_architecture="arm64",
            public_ip="203.0.113.1",
            private_ip="10.0.0.1",
            source_ami_id="ami-0123456789abcdef0",
        )
        self.revision = "a" * 40
        self.lock_sha256 = "b" * 64

    def environment(self):
        return package_builder.PoetryEnvironment(
            lock_sha256=self.lock_sha256,
            venv_path=f"/opt/bioseq-package-builder/venvs/{self.lock_sha256}",
            python_version="3.12.3",
            poetry_version="1.8.5",
        )

    def test_committed_lock_hash_comes_from_the_requested_git_revision(self) -> None:
        lock_contents = "[[package]]\nname = \"python-liquid\"\n"
        completed = subprocess.CompletedProcess([], 0, lock_contents, "")
        with mock.patch.object(
            package_builder, "run_command", return_value=completed
        ) as run:
            actual = package_builder.committed_poetry_lock_sha256(self.revision)
        self.assertEqual(
            actual, hashlib.sha256(lock_contents.encode("utf-8")).hexdigest()
        )
        command = run.call_args.args[0]
        self.assertEqual(command[:2], ["git", "show"])
        self.assertTrue(command[2].startswith(f"{self.revision}:"))

    def test_poetry_environment_manifest_exposes_only_verified_versions(self) -> None:
        environment = self.environment()
        self.assertEqual(
            environment.manifest_value(),
            {"python": "3.12.3", "poetry": "1.8.5"},
        )

    def test_remote_venv_setup_is_lock_keyed_atomic_and_poetry_bound(self) -> None:
        commands: list[str] = []
        sentinel = json.dumps(
            {
                "lock_sha256": self.lock_sha256,
                "python": "3.12.3",
                "poetry": "1.8.5",
            }
        )

        def fake_ssh(*args, **kwargs):
            commands.append(args[2])
            stdout = sentinel if kwargs.get("capture") else ""
            return subprocess.CompletedProcess([], 0, stdout, "")

        with mock.patch.object(package_builder, "ssh_run", side_effect=fake_ssh):
            environment = package_builder.prepare_remote_poetry_environment(
                self.builder,
                Path("/tmp/key"),
                "/opt/bioseq-package-builder/workspace/source",
                self.revision,
                self.lock_sha256,
            )

        self.assertEqual(environment.lock_sha256, self.lock_sha256)
        self.assertIn(self.lock_sha256, environment.venv_path)
        setup = commands[0]
        for required in (
            self.revision,
            self.lock_sha256,
            "flock 9",
            "python3 -m venv",
            "VIRTUAL_ENV=",
            "POETRY_VIRTUALENVS_CREATE=false",
            "poetry install",
            "--only main --no-root --sync",
            "import liquid",
            "os.fsync",
            "mv --",
        ):
            with self.subTest(required=required):
                self.assertIn(required, setup)

    def test_remote_worker_is_detached_systemd_and_uses_locked_venv(self) -> None:
        environment = self.environment()
        selection = package_builder.Selection(("arm64",), ("arch",), (17,))
        commands: list[str] = []

        def fake_ssh(*args, **kwargs):
            commands.append(args[2])
            return subprocess.CompletedProcess([], 0, "", "")

        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(
            package_builder, "LOG_DIR", Path(tmp)
        ), mock.patch.object(package_builder, "ssh_run", side_effect=fake_ssh):
            handle = package_builder.launch_remote_build(
                builder=self.builder,
                key_path=Path("/tmp/key"),
                remote_source="/opt/bioseq-package-builder/workspace/source",
                revision=self.revision,
                poetry_environment=environment,
                selection=selection,
                region="us-east-1",
                bucket="bucket",
                s3_prefix="bioseq/packages",
                secret_arn=(
                    "arn:aws:secretsmanager:us-east-1:123456789012:secret:key"
                ),
                run_id="run/id",
            )

        command = commands[0]
        self.assertIn("systemd-run", command)
        self.assertIn("--property=Type=exec", command)
        self.assertIn("--property=User=ubuntu", command)
        self.assertIn(
            f"--property=RuntimeMaxSec={package_builder.REMOTE_BUILD_RUNTIME_MAX}",
            command,
        )
        self.assertIn(
            f"--property=TimeoutStopSec={package_builder.REMOTE_BUILD_STOP_TIMEOUT}",
            command,
        )
        self.assertIn("--property=KillMode=control-group", command)
        self.assertEqual(
            package_builder.REMOTE_BUILD_WAIT_TIMEOUT_SECONDS,
            package_builder.REMOTE_BUILD_RUNTIME_MAX_SECONDS + 5 * 60,
        )
        self.assertIn(f"{environment.venv_path}/bin/python", command)
        self.assertIn(f"--source-revision {self.revision}", command)
        self.assertIn(f"--poetry-lock-sha256 {self.lock_sha256}", command)
        self.assertIn(package_builder.REMOTE_PACKAGE_LIFECYCLE_LOCK, command)
        self.assertIn("flock --exclusive --wait 120", command)
        self.assertIn("build_packages.py", command)
        self.assertIn("docker", command)
        self.assertIn(
            "--source-dir /opt/bioseq-package-builder/workspace/source",
            command,
        )
        self.assertNotIn("/workspace/source/main", command)
        self.assertNotIn("tee ", command)
        self.assertTrue(handle.unit_name.endswith(".service"))
        self.assertEqual(handle.builder, self.builder)


class ConcurrencyTests(unittest.TestCase):
    def test_both_architecture_workers_launch_before_either_is_waited(self) -> None:
        matrix = package_builder.load_matrix()
        selection = package_builder.Selection(
            tuple(matrix.architectures),
            tuple(matrix.distributions),
            matrix.postgresql_majors,
        )
        builders = {
            "arm64": package_builder.Builder(
                "arm64",
                "i-0aaabbbcccdddeee1",
                "c7g.xlarge",
                "arm64",
                "203.0.113.1",
                "10.0.0.1",
                "ami-0123456789abcdef0",
            ),
            "x64": package_builder.Builder(
                "x64",
                "i-0123456789abcdef0",
                "m4.xlarge",
                "x86_64",
                "203.0.113.2",
                "10.0.0.2",
                "ami-0123456789abcdef1",
            ),
        }
        revision = "a" * 40
        lock_sha256 = "b" * 64
        environment = package_builder.PoetryEnvironment(
            lock_sha256,
            f"/opt/bioseq-package-builder/venvs/{lock_sha256}",
            "3.12.3",
            "1.8.5",
        )
        launched: list[str] = []
        waited: list[str] = []
        launch_lock = threading.Lock()

        def launch(**kwargs):
            builder = kwargs["builder"]
            with launch_lock:
                launched.append(builder.architecture)
            return package_builder.RemoteBuildHandle(
                builder=builder,
                unit_name=f"bioseq-{builder.architecture}.service",
                remote_log_path=f"/tmp/{builder.architecture}.log",
                manifest_path=f"/tmp/{builder.architecture}.json",
                build_root=f"/tmp/{builder.architecture}",
                local_log_path=Path(f"/tmp/{builder.architecture}.log"),
                worker_argv=(
                    f"{environment.venv_path}/bin/python",
                    f"/source/{builder.architecture}/infra/packaging/"
                    "build_packages.py",
                ),
                launch_acknowledged=True,
            )

        def wait(handle, _key_path):
            with launch_lock:
                self.assertEqual(set(launched), {"arm64", "x64"})
                waited.append(handle.builder.architecture)
            return {
                "schema_revision": package_builder.PACKAGE_MANIFEST_SCHEMA_REVISION,
                "status": "success",
                "compiler_optimization": compiler_optimization_evidence(
                    handle.builder.architecture
                ),
            }

        outputs = {
            "package_bucket_name": "bucket",
            "signing_key_secret_arn": (
                "arn:aws:secretsmanager:us-east-1:123456789012:secret:key"
            ),
            "region": "us-east-1",
        }
        args = argparse.Namespace(region="us-east-1", s3_prefix="bioseq/packages")
        with mock.patch.object(
            package_builder, "commit_id", return_value=revision
        ), mock.patch.object(
            package_builder, "committed_poetry_lock_sha256", return_value=lock_sha256
        ), mock.patch.object(package_builder, "require_committed_paths"), mock.patch.object(
            package_builder, "ssh_key_path", return_value=Path("/tmp/key")
        ), mock.patch.object(
            package_builder,
            "sync_committed_source",
            side_effect=lambda builder, _key, _revision: f"/source/{builder.architecture}",
        ), mock.patch.object(package_builder, "wait_for_ssh"), mock.patch.object(
            package_builder, "reconcile_remote_bootstrap"
        ), mock.patch.object(
            package_builder, "force_remote_package_reconfigure"
        ), mock.patch.object(
            package_builder,
            "prepare_remote_poetry_environment",
            return_value=environment,
        ), mock.patch.object(package_builder, "initialize_signing_key"), mock.patch.object(
            package_builder, "launch_remote_build", side_effect=launch
        ), mock.patch.object(
            package_builder, "wait_remote_build", side_effect=wait
        ), mock.patch.object(
            package_builder, "validate_manifest", return_value=[]
        ), mock.patch.object(package_builder, "verify_s3_uploads"), mock.patch.object(
            package_builder, "write_last_run"
        ):
            result = package_builder.execute_build(
                args, matrix, selection, outputs, builders
            )

        self.assertEqual(result["status"], "success")
        self.assertEqual(result["schema_revision"], 1)
        self.assertEqual(result["worker_generation"], "python-poetry")
        self.assertEqual(result["worker_generation_revision"], 1)
        self.assertEqual(set(launched), {"arm64", "x64"})
        self.assertEqual(set(waited), {"arm64", "x64"})


class RemoteUnitCompletionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.builder = package_builder.Builder(
            "arm64",
            "i-0aaabbbcccdddeee1",
            "c7g.xlarge",
            "arm64",
            "203.0.113.1",
            "10.0.0.1",
            "ami-0123456789abcdef0",
        )
        self.worker_argv = (
            f"{package_builder.REMOTE_POETRY_VENV_ROOT}/{'b' * 64}/bin/python",
            package_builder._remote_committed_source_path("a" * 40)
            + "/infra/packaging/build_packages.py",
            "--architecture",
            "arm64",
        )

    def handle(
        self, directory: str, *, launch_acknowledged: bool
    ) -> package_builder.RemoteBuildHandle:
        return package_builder.RemoteBuildHandle(
            builder=self.builder,
            unit_name="bioseq-package-test-arm64.service",
            remote_log_path="/remote/build.log",
            manifest_path="/remote/manifest.json",
            build_root="/remote/build",
            local_log_path=Path(directory) / "build.log",
            worker_argv=self.worker_argv,
            launch_acknowledged=launch_acknowledged,
        )

    @staticmethod
    def status(
        *,
        load: str,
        active: str,
        sub: str,
        result: str = "success",
        exit_status: str = "0",
    ) -> dict[str, str]:
        return {
            "LoadState": load,
            "ActiveState": active,
            "SubState": sub,
            "Result": result,
            "ExecMainStatus": exit_status,
        }

    def ssh_results(self, manifest: dict) -> list[subprocess.CompletedProcess[str]]:
        return [
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, json.dumps(manifest), ""),
        ]

    def manifest(
        self, *, status: str = "success", errors: list[str] | None = None,
    ) -> dict[str, object]:
        return {
            "schema_revision": package_builder.WORKER_PACKAGE_MANIFEST_SCHEMA_REVISION,
            "package_version": package_builder.EXPECTED_PACKAGE_VERSION,
            "architecture": self.builder.architecture,
            "status": status,
            "started_at": "2026-08-10T00:00:00Z",
            "completed_at": "2026-08-10T00:01:00Z",
            "requested": {"postgres": [17], "distros": ["debian_13"]},
            "s3": {
                "bucket": "bucket",
                "region": "us-east-1",
                "base_prefix": "bioseq/packages",
            },
            "signing": {"fingerprint": package_builder.EXPECTED_SIGNING_FINGERPRINT},
            "source_revision": "a" * 40,
            "poetry_lock_sha256": "b" * 64,
            "python_environment": {"python": "3.12.3", "poetry": "1.8.5"},
            "cells": [],
            "errors": list(errors or []),
        }

    def test_wait_accepts_observed_transient_unit_after_clean_unload(self) -> None:
        manifest = self.manifest()
        statuses = [
            self.status(load="loaded", active="active", sub="running"),
            self.status(load="not-found", active="inactive", sub="dead"),
        ]
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(
            package_builder, "remote_unit_status", side_effect=statuses
        ), mock.patch.object(
            package_builder, "ssh_run", side_effect=self.ssh_results(manifest)
        ), mock.patch.object(
            package_builder, "require_exact_worker_absent"
        ) as absent, mock.patch.object(
            package_builder,
            "attest_remote_compiler_optimization",
            return_value=compiler_optimization_evidence("arm64"),
        ), mock.patch.object(package_builder.time, "sleep"):
            result = package_builder.wait_remote_build(
                self.handle(tmp, launch_acknowledged=True),
                Path("/tmp/key"),
                timeout_seconds=60,
            )

        self.assertEqual(
            result,
            {
                **manifest,
                "schema_revision": package_builder.PACKAGE_MANIFEST_SCHEMA_REVISION,
                "compiler_optimization": compiler_optimization_evidence("arm64"),
            },
        )
        absent.assert_called_once_with(self.builder, Path("/tmp/key"), self.worker_argv)

    def test_manifest_root_rejects_legacy_revision_and_unknown_keys(self) -> None:
        current = {
            **self.manifest(),
            "schema_revision": package_builder.PACKAGE_MANIFEST_SCHEMA_REVISION,
            "compiler_optimization": compiler_optimization_evidence("arm64"),
        }
        mutations = {
            "legacy": lambda value: value.update(
                schema_version=value.pop("schema_revision")),
            "wrong": lambda value: value.update(schema_revision=1),
            "boolean": lambda value: value.update(schema_revision=True),
            "unknown": lambda value: value.update(unexpected=True),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                candidate = json.loads(json.dumps(current))
                mutate(candidate)
                with self.assertRaises(package_builder.PackageBuilderError):
                    package_builder.require_current_package_manifest(
                        candidate, label="test package manifest")

    def test_wait_rejects_not_found_before_any_authoritative_observation(self) -> None:
        manifest = self.manifest()
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(
            package_builder,
            "remote_unit_status",
            return_value=self.status(
                load="not-found", active="inactive", sub="dead"
            ),
        ), mock.patch.object(
            package_builder, "ssh_run", side_effect=self.ssh_results(manifest)
        ), mock.patch.object(
            package_builder, "require_exact_worker_absent"
        ) as absent:
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "missing before"):
                package_builder.wait_remote_build(
                    self.handle(tmp, launch_acknowledged=False),
                    Path("/tmp/key"),
                    timeout_seconds=60,
                )
        absent.assert_not_called()

    def test_wait_rejects_vanished_unit_without_success_manifest(self) -> None:
        manifest = self.manifest(status="failed", errors=["failed"])
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(
            package_builder,
            "remote_unit_status",
            return_value=self.status(
                load="not-found", active="inactive", sub="dead"
            ),
        ), mock.patch.object(
            package_builder, "ssh_run", side_effect=self.ssh_results(manifest)
        ), mock.patch.object(
            package_builder, "require_exact_worker_absent"
        ) as absent:
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "terminal success manifest"
            ):
                package_builder.wait_remote_build(
                    self.handle(tmp, launch_acknowledged=True),
                    Path("/tmp/key"),
                    timeout_seconds=60,
                )
        absent.assert_not_called()

    def test_wait_rejects_vanished_unit_while_exact_worker_remains(self) -> None:
        manifest = self.manifest()
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(
            package_builder,
            "remote_unit_status",
            return_value=self.status(
                load="not-found", active="inactive", sub="dead"
            ),
        ), mock.patch.object(
            package_builder, "ssh_run", side_effect=self.ssh_results(manifest)
        ), mock.patch.object(
            package_builder,
            "require_exact_worker_absent",
            side_effect=package_builder.PackageBuilderError("worker is still running"),
        ):
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "still running"):
                package_builder.wait_remote_build(
                    self.handle(tmp, launch_acknowledged=True),
                    Path("/tmp/key"),
                    timeout_seconds=60,
                )

    def test_exact_worker_probe_reports_full_argv_match_and_fails_closed(self) -> None:
        completed = subprocess.CompletedProcess([], 0, "[421]", "")
        with mock.patch.object(
            package_builder, "ssh_run", return_value=completed
        ) as ssh:
            self.assertEqual(
                package_builder.exact_worker_processes(
                    self.builder, Path("/tmp/key"), self.worker_argv
                ),
                (421,),
            )
        command = ssh.call_args.args[2]
        self.assertIn("sudo python3 -c", command)
        self.assertIn(json.dumps(list(self.worker_argv)), command)

        with mock.patch.object(
            package_builder, "exact_worker_processes", return_value=(421,)
        ):
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "still running"
            ):
                package_builder.require_exact_worker_absent(
                    self.builder, Path("/tmp/key"), self.worker_argv
                )

    def test_terminal_status_rejects_every_ambiguous_or_failed_tuple(self) -> None:
        clean_not_found = self.status(
            load="not-found", active="inactive", sub="dead"
        )
        self.assertTrue(
            package_builder.successful_terminal_unit_status(
                clean_not_found, allow_not_found=True
            )
        )
        self.assertFalse(
            package_builder.successful_terminal_unit_status(
                clean_not_found, allow_not_found=False
            )
        )
        for status in (
            self.status(load="loaded", active="failed", sub="failed"),
            self.status(load="loaded", active="inactive", sub="dead", result="exit-code"),
            self.status(load="loaded", active="inactive", sub="dead", exit_status="1"),
            self.status(load="loaded", active="inactive", sub="exited"),
            self.status(load="masked", active="inactive", sub="dead"),
        ):
            with self.subTest(status=status):
                self.assertFalse(
                    package_builder.successful_terminal_unit_status(
                        status, allow_not_found=True
                    )
                )


class AmiQuiescenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.builder = package_builder.Builder(
            "arm64",
            "i-0aaabbbcccdddeee1",
            "c7g.xlarge",
            "arm64",
            "203.0.113.1",
            "10.0.0.1",
            "ami-0123456789abcdef0",
        )
        self.key_path = Path("/tmp/key")
        self.unit_name = "bioseq-package-run-arm64.service"
        self.worker_argv = (
            f"{package_builder.REMOTE_POETRY_VENV_ROOT}/{'b' * 64}/bin/python",
            "/different/source/infra/packaging/build_packages.py",
            "--architecture",
            "x64",
        )
        self.success = {
            "LoadState": "not-found",
            "ActiveState": "inactive",
            "SubState": "dead",
            "Result": "success",
            "ExecMainStatus": "0",
        }

    def test_different_argv_build_packages_process_is_rejected(self) -> None:
        completed = subprocess.CompletedProcess([], 0, "[731]", "")
        with mock.patch.object(
            package_builder, "ssh_run", return_value=completed
        ) as ssh:
            self.assertEqual(
                package_builder.package_worker_processes(
                    self.builder, self.key_path
                ),
                (731,),
            )
        self.assertIn('== "build_packages.py"', ssh.call_args.args[2])
        self.assertNotIn(json.dumps(list(self.worker_argv)), ssh.call_args.args[2])

        with mock.patch.object(
            package_builder, "package_worker_processes", return_value=(731,)
        ), mock.patch.object(package_builder, "active_package_units") as units:
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "workers are still running"
            ):
                package_builder.require_quiescent_remote_worker(
                    self.builder,
                    self.key_path,
                    self.unit_name,
                    self.worker_argv,
                    allow_not_found=True,
                )
        units.assert_not_called()

    def test_active_package_unit_is_rejected(self) -> None:
        unit_name = "bioseq-package-other-x64.service"
        for state in ("active", "activating", "deactivating"):
            row = {
                "unit": unit_name,
                "load": "loaded",
                "active": state,
                "sub": "running",
            }
            completed = subprocess.CompletedProcess([], 0, json.dumps([row]), "")
            with self.subTest(state=state), mock.patch.object(
                package_builder, "ssh_run", return_value=completed
            ):
                self.assertEqual(
                    package_builder.active_package_units(
                        self.builder, self.key_path
                    ),
                    (unit_name,),
                )

        with mock.patch.object(
            package_builder, "package_worker_processes", return_value=()
        ), mock.patch.object(
            package_builder, "active_package_units", return_value=(unit_name,)
        ), mock.patch.object(package_builder, "running_package_containers") as containers:
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "units are still active"
            ):
                package_builder.require_quiescent_remote_worker(
                    self.builder,
                    self.key_path,
                    self.unit_name,
                    self.worker_argv,
                    allow_not_found=True,
                )
        containers.assert_not_called()

    def test_running_package_container_is_rejected(self) -> None:
        container_id = "d" * 64
        row = {
            "Id": container_id,
            "Name": "/bioseq-arm64-pg17-debian_13-build",
            "State": {"Running": True},
            "Mounts": [
                {
                    "Type": "bind",
                    "Source": (
                        f"{package_builder.REMOTE_ROOT}/run-arm64/"
                        "cells/postgresql17/debian_13"
                    ),
                    "Destination": "/work",
                }
            ],
        }
        completed = subprocess.CompletedProcess([], 0, json.dumps([row]), "")
        with mock.patch.object(package_builder, "ssh_run", return_value=completed):
            self.assertEqual(
                package_builder.running_package_containers(
                    self.builder, self.key_path
                ),
                (f"{container_id}:bioseq-arm64-pg17-debian_13-build",),
            )

        with mock.patch.object(
            package_builder, "package_worker_processes", return_value=()
        ), mock.patch.object(
            package_builder, "active_package_units", return_value=()
        ), mock.patch.object(
            package_builder,
            "running_package_containers",
            return_value=(f"{container_id}:bioseq-arm64-pg17-debian_13-build",),
        ), mock.patch.object(package_builder, "require_successful_remote_unit") as unit:
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "containers are still running"
            ):
                package_builder.require_quiescent_remote_worker(
                    self.builder,
                    self.key_path,
                    self.unit_name,
                    self.worker_argv,
                    allow_not_found=True,
                )
        unit.assert_not_called()

    def test_clean_host_and_completed_unit_are_accepted(self) -> None:
        with mock.patch.object(
            package_builder, "package_worker_processes", return_value=()
        ), mock.patch.object(
            package_builder, "active_package_units", return_value=()
        ), mock.patch.object(
            package_builder, "running_package_containers", return_value=()
        ), mock.patch.object(
            package_builder,
            "require_successful_remote_unit",
            return_value=self.success,
        ) as unit:
            result = package_builder.require_quiescent_remote_worker(
                self.builder,
                self.key_path,
                self.unit_name,
                self.worker_argv,
                allow_not_found=True,
            )
        self.assertEqual(result, self.success)
        unit.assert_called_once_with(
            self.builder,
            self.key_path,
            self.unit_name,
            allow_not_found=True,
        )

    def test_malformed_host_probe_output_is_fail_closed(self) -> None:
        completed = subprocess.CompletedProcess([], 0, "not-json", "")
        for probe in (
            package_builder.package_worker_processes,
            package_builder.active_package_units,
            package_builder.running_package_containers,
        ):
            with self.subTest(probe=probe.__name__), mock.patch.object(
                package_builder, "ssh_run", return_value=completed
            ), self.assertRaisesRegex(package_builder.PackageBuilderError, "invalid JSON"):
                probe(self.builder, self.key_path)

    def test_host_probe_command_failure_is_fail_closed(self) -> None:
        failure = subprocess.CalledProcessError(1, ["ssh"], stderr="probe failed")
        with mock.patch.object(
            package_builder, "ssh_run", side_effect=failure
        ), self.assertRaises(subprocess.CalledProcessError):
            package_builder.package_worker_processes(self.builder, self.key_path)


class CommittedInputEquivalenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.recovered = "a" * 40
        self.current = "b" * 40

    @staticmethod
    def diff_result(paths: tuple[str, ...]) -> subprocess.CompletedProcess[str]:
        output = "".join(f"{path}\0" for path in paths)
        return subprocess.CompletedProcess([], 0, output, "")

    def test_controller_tests_and_docs_are_the_only_allowed_delta(self) -> None:
        allowed = (
            "infra/package_builder.py",
            "infra/tests/test_package_builder.py",
            "DOCS/CURRENT/TECH_SPECS/recovery.md",
        )
        with mock.patch.object(
            package_builder, "run_command", return_value=self.diff_result(allowed)
        ) as run:
            result = package_builder.require_committed_build_input_equivalence(
                self.recovered, self.current
            )
        self.assertEqual(result, tuple(sorted(allowed)))
        self.assertEqual(
            run.call_args.args[0],
            [
                "git",
                "diff",
                "--name-only",
                "--no-renames",
                "-z",
                self.recovered,
                self.current,
                "--",
            ],
        )

    def test_every_packaging_source_matrix_and_infra_delta_is_rejected(self) -> None:
        rejected = (
            "infra/packaging/bioseq_packaging/pipeline.py",
            "infra/packaging/deb/control.j2",
            "bioseq/sql/sequence.cppm",
            "CMakeLists.txt",
            "infra/package_matrix.json",
            "infra/package_build_infra/main.tf",
            "infra/initialize_package_signing_key.py",
        )
        for path in rejected:
            with self.subTest(path=path), mock.patch.object(
                package_builder,
                "run_command",
                return_value=self.diff_result((path,)),
            ), self.assertRaisesRegex(
                package_builder.PackageBuilderError, "build-affecting paths"
            ):
                package_builder.require_committed_build_input_equivalence(
                    self.recovered, self.current
                )

    def test_malformed_committed_diff_is_fail_closed(self) -> None:
        malformed = subprocess.CompletedProcess(
            [], 0, "infra/package_builder.py", ""
        )
        with mock.patch.object(
            package_builder, "run_command", return_value=malformed
        ), self.assertRaisesRegex(package_builder.PackageBuilderError, "malformed"):
            package_builder.require_committed_build_input_equivalence(
                self.recovered, self.current
            )


class ControllerReceiptTests(unittest.TestCase):
    def setUp(self) -> None:
        self.matrix = package_builder.load_matrix()
        self.revision = "a" * 40
        self.lock_sha256 = "b" * 64
        self.run_id = f"20260817T123305Z-{self.revision[:12]}"
        self.owner_token = "owner/repository:package-release:" + self.revision
        self.owner_sha256 = hashlib.sha256(
            self.owner_token.encode("utf-8")
        ).hexdigest()
        self.builders = {
            "arm64": package_builder.Builder(
                "arm64",
                "i-0aaabbbcccdddeee1",
                "c7g.xlarge",
                "arm64",
                "203.0.113.1",
                "10.0.0.1",
                "ami-0123456789abcdef0",
                availability_zone="us-east-1a",
                ssh_access="ec2-instance-connect",
            ),
            "x64": package_builder.Builder(
                "x64",
                "i-0123456789abcdef0",
                "m4.xlarge",
                "x86_64",
                "203.0.113.2",
                "10.0.0.2",
                "ami-0123456789abcdef1",
                availability_zone="us-east-1b",
                ssh_access="ec2-instance-connect",
            ),
        }
        self.records = {
            "arm64": {"State": {"Name": "stopped"}},
            "x64": {"State": {"Name": "running"}},
        }
        self.outputs = {
            "account_id": "123456789012",
            "caller_arn": "arn:aws:iam::123456789012:user/ci",
            "region": "us-east-1",
            "package_bucket_name": "package-bucket",
            "signing_key_secret_arn": (
                "arn:aws:secretsmanager:us-east-1:123456789012:secret:key"
            ),
            "ssh_private_key_path": "/tmp/key",
        }

    def receipt(self, *, active: bool = False) -> dict[str, object]:
        value = package_builder.new_controller_receipt(
            matrix=self.matrix,
            records=self.records,
            builders=self.builders,
            outputs=self.outputs,
            region="us-east-1",
            owner_token=self.owner_token,
            run_id=self.run_id,
            source_revision=self.revision,
            poetry_lock_sha256=self.lock_sha256,
        )
        if active:
            value["state"] = "active"
            for builder in value["builders"].values():
                builder["launch_acknowledged"] = True
                builder["python_environment"] = {
                    "venv_path": (
                        package_builder.REMOTE_POETRY_VENV_ROOT
                        + "/"
                        + self.lock_sha256
                    ),
                    "python": "3.12.3",
                    "poetry": "1.8.5",
                }
            package_builder.require_controller_receipt(
                value, matrix=self.matrix, label="active receipt"
            )
        return value

    def args(self, *, wait_seconds: int = 0) -> argparse.Namespace:
        return argparse.Namespace(
            region="us-east-1",
            existing_only=True,
            ssh_key="/tmp/key",
            access_receipt="/tmp/access.json",
            receipt="/tmp/controller.json",
            source_revision=self.revision,
            owner_token=self.owner_token,
            wait_seconds=wait_seconds,
            optimization="O3",
        )

    def test_controller_preparation_proves_quiescence_before_mutation(self) -> None:
        builder = self.builders["arm64"]
        key_path = Path("/tmp/key")
        remote_source = "/opt/bioseq-package-sources/revision"
        environment = package_builder.PoetryEnvironment(
            self.lock_sha256,
            f"{package_builder.REMOTE_POETRY_VENV_ROOT}/{self.lock_sha256}",
            "3.12.3",
            "1.8.5",
        )
        events: list[str] = []

        with mock.patch.object(
            package_builder,
            "wait_for_ssh",
            side_effect=lambda *_args: events.append("wait"),
        ), mock.patch.object(
            package_builder,
            "remote_package_quiescence_script",
            return_value="quiescence-probe",
        ), mock.patch.object(
            package_builder,
            "ssh_run",
            side_effect=lambda *_args, **_kwargs: events.append("quiescence"),
        ), mock.patch.object(
            package_builder,
            "sync_committed_source",
            side_effect=lambda *_args: events.append("sync") or remote_source,
        ), mock.patch.object(
            package_builder,
            "reconcile_remote_bootstrap",
            side_effect=lambda *_args: events.append("bootstrap"),
        ), mock.patch.object(
            package_builder,
            "force_remote_package_reconfigure",
            side_effect=lambda *_args: events.append("reconfigure"),
        ), mock.patch.object(
            package_builder,
            "prepare_remote_poetry_environment",
            side_effect=lambda *_args: events.append("poetry") or environment,
        ):
            observed_source, observed_environment = (
                package_builder.prepare_controller_builder(
                    builder,
                    key_path,
                    self.revision,
                    self.lock_sha256,
                )
            )

        self.assertEqual(observed_source, remote_source)
        self.assertIs(observed_environment, environment)
        self.assertEqual(
            events,
            ["wait", "quiescence", "sync", "bootstrap", "reconfigure", "poetry"],
        )

    def test_receipt_is_strict_versioned_and_contains_no_private_key(self) -> None:
        receipt = self.receipt()
        self.assertEqual(receipt["schema_revision"], 1)
        self.assertEqual(receipt["state"], "prepared")
        self.assertEqual(
            receipt["store_key"],
            f"bioseq/packages/controllers/v1/{self.revision}.json",
        )
        self.assertTrue(receipt["builders"]["arm64"]["stop_owned"])
        self.assertFalse(receipt["builders"]["x64"]["stop_owned"])
        encoded = json.dumps(receipt)
        self.assertNotIn("PRIVATE", encoded)
        self.assertNotIn("ssh_private_key_path", encoded)
        tampered = {**receipt, "unexpected": True}
        with self.assertRaisesRegex(
            package_builder.PackageBuilderError, "unexpected keys"
        ):
            package_builder.require_controller_receipt(
                tampered, matrix=self.matrix, label="tampered receipt"
            )

    def test_launched_receipt_requires_exact_python_provenance(self) -> None:
        receipt = self.receipt(active=True)
        receipt["builders"]["x64"]["python_environment"] = None
        with self.assertRaisesRegex(
            package_builder.PackageBuilderError, "lacks Python provenance"
        ):
            package_builder.require_controller_receipt(
                receipt, matrix=self.matrix, label="tampered active receipt"
            )

    def test_exact_existing_active_receipt_never_relaunches(self) -> None:
        receipt = self.receipt(active=True)
        context = (
            self.outputs,
            self.builders,
            self.records,
            Path("/tmp/key"),
            Path("/tmp/controller.json"),
            self.owner_sha256,
        )
        with mock.patch.object(
            package_builder, "controller_context", return_value=context
        ), mock.patch.object(
            package_builder, "commit_id", return_value=self.revision
        ), mock.patch.object(
            package_builder, "_require_controller_launch_inputs"
        ), mock.patch.object(
            package_builder,
            "committed_poetry_lock_sha256",
            return_value=self.lock_sha256,
        ), mock.patch.object(
            package_builder, "fetch_controller_receipt", return_value=receipt
        ), mock.patch.object(
            package_builder, "start_builders"
        ) as start, mock.patch.object(
            package_builder, "launch_remote_build"
        ) as launch:
            observed = package_builder.launch_controller_run(self.args())
        self.assertIs(observed, receipt)
        start.assert_not_called()
        launch.assert_not_called()

    def test_prepared_retry_observes_launched_units_before_any_reconfigure(self) -> None:
        receipt = self.receipt()
        for builder in receipt["builders"].values():
            builder["python_environment"] = {
                "venv_path": (
                    package_builder.REMOTE_POETRY_VENV_ROOT
                    + "/"
                    + self.lock_sha256
                ),
                "python": "3.12.3",
                "poetry": "1.8.5",
            }
        context = (
            self.outputs,
            self.builders,
            self.records,
            Path("/tmp/key"),
            Path("/tmp/controller.json"),
            self.owner_sha256,
        )
        status = {
            "LoadState": "loaded",
            "ActiveState": "active",
            "SubState": "running",
            "Result": "success",
            "ExecMainStatus": "0",
        }
        with mock.patch.object(
            package_builder, "controller_context", return_value=context
        ), mock.patch.object(
            package_builder, "commit_id", return_value=self.revision
        ), mock.patch.object(
            package_builder, "_require_controller_launch_inputs"
        ), mock.patch.object(
            package_builder,
            "committed_poetry_lock_sha256",
            return_value=self.lock_sha256,
        ), mock.patch.object(
            package_builder, "fetch_controller_receipt", return_value=receipt
        ), mock.patch.object(
            package_builder, "start_builders", return_value=self.records
        ), mock.patch.object(
            package_builder, "refreshed_builders", return_value=self.builders
        ), mock.patch.object(
            package_builder, "wait_for_ssh"
        ), mock.patch.object(
            package_builder, "remote_unit_status", return_value=status
        ), mock.patch.object(
            package_builder, "exact_worker_processes", return_value=(123,)
        ), mock.patch.object(
            package_builder,
            "active_package_units",
            side_effect=lambda builder, _key: (
                receipt["builders"][builder.architecture]["unit_name"],
            ),
        ), mock.patch.object(
            package_builder,
            "persist_controller_receipt",
            side_effect=lambda _path, value, **_kwargs: value,
        ), mock.patch.object(
            package_builder, "force_remote_package_reconfigure"
        ) as reconfigure, mock.patch.object(
            package_builder, "launch_remote_build"
        ) as launch:
            observed = package_builder.launch_controller_run(self.args())
        self.assertEqual(observed["state"], "active")
        self.assertTrue(all(
            value["launch_acknowledged"]
            for value in observed["builders"].values()
        ))
        reconfigure.assert_not_called()
        launch.assert_not_called()

    def test_active_attach_is_explicitly_nonterminal_and_never_launches(self) -> None:
        receipt = self.receipt(active=True)
        context = (
            self.outputs,
            self.builders,
            self.records,
            Path("/tmp/key"),
            Path("/tmp/controller.json"),
            self.owner_sha256,
        )
        status = {
            "LoadState": "loaded",
            "ActiveState": "active",
            "SubState": "running",
            "Result": "success",
            "ExecMainStatus": "0",
        }
        with mock.patch.object(
            package_builder, "controller_context", return_value=context
        ), mock.patch.object(
            package_builder,
            "require_controller_source_equivalence",
            return_value=self.lock_sha256,
        ), mock.patch.object(
            package_builder, "fetch_controller_receipt", return_value=receipt
        ), mock.patch.object(
            package_builder, "wait_for_ssh"
        ), mock.patch.object(
            package_builder, "remote_unit_status", return_value=status
        ), mock.patch.object(
            package_builder, "exact_worker_processes", return_value=(123,)
        ), mock.patch.object(
            package_builder,
            "active_package_units",
            side_effect=lambda builder, _key: (
                receipt["builders"][builder.architecture]["unit_name"],
            ),
        ), mock.patch.object(
            package_builder,
            "persist_controller_receipt",
            side_effect=lambda _path, value, **_kwargs: value,
        ), mock.patch.object(
            package_builder, "fetch_completed_manifest"
        ) as fetch, mock.patch.object(
            package_builder, "launch_remote_build"
        ) as launch:
            observed = package_builder.attach_controller_run(self.args())
        self.assertEqual(observed["state"], "active")
        fetch.assert_not_called()
        launch.assert_not_called()

    def test_failed_attach_persists_concise_terminal_diagnostics(self) -> None:
        receipt = self.receipt(active=True)
        context = (
            self.outputs,
            self.builders,
            self.records,
            Path("/tmp/key"),
            Path("/tmp/controller.json"),
            self.owner_sha256,
        )
        status = {
            "LoadState": "loaded",
            "ActiveState": "failed",
            "SubState": "failed",
            "Result": "exit-code",
            "ExecMainStatus": "124",
        }
        persisted: list[dict[str, object]] = []
        with mock.patch.object(
            package_builder, "controller_context", return_value=context
        ), mock.patch.object(
            package_builder,
            "require_controller_source_equivalence",
            return_value=self.lock_sha256,
        ), mock.patch.object(
            package_builder, "fetch_controller_receipt", return_value=receipt
        ), mock.patch.object(
            package_builder, "wait_for_ssh"
        ), mock.patch.object(
            package_builder, "remote_unit_status", return_value=status
        ), mock.patch.object(
            package_builder,
            "_controller_failure_diagnostic",
            side_effect=lambda builder, *_args: f"{builder.architecture}: failed",
        ), mock.patch.object(
            package_builder,
            "persist_controller_receipt",
            side_effect=lambda _path, value, **_kwargs: persisted.append(value.copy())
            or value,
        ):
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "controller workers failed"
            ):
                package_builder.attach_controller_run(self.args())
        self.assertEqual(receipt["state"], "failed")
        self.assertEqual(len(receipt["diagnostics"]), 2)
        self.assertTrue(persisted)

    def test_completed_attach_writes_the_normal_aggregate_contract(self) -> None:
        receipt = self.receipt(active=True)
        context = (
            self.outputs,
            self.builders,
            self.records,
            Path("/tmp/key"),
            Path("/tmp/controller.json"),
            self.owner_sha256,
        )
        status = {
            "LoadState": "loaded",
            "ActiveState": "inactive",
            "SubState": "dead",
            "Result": "success",
            "ExecMainStatus": "0",
        }
        aggregate = {"status": "success", "run_id": self.run_id}
        with mock.patch.object(
            package_builder, "controller_context", return_value=context
        ), mock.patch.object(
            package_builder,
            "require_controller_source_equivalence",
            return_value=self.lock_sha256,
        ), mock.patch.object(
            package_builder, "fetch_controller_receipt", return_value=receipt
        ), mock.patch.object(
            package_builder, "wait_for_ssh"
        ), mock.patch.object(
            package_builder, "remote_unit_status", return_value=status
        ), mock.patch.object(
            package_builder, "require_exact_worker_absent"
        ), mock.patch.object(
            package_builder, "fetch_completed_manifest", return_value={"ok": True}
        ), mock.patch.object(
            package_builder,
            "_controller_completed_result",
            return_value=aggregate,
        ), mock.patch.object(
            package_builder,
            "persist_controller_receipt",
            side_effect=lambda _path, value, **_kwargs: value,
        ), mock.patch.object(package_builder, "write_last_run") as write:
            observed = package_builder.attach_controller_run(self.args())
        self.assertEqual(observed["state"], "completed")
        self.assertIs(observed["result"], aggregate)
        write.assert_called_once_with(aggregate)

    def test_controller_cleanup_stops_only_collected_owned_builder(self) -> None:
        records = {
            name: {"State": {"Name": "running"}}
            for name in self.matrix.architectures
        }
        final_records = {
            "arm64": {"State": {"Name": "stopped"}},
            "x64": {"State": {"Name": "running"}},
        }
        controller = {
            "state": "collected",
            "builders": {
                "arm64": {"stop_owned": True},
                "x64": {"stop_owned": False},
            },
        }
        calls: list[tuple[str, ...]] = []

        def ssm(_region, instance_ids, **_kwargs):
            calls.append(tuple(instance_ids))
            return "command-id"

        with mock.patch.object(
            package_builder,
            "aws_preflight",
            return_value={"Account": "123456789012", "Arn": "arn:ci"},
        ), mock.patch.object(
            package_builder,
            "discover_existing_builders",
            return_value=(self.builders, records),
        ), mock.patch.object(
            package_builder,
            "validate_existing_artifact_bucket",
            return_value="package-bucket",
        ), mock.patch.object(
            package_builder, "fetch_controller_receipt", return_value=controller
        ), mock.patch.object(
            package_builder, "require_controller_receipt", return_value=controller
        ), mock.patch.object(
            package_builder, "wait_for_ssm_online"
        ), mock.patch.object(
            package_builder, "run_ssm_shell_command", side_effect=ssm
        ), mock.patch.object(
            package_builder, "wait_for_ec2_state"
        ), mock.patch.object(
            package_builder, "validate_builders", return_value=final_records
        ):
            states = package_builder.stop_existing_builders(
                "us-east-1",
                self.matrix,
                controller_receipt=Path("/tmp/controller.json"),
                controller_source_revision=self.revision,
                controller_owner_token_sha256=self.owner_sha256,
            )
        self.assertEqual(states, {"arm64": "stopped", "x64": "running"})
        self.assertEqual(
            calls,
            [
                tuple(builder.instance_id for builder in self.builders.values()),
                (self.builders["arm64"].instance_id,),
            ],
        )

    def test_controller_cleanup_refuses_before_verified_collection(self) -> None:
        controller = {
            "state": "finalized",
            "builders": {
                name: {"stop_owned": True} for name in self.matrix.architectures
            },
        }
        with mock.patch.object(
            package_builder,
            "aws_preflight",
            return_value={"Account": "123456789012", "Arn": "arn:ci"},
        ), mock.patch.object(
            package_builder,
            "discover_existing_builders",
            return_value=(self.builders, self.records),
        ), mock.patch.object(
            package_builder,
            "validate_existing_artifact_bucket",
            return_value="package-bucket",
        ), mock.patch.object(
            package_builder, "fetch_controller_receipt", return_value=controller
        ), mock.patch.object(
            package_builder, "require_controller_receipt", return_value=controller
        ), mock.patch.object(
            package_builder, "run_ssm_shell_command"
        ) as ssm, self.assertRaisesRegex(
            package_builder.PackageBuilderError, "verified collection"
        ):
            package_builder.stop_existing_builders(
                "us-east-1",
                self.matrix,
                controller_receipt=Path("/tmp/controller.json"),
                controller_source_revision=self.revision,
                controller_owner_token_sha256=self.owner_sha256,
            )
        ssm.assert_not_called()

    @staticmethod
    def cleanup_remote_result(
        receipt: dict[str, object], architecture: str, *, active: bool
    ) -> dict[str, object]:
        builder = receipt["builders"][architecture]
        return {
            "unit_name": builder["unit_name"],
            "build_root": builder["build_root"],
            "unit_before": {
                "LoadState": "loaded",
                "ActiveState": "active" if active else "inactive",
                "SubState": "running" if active else "dead",
                "Result": "success",
                "ExecMainStatus": "0",
            },
            "unit_after": {
                "LoadState": "loaded",
                "ActiveState": "inactive",
                "SubState": "dead",
                "Result": "success",
                "ExecMainStatus": "0",
            },
            "unit_cancelled": active,
            "worker_roots": [4100] if active else [],
            "worker_processes": [4100, 4101] if active else [],
            "containers": ["a" * 64] if active else [],
            "log": {"present": True, "size": 8123, "sha256": None},
            "manifest": {
                "present": not active,
                "size": 491 if not active else 0,
                "sha256": "c" * 64 if not active else None,
            },
            "build_root_preserved": True,
        }

    def test_workflow_cleanup_cancels_prepared_or_active_receipts_exactly(self) -> None:
        for state in ("prepared", "active"):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as tmp:
                receipt = self.receipt(active=state == "active")
                current_records = {
                    name: {"State": {"Name": "running"}}
                    for name in self.matrix.architectures
                }
                args = argparse.Namespace(
                    region="us-east-1",
                    receipt=str(Path(tmp) / "controller.json"),
                    evidence=str(Path(tmp) / "cleanup.json"),
                    source_revision=self.revision,
                    owner_token=self.owner_token,
                    if_present=False,
                )

                def detailed(_region, instance_ids, **_kwargs):
                    architecture = next(
                        name
                        for name, builder in self.builders.items()
                        if builder.instance_id == instance_ids[0]
                    )
                    result = self.cleanup_remote_result(
                        receipt, architecture, active=True
                    )
                    return (
                        f"cleanup-{architecture}",
                        [
                            {
                                "InstanceId": instance_ids[0],
                                "Status": "Success",
                                "CommandPlugins": [
                                    {"Output": json.dumps(result) + "\n"}
                                ],
                            }
                        ],
                    )

                with mock.patch.object(
                    package_builder,
                    "discover_existing_infrastructure",
                    return_value=(self.outputs, self.builders, current_records),
                ), mock.patch.object(
                    package_builder,
                    "fetch_controller_receipt",
                    return_value=receipt,
                ), mock.patch.object(
                    package_builder, "wait_for_ssm_online"
                ), mock.patch.object(
                    package_builder,
                    "run_ssm_shell_command_detailed",
                    side_effect=detailed,
                ) as cleanup_ssm, mock.patch.object(
                    package_builder,
                    "run_ssm_shell_command",
                    return_value="poweroff-arm64",
                ) as poweroff, mock.patch.object(
                    package_builder, "wait_for_ec2_state"
                ), mock.patch.object(
                    package_builder,
                    "persist_controller_receipt",
                    side_effect=lambda _path, value, **_kwargs: value,
                ) as persist:
                    evidence = package_builder.cleanup_controller_run(args)

                self.assertEqual(receipt["state"], "failed")
                self.assertEqual(evidence["receipt_state_before"], state)
                self.assertEqual(evidence["receipt_state_after"], "failed")
                self.assertEqual(cleanup_ssm.call_count, 2)
                persist.assert_called_once()
                poweroff.assert_called_once()
                self.assertEqual(
                    poweroff.call_args.args[1],
                    [self.builders["arm64"].instance_id],
                )
                self.assertEqual(
                    evidence["builders"]["arm64"]["instance_state_after"],
                    "stopped",
                )
                self.assertEqual(
                    evidence["builders"]["x64"]["instance_state_after"],
                    "running",
                )
                for call in cleanup_ssm.call_args_list:
                    script = call.kwargs["script"]
                    instance_id = call.args[1][0]
                    architecture = next(
                        name
                        for name, builder in self.builders.items()
                        if builder.instance_id == instance_id
                    )
                    self.assertIn(
                        receipt["builders"][architecture]["build_root"], script
                    )
                    self.assertNotIn("docker rm", script)
                persisted_evidence = json.loads(
                    Path(args.evidence).read_text(encoding="utf-8")
                )
                self.assertTrue(all(
                    value["remote"]["build_root_preserved"]
                    for value in persisted_evidence["builders"].values()
                ))

    def test_workflow_cleanup_retains_finalized_result_and_is_idempotent(self) -> None:
        receipt = self.receipt(active=True)
        receipt["state"] = "finalized"
        receipt["result"] = {"preserved": True}
        current_records = {
            name: {"State": {"Name": "running"}}
            for name in self.matrix.architectures
        }
        with tempfile.TemporaryDirectory() as tmp:
            args = argparse.Namespace(
                region="us-east-1",
                receipt=str(Path(tmp) / "controller.json"),
                evidence=str(Path(tmp) / "cleanup.json"),
                source_revision=self.revision,
                owner_token=self.owner_token,
                if_present=False,
            )

            def detailed(_region, instance_ids, **_kwargs):
                architecture = next(
                    name
                    for name, builder in self.builders.items()
                    if builder.instance_id == instance_ids[0]
                )
                result = self.cleanup_remote_result(
                    receipt, architecture, active=False
                )
                return (
                    f"cleanup-{architecture}",
                    [
                        {
                            "InstanceId": instance_ids[0],
                            "Status": "Success",
                            "CommandPlugins": [
                                {"Output": json.dumps(result) + "\n"}
                            ],
                        }
                    ],
                )

            with mock.patch.object(
                package_builder,
                "discover_existing_infrastructure",
                return_value=(self.outputs, self.builders, current_records),
            ), mock.patch.object(
                package_builder, "fetch_controller_receipt", return_value=receipt
            ), mock.patch.object(
                package_builder, "require_controller_receipt", return_value=receipt
            ), mock.patch.object(
                package_builder, "wait_for_ssm_online"
            ), mock.patch.object(
                package_builder,
                "run_ssm_shell_command_detailed",
                side_effect=detailed,
            ), mock.patch.object(
                package_builder,
                "run_ssm_shell_command",
                return_value="poweroff-arm64",
            ), mock.patch.object(
                package_builder, "wait_for_ec2_state"
            ), mock.patch.object(
                package_builder, "persist_controller_receipt"
            ) as persist:
                evidence = package_builder.cleanup_controller_run(args)

        self.assertEqual(evidence["receipt_state_before"], "finalized")
        self.assertEqual(evidence["receipt_state_after"], "finalized")
        self.assertEqual(receipt["result"], {"preserved": True})
        persist.assert_not_called()

    def test_workflow_cleanup_if_present_is_a_receipt_absence_noop(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            args = argparse.Namespace(
                region="us-east-1",
                receipt=str(Path(tmp) / "controller.json"),
                evidence=str(Path(tmp) / "cleanup.json"),
                source_revision=self.revision,
                owner_token=self.owner_token,
                if_present=True,
            )
            with mock.patch.object(
                package_builder,
                "discover_existing_infrastructure",
                return_value=(self.outputs, self.builders, self.records),
            ), mock.patch.object(
                package_builder, "fetch_controller_receipt", return_value=None
            ), mock.patch.object(
                package_builder, "run_ssm_shell_command_detailed"
            ) as ssm:
                evidence = package_builder.cleanup_controller_run(args)
        self.assertFalse(evidence["receipt_present"])
        ssm.assert_not_called()


class FinalizeRunTests(unittest.TestCase):
    def setUp(self) -> None:
        self.matrix = package_builder.load_matrix()
        self.revision = "a" * 40
        self.finalizer_revision = "c" * 40
        self.lock_sha256 = "b" * 64
        self.run_id = f"20260730T012519Z-{self.revision[:12]}"
        self.args = argparse.Namespace(
            run_id=self.run_id,
            region="us-east-1",
            s3_prefix="bioseq/packages",
        )
        self.builders = {
            "arm64": package_builder.Builder(
                "arm64",
                "i-0aaabbbcccdddeee1",
                "c7g.xlarge",
                "arm64",
                "203.0.113.1",
                "10.0.0.1",
                "ami-0123456789abcdef0",
            ),
            "x64": package_builder.Builder(
                "x64",
                "i-0123456789abcdef0",
                "m4.xlarge",
                "x86_64",
                "203.0.113.2",
                "10.0.0.2",
                "ami-0123456789abcdef1",
            ),
        }
        self.records = {
            name: {
                "State": {"Name": "running"},
                "PublicIpAddress": builder.public_ip,
                "PrivateIpAddress": builder.private_ip,
            }
            for name, builder in self.builders.items()
        }
        self.outputs = {
            "package_bucket_name": "bucket",
            "signing_key_secret_arn": (
                "arn:aws:secretsmanager:us-east-1:123456789012:secret:key"
            ),
            "ssh_private_key_path": "/tmp/key",
            "region": "us-east-1",
        }
        self.manifests = {
            name: {
                "schema_revision": package_builder.PACKAGE_MANIFEST_SCHEMA_REVISION,
                "package_version": package_builder.EXPECTED_PACKAGE_VERSION,
                "status": "success",
                "architecture": name,
                "compiler_optimization": compiler_optimization_evidence(name),
                "started_at": "2026-08-10T00:00:00Z",
                "completed_at": "2026-08-10T00:01:00Z",
                "requested": {
                    "postgres": list(self.matrix.postgresql_majors),
                    "distros": list(self.matrix.distributions),
                },
                "s3": {
                    "bucket": "bucket", "region": "us-east-1",
                    "base_prefix": "bioseq/packages",
                },
                "signing": {
                    "fingerprint": package_builder.EXPECTED_SIGNING_FINGERPRINT},
                "errors": [],
                "source_revision": self.revision,
                "poetry_lock_sha256": self.lock_sha256,
                "python_environment": {"python": "3.12.3", "poetry": "1.8.5"},
                "cells": [],
            }
            for name in self.matrix.architectures
        }

    def test_parser_exposes_strict_finalize_run_interface(self) -> None:
        parsed = package_builder.build_parser().parse_args(
            ["finalize-run", "--run-id", self.run_id]
        )
        self.assertEqual(parsed.handler, package_builder.cmd_finalize_run)
        self.assertEqual(parsed.run_id, self.run_id)
        with self.assertRaisesRegex(
            package_builder.PackageBuilderError, "--run-id"
        ):
            package_builder.finalize_completed_run(
                argparse.Namespace(
                    run_id="latest", region="us-east-1", s3_prefix="bioseq/packages"
                ),
                self.matrix,
                self.outputs,
                self.builders,
            )

    def test_finalize_revalidates_and_writes_only_after_both_images(self) -> None:
        events: list[str] = []
        uploads_per_architecture = [object()] * len(self.matrix.distributions)

        def fetch(builder, _key_path, _run_id):
            return self.manifests[builder.architecture]

        def image(*call_args, **call_kwargs):
            architecture = call_args[2].name
            self.assertTrue(call_kwargs["allow_not_found"])
            events.append(f"image-{architecture}")
            return {"ImageId": f"ami-{architecture}"}

        def write(payload):
            self.assertEqual(set(payload["cache_images"]), {"arm64", "x64"})
            events.append("write")

        with mock.patch.object(
            package_builder, "validate_builders", return_value=self.records
        ), mock.patch.object(
            package_builder, "ssh_key_path", return_value=Path("/tmp/key")
        ), mock.patch.object(package_builder, "wait_for_ssh"), mock.patch.object(
            package_builder, "fetch_completed_manifest", side_effect=fetch
        ), mock.patch.object(
            package_builder, "commit_id", return_value=self.finalizer_revision
        ), mock.patch.object(
            package_builder, "require_committed_build_input_equivalence"
        ) as equivalent, mock.patch.object(
            package_builder,
            "committed_poetry_lock_sha256",
            return_value=self.lock_sha256,
        ), mock.patch.object(
            package_builder,
            "validate_manifest",
            return_value=uploads_per_architecture,
        ) as validate, mock.patch.object(
            package_builder, "verify_s3_uploads"
        ) as verify, mock.patch.object(
            package_builder, "require_quiescent_remote_worker"
        ) as quiescent, mock.patch.object(
            package_builder, "create_cached_ami", side_effect=image
        ) as create, mock.patch.object(
            package_builder, "write_last_run", side_effect=write
        ) as write_last:
            result = package_builder.finalize_completed_run(
                self.args, self.matrix, self.outputs, self.builders
            )

        self.assertEqual(result["status"], "success")
        self.assertEqual(result["schema_revision"], 1)
        self.assertEqual(result["worker_generation"], "python-poetry")
        self.assertEqual(result["worker_generation_revision"], 1)
        self.assertEqual(result["finalizer_revision"], self.finalizer_revision)
        equivalent.assert_called_once_with(self.revision, self.finalizer_revision)
        self.assertTrue(result["finalized_from_completed_manifests"])
        self.assertEqual(validate.call_count, 2)
        verify.assert_called_once()
        self.assertEqual(quiescent.call_count, 2)
        self.assertEqual(create.call_count, 2)
        self.assertEqual(set(events[:-1]), {"image-arm64", "image-x64"})
        self.assertEqual(events[-1], "write")
        write_last.assert_called_once_with(result)

    def test_finalize_image_failure_never_writes_last_run(self) -> None:
        uploads_per_architecture = [object()] * len(self.matrix.distributions)

        def fetch(builder, _key_path, _run_id):
            return self.manifests[builder.architecture]

        def image(*call_args, **_call_kwargs):
            architecture = call_args[2].name
            if architecture == "x64":
                raise package_builder.PackageBuilderError("image failed")
            return {"ImageId": "ami-arm64"}

        with mock.patch.object(
            package_builder, "validate_builders", return_value=self.records
        ), mock.patch.object(
            package_builder, "ssh_key_path", return_value=Path("/tmp/key")
        ), mock.patch.object(package_builder, "wait_for_ssh"), mock.patch.object(
            package_builder, "fetch_completed_manifest", side_effect=fetch
        ), mock.patch.object(
            package_builder, "commit_id", return_value=self.finalizer_revision
        ), mock.patch.object(
            package_builder, "require_committed_build_input_equivalence"
        ), mock.patch.object(
            package_builder,
            "committed_poetry_lock_sha256",
            return_value=self.lock_sha256,
        ), mock.patch.object(
            package_builder,
            "validate_manifest",
            return_value=uploads_per_architecture,
        ), mock.patch.object(package_builder, "verify_s3_uploads"), mock.patch.object(
            package_builder, "require_quiescent_remote_worker"
        ), mock.patch.object(
            package_builder, "create_cached_ami", side_effect=image
        ), mock.patch.object(package_builder, "write_last_run") as write_last:
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "image failed"):
                package_builder.finalize_completed_run(
                    self.args, self.matrix, self.outputs, self.builders
                )
        write_last.assert_not_called()

    def test_finalize_requires_both_workers_quiescent_before_any_image(self) -> None:
        uploads_per_architecture = [object()] * len(self.matrix.distributions)

        def fetch(builder, _key_path, _run_id):
            return self.manifests[builder.architecture]

        def quiescent(builder, *_args, **_kwargs):
            if builder.architecture == "x64":
                raise package_builder.PackageBuilderError("worker still running")

        with mock.patch.object(
            package_builder, "validate_builders", return_value=self.records
        ), mock.patch.object(
            package_builder, "ssh_key_path", return_value=Path("/tmp/key")
        ), mock.patch.object(package_builder, "wait_for_ssh"), mock.patch.object(
            package_builder, "fetch_completed_manifest", side_effect=fetch
        ), mock.patch.object(
            package_builder, "commit_id", return_value=self.finalizer_revision
        ), mock.patch.object(
            package_builder, "require_committed_build_input_equivalence"
        ), mock.patch.object(
            package_builder,
            "committed_poetry_lock_sha256",
            return_value=self.lock_sha256,
        ), mock.patch.object(
            package_builder,
            "validate_manifest",
            return_value=uploads_per_architecture,
        ), mock.patch.object(package_builder, "verify_s3_uploads"), mock.patch.object(
            package_builder,
            "require_quiescent_remote_worker",
            side_effect=quiescent,
        ), mock.patch.object(package_builder, "create_cached_ami") as create, mock.patch.object(
            package_builder, "write_last_run"
        ) as write_last:
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "jointly quiescent"
            ):
                package_builder.finalize_completed_run(
                    self.args, self.matrix, self.outputs, self.builders
                )
        create.assert_not_called()
        write_last.assert_not_called()

    def test_finalize_rejects_non_running_builder_before_manifest_fetch(self) -> None:
        records = {
            **self.records,
            "x64": {**self.records["x64"], "State": {"Name": "stopped"}},
        }
        with mock.patch.object(
            package_builder, "validate_builders", return_value=records
        ), mock.patch.object(package_builder, "fetch_completed_manifest") as fetch:
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "already running"):
                package_builder.finalize_completed_run(
                    self.args, self.matrix, self.outputs, self.builders
                )
        fetch.assert_not_called()

    def test_finalize_rejects_manifest_revision_disagreement(self) -> None:
        manifests = {
            **self.manifests,
            "x64": {**self.manifests["x64"], "source_revision": "c" * 40},
        }

        def fetch(builder, _key_path, _run_id):
            return manifests[builder.architecture]

        with mock.patch.object(
            package_builder, "validate_builders", return_value=self.records
        ), mock.patch.object(
            package_builder, "ssh_key_path", return_value=Path("/tmp/key")
        ), mock.patch.object(package_builder, "wait_for_ssh"), mock.patch.object(
            package_builder, "fetch_completed_manifest", side_effect=fetch
        ), mock.patch.object(
            package_builder, "create_post_build_amis"
        ) as create, mock.patch.object(
            package_builder, "write_last_run"
        ) as write_last:
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "disagree"):
                package_builder.finalize_completed_run(
                    self.args, self.matrix, self.outputs, self.builders
                )
        create.assert_not_called()
        write_last.assert_not_called()


class AmiCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        self.matrix = package_builder.load_matrix()
        self.lock_sha256 = "b" * 64

    def test_only_the_authoritative_complete_matrix_is_image_eligible(self) -> None:
        complete = package_builder.Selection(
            tuple(self.matrix.architectures),
            tuple(self.matrix.distributions),
            self.matrix.postgresql_majors,
        )
        self.assertTrue(
            package_builder.is_complete_matrix_selection(complete, self.matrix)
        )
        partials = (
            package_builder.Selection(
                (complete.architectures[0],),
                complete.distributions,
                complete.postgresql_majors,
            ),
            package_builder.Selection(
                complete.architectures,
                complete.distributions[:-1],
                complete.postgresql_majors,
            ),
            package_builder.Selection(
                complete.architectures,
                complete.distributions,
                (),
            ),
        )
        for partial in partials:
            with self.subTest(partial=partial):
                self.assertFalse(
                    package_builder.is_complete_matrix_selection(partial, self.matrix)
                )

    def test_complete_matrix_fingerprint_is_lock_bound_and_deterministic(self) -> None:
        first = package_builder.complete_matrix_fingerprint(
            self.matrix, self.lock_sha256
        )
        self.assertEqual(
            first,
            package_builder.complete_matrix_fingerprint(
                self.matrix, self.lock_sha256
            ),
        )
        self.assertNotEqual(
            first,
            package_builder.complete_matrix_fingerprint(self.matrix, "c" * 64),
        )
        self.assertRegex(first, r"^[0-9a-f]{64}$")

    def test_cached_ami_lookup_requires_exact_architecture_and_provenance(self) -> None:
        architecture = self.matrix.architectures["arm64"]
        fingerprint = package_builder.complete_matrix_fingerprint(
            self.matrix, self.lock_sha256
        )
        tags = {
            **package_builder.CACHE_TAGS,
            "BuildArchitecture": architecture.name,
            "Architecture": architecture.aws_architecture,
            "BootstrapSchemaRevision": str(
                package_builder.BOOTSTRAP_SCHEMA_REVISION),
            "PoetryLockSha256": self.lock_sha256,
            "CompleteMatrixFingerprint": fingerprint,
        }
        image = {
            "ImageId": "ami-0123456789abcdef0",
            "Architecture": architecture.aws_architecture,
            "RootDeviceType": "ebs",
            "VirtualizationType": "hvm",
            "CreationDate": "2026-07-29T00:00:00Z",
            "Tags": [
                {"Key": key, "Value": value} for key, value in tags.items()
            ],
        }
        with mock.patch.object(
            package_builder, "aws_json", return_value={"Images": [image]}
        ):
            selected = package_builder.latest_cached_ami(
                "us-east-1", architecture, self.lock_sha256, fingerprint
            )
        self.assertEqual(selected, image)

        wrong_architecture = {**image, "Architecture": "x86_64"}
        with mock.patch.object(
            package_builder,
            "aws_json",
            return_value={"Images": [wrong_architecture]},
        ):
            self.assertIsNone(
                package_builder.latest_cached_ami(
                    "us-east-1", architecture, self.lock_sha256, fingerprint
                )
            )

    def test_partial_matrix_cannot_reach_ami_creation(self) -> None:
        selection = package_builder.Selection(
            ("arm64",),
            tuple(self.matrix.distributions),
            self.matrix.postgresql_majors,
        )
        args = argparse.Namespace(region="us-east-1", s3_prefix="bioseq/packages")
        with mock.patch.object(package_builder, "create_cached_ami") as create:
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "complete|partial"
            ):
                package_builder.create_post_build_amis(
                    args=args,
                    matrix=self.matrix,
                    selection=selection,
                    outputs={},
                    builders={},
                    build_result={},
                    lock_sha256=self.lock_sha256,
                )
        create.assert_not_called()

    def test_complete_matrix_revalidates_both_workers_before_imaging(self) -> None:
        selection = package_builder.Selection(
            tuple(self.matrix.architectures),
            tuple(self.matrix.distributions),
            self.matrix.postgresql_majors,
        )
        builders = {
            "arm64": package_builder.Builder(
                "arm64",
                "i-0aaabbbcccdddeee1",
                "c7g.xlarge",
                "arm64",
                "203.0.113.1",
                "10.0.0.1",
                "ami-0123456789abcdef0",
            ),
            "x64": package_builder.Builder(
                "x64",
                "i-0123456789abcdef0",
                "m4.xlarge",
                "x86_64",
                "203.0.113.2",
                "10.0.0.2",
                "ami-0123456789abcdef1",
            ),
        }
        revision = "a" * 40
        run_id = f"20260730T012519Z-{revision[:12]}"
        build_result = {
            "status": "success",
            "run_id": run_id,
            "revision": revision,
            "poetry_lock_sha256": self.lock_sha256,
            "artifact_count": (
                len(self.matrix.architectures)
                * len(self.matrix.distributions)
                * len(self.matrix.postgresql_majors)
            ),
            "manifests": {"arm64": {}, "x64": {}},
            "worker_units": {
                "arm64": f"bioseq-package-{run_id}-arm64.service",
                "x64": f"bioseq-package-{run_id}-x64.service",
            },
            "worker_launch_acknowledged": {"arm64": True, "x64": True},
            "python_environments": {
                architecture: {
                    "venv_path": (
                        f"{package_builder.REMOTE_POETRY_VENV_ROOT}/"
                        f"{self.lock_sha256}"
                    ),
                    "python": "3.12.3",
                    "poetry": "1.8.5",
                }
                for architecture in self.matrix.architectures
            },
        }
        args = argparse.Namespace(region="us-east-1", s3_prefix="bioseq/packages")
        outputs = {
            "package_bucket_name": "bucket",
            "signing_key_secret_arn": (
                "arn:aws:secretsmanager:us-east-1:123456789012:secret:key"
            ),
        }
        uploads_per_architecture = [object()] * len(self.matrix.distributions)

        def image_architecture(*call_args, **_call_kwargs):
            architecture = call_args[2]
            return {"ImageId": f"ami-{architecture.name}"}

        with mock.patch.object(
            package_builder, "validate_manifest", return_value=uploads_per_architecture
        ) as validate, mock.patch.object(
            package_builder, "verify_s3_uploads"
        ) as verify, mock.patch.object(
            package_builder, "ssh_key_path", return_value=Path("/tmp/key")
        ), mock.patch.object(
            package_builder, "require_quiescent_remote_worker"
        ), mock.patch.object(
            package_builder, "create_cached_ami", side_effect=image_architecture
        ) as create:
            images = package_builder.create_post_build_amis(
                args=args,
                matrix=self.matrix,
                selection=selection,
                outputs=outputs,
                builders=builders,
                build_result=build_result,
                lock_sha256=self.lock_sha256,
            )

        self.assertEqual(set(images), {"arm64", "x64"})
        self.assertEqual(validate.call_count, 2)
        verify.assert_called_once()
        self.assertEqual(create.call_count, 2)
        expected_fingerprint = package_builder.complete_matrix_fingerprint(
            self.matrix, self.lock_sha256
        )
        observed_units = set()
        for call in create.call_args_list:
            self.assertEqual(call.args[5], self.lock_sha256)
            self.assertEqual(call.args[6], expected_fingerprint)
            observed_units.add(call.args[7])
        self.assertEqual(
            observed_units,
            {
                f"bioseq-package-{run_id}-arm64.service",
                f"bioseq-package-{run_id}-x64.service",
            },
        )

    def test_cached_ami_requires_successful_unit_and_keeps_builder_running(self) -> None:
        builder = package_builder.Builder(
            "arm64",
            "i-0aaabbbcccdddeee1",
            "c7g.xlarge",
            "arm64",
            "203.0.113.1",
            "10.0.0.1",
            "ami-0123456789abcdef0",
        )
        fingerprint = "d" * 64
        created = {"ImageId": "ami-0123456789abcdef1"}
        observed_tags: dict[str, str] = {}

        def capture_tags(*args):
            for value in args:
                if isinstance(value, str) and value.startswith("Key="):
                    key, tag_value = value.removeprefix("Key=").split(",Value=", 1)
                    observed_tags[key] = tag_value

        def available_image(_region, _image_id):
            return {
                **created,
                "State": "available",
                "Architecture": "arm64",
                "RootDeviceType": "ebs",
                "VirtualizationType": "hvm",
                "Tags": [
                    {"Key": key, "Value": value}
                    for key, value in observed_tags.items()
                ],
            }

        worker_argv = (
            f"{package_builder.REMOTE_POETRY_VENV_ROOT}/{self.lock_sha256}/bin/python",
            package_builder._remote_committed_source_path("a" * 40)
            + "/infra/packaging/build_packages.py",
        )
        events: list[str] = []

        def describe(*_args):
            events.append("describe")
            return {"State": {"Name": "running"}}

        def sync(*_args, **_kwargs):
            events.append("sync")
            return subprocess.CompletedProcess([], 0, "", "")

        def quiescent(*_args, **_kwargs):
            events.append("quiescent")

        def create_image(*_args):
            events.append("create-image")
            return created

        with mock.patch.object(
            package_builder,
            "require_quiescent_remote_worker",
            side_effect=quiescent,
        ) as require_quiescent, mock.patch.object(
            package_builder,
            "describe_builder",
            side_effect=describe,
        ), mock.patch.object(
            package_builder, "ssh_run", side_effect=sync
        ) as ssh, mock.patch.object(
            package_builder, "aws_json", side_effect=create_image
        ) as aws_json, mock.patch.object(
            package_builder, "aws_no_output", side_effect=capture_tags
        ) as aws_no_output, mock.patch.object(
            package_builder, "wait_for_cached_ami", side_effect=available_image
        ):
            image = package_builder.create_cached_ami(
                "us-east-1",
                builder,
                self.matrix.architectures["arm64"],
                Path("/tmp/key"),
                "a" * 40,
                self.lock_sha256,
                fingerprint,
                "bioseq-arm64.service",
                worker_argv,
                allow_not_found=True,
            )

        self.assertEqual(image["State"], "available")
        self.assertEqual(events, ["describe", "sync", "quiescent", "create-image"])
        require_quiescent.assert_called_once_with(
            builder,
            Path("/tmp/key"),
            "bioseq-arm64.service",
            worker_argv,
            allow_not_found=True,
        )
        ssh.assert_called_once_with(builder, Path("/tmp/key"), "sync")
        create_args = aws_json.call_args.args
        self.assertIn("create-image", create_args)
        self.assertIn("--no-reboot", create_args)
        tag_args = aws_no_output.call_args.args
        self.assertIn(f"Key=PoetryLockSha256,Value={self.lock_sha256}", tag_args)
        self.assertIn(f"Key=CompleteMatrixFingerprint,Value={fingerprint}", tag_args)


class ManifestTests(unittest.TestCase):
    def test_s3_verification_heads_package_signature_and_repository_objects(self) -> None:
        upload = {
            "key": "bioseq/packages/x64/postgresql17/debian_13/stable/package.deb",
            "size": 10,
            "etag": "package-etag",
            "version_id": "package-version",
            "server_side_encryption": "AES256",
            "signature_verification": {
                "objects": [
                    {
                        "key": (
                            "bioseq/packages/x64/postgresql17/debian_13/"
                            "stable/InRelease"
                        ),
                        "size": 20,
                        "etag": "signature-etag",
                        "version_id": "signature-version",
                        "server_side_encryption": "AES256",
                    }
                ],
                "repository_objects": [
                    {
                        "key": (
                            "bioseq/packages/x64/postgresql17/debian_13/"
                            "stable/Packages"
                        ),
                        "size": 30,
                        "etag": "repository-etag",
                        "version_id": "repository-version",
                        "server_side_encryption": "AES256",
                    }
                ],
            },
        }
        responses = [
            {
                "ContentLength": 10,
                "ETag": '"package-etag"',
                "VersionId": "package-version",
                "ServerSideEncryption": "AES256",
            },
            {
                "ContentLength": 20,
                "ETag": '"signature-etag"',
                "VersionId": "signature-version",
                "ServerSideEncryption": "AES256",
            },
            {
                "ContentLength": 30,
                "ETag": '"repository-etag"',
                "VersionId": "repository-version",
                "ServerSideEncryption": "AES256",
            },
        ]
        with mock.patch.object(package_builder, "aws_json", side_effect=responses) as aws:
            package_builder.verify_s3_uploads("us-east-1", "bucket", [upload])
        self.assertEqual(aws.call_count, 3)
        self.assertIn("--version-id", aws.call_args_list[0].args)
        self.assertIn("package-version", aws.call_args_list[0].args)
        self.assertIn("signature-version", aws.call_args_list[1].args)
        self.assertIn("repository-version", aws.call_args_list[2].args)

    def test_s3_verification_rejects_kms_when_aes256_is_required(self) -> None:
        upload = {
            "key": "bioseq/packages/x64/postgresql17/debian_13/stable/package.deb",
            "size": 10,
            "etag": "package-etag",
            "version_id": "package-version",
            "server_side_encryption": "AES256",
            "signature_verification": {"objects": []},
        }
        response = {
            "ContentLength": 10,
            "ETag": '"package-etag"',
            "VersionId": "package-version",
            "ServerSideEncryption": "aws:kms",
        }
        with mock.patch.object(package_builder, "aws_json", return_value=response):
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "AES256|encrypted"
            ):
                package_builder.verify_s3_uploads("us-east-1", "bucket", [upload])

    def test_success_manifest_requires_optimization_zero_and_exact_s3_key(self) -> None:
        matrix = package_builder.load_matrix()
        selection = package_builder.Selection(("x64",), ("debian_13",), (17,))
        revision = "a" * 40
        lock_sha256 = "b" * 64
        python_environment = package_builder.PoetryEnvironment(
            lock_sha256=lock_sha256,
            venv_path=f"/opt/bioseq-package-builder/venvs/{lock_sha256}",
            python_version="3.12.3",
            poetry_version="1.8.5",
        )
        artifact_name = (
            f"bioseq-postgresql17_{package_builder.EXPECTED_PACKAGE_VERSION}_amd64.deb"
        )
        manifest = {
            "schema_revision": package_builder.PACKAGE_MANIFEST_SCHEMA_REVISION,
            "status": "success",
            "package_version": package_builder.EXPECTED_PACKAGE_VERSION,
            "started_at": "2026-08-10T00:00:00Z",
            "completed_at": "2026-08-10T00:01:00Z",
            "source_revision": revision,
            "poetry_lock_sha256": lock_sha256,
            "python_environment": python_environment.manifest_value(),
            "architecture": "x64",
            "compiler_optimization": compiler_optimization_evidence(
                "x64", distros=("debian_13",)
            ),
            "requested": {"postgres": [17], "distros": ["debian_13"]},
            "s3": {
                "bucket": "bucket",
                "region": "us-east-1",
                "base_prefix": "bioseq/packages",
            },
            "signing": {"fingerprint": package_builder.EXPECTED_SIGNING_FINGERPRINT},
            "errors": [],
            "cells": [
                {
                    "postgres_major": 17,
                    "distro": "debian_13",
                    "package_name": "bioseq-postgresql17",
                    "status": "uploaded",
                    "error": None,
                    "artifact": {
                        "filename": artifact_name,
                        "size": 123,
                        "sha256": "a" * 64,
                        "build_size": 123,
                        "build_sha256": "a" * 64,
                    },
                    "validation": {
                        "passed": True,
                        "six_artifacts": True,
                        "rpath_absent": True,
                        "compiler_runtime_safe": True,
                        "template_flatten_profile": "dna4_uint64",
                        "binaries": [
                            {
                                "path": "/tmp/bioseq.so",
                                "elf_architecture": "x86_64",
                                "rpath_absent": True,
                                "compiler_runtime_safe": True,
                            },
                            {
                                "path": "/tmp/bioseq_cache.so",
                                "elf_architecture": "x86_64",
                                "rpath_absent": True,
                                "compiler_runtime_safe": True,
                            },
                        ],
                    },
                    "smoke": {
                        "passed": True,
                        "package_installed": True,
                        "preload_active": True,
                        "bioseq_extension_created": True,
                        "bioseq_cache_extension_created": True,
                        "nonempty_roundtrip": True,
                        "fresh_container": True,
                        "architecture": "x64",
                        "postgres_major": 17,
                        "distro": "debian_13",
                        "roundtrip": "ACGT",
                        "container_image": "debian:13",
                    },
                    "upload": {
                        "published": True,
                        "bucket": "bucket",
                        "key": (
                            "bioseq/packages/x64/postgresql17/debian_13/stable/"
                            + artifact_name
                        ),
                        "size": 123,
                        "sha256": "a" * 64,
                        "etag": "package-etag",
                        "version_id": "package-version",
                        "server_side_encryption": "AES256",
                        "download_validation": {
                            "passed": True,
                            "six_artifacts": True,
                            "rpath_absent": True,
                            "compiler_runtime_safe": True,
                            "template_flatten_profile": "dna4_uint64",
                        },
                        "download_smoke": {
                            "passed": True,
                            "package_installed": True,
                            "preload_active": True,
                            "bioseq_extension_created": True,
                            "bioseq_cache_extension_created": True,
                            "nonempty_roundtrip": True,
                            "fresh_container": True,
                            "architecture": "x64",
                            "postgres_major": 17,
                            "distro": "debian_13",
                            "roundtrip": "ACGT",
                            "container_image": "debian:13",
                        },
                        "signature_verification": {
                            "passed": True,
                            "format": "deb",
                            "public_key_imported": True,
                            "signing_key_fingerprint": (
                                package_builder.EXPECTED_SIGNING_FINGERPRINT
                            ),
                            "package_signature_verified": True,
                            "package_signature": "signed-deb-release-sha256",
                            "repository_signature_verified": True,
                            "objects": [
                                {
                                    "key": (
                                        "bioseq/packages/x64/postgresql17/"
                                        "debian_13/stable/Release.key"
                                    ),
                                    "size": 1,
                                    "sha256": "b" * 64,
                                    "etag": "etag-key",
                                    "version_id": "version-key",
                                    "server_side_encryption": "AES256",
                                },
                                {
                                    "key": (
                                        "bioseq/packages/x64/postgresql17/"
                                        "debian_13/stable/Release"
                                    ),
                                    "size": 2,
                                    "sha256": "c" * 64,
                                    "etag": "etag-release",
                                    "version_id": "version-release",
                                    "server_side_encryption": "AES256",
                                },
                                {
                                    "key": (
                                        "bioseq/packages/x64/postgresql17/"
                                        "debian_13/stable/Release.gpg"
                                    ),
                                    "size": 3,
                                    "sha256": "d" * 64,
                                    "etag": "etag-release-gpg",
                                    "version_id": "version-release-gpg",
                                    "server_side_encryption": "AES256",
                                },
                                {
                                    "key": (
                                        "bioseq/packages/x64/postgresql17/"
                                        "debian_13/stable/InRelease"
                                    ),
                                    "size": 4,
                                    "sha256": "e" * 64,
                                    "etag": "etag-inrelease",
                                    "version_id": "version-inrelease",
                                    "server_side_encryption": "AES256",
                                },
                            ],
                            "repository_objects": [
                                {
                                    "key": (
                                        "bioseq/packages/x64/postgresql17/"
                                        "debian_13/stable/Packages"
                                    ),
                                    "size": 5,
                                    "sha256": "f" * 64,
                                    "etag": "etag-packages",
                                    "version_id": "version-packages",
                                    "server_side_encryption": "AES256",
                                },
                                {
                                    "key": (
                                        "bioseq/packages/x64/postgresql17/"
                                        "debian_13/stable/Packages.gz"
                                    ),
                                    "size": 6,
                                    "sha256": "1" * 64,
                                    "etag": "etag-packages-gz",
                                    "version_id": "version-packages-gz",
                                    "server_side_encryption": "AES256",
                                },
                            ],
                        },
                    },
                }
            ],
        }
        uploads = package_builder.validate_manifest(
            manifest,
            architecture="x64",
            selection=selection,
            matrix=matrix,
            bucket="bucket",
            region="us-east-1",
            s3_prefix="bioseq/packages",
            source_revision=revision,
            poetry_lock_sha256=lock_sha256,
            python_environment=python_environment,
        )
        self.assertEqual(len(uploads), 1)
        manifest["cells"][0]["validation"]["template_flatten_profile"] = (
            "all_alphabets_uint64"
        )
        with self.assertRaisesRegex(package_builder.PackageBuilderError, "failed validation"):
            package_builder.validate_manifest(
                manifest,
                architecture="x64",
                selection=selection,
                matrix=matrix,
                bucket="bucket",
                region="us-east-1",
                s3_prefix="bioseq/packages",
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=python_environment,
            )
        manifest["cells"][0]["validation"]["template_flatten_profile"] = (
            "dna4_uint64"
        )
        manifest["cells"][0]["upload"]["download_smoke"]["fresh_container"] = False
        with self.assertRaisesRegex(package_builder.PackageBuilderError, "S3-download smoke"):
            package_builder.validate_manifest(
                manifest,
                architecture="x64",
                selection=selection,
                matrix=matrix,
                bucket="bucket",
                region="us-east-1",
                s3_prefix="bioseq/packages",
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=python_environment,
            )
        manifest["cells"][0]["upload"]["download_smoke"]["fresh_container"] = True
        manifest["cells"][0]["upload"]["signature_verification"][
            "signing_key_fingerprint"
        ] = "B" * len(package_builder.EXPECTED_SIGNING_FINGERPRINT)
        with self.assertRaisesRegex(package_builder.PackageBuilderError, "signature verification"):
            package_builder.validate_manifest(
                manifest,
                architecture="x64",
                selection=selection,
                matrix=matrix,
                bucket="bucket",
                region="us-east-1",
                s3_prefix="bioseq/packages",
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=python_environment,
            )

        manifest["cells"][0]["upload"]["signature_verification"][
            "signing_key_fingerprint"
        ] = package_builder.EXPECTED_SIGNING_FINGERPRINT
        manifest["source_revision"] = "c" * 40
        with self.assertRaisesRegex(package_builder.PackageBuilderError, "revision|provenance"):
            package_builder.validate_manifest(
                manifest,
                architecture="x64",
                selection=selection,
                matrix=matrix,
                bucket="bucket",
                region="us-east-1",
                s3_prefix="bioseq/packages",
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=python_environment,
            )

        manifest["source_revision"] = revision
        manifest["python_environment"]["poetry"] = "0.0.0"
        with self.assertRaisesRegex(
            package_builder.PackageBuilderError, "Poetry|Python environment|provenance"
        ):
            package_builder.validate_manifest(
                manifest,
                architecture="x64",
                selection=selection,
                matrix=matrix,
                bucket="bucket",
                region="us-east-1",
                s3_prefix="bioseq/packages",
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=python_environment,
            )

        manifest["python_environment"] = python_environment.manifest_value()
        manifest["package_version"] = "0.0.0"
        with self.assertRaisesRegex(package_builder.PackageBuilderError, "package version"):
            package_builder.validate_manifest(
                manifest,
                architecture="x64",
                selection=selection,
                matrix=matrix,
                bucket="bucket",
                region="us-east-1",
                s3_prefix="bioseq/packages",
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=python_environment,
            )

        manifest["package_version"] = package_builder.EXPECTED_PACKAGE_VERSION
        manifest["cells"][0]["upload"]["version_id"] = ""
        with self.assertRaisesRegex(
            package_builder.PackageBuilderError, "upload contract|VersionId|version"
        ):
            package_builder.validate_manifest(
                manifest,
                architecture="x64",
                selection=selection,
                matrix=matrix,
                bucket="bucket",
                region="us-east-1",
                s3_prefix="bioseq/packages",
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=python_environment,
            )

        manifest["cells"][0]["upload"]["version_id"] = "package-version"
        manifest["cells"][0]["upload"]["server_side_encryption"] = "aws:kms"
        with self.assertRaisesRegex(
            package_builder.PackageBuilderError, "upload contract|AES256|encryption"
        ):
            package_builder.validate_manifest(
                manifest,
                architecture="x64",
                selection=selection,
                matrix=matrix,
                bucket="bucket",
                region="us-east-1",
                s3_prefix="bioseq/packages",
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=python_environment,
            )

class SsmQuiescenceTests(unittest.TestCase):
    def test_ssm_quiescence_requires_success_from_every_exact_instance(self) -> None:
        instance_ids = ["i-0aaabbbcccdddee1", "i-0aaabbbcccdddee2"]
        responses = [
            {"Command": {"CommandId": "command-1"}},
            {
                "CommandInvocations": [
                    {"InstanceId": value, "Status": "Success"}
                    for value in reversed(instance_ids)
                ]
            },
        ]
        with mock.patch.object(
            package_builder, "aws_json", side_effect=responses
        ) as aws:
            command_id = package_builder.run_ssm_shell_command(
                "us-east-1",
                instance_ids,
                script="true\n",
                comment="test quiescence",
            )
        self.assertEqual(command_id, "command-1")
        self.assertEqual(aws.call_count, 2)
        self.assertIn("send-command", aws.call_args_list[0].args)
        self.assertIn("list-command-invocations", aws.call_args_list[1].args)
        send_args = aws.call_args_list[0].args
        parameters = json.loads(send_args[send_args.index("--parameters") + 1])
        self.assertEqual(len(parameters["commands"]), 1)
        self.assertTrue(parameters["commands"][0].startswith("/bin/bash -lc "))
        self.assertIn("true", parameters["commands"][0])

    def test_ssm_quiescence_surfaces_remote_activity_and_fails_closed(self) -> None:
        responses = [
            {"Command": {"CommandId": "command-2"}},
            {
                "CommandInvocations": [
                    {
                        "InstanceId": "i-0aaabbbcccdddee1",
                        "Status": "Failed",
                        "CommandPlugins": [
                            {
                                "Output": (
                                    '{"worker_pids":[42],"active_units":[],'
                                    '"containers":[]}'
                                )
                            }
                        ],
                    }
                ]
            },
        ]
        with mock.patch.object(
            package_builder, "aws_json", side_effect=responses
        ), self.assertRaisesRegex(
            package_builder.PackageBuilderError, "worker_pids.*42"
        ):
            package_builder.run_ssm_shell_command(
                "us-east-1",
                ["i-0aaabbbcccdddee1"],
                script="exit 70\n",
                comment="test active worker",
            )

    def test_ssm_quiescence_cancels_and_waits_for_terminal_cancelled(self) -> None:
        instance_id = "i-0aaabbbcccdddee1"
        responses = [
            {"Command": {"CommandId": "command-cancelled"}},
            {
                "CommandInvocations": [
                    {"InstanceId": instance_id, "Status": "Cancelling"}
                ]
            },
            {
                "CommandInvocations": [
                    {"InstanceId": instance_id, "Status": "Cancelled"}
                ]
            },
        ]
        with mock.patch.object(
            package_builder, "aws_json", side_effect=responses
        ), mock.patch.object(
            package_builder.time, "monotonic", side_effect=(0, 91)
        ), mock.patch.object(
            package_builder, "aws_no_output"
        ) as cancel, self.assertRaisesRegex(
            package_builder.PackageBuilderError, "status=Cancelled"
        ):
            package_builder.run_ssm_shell_command(
                "us-east-1",
                [instance_id],
                script="true\n",
                comment="test terminal cancellation",
            )
        self.assertIn("cancel-command", cancel.call_args.args)
        self.assertIn(instance_id, cancel.call_args.args)

    def test_ssm_quiescence_rejects_nonterminal_cancelling_at_deadline(self) -> None:
        instance_id = "i-0aaabbbcccdddee1"
        responses = [
            {"Command": {"CommandId": "command-stuck"}},
            {
                "CommandInvocations": [
                    {"InstanceId": instance_id, "Status": "InProgress"}
                ]
            },
            {
                "CommandInvocations": [
                    {"InstanceId": instance_id, "Status": "Cancelling"}
                ]
            },
        ]
        with mock.patch.object(
            package_builder, "aws_json", side_effect=responses
        ), mock.patch.object(
            package_builder.time, "monotonic", side_effect=(0, 91, 121)
        ), mock.patch.object(
            package_builder, "aws_no_output"
        ) as cancel, self.assertRaisesRegex(
            package_builder.PackageBuilderError,
            "did not become terminal after cancellation",
        ):
            package_builder.run_ssm_shell_command(
                "us-east-1",
                [instance_id],
                script="true\n",
                comment="test stuck cancellation",
            )
        cancel.assert_called_once()

    def test_quiescence_probe_covers_workers_units_repository_and_smoke(self) -> None:
        script = package_builder.remote_package_quiescence_script(poweroff=False)
        for required in (
            package_builder.REMOTE_PACKAGE_LIFECYCLE_LOCK,
            package_builder.REMOTE_WORKSPACE,
            "build_packages.py",
            "bioseq-package-",
            "docker",
            'mount["Source"].startswith(workspace_prefix)',
        ):
            with self.subTest(required=required):
                self.assertIn(required, script)
        self.assertNotIn("poweroff", script)
        poweroff = package_builder.remote_package_quiescence_script(
            poweroff=True, not_after_epoch=2_000_000_000
        )
        self.assertIn("poweroff --no-block", poweroff)
        self.assertLess(poweroff.index("build_packages.py"), poweroff.index("poweroff"))
        self.assertEqual(poweroff.count("date +%s"), 2)
        self.assertLess(poweroff.rindex("date +%s"), poweroff.index("poweroff"))


class ReleaseAutomationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.matrix = package_builder.load_matrix()
        self.owner_token = "bioseq/repository:1234567890"
        self.owner_token_sha256, self.owner_description = (
            package_builder.ci_access_owner(self.owner_token)
        )

    def builder_record(self, architecture: str) -> dict:
        expected = self.matrix.architectures[architecture]
        suffix = "1" if architecture == "arm64" else "2"
        return {
            "InstanceId": f"i-0aaabbbcccdddee{suffix}",
            "ImageId": f"ami-0aaabbbcccdddee{suffix}",
            "InstanceType": expected.instance_type,
            "Architecture": expected.aws_architecture,
            "PublicIpAddress": f"203.0.113.{suffix}",
            "PrivateIpAddress": f"10.0.0.{suffix}",
            "Placement": {"AvailabilityZone": "us-east-1c"},
            "State": {"Name": "stopped"},
            "SecurityGroups": [{"GroupId": "sg-0aaabbbcccdddeee1"}],
            "Tags": [
                {"Key": key, "Value": value}
                for key, value in {
                    **package_builder.EXPECTED_INSTANCE_TAGS,
                    "BuildArchitecture": architecture,
                    "Architecture": expected.aws_architecture,
                    "AmiSchemaRevision": str(
                        package_builder.BOOTSTRAP_SCHEMA_REVISION
                    ),
                }.items()
            ],
        }

    def test_current_and_live_legacy_ami_schema_tags_are_exact(self) -> None:
        package_builder.require_builder_schema_tag(
            {"AmiSchemaRevision": "2"}, "i-0aaabbbcccdddeee1"
        )
        package_builder.require_builder_schema_tag(
            {"AmiSchemaVersion": "2"}, "i-0aaabbbcccdddeee1"
        )
        with self.assertRaisesRegex(
            package_builder.PackageBuilderError, "incompatible AMI schema"
        ):
            package_builder.require_builder_schema_tag(
                {"AmiSchemaRevision": "2", "AmiSchemaVersion": "1"},
                "i-0aaabbbcccdddeee1",
            )

    def test_existing_builder_discovery_is_tagged_and_account_local(self) -> None:
        records = {
            name: self.builder_record(name) for name in self.matrix.architectures
        }
        payload = {
            "Reservations": [
                {"Instances": [records["arm64"]]},
                {"Instances": [records["x64"]]},
            ]
        }
        with mock.patch.object(
            package_builder, "aws_json", return_value=payload
        ) as aws, mock.patch.object(
            package_builder, "validate_builders", return_value=records
        ) as validate:
            builders, observed = package_builder.discover_existing_builders(
                "us-east-1", self.matrix, ephemeral_ssh=True
            )
        self.assertEqual(observed, records)
        self.assertEqual(set(builders), {"arm64", "x64"})
        self.assertTrue(
            all(value.ssh_access == "ec2-instance-connect" for value in builders.values())
        )
        argv = aws.call_args.args
        self.assertEqual(argv[:3], ("us-east-1", "ec2", "describe-instances"))
        self.assertIn("Name=tag:Component,Values=package-build-machine", argv)
        self.assertIn("Name=tag:ManagedBy,Values=opentofu", argv)
        self.assertNotIn(records["arm64"]["InstanceId"], argv)
        validate.assert_called_once()

    def test_existing_only_never_applies_opentofu(self) -> None:
        records = {
            name: self.builder_record(name) for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                architecture=name,
                instance_id=record["InstanceId"],
                instance_type=record["InstanceType"],
                aws_architecture=record["Architecture"],
                public_ip=record["PublicIpAddress"],
                private_ip=record["PrivateIpAddress"],
                source_ami_id=record["ImageId"],
                availability_zone="us-east-1c",
                ssh_access="ec2-instance-connect",
            )
            for name, record in records.items()
        }
        args = argparse.Namespace(
            region="us-east-1",
            s3_prefix=package_builder.DEFAULT_S3_PREFIX,
            existing_only=True,
            ssh_key="/tmp/ephemeral-key",
            lifecycle_receipt="/tmp/access.json",
        )
        outputs = {
            "account_id": "123456789012",
            "caller_arn": "arn:aws:iam::123456789012:user/release",
            "ssh_private_key_path": "/tmp/ephemeral-key",
        }
        with mock.patch.object(
            package_builder,
            "discover_existing_infrastructure",
            return_value=(outputs, builders, records),
        ), mock.patch.object(
            package_builder, "read_ci_lifecycle_receipt"
        ) as read_receipt, mock.patch.object(
            package_builder, "ssh_key_path"
        ), mock.patch.object(
            package_builder, "start_builders", return_value=records
        ) as start, mock.patch.object(
            package_builder, "refreshed_builders", return_value=builders
        ), mock.patch.object(package_builder, "tofu_apply") as tofu_apply:
            observed = package_builder.provision_and_start(args, self.matrix)
        self.assertEqual(observed[0], outputs)
        read_receipt.assert_called_once()
        self.assertEqual(
            start.call_args.kwargs["lifecycle_receipt"],
            Path("/tmp/access.json").resolve(),
        )
        tofu_apply.assert_not_called()

    def test_start_records_stop_ownership_only_for_builder_it_starts(self) -> None:
        initial = {
            name: {
                **self.builder_record(name),
                "State": {"Name": "running" if name == "arm64" else "stopped"},
            }
            for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in initial.items()
        }
        running = {
            name: {**record, "State": {"Name": "running"}}
            for name, record in initial.items()
        }
        identity = {
            "Account": "123456789012",
            "Arn": "arn:aws:iam::123456789012:user/release",
        }
        lifecycle_events: list[str] = []
        with mock.patch.object(
            package_builder, "validate_builders", side_effect=(initial, running)
        ), mock.patch.object(
            package_builder,
            "record_builder_stop_ownership",
            side_effect=lambda **_kwargs: lifecycle_events.append("own"),
        ) as own, mock.patch.object(
            package_builder,
            "aws_no_output",
            side_effect=lambda *_args: lifecycle_events.append("start"),
        ) as start, mock.patch.object(package_builder, "run_command"):
            package_builder.start_builders(
                "us-east-1",
                builders,
                self.matrix,
                lifecycle_receipt=Path("/tmp/access.json"),
                caller_identity=identity,
            )
        own.assert_called_once()
        self.assertEqual(own.call_args.kwargs["architecture"], "x64")
        start.assert_called_once()
        self.assertIn(builders["x64"].instance_id, start.call_args.args)
        self.assertNotIn(builders["arm64"].instance_id, start.call_args.args)
        self.assertEqual(lifecycle_events, ["own", "start"])

    def test_pending_builder_remains_externally_owned(self) -> None:
        initial = {
            name: {
                **self.builder_record(name),
                "State": {"Name": "pending" if name == "arm64" else "running"},
            }
            for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in initial.items()
        }
        running = {
            name: {**record, "State": {"Name": "running"}}
            for name, record in initial.items()
        }
        identity = {
            "Account": "123456789012",
            "Arn": "arn:aws:iam::123456789012:user/release",
        }
        with mock.patch.object(
            package_builder, "validate_builders", side_effect=(initial, running)
        ), mock.patch.object(
            package_builder, "record_builder_stop_ownership"
        ) as own, mock.patch.object(
            package_builder, "aws_no_output"
        ) as start, mock.patch.object(package_builder, "run_command"):
            package_builder.start_builders(
                "us-east-1",
                builders,
                self.matrix,
                lifecycle_receipt=Path("/tmp/access.json"),
                caller_identity=identity,
            )
        own.assert_not_called()
        start.assert_not_called()

    def test_stopping_builder_is_claimed_only_after_it_reaches_stopped(self) -> None:
        initial = {
            name: {
                **self.builder_record(name),
                "State": {"Name": "stopping" if name == "arm64" else "running"},
            }
            for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in initial.items()
        }
        running = {
            name: {**record, "State": {"Name": "running"}}
            for name, record in initial.items()
        }
        identity = {
            "Account": "123456789012",
            "Arn": "arn:aws:iam::123456789012:user/release",
        }
        events: list[str] = []

        def command(argv, **_kwargs):
            if "instance-stopped" in argv:
                events.append("wait-stopped")
            return subprocess.CompletedProcess(argv, 0, "", "")

        with mock.patch.object(
            package_builder, "validate_builders", side_effect=(initial, running)
        ), mock.patch.object(
            package_builder,
            "record_builder_stop_ownership",
            side_effect=lambda **_kwargs: events.append("own"),
        ) as own, mock.patch.object(
            package_builder,
            "aws_no_output",
            side_effect=lambda *_args: events.append("start"),
        ) as start, mock.patch.object(
            package_builder, "run_command", side_effect=command
        ):
            package_builder.start_builders(
                "us-east-1",
                builders,
                self.matrix,
                lifecycle_receipt=Path("/tmp/access.json"),
                caller_identity=identity,
            )
        self.assertEqual(events[:3], ["wait-stopped", "own", "start"])
        self.assertEqual(own.call_args.kwargs["architecture"], "arm64")
        self.assertIn(builders["arm64"].instance_id, start.call_args.args)

    def test_stop_ownership_receipt_persists_only_the_claimed_builder(self) -> None:
        records = {
            name: self.builder_record(name) for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in records.items()
        }
        identity = {
            "Account": "123456789012",
            "Arn": "arn:aws:iam::123456789012:user/release",
        }
        payload = {
            "schema_revision": package_builder.CI_ACCESS_SCHEMA_REVISION,
            "account": identity["Account"],
            "caller_arn": identity["Arn"],
            "region": "us-east-1",
            "security_group_id": "sg-0aaabbbcccdddeee1",
            "cidr": "203.0.113.10/32",
            "owner_token_sha256": self.owner_token_sha256,
            "description": self.owner_description,
            "rule_added": True,
            "closed": False,
            "builders": {
                name: {
                    "instance_id": builder.instance_id,
                    "initial_state": "stopped",
                    "stop_owned": False,
                }
                for name, builder in builders.items()
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            receipt = Path(tmp) / "access.json"
            package_builder.write_json_atomic(receipt, payload)
            for _retry in range(2):
                package_builder.record_builder_stop_ownership(
                    receipt=receipt,
                    region="us-east-1",
                    identity=identity,
                    builders=builders,
                    architecture="x64",
                )
            observed = json.loads(receipt.read_text(encoding="utf-8"))
        self.assertFalse(observed["builders"]["arm64"]["stop_owned"])
        self.assertTrue(observed["builders"]["x64"]["stop_owned"])

    def test_receipt_scoped_stop_leaves_externally_running_builder_unchanged(self) -> None:
        records = {
            name: {**self.builder_record(name), "State": {"Name": "running"}}
            for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in records.items()
        }
        final = {
            "arm64": records["arm64"],
            "x64": {**records["x64"], "State": {"Name": "stopped"}},
        }
        receipt = {
            "builders": {
                "arm64": {"stop_owned": False},
                "x64": {"stop_owned": True},
            }
        }
        with mock.patch.object(
            package_builder,
            "aws_preflight",
            return_value={
                "Account": "123456789012",
                "Arn": "arn:aws:iam::123456789012:user/release",
            },
        ), mock.patch.object(
            package_builder,
            "discover_existing_builders",
            return_value=(builders, records),
        ), mock.patch.object(
            package_builder, "read_ci_lifecycle_receipt", return_value=receipt
        ), mock.patch.object(
            package_builder, "wait_for_ssm_online"
        ), mock.patch.object(
            package_builder, "run_ssm_shell_command", side_effect=(
                "cmd-barrier", "cmd-x64-poweroff"
            )
        ) as ssm, mock.patch.object(
            package_builder, "wait_for_ec2_state"
        ) as wait, mock.patch.object(
            package_builder, "validate_builders", return_value=final
        ):
            states = package_builder.stop_existing_builders(
                "us-east-1",
                self.matrix,
                lifecycle_receipt=Path("/tmp/access.json"),
            )
        self.assertEqual(states, {"arm64": "running", "x64": "stopped"})
        self.assertEqual(ssm.call_count, 2)
        self.assertEqual(
            set(ssm.call_args_list[0].args[1]),
            {builder.instance_id for builder in builders.values()},
        )
        self.assertEqual(
            ssm.call_args_list[1].args[1], [builders["x64"].instance_id]
        )
        self.assertIn(
            "poweroff --no-block", ssm.call_args_list[1].kwargs["script"]
        )
        wait.assert_called_once_with(
            "us-east-1", builders["x64"].instance_id, "stopped"
        )

    def test_receipt_scoped_stop_without_owned_builders_is_noop(self) -> None:
        records = {
            name: {**self.builder_record(name), "State": {"Name": "running"}}
            for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in records.items()
        }
        receipt = {
            "builders": {
                name: {"stop_owned": False} for name in self.matrix.architectures
            }
        }
        with mock.patch.object(
            package_builder,
            "aws_preflight",
            return_value={
                "Account": "123456789012",
                "Arn": "arn:aws:iam::123456789012:user/release",
            },
        ), mock.patch.object(
            package_builder,
            "discover_existing_builders",
            return_value=(builders, records),
        ), mock.patch.object(
            package_builder, "read_ci_lifecycle_receipt", return_value=receipt
        ), mock.patch.object(
            package_builder, "wait_for_ssm_online"
        ) as online, mock.patch.object(
            package_builder, "run_ssm_shell_command"
        ) as ssm, mock.patch.object(
            package_builder, "run_command"
        ) as mutate, mock.patch.object(
            package_builder, "validate_builders"
        ) as validate:
            states = package_builder.stop_existing_builders(
                "us-east-1",
                self.matrix,
                lifecycle_receipt=Path("/tmp/access.json"),
            )
        self.assertEqual(states, {"arm64": "running", "x64": "running"})
        online.assert_not_called()
        ssm.assert_not_called()
        mutate.assert_not_called()
        validate.assert_not_called()

    def test_stop_uses_instance_initiated_poweroff_for_both_builders(self) -> None:
        records = {
            name: {**self.builder_record(name), "State": {"Name": "running"}}
            for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in records.items()
        }
        stopped = {
            name: {**record, "State": {"Name": "stopped"}}
            for name, record in records.items()
        }
        ssm_wait_barrier = threading.Barrier(2)

        def wait_for_ssm(_region, _instance_id):
            ssm_wait_barrier.wait(timeout=2)

        with mock.patch.object(package_builder, "aws_preflight"), mock.patch.object(
            package_builder,
            "discover_existing_builders",
            return_value=(builders, records),
        ), mock.patch.object(
            package_builder, "wait_for_ssm_online", side_effect=wait_for_ssm
        ), mock.patch.object(
            package_builder,
            "run_ssm_shell_command",
            side_effect=("cmd-barrier", "cmd-poweroff-a", "cmd-poweroff-b"),
        ) as ssm, mock.patch.object(
            package_builder, "wait_for_ec2_state"
        ) as state_wait, mock.patch.object(
            package_builder, "validate_builders", return_value=stopped
        ):
            states = package_builder.stop_existing_builders("us-east-1", self.matrix)
        self.assertEqual(states, {"arm64": "stopped", "x64": "stopped"})
        self.assertEqual(ssm.call_count, 3)
        self.assertEqual(
            set(ssm.call_args_list[0].args[1]),
            {builder.instance_id for builder in builders.values()},
        )
        for call in ssm.call_args_list[1:]:
            self.assertEqual(len(call.args[1]), 1)
            self.assertIn("poweroff --no-block", call.kwargs["script"])
        self.assertEqual(state_wait.call_count, 2)
        self.assertTrue(
            all(call.args[2] == "stopped" for call in state_wait.call_args_list)
        )

    def test_stop_refuses_all_poweroff_when_joint_quiescence_fails(self) -> None:
        records = {
            name: {**self.builder_record(name), "State": {"Name": "running"}}
            for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in records.items()
        }
        with mock.patch.object(package_builder, "aws_preflight"), mock.patch.object(
            package_builder,
            "discover_existing_builders",
            return_value=(builders, records),
        ), mock.patch.object(
            package_builder, "wait_for_ssm_online"
        ), mock.patch.object(
            package_builder,
            "run_ssm_shell_command",
            side_effect=package_builder.PackageBuilderError(
                "SSM package cleanup failed closed: worker_pids=[42]"
            ),
        ) as ssm, mock.patch.object(
            package_builder, "run_command"
        ) as poweroff_wait, mock.patch.object(
            package_builder, "validate_builders"
        ) as validate:
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "worker_pids.*42"
            ):
                package_builder.stop_existing_builders("us-east-1", self.matrix)
        ssm.assert_called_once()
        poweroff_wait.assert_not_called()
        validate.assert_not_called()

    def test_stop_waits_for_pending_builder_before_ssm_quiescence(self) -> None:
        records = {
            name: {
                **self.builder_record(name),
                "State": {"Name": "pending" if name == "arm64" else "running"},
            }
            for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in records.items()
        }
        stopped = {
            name: {**record, "State": {"Name": "stopped"}}
            for name, record in records.items()
        }
        with mock.patch.object(package_builder, "aws_preflight"), mock.patch.object(
            package_builder,
            "discover_existing_builders",
            return_value=(builders, records),
        ), mock.patch.object(
            package_builder, "wait_for_ssm_online"
        ) as online, mock.patch.object(
            package_builder,
            "run_ssm_shell_command",
            side_effect=("cmd-barrier", "cmd-poweroff-a", "cmd-poweroff-b"),
        ), mock.patch.object(
            package_builder, "wait_for_ec2_state"
        ) as state_wait, mock.patch.object(
            package_builder, "validate_builders", return_value=stopped
        ):
            states = package_builder.stop_existing_builders(
                "us-east-1", self.matrix
            )
        self.assertEqual(states, {"arm64": "stopped", "x64": "stopped"})
        self.assertEqual(online.call_count, 2)
        self.assertIn(
            mock.call("us-east-1", builders["arm64"].instance_id, "running"),
            state_wait.call_args_list,
        )

    def test_ci_ingress_receipt_revokes_only_a_rule_it_added(self) -> None:
        records = {
            name: self.builder_record(name) for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in records.items()
        }
        group = {
            "Tags": [
                {"Key": key, "Value": value}
                for key, value in {
                    "Project": "bioseq",
                    "ManagedBy": "opentofu",
                    "Component": "package-build-machine",
                    "Name": "bioseq-package-builder-sg",
                }.items()
            ],
            "IpPermissions": [],
        }
        with tempfile.TemporaryDirectory() as tmp:
            key = Path(tmp) / "key"
            key.write_text("private", encoding="ascii")
            key.with_name("key.pub").write_text(
                "ssh-ed25519 AAAA release-test\n", encoding="ascii"
            )
            receipt = Path(tmp) / "access.json"
            with mock.patch.object(
                package_builder,
                "discover_existing_infrastructure",
                return_value=(
                    {
                        "account_id": "123456789012",
                        "caller_arn": "arn:aws:iam::123456789012:user/release",
                    },
                    builders,
                    records,
                ),
            ), mock.patch.object(
                package_builder,
                "validated_builder_security_group",
                return_value=("sg-0aaabbbcccdddeee1", group),
            ), mock.patch.object(package_builder, "aws_no_output") as mutate:
                opened = package_builder.open_ci_access(
                    region="us-east-1",
                    ssh_key=key,
                    ssh_cidr="203.0.113.10/32",
                    owner_token=self.owner_token,
                    receipt=receipt,
                )
            self.assertTrue(opened["rule_added"])
            self.assertEqual(
                opened["schema_revision"],
                package_builder.CI_ACCESS_SCHEMA_REVISION,
            )
            self.assertEqual(
                opened["caller_arn"],
                "arn:aws:iam::123456789012:user/release",
            )
            self.assertFalse(any(
                value["stop_owned"] for value in opened["builders"].values()
            ))
            self.assertTrue(receipt.is_file())
            mutate.assert_called_once()

            group["IpPermissions"] = [
                {
                    "IpProtocol": "tcp",
                    "FromPort": 22,
                    "ToPort": 22,
                    "IpRanges": [
                        {
                            "CidrIp": "203.0.113.10/32",
                            "Description": self.owner_description,
                        }
                    ],
                }
            ]
            with mock.patch.object(
                package_builder,
                "aws_preflight",
                return_value={
                    "Account": "123456789012",
                    "Arn": "arn:aws:iam::123456789012:user/release",
                },
            ), mock.patch.object(
                package_builder,
                "aws_json",
                return_value={"SecurityGroups": [group]},
            ), mock.patch.object(package_builder, "aws_no_output") as mutate:
                closed = package_builder.close_ci_access(
                    region="us-east-1", receipt=receipt
                )
            self.assertTrue(closed["closed"])
            mutate.assert_called_once()
            self.assertIn("revoke-security-group-ingress", mutate.call_args.args)

            wrong_identity = json.loads(receipt.read_text(encoding="utf-8"))
            wrong_identity["caller_arn"] = (
                "arn:aws:iam::123456789012:user/different-release-principal"
            )
            receipt.write_text(json.dumps(wrong_identity), encoding="utf-8")
            with mock.patch.object(
                package_builder,
                "aws_preflight",
                return_value={
                    "Account": "123456789012",
                    "Arn": "arn:aws:iam::123456789012:user/release",
                },
            ), mock.patch.object(package_builder, "aws_json") as aws, self.assertRaisesRegex(
                package_builder.PackageBuilderError,
                "different AWS credential identity",
            ):
                package_builder.close_ci_access(
                    region="us-east-1", receipt=receipt
                )
            aws.assert_not_called()

    def test_ci_ingress_retry_adopts_only_exact_ephemeral_description(self) -> None:
        records = {
            name: self.builder_record(name) for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in records.items()
        }
        group = {
            "IpPermissions": [
                {
                    "IpProtocol": "tcp",
                    "FromPort": 22,
                    "ToPort": 22,
                    "IpRanges": [
                        {
                            "CidrIp": "203.0.113.10/32",
                            "Description": self.owner_description,
                        }
                    ],
                }
            ]
        }
        with tempfile.TemporaryDirectory() as tmp:
            key = Path(tmp) / "key"
            key.write_text("private", encoding="ascii")
            key.with_name("key.pub").write_text(
                "ssh-ed25519 AAAA release-test\n", encoding="ascii"
            )
            receipt = Path(tmp) / "access.json"
            with mock.patch.object(
                package_builder,
                "discover_existing_infrastructure",
                return_value=(
                    {
                        "account_id": "123456789012",
                        "caller_arn": "arn:aws:iam::123456789012:user/release",
                    },
                    builders,
                    records,
                ),
            ), mock.patch.object(
                package_builder,
                "validated_builder_security_group",
                return_value=("sg-0aaabbbcccdddeee1", group),
            ), mock.patch.object(package_builder, "aws_no_output") as authorize:
                opened = package_builder.open_ci_access(
                    region="us-east-1",
                    ssh_key=key,
                    ssh_cidr="203.0.113.10/32",
                    owner_token=self.owner_token,
                    receipt=receipt,
                )
        self.assertTrue(opened["rule_added"])
        authorize.assert_not_called()

        with tempfile.TemporaryDirectory() as tmp:
            key = Path(tmp) / "key"
            key.write_text("private", encoding="ascii")
            key.with_name("key.pub").write_text(
                "ssh-ed25519 AAAA release-test\n", encoding="ascii"
            )
            receipt = Path(tmp) / "access.json"
            with mock.patch.object(
                package_builder,
                "discover_existing_infrastructure",
                return_value=(
                    {
                        "account_id": "123456789012",
                        "caller_arn": "arn:aws:iam::123456789012:user/release",
                    },
                    builders,
                    records,
                ),
            ), mock.patch.object(
                package_builder,
                "validated_builder_security_group",
                return_value=("sg-0aaabbbcccdddeee1", group),
            ), mock.patch.object(
                package_builder, "aws_no_output"
            ) as mutate, self.assertRaisesRegex(
                package_builder.PackageBuilderError, "another owner token"
            ):
                package_builder.open_ci_access(
                    region="us-east-1",
                    ssh_key=key,
                    ssh_cidr="203.0.113.10/32",
                    owner_token="bioseq/repository:other-run",
                    receipt=receipt,
                )
            mutate.assert_not_called()

    def test_ci_ingress_does_not_adopt_or_revoke_non_ephemeral_rule(self) -> None:
        records = {
            name: self.builder_record(name) for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in records.items()
        }
        group = {
            "Tags": [
                {"Key": key, "Value": value}
                for key, value in {
                    "Project": "bioseq",
                    "ManagedBy": "opentofu",
                    "Component": "package-build-machine",
                    "Name": "bioseq-package-builder-sg",
                }.items()
            ],
            "IpPermissions": [
                {
                    "IpProtocol": "tcp",
                    "FromPort": 22,
                    "ToPort": 22,
                    "IpRanges": [
                        {
                            "CidrIp": "203.0.113.10/32",
                            "Description": "operator-managed SSH",
                        }
                    ],
                }
            ],
        }
        with tempfile.TemporaryDirectory() as tmp:
            key = Path(tmp) / "key"
            key.write_text("private", encoding="ascii")
            key.with_name("key.pub").write_text(
                "ssh-ed25519 AAAA release-test\n", encoding="ascii"
            )
            receipt = Path(tmp) / "access.json"
            with mock.patch.object(
                package_builder,
                "discover_existing_infrastructure",
                return_value=(
                    {
                        "account_id": "123456789012",
                        "caller_arn": "arn:aws:iam::123456789012:user/release",
                    },
                    builders,
                    records,
                ),
            ), mock.patch.object(
                package_builder,
                "validated_builder_security_group",
                return_value=("sg-0aaabbbcccdddeee1", group),
            ), mock.patch.object(package_builder, "aws_no_output") as authorize:
                opened = package_builder.open_ci_access(
                    region="us-east-1",
                    ssh_key=key,
                    ssh_cidr="203.0.113.10/32",
                    owner_token=self.owner_token,
                    receipt=receipt,
                )
            self.assertFalse(opened["rule_added"])
            authorize.assert_not_called()

            with mock.patch.object(
                package_builder,
                "aws_preflight",
                return_value={
                    "Account": "123456789012",
                    "Arn": "arn:aws:iam::123456789012:user/release",
                },
            ), mock.patch.object(
                package_builder,
                "aws_json",
                return_value={"SecurityGroups": [group]},
            ), mock.patch.object(package_builder, "aws_no_output") as revoke:
                closed = package_builder.close_ci_access(
                    region="us-east-1", receipt=receipt
                )
            self.assertTrue(closed["closed"])
            revoke.assert_not_called()

    def test_ci_ingress_same_receipt_retry_preserves_lifecycle_ownership(self) -> None:
        records = {
            name: self.builder_record(name) for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in records.items()
        }
        group = {
            "IpPermissions": [
                {
                    "IpProtocol": "tcp",
                    "FromPort": 22,
                    "ToPort": 22,
                    "IpRanges": [
                        {
                            "CidrIp": "203.0.113.10/32",
                            "Description": self.owner_description,
                        }
                    ],
                }
            ]
        }
        payload = {
            "schema_revision": package_builder.CI_ACCESS_SCHEMA_REVISION,
            "account": "123456789012",
            "caller_arn": "arn:aws:iam::123456789012:user/release",
            "region": "us-east-1",
            "security_group_id": "sg-0aaabbbcccdddeee1",
            "cidr": "203.0.113.10/32",
            "owner_token_sha256": self.owner_token_sha256,
            "description": self.owner_description,
            "rule_added": True,
            "closed": False,
            "builders": {
                name: {
                    "instance_id": builder.instance_id,
                    "initial_state": "stopped",
                    "stop_owned": name == "x64",
                }
                for name, builder in builders.items()
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            key = Path(tmp) / "key"
            key.write_text("private", encoding="ascii")
            key.with_name("key.pub").write_text(
                "ssh-ed25519 AAAA release-test\n", encoding="ascii"
            )
            receipt = Path(tmp) / "access.json"
            package_builder.write_json_atomic(receipt, payload)
            with mock.patch.object(
                package_builder,
                "discover_existing_infrastructure",
                return_value=(
                    {
                        "account_id": payload["account"],
                        "caller_arn": payload["caller_arn"],
                    },
                    builders,
                    records,
                ),
            ), mock.patch.object(
                package_builder,
                "validated_builder_security_group",
                return_value=(payload["security_group_id"], group),
            ), mock.patch.object(package_builder, "aws_no_output") as mutate:
                retried = package_builder.open_ci_access(
                    region="us-east-1",
                    ssh_key=key,
                    ssh_cidr=payload["cidr"],
                    owner_token=self.owner_token,
                    receipt=receipt,
                )
            mutate.assert_not_called()
            group["IpPermissions"] = []
            with mock.patch.object(
                package_builder,
                "discover_existing_infrastructure",
                return_value=(
                    {
                        "account_id": payload["account"],
                        "caller_arn": payload["caller_arn"],
                    },
                    builders,
                    records,
                ),
            ), mock.patch.object(
                package_builder,
                "validated_builder_security_group",
                return_value=(payload["security_group_id"], group),
            ), mock.patch.object(package_builder, "aws_no_output") as authorize:
                resumed = package_builder.open_ci_access(
                    region="us-east-1",
                    ssh_key=key,
                    ssh_cidr=payload["cidr"],
                    owner_token=self.owner_token,
                    receipt=receipt,
                )
        self.assertTrue(retried["builders"]["x64"]["stop_owned"])
        self.assertFalse(retried["builders"]["arm64"]["stop_owned"])
        self.assertTrue(resumed["builders"]["x64"]["stop_owned"])
        authorize.assert_called_once()
        self.assertIn(
            "authorize-security-group-ingress", authorize.call_args.args
        )

    def test_owner_cleanup_revokes_only_exact_description_and_is_idempotent(self) -> None:
        records = {
            name: self.builder_record(name) for name in self.matrix.architectures
        }
        builders = {
            name: package_builder.Builder(
                name,
                record["InstanceId"],
                record["InstanceType"],
                record["Architecture"],
                record["PublicIpAddress"],
                record["PrivateIpAddress"],
                record["ImageId"],
            )
            for name, record in records.items()
        }
        rules = [
            {
                "SecurityGroupRuleId": "sgr-0aaabbbcccdddeee1",
                "GroupId": "sg-0aaabbbcccdddeee1",
                "IsEgress": False,
                "IpProtocol": "tcp",
                "FromPort": 22,
                "ToPort": 22,
                "CidrIpv4": "203.0.113.10/32",
                "Description": self.owner_description,
            },
            {
                "SecurityGroupRuleId": "sgr-0123456789abcdef0",
                "GroupId": "sg-0aaabbbcccdddeee1",
                "IsEgress": False,
                "IpProtocol": "tcp",
                "FromPort": 22,
                "ToPort": 22,
                "CidrIpv4": "203.0.113.11/32",
                "Description": "operator-managed SSH",
            },
        ]
        with mock.patch.object(
            package_builder,
            "aws_preflight",
            return_value={"Account": "123456789012", "Arn": "arn:ci"},
        ), mock.patch.object(
            package_builder,
            "discover_existing_builders",
            return_value=(builders, records),
        ), mock.patch.object(
            package_builder,
            "validated_builder_security_group",
            return_value=("sg-0aaabbbcccdddeee1", {}),
        ), mock.patch.object(
            package_builder,
            "aws_json",
            side_effect=(
                {"SecurityGroupRules": rules},
                {"SecurityGroupRules": [rules[1]]},
            ),
        ), mock.patch.object(package_builder, "aws_no_output") as revoke:
            first = package_builder.close_ci_access_owner(
                region="us-east-1", owner_token=self.owner_token
            )
            second = package_builder.close_ci_access_owner(
                region="us-east-1", owner_token=self.owner_token
            )
        self.assertEqual(
            first["revoked_rule_ids"], ["sgr-0aaabbbcccdddeee1"]
        )
        self.assertEqual(second["revoked_rule_ids"], [])
        revoke.assert_called_once()
        self.assertIn("--security-group-rule-ids", revoke.call_args.args)
        self.assertNotIn("203.0.113.11/32", revoke.call_args.args)

    def test_collection_enumerates_ten_exact_packages_and_four_json_receipts(self) -> None:
        revision = "a" * 40
        lock_sha256 = "b" * 64
        run_id = f"20260815T120000Z-{revision[:12]}"
        manifests: dict[str, dict] = {}
        uploads_by_architecture: dict[str, list[dict]] = {}
        for architecture in self.matrix.architectures:
            cells = []
            uploads = []
            for distro in self.matrix.distributions:
                suffix = package_builder.expected_artifact_suffix(
                    self.matrix.distributions[distro].package_format
                )
                filename = f"bioseq-postgresql17-4.2.3-{architecture}{suffix}"
                upload = {
                    "published": True,
                    "bucket": "bucket",
                    "key": f"bioseq/packages/{architecture}/postgresql17/{distro}/{filename}",
                    "version_id": f"version-{architecture}-{distro}",
                    "etag": f"etag-{architecture}-{distro}",
                    "server_side_encryption": "AES256",
                    "size": 1,
                    "sha256": hashlib.sha256(b"x").hexdigest(),
                }
                uploads.append(upload)
                cells.append(
                    {
                        "postgres_major": 17,
                        "distro": distro,
                        "artifact": {"filename": filename},
                        "upload": upload,
                    }
                )
            manifests[architecture] = {
                "schema_revision": package_builder.PACKAGE_MANIFEST_SCHEMA_REVISION,
                "package_version": package_builder.EXPECTED_PACKAGE_VERSION,
                "architecture": architecture,
                "compiler_optimization": compiler_optimization_evidence(
                    architecture
                ),
                "status": "success",
                "started_at": "2026-08-15T12:00:00+00:00",
                "completed_at": "2026-08-15T12:01:00+00:00",
                "requested": {
                    "postgres": list(self.matrix.postgresql_majors),
                    "distros": list(self.matrix.distributions),
                },
                "s3": {
                    "bucket": "bucket",
                    "region": "us-east-1",
                    "base_prefix": "bioseq/packages",
                },
                "signing": {
                    "fingerprint": package_builder.EXPECTED_SIGNING_FINGERPRINT,
                },
                "source_revision": revision,
                "poetry_lock_sha256": lock_sha256,
                "python_environment": {"python": "3.12.8", "poetry": "1.8.5"},
                "cells": cells,
                "errors": [],
            }
            uploads_by_architecture[architecture] = uploads
        result = {
            "schema_revision": package_builder.PACKAGE_RUN_SCHEMA_REVISION,
            "status": "success",
            "run_id": run_id,
            "revision": revision,
            "source_revision": revision,
            "poetry_lock_sha256": lock_sha256,
            "worker_generation": package_builder.WORKER_GENERATION,
            "worker_generation_revision": package_builder.WORKER_GENERATION_REVISION,
            "completed_at": "2026-08-15T12:01:00+00:00",
            "selection": {
                "architectures": list(self.matrix.architectures),
                "distributions": list(self.matrix.distributions),
                "postgresql_majors": list(self.matrix.postgresql_majors),
            },
            "bucket": "bucket",
            "s3_prefix": "bioseq/packages",
            "artifact_count": 10,
            "python_environments": {
                architecture: {
                    "venv_path": (
                        f"{package_builder.REMOTE_POETRY_VENV_ROOT}/{lock_sha256}"
                    ),
                    "python": "3.12.8",
                    "poetry": "1.8.5",
                }
                for architecture in self.matrix.architectures
            },
            "worker_units": {},
            "worker_launch_acknowledged": {},
            "manifests": manifests,
            "optimization": package_builder.package_run_optimization_receipt(
                manifests
            ),
            "builders_left_running": {},
        }

        def validate(_manifest, *, architecture, **_kwargs):
            return uploads_by_architecture[architecture]

        def download(*, destination, **_kwargs):
            destination.write_bytes(b"x")

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run_result = root / "run.json"
            run_result.write_text(json.dumps(result), encoding="utf-8")
            output = root / "assets"
            with mock.patch.object(
                package_builder,
                "_result_poetry_environment",
                return_value=mock.sentinel.environment,
            ), mock.patch.object(
                package_builder, "validate_manifest", side_effect=validate
            ), mock.patch.object(package_builder, "verify_s3_uploads") as verify, mock.patch.object(
                package_builder, "download_exact_package", side_effect=download
            ) as download_mock:
                collection = package_builder.collect_release_assets(
                    region="us-east-1",
                    run_result_path=run_result,
                    output_dir=output,
                    expected_revision=revision,
                )
            self.assertEqual(len(collection["assets"]), 10)
            self.assertEqual(download_mock.call_count, 10)
            verify.assert_called_once()
            self.assertEqual(len(list(output.glob("*.json"))), 4)
            self.assertEqual(len(list(output.iterdir())), 15)
            self.assertTrue((output / "SHA256SUMS").is_file())
            release_identity = f"v4.2.3-{revision[:12]}"
            self.assertTrue(
                (output / f"package-run-{release_identity}.json").is_file()
            )
            self.assertFalse(any(run_id in path.name for path in output.iterdir()))
            with mock.patch.object(
                package_builder, "validate_manifest", side_effect=validate
            ) as offline_validate:
                verified = package_builder.verify_release_asset_directory(
                    asset_dir=output, expected_revision=revision
                )
            self.assertEqual(verified["package_count"], 10)
            self.assertEqual(verified["asset_count"], 15)
            self.assertEqual(offline_validate.call_count, 2)
            with self.assertRaisesRegex(
                package_builder.PackageBuilderError, "was not uploaded"
            ):
                package_builder.verify_release_asset_directory(
                    asset_dir=output, expected_revision=revision
                )

            collection_path = output / f"package-assets-{release_identity}.json"
            original_collection = collection_path.read_text(encoding="utf-8")
            stale_collection = json.loads(original_collection)
            stale_collection["assets"][0]["s3"]["etag"] = "stale-etag"
            collection_path.write_text(
                json.dumps(stale_collection, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            with mock.patch.object(
                package_builder, "validate_manifest", side_effect=validate
            ), self.assertRaisesRegex(
                package_builder.PackageBuilderError, "cross-linked"
            ):
                package_builder.verify_release_asset_directory(
                    asset_dir=output, expected_revision=revision
                )
            collection_path.write_text(original_collection, encoding="utf-8")

            next(iter(output.glob("*.deb"))).write_bytes(b"tampered")
            with mock.patch.object(
                package_builder, "validate_manifest", side_effect=validate
            ), self.assertRaisesRegex(
                package_builder.PackageBuilderError,
                "package bytes do not match receipt",
            ):
                package_builder.verify_release_asset_directory(
                    asset_dir=output, expected_revision=revision
                )

    def test_parser_exposes_release_automation_commands(self) -> None:
        parser = package_builder.build_parser()
        controller_options = [
            "--existing-only",
            "--ssh-key",
            "key",
            "--access-receipt",
            "access.json",
            "--receipt",
            "controller.json",
            "--source-revision",
            "a" * 40,
            "--owner-token",
            "bioseq/repository:package-release:" + "a" * 40,
        ]
        for argv, command in (
            (["matrix", "--require-complete"], "matrix"),
            (["inventory"], "inventory"),
            (["stop"], "stop"),
            (["launch-run", *controller_options, "--optimization", "O3"], "launch-run"),
            (["attach-run", *controller_options, "--wait-seconds", "1"], "attach-run"),
            (
                ["adopt-run", *controller_options, "--run-id", "20260817T123305Z-aaaaaaaaaaaa"],
                "adopt-run",
            ),
            (["finalize-run", *controller_options], "finalize-run"),
            (
                [
                    "cleanup-run",
                    "--if-present",
                    "--receipt",
                    "controller.json",
                    "--source-revision",
                    "a" * 40,
                    "--owner-token",
                    "bioseq/repository:package-release:" + "a" * 40,
                    "--evidence",
                    "cleanup.json",
                ],
                "cleanup-run",
            ),
            (
                [
                    "close-ci-access-owner",
                    "--owner-token",
                    "bioseq/repository:1:poll_1:1",
                ],
                "close-ci-access-owner",
            ),
            (
                [
                    "collect",
                    "--run-result",
                    "run.json",
                    "--output-dir",
                    "assets",
                    "--expected-revision",
                    "a" * 40,
                ],
                "collect",
            ),
            (
                [
                    "verify-release-assets",
                    "--asset-dir",
                    "assets",
                    "--expected-revision",
                    "a" * 40,
                ],
                "verify-release-assets",
            ),
        ):
            self.assertEqual(parser.parse_args(argv).command, command)


class PackageReleaseWorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = WORKFLOW_PATH.read_text(encoding="utf-8")

    def parsed_trigger_contract(self) -> tuple[set[str], list[str]]:
        """Parse the small workflow trigger mapping without a YAML dependency."""
        lines = self.workflow.splitlines()
        trigger_start = lines.index('"on":')
        trigger_lines: list[str] = []
        for line in lines[trigger_start + 1:]:
            if line and not line.startswith("  "):
                break
            trigger_lines.append(line)
        events = {
            match.group(1)
            for line in trigger_lines
            if (match := re.fullmatch(r"  ([a-z_]+):", line)) is not None
        }
        push_start = trigger_lines.index("  push:")
        push_lines: list[str] = []
        for line in trigger_lines[push_start + 1:]:
            if re.fullmatch(r"  [a-z_]+:", line):
                break
            push_lines.append(line)
        branches = [
            match.group(1)
            for line in push_lines
            if (match := re.fullmatch(r"      - ([A-Za-z0-9_./-]+)", line))
            is not None
        ]
        return events, branches

    def job_condition(self) -> str:
        lines = self.workflow.splitlines()
        start = lines.index("    if: >-")
        body: list[str] = []
        for line in lines[start + 1:]:
            if line and not line.startswith("      "):
                break
            if line:
                body.append(line.strip())
        return " ".join(body)

    def evaluate_job_condition(
        self, *, event_name: str, ref: str, head_message: str = ""
    ) -> bool:
        """Evaluate the workflow's deliberately narrow GitHub expression."""
        expression = self.job_condition()
        expression = re.sub(
            r"github\.event_name == '([^']+)'",
            lambda match: str(event_name == match.group(1)),
            expression,
        )
        expression = re.sub(
            r"github\.ref == '([^']+)'",
            lambda match: str(ref == match.group(1)),
            expression,
        )
        expression = re.sub(
            r"contains\(github\.event\.head_commit\.message, '([^']+)'\)",
            lambda match: str(
                match.group(1).casefold() in head_message.casefold()
            ),
            expression,
        )
        expression = expression.replace("&&", " and ").replace("||", " or ")
        compact = re.sub(r"\s+", "", expression)
        tokens = re.findall(r"True|False|and|or|\(|\)", compact)
        self.assertEqual("".join(tokens), compact)
        return bool(eval(expression, {"__builtins__": {}}, {}))

    def step_script(self, name: str) -> str:
        lines = self.workflow.splitlines()
        start = lines.index(f"      - name: {name}")
        run = next(
            index
            for index in range(start + 1, len(lines))
            if lines[index] == "        run: |"
        )
        body: list[str] = []
        for line in lines[run + 1:]:
            if line.startswith("      - name:"):
                break
            if line and not line.startswith("          "):
                break
            body.append(line[10:] if line else "")
        return "\n".join(body) + "\n"

    @staticmethod
    def write_fake_gh(path: Path) -> None:
        script = f"""#!{sys.executable}
import hashlib
import json
import os
from pathlib import Path
import sys

state_path = Path(os.environ["GH_STATE"])
state = json.loads(state_path.read_text(encoding="utf-8"))
args = sys.argv[1:]

def save():
    state_path.write_text(json.dumps(state, sort_keys=True), encoding="utf-8")

def record(value):
    state.setdefault("calls", []).append(value)
    save()

def repository_path():
    for value in args[1:]:
        if value.startswith("repos/"):
            return value
    raise SystemExit("missing repository API path")

if args[0] == "api":
    path = repository_path()
    if "/git/matching-refs/tags/" in path:
        requested_tag = path.rsplit("/", 1)[-1]
        if requested_tag == state["tag"]:
            print(state["tag_target"])
    elif "--method" in args and args[args.index("--method") + 1] == "DELETE":
        asset_id = int(path.rsplit("/", 1)[-1])
        state["assets"] = [
            asset for asset in state["assets"] if asset["id"] != asset_id
        ]
        record(f"delete:{{asset_id}}")
    elif "/assets?per_page=" in path:
        for asset in state["assets"]:
            print(asset["id"])
    elif "/releases/tags/" in path:
        query = args[args.index("--jq") + 1]
        if "select(.draft == true)" in query:
            if state["draft"]:
                print(state["release_id"])
        elif ".assets[]" in query:
            for asset in state["assets"]:
                print(f"{{asset['name']}}\\t{{asset['digest']}}")
        else:
            raise SystemExit(f"unsupported release API query: {{query}}")
    else:
        raise SystemExit(f"unsupported API path: {{path}}")
elif args[:2] == ["release", "view"]:
    if not state["exists"]:
        raise SystemExit(1)
    if "--json" in args:
        query = args[args.index("--jq") + 1]
        if query == ".isDraft":
            print("true" if state["draft"] else "false")
        elif query == "[.isDraft, .isPrerelease, .name] | @tsv":
            print(
                f"{{str(state['draft']).lower()}}\t"
                f"{{str(state['prerelease']).lower()}}\t{{state['name']}}"
            )
        elif query == ".assets | length":
            print(len(state["assets"]))
        else:
            raise SystemExit(f"unsupported release view query: {{query}}")
elif args[:2] == ["release", "upload"]:
    repo_index = args.index("--repo")
    files = [Path(value) for value in args[3:repo_index]]
    uploaded = []
    next_id = int(state.get("next_id", 1000))
    for source in files:
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        uploaded.append(
            {{"id": next_id, "name": source.name, "digest": "sha256:" + digest}}
        )
        next_id += 1
    if state.get("corrupt_upload") and uploaded:
        uploaded[0]["digest"] = "sha256:" + "0" * 64
    state["assets"] = uploaded
    state["next_id"] = next_id
    record("upload")
elif args[:2] == ["release", "edit"]:
    if "--draft=false" not in args:
        raise SystemExit("release edit did not publish")
    state["draft"] = False
    record("publish")
elif args[:2] == ["release", "create"]:
    state["exists"] = True
    state["draft"] = True
    state["prerelease"] = "--prerelease" in args
    state["tag"] = args[2]
    state["name"] = args[args.index("--title") + 1]
    state["tag_target"] = os.environ["GITHUB_SHA"]
    record("create")
else:
    raise SystemExit(f"unsupported gh invocation: {{args}}")

save()
"""
        path.write_text(textwrap.dedent(script), encoding="utf-8")
        path.chmod(0o755)

    def run_release_step(
        self,
        *,
        draft: bool,
        corrupt_upload: bool = False,
        exists: bool = True,
        expected_prerelease: bool = True,
        release_prerelease: bool | None = None,
        release_name: str | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], dict[str, object], set[str]]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            assets = root / "release-assets"
            assets.mkdir()
            expected_names = set()
            for index in range(15):
                path = assets / f"asset-{index:02d}.bin"
                path.write_bytes(f"package-{index}\n".encode("ascii"))
                expected_names.add(path.name)
            binary = root / "bin"
            binary.mkdir()
            self.write_fake_gh(binary / "gh")
            fake_python = binary / "python3"
            fake_python.write_text("#!/bin/bash\nexit 0\n", encoding="utf-8")
            fake_python.chmod(0o755)
            revision = "a" * 40
            short_revision = revision[:12]
            expected_title = (
                "Bioseq native packages "
                f"{package_builder.EXPECTED_PACKAGE_VERSION} ({short_revision})"
            )
            if expected_prerelease:
                expected_title += " [refactor validation]"
            if release_prerelease is None:
                release_prerelease = expected_prerelease
            if release_name is None:
                release_name = expected_title
            release_tag_suffix = (
                "-refactor-validation" if expected_prerelease else ""
            )
            expected_tag = (
                "bioseq-packages-v"
                f"{package_builder.EXPECTED_PACKAGE_VERSION}-{short_revision}"
                f"{release_tag_suffix}"
            )
            state_path = root / "gh-state.json"
            state_path.write_text(
                json.dumps(
                    {
                        "exists": exists,
                        "draft": draft,
                        "prerelease": release_prerelease,
                        "tag": expected_tag,
                        "name": release_name,
                        "tag_target": revision if exists else "",
                        "release_id": 77,
                        "assets": (
                            [
                                {
                                    "id": 9,
                                    "name": "stale-extra.bin",
                                    "digest": "sha256:" + "f" * 64,
                                }
                            ]
                            if exists
                            else []
                        ),
                        "next_id": 100,
                        "corrupt_upload": corrupt_upload,
                        "calls": [],
                    }
                ),
                encoding="utf-8",
            )
            environment = {
                **os.environ,
                "PATH": f"{binary}:{os.environ['PATH']}",
                "RUNNER_TEMP": str(root),
                "PACKAGE_VERSION": package_builder.EXPECTED_PACKAGE_VERSION,
                "GITHUB_SHA": revision,
                "GITHUB_REF": (
                    "refs/heads/refactor/start-massive-refactor"
                    if expected_prerelease
                    else "refs/heads/main"
                ),
                "GITHUB_REPOSITORY": "bioseq/repository",
                "GH_STATE": str(state_path),
                "RELEASE_PRERELEASE": str(expected_prerelease).lower(),
                "RELEASE_TAG_SUFFIX": release_tag_suffix,
                "RELEASE_TITLE": expected_title,
            }
            completed = subprocess.run(
                [
                    "bash",
                    "-c",
                    self.step_script("Create deterministic GitHub release"),
                ],
                cwd=WORKFLOW_PATH.parents[2],
                env=environment,
                capture_output=True,
                text=True,
            )
            state = json.loads(state_path.read_text(encoding="utf-8"))
            return completed, state, expected_names

    def test_workflow_trigger_contract_gates_the_paid_refactor_lane(self) -> None:
        events, branches = self.parsed_trigger_contract()
        self.assertEqual(events, {"push", "workflow_dispatch"})
        self.assertEqual(
            branches, ["main", "refactor/start-massive-refactor"]
        )
        cases = (
            ("push", "refs/heads/main", "ordinary", True),
            (
                "push",
                "refs/heads/refactor/start-massive-refactor",
                "ordinary refactor commit",
                False,
            ),
            (
                "push",
                "refs/heads/refactor/start-massive-refactor",
                "Validate native packages [package-release]",
                True,
            ),
            (
                "push",
                "refs/heads/refactor/start-massive-refactor",
                "GitHub matches [PACKAGE-RELEASE] without case sensitivity",
                True,
            ),
            ("workflow_dispatch", "refs/heads/main", "", True),
            (
                "workflow_dispatch",
                "refs/heads/refactor/start-massive-refactor",
                "",
                True,
            ),
            ("workflow_dispatch", "refs/heads/unrelated", "", False),
            ("pull_request", "refs/heads/main", "[package-release]", False),
        )
        for event_name, ref, head_message, expected in cases:
            with self.subTest(event_name=event_name, ref=ref, message=head_message):
                self.assertEqual(
                    self.evaluate_job_condition(
                        event_name=event_name,
                        ref=ref,
                        head_message=head_message,
                    ),
                    expected,
                )

    def test_workflow_is_globally_serial_and_exact_revision_bound(self) -> None:
        self.assertIn("group: bioseq-native-package-release", self.workflow)
        self.assertIn("cancel-in-progress: false", self.workflow)
        job_timeouts = [
            int(value)
            for value in re.findall(
                r"^    timeout-minutes: (\d+)$", self.workflow, re.MULTILINE
            )
        ]
        self.assertTrue(job_timeouts)
        self.assertLess(max(job_timeouts), 360)
        self.assertEqual(self.workflow.count("--wait-seconds 15600"), 4)
        self.assertIn("poll_1:", self.workflow)
        self.assertIn("poll_4:", self.workflow)
        self.assertIn("wait_budget_exhausted:", self.workflow)
        self.assertIn("refusing unbounded recursion", self.workflow)
        self.assertIn("timeout-minutes: 25", self.workflow)
        self.assertIn("ref: ${{ github.sha }}", self.workflow)
        self.assertIn('test "$(git rev-parse HEAD)" = "$GITHUB_SHA"', self.workflow)

    def test_cleanup_step_budgets_dominate_every_internal_wait(self) -> None:
        def timeout_minutes(step_name: str) -> int:
            lines = self.workflow.splitlines()
            start = lines.index(f"      - name: {step_name}")
            value = next(
                line.strip()
                for line in lines[start + 1:]
                if line.strip().startswith("timeout-minutes:")
            )
            return int(value.split(":", 1)[1].strip())

        # Readiness, exact remote cancellation, quiescent poweroff, and the
        # terminal EC2-state proof are bounded phases. Architectures run in
        # parallel inside each phase.
        worst_case_seconds = 4 * package_builder.CONTROLLER_CLEANUP_TIMEOUT_SECONDS
        minimum_with_margin = (worst_case_seconds + 59) // 60 + 2
        self.assertGreaterEqual(
            timeout_minutes("Cancel only receipt-owned package work"),
            minimum_with_margin,
        )

        finalizer = self.workflow.split("  lifecycle_finalizer:", 1)[1].split(
            "\n  poll_1:", 1
        )[0]
        finalizer_timeout_match = re.search(
            r"^    timeout-minutes: (\d+)$", finalizer, re.MULTILINE
        )
        self.assertIsNotNone(finalizer_timeout_match)
        assert finalizer_timeout_match is not None
        finalizer_timeout = int(finalizer_timeout_match.group(1))
        finalizer_step_timeouts = [
            int(value)
            for value in re.findall(
                r"^        timeout-minutes: (\d+)$", finalizer, re.MULTILINE
            )
        ]
        self.assertGreaterEqual(
            finalizer_timeout,
            sum(finalizer_step_timeouts) + 5,
        )

    def test_workflow_uses_only_secret_backed_standard_credential_env(self) -> None:
        access_mapping = "AWS_ACCESS_KEY_ID: ${{ secrets.AWS_ACCESS_KEY }}"
        secret_mapping = "AWS_SECRET_ACCESS_KEY: ${{ secrets.AWS_SECRET_KEY }}"
        self.assertEqual(self.workflow.count(access_mapping), 9)
        self.assertEqual(self.workflow.count(secret_mapping), 9)
        global_environment = self.workflow.split("jobs:", 1)[0]
        self.assertNotIn(access_mapping, global_environment)
        self.assertNotIn(secret_mapping, global_environment)
        self.assertEqual(self.workflow.count("${{ secrets.GH_PAT }}"), 2)
        self.assertIn("GH_TOKEN: ${{ secrets.GH_PAT }}", self.workflow)
        self.assertNotIn("aws configure", self.workflow)

    def test_workflow_reuses_full_matrix_and_has_cost_cleanup(self) -> None:
        self.assertIn("package_builder.py matrix --require-complete", self.workflow)
        self.assertEqual(self.workflow.count("package_builder.py launch-run"), 1)
        self.assertEqual(self.workflow.count("package_builder.py attach-run"), 4)
        self.assertEqual(self.workflow.count("package_builder.py finalize-run"), 1)
        self.assertNotIn("package_builder.py run \\", self.workflow)
        self.assertIn("--existing-only", self.workflow)
        self.assertIn(
            '--owner-token "$GITHUB_REPOSITORY:package-release:$GITHUB_SHA"',
            self.workflow,
        )
        self.assertEqual(self.workflow.count("--optimization O3"), 1)
        self.assertNotIn("--architectures", self.workflow)
        self.assertNotIn("--distros", self.workflow)
        self.assertNotIn("--postgres", self.workflow)
        self.assertNotIn("package_builder.py stop", self.workflow)
        self.assertEqual(self.workflow.count("package_builder.py cleanup-run"), 1)
        self.assertEqual(
            self.workflow.count("package_builder.py close-ci-access-owner"), 1
        )
        self.assertEqual(
            self.workflow.count("package_builder.py close-ci-access")
            - self.workflow.count("package_builder.py close-ci-access-owner"),
            6,
        )
        self.assertNotIn("--lifecycle-receipt", self.workflow)
        self.assertGreaterEqual(
            self.workflow.count(
                '--receipt "$RUNNER_TEMP/package-controller.json"'
            ),
            6,
        )
        self.assertIn("--controller-receipt", self.workflow)
        self.assertIn("if: always()", self.workflow)
        self.assertIn(
            'if [[ -f "$RUNNER_TEMP/package-builder-access.json" ]]',
            self.workflow,
        )
        self.assertIn("lifecycle_finalizer:", self.workflow)
        finalizer = self.workflow.split("  lifecycle_finalizer:", 1)[1].split(
            "\n  poll_1:", 1
        )[0]
        for dependency in (
            "metadata",
            "launch",
            "poll_1",
            "poll_2",
            "poll_3",
            "poll_4",
            "wait_budget_exhausted",
            "finalize_release",
        ):
            self.assertIn(f"      - {dependency}", finalizer)
        self.assertIn("always()", finalizer)
        self.assertIn("--if-present", finalizer)
        self.assertIn("package-cleanup-evidence.json", finalizer)
        for step in (
            "Revoke only launch-job access",
            "Revoke only poll-1 access",
            "Revoke only poll-2 access",
            "Revoke only poll-3 access",
            "Revoke only poll-4 access",
            "Revoke only finalizer-job access (always)",
        ):
            self.assertNotIn("package_builder.py stop", self.step_script(step))

    def test_access_finalizer_attempts_every_owner_after_individual_failure(self) -> None:
        step = "Revoke every workflow-owned access rule independently"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = root / "bin"
            binary.mkdir()
            fake_python = binary / "python3"
            fake_python.write_text(
                "#!/bin/bash\n"
                "printf '%s\\n' \"$*\" >> \"$CALL_LOG\"\n"
                "if [[ \"$*\" == *\":poll_2:\"* ]]; then exit 17; fi\n",
                encoding="utf-8",
            )
            fake_python.chmod(0o755)
            completed = subprocess.run(
                ["bash", "-c", self.step_script(step)],
                cwd=WORKFLOW_PATH.parents[2],
                env={
                    **os.environ,
                    "PATH": f"{binary}:{os.environ['PATH']}",
                    "RUNNER_TEMP": str(root),
                    "AWS_REGION": "us-east-1",
                    "CALL_LOG": str(root / "calls"),
                    "GITHUB_REPOSITORY": "bioseq/repository",
                    "GITHUB_RUN_ID": "55",
                    "GITHUB_RUN_ATTEMPT": "2",
                },
                capture_output=True,
                text=True,
            )
            self.assertEqual(completed.returncode, 17, completed.stderr)
            calls = (root / "calls").read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(calls), 12)
            self.assertTrue(all("close-ci-access-owner" in call for call in calls))
            self.assertIn("bioseq/repository:55:launch:1", calls[0])
            self.assertIn("bioseq/repository:55:finalize_release:2", calls[-1])

    def test_receipt_cleanup_is_not_gated_on_collection_or_prior_job_success(self) -> None:
        script = self.step_script("Cancel only receipt-owned package work")
        self.assertIn("package_builder.py cleanup-run", script)
        self.assertIn("--if-present", script)
        self.assertNotIn("PACKAGE_COLLECTION_SUCCEEDED", script)
        finalizer = self.workflow.split("  lifecycle_finalizer:", 1)[1].split(
            "\n  poll_1:", 1
        )[0]
        self.assertIn("if: always()", finalizer)
        self.assertIn("needs.metadata.result != 'skipped'", finalizer)

    def test_stale_draft_deletes_every_old_asset_and_reuploads_complete_set(self) -> None:
        completed, state, expected_names = self.run_release_step(draft=True)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertFalse(state["draft"])
        self.assertEqual(
            {asset["name"] for asset in state["assets"]}, expected_names
        )
        self.assertEqual(len(state["assets"]), 15)
        self.assertEqual(state["calls"], ["delete:9", "upload", "publish"])

    def test_new_release_uses_the_ref_specific_prerelease_contract(self) -> None:
        for expected_prerelease in (False, True):
            with self.subTest(prerelease=expected_prerelease):
                completed, state, expected_names = self.run_release_step(
                    draft=True,
                    exists=False,
                    expected_prerelease=expected_prerelease,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertFalse(state["draft"])
                self.assertEqual(state["prerelease"], expected_prerelease)
                expected_suffix = (
                    "-refactor-validation" if expected_prerelease else ""
                )
                self.assertEqual(
                    state["tag"],
                    "bioseq-packages-v"
                    f"{package_builder.EXPECTED_PACKAGE_VERSION}-"
                    f"{'a' * 12}{expected_suffix}",
                )
                self.assertEqual(
                    state["name"].endswith("[refactor validation]"),
                    expected_prerelease,
                )
                self.assertEqual(
                    {asset["name"] for asset in state["assets"]},
                    expected_names,
                )
                self.assertEqual(
                    state["calls"], ["create", "upload", "publish"]
                )

    def test_foreign_or_wrong_channel_draft_refuses_all_mutation(self) -> None:
        for overrides in (
            {"release_prerelease": False},
            {"release_name": "Unrelated draft"},
        ):
            with self.subTest(overrides=overrides):
                completed, state, _expected_names = self.run_release_step(
                    draft=True, **overrides
                )
                self.assertNotEqual(completed.returncode, 0)
                self.assertTrue(state["draft"])
                self.assertEqual(
                    [asset["name"] for asset in state["assets"]],
                    ["stale-extra.bin"],
                )
                self.assertEqual(state["calls"], [])

    def test_stale_draft_digest_mismatch_remains_unpublished(self) -> None:
        completed, state, expected_names = self.run_release_step(
            draft=True, corrupt_upload=True
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertTrue(state["draft"])
        self.assertEqual(
            {asset["name"] for asset in state["assets"]}, expected_names
        )
        self.assertEqual(state["calls"], ["delete:9", "upload"])

    def test_published_release_with_stale_assets_refuses_all_mutation(self) -> None:
        completed, state, _expected_names = self.run_release_step(draft=False)
        self.assertNotEqual(completed.returncode, 0)
        self.assertFalse(state["draft"])
        self.assertEqual(
            [asset["name"] for asset in state["assets"]], ["stale-extra.bin"]
        )
        self.assertEqual(state["calls"], [])

    def test_third_party_actions_are_immutable_sha_pinned(self) -> None:
        uses = re.findall(
            r"^\s*uses:\s*([^\s#]+)(?:\s+#.*)?$", self.workflow, re.MULTILINE
        )
        self.assertGreaterEqual(len(uses), 2)
        for value in uses:
            owner_action, separator, revision = value.partition("@")
            self.assertTrue(separator)
            self.assertIn("/", owner_action)
            self.assertRegex(revision, r"^[0-9a-f]{40}$")

    def test_every_workflow_shell_block_parses(self) -> None:
        lines = self.workflow.splitlines()
        scripts: list[str] = []
        for index, line in enumerate(lines):
            if line != "        run: |":
                continue
            body: list[str] = []
            for candidate in lines[index + 1:]:
                if candidate and not candidate.startswith("          "):
                    break
                body.append(candidate[10:] if candidate else "")
            scripts.append("\n".join(body) + "\n")
        self.assertGreaterEqual(len(scripts), 10)
        for index, script in enumerate(scripts):
            with self.subTest(script=index):
                completed = subprocess.run(
                    ["bash", "-n"],
                    input=script,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_release_is_draft_until_asset_count_is_complete(self) -> None:
        tag_assignment = (
            'tag="bioseq-packages-v${PACKAGE_VERSION}-${short_sha}'
            '${RELEASE_TAG_SUFFIX}"'
        )
        self.assertEqual(self.workflow.count(tag_assignment), 2)
        self.assertIn('release_tag_suffix=""', self.workflow)
        self.assertIn(
            'release_tag_suffix="-refactor-validation"', self.workflow
        )
        self.assertIn("gh release create", self.workflow)
        self.assertIn("--draft", self.workflow)
        self.assertIn('test "$observed" = "$expected"', self.workflow)
        self.assertIn("gh release edit", self.workflow)
        self.assertIn("--draft=false", self.workflow)
        self.assertIn("[.name, .digest]", self.workflow)
        self.assertIn("sha256sum", self.workflow)
        self.assertIn("verify-release-assets", self.workflow)
        self.assertIn("stale-draft-asset-ids.txt", self.workflow)
        self.assertIn("gh api --method DELETE", self.workflow)
        self.assertIn("select(.draft == true)", self.workflow)
        self.assertIn("release_already_complete=1", self.workflow)
        self.assertIn("printf '%s\\t%s\\n'", self.workflow)
        self.assertNotIn("printf '%s\\tsha256:%s\\n'", self.workflow)
        self.assertIn("git/matching-refs/tags/$tag", self.workflow)
        self.assertIn('test "$tag_target" = "$GITHUB_SHA"', self.workflow)
        self.assertLess(
            self.workflow.index("Reuse an already published exact-commit release"),
            self.workflow.index("Open launch-job builder access"),
        )
        self.assertLess(
            self.workflow.index("git/matching-refs/tags/$tag"),
            self.workflow.index("gh release create"),
        )
        convergence = self.workflow.index("stale-draft-asset-ids.txt")
        self.assertLess(
            self.workflow.index("select(.draft == true)"), convergence
        )
        self.assertLess(convergence, self.workflow.index("gh release upload"))


if __name__ == "__main__":
    unittest.main()
