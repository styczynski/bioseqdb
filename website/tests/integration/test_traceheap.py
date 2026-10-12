#!/usr/bin/env python3
"""Compile the authored PostgreSQL 17 TAM example and run it in a private cluster.

Uses a disposable copy of the already staged PostgreSQL prefix. PGXS installs
only under DESTDIR; neither the website SQL cluster nor the system install is
modified. The ordinary website SQL model does not acquire the demo's objects.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
DEMO = ROOT / "website/static/examples/traceheap"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pg-config", required=True, type=Path)
    parser.add_argument("--stage-root", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    config = args.pg_config.resolve()
    stage = args.stage_root.resolve(strict=True)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)

    def setting(option: str) -> str:
        return subprocess.check_output([str(config), option], text=True, timeout=10).strip()

    version = setting("--version")
    if not version.startswith("PostgreSQL 17."):
        print(f"traceheap: requires PostgreSQL 17; configured {version}, compile check skipped")
        return
    bindir = Path(setting("--bindir"))
    if not bindir.is_absolute():
        raise ValueError("pg_config bindir must be absolute")
    # Retain logs outside the temporary tree for a failed build's diagnostics.
    with (output / "build-and-smoke.log").open("w") as log:
        def run(command: list[str], timeout: int = 180) -> None:
            log.write("Running: " + repr(command) + "\n")
            log.flush()
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=timeout, env=os.environ.copy())
            if result.returncode:
                raise RuntimeError(f"traceheap validation failed; see {log.name}")

        with tempfile.TemporaryDirectory(prefix="traceheap-", dir=output) as temporary:
            work = Path(temporary)
            demo = work / "source"
            prefix = work / "postgres-prefix"
            shutil.copytree(DEMO, demo)
            # Dereference symlinks so DESTDIR cannot write through a copied link
            # into the original PostgreSQL installation.
            shutil.copytree(stage, prefix, symlinks=False)
            private_bin = prefix / bindir.relative_to("/")
            for option in ("--pkglibdir", "--sharedir", "--bindir"):
                destination = prefix / Path(setting(option)).relative_to("/")
                if not destination.resolve().is_relative_to(prefix):
                    raise ValueError("PostgreSQL install destination escapes private prefix")
            run(["make", "-C", str(demo), f"PG_CONFIG={config}", "-j2"])
            run(["make", "-C", str(demo), f"PG_CONFIG={config}",
                 f"DESTDIR={prefix}", "install"])
            run([sys.executable, str(ROOT / "cmake/run_private_postgres.py"),
                 "--pg-bin-dir", str(private_bin), "--database", "traceheap_tutorial",
                 "--log-dir", str(output / "cluster"), "--disable-prepared-transactions",
                 "--", str(private_bin / "psql"), "-X", "-v", "ON_ERROR_STOP=1",
                 "-f", str(demo / "test.sql")])
    print(f"traceheap: PASS ({version}, C++ build and private PostgreSQL smoke checks)")


if __name__ == "__main__":
    main()
