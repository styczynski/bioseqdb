#!/usr/bin/env python3
"""Reject production compile databases that do not use only ``-O3``."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shlex


SOURCE_LANGUAGES = {
    ".c": "C",
    ".cc": "CXX",
    ".cpp": "CXX",
    ".cppm": "CXX",
    ".cxx": "CXX",
    ".ixx": "CXX",
}


def command_arguments(entry: dict[str, object]) -> list[str]:
    """Return one compile-database entry's tokenized command."""
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(
            isinstance(argument, str) for argument in arguments):
        return list(arguments)
    command = entry.get("command")
    if isinstance(command, str):
        return shlex.split(command)
    raise RuntimeError(
        "compile command has neither string arguments nor command")


def source_language(entry: dict[str, object]) -> str | None:
    """Classify the C or C++ source owned by a compile-database entry."""
    source = entry.get("file")
    if not isinstance(source, str) or not source:
        raise RuntimeError("compile command has no source file")
    return SOURCE_LANGUAGES.get(Path(source).suffix.lower())


def verify(path: Path) -> dict[str, int]:
    """Verify every C/C++ command and return checked counts by language."""
    try:
        entries = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(
            f"cannot read compile database {path}: {error}") from error
    if not isinstance(entries, list) or not entries:
        raise RuntimeError(f"compile database is empty: {path}")

    counts = {"C": 0, "CXX": 0}
    for entry in entries:
        if not isinstance(entry, dict):
            raise RuntimeError(f"invalid compile command in {path}")
        language = source_language(entry)
        if language is None:
            continue
        arguments = command_arguments(entry)
        optimization_arguments = sorted({
            argument for argument in arguments if argument.startswith("-O")
        })
        if optimization_arguments != ["-O3"]:
            raise RuntimeError(
                "production/benchmark C and C++ compile commands must contain "
                "explicit -O3 and no competing optimization flags; found "
                f"{optimization_arguments}: {entry['file']}")
        counts[language] += 1

    if sum(counts.values()) == 0:
        raise RuntimeError(
            f"compile database contains no C or C++ commands: {path}")
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compile_databases", nargs="+", type=Path)
    arguments = parser.parse_args()
    totals = {"C": 0, "CXX": 0}
    for path in arguments.compile_databases:
        counts = verify(path)
        for language in totals:
            totals[language] += counts[language]
    print(
        "verified production/benchmark compile commands at explicit -O3: "
        f"C={totals['C']} CXX={totals['CXX']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
