#!/usr/bin/env python3
"""Resolve an exact-major pg_config without accepting CMake's >= fallback."""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def postgres_major(pg_config: Path) -> int:
    result = subprocess.run(
        [str(pg_config), "--version"],
        check=True,
        capture_output=True,
        text=True,
    )
    match = re.search(r"PostgreSQL\s+(\d+)(?:\.\d+)?", result.stdout)
    if not match:
        raise ValueError(f"cannot parse PostgreSQL version from {result.stdout!r}")
    return int(match.group(1))


def candidates(requested_major: int) -> list[Path]:
    values: list[Path] = []
    explicit = os.environ.get("BIOSEQ_PG_CONFIG", "")
    if explicit:
        values.append(Path(explicit))
    values.extend(
        [
            Path(f"/usr/lib/postgresql/{requested_major}/bin/pg_config"),
            Path(f"/usr/pgsql-{requested_major}/bin/pg_config"),
            Path(f"/opt/pgsql-{requested_major}/bin/pg_config"),
            Path(f"/usr/lib/postgresql{requested_major}/bin/pg_config"),
            Path(f"/usr/lib64/postgresql{requested_major}/bin/pg_config"),
            Path(f"/usr/bin/pg_config-{requested_major}"),
        ]
    )
    default = shutil.which("pg_config")
    if default:
        values.append(Path(default))
    deduplicated: list[Path] = []
    seen: set[Path] = set()
    for value in values:
        try:
            canonical = value.resolve(strict=False)
        except OSError:
            canonical = value
        if canonical not in seen:
            seen.add(canonical)
            deduplicated.append(value)
    return deduplicated


def resolve(requested_major: int) -> Path:
    wrong_versions: list[str] = []
    for candidate in candidates(requested_major):
        if not candidate.is_file() or not os.access(candidate, os.X_OK):
            continue
        try:
            actual_major = postgres_major(candidate)
        except (OSError, subprocess.SubprocessError, ValueError) as error:
            wrong_versions.append(f"{candidate}: {error}")
            continue
        if actual_major == requested_major:
            return candidate.resolve()
        wrong_versions.append(f"{candidate}: PostgreSQL {actual_major}")
    detail = "; ".join(wrong_versions) if wrong_versions else "no executable candidates"
    raise RuntimeError(
        f"no pg_config for exact PostgreSQL major {requested_major}; {detail}"
    )


def main() -> int:
    raw_major = os.environ.get("BIOSEQ_POSTGRES_MAJOR", "")
    if not re.fullmatch(r"\d+", raw_major):
        print("BIOSEQ_POSTGRES_MAJOR must be a decimal integer", file=sys.stderr)
        return 64
    try:
        print(resolve(int(raw_major)))
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
