from __future__ import annotations

import contextlib
from dataclasses import replace
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import runpy
import stat
import subprocess
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from bioseq_packaging import (
    cli,
    commands,
    config,
    containers,
    manifest,
    model,
    pipeline,
    publishing,
    signing,
    templates,
)
from bioseq_packaging.packagers import deb, pacman, rpm
from bioseq_packaging.repositories import RepositoryBuild
from bioseq_packaging.repositories import deb as deb_repository
from bioseq_packaging.repositories import pacman as pacman_repository
from bioseq_packaging.repositories import rpm as rpm_repository


PACKAGING_ROOT = Path(__file__).resolve().parents[1]
MAIN_ROOT = PACKAGING_ROOT.parents[1]
REPOSITORY_ROOT = MAIN_ROOT.parent
PINNED_FINGERPRINT = signing.PINNED_SIGNING_FINGERPRINT


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def worker_request(
    root: Path,
    *,
    architecture: model.Architecture = model.Architecture.X64,
    distros: tuple[model.DistroId, ...] = (model.DistroId.DEBIAN_13,),
    postgres: tuple[int, ...] = (17,),
) -> model.WorkerRequest:
    return model.WorkerRequest(
        architecture=architecture,
        bucket="valid-package-bucket",
        region="us-east-1",
        postgres_majors=postgres,
        distro_ids=distros,
        source_dir=MAIN_ROOT.resolve(),
        build_root=(root / "build").resolve(),
        manifest_path=(root / "manifest.json").resolve(),
        s3_prefix="bioseq/packages",
        aws_bin="aws",
        container_output=model.ContainerOutput.STDOUT,
        fail_fast=True,
        signing_secret_arn=(
            "arn:aws:secretsmanager:us-east-1:123456789012:secret:package-key"
        ),
        source_revision="a" * 40,
        poetry_lock_sha256="b" * 64,
    )


def cell_context(
    root: Path,
    distro: model.DistroId,
    *,
    architecture: model.Architecture = model.Architecture.X64,
) -> model.CellContext:
    request = worker_request(root, architecture=architecture, distros=(distro,))
    spec = config.build_cells(request)[0]
    cell_root = (root / "cells" / distro.value).resolve()
    paths = model.CellPaths(
        root=cell_root,
        package_root=cell_root / "package",
        repository_root=cell_root / "repository",
        download_root=cell_root / "download",
        toolchain_cache=(root / "caches" / "toolchain" / distro.value).resolve(),
        cmake_cache=(root / "caches" / "cmake" / distro.value).resolve(),
    )
    return model.CellContext.from_spec(spec, paths)


class RecordingContainers:
    def __init__(self, callback=None) -> None:
        self.calls: list[tuple[model.ContainerPlan, dict[str, str]]] = []
        self.callback = callback

    def run(
        self, plan: model.ContainerPlan, *, environment: dict[str, str]
    ) -> None:
        copied_environment = dict(environment)
        self.calls.append((plan, copied_environment))
        if self.callback is not None:
            self.callback(plan, copied_environment)


class FreshWorkspace:
    def prepare_fresh_directory(self, path: Path) -> Path:
        path.mkdir(parents=True, exist_ok=False)
        return path.resolve()


class FakeS3Runner:
    """Stateful AWS CLI test double that preserves publication ordering."""

    def __init__(
        self,
        *,
        initial_objects: dict[str, bytes] | None = None,
        omit_put_version: bool = False,
        head_encryption: str = "AES256",
        head_version_override: str | None = None,
        out_of_prefix_list: bool = False,
    ) -> None:
        self.objects = dict(initial_objects or {})
        self.versions: dict[str, str] = {}
        self.etags: dict[str, str] = {}
        self.commands: list[commands.Command] = []
        self.events: list[tuple[str, str]] = []
        self.omit_put_version = omit_put_version
        self.head_encryption = head_encryption
        self.head_version_override = head_version_override
        self.out_of_prefix_list = out_of_prefix_list
        self._listed_once = False
        self._next_version = 0

    @staticmethod
    def _argument(argv: tuple[str, ...], name: str) -> str:
        return argv[argv.index(name) + 1]

    def run_captured(
        self,
        command: commands.Command,
        *,
        environment: dict[str, str],
    ) -> str:
        del environment
        self.commands.append(command)
        argv = command.argv
        self.assert_aws_command(argv)
        operation = argv[2]
        if operation == "list-objects-v2":
            prefix = self._argument(argv, "--prefix")
            self.events.append(("list", prefix))
            if self.out_of_prefix_list and not self._listed_once:
                keys = ["outside/the/exact/cell"]
            else:
                keys = sorted(key for key in self.objects if key.startswith(prefix))
            self._listed_once = True
            return json.dumps(
                {
                    "Contents": [{"Key": key} for key in keys],
                    "IsTruncated": False,
                }
            )

        key = self._argument(argv, "--key")
        self.events.append((operation, key))
        if operation == "delete-object":
            self.objects.pop(key, None)
            self.versions.pop(key, None)
            self.etags.pop(key, None)
            return "{}"
        if operation == "put-object":
            data = Path(self._argument(argv, "--body")).read_bytes()
            self._next_version += 1
            version = f"version-{self._next_version}"
            etag = hashlib.sha256(data).hexdigest()[:32]
            self.objects[key] = data
            self.versions[key] = version
            self.etags[key] = etag
            result = {
                "ETag": f'"{etag}"',
                "ServerSideEncryption": "AES256",
            }
            if not self.omit_put_version:
                result["VersionId"] = version
            return json.dumps(result)
        if operation == "head-object":
            version = self.head_version_override or self.versions[key]
            return json.dumps(
                {
                    "VersionId": version,
                    "ServerSideEncryption": self.head_encryption,
                    "ContentLength": len(self.objects[key]),
                    "ETag": f'"{self.etags[key]}"',
                }
            )
        if operation == "get-object":
            destination = Path(argv[-1])
            destination.write_bytes(self.objects[key])
            return json.dumps(
                {
                    "VersionId": self.versions[key],
                    "ServerSideEncryption": "AES256",
                }
            )
        raise AssertionError(f"unexpected AWS operation: {operation}")

    @staticmethod
    def assert_aws_command(argv: tuple[str, ...]) -> None:
        if len(argv) < 3 or argv[0:2] != ("aws", "s3api"):
            raise AssertionError(f"unexpected command: {argv}")


def rpm_publish_tree(root: Path, cell: model.CellContext) -> RepositoryBuild:
    repository_root = (root / "publish-repository").resolve()
    repodata = repository_root / "repodata"
    repodata.mkdir(parents=True)
    package = repository_root / (
        f"{cell.package_name}-{cell.package_version}-1.x86_64.rpm"
    )
    package.write_bytes(b"signed-rpm-payload")
    public_key = repodata / "repomd.xml.key"
    metadata = repodata / "repomd.xml"
    metadata_signature = repodata / "repomd.xml.asc"
    primary = repodata / "primary.xml.gz"
    public_key.write_bytes(b"public-key")
    metadata.write_bytes(b"repository-metadata")
    metadata_signature.write_bytes(b"metadata-signature")
    primary.write_bytes(b"primary-metadata")
    return RepositoryBuild(
        root=repository_root,
        package_path=package,
        signature_paths=(public_key, metadata, metadata_signature),
        metadata_paths=(public_key, metadata, metadata_signature, primary),
    )


class CatalogAndProvenanceTests(unittest.TestCase):
    def test_catalog_is_the_exact_native_doc0390_matrix(self) -> None:
        expected = {
            model.DistroId.OPENSUSE_16_0: (
                model.PackageFormat.RPM,
                "opensuse/leap:16.0",
                "opensuse/leap:16.0",
            ),
            model.DistroId.FEDORA_44: (
                model.PackageFormat.RPM,
                "fedora:44",
                "fedora:44",
            ),
            model.DistroId.DEBIAN_13: (
                model.PackageFormat.DEB,
                "debian:trixie",
                "debian:trixie",
            ),
            model.DistroId.UBUNTU_26_04: (
                model.PackageFormat.DEB,
                "ubuntu:26.04",
                "ubuntu:26.04",
            ),
            model.DistroId.ARCH: (
                model.PackageFormat.PACMAN,
                "archlinux:latest",
                "menci/archlinuxarm:base",
            ),
        }
        self.assertEqual(set(config.DISTRO_CATALOG), set(expected))
        for distro_id, (package_format, x64_image, arm64_image) in expected.items():
            with self.subTest(distro=distro_id):
                distro = config.distro_spec(distro_id)
                self.assertIs(distro.package_format, package_format)
                self.assertEqual(
                    distro.image_for(model.Architecture.X64), x64_image
                )
                self.assertEqual(
                    distro.image_for(model.Architecture.ARM64), arm64_image
                )
                self.assertEqual(
                    distro.capabilities,
                    frozenset({model.ContainerCapability.SYS_NICE}),
                )
                self.assertTrue(distro.template_path.is_absolute())
                self.assertTrue(distro.template_path.exists())

        self.assertEqual(
            config.runtime_dependencies(model.DistroId.DEBIAN_13, 17),
            ("postgresql-17", "libvulkan1"),
        )
        self.assertEqual(
            config.runtime_dependencies(model.DistroId.FEDORA_44, 17),
            ("postgresql17-server", "vulkan-loader"),
        )
        self.assertEqual(
            config.runtime_dependencies(model.DistroId.OPENSUSE_16_0, 17),
            ("postgresql17-server", "libvulkan1"),
        )
        self.assertEqual(
            config.runtime_dependencies(model.DistroId.ARCH, 17),
            ("postgresql-old-upgrade", "vulkan-icd-loader"),
        )

    def test_cells_preserve_requested_order_and_never_reduce_the_matrix(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            request = worker_request(
                Path(raw),
                distros=(model.DistroId.FEDORA_44, model.DistroId.DEBIAN_13),
                postgres=(17, 18),
            )
            cells = config.build_cells(request)
        self.assertEqual(
            [(cell.postgres_major, cell.distro.distro_id) for cell in cells],
            [
                (17, model.DistroId.FEDORA_44),
                (17, model.DistroId.DEBIAN_13),
                (18, model.DistroId.FEDORA_44),
                (18, model.DistroId.DEBIAN_13),
            ],
        )
        self.assertTrue(
            all(cell.package_version == "4.2.3" for cell in cells)
        )
        self.assertTrue(
            all(
                cell.s3_cell_prefix
                == (
                    f"bioseq/packages/x64/postgresql{cell.postgres_major}/"
                    f"{cell.distro.distro_id.value}/"
                    + (
                        "stable/"
                        if cell.distro.package_format is model.PackageFormat.DEB
                        else ""
                    )
                )
                for cell in cells
            )
        )

    def test_worker_request_requires_exact_source_and_lock_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            request = worker_request(Path(raw))
            self.assertEqual(request.source_revision, "a" * 40)
            self.assertEqual(request.poetry_lock_sha256, "b" * 64)
            with self.assertRaises(model.PackageBuildError):
                replace(request, source_revision="a" * 64)
            with self.assertRaises(model.PackageBuildError):
                replace(request, poetry_lock_sha256="not-a-digest")

        published = model.PublishedObject(
            key="bioseq/packages/x64/postgresql17/debian_13/stable/p.deb",
            size=1,
            sha256="c" * 64,
            etag="etag",
            version_id="version-1",
            server_side_encryption="AES256",
        )
        self.assertEqual(published.to_manifest()["version_id"], "version-1")
        with self.assertRaises(model.PackageBuildError):
            replace(published, version_id="")
        with self.assertRaises(model.PackageBuildError):
            replace(published, server_side_encryption="aws:kms")


class CliAndContainerTests(unittest.TestCase):
    def test_cli_requires_provenance_and_has_no_legacy_binary_override(self) -> None:
        arguments = [
            "--architecture",
            "x64",
            "--bucket",
            "valid-package-bucket",
            "--region",
            "us-east-1",
            "--postgres",
            "17",
            "--distros",
            "debian_13",
            "--source-revision",
            "a" * 40,
            "--poetry-lock-sha256",
            "b" * 64,
        ]
        parsed = cli.argument_parser().parse_args(arguments)
        self.assertEqual(parsed.source_revision, "a" * 40)
        self.assertEqual(parsed.poetry_lock_sha256, "b" * 64)
        self.assertFalse(hasattr(parsed, "omnipackage_bin"))

        legacy_flag = "--" + "omni" + "package-bin"
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            cli.argument_parser().parse_args([*arguments, legacy_flag, "/tmp/tool"])

    def test_native_plans_have_fixed_mounts_capabilities_and_secret_scope(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            for architecture in model.Architecture:
                for distro_id in model.DistroId:
                    with self.subTest(architecture=architecture, distro=distro_id):
                        cell = cell_context(root, distro_id, architecture=architecture)
                        plan = containers.native_container_plan(
                            cell,
                            model.ContainerPurpose.BUILD,
                            ("/bin/true",),
                            ("BIOSEQ_POSTGRES_MAJOR",),
                        )
                        self.assertEqual(len(plan.mounts), 4)
                        self.assertTrue(plan.mounts[0].read_only)
                        self.assertEqual(str(plan.mounts[0].target), "/source")
                        self.assertEqual(
                            [mount.read_only for mount in plan.mounts],
                            [True, False, False, False],
                        )
                        self.assertTrue(
                            all(mount.source.is_absolute() for mount in plan.mounts)
                        )
                        self.assertEqual(
                            plan.capabilities,
                            frozenset({model.ContainerCapability.SYS_NICE}),
                        )
                        command = containers.docker_command(plan)
                        self.assertNotIn("--platform", command.argv)
                        self.assertEqual(command.argv.count("--cap-add"), 1)
                        self.assertNotIn("GPG_PRIVATE_KEY", command.argv)

            cell = cell_context(root, model.DistroId.DEBIAN_13)
            with self.assertRaises(model.PackageBuildError):
                containers.native_container_plan(
                    cell,
                    model.ContainerPurpose.BUILD,
                    ("/bin/true",),
                    ("GPG_PRIVATE_KEY",),
                )
            repository_plan = containers.native_container_plan(
                cell,
                model.ContainerPurpose.REPOSITORY,
                ("/bin/true",),
                ("GPG_PRIVATE_KEY",),
            )
            self.assertEqual(repository_plan.capabilities, frozenset())
            repository_command = containers.docker_command(repository_plan)
            self.assertIn("GPG_PRIVATE_KEY", repository_command.argv)
            self.assertEqual(
                repository_command.sensitive_environment, frozenset({"GPG_PRIVATE_KEY"})
            )
            self.assertNotIn("test-only-private-key", " ".join(repository_command.argv))

            unsafe = replace(repository_plan, argv=("--platform=linux/amd64",))
            with self.assertRaises(model.PackageBuildError):
                containers.docker_command(unsafe)


class FormatPlanTests(unittest.TestCase):
    def test_deb_and_rpm_metadata_and_build_plans_are_direct_and_o0(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            debian = cell_context(root, model.DistroId.DEBIAN_13)
            deb_variables = deb.template_variables(debian)
            self.assertEqual(deb_variables["architecture"], "amd64")
            self.assertEqual(deb_variables["postgres_major"], 17)
            self.assertEqual(
                deb_variables["runtime_dependencies"],
                ["postgresql-17", "libvulkan1"],
            )
            deb_environment = deb.build_environment(debian)
            self.assertEqual(deb_environment["BIOSEQ_OPTTIMIZATION_LEVEL"], "0")
            self.assertEqual(tuple(config.SOURCE_EXCLUDES), tuple(
                deb_environment["BIOSEQ_SOURCE_EXCLUDES"].splitlines()
            ))
            deb_plan = deb.build_plan(debian)
            self.assertIn("dpkg-buildpackage --no-sign -b -tc", deb_plan.argv[-1])
            self.assertNotIn("--platform", " ".join(deb_plan.argv))
            self.assertEqual(
                deb_plan.capabilities,
                frozenset({model.ContainerCapability.SYS_NICE}),
            )

            fedora = cell_context(root, model.DistroId.FEDORA_44)
            rpm_variables = rpm.template_variables(fedora)
            self.assertEqual(rpm_variables["architecture"], "x64")
            self.assertEqual(
                rpm_variables["runtime_dependencies"],
                ["postgresql17-server", "vulkan-loader"],
            )
            rpm_environment = rpm.build_environment(fedora)
            self.assertEqual(rpm_environment["BIOSEQ_OPTTIMIZATION_LEVEL"], "0")
            rpm_plan = rpm.build_plan(fedora)
            self.assertIn("rpmbuild --clean -bb", rpm_plan.argv[-1])
            self.assertEqual(
                rpm_plan.capabilities,
                frozenset({model.ContainerCapability.SYS_NICE}),
            )

    def test_pacman_build_is_native_zstd_and_sandbox_adjustment_is_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            for architecture, package_architecture in (
                (model.Architecture.X64, "x86_64"),
                (model.Architecture.ARM64, "aarch64"),
            ):
                with self.subTest(architecture=architecture):
                    cell = cell_context(
                        root / architecture.value,
                        model.DistroId.ARCH,
                        architecture=architecture,
                    )
                    variables = pacman.template_variables(cell)
                    self.assertEqual(variables["architecture"], package_architecture)
                    self.assertEqual(variables["optimization_level"], 0)
                    environment = pacman.build_environment(cell)
                    self.assertEqual(environment["BIOSEQ_OPTTIMIZATION_LEVEL"], "0")
                    self.assertEqual(environment["BIOSEQ_PACKAGE_PARALLELISM"], "4")
                    self.assertEqual(
                        pacman.expected_artifact_name(cell),
                        f"bioseq-postgresql17-4.2.3-1-{package_architecture}.pkg.tar.zst",
                    )
                    plan = pacman.build_plan(cell)
                    script = plan.argv[-1]
                    self.assertIn("sandbox_option=DisableSandbox", script)
                    self.assertIn("PKGEXT='.pkg.tar.zst'", script)
                    self.assertIn("makepkg --force --nodeps --noconfirm", script)
                    self.assertLess(
                        script.index("PKGEXT='.pkg.tar.zst'"),
                        script.index("makepkg --force --nodeps --noconfirm"),
                    )
                    self.assertEqual(len(plan.mounts), 4)
                    self.assertEqual(
                        plan.capabilities,
                        frozenset({model.ContainerCapability.SYS_NICE}),
                    )

    def test_pacman_sandbox_adjustment_accepts_repeated_options_sections(self) -> None:
        def sandbox_prelude(script: str) -> str:
            start = script.index("pacman_conf=/etc/pacman.conf")
            end = script.index("pacman -Syu", start)
            body = script[start:end].replace(
                "pacman_conf=/etc/pacman.conf",
                'pacman_conf="${PACMAN_CONF_FIXTURE}"',
                1,
            )
            return (
                "set -Eeuo pipefail\n"
                "configure_fixture() {\n"
                f"{body}\n"
                "}\n"
                "configure_fixture\n"
            )

        smoke_script = (
            PACKAGING_ROOT / "scripts" / "smoke_installed_package.sh"
        ).read_text(encoding="utf-8")
        scripts = {
            "build": pacman.PACMAN_BUILD_SCRIPT,
            "repository": pacman_repository.PACMAN_REPOSITORY_SCRIPT,
            "smoke": smoke_script,
        }
        valid_fixtures = {
            "single": "[options]\nArchitecture = auto\n[core]\nInclude = mirrorlist\n",
            "repeated": (
                "[options]\nArchitecture = auto\n"
                "[core]\nInclude = mirrorlist\n"
                "[options]\nNoExtract = usr/share/doc/*\n"
            ),
            "existing_first": (
                "[options]\nDisableSandbox\nArchitecture = auto\n"
                "[core]\nInclude = mirrorlist\n"
            ),
            "existing_second": (
                "[options]\nArchitecture = auto\n"
                "[core]\nInclude = mirrorlist\n"
                "[options]\nDisableSandbox\nNoExtract = usr/share/doc/*\n"
            ),
        }
        invalid_fixtures = {
            "missing_options": "Architecture = auto\n[core]\nInclude = mirrorlist\n",
            "duplicate_same_section": (
                "[options]\nDisableSandbox\nDisableSandbox\nArchitecture = auto\n"
            ),
            "duplicate_across_sections": (
                "[options]\nDisableSandbox\n[core]\nInclude = mirrorlist\n"
                "[options]\nDisableSandbox\n"
            ),
            "outside_options": "DisableSandbox\n[options]\nArchitecture = auto\n",
        }
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            for script_name, script in scripts.items():
                prelude = sandbox_prelude(script)
                for name, contents in valid_fixtures.items():
                    pacman_conf = root / f"pacman-{script_name}-{name}.conf"
                    pacman_conf.write_text(contents, encoding="utf-8")
                    environment = dict(os.environ)
                    environment["PACMAN_CONF_FIXTURE"] = str(pacman_conf)
                    environment["BIOSEQ_BUILD_DEPENDENCIES"] = "cmake"
                    with self.subTest(script=script_name, fixture=name):
                        for _ in range(2):
                            subprocess.run(
                                ["bash", "-c", prelude],
                                check=True,
                                env=environment,
                                capture_output=True,
                                text=True,
                            )
                        lines = pacman_conf.read_text(
                            encoding="utf-8"
                        ).splitlines()
                        self.assertEqual(lines.count("[options]"), contents.count("[options]"))
                        self.assertEqual(lines.count("DisableSandbox"), 1)
                        sandbox_index = lines.index("DisableSandbox")
                        prior_headers = [
                            line
                            for line in lines[:sandbox_index]
                            if line.startswith("[") and line.endswith("]")
                        ]
                        self.assertTrue(prior_headers)
                        self.assertEqual(prior_headers[-1], "[options]")

                for name, contents in invalid_fixtures.items():
                    pacman_conf = root / f"pacman-{script_name}-{name}.conf"
                    pacman_conf.write_text(contents, encoding="utf-8")
                    environment = dict(os.environ)
                    environment["PACMAN_CONF_FIXTURE"] = str(pacman_conf)
                    environment["BIOSEQ_BUILD_DEPENDENCIES"] = "cmake"
                    with self.subTest(script=script_name, invalid_fixture=name):
                        result = subprocess.run(
                            ["bash", "-c", prelude],
                            check=False,
                            env=environment,
                            capture_output=True,
                            text=True,
                        )
                        self.assertEqual(result.returncode, 64)


class TemplateRendererTests(unittest.TestCase):
    def test_renderer_is_strict_public_only_and_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            cell = cell_context(root, model.DistroId.FEDORA_44)
            variables = rpm.template_variables(cell)
            first = templates.TemplateRenderer(source_date_epoch=0)
            second = templates.TemplateRenderer(source_date_epoch="0")
            template_text = cell.distro.template_path.read_text(encoding="utf-8")
            rendered_first = first.render_text(
                template_text, variables, source="bioseq.spec.liquid"
            )
            rendered_second = second.render_text(
                template_text, variables, source="bioseq.spec.liquid"
            )
            self.assertEqual(rendered_first.encode(), rendered_second.encode())
            self.assertNotIn("{{", rendered_first)
            self.assertNotIn("{%", rendered_first)
            self.assertNotIn("${BIOSEQ_", rendered_first)

            forbidden = dict(variables)
            forbidden["GPG_PRIVATE_KEY"] = "must-never-enter-a-template"
            with self.assertRaises(model.PackageBuildError):
                first.render_text("{{ package_name }}", forbidden)
            missing = dict(variables)
            missing.pop("homepage")
            with self.assertRaises(model.PackageBuildError):
                first.render_text("{{ package_name }}", missing)
            override = dict(variables)
            override["current_time_rfc2822"] = "caller-controlled"
            with self.assertRaises(model.PackageBuildError):
                first.render_text("{{ current_time_rfc2822 }}", override)

    def test_debian_tree_is_recursive_resolved_and_byte_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            renderer = templates.TemplateRenderer(source_date_epoch=0)
            cells = (
                cell_context(root / "first", model.DistroId.DEBIAN_13),
                cell_context(root / "second", model.DistroId.DEBIAN_13),
            )
            rendered_trees = [
                deb.render_debian_tree(cell, templates=renderer) for cell in cells
            ]
            rendered = []
            for tree in rendered_trees:
                files = {
                    path.relative_to(tree).as_posix(): path
                    for path in tree.rglob("*")
                    if path.is_file()
                }
                self.assertEqual(
                    set(files),
                    {"changelog", "control", "rules", "source/format"},
                )
                self.assertEqual(stat.S_IMODE(files["rules"].stat().st_mode), 0o755)
                for relative, path in files.items():
                    if relative != "rules":
                        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o644)
                    payload = path.read_bytes()
                    self.assertNotIn(b"{{", payload)
                    self.assertNotIn(b"{%", payload)
                    self.assertNotIn(b"${BIOSEQ_", payload)
                rendered.append(
                    {relative: path.read_bytes() for relative, path in files.items()}
                )
            self.assertEqual(rendered[0], rendered[1])


class PublishingContractTests(unittest.TestCase):
    @staticmethod
    def _publisher(runner: FakeS3Runner) -> publishing.Publisher:
        return publishing.Publisher(
            aws_bin="aws",
            region="us-east-1",
            bucket="valid-package-bucket",
            prefix="bioseq/packages",
            runner=runner,
            redactor=commands.Redactor(),
        )

    def test_publish_orders_metadata_and_attests_exact_encrypted_versions(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            cell = cell_context(root, model.DistroId.FEDORA_44)
            repository = rpm_publish_tree(root, cell)
            prefix = cell.s3_cell_prefix
            old_metadata = {
                prefix + "repodata/repomd.xml": b"old-metadata",
                prefix + "repodata/repomd.xml.asc": b"old-signature",
                prefix + "stale-object": b"stale",
            }
            runner = FakeS3Runner(initial_objects=old_metadata)
            result = self._publisher(runner).publish(cell, repository)

            put_positions = {
                key: position
                for position, (operation, key) in enumerate(runner.events)
                if operation == "put-object"
            }
            self.assertEqual(
                set(put_positions),
                {
                    prefix + path.relative_to(repository.root).as_posix()
                    for path in repository.root.rglob("*")
                    if path.is_file()
                },
            )
            payload_keys = {
                prefix + repository.package_path.name,
                prefix + "repodata/repomd.xml.key",
                prefix + "repodata/primary.xml.gz",
            }
            metadata_keys = {
                prefix + "repodata/repomd.xml",
                prefix + "repodata/repomd.xml.asc",
            }
            self.assertLess(
                max(put_positions[key] for key in payload_keys),
                min(put_positions[key] for key in metadata_keys),
            )
            first_put = min(put_positions.values())
            self.assertTrue(
                all(
                    runner.events.index(("delete-object", key)) < first_put
                    for key in metadata_keys
                )
            )
            stale_delete = runner.events.index(
                ("delete-object", prefix + "stale-object")
            )
            self.assertGreater(stale_delete, max(put_positions.values()))

            for command in runner.commands:
                operation = command.argv[2]
                if operation == "put-object":
                    index = command.argv.index("--server-side-encryption")
                    self.assertEqual(command.argv[index + 1], "AES256")
                if operation in {"head-object", "get-object"}:
                    key = FakeS3Runner._argument(command.argv, "--key")
                    version = FakeS3Runner._argument(command.argv, "--version-id")
                    self.assertEqual(version, runner.versions[key])

            self.assertEqual(result.package.sha256, sha256(result.downloaded_path))
            self.assertEqual(result.package.version_id, runner.versions[result.package.key])
            self.assertEqual(result.package.server_side_encryption, "AES256")
            self.assertEqual(
                set(result.downloaded_objects),
                set(runner.objects),
            )
            for key, destination in result.downloaded_objects.items():
                self.assertEqual(destination.read_bytes(), runner.objects[key])

    def test_publish_fails_closed_on_listing_put_and_head_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            cell = cell_context(root, model.DistroId.FEDORA_44)

            outside_runner = FakeS3Runner(out_of_prefix_list=True)
            with self.assertRaises(model.PackageBuildError):
                self._publisher(outside_runner).publish(
                    cell, rpm_publish_tree(root / "outside", cell)
                )
            self.assertFalse(
                any(
                    operation in {"delete-object", "put-object"}
                    for operation, _key in outside_runner.events
                )
            )

            missing_version_runner = FakeS3Runner(omit_put_version=True)
            with self.assertRaises(model.PackageBuildError):
                self._publisher(missing_version_runner).publish(
                    cell, rpm_publish_tree(root / "missing-version", cell)
                )
            put_commands = [
                command
                for command in missing_version_runner.commands
                if command.argv[2] == "put-object"
            ]
            self.assertEqual(len(put_commands), 1)
            self.assertIn("--server-side-encryption", put_commands[0].argv)

            failure_modes = (
                {"head_encryption": "aws:kms"},
                {"head_version_override": "wrong-version"},
            )
            for index, options in enumerate(failure_modes):
                with self.subTest(options=options):
                    runner = FakeS3Runner(**options)
                    with self.assertRaises(model.PackageBuildError):
                        self._publisher(runner).publish(
                            cell,
                            rpm_publish_tree(root / f"head-failure-{index}", cell),
                        )
                    head_commands = [
                        command
                        for command in runner.commands
                        if command.argv[2] == "head-object"
                    ]
                    self.assertEqual(len(head_commands), 1)
                    self.assertIn("--version-id", head_commands[0].argv)


class RepositoryContractTests(unittest.TestCase):
    def test_pacman_alias_materialization_is_readable_atomic_and_clean(self) -> None:
        script = pacman_repository.PACMAN_REPOSITORY_SCRIPT
        function_start = script.index("materialize_alias() {")
        function_end = script.index('\ndatabase="${BIOSEQ_REPOSITORY_NAME}', function_start)
        function = script[function_start:function_end]
        harness = "\n".join(
            (
                "set -Eeuo pipefail",
                function,
                'materialize_alias "$1" "$2"',
            )
        )

        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            canonical = root / "repository.db.tar.gz"
            alias = root / "repository.db"
            canonical.write_bytes(b"repository-metadata")

            subprocess.run(
                ["bash", "-c", harness, "pacman-alias-test", canonical, alias],
                check=True,
                text=True,
                capture_output=True,
            )
            self.assertEqual(alias.read_bytes(), canonical.read_bytes())
            self.assertEqual(stat.S_IMODE(alias.stat().st_mode), 0o644)
            self.assertTrue(alias.is_file())
            self.assertFalse(alias.is_symlink())
            self.assertEqual(list(root.glob("repository.db.tmp.*")), [])

            alias.unlink()
            alias.mkdir()
            failed = subprocess.run(
                ["bash", "-c", harness, "pacman-alias-test", canonical, alias],
                check=False,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(list(root.glob("repository.db.tmp.*")), [])

    def test_rpm_repository_plan_is_pinned_secret_scoped_and_exact(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            cell = cell_context(root, model.DistroId.FEDORA_44)
            artifact = root / "bioseq-postgresql17-4.2.3-1.x86_64.rpm"
            artifact.write_bytes(b"unsigned-rpm")
            key = signing.normalize_signing_key(
                """-----BEGIN PGP PRIVATE KEY BLOCK-----
test-only-private-key-material
-----END PGP PRIVATE KEY BLOCK-----
"""
            )
            material = signing.SigningMaterial(
                key=key, fingerprint=PINNED_FINGERPRINT
            )
            try:
                plan = rpm_repository.repository_plan(cell, artifact, material)
                self.assertEqual(plan.purpose, model.ContainerPurpose.REPOSITORY)
                self.assertEqual(plan.capabilities, frozenset())
                self.assertEqual(len(plan.mounts), 1)
                self.assertEqual(str(plan.mounts[0].target), "/repo")
                self.assertEqual(
                    set(plan.environment_names),
                    {
                        "BIOSEQ_DISTRO_ID",
                        "BIOSEQ_RPM_FILENAME",
                        "BIOSEQ_SIGNING_FINGERPRINT",
                        "GPG_PRIVATE_KEY",
                    },
                )
                script = plan.argv[-1]
                self.assertIn("--addsign", script)
                self.assertIn("createrepo_c --retain-old-md=0", script)
                self.assertIn("--local-user", script)

                def create_repository(
                    _plan: model.ContainerPlan, _environment: dict[str, str]
                ) -> None:
                    repository = cell.paths.repository_root
                    repodata = repository / "repodata"
                    repodata.mkdir()
                    for name in (
                        "repomd.xml.key",
                        "repomd.xml",
                        "repomd.xml.asc",
                    ):
                        (repodata / name).write_text(name)

                runner = RecordingContainers(create_repository)
                result = rpm_repository.build_repository(
                    cell,
                    artifact,
                    signing=material,
                    containers=runner,
                    workspace=FreshWorkspace(),
                )
                self.assertEqual(
                    tuple(path.relative_to(result.root).as_posix() for path in result.signature_paths),
                    (
                        "repodata/repomd.xml.key",
                        "repodata/repomd.xml",
                        "repodata/repomd.xml.asc",
                    ),
                )
                self.assertEqual(len(runner.calls), 1)
                self.assertIn("GPG_PRIVATE_KEY", runner.calls[0][1])

                retained_environment: dict[str, str] | None = None
                with rpm_repository.repository_environment(
                    cell, artifact, material
                ) as environment:
                    retained_environment = environment
                    self.assertEqual(
                        set(environment),
                        {
                            "BIOSEQ_DISTRO_ID",
                            "BIOSEQ_RPM_FILENAME",
                            "BIOSEQ_SIGNING_FINGERPRINT",
                            "GPG_PRIVATE_KEY",
                        },
                    )
                self.assertEqual(retained_environment, {})
            finally:
                material.close()

    def test_deb_repository_binds_exact_package_bytes_and_signature_set(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            cell = cell_context(root, model.DistroId.DEBIAN_13)
            artifact = root / "bioseq-postgresql17_4.2.3_amd64.deb"
            artifact.write_bytes(b"unsigned-deb")

            def create_repository(
                _plan: model.ContainerPlan, environment: dict[str, str]
            ) -> None:
                stable = cell.paths.repository_root / "stable"
                package = stable / environment["BIOSEQ_PACKAGE_FILENAME"]
                packages = stable / "Packages"
                packages.write_text(
                    "\n".join(
                        (
                            "Package: bioseq-postgresql17",
                            f"Filename: {package.name}",
                            f"Size: {package.stat().st_size}",
                            f"SHA256: {sha256(package)}",
                            "",
                        )
                    )
                )
                (stable / "Packages.gz").write_bytes(
                    gzip.compress(packages.read_bytes(), compresslevel=9, mtime=0)
                )
                signed_names = (package.name, "Packages", "Packages.gz")
                release_lines = ["Suite: stable", "SHA256:"]
                for name in signed_names:
                    payload = stable / name
                    release_lines.append(
                        f" {sha256(payload)} {payload.stat().st_size} {name}"
                    )
                (stable / "Release").write_text("\n".join(release_lines) + "\n")
                for name in ("Release.key", "Release.gpg", "InRelease"):
                    (stable / name).write_text(name)

            runner = RecordingContainers(create_repository)
            key = signing.normalize_signing_key(
                """-----BEGIN PGP PRIVATE KEY BLOCK-----
test-only-private-key-material
-----END PGP PRIVATE KEY BLOCK-----
"""
            )
            material = signing.SigningMaterial(
                key=key, fingerprint=PINNED_FINGERPRINT
            )
            try:
                result = deb_repository.build_repository(
                    cell,
                    artifact,
                    signing=material,
                    containers=runner,
                    workspace=FreshWorkspace(),
                )
            finally:
                material.close()
            self.assertEqual(result.package_path.read_bytes(), artifact.read_bytes())
            self.assertEqual(
                tuple(path.relative_to(result.root).as_posix() for path in result.signature_paths),
                (
                    "Release.key",
                    "Release",
                    "Release.gpg",
                    "InRelease",
                ),
            )
            plan, environment = runner.calls[0]
            self.assertEqual(plan.capabilities, frozenset())
            self.assertEqual(len(plan.mounts), 1)
            self.assertIn("dpkg-scanpackages", plan.argv[-1])
            self.assertIn("gzip -n -9", plan.argv[-1])
            self.assertEqual(environment["BIOSEQ_SIGNING_FINGERPRINT"], PINNED_FINGERPRINT)

    def test_pacman_repository_is_regular_file_exact_and_secret_scoped(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            cell = cell_context(root, model.DistroId.ARCH)
            artifact = root / pacman.expected_artifact_name(cell)
            artifact.write_bytes(b"signed-pacman-package")
            key = signing.normalize_signing_key(
                """-----BEGIN PGP PRIVATE KEY BLOCK-----
test-only-private-key-material
-----END PGP PRIVATE KEY BLOCK-----
"""
            )
            material = signing.SigningMaterial(
                key=key, fingerprint=PINNED_FINGERPRINT
            )

            def create_repository(
                _plan: model.ContainerPlan, environment: dict[str, str]
            ) -> None:
                repository = cell.paths.repository_root
                package = repository / environment["BIOSEQ_PACKAGE_FILENAME"]
                repository_name = environment["BIOSEQ_REPOSITORY_NAME"]
                public_key = repository / "public.key"
                package_signature = repository / f"{package.name}.sig"
                database = repository / f"{repository_name}.db.tar.gz"
                database_signature = database.with_name(database.name + ".sig")
                files = repository / f"{repository_name}.files.tar.gz"
                files_signature = files.with_name(files.name + ".sig")
                public_key.write_bytes(b"public-key")
                package_signature.write_bytes(b"package-signature")
                description = "\n".join(
                    (
                        "%NAME%",
                        cell.package_name,
                        "",
                        "%FILENAME%",
                        package.name,
                        "",
                        "%VERSION%",
                        f"{cell.package_version}-1",
                        "",
                        "%ARCH%",
                        "x86_64",
                        "",
                        "%CSIZE%",
                        str(package.stat().st_size),
                        "",
                        "%SHA256SUM%",
                        sha256(package),
                        "",
                    )
                ).encode()
                with tarfile.open(database, "w:gz") as archive:
                    member = tarfile.TarInfo(
                        f"{cell.package_name}-{cell.package_version}-1/desc"
                    )
                    member.size = len(description)
                    member.mtime = 0
                    archive.addfile(member, io.BytesIO(description))
                database_signature.write_bytes(b"database-signature")
                files.write_bytes(b"files-database")
                files_signature.write_bytes(b"files-signature")
                for canonical, alias in (
                    (database, repository / f"{repository_name}.db"),
                    (
                        database_signature,
                        repository / f"{repository_name}.db.sig",
                    ),
                    (files, repository / f"{repository_name}.files"),
                    (
                        files_signature,
                        repository / f"{repository_name}.files.sig",
                    ),
                ):
                    alias.write_bytes(canonical.read_bytes())

            runner = RecordingContainers(create_repository)
            try:
                plan = pacman_repository.repository_plan(cell, artifact, material)
                self.assertEqual(plan.purpose, model.ContainerPurpose.REPOSITORY)
                self.assertEqual(plan.capabilities, frozenset())
                self.assertEqual(len(plan.mounts), 1)
                self.assertEqual(str(plan.mounts[0].target), "/repo")
                self.assertIn("repo-add --sign --verify", plan.argv[-1])
                self.assertIn("materialize_alias", plan.argv[-1])
                self.assertIn('chmod 0644 "${temporary_alias}"', plan.argv[-1])
                self.assertEqual(
                    set(plan.environment_names),
                    {
                        "BIOSEQ_PACKAGE_FILENAME",
                        "BIOSEQ_REPOSITORY_NAME",
                        "BIOSEQ_SIGNING_FINGERPRINT",
                        "GPG_PRIVATE_KEY",
                    },
                )
                with mock.patch.object(pacman_repository, "verify_with_gpgv"):
                    result = pacman_repository.build_repository(
                        cell,
                        artifact,
                        signing=material,
                        containers=runner,
                        workspace=FreshWorkspace(),
                    )
                self.assertEqual(
                    tuple(
                        path.relative_to(result.root).as_posix()
                        for path in result.signature_paths
                    ),
                    (
                        "public.key",
                        f"{artifact.name}.sig",
                        f"{cell.package_name}.db.tar.gz",
                        f"{cell.package_name}.db.tar.gz.sig",
                    ),
                )
                self.assertEqual(
                    tuple(
                        path.relative_to(result.root).as_posix()
                        for path in result.metadata_paths
                    ),
                    (
                        f"{cell.package_name}.files.tar.gz",
                        f"{cell.package_name}.files.tar.gz.sig",
                        f"{cell.package_name}.db",
                        f"{cell.package_name}.db.sig",
                        f"{cell.package_name}.files",
                        f"{cell.package_name}.files.sig",
                    ),
                )
                self.assertEqual(len(runner.calls), 1)
                self.assertIn("GPG_PRIVATE_KEY", runner.calls[0][1])
                for path in (
                    result.package_path,
                    *result.signature_paths,
                    *result.metadata_paths,
                ):
                    self.assertEqual(path.stat().st_mode & 0o444, 0o444)

                retained_environment: dict[str, str] | None = None
                with pacman_repository.repository_environment(
                    cell, artifact, material
                ) as environment:
                    retained_environment = environment
                self.assertEqual(retained_environment, {})
            finally:
                material.close()


class PipelineOrchestrationTests(unittest.TestCase):
    @staticmethod
    def _paths(root: Path, spec: model.CellSpec) -> model.CellPaths:
        cell_root = (root / "pipeline-cell").resolve()
        return model.CellPaths(
            root=cell_root,
            package_root=cell_root / "package",
            repository_root=cell_root / "repository",
            download_root=cell_root / "download",
            toolchain_cache=(root / "toolchain-cache").resolve(),
            cmake_cache=(root / "cmake-cache").resolve(),
        )

    def test_cell_sequence_checkpoints_every_acceptance_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            spec = config.build_cells(worker_request(root))[0]
            paths = self._paths(root, spec)
            events: list[str] = []

            class Workspace:
                def cell_paths(self, _spec: model.CellSpec) -> model.CellPaths:
                    return paths

                def prepare_cell(self, _spec: model.CellSpec) -> model.CellContext:
                    for directory in (
                        paths.root,
                        paths.package_root,
                        paths.repository_root,
                        paths.download_root,
                    ):
                        directory.mkdir(parents=True, exist_ok=True)
                    return model.CellContext.from_spec(spec, paths)

            built_path = paths.package_root / "bioseq-postgresql17_4.2.3_amd64.deb"
            downloaded_path = paths.download_root / built_path.name

            class Acceptance:
                def validate_built(self, _cell, artifact):
                    events.append("validate-built")
                    self.assert_path(artifact, built_path)
                    return "built-artifact-evidence", {"stage": "built"}

                def smoke(self, _cell, artifact):
                    stage = "built" if artifact == built_path else "downloaded"
                    events.append(f"smoke-{stage}")
                    return {"stage": stage}

                def validate_download(self, _cell, built, downloaded):
                    events.append("validate-download")
                    if built != "built-artifact-evidence" or downloaded != downloaded_path:
                        raise AssertionError("pipeline passed wrong download evidence")
                    return "downloaded-artifact-evidence", {"stage": "downloaded"}

                @staticmethod
                def assert_path(actual: Path, expected: Path) -> None:
                    if actual != expected:
                        raise AssertionError(f"{actual} != {expected}")

            package_object = model.PublishedObject(
                key=spec.s3_cell_prefix + built_path.name,
                size=len(b"package-bytes"),
                sha256=hashlib.sha256(b"package-bytes").hexdigest(),
                etag="package-etag",
                version_id="package-version",
                server_side_encryption="AES256",
            )
            signature_object = model.PublishedObject(
                key=spec.s3_cell_prefix + "Release.gpg",
                size=9,
                sha256=hashlib.sha256(b"signature").hexdigest(),
                etag="signature-etag",
                version_id="signature-version",
                server_side_encryption="AES256",
            )
            repository_object = model.PublishedObject(
                key=spec.s3_cell_prefix + "Packages.gz",
                size=10,
                sha256=hashlib.sha256(b"repository").hexdigest(),
                etag="repository-etag",
                version_id="repository-version",
                server_side_encryption="AES256",
            )

            class Publisher:
                def publish(self, _cell, _repository):
                    events.append("publish")
                    downloaded_path.write_bytes(b"package-bytes")
                    return SimpleNamespace(
                        package=package_object,
                        downloaded_path=downloaded_path,
                        signature_objects=(signature_object,),
                        objects=(package_object, signature_object, repository_object),
                        downloaded_objects={
                            package_object.key: downloaded_path,
                            signature_object.key: paths.download_root / "Release.gpg",
                            repository_object.key: paths.download_root / "Packages.gz",
                        },
                    )

            class Manifest:
                def start_cell(self, **_kwargs):
                    events.append("start-cell")
                    return {"status": "building"}

                def mark_validated(self, _entry, _artifact, _validation):
                    events.append("mark-validated")

                def mark_smoked(self, _entry, _smoke):
                    events.append("mark-smoked")

                def mark_uploaded(self, _entry, evidence, *, artifact):
                    if (
                        not evidence.published
                        or evidence.package != package_object
                        or artifact != "downloaded-artifact-evidence"
                    ):
                        raise AssertionError("pipeline produced wrong upload evidence")
                    events.append("mark-uploaded")

                def fail_cell(self, _entry, _error):
                    events.append("fail-cell")

            def build_package(_cell, _services):
                events.append("build-package")
                built_path.write_bytes(b"package-bytes")
                return built_path

            def build_repository(_cell, artifact, _signing, _services):
                if artifact != built_path:
                    raise AssertionError("repository received wrong artifact")
                events.append("build-repository")
                return "repository-evidence"

            signature_evidence = model.SignatureEvidence(
                format=model.PackageFormat.DEB,
                signing_key_fingerprint=PINNED_FINGERPRINT,
                package_signature="signed-deb-release-sha256",
                objects=(package_object,),
            )

            def verify_signatures(**kwargs):
                if kwargs["expected_signing_fingerprint"] != PINNED_FINGERPRINT:
                    raise AssertionError("pipeline did not pin signing fingerprint")
                events.append("verify-signatures")
                return signature_evidence

            services = SimpleNamespace(
                workspace=Workspace(),
                acceptance=Acceptance(),
                publisher=Publisher(),
                runner=object(),
                environment={},
            )
            signing_material = SimpleNamespace(fingerprint=PINNED_FINGERPRINT)
            with (
                mock.patch.object(
                    pipeline, "_build_package", side_effect=build_package
                ),
                mock.patch.object(
                    pipeline, "_build_repository", side_effect=build_repository
                ),
                mock.patch.object(
                    pipeline,
                    "verify_published_signatures",
                    side_effect=verify_signatures,
                ),
            ):
                pipeline._process_cell(
                    spec,
                    signing=signing_material,
                    services=services,
                    manifest=Manifest(),
                )

            self.assertEqual(
                events,
                [
                    "start-cell",
                    "build-package",
                    "validate-built",
                    "mark-validated",
                    "smoke-built",
                    "mark-smoked",
                    "build-repository",
                    "publish",
                    "validate-download",
                    "smoke-downloaded",
                    "verify-signatures",
                    "mark-uploaded",
                ],
            )

    def test_cell_failure_is_checkpointed_and_rethrown(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            spec = config.build_cells(worker_request(root))[0]
            paths = self._paths(root, spec)
            events: list[str] = []

            workspace = SimpleNamespace(
                cell_paths=lambda _spec: paths,
                prepare_cell=lambda _spec: model.CellContext.from_spec(spec, paths),
            )

            class Acceptance:
                def validate_built(self, _cell, _artifact):
                    events.append("validate-built")
                    raise model.PackageBuildError("validation stopped the cell")

            class Manifest:
                def start_cell(self, **_kwargs):
                    events.append("start-cell")
                    return {}

                def fail_cell(self, _entry, error):
                    self.error = error
                    events.append("fail-cell")

            cell_manifest = Manifest()
            services = SimpleNamespace(workspace=workspace, acceptance=Acceptance())
            with mock.patch.object(
                pipeline,
                "_build_package",
                side_effect=lambda _cell, _services: events.append("build-package")
                or root / "artifact.deb",
            ):
                with self.assertRaisesRegex(
                    model.PackageBuildError, "validation stopped"
                ):
                    pipeline._process_cell(
                        spec,
                        signing=SimpleNamespace(fingerprint=PINNED_FINGERPRINT),
                        services=services,
                        manifest=cell_manifest,
                    )
            self.assertEqual(
                events,
                ["start-cell", "build-package", "validate-built", "fail-cell"],
            )


class BuildAndStageContractTests(unittest.TestCase):
    def test_build_revalidates_complete_package_cmake_profile(self) -> None:
        build_script = (
            PACKAGING_ROOT / "scripts" / "build_and_stage.sh"
        ).read_text(encoding="utf-8")

        self.assertIn("cmake_cache_matches_package_profile", build_script)
        self.assertIn("require_package_cmake_profile", build_script)
        for key, value in (
            ("BIOSEQ_PACKAGE_BUILD", "ON"),
            ("BIOSEQ_BUILD_CORE", "ON"),
            ("BIOSEQ_BUILD_CACHE", "ON"),
            ("BIOSEQ_TEST_INDEXES", "OFF"),
            ("BIOSEQ_CACHE_BUILD_TESTS", "OFF"),
            ("BIOSEQ_CACHE_TESTING", "OFF"),
        ):
            with self.subTest(key=key):
                self.assertRegex(
                    build_script,
                    rf"(?m)^\s*{key}\s+{value}\s*\\?$",
                )

        profile_check = build_script.index(
            "\n  require_package_cmake_profile\n",
            build_script.index(
                'if [[ "${action}" == "build" ]]',
                build_script.index("require_package_cmake_profile()"),
            ),
        )
        build_targets = build_script.index(
            "--target postgresql_bioseq postgresql_bioseq_cache",
        )
        self.assertLess(profile_check, build_targets)

    def test_build_consumes_exact_generated_production_dso_inventory(self) -> None:
        manifest_name = "bioseq-package-target-artifacts.tsv"
        build_script = (
            PACKAGING_ROOT / "scripts" / "build_and_stage.sh"
        ).read_text(encoding="utf-8")
        root_cmake = (MAIN_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn(manifest_name, root_cmake)
        self.assertIn(manifest_name, build_script)
        for target, basename in (
            ("postgresql_bioseq", "bioseq.so"),
            ("postgresql_bioseq_cache", "bioseq_cache.so"),
        ):
            with self.subTest(target=target):
                self.assertRegex(
                    root_cmake,
                    rf"{target}(?:\\t|\t)\$<TARGET_FILE:{target}>",
                )
                self.assertRegex(
                    build_script,
                    rf"(?m)^[^\n]*{target}[^\n]*{basename}[^\n]*$",
                )

        self.assertIn("IFS=$'\\t'", build_script)
        self.assertRegex(
            build_script,
            (
                r'\[\[[^\]\n]*\$\{#[A-Za-z_][A-Za-z0-9_]*\[@\]\}'
                r'[^\]\n]*(?:==|-eq)[^\]\n]*"?2"?[^\]\n]*\]\]'
            ),
        )
        self.assertNotIn("-name bioseq.so", build_script)
        self.assertNotIn("-name bioseq_cache.so", build_script)

    def test_test_support_dso_uses_shared_manifest_closure_validator(
            self) -> None:
        helper = (
            MAIN_ROOT / "cmake" / "AddPostgreSQLExtension.cmake"
        ).read_text(encoding="utf-8")
        cache_cmake = (
            MAIN_ROOT / "bioseq_caching" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "function(bioseq_attach_extension_dso_closure TARGET_NAME)",
            helper,
        )
        self.assertIn("--dso-only", helper)
        self.assertIn("--require-complete-codegen", helper)
        self.assertRegex(
            cache_cmake,
            (
                r"bioseq_attach_extension_dso_closure\(\s*"
                r"postgresql_bioseq_cache_test_support[\s\S]*?"
                r"BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_MANIFEST"
                r"[\s\S]*?REQUIRE_COMPLETE_CODEGEN\)"
            ),
        )

    def test_install_enforces_cache_inclusive_exact_six_artifact_gate(self) -> None:
        build_script = (
            PACKAGING_ROOT / "scripts" / "build_and_stage.sh"
        ).read_text(encoding="utf-8")
        validator_namespace = runpy.run_path(
            str(PACKAGING_ROOT / "scripts" / "validate_package.py")
        )
        expected_basenames = frozenset(
            {
                "bioseq.so",
                "bioseq_cache.so",
                "bioseq--1.0.sql",
                "bioseq_cache--1.0.sql",
                "bioseq.control",
                "bioseq_cache.control",
            }
        )

        self.assertEqual(
            validator_namespace["REQUIRED_BASENAMES"],
            expected_basenames,
        )
        install_index = build_script.index(
            '"${cmake_bin}" --install "${build_dir}" --component bioseq'
        )
        validation_index = build_script.index("--staged-root", install_index)
        inventory_index = build_script.index(
            '"${package_metadata_dir}/package-files.list"',
            validation_index,
        )
        exact_count_index = build_script.index(
            '"6"',
            inventory_index,
        )
        self.assertIn(
            (
                '[[ "$(wc -l < "${package_metadata_dir}/package-files.list")" '
                '== "6" ]]'
            ),
            build_script,
        )
        self.assertLess(
            install_index,
            validation_index,
        )
        self.assertLess(
            validation_index,
            inventory_index,
        )
        self.assertLess(
            inventory_index,
            exact_count_index,
        )
        for option in (
            "--pg-config",
            "--postgres-major",
            "--architecture",
        ):
            with self.subTest(option=option):
                self.assertIn(
                    option,
                    build_script[validation_index:inventory_index],
                )


class ManifestAndLegacyTests(unittest.TestCase):
    def test_manifest_records_exact_provenance_before_any_cell_work(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "manifest.json"
            store = manifest.ManifestStore.create(
                path,
                architecture=model.Architecture.X64,
                requested_postgres=(17,),
                requested_distros=("debian_13",),
                bucket="valid-package-bucket",
                region="us-east-1",
                base_prefix="bioseq/packages",
                signing_fingerprint=PINNED_FINGERPRINT,
                source_revision="a" * 40,
                poetry_lock_sha256="b" * 64,
                python_environment={"python": "3.12.3", "poetry": "2.1.3"},
                clock=lambda: "2026-07-29T00:00:00Z",
            )
            self.assertTrue(path.is_file())
            self.assertEqual(store.payload["status"], "failed")
            self.assertEqual(store.payload["source_revision"], "a" * 40)
            self.assertEqual(store.payload["poetry_lock_sha256"], "b" * 64)
            self.assertEqual(
                store.payload["python_environment"],
                {"python": "3.12.3", "poetry": "2.1.3"},
            )

    def test_active_packaging_runtime_has_no_legacy_toolchain(self) -> None:
        legacy_name = "omni" + "package"
        forbidden = (
            legacy_name,
            legacy_name + "-target",
            "--" + legacy_name + "-bin",
            "cargo",
            "rustup",
            "rustc",
            ".cargo",
        )
        roots = (
            PACKAGING_ROOT / "build_packages.py",
            PACKAGING_ROOT / "config.yml",
            PACKAGING_ROOT / "bioseq_packaging",
            MAIN_ROOT / "infra" / "package_builder.py",
            MAIN_ROOT / "infra" / "package_build_infra" / "scripts" / "bootstrap.sh",
        )
        sources: list[Path] = []
        for root in roots:
            if root.is_dir():
                sources.extend(
                    path
                    for path in root.rglob("*")
                    if path.is_file()
                    and path.suffix in {".py", ".sh", ".toml", ".yml", ".yaml"}
                )
            elif root.is_file():
                sources.append(root)
        self.assertTrue(sources)
        for source in sources:
            content = source.read_text(encoding="utf-8").lower()
            for token in forbidden:
                with self.subTest(source=source, token=token):
                    self.assertNotIn(token, content)

        self.assertFalse(REPOSITORY_ROOT.joinpath(legacy_name + "-rs-master").exists())
        self.assertFalse(REPOSITORY_ROOT.joinpath(legacy_name + "-docs-master").exists())

    def test_directly_invoked_packaging_sources_are_executable(self) -> None:
        paths = [PACKAGING_ROOT / "build_packages.py"]
        paths.extend((PACKAGING_ROOT / "scripts").glob("*.sh"))
        paths.extend((PACKAGING_ROOT / "scripts").glob("*.py"))
        for path in paths:
            with self.subTest(path=path):
                self.assertTrue(path.stat().st_mode & stat.S_IXUSR, str(path))


if __name__ == "__main__":
    unittest.main()
