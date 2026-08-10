#!/usr/bin/env python3
"""Relocation and closed-manifest tests for stage_private_postgres.cmake.

The fixture is a tiny PostgreSQL-shaped installation whose tools load two
transitive sibling libraries through @loader_path (Darwin) or $ORIGIN (Linux).
After staging, the source installation is hidden before every copied tool is
executed.  This proves that the stage contains the exact runtime closure rather
than accidentally resolving it from the source installation.  Every case
reconstructs the current stage-manifest digest and audits the complete
materialized bindir/sharedir/pkglibdir inventory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import subprocess
import sys


TOOLS = ("postgres", "initdb", "pg_ctl", "createdb", "dropdb", "psql")
ROOT_ORDER = {"bindir": 0, "sharedir": 1, "pkglibdir": 2}
ARTIFACT_SPEC_SCHEMA = "bioseq.private_postgres.artifacts"
ARTIFACT_SPEC_SCHEMA_REVISION = 1
STAGE_MANIFEST_SCHEMA = "bioseq.private_postgres.stage_manifest"
STAGE_MANIFEST_SCHEMA_REVISION = 3
RUNTIME_CLOSURE_ALGORITHM = "postgresql.materialized_package"
RUNTIME_CLOSURE_SCHEMA_REVISION = 1
STAGE_MANIFEST_KEYS = {
    "schema",
    "schema_revision",
    "postgresql_version",
    "postgresql_configure",
    "source",
    "staged",
    "extension_artifacts",
    "expected_tool_paths",
    "tool_signatures",
    "runtime_closure",
}
RUNTIME_CLOSURE_KEYS = {
    "algorithm",
    "schema_revision",
    "entry_count",
    "aggregate_sha256",
    "roots",
    "required_sentinels",
    "loader_dependencies",
    "entries",
}
RUNTIME_ENTRY_KEYS = {
    "root",
    "path",
    "kind",
    "source_kind",
    "source_path",
    "source_link_target",
    "resolved_source_path",
    "size",
    "sha256",
}
ROOT_KEYS = {"reported_source", "canonical_source", "staged"}
SENTINEL_KEYS = {"root", "path", "kind", "sha256"}
LOADER_CLOSURE_KEYS = {"entry_count", "aggregate_sha256", "entries"}
LOADER_ENTRY_KEYS = {"source", "staged", "size", "sha256"}
EXTENSION_CLOSURE_KEYS = {"aggregate_sha256", "artifacts"}
EXTENSION_ARTIFACT_KEYS = {
    "name",
    "module_pathname_override",
    "effective_module_name",
    "control",
    "install_sql",
    "library",
}
CONTROL_ARTIFACT_KEYS = {
    "source", "filename", "staged", "source_sha256", "sha256"
}
SQL_ARTIFACT_KEYS = {"source", "filename", "staged", "version", "sha256"}
LIBRARY_ARTIFACT_KEYS = {"source", "filename", "staged", "sha256"}
TOOL_SIGNATURE_KEYS = {"tool", "staged_path", "version", "sha256"}
SHARED_FILE_SENTINELS = {
    "postgres_bki": "postgres.bki",
    "postgresql_conf_sample": "postgresql.conf.sample",
    "pg_hba_conf_sample": "pg_hba.conf.sample",
    "pg_ident_conf_sample": "pg_ident.conf.sample",
    "information_schema_sql": "information_schema.sql",
    "system_functions_sql": "system_functions.sql",
    "system_views_sql": "system_views.sql",
    "system_constraints_sql": "system_constraints.sql",
    "snowball_create_sql": "snowball_create.sql",
}
SHARED_DIR_SENTINELS = {
    "timezone": "timezone",
    "timezonesets": "timezonesets",
    "tsearch_data": "tsearch_data",
    "extension": "extension",
}
PLPGSQL_MODULE = "plpgsql.dylib" if platform.system() == "Darwin" else "plpgsql.so"
SENTINEL_NAMES = {
    *SHARED_FILE_SENTINELS,
    *SHARED_DIR_SENTINELS,
    "plpgsql",
}


def _run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    kwargs.setdefault("timeout", 60)
    return subprocess.run(
        command,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        **kwargs,
    )


def _write(path: Path, text: str, *, executable: bool = False) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    if executable:
        path.chmod(path.stat().st_mode | 0o755)
    return path


def _write_artifact_spec(
    path: Path,
    artifacts: list[dict[str, str | Path]],
    *,
    schema: object = ARTIFACT_SPEC_SCHEMA,
    schema_revision: object = ARTIFACT_SPEC_SCHEMA_REVISION,
    extra_members: dict[str, object] | None = None,
) -> Path:
    document: dict[str, object] = {
        "schema": schema,
        "schema_revision": schema_revision,
        "artifacts": [
            {key: str(value) for key, value in artifact.items()}
            for artifact in artifacts
        ],
    }
    if extra_members:
        document.update(extra_members)
    return _write(path, json.dumps(document, indent=2) + "\n")


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _staged_path(stage_root: Path, source_path: Path) -> Path:
    absolute = source_path.absolute()
    return stage_root / absolute.relative_to(absolute.anchor)


def _assert_exact_keys(value: dict[str, object], expected: set[str],
                       label: str) -> None:
    assert set(value) == expected, (
        f"{label} member drift: expected {sorted(expected)}, "
        f"got {sorted(value)}"
    )


def _stage_relative_path(stage_root: Path, coordinate: str) -> Path:
    relative = PurePosixPath(coordinate)
    assert coordinate and not relative.is_absolute()
    assert relative.as_posix() == coordinate
    assert all(part not in ("", ".", "..") for part in relative.parts)
    return stage_root.joinpath(*relative.parts)


def _runtime_entry_record(entry: dict[str, object]) -> str:
    return (
        f"root:{entry['root']}\n"
        f"path:{entry['path']}\n"
        f"kind:{entry['kind']}\n"
        f"source_kind:{entry['source_kind']}\n"
        f"source_path:{entry['source_path']}\n"
        "source_link_target:"
        f"{entry['source_link_target'] if entry['source_link_target'] is not None else '-'}\n"
        f"resolved_source_path:{entry['resolved_source_path']}\n"
        f"size:{entry['size'] if entry['size'] is not None else '-'}\n"
        f"sha256:{entry['sha256'] if entry['sha256'] is not None else '-'}\n"
    )


def _loader_entry_record(entry: dict[str, object]) -> str:
    return (
        f"source:{entry['source']}\n"
        f"staged:{entry['staged']}\n"
        f"size:{entry['size']}\n"
        f"sha256:{entry['sha256']}\n"
    )


def _loader_pair_sort_key(staged: str, source: str) -> tuple[bytes, bytes]:
    """Return the canonical staged-path/source-path bytewise order."""
    return (staged.encode("utf-8"), source.encode("utf-8"))


def _compile_runtime_fixture(cc: str, bindir: Path, runtime_dir: Path,
                             sources: Path) -> tuple[str, str]:
    runtime_dir.mkdir(parents=True, exist_ok=True)
    bindir.mkdir(parents=True, exist_ok=True)
    leaf_source = sources / "leaf.c"
    support_source = sources / "support.c"
    tool_source = sources / "tool.c"
    initdb_source = sources / "initdb.c"
    _write(leaf_source, "int fixture_leaf(void) { return 41; }\n")
    _write(
        support_source,
        "extern int fixture_leaf(void);\n"
        "int fixture_support(void) { return fixture_leaf() + 1; }\n",
    )
    _write(
        tool_source,
        "#include <stdio.h>\n"
        "extern int fixture_support(void);\n"
        "int main(void) { printf(\"fixture-postgres %d\\n\", fixture_support()); return 0; }\n",
    )
    _write(
        initdb_source,
        "#include <dirent.h>\n"
        "#include <errno.h>\n"
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "#include <sys/stat.h>\n"
        "extern int fixture_support(void);\n"
        "int main(int argc, char **argv) {\n"
        "  if (argc == 2 && strcmp(argv[1], \"--version\") == 0) {\n"
        "    printf(\"fixture-postgres %d\\n\", fixture_support()); return 0;\n"
        "  }\n"
        "  const char *share = 0; const char *data = 0;\n"
        "  for (int i = 1; i + 1 < argc; ++i) {\n"
        "    if (strcmp(argv[i], \"-L\") == 0) share = argv[++i];\n"
        "    else if (strcmp(argv[i], \"-D\") == 0) data = argv[++i];\n"
        "  }\n"
        "  if (!share || !data || fixture_support() != 42) return 2;\n"
        "  const char *files[] = {\"postgres.bki\", \"postgresql.conf.sample\",\n"
        "    \"pg_hba.conf.sample\", \"pg_ident.conf.sample\",\n"
        "    \"information_schema.sql\", \"system_functions.sql\",\n"
        "    \"system_views.sql\", \"system_constraints.sql\",\n"
        "    \"snowball_create.sql\", 0};\n"
        "  const char *dirs[] = {\"timezone\", \"timezonesets\", \"tsearch_data\",\n"
        "    \"extension\", 0};\n"
        "  char path[4096];\n"
        "  for (int i = 0; files[i]; ++i) {\n"
        "    if (snprintf(path, sizeof(path), \"%s/%s\", share, files[i]) < 0)\n"
        "      return 3;\n"
        "    FILE *file = fopen(path, \"rb\"); if (!file) return 4;\n"
        "    (void)fgetc(file); if (fclose(file) != 0) return 5;\n"
        "  }\n"
        "  for (int i = 0; dirs[i]; ++i) {\n"
        "    if (snprintf(path, sizeof(path), \"%s/%s\", share, dirs[i]) < 0)\n"
        "      return 6;\n"
        "    DIR *dir = opendir(path); if (!dir) return 7; closedir(dir);\n"
        "  }\n"
        "  if (mkdir(data, 0700) != 0 && errno != EEXIST) return 8;\n"
        "  puts(\"fixture-initdb materialized-share-ok\"); return 0;\n"
        "}\n",
    )

    if platform.system() == "Darwin":
        leaf_name = "libpg_fixture_leaf.1.dylib"
        leaf_link = "libpg_fixture_leaf.dylib"
        support_name = "libpg_fixture_support.1.dylib"
        support_link = "libpg_fixture_support.dylib"
        _run([
            cc, "-dynamiclib", str(leaf_source),
            "-Wl,-install_name,@rpath/" + leaf_name,
            "-o", str(runtime_dir / leaf_name),
        ])
        os.symlink(leaf_name, runtime_dir / leaf_link)
        _run([
            cc, "-dynamiclib", str(support_source),
            "-L" + str(runtime_dir), "-lpg_fixture_leaf",
            "-Wl,-install_name,@rpath/" + support_name,
            "-Wl,-rpath,@loader_path",
            "-o", str(runtime_dir / support_name),
        ])
        os.symlink(support_name, runtime_dir / support_link)
        tool_rpath = "-Wl,-rpath,@loader_path/../lib"
    elif platform.system() == "Linux":
        leaf_name = "libpg_fixture_leaf.so.1"
        leaf_real = "libpg_fixture_leaf.so.1.0"
        support_name = "libpg_fixture_support.so.1"
        support_real = "libpg_fixture_support.so.1.0"
        _run([
            cc, "-shared", "-fPIC", str(leaf_source),
            "-Wl,-soname," + leaf_name,
            "-o", str(runtime_dir / leaf_real),
        ])
        os.symlink(leaf_real, runtime_dir / leaf_name)
        os.symlink(leaf_name, runtime_dir / "libpg_fixture_leaf.so")
        _run([
            cc, "-shared", "-fPIC", str(support_source),
            "-L" + str(runtime_dir), "-lpg_fixture_leaf",
            "-Wl,-soname," + support_name,
            "-Wl,-rpath,$ORIGIN",
            "-o", str(runtime_dir / support_real),
        ])
        os.symlink(support_real, runtime_dir / support_name)
        os.symlink(support_name, runtime_dir / "libpg_fixture_support.so")
        tool_rpath = "-Wl,-rpath,$ORIGIN/../lib"
    else:
        raise RuntimeError("runtime staging fixture supports Darwin and Linux")

    postgres = bindir / "postgres"
    _run([
        cc, str(tool_source), "-L" + str(runtime_dir),
        "-lpg_fixture_support", tool_rpath, "-o", str(postgres),
    ])
    for tool in TOOLS[1:]:
        shutil.copy2(postgres, bindir / tool)
    _run([
        cc, str(initdb_source), "-L" + str(runtime_dir),
        "-lpg_fixture_support", tool_rpath, "-o", str(bindir / "initdb"),
    ])
    return leaf_name, support_name


def _compile_extension_fixture(
    cc: str,
    artifact_dir: Path,
    module_runtime_dir: Path,
    staged_module_runtime_relative: str,
) -> tuple[Path, str, Path]:
    source = artifact_dir / "bioseq_fixture.c"
    dependency_source = artifact_dir / "module_dependency.c"
    probe_source = artifact_dir / "module_probe.c"
    _write(
        source,
        "extern int fixture_module_only(void);\n"
        "int bioseq_fixture(void) { return fixture_module_only() + 1; }\n",
    )
    _write(dependency_source, "int fixture_module_only(void) { return 83; }\n")
    _write(
        probe_source,
        "#include <dlfcn.h>\n"
        "#include <stdio.h>\n"
        "int main(int argc, char **argv) {\n"
        "  if (argc != 2) return 2;\n"
        "  void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);\n"
        "  if (!handle) { fprintf(stderr, \"%s\\n\", dlerror()); return 3; }\n"
        "  dlerror();\n"
        "  int (*function)(void) = (int (*)(void))dlsym(handle, \"bioseq_fixture\");\n"
        "  const char *error = dlerror();\n"
        "  if (error) { fprintf(stderr, \"%s\\n\", error); return 4; }\n"
        "  printf(\"fixture-module %d\\n\", function());\n"
        "  return dlclose(handle) == 0 ? 0 : 5;\n"
        "}\n",
    )
    module_runtime_dir.mkdir(parents=True, exist_ok=True)
    source_runtime_relative = os.path.relpath(module_runtime_dir, artifact_dir)
    if platform.system() == "Darwin":
        dependency_name = "libpg_fixture_module.1.dylib"
        dependency_link = module_runtime_dir / "libpg_fixture_module.dylib"
        _run([
            cc, "-dynamiclib", str(dependency_source),
            "-Wl,-install_name,@rpath/" + dependency_name,
            "-o", str(module_runtime_dir / dependency_name),
        ])
        os.symlink(dependency_name, dependency_link)
        library = artifact_dir / "bioseq.dylib"
        # PostgreSQL extensions are CMake MODULE targets: on Darwin that is an
        # MH_BUNDLE loadable module, not an MH_DYLIB shared library.
        command = [
            cc, "-bundle", str(source),
            "-L" + str(module_runtime_dir), "-lpg_fixture_module",
            "-Wl,-rpath,@loader_path/" + source_runtime_relative,
            "-Wl,-rpath,@loader_path/" + staged_module_runtime_relative,
            "-Wl,-rpath,@loader_path/..",
            "-o", str(library),
        ]
        probe_link_options: list[str] = []
    else:
        dependency_name = "libpg_fixture_module.so.1"
        dependency_real = "libpg_fixture_module.so.1.0"
        _run([
            cc, "-shared", "-fPIC", str(dependency_source),
            "-Wl,-soname," + dependency_name,
            "-o", str(module_runtime_dir / dependency_real),
        ])
        os.symlink(dependency_real, module_runtime_dir / dependency_name)
        os.symlink(dependency_name, module_runtime_dir / "libpg_fixture_module.so")
        library = artifact_dir / "bioseq.so"
        # ELF represents CMake MODULE targets as non-linked shared objects.
        command = [
            cc, "-shared", "-fPIC", str(source),
            "-L" + str(module_runtime_dir), "-lpg_fixture_module",
            "-Wl,-rpath,$ORIGIN/" + source_runtime_relative,
            "-Wl,-rpath,$ORIGIN/" + staged_module_runtime_relative,
            "-Wl,-rpath,$ORIGIN/..",
            "-o", str(library),
        ]
        probe_link_options = ["-ldl"]
    _run(command)
    probe = artifact_dir / "module_probe"
    _run([cc, str(probe_source), *probe_link_options, "-o", str(probe)])
    return library, dependency_name, probe


def _write_pg_config(path: Path, bindir: Path, sharedir: Path,
                     pkglibdir: Path) -> None:
    values = {
        "--bindir": str(bindir),
        "--sharedir": str(sharedir),
        "--pkglibdir": str(pkglibdir),
        "--version": "PostgreSQL 17.fixture",
        "--configure": "--with-private-runtime-fixture",
    }
    _write(
        path,
        "#!/usr/bin/env python3\n"
        "import sys\n"
        f"values = {values!r}\n"
        "if len(sys.argv) != 2 or sys.argv[1] not in values:\n"
        "    raise SystemExit(2)\n"
        "print(values[sys.argv[1]])\n",
        executable=True,
    )


def _populate_postgres_core(sharedir: Path, pkglibdir: Path) -> None:
    for name, relative in SHARED_FILE_SENTINELS.items():
        _write(sharedir / relative, f"{name} fixture\n")
    for relative in SHARED_DIR_SENTINELS.values():
        (sharedir / relative).mkdir(parents=True, exist_ok=True)
    _write(sharedir / "extension" / "plpgsql.control", "builtin fixture\n")
    _write(pkglibdir / PLPGSQL_MODULE, "builtin plpgsql fixture\n")


def _symlink_to(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.symlink_to(
        os.path.relpath(source, destination.parent),
        target_is_directory=source.is_dir(),
    )


def _create_homebrew_facade(canonical: Path, facade: Path) -> None:
    """Create Homebrew's real directory containers and per-file cellar links."""
    facade.mkdir(parents=True, exist_ok=True)
    children = sorted(
        canonical.rglob("*"),
        key=lambda child: (len(child.relative_to(canonical).parts), str(child)),
    )
    for source in children:
        relative = source.relative_to(canonical)
        destination = facade / relative
        if source.is_dir() and not source.is_symlink():
            destination.mkdir(parents=True, exist_ok=True)
        else:
            _symlink_to(source, destination)


def _overlay_coordinates(
    manifest: dict[str, object],
) -> set[tuple[str, str]]:
    closure = manifest["runtime_closure"]
    assert isinstance(closure, dict)
    roots = closure["roots"]
    assert isinstance(roots, dict)
    coordinates: set[tuple[str, str]] = set()
    extension_artifacts = manifest["extension_artifacts"]
    assert isinstance(extension_artifacts, dict)
    artifacts = extension_artifacts["artifacts"]
    assert isinstance(artifacts, list)
    for artifact in artifacts:
        assert isinstance(artifact, dict)
        for member, root in (
            ("control", "sharedir"),
            ("install_sql", "sharedir"),
            ("library", "pkglibdir"),
        ):
            artifact_member = artifact[member]
            assert isinstance(artifact_member, dict)
            root_metadata = roots[root]
            assert isinstance(root_metadata, dict)
            staged = PurePosixPath(str(artifact_member["staged"]))
            root_staged = PurePosixPath(str(root_metadata["staged"]))
            relative = staged.relative_to(root_staged).as_posix()
            coordinates.add((root, relative))
    return coordinates


def _audit_runtime_closure(stage_root: Path,
                           manifest: dict[str, object]) -> None:
    """Recompute the sealed tree exactly as the runtime consumer must."""
    closure = manifest["runtime_closure"]
    assert isinstance(closure, dict)
    entries = closure["entries"]
    assert isinstance(entries, list)
    overlay_coordinates = _overlay_coordinates(manifest)

    entry_keys: list[tuple[int, str]] = []
    sealed_coordinates: set[tuple[str, str]] = set()
    declared_stage_files: set[Path] = set()
    records = ""
    for entry in entries:
        assert isinstance(entry, dict)
        _assert_exact_keys(entry, RUNTIME_ENTRY_KEYS, "runtime entry")
        root = str(entry["root"])
        relative = str(entry["path"])
        assert root in ROOT_ORDER
        relative_path = PurePosixPath(relative)
        assert relative and not relative_path.is_absolute()
        assert relative_path.as_posix() == relative
        assert all(part not in ("", ".", "..") for part in relative_path.parts)
        key = (root, relative)
        assert key not in sealed_coordinates
        assert key not in overlay_coordinates
        sealed_coordinates.add(key)
        entry_keys.append((ROOT_ORDER[root], relative))

        roots = closure["roots"]
        assert isinstance(roots, dict)
        root_metadata = roots[root]
        assert isinstance(root_metadata, dict)
        root_destination = _stage_relative_path(
            stage_root, str(root_metadata["staged"])
        )
        destination = root_destination.joinpath(*relative_path.parts)
        assert not destination.is_symlink()
        if entry["kind"] == "dir":
            assert destination.is_dir()
            assert entry["size"] is None
            assert entry["sha256"] is None
        else:
            assert entry["kind"] == "file"
            assert destination.is_file()
            assert entry["size"] == destination.stat().st_size
            assert entry["sha256"] == _sha256(destination)
            declared_stage_files.add(destination)
        assert entry["source_kind"] in {"dir", "file", "symlink"}
        assert Path(str(entry["source_path"])).is_absolute()
        resolved_source = Path(str(entry["resolved_source_path"]))
        assert resolved_source.is_absolute()
        canonical_source = Path(str(root_metadata["canonical_source"]))
        resolved_source.relative_to(canonical_source)
        if entry["kind"] == "dir":
            assert resolved_source.is_dir()
        else:
            assert resolved_source.is_file()
            assert entry["sha256"] == _sha256(resolved_source)
        if entry["source_kind"] == "symlink":
            assert isinstance(entry["source_link_target"], str)
            assert entry["source_link_target"]
        else:
            assert entry["source_link_target"] is None
        records += _runtime_entry_record(entry)

    assert entry_keys == sorted(entry_keys)
    assert closure["entry_count"] == len(entries)
    assert closure["aggregate_sha256"] == hashlib.sha256(
        records.encode("utf-8")
    ).hexdigest()

    roots = closure["roots"]
    assert isinstance(roots, dict)
    actual_coordinates: set[tuple[str, str]] = set()
    for root in ROOT_ORDER:
        root_metadata = roots[root]
        assert isinstance(root_metadata, dict)
        root_destination = _stage_relative_path(
            stage_root, str(root_metadata["staged"])
        )
        assert root_destination.is_dir() and not root_destination.is_symlink()
        for destination in root_destination.rglob("*"):
            assert not destination.is_symlink()
            relative = destination.relative_to(root_destination).as_posix()
            actual_coordinates.add((root, relative))
    loader_root_coordinates: set[tuple[str, str]] = set()
    raw_loader_dependencies = closure["loader_dependencies"]
    assert isinstance(raw_loader_dependencies, dict)
    raw_loader_entries = raw_loader_dependencies["entries"]
    assert isinstance(raw_loader_entries, list)
    for loader_entry in raw_loader_entries:
        assert isinstance(loader_entry, dict)
        loader_destination = _stage_relative_path(
            stage_root, str(loader_entry["staged"])
        )
        for root in ROOT_ORDER:
            root_metadata = roots[root]
            assert isinstance(root_metadata, dict)
            root_destination = _stage_relative_path(
                stage_root, str(root_metadata["staged"])
            )
            try:
                relative_loader = loader_destination.relative_to(
                    root_destination
                )
            except ValueError:
                continue
            for depth in range(1, len(relative_loader.parts) + 1):
                loader_root_coordinates.add((
                    root,
                    PurePosixPath(
                        *relative_loader.parts[:depth]
                    ).as_posix(),
                ))
    assert actual_coordinates == (
        sealed_coordinates | overlay_coordinates | loader_root_coordinates
    )
    for root, relative in overlay_coordinates:
        root_metadata = roots[root]
        assert isinstance(root_metadata, dict)
        overlay = _stage_relative_path(
            stage_root, str(root_metadata["staged"])
        ).joinpath(*PurePosixPath(relative).parts)
        assert overlay.is_file() and not overlay.is_symlink()
        declared_stage_files.add(overlay)

    loader_dependencies = closure["loader_dependencies"]
    assert isinstance(loader_dependencies, dict)
    _assert_exact_keys(
        loader_dependencies, LOADER_CLOSURE_KEYS, "loader dependencies"
    )
    loader_entries = loader_dependencies["entries"]
    assert isinstance(loader_entries, list)
    overlay_stage_paths = {
        str(artifact[member]["staged"])
        for artifact in manifest["extension_artifacts"]["artifacts"]
        for member in ("control", "install_sql", "library")
    }
    loader_records = ""
    staged_loader_coordinates: set[str] = set()
    loader_keys: list[tuple[bytes, bytes]] = []
    for entry in loader_entries:
        assert isinstance(entry, dict)
        _assert_exact_keys(entry, LOADER_ENTRY_KEYS, "loader entry")
        source = Path(str(entry["source"]))
        assert source.is_absolute() and source.is_file()
        assert source == source.resolve()
        staged = str(entry["staged"])
        assert staged not in overlay_stage_paths
        assert staged not in staged_loader_coordinates
        staged_loader_coordinates.add(staged)
        destination = _stage_relative_path(stage_root, staged)
        assert destination.is_file() and not destination.is_symlink()
        declared_stage_files.add(destination)
        assert entry["size"] == destination.stat().st_size
        assert entry["sha256"] == _sha256(destination)
        assert entry["sha256"] == _sha256(source)
        loader_keys.append(_loader_pair_sort_key(staged, str(source)))
        loader_records += _loader_entry_record(entry)
    assert loader_keys == sorted(loader_keys)
    assert loader_dependencies["entry_count"] == len(loader_entries)
    assert loader_dependencies["aggregate_sha256"] == hashlib.sha256(
        loader_records.encode("utf-8")
    ).hexdigest()
    actual_stage_files = {
        path for path in stage_root.rglob("*") if path.is_file()
    }
    assert actual_stage_files == declared_stage_files


def _assert_runtime_audit_detects_mutations(
    stage_root: Path,
    manifest: dict[str, object],
) -> None:
    closure = manifest["runtime_closure"]
    assert isinstance(closure, dict)
    entries = closure["entries"]
    assert isinstance(entries, list)
    roots = closure["roots"]
    assert isinstance(roots, dict)
    file_entry = next(
        entry for entry in entries
        if isinstance(entry, dict) and entry["kind"] == "file"
    )
    file_root = str(file_entry["root"])
    root_metadata = roots[file_root]
    assert isinstance(root_metadata, dict)
    destination = _stage_relative_path(
        stage_root, str(root_metadata["staged"])
    ) / str(file_entry["path"])
    original = destination.read_bytes()
    original_mode = destination.stat().st_mode

    destination.unlink()
    try:
        try:
            _audit_runtime_closure(stage_root, manifest)
        except AssertionError:
            pass
        else:
            raise AssertionError("runtime closure audit accepted a deletion")
    finally:
        destination.write_bytes(original)
        destination.chmod(original_mode)

    destination.write_bytes(original + b"\ntampered\n")
    try:
        try:
            _audit_runtime_closure(stage_root, manifest)
        except AssertionError:
            pass
        else:
            raise AssertionError("runtime closure audit accepted tampering")
    finally:
        destination.write_bytes(original)
        destination.chmod(original_mode)

    shared_metadata = roots["sharedir"]
    assert isinstance(shared_metadata, dict)
    addition = _stage_relative_path(
        stage_root, str(shared_metadata["staged"])
    ) / "unexpected-runtime-addition.fixture"
    addition.write_text("must be rejected\n")
    try:
        try:
            _audit_runtime_closure(stage_root, manifest)
        except AssertionError:
            pass
        else:
            raise AssertionError("runtime closure audit accepted an addition")
    finally:
        addition.unlink()

    loader_dependencies = closure["loader_dependencies"]
    assert isinstance(loader_dependencies, dict)
    loader_entries = loader_dependencies["entries"]
    assert isinstance(loader_entries, list) and loader_entries
    first_loader = loader_entries[0]
    assert isinstance(first_loader, dict)
    loader_destination = _stage_relative_path(
        stage_root, str(first_loader["staged"])
    )
    loader_addition = (
        loader_destination.parent / "unexpected-loader-sibling.fixture"
    )
    loader_addition.write_text("must be rejected\n")
    try:
        try:
            _audit_runtime_closure(stage_root, manifest)
        except AssertionError:
            pass
        else:
            raise AssertionError(
                "runtime closure audit accepted a loader sibling"
            )
    finally:
        loader_addition.unlink()
    _audit_runtime_closure(stage_root, manifest)


def _verify_stage_manifest(
    manifest_path: Path,
    hash_path: Path,
    *,
    artifacts: list[dict[str, str | Path]],
    stage_root: Path,
    reported_roots: dict[str, Path],
    canonical_roots: dict[str, Path],
) -> dict[str, object]:
    manifest = json.loads(manifest_path.read_text())
    assert isinstance(manifest, dict)
    _assert_exact_keys(manifest, STAGE_MANIFEST_KEYS, "stage manifest")
    assert manifest["schema"] == STAGE_MANIFEST_SCHEMA
    assert type(manifest["schema_revision"]) is int
    assert manifest["schema_revision"] == STAGE_MANIFEST_SCHEMA_REVISION
    assert manifest["postgresql_version"] == "PostgreSQL 17.fixture"
    assert manifest["postgresql_configure"] == (
        "--with-private-runtime-fixture"
    )
    assert manifest["source"] == {
        root: str(reported_roots[root]) for root in ROOT_ORDER
    }
    assert manifest["staged"] == {
        root: _staged_path(stage_root, reported_roots[root])
        .relative_to(stage_root)
        .as_posix()
        for root in ROOT_ORDER
    }
    assert set(manifest["expected_tool_paths"]) == set(TOOLS)
    signatures = manifest["tool_signatures"]
    assert isinstance(signatures, list)
    for signature in signatures:
        _assert_exact_keys(
            signature, TOOL_SIGNATURE_KEYS, "tool signature"
        )
        tool = str(signature["tool"])
        expected_tool_path = (
            str(manifest["staged"]["bindir"]) + "/" + tool
        )
        assert manifest["expected_tool_paths"][tool] == expected_tool_path
        assert signature["staged_path"] == expected_tool_path
        staged_tool = _stage_relative_path(stage_root, expected_tool_path)
        assert signature["sha256"] == _sha256(staged_tool)
        assert signature["version"] == "fixture-postgres 42"
    assert {entry["tool"] for entry in signatures} == set(TOOLS)

    closure = manifest["runtime_closure"]
    assert isinstance(closure, dict)
    _assert_exact_keys(closure, RUNTIME_CLOSURE_KEYS, "runtime closure")
    assert closure["algorithm"] == RUNTIME_CLOSURE_ALGORITHM
    assert type(closure["schema_revision"]) is int
    assert (
        closure["schema_revision"] == RUNTIME_CLOSURE_SCHEMA_REVISION
    )
    serialized_closure = json.dumps(closure, sort_keys=True)
    assert str(stage_root) not in serialized_closure
    assert "external-citus" not in serialized_closure
    assert "bioseq--globally-installed.sql" not in serialized_closure
    assert "bioseq_cache.fixture" not in serialized_closure
    assert "libunrelated.fixture" not in serialized_closure
    roots = closure["roots"]
    assert isinstance(roots, dict)
    assert set(roots) == set(ROOT_ORDER)
    for root in ROOT_ORDER:
        metadata = roots[root]
        assert isinstance(metadata, dict)
        _assert_exact_keys(metadata, ROOT_KEYS, f"{root} root")
        assert metadata == {
            "reported_source": str(reported_roots[root]),
            "canonical_source": str(canonical_roots[root]),
            "staged": _staged_path(stage_root, reported_roots[root])
            .relative_to(stage_root)
            .as_posix(),
        }

    sentinels = closure["required_sentinels"]
    assert isinstance(sentinels, dict)
    assert set(sentinels) == SENTINEL_NAMES
    expected_sentinels = {
        name: ("sharedir", relative, "file")
        for name, relative in SHARED_FILE_SENTINELS.items()
    }
    expected_sentinels.update({
        name: ("sharedir", relative, "dir")
        for name, relative in SHARED_DIR_SENTINELS.items()
    })
    expected_sentinels["plpgsql"] = (
        "pkglibdir", PLPGSQL_MODULE, "file"
    )
    for name, (root, relative, kind) in expected_sentinels.items():
        sentinel = sentinels[name]
        assert isinstance(sentinel, dict)
        _assert_exact_keys(sentinel, SENTINEL_KEYS, f"{name} sentinel")
        assert sentinel["root"] == root
        assert sentinel["path"] == relative
        assert sentinel["kind"] == kind
        destination = _stage_relative_path(
            stage_root, str(roots[root]["staged"])
        ) / relative
        assert not destination.is_symlink()
        if kind == "dir":
            assert destination.is_dir()
            assert sentinel["sha256"] is None
        else:
            assert destination.is_file()
            assert sentinel["sha256"] == _sha256(destination)
        matching_entry = next(
            entry for entry in closure["entries"]
            if entry["root"] == root and entry["path"] == relative
        )
        assert matching_entry["kind"] == kind
        assert matching_entry["sha256"] == sentinel["sha256"]

    extension_closure = manifest["extension_artifacts"]
    assert isinstance(extension_closure, dict)
    _assert_exact_keys(
        extension_closure, EXTENSION_CLOSURE_KEYS, "extension artifacts"
    )
    entries = extension_closure["artifacts"]
    assert [entry["name"] for entry in entries] == [
        artifact["name"] for artifact in artifacts
    ]
    for entry in entries:
        _assert_exact_keys(
            entry, EXTENSION_ARTIFACT_KEYS, "extension artifact"
        )
        _assert_exact_keys(
            entry["control"], CONTROL_ARTIFACT_KEYS, "control artifact"
        )
        _assert_exact_keys(
            entry["install_sql"], SQL_ARTIFACT_KEYS, "SQL artifact"
        )
        _assert_exact_keys(
            entry["library"], LIBRARY_ARTIFACT_KEYS, "library artifact"
        )
    by_name = {entry["name"]: entry for entry in entries}
    record = ""
    for artifact in sorted(artifacts, key=lambda item: str(item["name"])):
        name = str(artifact["name"])
        control = Path(artifact["control"])
        install_sql = Path(artifact["install_sql"])
        library = Path(artifact["library"])
        override = str(artifact.get("module_pathname_override", ""))
        staged_share = _stage_relative_path(
            stage_root, str(roots["sharedir"]["staged"])
        )
        staged_pkglib = _stage_relative_path(
            stage_root, str(roots["pkglibdir"]["staged"])
        )
        staged_control = staged_share / "extension" / control.name
        staged_sql = staged_share / "extension" / install_sql.name
        staged_library = staged_pkglib / library.name
        entry = by_name[name]
        assert entry["module_pathname_override"] == (override or None)
        assert entry["effective_module_name"] == (
            override or library.name.split(".", 1)[0]
        )
        assert entry["control"]["source"] == str(control)
        assert entry["install_sql"]["source"] == str(install_sql)
        assert entry["library"]["source"] == str(library)
        assert entry["control"]["source_sha256"] == _sha256(control)
        assert entry["control"]["sha256"] == _sha256(staged_control)
        assert entry["install_sql"]["sha256"] == _sha256(staged_sql)
        assert entry["library"]["sha256"] == _sha256(staged_library)
        record += (
            f"artifact:{name}\n"
            f"module_pathname_override:{override}\n"
            f"control:{control.name}:{_sha256(staged_control)}\n"
            f"install_sql:{install_sql.name}:{_sha256(staged_sql)}\n"
            f"library:{library.name}:{_sha256(staged_library)}\n"
        )
    expected = hashlib.sha256(record.encode()).hexdigest()
    assert manifest["extension_artifacts"]["aggregate_sha256"] == expected
    assert hash_path.read_text() == expected + "\n"
    _audit_runtime_closure(stage_root, manifest)
    closure_entries = {
        (entry["root"], entry["path"]): entry
        for entry in closure["entries"]
    }
    for root, relative in (
        ("sharedir", "postgres.bki"),
        ("pkglibdir", PLPGSQL_MODULE),
    ):
        if reported_roots[root] == canonical_roots[root]:
            continue
        entry = closure_entries[(root, relative)]
        assert entry["source_kind"] == "symlink"
        assert entry["source_path"] == str(reported_roots[root] / relative)
        assert entry["source_link_target"]
        assert entry["resolved_source_path"] == str(
            canonical_roots[root] / relative
        )
    return manifest


def _run_layout_case(
    cmake: str,
    stage_script: Path,
    scratch: Path,
    cc: str,
    layout: str,
    *,
    multiple_artifacts: bool,
) -> None:
    artifact_shape = (
        "multiple-artifacts" if multiple_artifacts else "one-artifact"
    )
    case_name = f"{layout}-{artifact_shape}"
    case_root = scratch / case_name
    source_root = case_root / "source"
    if layout == "nested-runtime":
        prefix = source_root / "Postgres.app" / "Contents" / "Versions" / "17"
        bindir = prefix / "bin"
        runtime_dir = prefix / "lib"
        canonical_sharedir = prefix / "share" / "postgresql"
        canonical_pkglibdir = runtime_dir / "postgresql"
        sharedir = canonical_sharedir
        pkglibdir = canonical_pkglibdir
    elif layout == "split-homebrew":
        brew = source_root / "homebrew"
        prefix = brew / "Cellar" / "postgresql@17" / "17.fixture"
        bindir = prefix / "bin"
        runtime_dir = prefix / "lib"
        canonical_sharedir = prefix / "share" / "postgresql"
        canonical_pkglibdir = runtime_dir / "postgresql"
        sharedir = brew / "share" / "postgresql@17"
        pkglibdir = brew / "lib" / "postgresql@17"
    else:
        raise AssertionError(f"unknown fixture layout {layout}")

    sources = case_root / "sources"
    leaf_name, support_name = _compile_runtime_fixture(
        cc, bindir, runtime_dir, sources)
    _populate_postgres_core(canonical_sharedir, canonical_pkglibdir)
    _write(runtime_dir / "libunrelated.fixture", "must not be broadly copied\n")

    artifact_dir = case_root / "artifacts"
    artifact_dir.mkdir(parents=True)
    control = artifact_dir / "bioseq.control"
    install_sql = artifact_dir / "bioseq--1.0.sql"
    _write(
        control,
        "default_version = '1.0'\n"
        "module_pathname = '$libdir/bioseq'\n",
    )
    _write(install_sql, "SELECT 1;\n")
    module_runtime_dir = canonical_pkglibdir.parent
    library, module_dependency_name, module_probe = _compile_extension_fixture(
        cc,
        artifact_dir,
        module_runtime_dir,
        os.path.relpath(module_runtime_dir, pkglibdir),
    )

    pg_config = bindir / "pg_config"
    stage_root = case_root / "stage"
    manifest = case_root / "stage-manifest.json"
    hash_path = case_root / "staged-extension.sha256"
    command = [
        cmake,
        "-DPG_CONFIG=" + str(pg_config),
        "-DSTAGE_ROOT=" + str(stage_root),
        "-DSTAGE_PARENT=" + str(case_root),
        "-DMANIFEST_OUT=" + str(manifest),
        "-DHASH_OUT=" + str(hash_path),
    ]
    artifacts: list[dict[str, str | Path]] = [
        {
            "name": "bioseq",
            "control": control,
            "install_sql": install_sql,
            "library": library,
        },
    ]
    if multiple_artifacts:
        cache_control = artifact_dir / "bioseq_cache.control"
        cache_install_sql = artifact_dir / "bioseq_cache--1.0.sql"
        cache_library = artifact_dir / library.name.replace(
            "bioseq", "bioseq_cache", 1
        )
        _write(
            cache_control,
            "default_version = '1.0'\n"
            "comment = 'preserve this byte-for-byte'\n"
            "module_pathname = '$libdir/not_the_staged_module'\n",
        )
        _write(cache_install_sql, "SELECT 2;\n")
        shutil.copy2(library, cache_library)
        loader_match_control = artifact_dir / "bioseq_loader_match.control"
        loader_match_sql = artifact_dir / "bioseq_loader_match--1.0.sql"
        loader_match_library = (runtime_dir / support_name).resolve()
        _write(loader_match_control, "default_version = '1.0'\n")
        _write(loader_match_sql, "SELECT 3;\n")
        artifacts.extend([
            {
                "name": "bioseq_cache",
                "control": cache_control,
                "install_sql": cache_install_sql,
                "library": cache_library,
                "module_pathname_override": "bioseq_cache",
            },
            {
                "name": "bioseq_loader_match",
                "control": loader_match_control,
                "install_sql": loader_match_sql,
                "library": loader_match_library,
            },
        ])
    artifact_spec = _write_artifact_spec(
        case_root / "extension-artifacts.json", artifacts
    )
    command.append("-DARTIFACT_SPEC=" + str(artifact_spec))

    # Exact overlay coordinates deliberately contain stale package bytes. They
    # must be excluded from the core closure and then replaced only by the
    # declared artifact set.
    for artifact in artifacts:
        stale_control = Path(artifact["control"])
        stale_sql = Path(artifact["install_sql"])
        stale_library = Path(artifact["library"])
        _write(
            canonical_sharedir / "extension" / stale_control.name,
            "stale package control must not be attested\n",
        )
        _write(
            canonical_sharedir / "extension" / stale_sql.name,
            "stale package SQL must not be attested\n",
        )
        _write(
            canonical_pkglibdir / stale_library.name,
            "stale package DSO must not be attested\n",
        )

    if multiple_artifacts:
        loader_match_library = Path(artifacts[2]["library"])
        loader_match_alias = canonical_pkglibdir / loader_match_library.name
        loader_match_alias.unlink()
        _symlink_to(loader_match_library, loader_match_alias)

    if layout == "split-homebrew":
        _create_homebrew_facade(canonical_sharedir, sharedir)
        _create_homebrew_facade(canonical_pkglibdir, pkglibdir)
        external = source_root / "external-citus"
        citus_control = _write(
            external / "share" / "extension" / "citus.control",
            "external package\n",
        )
        citus_sql = _write(
            external / "share" / "extension" / "citus--99.sql",
            "external package\n",
        )
        citus_library = _write(
            external / "lib" / ("citus.dylib" if platform.system() == "Darwin"
                                else "citus.so"),
            "external package\n",
        )
        _symlink_to(citus_control, sharedir / "extension" / citus_control.name)
        _symlink_to(citus_sql, sharedir / "extension" / citus_sql.name)
        _symlink_to(citus_library, pkglibdir / citus_library.name)
        _write(
            sharedir / "extension" / "bioseq--globally-installed.sql",
            "facade-only global extension\n",
        )
        _write(
            pkglibdir / "bioseq_cache.fixture",
            "facade-only global module\n",
        )

    _write_pg_config(pg_config, bindir, sharedir, pkglibdir)
    command.extend(["-P", str(stage_script)])
    result = _run(command)
    assert "stage_private_postgres: staged PostgreSQL 17.fixture" in result.stdout

    staged_bindir = _staged_path(stage_root, bindir)
    staged_runtime = _staged_path(stage_root, runtime_dir)
    staged_module_runtime = _staged_path(stage_root, module_runtime_dir)
    staged_share = _staged_path(stage_root, sharedir)
    staged_pkglib = _staged_path(stage_root, pkglibdir)
    for staged_root_coordinate in (
        staged_bindir,
        staged_share,
        staged_pkglib,
    ):
        assert (
            staged_root_coordinate.is_dir()
            and not staged_root_coordinate.is_symlink()
        )
    assert (staged_runtime / leaf_name).exists()
    assert (staged_runtime / support_name).exists()
    assert (staged_module_runtime / module_dependency_name).exists()
    assert not (staged_runtime / "libunrelated.fixture").exists()
    assert not (
        staged_share / "extension" / "bioseq--globally-installed.sql"
    ).exists()
    assert not (staged_share / "extension" / "citus.control").exists()
    assert not (staged_share / "extension" / "citus--99.sql").exists()
    assert not (staged_pkglib / "bioseq_cache.fixture").exists()
    assert not any(staged_pkglib.glob("citus.*"))
    assert (
        staged_share / "extension" / control.name
    ).read_bytes() == control.read_bytes()
    assert (
        staged_share / "extension" / install_sql.name
    ).read_bytes() == install_sql.read_bytes()
    assert (staged_pkglib / library.name).read_bytes() == library.read_bytes()
    assert (staged_pkglib / PLPGSQL_MODULE).is_file()
    assert not any(path.is_symlink() for path in stage_root.rglob("*"))

    if multiple_artifacts:
        cache_artifact = artifacts[1]
        cache_control = Path(cache_artifact["control"])
        cache_install_sql = Path(cache_artifact["install_sql"])
        cache_library = Path(cache_artifact["library"])
        assert cache_control.read_text().endswith(
            "module_pathname = '$libdir/not_the_staged_module'\n"
        )
        assert (
            staged_share / "extension" / cache_control.name
        ).read_text() == (
            "default_version = '1.0'\n"
            "comment = 'preserve this byte-for-byte'\n"
            "module_pathname = 'bioseq_cache'\n"
        )
        assert (
            staged_share / "extension" / cache_install_sql.name
        ).read_bytes() == cache_install_sql.read_bytes()
        assert (
            staged_pkglib / cache_library.name
        ).read_bytes() == cache_library.read_bytes()
        first_manifest = _verify_stage_manifest(
            manifest,
            hash_path,
            artifacts=artifacts,
            stage_root=stage_root,
            reported_roots={
                "bindir": bindir,
                "sharedir": sharedir,
                "pkglibdir": pkglibdir,
            },
            canonical_roots={
                "bindir": bindir,
                "sharedir": canonical_sharedir,
                "pkglibdir": canonical_pkglibdir,
            },
        )
        _assert_runtime_audit_detects_mutations(stage_root, first_manifest)
        rerun = _run(command)
        assert (
            "stage_private_postgres: staged PostgreSQL 17.fixture"
            in rerun.stdout
        )
        second_manifest = _verify_stage_manifest(
            manifest,
            hash_path,
            artifacts=artifacts,
            stage_root=stage_root,
            reported_roots={
                "bindir": bindir,
                "sharedir": sharedir,
                "pkglibdir": pkglibdir,
            },
            canonical_roots={
                "bindir": bindir,
                "sharedir": canonical_sharedir,
                "pkglibdir": canonical_pkglibdir,
            },
        )
        assert (
            first_manifest["runtime_closure"]
            == second_manifest["runtime_closure"]
        ), "runtime closure changed across byte-identical staging runs"
        if layout == "split-homebrew":
            loader_match_library = Path(artifacts[2]["library"])
            collision_library = (
                artifact_dir / "loader-overlay-collision"
                / loader_match_library.name
            )
            collision_library.parent.mkdir(parents=True)
            shutil.copy2(loader_match_library, collision_library)
            collision_artifacts = [dict(artifact) for artifact in artifacts]
            collision_artifacts[2]["library"] = collision_library
            _write_artifact_spec(artifact_spec, collision_artifacts)
            collision_result = subprocess.run(
                command,
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=60,
            )
            assert collision_result.returncode != 0
            assert "loader/overlay coordinate collision" in (
                collision_result.stdout + collision_result.stderr
            )
            assert not manifest.exists() and not hash_path.exists()
            _write_artifact_spec(artifact_spec, artifacts)
            _run(command)
    else:
        _verify_stage_manifest(
            manifest,
            hash_path,
            artifacts=artifacts,
            stage_root=stage_root,
            reported_roots={
                "bindir": bindir,
                "sharedir": sharedir,
                "pkglibdir": pkglibdir,
            },
            canonical_roots={
                "bindir": bindir,
                "sharedir": canonical_sharedir,
                "pkglibdir": canonical_pkglibdir,
            },
        )

    hidden_source = case_root / "source-hidden"
    source_root.rename(hidden_source)
    for tool in TOOLS:
        completed = _run([str(staged_bindir / tool), "--version"])
        assert completed.stdout == "fixture-postgres 42\n"
    initialized = _run([
        str(staged_bindir / "initdb"),
        "-L",
        str(staged_share),
        "-D",
        str(case_root / "initdb-data"),
    ])
    assert initialized.stdout == "fixture-initdb materialized-share-ok\n"
    loaded = _run([str(module_probe), str(staged_pkglib / library.name)])
    assert loaded.stdout == "fixture-module 84\n"
    if multiple_artifacts:
        cache_library = Path(artifacts[1]["library"])
        loaded = _run([
            str(module_probe),
            str(staged_pkglib / cache_library.name),
        ])
        assert loaded.stdout == "fixture-module 84\n"


def _run_artifact_spec_contract_cases(
    cmake: str,
    stage_script: Path,
    scratch: Path,
) -> None:
    case_root = scratch / "artifact-spec-contract"
    artifacts_root = case_root / "artifacts"
    control = _write(
        artifacts_root / "bioseq.control", "default_version = '1.0'\n"
    )
    install_sql = _write(
        artifacts_root / "bioseq--1.0.sql", "SELECT 1;\n"
    )
    library = _write(
        artifacts_root
        / ("bioseq.dylib" if platform.system() == "Darwin" else "bioseq.so"),
        "fixture module\n",
    )
    artifacts: list[dict[str, str | Path]] = [{
        "name": "bioseq",
        "control": control,
        "install_sql": install_sql,
        "library": library,
    }]

    def run_rejected(
        label: str,
        *,
        document: object | None,
        expected_diagnostic: str,
        extra_arguments: list[str] | None = None,
    ) -> None:
        case = case_root / label
        manifest = case / "manifest.json"
        artifact_hash = case / "artifacts.sha256"
        command = [
            cmake,
            "-DPG_CONFIG=" + str(stage_script),
            "-DSTAGE_ROOT=" + str(case / "stage"),
            "-DSTAGE_PARENT=" + str(case),
            "-DMANIFEST_OUT=" + str(manifest),
            "-DHASH_OUT=" + str(artifact_hash),
        ]
        if document is not None:
            spec = _write(
                case / "extension-artifacts.json",
                json.dumps(document, indent=2) + "\n",
            )
            command.append("-DARTIFACT_SPEC=" + str(spec))
        command.extend(extra_arguments or [])
        command.extend(["-P", str(stage_script)])
        result = subprocess.run(
            command,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=60,
        )
        diagnostic = result.stdout + result.stderr
        assert result.returncode != 0, f"{label} must be rejected"
        assert expected_diagnostic in diagnostic, diagnostic
        assert not manifest.exists()
        assert not artifact_hash.exists()

    current_document: dict[str, object] = {
        "schema": ARTIFACT_SPEC_SCHEMA,
        "schema_revision": ARTIFACT_SPEC_SCHEMA_REVISION,
        "artifacts": [
            {key: str(value) for key, value in artifact.items()}
            for artifact in artifacts
        ],
    }
    run_rejected(
        "artifact-spec-required",
        document=None,
        expected_diagnostic="-DARTIFACT_SPEC is required",
    )
    run_rejected(
        "single-artifact-input",
        document=None,
        expected_diagnostic="unsupported single-artifact input",
        extra_arguments=[
            "-DCONTROL_FILE=" + str(control),
            "-DINSTALL_SQL=" + str(install_sql),
            "-DLIBRARY_FILE=" + str(library),
            "-DEXTENSION_VERSION=1.0",
        ],
    )
    run_rejected(
        "mixed-single-artifact-input",
        document=current_document,
        expected_diagnostic="unsupported single-artifact input",
        extra_arguments=["-DCONTROL_FILE=" + str(control)],
    )
    versioned_identity = dict(current_document)
    versioned_identity["schema"] = "bioseq_private_postgres_artifacts/v1"
    run_rejected(
        "versioned-schema-identity",
        document=versioned_identity,
        expected_diagnostic=(
            "ARTIFACT_SPEC schema must be bioseq.private_postgres.artifacts"
        ),
    )
    missing_revision = dict(current_document)
    del missing_revision["schema_revision"]
    run_rejected(
        "missing-schema-revision",
        document=missing_revision,
        expected_diagnostic="must contain exactly schema, schema_revision",
    )
    run_rejected(
        "nonobject-root",
        document=[],
        expected_diagnostic="ARTIFACT_SPEC root must be an object",
    )
    nonarray_artifacts = dict(current_document)
    nonarray_artifacts["artifacts"] = {}
    run_rejected(
        "nonarray-artifacts",
        document=nonarray_artifacts,
        expected_diagnostic="ARTIFACT_SPEC artifacts must be an array",
    )
    empty_artifacts = dict(current_document)
    empty_artifacts["artifacts"] = []
    run_rejected(
        "empty-artifacts",
        document=empty_artifacts,
        expected_diagnostic="ARTIFACT_SPEC artifacts must not be empty",
    )
    for label, revision in (
        ("string-schema-revision", "1"),
        ("boolean-schema-revision", True),
        ("stale-schema-revision", 2),
    ):
        invalid_revision = dict(current_document)
        invalid_revision["schema_revision"] = revision
        run_rejected(
            label,
            document=invalid_revision,
            expected_diagnostic=(
                "schema_revision must be the numeric current revision 1"
            ),
        )
    unknown_root_member = dict(current_document)
    unknown_root_member["unexpected"] = None
    run_rejected(
        "unknown-root-member",
        document=unknown_root_member,
        expected_diagnostic="must contain exactly schema, schema_revision",
    )
    unknown_artifact_member = json.loads(json.dumps(current_document))
    unknown_artifact_member["artifacts"][0]["unexpected"] = None
    run_rejected(
        "unknown-artifact-member",
        document=unknown_artifact_member,
        expected_diagnostic="unknown member 'unexpected' in artifact 0",
    )


def _run_symlink_parent_guard_case(
    cmake: str,
    stage_script: Path,
    scratch: Path,
) -> None:
    case_root = scratch / "symlink-parent-guard"
    candidate = case_root / "candidate"
    candidate.mkdir(parents=True)
    outside = case_root / "outside"
    outside_stage = outside / "production"
    outside_stage.mkdir(parents=True)
    sentinel = _write(
        outside_stage / "must-survive.txt",
        "outside stage must not be deleted\n",
    )
    stage_parent = candidate / "stages"
    stage_parent.symlink_to(outside, target_is_directory=True)

    prefix = case_root / "source"
    bindir = prefix / "bin"
    sharedir = prefix / "share" / "postgresql"
    pkglibdir = prefix / "lib" / "postgresql"
    for tool in TOOLS:
        _write(
            bindir / tool,
            "#!/bin/sh\nprintf 'fixture-postgres 17\\n'\n",
            executable=True,
        )
    _populate_postgres_core(sharedir, pkglibdir)
    pg_config = bindir / "pg_config"
    _write_pg_config(pg_config, bindir, sharedir, pkglibdir)
    artifact_dir = case_root / "artifacts"
    control = artifact_dir / "bioseq.control"
    install_sql = artifact_dir / "bioseq--1.0.sql"
    library = artifact_dir / (
        "bioseq.dylib" if platform.system() == "Darwin" else "bioseq.so"
    )
    _write(control, "default_version = '1.0'\n")
    _write(install_sql, "SELECT 1;\n")
    _write(library, "fixture dso\n")
    artifact_spec = _write_artifact_spec(
        case_root / "extension-artifacts.json",
        [{
            "name": "bioseq",
            "control": control,
            "install_sql": install_sql,
            "library": library,
        }],
    )

    result = subprocess.run(
        [
            cmake,
            "-DPG_CONFIG=" + str(pg_config),
            "-DSTAGE_ROOT=" + str(stage_parent / "production"),
            "-DSTAGE_PARENT=" + str(stage_parent),
            "-DARTIFACT_SPEC=" + str(artifact_spec),
            "-DMANIFEST_OUT=" + str(case_root / "manifest.json"),
            "-DHASH_OUT=" + str(case_root / "hash.txt"),
            "-P",
            str(stage_script),
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=60,
    )
    assert result.returncode != 0, "symlinked stage parent must be rejected"
    assert "STAGE_PARENT" in result.stderr
    assert sentinel.read_text() == "outside stage must not be deleted\n"
    assert not (case_root / "manifest.json").exists()
    assert not (case_root / "hash.txt").exists()


def _verify_static_cmake_contract(stage_script: Path) -> None:
    source = stage_script.read_text()
    required_literals = (
        "bioseq.private_postgres.artifacts",
        "bioseq.private_postgres.stage_manifest",
        "postgresql.materialized_package",
        "schema_revision",
        "unsupported single-artifact input",
        "runtime_closure",
        "required_sentinels",
        "loader_dependencies",
        "BIOSEQ_RUNTIME_EXCLUDED_COORDINATES",
        "postgres.bki",
        "postgresql.conf.sample",
        "pg_hba.conf.sample",
        "pg_ident.conf.sample",
        "information_schema.sql",
        "system_functions.sql",
        "system_views.sql",
        "system_constraints.sql",
        "snowball_create.sql",
        "timezonesets",
        "tsearch_data",
        "plpgsql",
        "dangling runtime link",
        "runtime link escapes canonical",
        "runtime directory link cycle",
        "special or unreadable file",
        "reported/canonical runtime collision",
        'string(HEX "${_loader_staged_relative}"',
        'string(HEX "${_dependency_real}"',
        '"${_loader_staged_sort_hex}!${_loader_source_sort_hex}"',
        "_runtime_overlay_library_coordinates",
        "_runtime_overlay_library_sources",
        "loader/overlay coordinate collision",
    )
    for literal in required_literals:
        assert literal in source, f"CMake stage contract missing {literal!r}"
    forbidden_broad_copies = (
        'file(COPY "${PG_BINDIR}/"',
        'file(COPY "${PG_SHAREDIR}/"',
        'file(COPY "${PG_PKGLIBDIR}/"',
        "FOLLOW_SYMLINK_CHAIN",
        'set(_loader_key "${_loader_staged_relative}|${_dependency_real}")',
        "bioseq_private_postgres_artifacts/v1",
        "stage_manifest/v1",
        "stage_manifest/v3",
        "postgresql-materialized-package/v1",
        "bioseq_artifacts",
        "_vector_mode",
    )
    for literal in forbidden_broad_copies:
        assert literal not in source, (
            f"CMake runtime staging still preserves an open tree: {literal}"
        )
    prefix_pairs = [("a/b", "/source/a"), ("a", "/source/z")]
    assert sorted(prefix_pairs, key=lambda pair: _loader_pair_sort_key(*pair)) == [
        ("a", "/source/z"),
        ("a/b", "/source/a"),
    ]


def _run_runtime_fault_case(
    cmake: str,
    stage_script: Path,
    scratch: Path,
    fault: str,
    expected_diagnostic: str,
) -> None:
    case_root = scratch / f"runtime-fault-{fault}"
    brew = case_root / "source" / "homebrew"
    prefix = brew / "Cellar" / "postgresql@17" / "17.fixture"
    bindir = prefix / "bin"
    canonical_sharedir = prefix / "share" / "postgresql"
    canonical_pkglibdir = prefix / "lib" / "postgresql"
    sharedir = brew / "share" / "postgresql@17"
    pkglibdir = brew / "lib" / "postgresql@17"

    for tool in TOOLS:
        _write(
            bindir / tool,
            "#!/bin/sh\nprintf 'fixture-postgres 17\\n'\n",
            executable=True,
        )
    _populate_postgres_core(canonical_sharedir, canonical_pkglibdir)
    _create_homebrew_facade(canonical_sharedir, sharedir)
    _create_homebrew_facade(canonical_pkglibdir, pkglibdir)

    fault_path = canonical_sharedir / "000-runtime-fault"
    if fault == "dangling":
        _write(fault_path, "canonical package file\n")
        reported_fault = sharedir / fault_path.name
        missing_target = canonical_sharedir / "missing-runtime-target"
        reported_fault.symlink_to(
            os.path.relpath(missing_target, reported_fault.parent)
        )
    elif fault == "escape":
        outside = _write(case_root / "outside-package.txt", "outside\n")
        _symlink_to(outside, fault_path)
    elif fault == "cycle":
        fault_path.symlink_to(".", target_is_directory=True)
    elif fault == "special":
        os.mkfifo(fault_path)
    elif fault == "collision":
        _write(fault_path, "canonical package file\n")
        _write(sharedir / fault_path.name, "different facade file\n")
    else:
        raise AssertionError(f"unknown runtime fault {fault}")

    pg_config = bindir / "pg_config"
    _write_pg_config(pg_config, bindir, sharedir, pkglibdir)
    artifacts = case_root / "artifacts"
    control = _write(artifacts / "bioseq.control", "default_version = '1.0'\n")
    install_sql = _write(artifacts / "bioseq--1.0.sql", "SELECT 1;\n")
    library = _write(
        artifacts / ("bioseq.dylib" if platform.system() == "Darwin"
                     else "bioseq.so"),
        "fixture module\n",
    )
    artifact_spec = _write_artifact_spec(
        case_root / "extension-artifacts.json",
        [{
            "name": "bioseq",
            "control": control,
            "install_sql": install_sql,
            "library": library,
        }],
    )
    manifest_path = case_root / "manifest.json"
    hash_path = case_root / "hash.txt"
    # A failing restage must invalidate receipts from an earlier successful
    # publication rather than leaving them beside a partial replacement tree.
    _write(manifest_path, '{"schema":"stale"}\n')
    _write(hash_path, "stale\n")
    result = subprocess.run(
        [
            cmake,
            "-DPG_CONFIG=" + str(pg_config),
            "-DSTAGE_ROOT=" + str(case_root / "stage"),
            "-DSTAGE_PARENT=" + str(case_root),
            "-DARTIFACT_SPEC=" + str(artifact_spec),
            "-DMANIFEST_OUT=" + str(manifest_path),
            "-DHASH_OUT=" + str(hash_path),
            "-P",
            str(stage_script),
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=60,
    )
    assert result.returncode != 0, f"{fault} runtime fault must be rejected"
    diagnostic = result.stdout + result.stderr
    assert expected_diagnostic in diagnostic, diagnostic
    assert not manifest_path.exists()
    assert not hash_path.exists()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default=shutil.which("cmake"))
    parser.add_argument("--cc", default=shutil.which("cc") or shutil.which("clang"))
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    if not args.cmake or not args.cc:
        parser.error("both cmake and a C compiler are required")
    scratch = Path(args.scratch_root).expanduser().resolve()
    shutil.rmtree(scratch, ignore_errors=True)
    scratch.mkdir(parents=True)
    stage_script = (
        Path(__file__).resolve().parents[2] / "stage_private_postgres.cmake"
    )
    failures = 0
    total = 0
    try:
        total += 1
        _verify_static_cmake_contract(stage_script)
        print("ok - static-cmake-contract")
    except Exception as error:  # noqa: BLE001
        failures += 1
        print(
            "not ok - static-cmake-contract: "
            f"{type(error).__name__}: {error}"
        )
    try:
        total += 1
        _run_artifact_spec_contract_cases(
            args.cmake, stage_script, scratch
        )
        print("ok - artifact-spec-contract")
    except Exception as error:  # noqa: BLE001
        failures += 1
        print(
            "not ok - artifact-spec-contract: "
            f"{type(error).__name__}: {error}"
        )
    try:
        total += 1
        _run_symlink_parent_guard_case(args.cmake, stage_script, scratch)
        print("ok - symlink-parent-guard")
    except Exception as error:  # noqa: BLE001
        failures += 1
        print(
            "not ok - symlink-parent-guard: "
            f"{type(error).__name__}: {error}"
        )
    fault_cases = {
        "dangling": "dangling runtime link",
        "escape": "runtime link escapes canonical",
        "cycle": "runtime directory link cycle",
        "special": "special or unreadable file",
        "collision": "reported/canonical runtime collision",
    }
    for fault, diagnostic in fault_cases.items():
        try:
            total += 1
            _run_runtime_fault_case(
                args.cmake,
                stage_script,
                scratch,
                fault,
                diagnostic,
            )
            print(f"ok - runtime-fault-{fault}")
        except Exception as error:  # noqa: BLE001
            failures += 1
            print(
                f"not ok - runtime-fault-{fault}: "
                f"{type(error).__name__}: {error}"
            )
    for multiple_artifacts in (False, True):
        for layout in ("nested-runtime", "split-homebrew"):
            artifact_shape = (
                "multiple-artifacts"
                if multiple_artifacts
                else "one-artifact"
            )
            case_name = f"{layout}-{artifact_shape}"
            try:
                total += 1
                _run_layout_case(
                    args.cmake,
                    stage_script,
                    scratch,
                    args.cc,
                    layout,
                    multiple_artifacts=multiple_artifacts,
                )
                print(f"ok - {case_name}")
            except Exception as error:  # noqa: BLE001
                failures += 1
                print(
                    f"not ok - {case_name}: "
                    f"{type(error).__name__}: {error}"
                )
    print(
        f"{total - failures}/{total} private-stage relocation tests passed"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
