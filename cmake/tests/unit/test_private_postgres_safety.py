#!/usr/bin/env python3
"""Focused unit tests for the dual-profile runtime-evidence lifecycle."""

from __future__ import annotations

import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock

_CMAKE_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_CMAKE_ROOT))

import run_private_postgres_safety_tests as lifecycle  # noqa: E402


def _write(path: Path, content: bytes, *, executable: bool = False) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    if executable:
        path.chmod(0o700)
    return path


def _stage(root: Path, profile: str) -> tuple[Path, dict[str, object]]:
    stage_root = root / "stages" / profile
    for relative in ("bin", "share/extension", "lib"):
        (stage_root / relative).mkdir(parents=True, exist_ok=True)
    tool_signatures: list[dict[str, str]] = []
    expected_tool_paths: dict[str, str] = {}
    runtime_entries: list[dict[str, object]] = []
    for tool in lifecycle._POSTGRESQL_TOOLS:
        relative = f"bin/{tool}"
        binary = _write(
            stage_root / relative,
            f"{tool} fixture\n".encode(),
            executable=True,
        )
        expected_tool_paths[tool] = relative
        tool_signatures.append({
            "tool": tool,
            "staged_path": relative,
            "version": f"{tool} (PostgreSQL) 17.10 (test fixture)",
            "sha256": lifecycle._sha256(binary),
        })
        runtime_entries.append({
            "root": "bindir",
            "path": tool,
            "kind": "file",
            "source_kind": "file",
            "source_path": str(binary.resolve()),
            "source_link_target": None,
            "resolved_source_path": str(binary.resolve()),
            "size": binary.stat().st_size,
            "sha256": lifecycle._sha256(binary),
        })
    runtime_entries.sort(key=lambda entry: str(entry["path"]).encode())
    runtime_aggregate = lifecycle._sha256_bytes(
        "".join(
            lifecycle._runtime_entry_record(entry)
            for entry in runtime_entries
        ).encode("utf-8")
    )
    control = _write(
        stage_root / "share" / "extension" / "bioseq.control",
        b"control fixture\n",
    )
    install_sql = _write(
        stage_root / "share" / "extension" / "bioseq--1.0.sql",
        b"install SQL fixture\n",
    )
    library = _write(
        stage_root / "lib" / "bioseq.so",
        b"library fixture\n",
    )
    artifact_hashes = {
        "control": lifecycle._sha256(control),
        "install_sql": lifecycle._sha256(install_sql),
        "library": lifecycle._sha256(library),
    }
    source = {
        "bindir": str((stage_root / "bin").resolve()),
        "sharedir": str((stage_root / "share").resolve()),
        "pkglibdir": str((stage_root / "lib").resolve()),
    }
    staged = {
        "bindir": "bin",
        "sharedir": "share",
        "pkglibdir": "lib",
    }
    document: dict[str, object] = {
        "schema": lifecycle._STAGE_MANIFEST_SCHEMA,
        "schema_revision": lifecycle._STAGE_MANIFEST_SCHEMA_REVISION,
        "postgresql_version": "PostgreSQL 17.10 (test fixture)",
        "postgresql_configure": "--with-test-fixture",
        "source": source,
        "staged": staged,
        "runtime_closure": {
            "algorithm": lifecycle._RUNTIME_CLOSURE_ALGORITHM,
            "schema_revision": lifecycle._RUNTIME_CLOSURE_SCHEMA_REVISION,
            "entry_count": len(runtime_entries),
            "aggregate_sha256": runtime_aggregate,
            "roots": {
                name: {
                    "reported_source": source[name],
                    "canonical_source": source[name],
                    "staged": staged[name],
                }
                for name in ("bindir", "sharedir", "pkglibdir")
            },
            "required_sentinels": {},
            "loader_dependencies": {
                "entry_count": 0,
                "aggregate_sha256": lifecycle._sha256_bytes(b""),
                "entries": [],
            },
            "entries": runtime_entries,
        },
        "extension_artifacts": {
            "aggregate_sha256": lifecycle._stage_package_aggregate(
                control_filename=control.name,
                install_sql_filename=install_sql.name,
                library_filename=library.name,
                control_sha256=artifact_hashes["control"],
                install_sql_sha256=artifact_hashes["install_sql"],
                library_sha256=artifact_hashes["library"],
            ),
            "artifacts": [{
                "name": "bioseq",
                "module_pathname_override": None,
                "effective_module_name": "bioseq",
                "control": {
                    "source": str(control.resolve()),
                    "filename": control.name,
                    "staged": f"share/extension/{control.name}",
                    "source_sha256": artifact_hashes["control"],
                    "sha256": artifact_hashes["control"],
                },
                "install_sql": {
                    "source": str(install_sql.resolve()),
                    "filename": install_sql.name,
                    "staged": f"share/extension/{install_sql.name}",
                    "version": "1.0",
                    "sha256": artifact_hashes["install_sql"],
                },
                "library": {
                    "source": str(library.resolve()),
                    "filename": library.name,
                    "staged": f"lib/{library.name}",
                    "sha256": artifact_hashes["library"],
                },
            }],
        },
        "expected_tool_paths": expected_tool_paths,
        "tool_signatures": tool_signatures,
    }
    (stage_root / "stage-manifest.json").write_text(
        json.dumps(document, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return stage_root, document


class _FakeCluster:
    instances: list["_FakeCluster"] = []
    next_port = 41000
    stamp: Path | None = None

    def __init__(self, inputs: object) -> None:
        self.inputs = inputs
        self.profile = str(inputs.run_label).removeprefix(
            "private_postgres_safety_"
        )
        self.port = _FakeCluster.next_port
        _FakeCluster.next_port += 1
        self.database = inputs.database
        self.database_user = "runtime_owner"
        self.run_dir = inputs.output_root / f"{self.profile}-run"
        self.data_dir = self.run_dir / "data"
        self.socket_dir = self.run_dir / "socket"
        self.server_log = self.run_dir / "server_startup.log"
        self.started = False
        self.stopped = False
        _FakeCluster.instances.append(self)

    def start(self) -> None:
        self.data_dir.mkdir(parents=True)
        self.socket_dir.mkdir(parents=True)
        self.server_log.write_text(
            "private PostgreSQL fixture log\n",
            encoding="utf-8",
        )
        self.started = True

    def stop(self) -> None:
        if _FakeCluster.stamp is not None:
            assert not _FakeCluster.stamp.exists()
        self.stopped = True
        self.started = False

    def child_environment(self) -> dict[str, str]:
        return {
            "PATH": os.environ.get("PATH", ""),
            "PGHOST": "ambient-must-be-removed",
            "BIOSEQ_TEST_PGHOST": "ambient-must-be-removed",
        }


class RuntimeEvidenceLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        _FakeCluster.instances = []
        _FakeCluster.next_port = 41000
        _FakeCluster.stamp = None

    def _arguments(
        self,
        root: Path,
    ) -> tuple[object, dict[str, Path]]:
        inputs = root / "inputs"
        files: dict[str, Path] = {}
        for option in (
            "stage-script",
            "config-template",
            "runtime-producer",
            "production-ledger",
            "test-indexes-ledger",
            "production-runtime-inventory",
            "test-indexes-runtime-inventory",
            "runtime-inventory-producer-executable",
            "production-sql-manifest",
            "test-indexes-sql-manifest",
            "control-dso",
            "runtime-policy",
            "production-test-executable",
            "test-indexes-test-executable",
        ):
            files[option] = _write(
                inputs / option,
                (option + "\n").encode(),
            )
        files["cmake"] = _write(
            inputs / "cmake", b"cmake", executable=True)
        files["pg-config"] = _write(
            inputs / "pg_config", b"pg_config", executable=True)
        files["control-file"] = _write(
            inputs / "bioseq.control", b"control\n")
        files["production-sql-file"] = _write(
            inputs / "production" / "bioseq--1.0.sql",
            b"production sql\n",
        )
        files["test-indexes-sql-file"] = _write(
            inputs / "test_indexes" / "bioseq--1.0.sql",
            b"test indexes sql\n",
        )
        files["production-dso"] = _write(
            inputs / "production" / "bioseq.so",
            b"production dso\n",
        )
        files["test-indexes-dso"] = _write(
            inputs / "test_indexes" / "bioseq.so",
            b"test indexes dso\n",
        )
        for option in (
            "production-case-plan",
            "test-indexes-case-plan",
        ):
            files[option] = _write(
                inputs / option,
                json.dumps({"cases": [{}, {}]}).encode(),
            )
        output_root = root / "candidate"
        stamp = output_root / "candidate.stamp"
        argv: list[str] = []
        for option in (
            "cmake",
            "pg-config",
            "stage-script",
            "config-template",
            "runtime-producer",
            "production-ledger",
            "test-indexes-ledger",
            "production-runtime-inventory",
            "test-indexes-runtime-inventory",
            "runtime-inventory-producer-executable",
            "production-sql-manifest",
            "test-indexes-sql-manifest",
            "production-sql-file",
            "test-indexes-sql-file",
            "control-file",
            "control-dso",
            "runtime-policy",
            "production-case-plan",
            "test-indexes-case-plan",
            "production-dso",
            "test-indexes-dso",
            "production-test-executable",
            "test-indexes-test-executable",
        ):
            argv.extend((f"--{option}", str(files[option])))
        argv.extend((
            "--output-root",
            str(output_root),
            "--stamp",
            str(stamp),
            "--timeout-seconds",
            "17",
        ))
        files["output-root"] = output_root
        files["stamp"] = stamp
        return lifecycle._parser().parse_args(argv), files

    def test_executable_rejects_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = _write(root / "real", b"tool", executable=True)
            link = root / "link"
            link.symlink_to(target)
            with self.assertRaisesRegex(
                lifecycle.RuntimeEvidenceLifecycleError,
                "regular non-symlink",
            ):
                lifecycle._executable(link, "tool")

    def test_symlinked_stage_parent_cannot_delete_outside_candidate(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments, files = self._arguments(root)
            candidate = files["output-root"]
            candidate.mkdir()
            outside = root / "outside"
            outside_stage = outside / "production"
            outside_stage.mkdir(parents=True)
            sentinel = _write(
                outside_stage / "must-survive.txt",
                b"outside stage must survive\n",
            )
            (candidate / "stages").symlink_to(
                outside,
                target_is_directory=True,
            )
            with self.assertRaisesRegex(
                lifecycle.RuntimeEvidenceLifecycleError,
                "stages writable root.*symlink",
            ):
                lifecycle.run(arguments)
            self.assertEqual(
                sentinel.read_bytes(),
                b"outside stage must survive\n",
            )

    def test_candidate_and_reviewed_leaves_reject_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments, files = self._arguments(root)
            candidate = files["output-root"]
            reviewed = candidate / "reviewed"
            reviewed.mkdir(parents=True)
            outside = _write(root / "outside-ledger.json", b"outside\n")
            (reviewed / "production-ledger.json").symlink_to(outside)
            with self.assertRaisesRegex(
                lifecycle.RuntimeEvidenceLifecycleError,
                "reviewed production-ledger.json.*regular non-symlink",
            ):
                lifecycle.run(arguments)
            self.assertEqual(outside.read_bytes(), b"outside\n")

    def test_stage_manifest_rejects_escape(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage_root, document = _stage(root, "production")
            staged = document["staged"]
            assert isinstance(staged, dict)
            staged["bindir"] = "../outside"
            manifest = stage_root / "stage-manifest.json"
            manifest.write_text(
                json.dumps(document),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                lifecycle.RuntimeEvidenceLifecycleError,
                "escapes",
            ):
                lifecycle._load_stage_manifest(manifest, stage_root)

    def test_stage_manifest_requires_current_closed_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)

            def set_runtime_member(
                document: dict[str, object],
                member: str,
                value: object,
            ) -> None:
                runtime_closure = document["runtime_closure"]
                assert isinstance(runtime_closure, dict)
                runtime_closure[member] = value

            def remove_runtime_member(
                document: dict[str, object],
                member: str,
            ) -> None:
                runtime_closure = document["runtime_closure"]
                assert isinstance(runtime_closure, dict)
                runtime_closure.pop(member)

            def replace_root_revision_with_legacy(
                document: dict[str, object],
            ) -> None:
                document["schema_version"] = document.pop(
                    "schema_revision"
                )

            def replace_runtime_revision_with_legacy(
                document: dict[str, object],
            ) -> None:
                runtime_closure = document["runtime_closure"]
                assert isinstance(runtime_closure, dict)
                runtime_closure["schema_version"] = runtime_closure.pop(
                    "schema_revision"
                )

            mutations = (
                (
                    "schema identity",
                    lambda document: document.update({
                        "schema": "invalid.stage_manifest",
                    }),
                    "wrong schema",
                ),
                (
                    "schema revision",
                    lambda document: document.update({
                        "schema_revision": 2,
                    }),
                    "wrong schema",
                ),
                (
                    "missing schema revision",
                    lambda document: document.pop("schema_revision"),
                    "must contain exactly",
                ),
                (
                    "legacy schema revision",
                    replace_root_revision_with_legacy,
                    "must contain exactly",
                ),
                (
                    "runtime algorithm",
                    lambda document: set_runtime_member(
                        document,
                        "algorithm",
                        "invalid.runtime",
                    ),
                    "algorithm/schema is invalid",
                ),
                (
                    "runtime revision",
                    lambda document: set_runtime_member(
                        document,
                        "schema_revision",
                        2,
                    ),
                    "algorithm/schema is invalid",
                ),
                (
                    "missing runtime revision",
                    lambda document: remove_runtime_member(
                        document,
                        "schema_revision",
                    ),
                    "must contain exactly",
                ),
                (
                    "legacy runtime schema revision",
                    replace_runtime_revision_with_legacy,
                    "must contain exactly",
                ),
                (
                    "unknown root member",
                    lambda document: document.update({"legacy": True}),
                    "must contain exactly",
                ),
            )
            for label, mutate, expected in mutations:
                with self.subTest(label=label):
                    stage_root, document = _stage(root, "production")
                    mutate(document)
                    manifest = stage_root / "stage-manifest.json"
                    manifest.write_text(
                        json.dumps(document),
                        encoding="utf-8",
                    )
                    with self.assertRaisesRegex(
                        lifecycle.RuntimeEvidenceLifecycleError,
                        expected,
                    ):
                        lifecycle._load_stage_manifest(
                            manifest,
                            stage_root,
                        )

    def test_stage_manifest_requires_exact_pinned_postgres_release(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage_root, document = _stage(root, "production")
            manifest = stage_root / "stage-manifest.json"
            for version in (
                "PostgreSQL 17.5",
                "PostgreSQL 17.7 (Homebrew)",
                "PostgreSQL 17.test",
                "PostgreSQL 17.100",
                "PostgreSQL 18.0",
            ):
                document["postgresql_version"] = version
                manifest.write_text(json.dumps(document), encoding="utf-8")
                with self.subTest(version=version), self.assertRaisesRegex(
                    lifecycle.RuntimeEvidenceLifecycleError,
                    "requires exact PostgreSQL 17[.]10",
                ):
                    lifecycle._load_stage_manifest(manifest, stage_root)

            for version in (
                "PostgreSQL 17.10",
                "PostgreSQL 17.10 (Homebrew)",
            ):
                document["postgresql_version"] = version
                manifest.write_text(json.dumps(document), encoding="utf-8")
                with self.subTest(version=version):
                    loaded = lifecycle._load_stage_manifest(
                        manifest,
                        stage_root,
                    )
                    self.assertEqual(
                        loaded["postgresql_version"],
                        version,
                    )

    def test_stage_manifest_authenticates_every_postgres_tool(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)

            def require_failure(
                mutate: object,
                expected: str,
            ) -> None:
                stage_root, document = _stage(root, "production")
                mutate(stage_root, document)
                manifest = stage_root / "stage-manifest.json"
                manifest.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaisesRegex(
                    lifecycle.RuntimeEvidenceLifecycleError,
                    expected,
                ):
                    lifecycle._load_stage_manifest(manifest, stage_root)

            require_failure(
                lambda _root, document: document[
                    "tool_signatures"
                ].pop(),
                "does not close over",
            )
            require_failure(
                lambda _root, document: document[
                    "tool_signatures"
                ][0].update({"version": "postgres (PostgreSQL) 17.7"}),
                "requires exact PostgreSQL 17[.]10 tool postgres",
            )
            require_failure(
                lambda _root, document: document[
                    "tool_signatures"
                ][0].update({"sha256": "0" * 64}),
                "hash for PostgreSQL tool postgres is invalid",
            )
            require_failure(
                lambda stage_root, _document: (
                    stage_root / "bin" / "postgres"
                ).write_bytes(b"changed"),
                "hash for PostgreSQL tool postgres is invalid",
            )
            require_failure(
                lambda _root, document: document[
                    "expected_tool_paths"
                ].update({"postgres": "bin/initdb"}),
                "path for PostgreSQL tool postgres is invalid",
            )

    def test_stage_profile_uses_closed_artifact_specification(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output_root = lifecycle._output_root(root / "candidate")
            cmake = _write(root / "cmake", b"cmake", executable=True)
            pg_config = _write(
                root / "pg_config",
                b"pg_config",
                executable=True,
            )
            stage_script = _write(root / "stage.cmake", b"stage\n")
            control = _write(root / "bioseq.control", b"control\n")
            install_sql = _write(
                root / "bioseq--1.0.sql",
                b"install SQL\n",
            )
            library = _write(root / "bioseq.so", b"library\n")
            commands: list[list[str]] = []
            stage_documents: list[dict[str, object]] = []

            def fake_run(command: list[str], **_keywords: object) -> str:
                commands.append(list(command))
                stage_root, document = _stage(
                    output_root,
                    "production",
                )
                (stage_root / "share/extension/bioseq.control").write_bytes(
                    control.read_bytes())
                (stage_root / "share/extension/bioseq--1.0.sql").write_bytes(
                    install_sql.read_bytes())
                (stage_root / "lib/bioseq.so").write_bytes(
                    library.read_bytes())
                stage_documents.append(document)
                return ""

            def fake_load(
                _manifest: Path,
                _stage_root: Path,
                **keywords: object,
            ) -> dict[str, object]:
                self.assertEqual(
                    keywords["expected_artifact"],
                    (control, install_sql, library),
                )
                self.assertEqual(
                    keywords["aggregate_path"],
                    output_root
                    / "stages/production/stage-artifacts.sha256",
                )
                return stage_documents[-1]

            with (
                mock.patch.object(
                    lifecycle,
                    "_run",
                    side_effect=fake_run,
                ),
                mock.patch.object(
                    lifecycle,
                    "_load_stage_manifest",
                    side_effect=fake_load,
                ),
            ):
                lifecycle._stage_profile(
                    profile="production",
                    cmake=cmake,
                    pg_config=pg_config,
                    stage_script=stage_script,
                    control_file=control,
                    sql_file=install_sql,
                    dso=library,
                    output_root=output_root,
                    timeout=17,
                )

            self.assertEqual(len(commands), 1)
            definitions = {
                argument.partition("=")[0]
                for argument in commands[0]
                if argument.startswith("-D")
            }
            self.assertEqual(
                definitions,
                {
                    "-DPG_CONFIG",
                    "-DSTAGE_ROOT",
                    "-DSTAGE_PARENT",
                    "-DARTIFACT_SPEC",
                    "-DMANIFEST_OUT",
                    "-DHASH_OUT",
                },
            )
            spec_argument = next(
                argument
                for argument in commands[0]
                if argument.startswith("-DARTIFACT_SPEC=")
            )
            spec_path = Path(spec_argument.partition("=")[2])
            self.assertEqual(
                json.loads(spec_path.read_text(encoding="utf-8")),
                lifecycle._artifact_spec_document(
                    control_file=control,
                    install_sql=install_sql,
                    library=library,
                ),
            )
            self.assertEqual(stat.S_IMODE(spec_path.stat().st_mode), 0o600)

    def test_cluster_config_is_private_and_harness_compatible(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage_root, manifest = _stage(root, "production")
            control = _write(root / "bioseq.control", b"control")
            control_dso = _write(root / "control.so", b"control dso")
            sql_file = _write(root / "bioseq--1.0.sql", b"sql")
            dso = _write(root / "bioseq.so", b"dso")
            cluster = _FakeCluster(type("Inputs", (), {
                "run_label": "private_postgres_safety_production",
                "database": "bioseq_safety_runtime",
                "output_root": root / "runs",
            })())
            cluster.start()
            config_path = lifecycle._cluster_config(
                profile="production",
                cluster=cluster,
                system_identifier="123456789",
                stage_root=stage_root,
                stage_manifest=manifest,
                control_file=control,
                control_dso=control_dso,
                sql_file=sql_file,
                dso=dso,
                output_root=root,
            )
            config = json.loads(config_path.read_text())
            self.assertEqual(
                set(config),
                {
                    "schema_revision",
                    "profile",
                    "dedicated_safety_cluster",
                    "connection",
                    "private_stage_root",
                    "expected_data_directory",
                    "expected_system_identifier",
                    "authorization_file",
                    "terminal_marker_directory",
                    "server_log_path",
                    "installed_dso_path",
                    "installed_control_dso_path",
                    "installed_control_path",
                    "installed_sql_path",
                    "stage_manifest_path",
                    "stage_manifest_sha256",
                    "tool_signature_suite_sha256",
                    "restart_command",
                },
            )
            self.assertEqual(
                config["schema_revision"],
                1,
            )
            self.assertIs(type(config["schema_revision"]), int)
            self.assertNotIn("schema_version", config)
            self.assertEqual(
                stat.S_IMODE(config_path.stat().st_mode), 0o600)
            marker = Path(config["authorization_file"])
            self.assertEqual(
                marker.read_bytes(),
                b"bioseq.postgres_error_safety.authorization_marker\n"
                b"revision=2\n",
            )
            self.assertEqual(stat.S_IMODE(marker.stat().st_mode), 0o600)
            terminal_marker_directory = Path(
                config["terminal_marker_directory"])
            self.assertEqual(
                terminal_marker_directory,
                (
                    cluster.data_dir
                    / lifecycle._TERMINAL_MARKER_DIRECTORY
                ).resolve(),
            )
            self.assertEqual(
                terminal_marker_directory.parent,
                cluster.data_dir.resolve(),
            )
            terminal_metadata = terminal_marker_directory.lstat()
            self.assertTrue(stat.S_ISDIR(terminal_metadata.st_mode))
            self.assertFalse(stat.S_ISLNK(terminal_metadata.st_mode))
            self.assertEqual(stat.S_IMODE(terminal_metadata.st_mode), 0o700)
            self.assertEqual(terminal_metadata.st_uid, os.getuid())
            self.assertEqual(
                config["server_log_path"],
                str(cluster.server_log.resolve()),
            )
            self.assertTrue(config["connection"]["password"])
            stage_manifest_path = stage_root / "stage-manifest.json"
            self.assertEqual(
                config["stage_manifest_path"],
                str(stage_manifest_path.resolve()),
            )
            self.assertEqual(
                config["stage_manifest_sha256"],
                lifecycle._sha256(stage_manifest_path),
            )
            normalized_signatures = sorted(
                manifest["tool_signatures"],
                key=lambda signature: signature["tool"],
            )
            signature_suite = (
                json.dumps(
                    normalized_signatures,
                    ensure_ascii=False,
                    sort_keys=True,
                    separators=(",", ":"),
                )
                + "\n"
            ).encode()
            self.assertEqual(
                config["tool_signature_suite_sha256"],
                lifecycle.hashlib.sha256(signature_suite).hexdigest(),
            )
            self.assertEqual(
                config["restart_command"],
                [
                    str((stage_root / "bin" / "pg_ctl").resolve()),
                    "--pgdata",
                    str(cluster.data_dir.resolve()),
                    "--wait",
                    "--mode=fast",
                    "restart",
                ],
            )
            real_server_log = cluster.run_dir / "real-server.log"
            real_server_log.write_text("redirected\n", encoding="utf-8")
            cluster.server_log.unlink()
            cluster.server_log.symlink_to(real_server_log)
            with self.assertRaisesRegex(
                lifecycle.RuntimeEvidenceLifecycleError,
                "server log must be a regular non-symlink file",
            ):
                lifecycle._cluster_config(
                    profile="production",
                    cluster=cluster,
                    system_identifier="123456789",
                    stage_root=stage_root,
                    stage_manifest=manifest,
                    control_file=control,
                    control_dso=control_dso,
                    sql_file=sql_file,
                    dso=dso,
                    output_root=root,
                )

    def test_terminal_marker_directory_rejects_unsafe_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)

            valid_data = root / "valid-data"
            valid_data.mkdir()
            valid_marker = lifecycle._terminal_marker_directory(valid_data)
            self.assertEqual(
                valid_marker,
                (
                    valid_data / lifecycle._TERMINAL_MARKER_DIRECTORY
                ).resolve(),
            )
            self.assertEqual(
                stat.S_IMODE(valid_marker.lstat().st_mode),
                0o700,
            )
            self.assertEqual(
                lifecycle._terminal_marker_directory(valid_data),
                valid_marker,
            )

            symlink_data_target = root / "symlink-data-target"
            symlink_data_target.mkdir()
            symlink_data = root / "data-directory-link"
            symlink_data.symlink_to(
                symlink_data_target,
                target_is_directory=True,
            )
            with self.assertRaisesRegex(
                lifecycle.RuntimeEvidenceLifecycleError,
                "private data directory must be a real directory",
            ):
                lifecycle._terminal_marker_directory(symlink_data)

            for label, prepare in (
                (
                    "symlink",
                    lambda marker: marker.symlink_to(
                        root / "outside-marker-directory",
                        target_is_directory=True,
                    ),
                ),
                (
                    "regular-file",
                    lambda marker: marker.write_text(
                        "not a directory",
                        encoding="utf-8",
                    ),
                ),
                (
                    "wrong-mode",
                    lambda marker: (
                        marker.mkdir(mode=0o700),
                        marker.chmod(0o750),
                    ),
                ),
            ):
                with self.subTest(label=label):
                    data_directory = root / f"{label}-data"
                    data_directory.mkdir()
                    if label == "symlink":
                        (root / "outside-marker-directory").mkdir()
                    prepare(
                        data_directory
                        / lifecycle._TERMINAL_MARKER_DIRECTORY
                    )
                    with self.assertRaisesRegex(
                        lifecycle.RuntimeEvidenceLifecycleError,
                        "terminal marker directory must be a private real "
                        "directory",
                    ):
                        lifecycle._terminal_marker_directory(
                            data_directory)

            ownership_data = root / "wrong-ownership-data"
            ownership_data.mkdir()
            ownership_marker = (
                ownership_data / lifecycle._TERMINAL_MARKER_DIRECTORY
            )
            ownership_marker.mkdir(mode=0o700)
            ownership_marker.chmod(0o700)
            with (
                mock.patch.object(
                    lifecycle.os,
                    "getuid",
                    return_value=ownership_marker.stat().st_uid + 1,
                ),
                self.assertRaisesRegex(
                    lifecycle.RuntimeEvidenceLifecycleError,
                    "owned by the current user",
                ),
            ):
                lifecycle._terminal_marker_directory(ownership_data)

    def test_success_keeps_both_clusters_live_and_preserves_reviewed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments, files = self._arguments(root)
            reviewed = files["output-root"] / "reviewed"
            sentinel = _write(reviewed / "runtime-matrix.json", b"reviewed\n")
            commands: list[list[str]] = []

            def fake_stage_profile(**keywords: object):
                return _stage(
                    Path(keywords["output_root"]),
                    str(keywords["profile"]),
                )

            def fake_run(
                command: list[str],
                **keywords: object,
            ) -> str:
                del keywords
                commands.append(list(command))
                self.assertEqual(len(_FakeCluster.instances), 2)
                self.assertTrue(all(
                    cluster.started
                    for cluster in _FakeCluster.instances
                ))
                if "run" in command:
                    output_root = files["output-root"]
                    _write(
                        output_root / "runtime-matrix.json",
                        b"candidate matrix\n",
                    )
                    _write(
                        output_root / "evidence" / "case.json",
                        b"candidate evidence\n",
                    )
                return ""

            _FakeCluster.stamp = files["stamp"]
            with (
                mock.patch.object(
                    lifecycle,
                    "_stage_profile",
                    side_effect=fake_stage_profile,
                ),
                mock.patch.object(
                    lifecycle,
                    "UniquePrivateCluster",
                    _FakeCluster,
                ),
                mock.patch.object(
                    lifecycle,
                    "_cluster_system_identifier",
                    side_effect=("1001", "1002"),
                ),
                mock.patch.object(
                    lifecycle,
                    "_run",
                    side_effect=fake_run,
                ),
            ):
                lifecycle.run(arguments)

            self.assertTrue(all(
                cluster.stopped for cluster in _FakeCluster.instances))
            self.assertEqual(sentinel.read_bytes(), b"reviewed\n")
            self.assertEqual(
                (files["output-root"] / "production-ledger.json").read_bytes(),
                files["production-ledger"].read_bytes(),
            )
            self.assertEqual(
                (files["output-root"] / "test-indexes-ledger.json").read_bytes(),
                files["test-indexes-ledger"].read_bytes(),
            )
            self.assertEqual(
                (files["output-root"] / "runtime-matrix.json").read_bytes(),
                b"candidate matrix\n",
            )
            self.assertEqual(
                (files["output-root"] / "evidence" / "case.json").read_bytes(),
                b"candidate evidence\n",
            )
            stamp = json.loads(files["stamp"].read_text())
            self.assertEqual(
                set(stamp),
                {
                    "schema_revision",
                    "production_ledger_sha256",
                    "test_indexes_ledger_sha256",
                    "runtime_matrix_sha256",
                },
            )
            self.assertEqual(stamp["schema_revision"], 1)
            self.assertIs(type(stamp["schema_revision"]), int)
            self.assertNotIn("schema_version", stamp)
            run_command = next(command for command in commands if "run" in command)
            self.assertIn("isolated", run_command)
            self.assertNotIn("--stamp", run_command)
            production_config = json.loads(
                (
                    files["output-root"]
                    / "cluster-configs"
                    / "production.json"
                ).read_text(encoding="utf-8")
            )
            test_indexes_config = json.loads(
                (
                    files["output-root"]
                    / "cluster-configs"
                    / "test_indexes.json"
                ).read_text(encoding="utf-8")
            )
            self.assertNotEqual(
                production_config["terminal_marker_directory"],
                test_indexes_config["terminal_marker_directory"],
            )
            self.assertNotEqual(
                production_config["server_log_path"],
                test_indexes_config["server_log_path"],
            )
            self.assertEqual(
                run_command[
                    run_command.index("--runtime-control-dso") + 1
                ],
                str(files["control-dso"].resolve()),
            )
            for profile in ("production", "test_indexes"):
                flag = (
                    "--production-stage-manifest"
                    if profile == "production"
                    else "--test-indexes-stage-manifest"
                )
                self.assertEqual(
                    run_command[run_command.index(flag) + 1],
                    str(
                        (
                            files["output-root"]
                            / "stages"
                            / profile
                            / "stage-manifest.json"
                        ).resolve()
                    ),
                )

    def test_failure_stops_both_clusters_and_does_not_stamp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments, files = self._arguments(root)

            def fake_stage_profile(**keywords: object):
                return _stage(
                    Path(keywords["output_root"]),
                    str(keywords["profile"]),
                )

            def fake_run(command: list[str], **keywords: object) -> str:
                del keywords
                if "run" in command:
                    raise lifecycle.RuntimeEvidenceLifecycleError(
                        "injected runtime failure")
                return ""

            _FakeCluster.stamp = files["stamp"]
            with (
                mock.patch.object(
                    lifecycle,
                    "_stage_profile",
                    side_effect=fake_stage_profile,
                ),
                mock.patch.object(
                    lifecycle,
                    "UniquePrivateCluster",
                    _FakeCluster,
                ),
                mock.patch.object(
                    lifecycle,
                    "_cluster_system_identifier",
                    side_effect=("1001", "1002"),
                ),
                mock.patch.object(
                    lifecycle,
                    "_run",
                    side_effect=fake_run,
                ),
            ):
                with self.assertRaisesRegex(
                    lifecycle.RuntimeEvidenceLifecycleError,
                    "injected runtime failure",
                ):
                    lifecycle.run(arguments)

            self.assertTrue(all(
                cluster.stopped for cluster in _FakeCluster.instances))
            self.assertFalse(files["stamp"].exists())


if __name__ == "__main__":
    unittest.main()
