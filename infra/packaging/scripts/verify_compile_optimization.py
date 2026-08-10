#!/usr/bin/env python3
"""Fail unless every package compile command uses only compiler -O0."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shlex


def command_arguments(entry: dict[str, object]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(item, str) for item in arguments):
        return list(arguments)
    command = entry.get("command")
    if isinstance(command, str):
        return shlex.split(command)
    raise RuntimeError("compile_commands entry has neither string arguments nor command")


def verify(path: Path) -> int:
    try:
        entries = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read compile database {path}: {error}") from error
    if not isinstance(entries, list) or not entries:
        raise RuntimeError(f"compile database is empty: {path}")

    checked = 0
    for entry in entries:
        if not isinstance(entry, dict):
            raise RuntimeError(f"invalid compile command in {path}")
        arguments = command_arguments(entry)
        forbidden = sorted(
            {argument for argument in arguments if argument.startswith("-O") and argument != "-O0"}
        )
        if forbidden:
            raise RuntimeError(
                f"package compile command contains forbidden optimization {forbidden}: "
                f"{entry.get('file', '<unknown>')}"
            )
        if "-O0" not in arguments:
            raise RuntimeError(
                f"package compile command omits explicit -O0: {entry.get('file', '<unknown>')}"
            )
        forbidden_lto = sorted(
            {
                argument
                for argument in arguments
                if argument == "-flto" or argument.startswith("-flto=")
            }
        )
        if forbidden_lto:
            raise RuntimeError(
                f"package compile command contains forbidden LTO {forbidden_lto}: "
                f"{entry.get('file', '<unknown>')}"
            )
        checked += 1
    return checked


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compile_databases", nargs="+", type=Path)
    args = parser.parse_args()
    checked = sum(verify(path) for path in args.compile_databases)
    print(f"verified {checked} package compile commands at explicit -O0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
