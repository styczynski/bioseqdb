#!/usr/bin/env python3
"""Identify installed PostgreSQL headers independently of archive timestamps.

Fresh distro containers can install a newer package whose header mtimes still
predate the persistent CMake/codegen cache. Put this identity on ServerHeaders
consumers' compile commands so dependency discovery sees the changed SDK. This
does not reject artifacts, clear caches, or alter any extension functionality.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
import sys


def header_identity(roots: list[Path]) -> str:
    headers = sorted({path.resolve() for root in roots
                      for path in root.rglob("*.h") if path.is_file()})
    if not headers:
        raise RuntimeError("selected PostgreSQL include directories contain no headers")
    digest = hashlib.sha256()
    for path in headers:
        digest.update(str(path).encode() + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def main() -> None:
    pg_config = sys.argv[1]
    roots = [Path(subprocess.check_output([pg_config, option], text=True).strip())
             for option in ("--includedir", "--includedir-server")]
    if any(not root.is_absolute() or not root.is_dir() for root in roots):
        raise RuntimeError("pg_config returned an invalid header directory")
    print(header_identity(roots))


if __name__ == "__main__":
    main()
