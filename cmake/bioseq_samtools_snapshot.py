#!/usr/bin/env python3
"""Verify and stage the pinned samtools source for the managed CMake target.

No compiler, configure script or benchmark is executed here. Source snapshots
are content-addressed and never repaired in place. The separate work tree is
allowed to contain configure/make products, but every upstream input must still
match the verified snapshot whenever CMake configures the consuming target.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import tempfile


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode("utf-8")


def digest(path: Path) -> str:
    before = path.stat()
    result = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            result.update(block)
    after = path.stat()
    fields = lambda item: (item.st_dev, item.st_ino, item.st_size, item.st_mtime_ns)
    if fields(before) != fields(after):
        raise ValueError(f"samtools input changed while hashing: {path}")
    return result.hexdigest()


def _object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate source identity key: {key}")
        result[key] = value
    return result


def _read_json(path: Path) -> dict:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"source identity must be a regular file: {path}")
    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_object)
    if not isinstance(value, dict):
        raise ValueError(f"source identity must be an object: {path}")
    return value


def _relative(name: str) -> Path:
    if not isinstance(name, str) or not name or "\\" in name:
        raise ValueError(f"invalid source manifest path: {name!r}")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in name.split("/")):
        raise ValueError(f"invalid source manifest path: {name!r}")
    return Path(*path.parts)


def _regular(root: Path, relative: str) -> Path:
    path = root
    for part in _relative(relative).parts:
        path /= part
        if path.is_symlink():
            raise ValueError(f"symlink in pinned source: {path}")
    if not path.is_file():
        raise ValueError(f"missing pinned source: {path}")
    return path


def source_identity(source: Path) -> tuple[dict, dict[str, str], str]:
    lock = _read_json(source / "source_lock.json")
    manifest = _read_json(source / "source_manifest.json")
    files = manifest.get("sha256_files")
    if (lock.get("schema_version") != 1 or not isinstance(files, dict)
            or not files or lock.get("file_manifest") != "source_manifest.json"
            or lock.get("repository_added_files") != ["source_lock.json", "source_manifest.json"]
            or lock.get("modifications_to_upstream_files") != []
            or lock.get("upstream_file_count") != len(files)):
        raise ValueError("unsupported or inconsistent samtools source lock/manifest")
    total = 0
    for name, expected in files.items():
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError(f"invalid pinned digest: {name}")
        path = _regular(source, name)
        if digest(path) != expected:
            raise ValueError(f"pinned source digest mismatch: {name}")
        total += path.stat().st_size
    if total != lock.get("upstream_content_bytes"):
        raise ValueError("samtools source byte count differs from lock")
    expected_names = set(files) | {"source_lock.json", "source_manifest.json"}
    observed = set()
    for directory, children, names in os.walk(source, followlinks=False):
        for child in children:
            if (Path(directory) / child).is_symlink():
                raise ValueError("symlink directory in pinned source")
        for name in names:
            path = Path(directory) / name
            if path.is_symlink() or not path.is_file():
                raise ValueError(f"nonregular pinned source: {path}")
            observed.add(path.relative_to(source).as_posix())
    if observed != expected_names:
        raise ValueError("samtools source inventory differs from manifest")
    identity = hashlib.sha256(canonical({"lock": lock, "manifest": manifest})).hexdigest()
    return lock, files, identity


def _verify_copy(path: Path, files: dict[str, str]) -> None:
    for name, expected in files.items():
        if digest(_regular(path, name)) != expected:
            raise ValueError(f"immutable samtools input changed: {path / name}")


def _copy_once(source: Path, destination: Path, files: dict[str, str], *, strict: bool) -> None:
    if destination.is_symlink():
        raise ValueError(f"snapshot destination must not be a symlink: {destination}")
    if not destination.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = Path(tempfile.mkdtemp(prefix=".samtools-", dir=destination.parent))
        try:
            shutil.copytree(source, temporary / "tree", symlinks=True)
            _verify_copy(temporary / "tree", files)
            (temporary / "tree").rename(destination)
        finally:
            shutil.rmtree(temporary)
    _verify_copy(destination, files)
    if strict:
        # Also reject added files, altered pin files and symlink directories.
        _, _, copied_identity = source_identity(destination)
        _, _, original_identity = source_identity(source)
        if copied_identity != original_identity:
            raise ValueError("immutable samtools snapshot identity differs")


def _write_if_different(path: Path, data: bytes) -> None:
    if path.is_symlink():
        raise ValueError(f"generated identity must not be a symlink: {path}")
    if path.exists() and path.read_bytes() == data:
        return
    temporary = path.with_name(path.name + ".tmp")
    if temporary.exists() or temporary.is_symlink():
        raise ValueError(f"unexpected identity temporary file: {temporary}")
    try:
        with temporary.open("xb") as handle:
            handle.write(data)
        temporary.replace(path)
    finally:
        if temporary.exists():
            temporary.unlink()


def prepare(source: Path, root: Path, configuration: dict,
            *, expected_build_id: str | None = None) -> dict:
    """Create/verify one source snapshot and one configuration-specific work tree."""
    source = source.resolve(strict=True)
    root.mkdir(parents=True, exist_ok=True)
    root = root.resolve(strict=True)
    lock, files, source_id = source_identity(source)
    build_id = hashlib.sha256(canonical({
        "source_sha256": source_id, "configuration": configuration,
        "snapshot_builder_sha256": digest(Path(__file__)),
    })).hexdigest()
    if expected_build_id is not None and build_id != expected_build_id:
        raise ValueError("selected samtools build identity changed; reconfigure before building")
    snapshot = root / "sources" / source_id
    work = root / "builds" / build_id / "work"
    external = work.parent / "external"
    if not work.exists() and (external.exists() or external.is_symlink()):
        raise ValueError("missing samtools work tree has surviving ExternalProject state")
    for directory in (snapshot.parent, work.parent):
        current = root
        for part in directory.relative_to(root).parts:
            current /= part
            if current.is_symlink() or (current.exists() and not current.is_dir()):
                raise ValueError(f"snapshot parent must be a real directory: {current}")
            current.mkdir(exist_ok=True)
    _copy_once(source, snapshot, files, strict=True)
    _copy_once(snapshot, work, files, strict=False)
    identity = {
        "schema_version": 1, "source_sha256": source_id, "build_sha256": build_id,
        "configuration": configuration,
        "archive": lock.get("archive"), "samtools": lock.get("samtools"),
        "htslib": lock.get("htslib"),
        "executable": "bench/tool_samtools/samtools",
        "compiled_source": str(work), "source_snapshot": str(snapshot),
    }
    _write_if_different(root / "runtime_identity.json", canonical(identity) + b"\n")
    return identity


def configuration(args: argparse.Namespace) -> tuple[dict, list[Path]]:
    includes = [Path(value).resolve(strict=True) for value in args.includes.split(";") if value]
    libdir = Path(args.libdir).resolve(strict=True)
    cc, make = Path(args.cc).resolve(strict=True), Path(args.make).resolve(strict=True)
    dependencies = {cc, make, Path(__file__).resolve()}
    for header in ("zlib.h", "zconf.h", "libdeflate.h"):
        matches = [directory / header for directory in includes if (directory / header).is_file()]
        if not matches:
            raise ValueError(f"pinned baseline dependency header not found: {header}")
        dependencies.update(matches)
    for pattern in ("libz.*", "libdeflate.*"):
        matches = list(libdir.glob(pattern))
        if not matches:
            raise ValueError(f"pinned baseline dependency library not found: {pattern}")
        dependencies.update(path for path in matches if path.is_file())
    sysroot = Path(args.sysroot)
    for name in ("SDKSettings.json", "SDKSettings.plist"):
        if (sysroot / name).is_file():
            dependencies.add(sysroot / name)
    evidence = [
        {"path": str(path), "realpath": str(path.resolve(strict=True)), "sha256": digest(path)}
        for path in sorted(dependencies)
    ]
    value = {
        "cc": str(cc), "make": str(make), "cflags": args.cflags,
        "cppflags": " ".join(shlex.quote("-I" + str(path)) for path in includes),
        "ldflags": shlex.quote("-L" + str(libdir)),
        "system": args.system, "sysroot": args.sysroot,
        "configure": args.configure_arg,
        "dependencies": evidence,
    }
    return value, sorted(dependencies)


def _cmake(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace(";", "\\;").replace("$", "\\$") + '"'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "root", "cc", "make", "includes", "libdir", "system", "sysroot", "cflags"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--configure-arg", action="append", required=True)
    parser.add_argument("--expected-build-id")
    args = parser.parse_args()
    config, dependencies = configuration(args)
    source, root = Path(args.source).resolve(strict=True), Path(args.root)
    identity = prepare(source, root, config, expected_build_id=args.expected_build_id)
    variables = {
        "BIOSEQ_SAMTOOLS_BUILD_ID": identity["build_sha256"],
        "BIOSEQ_SAMTOOLS_BUILD_DIR": identity["compiled_source"],
        "BIOSEQ_SAMTOOLS_CPPFLAGS": config["cppflags"],
        "BIOSEQ_SAMTOOLS_LDFLAGS": config["ldflags"],
    }
    lines = [f"set({key} {_cmake(value)})" for key, value in variables.items()]
    lines.append("set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS")
    lines.extend("  " + _cmake(str(path)) for path in [*dependencies, *sorted(source.rglob("*"))] if path.is_file())
    lines.append(")")
    _write_if_different(root / "selected.cmake", ("\n".join(lines) + "\n").encode("utf-8"))


if __name__ == "__main__":
    main()
