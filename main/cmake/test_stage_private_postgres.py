#!/usr/bin/env python3
"""Focused relocation tests for stage_private_postgres.cmake.

The fixture is a tiny PostgreSQL-shaped installation whose tools load two
transitive sibling libraries through @loader_path (Darwin) or $ORIGIN (Linux).
After staging, the source installation is hidden before every copied tool is
executed.  This proves that the stage contains the exact runtime closure rather
than accidentally resolving it from the source installation.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


TOOLS = ("postgres", "initdb", "pg_ctl", "createdb", "dropdb", "psql")


def _run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        **kwargs,
    )


def _write(path: Path, text: str, *, executable: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    if executable:
        path.chmod(path.stat().st_mode | 0o755)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _staged_path(stage_root: Path, source_path: Path) -> Path:
    return stage_root / source_path.resolve().relative_to(source_path.anchor)


def _compile_runtime_fixture(cc: str, bindir: Path, runtime_dir: Path,
                             sources: Path) -> tuple[str, str]:
    runtime_dir.mkdir(parents=True, exist_ok=True)
    bindir.mkdir(parents=True, exist_ok=True)
    leaf_source = sources / "leaf.c"
    support_source = sources / "support.c"
    tool_source = sources / "tool.c"
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
    return leaf_name, support_name


def _compile_extension_fixture(cc: str, artifact_dir: Path,
                               module_runtime_dir: Path) -> tuple[Path, str, Path]:
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


def _verify_manifest(manifest_path: Path, hash_path: Path, *, bindir: Path,
                     sharedir: Path, pkglibdir: Path, control: Path,
                     install_sql: Path, library: Path) -> None:
    manifest = json.loads(manifest_path.read_text())
    assert manifest["schema"] == "stage_manifest/v1"
    assert manifest["source"] == {
        "bindir": str(bindir),
        "sharedir": str(sharedir),
        "pkglibdir": str(pkglibdir),
    }
    assert set(manifest["expected_tool_paths"]) == set(TOOLS)
    assert {entry["tool"] for entry in manifest["tool_signatures"]} == set(TOOLS)
    record = (
        f"control:{control.name}:{_sha256(control)}\n"
        f"install_sql:{install_sql.name}:{_sha256(install_sql)}\n"
        f"library:{library.name}:{_sha256(library)}\n"
    )
    expected = hashlib.sha256(record.encode()).hexdigest()
    assert manifest["bioseq_artifacts"]["aggregate_sha256"] == expected
    assert hash_path.read_text() == expected + "\n"


def _run_layout_case(cmake: str, stage_script: Path, scratch: Path,
                     cc: str, layout: str) -> None:
    case_root = scratch / layout
    source_root = case_root / "source"
    if layout == "nested-runtime":
        prefix = source_root / "Postgres.app" / "Contents" / "Versions" / "17"
        bindir = prefix / "bin"
        runtime_dir = prefix / "lib"
        sharedir = prefix / "share" / "postgresql"
        pkglibdir = runtime_dir / "postgresql"
    elif layout == "split-homebrew":
        brew = source_root / "homebrew"
        prefix = brew / "Cellar" / "postgresql@17" / "17.fixture"
        bindir = prefix / "bin"
        runtime_dir = prefix / "lib"
        sharedir = brew / "share" / "postgresql@17"
        pkglibdir = brew / "lib" / "postgresql@17"
    else:
        raise AssertionError(f"unknown fixture layout {layout}")

    sources = case_root / "sources"
    leaf_name, support_name = _compile_runtime_fixture(
        cc, bindir, runtime_dir, sources)
    shared_extension = sharedir / "extension"
    shared_extension.mkdir(parents=True)
    pkglibdir.mkdir(parents=True)
    _write(shared_extension / "plpgsql.control", "builtin fixture\n")
    _write(shared_extension / "bioseq--globally-installed.sql", "must disappear\n")
    _write(pkglibdir / "plpgsql.fixture", "builtin fixture\n")
    _write(pkglibdir / "bioseq_cache.fixture", "must disappear\n")
    _write(runtime_dir / "libunrelated.fixture", "must not be broadly copied\n")

    pg_config = bindir / "pg_config"
    _write_pg_config(pg_config, bindir, sharedir, pkglibdir)
    artifact_dir = case_root / "artifacts"
    artifact_dir.mkdir(parents=True)
    control = artifact_dir / "bioseq.control"
    install_sql = artifact_dir / "bioseq--1.0.sql"
    _write(control, "default_version = '1.0'\n")
    _write(install_sql, "SELECT 1;\n")
    module_runtime_dir = pkglibdir.parent
    library, module_dependency_name, module_probe = _compile_extension_fixture(
        cc, artifact_dir, module_runtime_dir)

    stage_root = case_root / "stage"
    manifest = case_root / "stage-manifest.json"
    hash_path = case_root / "staged-extension.sha256"
    result = _run([
        cmake,
        "-DPG_CONFIG=" + str(pg_config),
        "-DSTAGE_ROOT=" + str(stage_root),
        "-DCONTROL_FILE=" + str(control),
        "-DINSTALL_SQL=" + str(install_sql),
        "-DLIBRARY_FILE=" + str(library),
        "-DEXTENSION_VERSION=1.0",
        "-DMANIFEST_OUT=" + str(manifest),
        "-DHASH_OUT=" + str(hash_path),
        "-P", str(stage_script),
    ])
    assert "stage_private_postgres: staged PostgreSQL 17.fixture" in result.stdout

    staged_bindir = _staged_path(stage_root, bindir)
    staged_runtime = _staged_path(stage_root, runtime_dir)
    staged_module_runtime = _staged_path(stage_root, module_runtime_dir)
    staged_share = _staged_path(stage_root, sharedir)
    staged_pkglib = _staged_path(stage_root, pkglibdir)
    assert (staged_runtime / leaf_name).exists()
    assert (staged_runtime / support_name).exists()
    assert (staged_module_runtime / module_dependency_name).exists()
    assert not (staged_runtime / "libunrelated.fixture").exists()
    assert not any((staged_share / "extension").glob("bioseq--globally-installed*"))
    assert not any(staged_pkglib.glob("bioseq_cache*"))
    assert (staged_share / "extension" / control.name).read_bytes() == control.read_bytes()
    assert (staged_share / "extension" / install_sql.name).read_bytes() == install_sql.read_bytes()
    assert (staged_pkglib / library.name).read_bytes() == library.read_bytes()
    assert (staged_pkglib / "plpgsql.fixture").is_file()

    _verify_manifest(
        manifest, hash_path, bindir=bindir, sharedir=sharedir,
        pkglibdir=pkglibdir, control=control, install_sql=install_sql,
        library=library,
    )

    hidden_source = case_root / "source-hidden"
    source_root.rename(hidden_source)
    for tool in TOOLS:
        completed = _run([str(staged_bindir / tool), "--version"])
        assert completed.stdout == "fixture-postgres 42\n"
    loaded = _run([str(module_probe), str(staged_pkglib / library.name)])
    assert loaded.stdout == "fixture-module 84\n"


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
    stage_script = Path(__file__).resolve().parent / "stage_private_postgres.cmake"
    failures = 0
    for layout in ("nested-runtime", "split-homebrew"):
        try:
            _run_layout_case(args.cmake, stage_script, scratch, args.cc, layout)
            print(f"ok - {layout}")
        except Exception as error:  # noqa: BLE001
            failures += 1
            print(f"not ok - {layout}: {type(error).__name__}: {error}")
    print(f"{2 - failures}/2 private-stage relocation tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
