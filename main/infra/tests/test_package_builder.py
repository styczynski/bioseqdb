from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import signal
import subprocess
import sys
import tempfile
import threading
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
            Path(__file__).resolve().parents[2]
            / "bioseq_caching"
            / "pg"
            / "index.cppm"
        ).read_text(encoding="utf-8")
        self.assertIn('"bioseq.query_cache_optimisation_mode"', cache_source)
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
        self.assertIn(f"{environment.venv_path}/bin/python", command)
        self.assertIn(f"--source-revision {self.revision}", command)
        self.assertIn(f"--poetry-lock-sha256 {self.lock_sha256}", command)
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
            )

        def wait(handle, _key_path):
            with launch_lock:
                self.assertEqual(set(launched), {"arm64", "x64"})
                waited.append(handle.builder.architecture)
            return {"schema_version": 1, "status": "success"}

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
        self.assertEqual(set(launched), {"arm64", "x64"})
        self.assertEqual(set(waited), {"arm64", "x64"})


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
            "BootstrapSchema": package_builder.BOOTSTRAP_SCHEMA,
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
        build_result = {
            "status": "success",
            "revision": revision,
            "poetry_lock_sha256": self.lock_sha256,
            "artifact_count": (
                len(self.matrix.architectures)
                * len(self.matrix.distributions)
                * len(self.matrix.postgresql_majors)
            ),
            "manifests": {"arm64": {}, "x64": {}},
            "worker_units": {
                "arm64": "bioseq-arm64.service",
                "x64": "bioseq-x64.service",
            },
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
        outputs = {"package_bucket_name": "bucket"}
        uploads_per_architecture = [object()] * len(self.matrix.distributions)

        def image_architecture(*call_args):
            architecture = call_args[2]
            return {"ImageId": f"ami-{architecture.name}"}

        with mock.patch.object(
            package_builder, "validate_manifest", return_value=uploads_per_architecture
        ) as validate, mock.patch.object(
            package_builder, "verify_s3_uploads"
        ) as verify, mock.patch.object(
            package_builder, "ssh_key_path", return_value=Path("/tmp/key")
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
            observed_units, {"bioseq-arm64.service", "bioseq-x64.service"}
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

        with mock.patch.object(
            package_builder, "require_successful_remote_unit"
        ) as require_unit, mock.patch.object(
            package_builder,
            "describe_builder",
            return_value={"State": {"Name": "running"}},
        ), mock.patch.object(package_builder, "ssh_run") as ssh, mock.patch.object(
            package_builder, "aws_json", return_value=created
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
            )

        self.assertEqual(image["State"], "available")
        require_unit.assert_called_once_with(
            builder, Path("/tmp/key"), "bioseq-arm64.service"
        )
        ssh.assert_called_once_with(builder, Path("/tmp/key"), "sync")
        create_args = aws_json.call_args.args
        self.assertIn("create-image", create_args)
        self.assertIn("--no-reboot", create_args)
        tag_args = aws_no_output.call_args.args
        self.assertIn(f"Key=PoetryLockSha256,Value={self.lock_sha256}", tag_args)
        self.assertIn(f"Key=CompleteMatrixFingerprint,Value={fingerprint}", tag_args)


class ManifestTests(unittest.TestCase):
    def test_s3_verification_heads_package_and_every_signature_object(self) -> None:
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
                ]
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
        ]
        with mock.patch.object(package_builder, "aws_json", side_effect=responses) as aws:
            package_builder.verify_s3_uploads("us-east-1", "bucket", [upload])
        self.assertEqual(aws.call_count, 2)
        self.assertIn("--version-id", aws.call_args_list[0].args)
        self.assertIn("package-version", aws.call_args_list[0].args)
        self.assertIn("signature-version", aws.call_args_list[1].args)

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
            "schema_version": 1,
            "status": "success",
            "package_version": package_builder.EXPECTED_PACKAGE_VERSION,
            "source_revision": revision,
            "poetry_lock_sha256": lock_sha256,
            "python_environment": python_environment.manifest_value(),
            "architecture": "x64",
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
                        "etag": "package-etag",
                        "version_id": "package-version",
                        "server_side_encryption": "AES256",
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
                source_revision=revision,
                poetry_lock_sha256=lock_sha256,
                python_environment=python_environment,
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

if __name__ == "__main__":
    unittest.main()
