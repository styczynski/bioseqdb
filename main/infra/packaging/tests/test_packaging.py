from __future__ import annotations

import base64
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile
import unittest
from unittest import mock


PACKAGING = Path(__file__).resolve().parents[1]
SCRIPTS = PACKAGING / "scripts"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = load_module("bioseq_build_packages", PACKAGING / "build_packages.py")
smoke = load_module("bioseq_smoke_test_package", SCRIPTS / "smoke_test_package.py")
optimization = load_module(
    "bioseq_verify_compile_optimization", SCRIPTS / "verify_compile_optimization.py"
)


ARMOR = """-----BEGIN PGP PRIVATE KEY BLOCK-----
test-only
-----END PGP PRIVATE KEY BLOCK-----
"""


def valid_validation() -> str:
    return json.dumps(
        {
            "passed": True,
            "six_artifacts": True,
            "optimization_level": 0,
            "binaries": [
                {
                    "path": "/tmp/bioseq.so",
                    "elf_architecture": "Advanced Micro Devices X86-64",
                    "rpath_absent": True,
                    "compiler_runtime_safe": True,
                },
                {
                    "path": "/tmp/bioseq_cache.so",
                    "elf_architecture": "Advanced Micro Devices X86-64",
                    "rpath_absent": True,
                    "compiler_runtime_safe": True,
                },
            ],
        }
    )


def valid_smoke(distro: str, postgres_major: int) -> str:
    return json.dumps(
        {
            "passed": True,
            "package_installed": True,
            "preload_active": True,
            "bioseq_extension_created": True,
            "bioseq_cache_extension_created": True,
            "nonempty_roundtrip": True,
            "roundtrip": "ACGT",
            "distro": distro,
            "architecture": "x64",
            "postgres_major": postgres_major,
            "fresh_container": True,
        }
    )


class InputAndSigningTests(unittest.TestCase):
    def test_matrix_parsers_are_strict_and_deduplicate(self):
        self.assertEqual(runner.parse_postgres(["17,18", "17"]), [17, 18])
        self.assertEqual(
            runner.parse_distros(["debian_13,arch", "debian_13"]),
            ["debian_13", "arch"],
        )
        with self.assertRaises(runner.PackageBuildError):
            runner.parse_postgres(["16"])
        with self.assertRaises(runner.PackageBuildError):
            runner.parse_distros(["debian_12"])

    def test_ascii_armor_json_and_base64_are_normalized(self):
        encoded = base64.b64encode(ARMOR.encode()).decode()
        self.assertEqual(runner.normalize_signing_key(ARMOR), encoded)
        self.assertEqual(runner.normalize_signing_key(encoded), encoded)
        self.assertEqual(
            runner.normalize_signing_key(json.dumps({"GPG_PRIVATE_KEY": ARMOR})),
            encoded,
        )
        with self.assertRaises(runner.PackageBuildError):
            runner.normalize_signing_key("")

    def test_aws_requires_secret_arn(self):
        with self.assertRaises(runner.PackageBuildError):
            runner.fetch_signing_key(
                secret_arn=None,
                aws_bin="aws",
                region="us-east-1",
                environment={},
                is_aws=True,
            )

    def test_cell_environment_forces_fast_native_contract(self):
        environment = runner.cell_environment(
            {},
            architecture="arm64",
            bucket="valid-bucket",
            region="us-east-1",
            prefix="bioseq/packages/arm64/postgresql17",
            postgres_major=17,
            signing_key="secret",
            toolchain_cache_dir=Path("/cache/opensuse/postgresql17"),
            cmake_cache_dir=Path("/cache-cmake/opensuse/postgresql17"),
        )
        self.assertEqual(environment["BIOSEQ_OPTTIMIZATION_LEVEL"], "0")
        self.assertEqual(environment["BIOSEQ_PACKAGE_NAME"], "bioseq-postgresql17")
        self.assertEqual(environment["BIOSEQ_ARCH_CONTAINER_IMAGE"], "menci/archlinuxarm:base")
        self.assertEqual(
            environment["BIOSEQ_TOOLCHAIN_CACHE_DIR"],
            "/cache/opensuse/postgresql17",
        )
        self.assertEqual(
            environment["BIOSEQ_CMAKE_CACHE_DIR"],
            "/cache-cmake/opensuse/postgresql17",
        )

    def test_cli_path_overrides_and_main_source_default(self):
        parser = runner.argument_parser()
        arguments = parser.parse_args(
            [
                "--architecture",
                "x64",
                "--bucket",
                "valid-bucket",
                "--region",
                "us-east-1",
                "--postgres",
                "17",
                "--distros",
                "debian_13",
                "--build-root",
                "/tmp/custom-build",
                "--manifest",
                "/tmp/custom-manifest.json",
            ]
        )
        self.assertEqual(arguments.source_dir.name, "main")
        self.assertEqual(arguments.build_root, Path("/tmp/custom-build"))
        self.assertEqual(arguments.manifest, Path("/tmp/custom-manifest.json"))


class OmnipackageAndValidationTests(unittest.TestCase):
    def test_vendored_omnipackage_uses_locked_release_build(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            source = root / "main"
            vendored = root / "omnipackage-rs-master"
            source.mkdir()
            vendored.mkdir()
            (vendored / "Cargo.toml").write_text("[package]\nname='x'\nversion='0.1.0'\n")
            (vendored / "Cargo.lock").write_text("# lock\n")
            calls = []

            def fake_run(command, *, environment, cwd=None):
                calls.append((command, environment, cwd))
                output = Path(environment["CARGO_TARGET_DIR"]) / "release" / "omnipackage"
                output.parent.mkdir(parents=True)
                output.write_text("#!/bin/sh\n")
                output.chmod(0o755)

            with mock.patch.object(runner.shutil, "which", return_value="/usr/bin/cargo"), mock.patch.object(
                runner, "run_streamed", side_effect=fake_run
            ):
                resolved = runner.resolve_omnipackage(
                    None,
                    source,
                    environment={"HOME": str(root), "PATH": "/usr/bin", "GPG_PRIVATE_KEY": "nope"},
                )
            self.assertEqual(
                resolved, (root / "caches/omnipackage-target/release/omnipackage").resolve()
            )
            self.assertEqual(calls[0][0], ["/usr/bin/cargo", "build", "--release", "--locked"])
            self.assertEqual(calls[0][2], vendored)
            self.assertNotIn("GPG_PRIVATE_KEY", calls[0][1])

    def test_validation_exposes_one_canonical_elf_architecture(self):
        parsed = runner.parse_validation(valid_validation())
        self.assertEqual(parsed["elf_architecture"], "x86_64")
        self.assertTrue(parsed["rpath_absent"])
        self.assertTrue(parsed["compiler_runtime_safe"])
        malformed = json.loads(valid_validation())
        malformed["binaries"].pop()
        with self.assertRaises(runner.PackageBuildError):
            runner.parse_validation(json.dumps(malformed))

    def test_signing_fingerprint_and_signed_deb_release_are_fail_closed(self):
        fingerprint = "A1" * 20
        self.assertEqual(
            runner._parse_gpg_fingerprint(f"pub:::::::::\nfpr:::::::::{fingerprint}:\n"),
            fingerprint,
        )
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            artifact = root / "bioseq-postgresql17_4.2.3_amd64.deb"
            artifact.write_bytes(b"signed-package-bytes")
            digest = runner.sha256_file(artifact)
            release = root / "Release"
            release.write_text(
                f"Suite: stable\nSHA256:\n {digest} {artifact.stat().st_size} {artifact.name}\n"
            )
            runner._verify_deb_release_artifact(
                release, artifact, digest, artifact.stat().st_size
            )
            release.write_text(
                f"Suite: stable\nSHA256:\n {'0' * 64} {artifact.stat().st_size} {artifact.name}\n"
            )
            with self.assertRaises(runner.PackageBuildError):
                runner._verify_deb_release_artifact(
                    release, artifact, digest, artifact.stat().st_size
                )

    def test_deb_s3_key_contains_stable_component(self):
        prefix = "bioseq/packages/x64/postgresql17"
        self.assertEqual(
            runner.s3_artifact_key(prefix, "debian_13", "deb", "p.deb"),
            f"{prefix}/debian_13/stable/p.deb",
        )
        self.assertEqual(
            runner.s3_artifact_key(prefix, "fedora_44", "rpm", "p.rpm"),
            f"{prefix}/fedora_44/p.rpm",
        )

    def test_compile_database_rejects_every_nonzero_or_ambiguous_O_flag(self):
        with tempfile.TemporaryDirectory() as raw:
            database = Path(raw) / "compile_commands.json"
            database.write_text(
                json.dumps([{"file": "ok.cpp", "arguments": ["clang++", "-O0", "ok.cpp"]}])
            )
            self.assertEqual(optimization.verify(database), 1)
            for flag in (
                "-O",
                "-O1",
                "-O2",
                "-O3",
                "-O4",
                "-Og",
                "-Os",
                "-Oz",
                "-Ofast",
                "-flto",
            ):
                database.write_text(
                    json.dumps(
                        [{"file": "bad.cpp", "arguments": ["clang++", "-O0", flag, "bad.cpp"]}]
                    )
                )
                with self.subTest(flag=flag), self.assertRaises(RuntimeError):
                    optimization.verify(database)


class SmokeTests(unittest.TestCase):
    def test_fresh_container_command_is_native_and_read_only(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            source = root / "main"
            script = source / "infra/packaging/scripts/smoke_installed_package.sh"
            script.parent.mkdir(parents=True)
            script.write_text("#!/bin/bash\n")
            artifact = root / "bioseq-postgresql17_4.2.3_amd64.deb"
            artifact.write_bytes(b"package")
            command, image = smoke.docker_command(
                docker="docker",
                source_dir=source,
                artifact=artifact,
                architecture="x64",
                postgres_major=17,
                distro="debian_13",
                arch_image="archlinux:latest",
            )
        self.assertEqual(image, "debian:trixie")
        self.assertNotIn("--platform", command)
        self.assertEqual(command[:4], ["docker", "run", "--rm", "--init"])
        self.assertEqual(sum("readonly" in value for value in command), 2)

    def test_smoke_parser_requires_roundtrip_and_all_extensions(self):
        parsed = runner.parse_smoke(valid_smoke("debian_13", 17))
        self.assertEqual(parsed["roundtrip"], "ACGT")
        failed = json.loads(valid_smoke("debian_13", 17))
        failed["bioseq_cache_extension_created"] = False
        with self.assertRaises(runner.PackageBuildError):
            runner.parse_smoke(json.dumps(failed))

    def test_runner_smokes_every_cell_before_publish_and_downloaded_bytes(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            source = root / "main"
            source.mkdir()
            (source / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 4.0)\n")
            executable = root / "omnipackage"
            executable.write_text("#!/bin/sh\n")
            executable.chmod(0o755)
            build_root = root / "build"
            manifest_path = root / "result.json"
            events = []
            artifacts = {}

            def fake_streamed(command, *, environment, cwd=None):
                operation = command[1]
                major = int(environment["BIOSEQ_POSTGRES_MAJOR"])
                events.append((major, operation))
                if operation == "build":
                    output = Path(command[command.index("--build-dir") + 1]) / "output"
                    output.mkdir(parents=True, exist_ok=True)
                    artifact = output / f"bioseq-postgresql{major}_4.2.3_amd64.deb"
                    artifact.write_bytes(f"package-{major}".encode())
                    artifacts[major] = artifact

            def fake_captured(command, *, environment=None):
                rendered = " ".join(str(value) for value in command)
                if "validate_package.py" in rendered:
                    return valid_validation()
                if "smoke_test_package.py" in rendered:
                    major = int(command[command.index("--postgres-major") + 1])
                    events.append((major, "smoke"))
                    return valid_smoke("debian_13", major)
                if "get-object" in command:
                    key = command[command.index("--key") + 1]
                    major = 17 if "postgresql17" in key else 18
                    destination = Path(command[-1])
                    destination.write_bytes(artifacts[major].read_bytes())
                    return ""
                raise AssertionError(command)

            def fake_head(_aws, _bucket, key, _region, _environment):
                major = 17 if "postgresql17" in key else 18
                return {"ContentLength": artifacts[major].stat().st_size, "ETag": '"etag"'}

            signature_attestation = {
                "passed": True,
                "public_key_imported": True,
                "signing_key_fingerprint": "A" * 40,
                "package_signature_verified": True,
                "repository_signature_verified": True,
                "objects": [{"key": "test", "size": 1, "sha256": "a" * 64}],
            }

            arguments = [
                "--architecture",
                "x64",
                "--bucket",
                "valid-bucket",
                "--region",
                "us-east-1",
                "--postgres",
                "17,18",
                "--distros",
                "debian_13",
                "--source-dir",
                str(source),
                "--build-root",
                str(build_root),
                "--manifest",
                str(manifest_path),
                "--omnipackage-bin",
                str(executable),
            ]
            with mock.patch.object(runner, "native_architecture", return_value="x64"), mock.patch.object(
                runner, "fetch_signing_key", return_value="normalized-key"
            ), mock.patch.object(
                runner, "signing_key_fingerprint", return_value="A" * 40
            ), mock.patch.object(runner, "run_streamed", side_effect=fake_streamed), mock.patch.object(
                runner, "run_captured", side_effect=fake_captured
            ), mock.patch.object(runner, "s3_head", side_effect=fake_head), mock.patch.object(
                runner, "verify_published_signatures", return_value=signature_attestation
            ):
                self.assertEqual(runner.main(arguments), 0)

            self.assertEqual(
                events,
                [
                    (17, "build"),
                    (17, "smoke"),
                    (17, "publish"),
                    (17, "smoke"),
                    (18, "build"),
                    (18, "smoke"),
                    (18, "publish"),
                    (18, "smoke"),
                ],
            )
            manifest = json.loads(manifest_path.read_text())
            self.assertEqual(manifest["status"], "success")
            for cell in manifest["cells"]:
                self.assertTrue(cell["smoke"]["passed"])
                self.assertTrue(cell["upload"]["download_smoke"]["passed"])
                self.assertTrue(cell["upload"]["signature_verification"]["passed"])
                self.assertEqual(cell["artifact"]["sha256"], cell["upload"]["sha256"])


class StaticContractTests(unittest.TestCase):
    def test_directly_invoked_packaging_sources_are_executable(self):
        paths = [PACKAGING / "build_packages.py", PACKAGING / "deb/rules.liquid"]
        paths.extend(SCRIPTS.glob("*.sh"))
        paths.extend(SCRIPTS.glob("*.py"))
        for path in paths:
            with self.subTest(path=path):
                self.assertTrue(path.stat().st_mode & stat.S_IXUSR, str(path))

    def test_config_is_rerun_safe_and_covers_the_full_matrix(self):
        config = (PACKAGING / "config.yml").read_text()
        self.assertIn("retain_packages: 0", config)
        self.assertIn("optimization_level: 0", config)
        self.assertIn("container_capabilities: [SYS_NICE]", config)
        self.assertIn("${BIOSEQ_CMAKE_CACHE_DIR}: /opt/bioseq-package-cmake-cache", config)
        for distro in runner.SUPPORTED_DISTROS:
            self.assertIn(f"distro: {distro}", config)

    def test_preparation_is_pinned_and_supports_priority_capability(self):
        preparation = (SCRIPTS / "prepare_build_environment.sh").read_text()
        self.assertIn("llvm_version=22.1.8", preparation)
        self.assertIn("cmake_version=4.2.1", preparation)
        self.assertIn("libcap2-bin", preparation)
        self.assertIn("libcap-progs", preparation)
        self.assertIn("setcap cap_sys_nice=ep", preparation)
        self.assertGreaterEqual(len([line for line in preparation.splitlines() if "sha256=" in line]), 4)

    def test_explicit_package_O0_profile_relaxes_only_the_pipeline_guard(self):
        pipeline = (PACKAGING.parents[1] / "bioseq/backend/pipeline/pipe.cppm").read_text()
        bioseq_cmake = (PACKAGING.parents[1] / "bioseq/CMakeLists.txt").read_text()
        codegen_cmake = (PACKAGING.parents[1] / "cmake/BioseqCodegen.cmake").read_text()
        build_script = (SCRIPTS / "build_and_stage.sh").read_text()
        self.assertIn("!defined(BIOSEQ_PACKAGE_O0_PROFILE)", pipeline)
        self.assertIn(
            "BIOSEQ_PACKAGE_O0_PROFILE requires BIOSEQ_PIPELINE_FORCE_INLINE=0",
            pipeline,
        )
        self.assertIn("BIOSEQ_PACKAGE_O0_PROFILE=1", bioseq_cmake)
        self.assertIn("BIOSEQ_PIPELINE_FORCE_INLINE=0", bioseq_cmake)
        self.assertIn(
            "-DBIOSEQ_CODEGEN_RUN_POST_BUILD_SELF_TESTS:BOOL=OFF",
            codegen_cmake,
        )
        self.assertIn("BIOSEQ_SQL_RUN_PREPARE_INCREMENTAL_SELF_TEST", bioseq_cmake)
        self.assertIn("BIOSEQ_SQL_RUN_NEGATIVE_CONTRACT_SELF_TEST", bioseq_cmake)
        self.assertIn("BIOSEQ_DOC0388_RUN_LEDGER_MAINTAINER_CHECK", bioseq_cmake)
        self.assertIn("cmake_cache_root=/opt/bioseq-package-cmake-cache", build_script)
        self.assertIn('"${cmake_cache_root}/source"', build_script)
        self.assertIn('"${cmake_cache_root}/main"', build_script)
        self.assertIn('"${cmake_cache_root}/build-rooted"', build_script)
        self.assertIn(
            "rsync --archive --checksum --no-times --omit-dir-times --delete",
            build_script,
        )
        self.assertIn(
            "for repository_tree in bioseq codegen cmake; do",
            build_script,
        )
        self.assertIn(
            "reusing existing CMake/Ninja package configuration",
            build_script,
        )
        self.assertIn(
            'CMAKE_HOME_DIRECTORY:INTERNAL=${persistent_source_dir}',
            build_script,
        )
        self.assertIn(
            'export PATH="${tool_root}/cmake/bin:${PATH}"',
            build_script,
        )
        smoke_script = (SCRIPTS / "smoke_installed_package.sh").read_text()
        self.assertIn(
            "PostgreSQL[^0-9]*${postgres_major}",
            smoke_script,
        )
        self.assertIn(
            "-DBIOSEQ_SQL_RUN_NEGATIVE_CONTRACT_SELF_TEST=OFF",
            build_script,
        )
        self.assertIn(
            "-DBIOSEQ_DOC0388_RUN_LEDGER_MAINTAINER_CHECK=OFF",
            build_script,
        )
        self.assertIn(
            "-DBIOSEQ_SQL_RUN_PREPARE_INCREMENTAL_SELF_TEST=OFF",
            build_script,
        )


if __name__ == "__main__":
    unittest.main()
