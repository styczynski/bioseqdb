#!/usr/bin/env python3
"""Validate that pinned C++ runtime archives produce a portable native ELF DSO."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile


FORBIDDEN_RUNTIME_SONAMES = (
    "libc++.so",
    "libc++abi.so",
    "libunwind.so",
    "libstdc++.so",
    "libgcc_s.so",
)


def run(command: list[str]) -> str:
    completed = subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout


def prove(
    cxx: Path,
    libcxx: Path,
    libcxxabi: Path,
    libunwind: Path,
    architecture: str,
) -> None:
    inputs = (cxx, libcxx, libcxxabi, libunwind)
    if not all(path.is_absolute() and path.is_file() for path in inputs):
        raise RuntimeError("compiler and runtime inputs must be existing absolute files")

    source = r"""
#include <stdexcept>
#include <string>
extern "C" __attribute__((visibility("default"))) int bioseq_runtime_probe() {
  try {
    throw std::runtime_error(std::string("bioseq-static-runtime"));
  } catch (const std::exception& error) {
    return error.what()[0] == 'b' ? 0 : 1;
  }
}
"""
    with tempfile.TemporaryDirectory(prefix="bioseq-runtime-proof-") as raw_directory:
        directory = Path(raw_directory)
        source_path = directory / "runtime_probe.cpp"
        output_path = directory / "runtime_probe.so"
        source_path.write_text(source, encoding="utf-8")
        run(
            [
                str(cxx),
                "-std=c++20",
                "-O0",
                "-fPIC",
                "-shared",
                "-stdlib=libc++",
                "-rtlib=compiler-rt",
                "-unwindlib=none",
                "-nostdlib++",
                str(source_path),
                "-Wl,--start-group",
                str(libcxx),
                str(libcxxabi),
                str(libunwind),
                "-Wl,--end-group",
                "-Wl,--exclude-libs,libc++.a:libc++abi.a:libunwind.a",
                "-Wl,--disable-new-dtags",
                "-o",
                str(output_path),
            ]
        )
        dynamic = run(["readelf", "-d", str(output_path)])
        for soname in FORBIDDEN_RUNTIME_SONAMES:
            if soname in dynamic:
                raise RuntimeError(f"static-runtime proof retained dynamic {soname}")
        if "(RPATH)" in dynamic or "(RUNPATH)" in dynamic:
            raise RuntimeError("static-runtime proof emitted RPATH/RUNPATH")

        header = run(["readelf", "-h", str(output_path)])
        machine = "AArch64" if architecture == "arm64" else "X86-64"
        if machine not in header:
            raise RuntimeError(
                f"static-runtime proof has wrong ELF architecture; expected {machine}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", type=Path, required=True)
    parser.add_argument("--libcxx", type=Path, required=True)
    parser.add_argument("--libcxxabi", type=Path, required=True)
    parser.add_argument("--libunwind", type=Path, required=True)
    parser.add_argument("--architecture", choices=("arm64", "x64"), required=True)
    args = parser.parse_args()
    prove(
        args.cxx,
        args.libcxx,
        args.libcxxabi,
        args.libunwind,
        args.architecture,
    )
    print("pinned static libc++/libc++abi/libunwind linkage proof passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
