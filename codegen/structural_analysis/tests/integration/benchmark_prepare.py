#!/usr/bin/env python3
"""Time cold and warm PREPARE on a reproducible shared-header module graph.

This measures generator preparation, not application compilation or runtime.
Use separate output directories when comparing generator/toolchain versions.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import resource
import statistics
import subprocess
import sys
import time


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codegen", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--providers", type=int, default=32)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.providers <= 256 or not 1 <= args.repeats <= 10:
        parser.error("providers must be 1..256 and repeats 1..10")
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if (root / "raw.json").exists():
        parser.error("output directory already contains a benchmark; choose a fresh path")
    toolchain = args.toolchain.resolve()
    tools = {
        "clang-binary": toolchain / "bin/clang++",
        "clang-scan-deps": toolchain / "bin/clang-scan-deps",
        "llvm-config": toolchain / "bin/llvm-config",
    }
    inventories = sorted((toolchain / "lib").rglob("libc++.modules.json"))
    if len(inventories) != 1:
        parser.error("toolchain must have exactly one libc++.modules.json")
    tools["compiler-module-inventory"] = inventories[0]
    headers = "module;\n#include <vector>\n#include <string>\n#include <algorithm>\n"
    sources = []
    for index in range(args.providers):
        source = root / f"provider_{index}.cppm"
        source.write_text(headers + f"export module fixture.provider{index};\n"
                          + f"export int value_{index}() {{ return {index}; }}\n")
        sources.append(source)
    source = root / "root.cppm"
    source.write_text("export module fixture.root;\n" + "".join(
        f"import fixture.provider{index};\n" for index in range(args.providers)))
    sources.append(source)
    database = [{"directory": str(root), "file": str(path), "arguments": [
        str(tools["clang-binary"]), "-std=c++26", "-stdlib=libc++",
        "-x", "c++-module", str(path),
    ]} for path in sources]
    (root / "raw.json").write_text(json.dumps(database, indent=2) + "\n")
    command = [str(args.codegen.resolve()), "sql", "prepare", "--authority", "support",
               "--profile", "production", "--source", str(source),
               "--raw-compile-database", str(root / "raw.json"),
               "--compile-database-output", str(root / "owned.json"),
               "--pcm-root", str(root / "pcm"), "--pcm-manifest", str(root / "manifest.json"),
               "--depfile", str(root / "prepare.d"), "--stamp", str(root / "prepare.stamp")]
    hashes = {"clang-binary": "clang", "compiler-module-inventory": "compiler-module-inventory"}
    for name, path in tools.items():
        command += [f"--{name}", str(path)]
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        command += [f"--{hashes.get(name, name)}-sha256", digest]
    times = []
    for iteration in range(args.repeats + 1):
        start = time.perf_counter()
        result = subprocess.run(command, capture_output=True, text=True, timeout=600)
        elapsed = time.perf_counter() - start
        (root / f"prepare-{iteration}.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f"PREPARE failed; see {root}/prepare-{iteration}.log")
        times.append(elapsed)
        print(f"{'cold' if iteration == 0 else 'warm'}: {elapsed:.3f}s", flush=True)
    report = {"codegen": str(args.codegen.resolve()), "toolchain": str(toolchain),
              "providers": args.providers, "cold_seconds": times[0],
              "warm_seconds": times[1:], "warm_median_seconds": statistics.median(times[1:]),
              # Maximum single-child RSS over the whole fixture, not a sum of processes.
              "peak_process_rss_kib": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
              / (1024 if sys.platform == "darwin" else 1)}
    (root / "timings.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
