from __future__ import annotations

import contextlib
from dataclasses import replace
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import runpy
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
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
    workspace,
)
from bioseq_packaging.packagers import deb, pacman, rpm
from bioseq_packaging.repositories import RepositoryBuild
from bioseq_packaging.repositories import deb as deb_repository
from bioseq_packaging.repositories import pacman as pacman_repository
from bioseq_packaging.repositories import rpm as rpm_repository


PACKAGING_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = PACKAGING_ROOT.parents[1]
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
        source_dir=REPOSITORY_ROOT.resolve(),
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


class ReleaseFleetAssignmentTests(unittest.TestCase):
    def test_completed_package_cleanup_preserves_outputs_and_refuses_live_workers(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        cleanup = namespace["reclaim_collected_package_objects"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            tree = root / "debian_13/postgresql19"
            tree.mkdir(parents=True)
            for name in ("CMakeCache.txt", "one.o", "two.pcm", "bioseq.so", "report.json"):
                (tree / name).write_text("retained fixture")
            outside = root / "outside.o"
            outside.write_text("not in requested tree")
            (tree / "link.o").symlink_to(outside)
            cell = dict(distro="debian_13", postgres_major=19)
            with mock.patch.object(namespace["subprocess"], "check_output", return_value=(
                    "999999 python build_mcp/server.py --run-build package-linux")):
                self.assertIn("skipped", cleanup([cell], str(root)))
                self.assertTrue((tree / "one.o").is_file())
            with mock.patch.object(namespace["subprocess"], "check_output", return_value=""), \
                    mock.patch.object(namespace["shutil"], "disk_usage",
                                      return_value=SimpleNamespace(free=110 * 1024**3)):
                result = cleanup([cell], str(root))
                self.assertEqual(result["removed_files"], 2)
                self.assertFalse((tree / "one.o").exists())
                self.assertFalse((tree / "two.pcm").exists())
                for name in ("CMakeCache.txt", "bioseq.so", "report.json", "link.o"):
                    self.assertTrue((tree / name).exists())
                self.assertTrue(outside.exists())
                with self.assertRaisesRegex(ValueError, "Invalid package cache cell"):
                    cleanup([dict(cell, distro="../outside")], str(root))

    def test_coordinator_ip_refresh_only_changes_enrolled_narrow_rules(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        refresh = namespace["refresh_coordinator_ssh_ingress"]
        target = dict(profile="test3", region="us-east-1", group_id="sg-owned", rule_id="sgr-owned",
                      hosts=["test3-c7g"], current_cidr="192.0.2.1/32")
        rule = dict(GroupId="sg-owned", IsEgress=False, IpProtocol="tcp", FromPort=22, ToPort=22,
                    Description="Bioseq release coordinator current public IP", CidrIpv4="192.0.2.1/32")
        for case in ("rotated", "unchanged", "retired", "foreign_rule", "broad_rule"):
            state = dict(coordinator_ssh_ingress=[dict(target)])
            current_rule = dict(rule)
            if case == "unchanged":
                state["coordinator_ssh_ingress"][0]["current_cidr"] = "192.0.2.2/32"
            if case == "foreign_rule":
                current_rule["Description"] = "Someone else's SSH"
            if case == "broad_rule":
                current_rule["CidrIpv4"] = "0.0.0.0/0"
            run = mock.Mock(return_value=SimpleNamespace(stdout=json.dumps({
                "SecurityGroupRules": [current_rule]})))
            response = mock.MagicMock()
            response.__enter__.return_value.read.return_value = b"192.0.2.2\n"
            with self.subTest(case=case), mock.patch.dict(refresh.__globals__, {
                    "retirement_policy": mock.Mock(return_value=case == "retired"),
                    "event": mock.Mock()}), \
                    mock.patch.object(namespace["urllib"].request, "urlopen", return_value=response) as fetch, \
                    mock.patch.object(namespace["subprocess"], "run", run):
                refresh(state)
                if case == "rotated":
                    self.assertEqual(run.call_count, 2)
                    command = run.call_args.args[0]
                    self.assertEqual(command[:6], ["aws", "--profile", "test3", "--region", "us-east-1", "ec2"])
                    self.assertEqual(json.loads(command[-1])[0]["SecurityGroupRule"]["CidrIpv4"], "192.0.2.2/32")
                    self.assertEqual(state["coordinator_ssh_ingress"][0]["current_cidr"], "192.0.2.2/32")
                    refresh(state)
                    self.assertEqual(run.call_count, 2)
                elif case in ("foreign_rule", "broad_rule"):
                    self.assertEqual(run.call_count, 1)
                    self.assertEqual(state["coordinator_ssh_ingress"][0]["current_cidr"], "192.0.2.1/32")
                else:
                    run.assert_not_called()
                if case == "retired":
                    fetch.assert_not_called()

    def test_revalidation_preserves_binary_revision_without_a_reuse_gate(self) -> None:
        namespace = runpy.run_path(str(REPOSITORY_ROOT / "build_mcp/package_native.py"))
        revision = namespace["artifact_build_revision"]
        previous = {"packages": [{"sha256": "unchanged-bytes",
                                  "git_revision_informational_only": "original-build"}]}
        self.assertEqual(revision(previous, "unchanged-bytes", "new-validator",
                                  validation_only=True), "original-build")
        self.assertIsNone(revision({}, "unchanged-bytes", "new-validator", validation_only=True))
        self.assertIsNone(revision({"packages": None}, "unchanged-bytes", "new-validator",
                                  validation_only=True))
        self.assertEqual(revision(previous, "new-bytes", "new-build",
                                  validation_only=False), "new-build")

    def test_queued_repair_prefers_original_cache_unless_owner_is_retiring(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        dispatch = namespace["dispatch_available"]
        borrower, owner = "test3-m4", "test4-m7i"
        cell = dict(postgres_major=19, distro="fedora_44", architecture="x64")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "infra").mkdir()
            (root / "packages").mkdir()
            (root / "infra/package_matrix.json").write_text(json.dumps({
                "postgresql_majors": [], "distributions": []}))
            (root / "packages/manifest.json").write_text(json.dumps({"packages": []}))
            for retiring in (False, True):
                (root / "infra/package_retirements.json").write_text(json.dumps({
                    "hosts": {owner: {"stop_polling_after_epoch": 0}} if retiring else {}}))
                state = dict(jobs={h: dict(launched=True, observed=dict(status="completed"))
                                   for h in (borrower, owner)}, package_rebuilds={owner: [cell]})
                launch = mock.Mock(return_value=dict(status="running", pid=10))
                with self.subTest(retiring=retiring), mock.patch.dict(dispatch.__globals__, {
                        "ROOT": root, "OUTPUT": root, "STATE": root / "state.json",
                        "PLAN": {borrower: ["debian_13"], owner: ["fedora_44"]},
                        "launch": launch, "event": mock.Mock(),
                        "remote": mock.Mock(return_value=str(100 * 1024**3))}):
                    dispatch(state)
                    launch.assert_called_once_with(borrower if retiring else owner, cell=cell)

    def test_deferred_repairs_wait_for_fresh_coverage_and_never_duplicate_live_cells(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        dispatch = namespace["dispatch_available"]
        host = "test3-c7g"
        fresh = dict(postgres_major=19, distro="debian_13", architecture="arm64")
        repair = dict(fresh, postgres_major=17)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "infra").mkdir()
            (root / "packages").mkdir()
            (root / "infra/package_matrix.json").write_text(json.dumps({
                "postgresql_majors": [19], "distributions": [{"id": "debian_13"}]}))
            (root / "packages/manifest.json").write_text(json.dumps({"packages": [repair]}))
            state = dict(jobs={host: dict(launched=True, observed=dict(status="completed"))},
                         deferred_package_rebuilds={host: [repair]})
            launch = mock.Mock(return_value=dict(status="running", pid=10))
            with mock.patch.dict(dispatch.__globals__, {
                    "ROOT": root, "OUTPUT": root, "STATE": root / "state.json",
                    "PLAN": {host: ["debian_13"]}, "launch": launch, "event": mock.Mock(),
                    "remote": mock.Mock(return_value=str(100 * 1024**3))}):
                dispatch(state)
                launch.assert_called_once_with(host, cell=fresh)
                self.assertEqual(state["deferred_package_rebuilds"][host], [repair])
                dispatch(state)
                self.assertEqual(launch.call_count, 1)
                state["jobs"][host]["observed"]["status"] = "completed"
                state["jobs"]["peer-c7g"] = dict(
                    launched=True, cell=repair, observed=dict(status="running"))
                dispatch(state)
                self.assertEqual(launch.call_count, 1)
                self.assertEqual(state["deferred_package_rebuilds"][host], [repair])
                del state["jobs"]["peer-c7g"]
                dispatch(state)
                launch.assert_called_with(host, cell=repair)
                self.assertEqual(launch.call_count, 2)
                self.assertEqual(state["deferred_package_rebuilds"][host], [])

    def test_retirement_blocks_explicit_launch_before_remote_access(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        launch = namespace["launch"]
        with mock.patch.dict(launch.__globals__, {
                "retirement_policy": mock.Mock(return_value={"account_id": "retired"}),
                "state_on": mock.Mock(side_effect=AssertionError("contacted retired host"))}):
            with self.assertRaisesRegex(RuntimeError, "admission disabled"):
                launch("test2-c7g")

    def test_idle_host_borrows_pending_qualification_once_in_major_order(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        dispatch = namespace["dispatch_available"]
        host, owner = "test4-m7a", "test2-m4"
        candidates = [dict(postgres_major=major, distro="opensuse_16.0",
                           architecture="x64", build_host=owner, installed_sql_smoke={})
                      for major in (18, 17)]
        cell = dict(postgres_major=17, distro="opensuse_16.0", architecture="x64")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "infra").mkdir()
            (root / "packages").mkdir()
            (root / "infra/package_matrix.json").write_text(json.dumps({
                "postgresql_majors": [], "distributions": []}))
            (root / "packages/manifest.json").write_text(json.dumps({"packages": candidates}))
            state = dict(qualify_shared_tableam=True, jobs={
                host: dict(launched=True, observed=dict(status="completed")),
                owner: dict(launched=True, observed=dict(status="running"))})
            launch = mock.Mock(return_value=dict(status="running", pid=10))
            with mock.patch.dict(dispatch.__globals__, {
                    "ROOT": root, "OUTPUT": root, "STATE": root / "state.json",
                    "PLAN": {host: ["arch"], owner: ["opensuse_16.0"]},
                    "launch": launch, "event": mock.Mock(),
                    "remote": mock.Mock(return_value=str(100 * 1024**3))}):
                dispatch(state)
                launch.assert_called_once_with(host, cell=cell)
                self.assertEqual(state["tableam_qualification_attempted_cells"],
                                 ["pg17/opensuse_16.0/x64"])
                # Original owner becomes free while the borrowed cell is active.
                state["jobs"][owner]["observed"]["status"] = "completed"
                dispatch(state)
                launch.assert_called_with(owner, cell=dict(cell, postgres_major=18))
                self.assertEqual(launch.call_count, 2)

    def test_collection_whitelists_files_and_preserves_cross_host_revision(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        collect = namespace["collect_package_revision"]
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory)
            old = destination / "distro/bioseq.rpm"
            old.parent.mkdir()
            old.write_bytes(b"old revision")
            prior = dict(postgres_major=17, distro="opensuse_16.0", file="distro/bioseq.rpm",
                         bytes=old.stat().st_size, sha256=sha256(old), installed_sql_smoke={})
            original_manifest = destination / "original-manifest.json"
            original_manifest.write_text(json.dumps(dict(architecture="x64", packages=[prior])))
            payloads = {"distro/bioseq.rpm": b"qualified replacement", "sdk/runtime.rpm": b"SDK"}
            incoming = dict(prior, bytes=len(payloads[prior["file"]]),
                sha256=hashlib.sha256(payloads[prior["file"]]).hexdigest(),
                validation=dict(passed=True), installed_sql_smoke=dict(
                    passed=True, default_insert_search=True, default_persisted_search=True,
                    column_defaults_and_vacuum=True), postgres_sdk_support_files=[dict(
                        file="sdk/runtime.rpm", bytes=3, sha256=hashlib.sha256(b"SDK").hexdigest())])

            def transfer(command, **kwargs):
                self.assertIn("--files-from=-", command)
                self.assertIn("--from0", command)
                self.assertEqual(set(kwargs["input"].rstrip("\0").split("\0")), set(payloads))
                for name, data in payloads.items():
                    target = Path(command[-1]) / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)
                return SimpleNamespace(returncode=0)

            with mock.patch.object(subprocess, "run", side_effect=transfer), \
                    mock.patch.dict(collect.__globals__, {"write_release_manifest": mock.Mock()}):
                collect("borrower", dict(architecture="x64", packages=[incoming]),
                        "/remote/packages/", destination, ["ssh", "host"])
            replaced = json.loads(original_manifest.read_text())
            self.assertEqual(replaced["packages"], [])
            self.assertEqual(replaced["retired_packages"][0]["sha256"], prior["sha256"])
            self.assertEqual(old.read_bytes(), b"old revision")
            accepted = json.loads((destination / "borrower-manifest.json").read_text())["packages"][0]
            self.assertTrue(accepted["file"].startswith("collected_revisions/"))
            self.assertEqual((destination / accepted["file"]).read_bytes(), b"qualified replacement")
            self.assertEqual((destination / accepted["postgres_sdk_support_files"][0]["file"]).read_bytes(), b"SDK")
            # Another worker cannot overwrite a now-qualified cell.
            with mock.patch.object(subprocess, "run", side_effect=AssertionError("copied rejected files")):
                with self.assertRaisesRegex(RuntimeError, "Cross-host replacement"):
                    collect("other", dict(architecture="x64", packages=[dict(accepted)]),
                            "/remote/packages/", destination, ["ssh", "host"])

    def test_qualified_cross_host_repair_requires_exact_handoff(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        collect = namespace["collect_package_revision"]
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory)
            old = destination / "old.pkg"
            old.write_bytes(b"old")
            cell = dict(postgres_major=19, distro="arch", architecture="x64")
            prior = dict(postgres_major=19, distro="arch", file="old.pkg", bytes=3,
                sha256=sha256(old), validation=dict(passed=True), installed_sql_smoke=dict(
                    passed=True, default_insert_search=True, default_persisted_search=True,
                    column_defaults_and_vacuum=True))
            original = destination / "retired-host-manifest.json"
            original.write_text(json.dumps(dict(architecture="x64", packages=[prior])))
            incoming = dict(prior, file="new.pkg", sha256=hashlib.sha256(b"new").hexdigest())
            authority = dict(target_host="replacement", previous_host="retired-host",
                             previous_sha256=prior["sha256"], cell=cell)
            invalid = [dict(authority, target_host="other"),
                       dict(authority, previous_host="other"),
                       dict(authority, previous_sha256="0" * 64),
                       dict(authority, cell=dict(cell, postgres_major=20))]
            with mock.patch.object(subprocess, "run", side_effect=AssertionError("unexpected transfer")):
                for entry in invalid:
                    with self.subTest(entry=entry), self.assertRaisesRegex(RuntimeError, "Cross-host replacement"):
                        collect("replacement", dict(architecture="x64", packages=[dict(incoming)]),
                            "/remote/", destination, ["ssh", "host"],
                            replacement_authorizations=[entry])
                with self.assertRaisesRegex(RuntimeError, "Cross-host replacement"):
                    collect("replacement", dict(architecture="x64", packages=[
                        dict(incoming, installed_sql_smoke={})]), "/remote/", destination,
                        ["ssh", "host"], replacement_authorizations=[authority])

            def transfer(command, **kwargs):
                (Path(command[-1]) / "new.pkg").write_bytes(b"new")
                return SimpleNamespace(returncode=0)

            with mock.patch.object(subprocess, "run", side_effect=transfer), \
                    mock.patch.dict(collect.__globals__, {"write_release_manifest": mock.Mock()}):
                collect("replacement", dict(architecture="x64", packages=[incoming]),
                    "/remote/", destination, ["ssh", "host"], replacement_authorizations=[authority])
            retired = json.loads(original.read_text())
            self.assertEqual(retired["packages"], [])
            self.assertEqual(retired["retired_packages"][0]["sha256"], prior["sha256"])
            self.assertEqual(old.read_bytes(), b"old")
            accepted = json.loads((destination / "replacement-manifest.json").read_text())["packages"][0]
            self.assertEqual((destination / accepted["file"]).read_bytes(), b"new")

    def test_collection_rejects_bad_bytes_before_changing_manifest(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        collect = namespace["collect_package_revision"]
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory)
            prior = destination / "worker-manifest.json"
            prior.write_text(json.dumps(dict(architecture="x64", packages=[])))
            before = prior.read_bytes()
            incoming = dict(postgres_major=17, distro="arch", file="bioseq.pkg",
                            sha256=hashlib.sha256(b"good").hexdigest(), bytes=4)

            def transfer(command, **kwargs):
                (Path(command[-1]) / "bioseq.pkg").write_bytes(b"oops")
                return SimpleNamespace(returncode=0)

            with mock.patch.object(subprocess, "run", side_effect=transfer):
                with self.assertRaisesRegex(RuntimeError, "checksum/size/path"):
                    collect("worker", dict(architecture="x64", packages=[incoming]),
                            "/remote/packages/", destination, ["ssh", "host"])
            self.assertEqual(prior.read_bytes(), before)
            self.assertFalse((destination / "collected_revisions").exists())
            with mock.patch.object(subprocess, "run", side_effect=AssertionError("unsafe transfer")):
                with self.assertRaisesRegex(RuntimeError, "Unsafe advertised"):
                    collect("worker", dict(architecture="x64", packages=[dict(incoming, file="../outside")]),
                            "/remote/packages/", destination, ["ssh", "host"])

    def test_idle_cleanup_keeps_build_metadata_binaries_and_package_cache(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        reclaim = namespace["reclaim_retired_benchmark_intermediates"]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "CMakeLists.txt").write_text("project(test)")
            tree = root / "build/aws-x64-linux-e2e"
            objects = tree / "bioseq/CMakeFiles"
            objects.mkdir(parents=True)
            retained = [tree / "CMakeCache.txt", objects / "build.ninja", tree / "bioseq.dylib",
                        root / "build/package-linux/current.o", root / "results.yaml"]
            for path in retained:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("retained")
            for name in ("a.o", "a.pcm"):
                (objects / name).write_text("intermediate")
            generated = tree / "bioseq/generated/highway/pcm"
            generated.mkdir(parents=True)
            (generated / "a.pcm").write_text("intermediate")
            (generated / "source.cpp").write_text("retained")
            (objects / "linked.o").symlink_to(retained[-1])
            outside = root / "outside"
            outside.mkdir()
            (outside / "protected.o").write_text("retained")
            (objects / "symlinked_directory").symlink_to(outside, target_is_directory=True)
            observer = '999999 python -c \'print("server.py", "--run-build ")\'\n'
            with mock.patch.object(subprocess, "check_output", side_effect=[observer, ""]), \
                    mock.patch.object(namespace["shutil"], "disk_usage", return_value=SimpleNamespace(free=0)):
                result = reclaim(root)
            self.assertEqual(result["removed_files"], 3)
            self.assertEqual(result["removed_file_bytes"], 36)
            self.assertEqual(result["cleanup_revision"], 2)
            self.assertEqual((generated / "source.cpp").read_text(), "retained")
            self.assertTrue(objects.is_dir())
            self.assertTrue((objects / "linked.o").is_symlink())
            self.assertTrue((outside / "protected.o").exists())
            for path in retained:
                self.assertEqual(path.read_text(), "retained")

    def test_retired_cleanup_refuses_live_workers_containers_or_build_references(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        reclaim = namespace["reclaim_retired_benchmark_intermediates"]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "CMakeLists.txt").write_text("project(test)")
            cases = [(["999999 python server.py --run-build package-linux\n"], "build worker"),
                     (["", "container-id\n"], "container"),
                     ([f"999999 clang++ {root}/build/aws-x64-linux-e2e/input.cpp\n", ""], "referenced")]
            for outputs, reason in cases:
                with self.subTest(reason=reason), \
                        mock.patch.object(subprocess, "check_output", side_effect=outputs), \
                        mock.patch.object(Path, "unlink", side_effect=AssertionError("deleted while busy")):
                    self.assertIn(reason, reclaim(root)["skipped"])

    def test_idle_host_takes_only_missing_inactive_compatible_retries(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        dispatch = namespace["dispatch_available"]
        host, owner = "test4-m7i", "test2-m7a"
        missing = dict(postgres_major=19, distro="fedora_44", architecture="x64")
        running = dict(postgres_major=20, distro="arch", architecture="x64")
        arm = dict(postgres_major=18, distro="arch", architecture="arm64")
        accepted = dict(postgres_major=17, distro="fedora_44", architecture="x64")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "infra").mkdir()
            (root / "packages").mkdir()
            (root / "infra/package_matrix.json").write_text(json.dumps({
                "postgresql_majors": [], "distributions": [{"id": "fedora_44"}]}))
            (root / "packages/manifest.json").write_text(json.dumps({"packages": [accepted]}))
            state = dict(jobs={
                host: dict(launched=True, observed=dict(status="completed")),
                owner: dict(launched=True, cell=running, observed=dict(status="running")),
            }, package_rebuilds={owner: [arm, accepted, running, missing]})
            launch = mock.Mock(return_value=dict(status="running", pid=10))
            with mock.patch.dict(dispatch.__globals__, {
                    "ROOT": root, "OUTPUT": root, "STATE": root / "state.json",
                    "PLAN": {host: ["debian_13"], owner: ["fedora_44"]},
                    "launch": launch, "event": mock.Mock(),
                    "remote": mock.Mock(return_value=str(100 * 1024**3))}):
                dispatch(state)
                launch.assert_called_once_with(host, cell=missing)
                self.assertEqual(state["package_rebuilds"][owner], [arm, accepted, running])
                dispatch(state)
                self.assertEqual(launch.call_count, 1)

    def test_failed_cells_remain_visible_after_hosts_move_on(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        cells = [dict(postgres_major=major, distro="fedora_44", architecture="x64")
                 for major in (17, 18, 19, 20)]
        history = [dict(host="test2-m7a", previous_job=dict(
            cell=cell, observed=dict(status="failed", pid=number, log_file=f"/tmp/{number}.log")))
            for number, cell in enumerate(cells, 1)]
        history.append({"event": "unrelated_history_shape"})
        state = dict(history=history, jobs={"test2-m7a": dict(
            cell=cells[1], observed=dict(status="running", pid=5))},
            package_rebuilds={"test2-m7i": [cells[0]]})
        inventory = dict(packages=[dict(cells[2], installed_sql_smoke=dict(
            column_defaults_and_vacuum=True))])
        pending = namespace["pending_package_failures"](state, inventory)
        self.assertEqual([row["cell"]["postgres_major"] for row in pending], [17, 20])
        self.assertEqual([row["retry_queued"] for row in pending], [True, False])
        self.assertEqual(pending[1]["log_file"], "/tmp/4.log")
        state["jobs"]["test2-m7i"] = dict(cell=cells[3], observed=dict(status="completed"))
        self.assertEqual(len(namespace["pending_package_failures"](state, inventory)), 1)

    def test_worker_status_strips_colors_without_hiding_live_failed_compilation(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        diagnose = namespace["worker_diagnostics"]
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "compiler.log"
            log.write_text(
                "a.cppm:1:1: warning: harmless\n"
                "\x1b[1ma.cppm:8:3: \x1b[31merror: \x1b[0munknown member\n"
                "ld.lld: error: undefined symbol\n")
            state = dict(pid=123, status="running", log_file=str(log))
            with mock.patch.object(subprocess, "run", return_value=SimpleNamespace(
                    returncode=0, stdout="python3 /repo/build_mcp/server.py --run-build package-linux --debug false")):
                observed = diagnose(state, "package-linux")
            self.assertTrue(observed["pid_live"])
            self.assertEqual(observed["status"], "running")
            self.assertEqual(observed["compiler_errors"], [
                {"line": 2, "text": "a.cppm:8:3: error: unknown member"},
                {"line": 3, "text": "ld.lld: error: undefined symbol"}])
            with mock.patch.object(subprocess, "run", return_value=SimpleNamespace(
                    returncode=0, stdout="python3 unrelated.py")):
                self.assertFalse(diagnose(state, "package-linux")["pid_live"])

    def test_status_cli_does_not_acquire_lifecycle_lock_or_collect(self) -> None:
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        main = namespace["main"]
        snapshot = mock.Mock(return_value={"accepted_packages": 10})
        with mock.patch.dict(main.__globals__, {"status_snapshot": snapshot}), \
                mock.patch.object(sys, "argv", ["build_release_fleet.py", "--status"]), \
                mock.patch.object(Path, "mkdir", side_effect=AssertionError("status wrote a directory")), \
                mock.patch.object(namespace["fcntl"], "flock", side_effect=AssertionError("status locked lifecycle")), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            main()
        snapshot.assert_called_once_with()
        self.assertEqual(json.loads(output.getvalue()), {"accepted_packages": 10})

    def test_former_website_host_keeps_dispatching_packages(self) -> None:
        # Load scheduler logic without importing live benchmark-fleet clients.
        with mock.patch.dict(sys.modules, {"manage_e2e_fleet": SimpleNamespace()}), \
                mock.patch.object(sys, "path", list(sys.path)):
            namespace = runpy.run_path(str(REPOSITORY_ROOT / "infra/build_release_fleet.py"))
        host = "test3-m7i"
        mode = namespace["job_mode"]
        self.assertEqual(mode(host, {}), "website")
        previous_cell = dict(postgres_major=18, distro="debian_13", architecture="x64")
        self.assertEqual(mode(host, {"cell": previous_cell}), "package-linux")
        dispatch = namespace["dispatch_available"]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "infra").mkdir()
            (root / "packages").mkdir()
            (root / "infra/package_matrix.json").write_text(json.dumps({
                "postgresql_majors": [18, 19],
                "distributions": [{"id": "debian_13"}],
            }))
            (root / "packages/manifest.json").write_text('{"packages": []}')
            state = {"jobs": {host: {
                "launched": True, "mode": "website", "collected": True,
                "observed": {"status": "failed", "pid": 1},
            }}, "retired_intermediate_cleanup": {host: {}}}
            launch = mock.Mock(return_value={"status": "running", "pid": 3})
            remote = mock.Mock(return_value=str(40 * 1024**3))
            with mock.patch.dict(dispatch.__globals__, {
                    "ROOT": root, "OUTPUT": root, "STATE": root / "state.json",
                    "PLAN": {host: ["website"]}, "launch": launch,
                    "remote": remote, "event": mock.Mock()}):
                dispatch(state)
                launch.assert_not_called()  # An unaccepted website still takes priority.
                state["jobs"][host].update(mode="package-linux", cell=previous_cell)
                dispatch(state)
                launch.assert_not_called()  # Host disk shortage must not consume a cell.
                self.assertFalse(state.get("attempted_cells"))
                self.assertIn(host, state["package_disk_attention"])
                remote.return_value = str(100 * 1024**3)
                dispatch(state)
                self.assertNotIn(host, state["package_disk_attention"])
                expected = dict(postgres_major=19, distro="debian_13", architecture="x64")
                launch.assert_called_once_with(host, cell=expected)
                self.assertEqual(state["jobs"][host]["cell"], expected)
                self.assertEqual(state["jobs"][host]["observed"]["pid"], 3)
                state["validation_rechecks"] = {host: previous_cell}
                dispatch(state)
                self.assertEqual(launch.call_count, 1)  # Never interrupt a running compile.
                state["jobs"][host]["observed"]["status"] = "completed"
                dispatch(state)
                launch.assert_called_with(host, cell=previous_cell, validate_existing_only=True)
                self.assertTrue(state["jobs"][host]["validation_only"])
                self.assertFalse(state["validation_rechecks"])


class PostgresHeaderIdentityTests(unittest.TestCase):
    def test_content_changes_with_preserved_mtime_and_duplicate_roots(self) -> None:
        identify = runpy.run_path(str(
            PACKAGING_ROOT / "scripts" / "postgres_header_identity.py"
        ))["header_identity"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = root / "postgres.h"
            header.write_text("#define SDK_VALUE 1\n")
            before = header.stat()
            original = identify([root])
            self.assertEqual(original, identify([root, root]))
            header.write_text("#define SDK_VALUE 2\n")
            os.utime(header, ns=(before.st_atime_ns, before.st_mtime_ns))
            self.assertNotEqual(original, identify([root]))


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
    def test_catalog_is_the_exact_native_package_matrix(self) -> None:
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
        self.assertEqual(
            config.runtime_dependencies(model.DistroId.ARCH, 19),
            ("bioseq-postgresql19-runtime=19beta3-1", "vulkan-icd-loader"),
        )
        self.assertEqual(
            config.runtime_dependencies(model.DistroId.OPENSUSE_16_0, 20),
            ("bioseq-postgresql20-runtime = 20.0devel20260924-1", "libvulkan1"),
        )

    def test_fedora_prerelease_sdk_is_arm_only_and_keeps_native_runtime_identity(self) -> None:
        from bioseq_packaging import postgres_sdk

        distro = config.distro_spec(model.DistroId.FEDORA_44)
        for major in (17, 18, 19, 20):
            for architecture in model.Architecture:
                with self.subTest(major=major, architecture=architecture):
                    needs_sdk = major >= 19 and architecture is model.Architecture.ARM64
                    request = SimpleNamespace(architecture=architecture)
                    cell = SimpleNamespace(distro=distro, postgres_major=major, request=request)
                    self.assertEqual(postgres_sdk.required(cell), needs_sdk)
                    dependencies = config.runtime_dependencies(distro.distro_id, major, architecture)
                    self.assertEqual(rpm._runtime_dependencies(cell), dependencies)
                    self.assertEqual(dependencies[1], "vulkan-loader")
                    if needs_sdk:
                        pin = json.loads((PACKAGING_ROOT / "postgres_sdks.json").read_text())[str(major)]
                        self.assertEqual(dependencies[0],
                                         f"bioseq-postgresql{major}-runtime = {pin['version']}-1")
                    else:
                        self.assertEqual(dependencies[0], f"postgresql{major}-server")

    def test_private_sdk_rpm_path_exception_is_exact_and_subprocess_scoped(self) -> None:
        namespace = runpy.run_path(str(PACKAGING_ROOT / 'scripts/prepare_postgres_sdk.py'))
        environment_for = namespace['private_runtime_rpm_environment']
        with tempfile.TemporaryDirectory() as raw:
            payload = Path(raw)
            (payload / 'postgres').write_bytes(b'\x7fELFfixture')
            (payload / 'COPYRIGHT').write_text('not an ELF')
            (payload / 'postgres-link').symlink_to(payload / 'postgres')
            for major in (19, 20):
                prefix = Path(f'/opt/pgsql-{major}')
                with self.subTest(major=major), \
                        mock.patch.dict(os.environ, {'QA_RPATHS': '0xffff'}), \
                        mock.patch('subprocess.check_output', return_value=(
                            f' 0x000000001d (RUNPATH) Library runpath: [{prefix}/lib]\n')) as readelf:
                    environment = environment_for(payload, prefix)
                    self.assertEqual(environment['QA_RPATHS'], '0x0002')
                    self.assertEqual(os.environ['QA_RPATHS'], '0xffff')
                    readelf.assert_called_once_with(
                        ['readelf', '-d', str(payload / 'postgres')], text=True)
                for rejected in ('', '/tmp/build/lib', '$ORIGIN/../lib',
                                 f'{prefix}/lib:/usr/lib', f'{prefix}/lib/../lib'):
                    with self.subTest(major=major, rejected=rejected), \
                            mock.patch('subprocess.check_output', return_value=(
                                f' (RPATH) Library rpath: [{rejected}]\n')):
                        with self.assertRaisesRegex(RuntimeError, 'Unexpected private SDK'):
                            environment_for(payload, prefix)
            with mock.patch('subprocess.check_output', return_value=' (NEEDED) [libc.so.6]\n'):
                self.assertEqual(environment_for(payload, Path('/opt/pgsql-19'))['QA_RPATHS'], '0x0002')

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
    def test_source_projection_excludes_only_the_sealed_papers_location(self) -> None:
        self.assertTrue(workspace.is_source_excluded("main/papers"))
        self.assertTrue(workspace.is_source_excluded("main/papers/private/file.txt"))
        self.assertFalse(workspace.is_source_excluded("papers"))
        self.assertFalse(workspace.is_source_excluded("bioseq/papers"))

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
    def test_compiler_parallelism_respects_memory_and_cpu_budget(self) -> None:
        for gib, cpus, ordinary, heavy in (
            (1, 4, 1, 1), (8, 4, 2, 1), (16, 4, 4, 2),
            (32, 8, 4, 4), (64, 2, 2, 2), (16, 1, 1, 1), (8, None, 1, 1),
        ):
            for policy, expected in (
                (config.package_build_parallelism, ordinary),
                (config.package_heavy_build_parallelism, heavy),
            ):
                with self.subTest(gib=gib, cpus=cpus, policy=policy.__name__), mock.patch.object(
                    config.os, "sysconf", side_effect=(gib * 1024**3 // 4096, 4096)
                ), mock.patch.object(config.os, "cpu_count", return_value=cpus):
                    self.assertEqual(policy(), expected)
        with mock.patch.object(config.os, "sysconf", side_effect=OSError("unavailable")):
            self.assertEqual(config.package_build_parallelism(), 1)
            self.assertEqual(config.package_heavy_build_parallelism(), 1)

    def test_deb_and_rpm_metadata_and_build_plans_are_direct_and_optimized(
        self,
    ) -> None:
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
            self.assertEqual(
                deb_environment["BIOSEQ_TEMPLATE_FLATTEN_PROFILE"],
                "all_alphabets_uint64",
            )
            self.assertEqual(
                tuple(config.SOURCE_PROJECTION_EXCLUDES),
                tuple(
                    deb_environment[
                        "BIOSEQ_SOURCE_PROJECTION_EXCLUDES"
                    ].splitlines()
                ),
            )
            self.assertIn(
                "/main/papers/",
                config.SOURCE_PROJECTION_EXCLUDES,
            )
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
            self.assertEqual(
                rpm_environment["BIOSEQ_TEMPLATE_FLATTEN_PROFILE"],
                "all_alphabets_uint64",
            )
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
                    self.assertEqual(
                        variables["template_flatten_profile"], "all_alphabets_uint64"
                    )
                    environment = pacman.build_environment(cell)
                    self.assertEqual(
                        environment["BIOSEQ_TEMPLATE_FLATTEN_PROFILE"],
                        "all_alphabets_uint64",
                    )
                    self.assertEqual(
                        environment["BIOSEQ_PACKAGE_PARALLELISM"],
                        str(config.package_build_parallelism()),
                    )
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
                    self.assertLess(
                        script.index("PKGEXT='.pkg.tar.zst'"),
                        script.index("/source/infra/packaging/scripts/prepare_build_environment.sh"),
                    )
                    self.assertEqual(len(plan.mounts), 4)
                    self.assertEqual(
                        plan.capabilities,
                        frozenset({model.ContainerCapability.SYS_NICE}),
                    )

    def test_smoke_cleanup_preserves_failure_and_server_diagnostics(self) -> None:
        script = (PACKAGING_ROOT / "scripts" / "smoke_installed_package.sh").read_text()
        cleanup = script[script.index("cleanup() {"):script.index("trap cleanup EXIT")]
        with tempfile.TemporaryDirectory() as raw:
            log = Path(raw) / "postgres.log"
            log.write_text("TRAP: server failure detail\n" + "verbose shutdown detail\n" * 80)
            for status in (0, 3):
                with self.subTest(status=status):
                    result = subprocess.run(
                        ["bash", "-c", 'set -Eeuo pipefail\nstarted=false\n'
                         'log_file="$1"\n' + cleanup + '\ntrap cleanup EXIT\nexit "$2"',
                         "smoke-cleanup", str(log), str(status)],
                        capture_output=True, text=True,
                    )
                    self.assertEqual(result.returncode, status)
                    self.assertEqual(result.stdout, "")
                    self.assertEqual("server failure detail" in result.stderr, status != 0)
                    if status:
                        self.assertTrue(result.stderr.endswith("TRAP: server failure detail\n"))

    def test_smoke_failure_retains_full_trace_outside_clipped_error(self) -> None:
        namespace = runpy.run_path(str(PACKAGING_ROOT / "scripts/smoke_test_package.py"))
        run_smoke = namespace["run_smoke"]
        detail = "important first stack frame\n" + "stack frame\n" * 1000 + "assertion\n"
        error = subprocess.CalledProcessError(1, ["docker"], output="SQL stage\n", stderr=detail)
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "smoke.log"
            with mock.patch.object(subprocess, "run", side_effect=error):
                with self.assertRaises(namespace["SmokeError"]) as caught:
                    run_smoke(["docker"], diagnostic_path=path)
            self.assertEqual(path.read_text(), "SQL stage\n" + detail)
            self.assertTrue(str(caught.exception).endswith(str(path)))

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


class PackageOptimizationContractTests(unittest.TestCase):
    @staticmethod
    def _write_compile_database(directory: Path, entries: object) -> Path:
        path = directory / "compile_commands.json"
        path.write_text(json.dumps(entries), encoding="utf-8")
        return path

    @staticmethod
    def _verify_compile_database(path: Path) -> int:
        namespace = runpy.run_path(
            str(
                PACKAGING_ROOT
                / "scripts"
                / "verify_compile_optimization.py"
            )
        )
        return namespace["verify"](path)

    def test_compile_database_verifier_accepts_arguments_and_command_o3(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as raw:
            path = self._write_compile_database(
                Path(raw),
                [
                    {
                        "file": "first.cpp",
                        "arguments": [
                            "clang++",
                            "-stdlib=libc++",
                            "-O3",
                            "-c",
                            "first.cpp",
                        ],
                    },
                    {
                        "file": "second.cpp",
                        "command": "clang++ -O3 -c second.cpp",
                    },
                ],
            )
            self.assertEqual(self._verify_compile_database(path), 2)

    def test_compile_database_verifier_rejects_non_o3_profiles(self) -> None:
        invalid_arguments = {
            "missing": ["clang++", "-c", "unit.cpp"],
            "O0": ["clang++", "-O0", "-c", "unit.cpp"],
            "O1": ["clang++", "-O1", "-c", "unit.cpp"],
            "O2": ["clang++", "-O2", "-c", "unit.cpp"],
            "Og": ["clang++", "-Og", "-c", "unit.cpp"],
            "Os": ["clang++", "-Os", "-c", "unit.cpp"],
            "Oz": ["clang++", "-Oz", "-c", "unit.cpp"],
            "Ofast": ["clang++", "-Ofast", "-c", "unit.cpp"],
            "mixed": ["clang++", "-O3", "-O0", "-c", "unit.cpp"],
        }
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            for name, arguments in invalid_arguments.items():
                with self.subTest(name=name):
                    path = self._write_compile_database(
                        directory,
                        [{"file": "unit.cpp", "arguments": arguments}],
                    )
                    with self.assertRaisesRegex(RuntimeError, "explicit -O3"):
                        self._verify_compile_database(path)

    def test_compile_database_verifier_rejects_lto(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            for flag in ("-flto", "-flto=thin"):
                with self.subTest(flag=flag):
                    path = self._write_compile_database(
                        directory,
                        [
                            {
                                "file": "unit.cpp",
                                "arguments": [
                                    "clang++",
                                    "-O3",
                                    flag,
                                    "-c",
                                    "unit.cpp",
                                ],
                            }
                        ],
                    )
                    with self.assertRaisesRegex(RuntimeError, "forbidden LTO"):
                        self._verify_compile_database(path)

    def test_compile_database_verifier_rejects_malformed_inputs(self) -> None:
        invalid_databases = {
            "empty": [],
            "not-a-list": {},
            "non-object-entry": [[]],
            "invalid-arguments": [
                {"file": "unit.cpp", "arguments": ["clang++", 3]}
            ],
        }
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            for name, entries in invalid_databases.items():
                with self.subTest(name=name):
                    path = self._write_compile_database(directory, entries)
                    with self.assertRaises(RuntimeError):
                        self._verify_compile_database(path)

            malformed_path = directory / "malformed.json"
            malformed_path.write_text("{", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "cannot read"):
                self._verify_compile_database(malformed_path)

    def test_package_authorities_pin_o3_without_obsolete_profiles(self) -> None:
        root_cmake = (REPOSITORY_ROOT / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        presets = (REPOSITORY_ROOT / "CMakePresets.json").read_text(
            encoding="utf-8"
        )
        bioseq_cmake = (
            REPOSITORY_ROOT / "bioseq" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        postgres_source_graph = (
            REPOSITORY_ROOT / "cmake" / "bioseq_postgres_source_graph.cmake"
        ).read_text(encoding="utf-8")
        pipeline = (
            REPOSITORY_ROOT / "bioseq" / "backend" / "pipeline" / "pipe.cppm"
        ).read_text(encoding="utf-8")
        build_script = (
            PACKAGING_ROOT / "scripts" / "build_and_stage.sh"
        ).read_text(encoding="utf-8")
        codegen_cmake = (
            REPOSITORY_ROOT / "cmake" / "bioseq_codegen.cmake"
        ).read_text(encoding="utf-8")
        runtime_probe = (
            PACKAGING_ROOT / "scripts" / "validate_static_runtime.py"
        ).read_text(encoding="utf-8")

        release_flags = "-O3 -DNDEBUG -fno-omit-frame-pointer"
        self.assertIn(
            'if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")',
            root_cmake,
        )
        self.assertIn(
            "foreach(_bioseq_package_language IN ITEMS C CXX)",
            root_cmake,
        )
        self.assertIn(release_flags, root_cmake)
        self.assertGreaterEqual(build_script.count(release_flags), 4)
        self.assertEqual(codegen_cmake.count(release_flags), 2)
        self.assertIn('"-O3",', runtime_probe)
        self.assertNotIn('"-O0",', runtime_probe)

        for triplet_name in (
            "arm64-linux-libcxx.cmake",
            "x64-linux-libcxx.cmake",
        ):
            with self.subTest(triplet=triplet_name):
                triplet = (
                    REPOSITORY_ROOT / "cmake" / "triplets" / triplet_name
                ).read_text(encoding="utf-8")
                self.assertIn('VCPKG_C_FLAGS_RELEASE "-O3"', triplet)
                self.assertIn('VCPKG_CXX_FLAGS_RELEASE "-O3"', triplet)
                self.assertNotIn('FLAGS_RELEASE "-O0"', triplet)

        former_profile_authorities = (
            root_cmake,
            presets,
            bioseq_cmake,
            postgres_source_graph,
            pipeline,
        )
        for token in (
            "BIOSEQ_PACKAGE_O0_PROFILE",
            "BIOSEQ_PIPELINE_FORCE_INLINE",
            "BIOSEQ_GCC_TYPED_DISPATCH_OPT_LEVEL",
        ):
            for source in former_profile_authorities:
                with self.subTest(token=token):
                    self.assertNotIn(token, source)
        self.assertNotIn("__OPTIMIZE__", pipeline)
        self.assertIn(
            "BIOSEQ_TEMPLATE_DISPATCH_FORCE_INLINE=0",
            postgres_source_graph,
        )
        self.assertIn("preset -O3", postgres_source_graph)


class CompilerLauncherWatchdogTests(unittest.TestCase):
    def setUp(self) -> None:
        self.launcher = PACKAGING_ROOT / "scripts" / "compiler_launcher.sh"

    @staticmethod
    def _write_executable(path: Path, source: str) -> None:
        path.write_text(source, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)

    def _tool_environment(self, root: Path) -> tuple[dict[str, str], Path, Path]:
        tools = root / "tools"
        tools.mkdir()
        self._write_executable(
            tools / "setsid",
            "#!/usr/bin/env python3\n"
            "import os, sys\n"
            "os.setsid()\n"
            "os.execvp(sys.argv[1], sys.argv[1:])\n",
        )
        self._write_executable(
            tools / "chrt",
            "#!/usr/bin/env bash\n"
            "set -euo pipefail\n"
            "[[ $1 == --rr && $2 == 1 ]]\n"
            "shift 2\n"
            "exec \"$@\"\n",
        )
        compiler = root / "fake-compiler.py"
        pid_file = root / "compiler-pids"
        self._write_executable(
            compiler,
            "#!/usr/bin/env python3\n"
            "import os, pathlib, subprocess, sys, time\n"
            "if len(sys.argv) == 2 and sys.argv[1] == '--version':\n"
            "    print('fake-clang 22.1.6')\n"
            "    raise SystemExit(0)\n"
            "if 'BIOSEQ_TEST_EXIT_STATUS' in os.environ:\n"
            "    raise SystemExit(int(os.environ['BIOSEQ_TEST_EXIT_STATUS']))\n"
            "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(120)'])\n"
            "pathlib.Path(os.environ['BIOSEQ_TEST_PID_FILE']).write_text(\n"
            "    f'{os.getpid()}\\n{child.pid}\\n', encoding='ascii')\n"
            "time.sleep(120)\n",
        )
        environment = dict(os.environ)
        environment.update(
            {
                "PATH": str(tools) + os.pathsep + environment.get("PATH", ""),
                "BIOSEQ_COMPILER_WATCHDOG_TEST_MODE": "1",
                "BIOSEQ_COMPILER_WATCHDOG_TIMEOUT_SEC": "1",
                "BIOSEQ_COMPILER_WATCHDOG_GRACE_SEC": "1",
                "BIOSEQ_TEMPLATE_FLATTEN_PROFILE": "all_alphabets_uint64",
                "BIOSEQ_TEST_PID_FILE": str(pid_file),
            }
        )
        return environment, compiler, pid_file

    @staticmethod
    def _wait_for_pid_file(path: Path) -> list[int]:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if path.exists():
                values = [int(value) for value in path.read_text(encoding="ascii").split()]
                if len(values) == 2:
                    return values
            time.sleep(0.05)
        raise AssertionError("fake compiler did not publish its process identities")

    @staticmethod
    def _assert_processes_absent(pids: list[int]) -> None:
        deadline = time.monotonic() + 10
        remaining = set(pids)
        while remaining and time.monotonic() < deadline:
            for pid in tuple(remaining):
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    remaining.remove(pid)
                except PermissionError as error:
                    raise AssertionError(f"cannot inspect fake compiler pid {pid}") from error
            if remaining:
                time.sleep(0.05)
        if remaining:
            raise AssertionError(f"compiler process group survived: {sorted(remaining)}")

    def test_timeout_kills_compiler_group_and_emits_redacted_marker(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            environment, compiler, pid_file = self._tool_environment(root)
            result = subprocess.run(
                [
                    str(self.launcher),
                    str(compiler),
                    "-DPRIVATE_ARG=must-not-leak",
                    "/sealed/source/persist_queue_publish.cppm",
                ],
                env=environment,
                capture_output=True,
                text=True,
                timeout=60,
                check=False,
            )
            pids = self._wait_for_pid_file(pid_file)
            self.assertEqual(result.returncode, 124)
            markers = [
                line
                for line in result.stderr.splitlines()
                if line.startswith("BIOSEQ_COMPILER_WATCHDOG_TIMEOUT ")
            ]
            self.assertEqual(len(markers), 1)
            marker = markers[0]
            for field in (
                "source=persist_queue_publish.cppm",
                "profile=all_alphabets_uint64",
                "compiler_version=fake-clang_22.1.6",
                "argv_sha256=",
                "root_pid=",
                "pgid=",
                "pids=",
                "rss_kib=",
                "timeout_sec=1",
                "grace_sec=1",
            ):
                self.assertIn(field, marker)
            self.assertNotIn("must-not-leak", result.stderr)
            self._assert_processes_absent(pids)

    def test_cancellation_kills_compiler_group_without_timeout_marker(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            environment, compiler, pid_file = self._tool_environment(root)
            environment["BIOSEQ_COMPILER_WATCHDOG_TIMEOUT_SEC"] = "30"
            process = subprocess.Popen(
                [str(self.launcher), str(compiler), "unit.cppm"],
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            pids = self._wait_for_pid_file(pid_file)
            process.terminate()
            _stdout, stderr = process.communicate(timeout=15)
            self.assertEqual(process.returncode, 143)
            self.assertNotIn("BIOSEQ_COMPILER_WATCHDOG_TIMEOUT", stderr)
            self._assert_processes_absent(pids)

    def test_production_rejects_watchdog_limit_overrides(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            environment, compiler, pid_file = self._tool_environment(root)
            environment.pop("BIOSEQ_COMPILER_WATCHDOG_TEST_MODE")
            result = subprocess.run(
                [str(self.launcher), str(compiler), "unit.cppm"],
                env=environment,
                capture_output=True,
                text=True,
                timeout=5,
                check=False,
            )
            self.assertEqual(result.returncode, 64)
            self.assertIn("cannot be overridden", result.stderr)
            self.assertFalse(pid_file.exists())

    def test_fast_compiler_does_not_wait_for_watchdog(self) -> None:
        for expected_status in (0, 23) * 3:
            with self.subTest(status=expected_status), tempfile.TemporaryDirectory() as raw:
                environment, compiler, _ = self._tool_environment(Path(raw))
                environment.update({
                    "BIOSEQ_TEST_EXIT_STATUS": str(expected_status),
                    "BIOSEQ_COMPILER_WATCHDOG_TIMEOUT_SEC": "30",
                })
                process = subprocess.Popen(
                    [str(self.launcher), str(compiler), "unit.cppm"],
                    env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    text=True, start_new_session=True,
                )
                try:
                    _, stderr = process.communicate(timeout=8)
                    self.assertEqual(process.returncode, expected_status)
                    self.assertNotIn("BIOSEQ_COMPILER_WATCHDOG_TIMEOUT", stderr)
                finally:
                    # Bound the launcher and its single-process timer on failure.
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()

    def test_compiler_status_is_preserved_without_timeout_diagnostic(self) -> None:
        for expected_status in (0, 23):
            with (
                self.subTest(status=expected_status),
                tempfile.TemporaryDirectory() as raw,
            ):
                root = Path(raw)
                environment, compiler, pid_file = self._tool_environment(root)
                environment["BIOSEQ_TEST_EXIT_STATUS"] = str(expected_status)
                result = subprocess.run(
                    [str(self.launcher), str(compiler), "unit.cppm"],
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=5,
                    check=False,
                )
                self.assertEqual(result.returncode, expected_status)
                self.assertNotIn("BIOSEQ_COMPILER_WATCHDOG_TIMEOUT", result.stderr)
                self.assertFalse(pid_file.exists())


class BuildAndStageContractTests(unittest.TestCase):
    def test_cppast_overlay_receives_pinned_llvm_config(self) -> None:
        build_script = (
            PACKAGING_ROOT / "scripts" / "build_and_stage.sh"
        ).read_text(encoding="utf-8")
        cppast_port = (
            REPOSITORY_ROOT / "overlays" / "cppast" / "portfile.cmake"
        ).read_text(encoding="utf-8")

        self.assertIn(
            'export LLVM_CONFIG_BINARY="${llvm_root}/bin/llvm-config"',
            build_script,
        )
        self.assertIn("DEFINED ENV{LLVM_CONFIG_BINARY}", cppast_port)
        self.assertIn(
            'EXISTS "$ENV{LLVM_CONFIG_BINARY}"',
            cppast_port,
        )
        self.assertIn(
            'set(_cppast_llvm_config "$ENV{LLVM_CONFIG_BINARY}")',
            cppast_port,
        )

    def test_build_revalidates_complete_package_cmake_profile(self) -> None:
        build_script = (
            PACKAGING_ROOT / "scripts" / "build_and_stage.sh"
        ).read_text(encoding="utf-8")

        self.assertIn("cmake_cache_matches_package_profile", build_script)
        self.assertIn("require_package_cmake_profile", build_script)
        self.assertIn("from bioseq_packaging.config import package_heavy_build_parallelism", build_script)
        self.assertIn('-DBIOSEQ_HEAVY_JOBS="${heavy_parallelism}"', build_script)
        invariant_body = build_script.split(
            "package_cmake_invariants() {", maxsplit=1
        )[1].split("\n}", maxsplit=1)[0]
        configure_body = build_script.split(
            '"${cmake_bin}" -S "${persistent_source_dir}"',
            maxsplit=1,
        )[1].split("\n  fi", maxsplit=1)[0]
        configured_cache_keys = set(
            re.findall(r"-D([A-Za-z0-9_]+)(?::[^=]+)?=", configure_body)
        )
        for key in configured_cache_keys:
            with self.subTest(configured_cache_key=key):
                self.assertRegex(
                    invariant_body,
                    rf"(?m)^\s*{re.escape(key)}\s+",
                )
        for key, value in (
            ("CMAKE_BUILD_TYPE", "Release"),
            ("CMAKE_C_COMPILER_LAUNCHER", "${compiler_launcher}"),
            ("CMAKE_CXX_COMPILER_LAUNCHER", "${compiler_launcher}"),
            ("CMAKE_C_FLAGS", '""'),
            ("CMAKE_CXX_FLAGS", "-stdlib=libc++"),
            (
                "CMAKE_C_FLAGS_RELEASE",
                "-O3 -DNDEBUG -fno-omit-frame-pointer",
            ),
            (
                "CMAKE_CXX_FLAGS_RELEASE",
                "-O3 -DNDEBUG -fno-omit-frame-pointer",
            ),
            (
                "CMAKE_EXE_LINKER_FLAGS",
                "-stdlib=libc++ -static-libstdc++ -static-libgcc",
            ),
            ("CMAKE_SKIP_RPATH", "ON"),
            (
                "BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON",
                "${libcxx_modules}",
            ),
            ("BIOSEQ_PACKAGE_LIBCXX_ARCHIVE", "${libcxx_archive}"),
            (
                "BIOSEQ_PACKAGE_LIBCXXABI_ARCHIVE",
                "${libcxxabi_archive}",
            ),
            (
                "BIOSEQ_PACKAGE_LIBUNWIND_ARCHIVE",
                "${libunwind_archive}",
            ),
            ("BIOSEQ_PACKAGE_BUILD", "ON"),
            ("BIOSEQ_HEAVY_JOBS", "${heavy_parallelism}"),
            ("BIOSEQ_BUILD_CORE", "ON"),
            ("BIOSEQ_BUILD_CACHE", "ON"),
            ("BIOSEQ_TEST_INDEXES", "OFF"),
            ("BIOSEQ_CACHE_BUILD_TESTS", "OFF"),
            ("BIOSEQ_CACHE_TESTING", "OFF"),
        ):
            with self.subTest(key=key):
                self.assertRegex(
                    build_script,
                    rf'(?m)^\s*{key}\s+(?:{re.escape(value)}|"{re.escape(value)}")\s*\\?$',
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
        optimization_checks = [
            match.start()
            for match in re.finditer(
                "verify_compile_optimization.py",
                build_script,
            )
        ]
        self.assertEqual(len(optimization_checks), 2)
        self.assertLess(optimization_checks[0], build_targets)
        self.assertGreater(optimization_checks[1], build_targets)
        self.assertIn("mapfile -t compile_databases", build_script)

    def test_build_consumes_exact_generated_production_dso_inventory(self) -> None:
        manifest_name = "bioseq-package-target-artifacts.tsv"
        build_script = (
            PACKAGING_ROOT / "scripts" / "build_and_stage.sh"
        ).read_text(encoding="utf-8")
        root_cmake = (REPOSITORY_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

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
            REPOSITORY_ROOT / "cmake" / "add_postgresql_extension.cmake"
        ).read_text(encoding="utf-8")
        cache_cmake = (
            REPOSITORY_ROOT / "bioseq_caching" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        bioseq_cmake = (
            REPOSITORY_ROOT / "bioseq" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "function(bioseq_attach_extension_dso_closure TARGET_NAME)",
            helper,
        )
        self.assertIn("--dso-only", helper)
        self.assertIn('--dso-contract "${ARG_CONTRACT}"', helper)
        self.assertIn("--require-complete-codegen", helper)
        self.assertRegex(
            helper,
            (
                r"if\(NOT DEFINED ARG_CONTRACT "
                r"OR ARG_CONTRACT STREQUAL \"\"\)"
                r"\s*set\(ARG_CONTRACT bioseq\)"
            ),
        )
        self.assertNotIn("DSO_CLOSURE_CONTRACT manifest", bioseq_cmake)
        self.assertRegex(
            cache_cmake,
            (
                r"bioseq_add_postgresql_extension\(\s*postgresql_bioseq_cache"
                r"[\s\S]*?DSO_CLOSURE_CONTRACT manifest"
                r"[\s\S]*?REQUIRE_COMPLETE_CODEGEN"
            ),
        )
        self.assertRegex(
            cache_cmake,
            (
                r"bioseq_attach_extension_dso_closure\(\s*"
                r"postgresql_bioseq_cache_test_support[\s\S]*?"
                r"BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_MANIFEST"
                r"[\s\S]*?CONTRACT manifest"
                r"[\s\S]*?REQUIRE_COMPLETE_CODEGEN\)"
            ),
        )

    def test_manifest_dso_contract_has_non_bioseq_self_test(self) -> None:
        validator = (
            REPOSITORY_ROOT
            / "codegen"
            / "safety"
            / "validate_postgres_error_safety.py"
        ).read_text(encoding="utf-8")

        self.assertIn(
            'DSO_CLOSURE_CONTRACTS = frozenset({"bioseq", "manifest"})',
            validator,
        )
        self.assertIn('contract: str = "bioseq"', validator)
        self.assertIn('contract="manifest"', validator)
        self.assertIn(
            "Bioseq DSO defaults accepted a cache manifest",
            validator,
        )
        self.assertIn(
            "manifest DSO contract accepted an unowned fmgr export",
            validator,
        )
        self.assertIn(
            "full Bioseq evidence accepted the manifest DSO contract",
            validator,
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


class ReleasePublisherTests(unittest.TestCase):
    def publisher(self):
        return runpy.run_path(str(REPOSITORY_ROOT / 'infra/publish_release.py'))

    def test_upload_snapshot_is_removed_after_success_or_failure(self):
        function = self.publisher()['publish_packages']
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as raw:
                control = Path(raw)
                visited = []

                def publish(stage, packages, options):
                    self.assertTrue(stage.is_dir())
                    self.assertEqual(stage.parent, control)
                    (stage / 'temporary-copy').write_bytes(b'upload snapshot')
                    visited.append(stage)
                    if fail:
                        raise RuntimeError('simulated upload failure')

                with mock.patch.dict(function.__globals__, {
                        'CONTROL': control, 'event': mock.Mock(),
                        'accepted_packages': lambda: [{'package_version': 'alpha'}],
                        'sourceforge_access': lambda: [],
                        'publish_package_snapshot': publish}), \
                        mock.patch.dict(os.environ, {'SOURCE_FORGE_RELEASE_TOKEN': 'test-token'}):
                    if fail:
                        with self.assertRaisesRegex(RuntimeError, 'simulated upload failure'):
                            function(True)
                    else:
                        function(True)
                self.assertEqual(len(visited), 1)
                self.assertFalse(visited[0].exists())

    def test_sdk_support_files_keep_runtime_metadata_and_verified_bytes(self):
        function = self.publisher()['accepted_packages']
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            output = root / 'bin_artifacts/packages'
            output.mkdir(parents=True)
            package = output / 'bioseq.pkg'
            formula = output / 'postgresql@20.rb'
            package.write_bytes(b'unit-test package fixture')
            formula.write_text('# unit-test SDK fixture\n')
            record = dict(file=package.name, bytes=package.stat().st_size,
                          sha256=sha256(package), postgres_major=20, distro='macos',
                          architecture='arm64', postgres_release_channel='development',
                          postgres_version='PostgreSQL 20devel',
                          postgres_runtime_requirement='exact development SDK',
                          postgres_sdk_formula='bioseqdb/package-sdks/postgresql@20',
                          validation=dict(passed=True, format='pkg', package_version='0.1.0-alpha.1'),
                          installed_sql_smoke=dict(passed=True, default_insert_search=True,
                                                   default_persisted_search=True,
                                                   column_defaults_and_vacuum=True),
                          postgres_sdk_support_files=[dict(file=formula.name,
                              bytes=formula.stat().st_size, sha256=sha256(formula))])
            manifest = output / 'manifest.json'
            manifest.write_text(json.dumps(dict(packages=[record])))
            with mock.patch.dict(function.__globals__, {'ROOT': root}):
                accepted = function()[0]
                record['installed_sql_smoke'].pop('column_defaults_and_vacuum')
                manifest.write_text(json.dumps(dict(packages=[record])))
                with self.assertRaisesRegex(RuntimeError, 'No accepted packages'):
                    function()
                record['installed_sql_smoke']['column_defaults_and_vacuum'] = True
                manifest.write_text(json.dumps(dict(packages=[record])))
                self.assertEqual(accepted['postgres_runtime_requirement'], 'exact development SDK')
                companion = accepted['postgres_sdk_support_files'][0]
                self.assertEqual(companion['source'], formula)
                self.assertEqual(companion['sha256'], sha256(formula))
                self.assertEqual(Path(companion['file']).parent, Path(accepted['file']).parent)
                formula.write_text('# changed after acceptance\n')
                with self.assertRaises(RuntimeError):
                    function()
                record['postgres_sdk_support_files'][0]['file'] = '../outside.rb'
                manifest.write_text(json.dumps(dict(packages=[record])))
                with self.assertRaises(RuntimeError):
                    function()

    def site(self, root):
        root.mkdir()
        (root / 'index.html').write_text('new website')
        (root / 'site-manifest.json').write_text(json.dumps({
            'schema': 'bioseq.website.site_manifest', 'hash_algorithm': 'sha256',
            'outputs': {'/index.html': sha256(root / 'index.html')}}))

    def test_website_manifest_rejects_changed_extra_and_workflow_files(self):
        publisher = self.publisher()
        with tempfile.TemporaryDirectory() as raw:
            site = Path(raw) / 'site'
            self.site(site)
            publisher['website_inventory'](site)
            (site / 'index.html').write_text('corrupted')
            with self.assertRaises(RuntimeError):
                publisher['website_inventory'](site)
            (site / 'index.html').write_text('new website')
            (site / '.github').mkdir()
            (site / '.github/workflow.yml').write_text('do not publish')
            with self.assertRaises(RuntimeError):
                publisher['website_inventory'](site)
            publisher['website_inventory'](site, repository=True)
            (site / 'old.txt').write_text('old trash')
            with self.assertRaises(RuntimeError):
                publisher['website_inventory'](site, repository=True)

    def test_website_replacement_preserves_workflows_and_removes_old_files(self):
        publisher = self.publisher()
        function = publisher['replace_website']
        with tempfile.TemporaryDirectory() as raw:
            base = Path(raw)
            site = base / 'site'
            self.site(site)
            checkout = base / 'control/website/checkout'
            checkout.mkdir(parents=True)
            (checkout / '.git').mkdir()
            (checkout / '.github').mkdir()
            workflow = checkout / '.github/static.yml'
            workflow.write_text('preserve me')
            (checkout / 'old').mkdir()
            (checkout / 'old/old.html').write_text('remove me')
            # Publication exercises real Git; this offline test covers file scope.
            with mock.patch.dict(function.__globals__, {
                    'CONTROL': base / 'control', 'ROOT': base / 'source',
                    'command': lambda *a, **k: str(checkout)}):
                function(checkout, site)
            self.assertEqual(workflow.read_text(), 'preserve me')
            self.assertTrue((checkout / '.git').is_dir())
            self.assertFalse((checkout / 'old').exists())
            self.assertEqual((checkout / 'index.html').read_text(), 'new website')

    def test_website_replacement_refuses_source_checkout(self):
        publisher = self.publisher()
        function = publisher['replace_website']
        with mock.patch.dict(function.__globals__, {'command': lambda *a, **k: str(REPOSITORY_ROOT)}):
            with self.assertRaises(RuntimeError):
                function(REPOSITORY_ROOT, REPOSITORY_ROOT)

    def test_sourceforge_credentials_use_body_or_header_and_refuse_redirects(self):
        publisher = self.publisher()
        function = publisher['sf_api']
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = b'{"result":true}'
        opener = mock.Mock()
        opener.open.return_value = response
        with mock.patch.dict(os.environ, {'SOURCE_FORCE_OAUTH_TOKEN': 'oauth-secret',
                'SOURCE_FORGE_RELEASE_TOKEN': 'release-secret'}), \
                mock.patch('urllib.request.build_opener', return_value=opener):
            function('/rest/p/bioseqdb', oauth=True)
            request = opener.open.call_args.args[0]
            self.assertEqual(request.get_header('Authorization'), 'Bearer oauth-secret')
            self.assertNotIn('oauth-secret', request.full_url)
            function('/projects/bioseqdb/files/file', method='PUT', fields={'download_label': 'alpha'})
            request = opener.open.call_args.args[0]
            self.assertIn(b'api_key=release-secret', request.data)
            self.assertNotIn('release-secret', request.full_url)
        self.assertIsNone(publisher['NoRedirect']().redirect_request(None, None, 302, '', {}, 'https://elsewhere'))


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
            self.assertEqual(store.payload["schema_revision"], 1)
            self.assertNotIn("schema_version", store.payload)
            self.assertEqual(store.payload["status"], "failed")
            self.assertEqual(store.payload["source_revision"], "a" * 40)
            self.assertEqual(store.payload["poetry_lock_sha256"], "b" * 64)
            self.assertEqual(
                store.payload["python_environment"],
                {"python": "3.12.3", "poetry": "2.1.3"},
            )

            for mutation in ("legacy", "wrong-revision", "unknown-key"):
                with self.subTest(mutation=mutation):
                    invalid = dict(store.payload)
                    if mutation == "legacy":
                        invalid["schema_version"] = invalid.pop("schema_revision")
                    elif mutation == "wrong-revision":
                        invalid["schema_revision"] = 2
                    else:
                        invalid["unexpected"] = True
                    with self.assertRaises(manifest.ManifestError):
                        manifest.write_manifest(path, invalid)

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
            REPOSITORY_ROOT / "infra" / "package_builder.py",
            REPOSITORY_ROOT / "infra" / "package_build_infra" / "scripts" / "bootstrap.sh",
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
            # A retained AWS secret name is not a legacy toolchain invocation.
            # Renaming that resource would break existing signing deployments.
            if source == REPOSITORY_ROOT / "infra" / "package_builder.py":
                content = content.replace("-" + legacy_name + "-signing-key", "-signing-key")
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
