from __future__ import annotations

import argparse
import importlib.util
import inspect
import json
import signal
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "package_builder.py"
SPEC = importlib.util.spec_from_file_location("package_builder", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
package_builder = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = package_builder
SPEC.loader.exec_module(package_builder)


class MatrixTests(unittest.TestCase):
    def test_default_matrix_is_the_complete_doc0390_matrix(self) -> None:
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

    def test_run_stops_exact_builders_after_build_failure(self) -> None:
        builders = package_builder.output_builders(
            self.outputs("i-0aaabbbcccdddeee1"), self.matrix
        )
        args = argparse.Namespace(
            architectures=None,
            distros=None,
            postgres=None,
            region="us-east-1",
            s3_prefix="bioseq/packages",
        )
        with mock.patch.object(
            package_builder,
            "provision_and_start",
            return_value=({}, builders, {}, {}),
        ), mock.patch.object(
            package_builder,
            "execute_build",
            side_effect=package_builder.PackageBuilderError("expected failure"),
        ), mock.patch.object(package_builder, "stop_builders") as stop:
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "expected failure"):
                package_builder.cmd_run(args)
        stop.assert_called_once_with("us-east-1", builders, self.matrix)

    def test_partial_apply_recovers_exact_state_instance_and_stops_it(self) -> None:
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
        )
        with mock.patch.object(
            package_builder,
            "provision_and_start",
            side_effect=package_builder.PackageBuilderError("partial apply"),
        ), mock.patch.object(
            package_builder, "recover_managed_builders", return_value=recovered
        ), mock.patch.object(package_builder, "stop_builders") as stop:
            with self.assertRaisesRegex(package_builder.PackageBuilderError, "partial apply"):
                package_builder.cmd_run(args)
        stop.assert_called_once_with("us-east-1", recovered, self.matrix)

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

    def test_cached_ami_uses_stopped_instance_and_no_reboot_snapshot(self) -> None:
        source = inspect.getsource(package_builder.create_cached_ami)
        self.assertIn("stop_builders", source)
        self.assertIn("cloud-init clean", source)
        self.assertIn("--no-reboot", source)

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

    def test_first_native_build_failure_interrupts_sibling_and_fails_fast(self) -> None:
        remote_source = inspect.getsource(package_builder.run_remote_build)
        orchestration_source = inspect.getsource(package_builder.execute_build)
        self.assertIn('"--fail-fast"', remote_source)
        self.assertIn("interrupt_active_children()", orchestration_source)


class ManifestTests(unittest.TestCase):
    def test_success_manifest_requires_optimization_zero_and_exact_s3_key(self) -> None:
        matrix = package_builder.load_matrix()
        selection = package_builder.Selection(("x64",), ("debian_13",), (17,))
        artifact_name = "bioseq-postgresql17_4.2.3_amd64.deb"
        manifest = {
            "schema_version": 1,
            "status": "success",
            "architecture": "x64",
            "requested": {"postgres": [17], "distros": ["debian_13"]},
            "s3": {
                "bucket": "bucket",
                "region": "us-east-1",
                "base_prefix": "bioseq/packages",
            },
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
                        "optimization_level": 0,
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
                        "download_validation": {
                            "passed": True,
                            "six_artifacts": True,
                            "rpath_absent": True,
                            "compiler_runtime_safe": True,
                            "optimization_level": 0,
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
        )
        self.assertEqual(len(uploads), 1)
        manifest["cells"][0]["validation"]["optimization_level"] = 1
        with self.assertRaisesRegex(package_builder.PackageBuilderError, "failed validation"):
            package_builder.validate_manifest(
                manifest,
                architecture="x64",
                selection=selection,
                matrix=matrix,
                bucket="bucket",
                region="us-east-1",
                s3_prefix="bioseq/packages",
            )
        manifest["cells"][0]["validation"]["optimization_level"] = 0
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
            )

    def test_remote_command_exports_required_optimization_environment(self) -> None:
        builder = package_builder.Builder(
            "arm64",
            "i-0123456789abcdef0",
            "c7g.xlarge",
            "arm64",
            "203.0.113.1",
            "10.0.0.1",
            "ami-0123456789abcdef0",
        )
        selection = package_builder.Selection(("arm64",), ("arch",), (17,))
        manifest = {"schema_version": 1, "status": "success"}
        commands: list[str] = []

        def fake_ssh(*args, **kwargs):
            commands.append(args[2])
            if kwargs.get("capture"):
                return subprocess.CompletedProcess([], 0, json.dumps(manifest), "")
            return subprocess.CompletedProcess([], 0, "", "")

        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(
            package_builder, "LOG_DIR", Path(tmp)
        ), mock.patch.object(package_builder, "ssh_run", side_effect=fake_ssh):
            result = package_builder.run_remote_build(
                builder=builder,
                key_path=Path("/tmp/key"),
                remote_source="/opt/bioseq/source",
                revision="a" * 40,
                selection=selection,
                region="us-east-1",
                bucket="bucket",
                s3_prefix="bioseq/packages",
                secret_arn="arn:aws:secretsmanager:us-east-1:123456789012:secret:key",
                run_id="run",
            )
        self.assertEqual(result, manifest)
        self.assertIn("BIOSEQ_OPTTIMIZATION_LEVEL=0", commands[0])
        self.assertIn("--architecture arm64", commands[0])
        self.assertIn("--source-dir /opt/bioseq/source/main", commands[0])
        self.assertIn("--postgres 17", commands[0])
        self.assertIn("--distros arch", commands[0])


if __name__ == "__main__":
    unittest.main()
