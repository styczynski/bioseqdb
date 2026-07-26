#!/usr/bin/env python3
"""DOC0388 SQL code-generation performance qualification.

The ordinary benchmark consumes only a reviewed, passing qualification file.
This tool deliberately separates:

* measurement: raw samples from dedicated build trees;
* review: a human-approved baseline and explicit numeric limits;
* qualification: current measurements checked against that baseline.

No default baseline is embedded here.  In particular, post-cutover recovery
data cannot silently impersonate the missing Stage-0 record.

Trust model: this is a reproducibility and accidental-misuse guardrail, not a
privilege boundary against an arbitrary malicious process running as the same
user.  Qualification requires an exact MCP-published baseline receipt, ledger,
worker/session/state join, and one-use live callback so an ordinary direct CLI
invocation cannot become authoritative merely by supplying plausible hashes.
Repository or process compromise by that same user is outside DOC0388 §14.8;
such a threat model would require an external signer or permission-separated
service rather than another workspace secret.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, NoReturn, Sequence


SCHEMA_VERSION = 1
MARKER_NAME = ".bioseq-sql-guardrail-dedicated.json"
BUILD_SAMPLE_METRICS = (
    "generator_cold_ns",
    "generator_noop_ns",
    "generator_peak_rss_bytes",
    "compile_postgresql_bioseq_ns",
    "compile_bioseq_bench_e2e_ns",
)
COMPILE_METRICS = (
    "compile_postgresql_bioseq_ns",
    "compile_bioseq_bench_e2e_ns",
)
COMPILE_DELTA_METRICS = (
    "compile_postgresql_bioseq_delta_percent",
    "compile_bioseq_bench_e2e_delta_percent",
)
ARTIFACT_GROUPS = ("sql", "json", "cpp")
MEASURED_RUNTIME_ARTIFACT_NAMES = (
    "generator_executable",
    "production_dso",
    "benchmark_executable",
    "comparator_dso",
)
RUNTIME_ARTIFACT_NAMES = (
    *MEASURED_RUNTIME_ARTIFACT_NAMES,
    "installed_production_dso",
)
ARTIFACT_SIZE_METRICS = tuple(
    f"generated_{group}_bytes" for group in ARTIFACT_GROUPS
)
RUNTIME_SAMPLE_METRICS = (
    "from_array_arity_2_ns",
    "from_array_arity_3_ns",
    "typed_wrapper_generated_to_handwritten_ratio",
    "mmsearch_arity_2_ns",
    "mmsearch_arity_3_ns",
    "row_codec_generated_to_handwritten_ratio",
)
CONTEXT_BASELINE_METRICS = (
    "measurement_normalized_load_1_max",
    "measurement_available_memory_bytes_min",
)
BASELINE_METRICS = (
    *BUILD_SAMPLE_METRICS,
    *ARTIFACT_SIZE_METRICS,
    *RUNTIME_SAMPLE_METRICS,
    *CONTEXT_BASELINE_METRICS,
)
DIRECT_LIMIT_METRICS = (
    "generator_cold_ns",
    "generator_noop_ns",
    "generator_peak_rss_bytes",
    *ARTIFACT_SIZE_METRICS,
    *RUNTIME_SAMPLE_METRICS,
)
BUILD_DIRECT_LIMIT_METRICS = (
    "generator_cold_ns",
    "generator_noop_ns",
    "generator_peak_rss_bytes",
    *ARTIFACT_SIZE_METRICS,
)
ALL_LIMIT_METRICS = (
    *DIRECT_LIMIT_METRICS,
    *COMPILE_DELTA_METRICS,
    "measurement_normalized_load_1_max",
    "measurement_normalized_load_1_delta_max",
    "measurement_available_memory_bytes_min",
    "measurement_available_memory_drop_percent_max",
    "measurement_cpu_governor_states",
)
QUALIFICATION_OBSERVED_METRICS = (
    *BUILD_SAMPLE_METRICS,
    *ARTIFACT_SIZE_METRICS,
    *COMPILE_DELTA_METRICS,
    *CONTEXT_BASELINE_METRICS,
    "measurement_normalized_load_1_delta",
    "measurement_available_memory_drop_percent",
)
BENCHMARK_ALGORITHMS = (
    "qualification_reference",
    "from_array_arity_2",
    "from_array_arity_3",
    "typed_wrapper_generated_vs_handwritten",
    "mmsearch_arity_2",
    "mmsearch_arity_3",
    "row_codec_generated_vs_handwritten",
)
COMPARATOR_ALGORITHMS = (
    "typed_wrapper_generated_vs_handwritten",
    "row_codec_generated_vs_handwritten",
)
EXPECTED_ALGORITHM_ARITIES = {
    "qualification_reference": 0.0,
    "from_array_arity_2": 2.0,
    "from_array_arity_3": 3.0,
    "typed_wrapper_generated_vs_handwritten": 1.0,
    "mmsearch_arity_2": 2.0,
    "mmsearch_arity_3": 3.0,
    "row_codec_generated_vs_handwritten": 0.0,
}
EXPECTED_ROW_CODEC_FIELD_COUNTS = {
    "mmsearch_arity_3": 36.0,
    "row_codec_generated_vs_handwritten": 44.0,
}
# No historical Stage-0 artifact was captured before the DOC0388 cutover.
# Live benchmark consumption therefore rejects every Stage-0-labelled
# qualification until a separately reviewed historical hash is deliberately
# added here. Recovered post-cutover baselines remain explicitly reviewable.
LIVE_STAGE0_BASELINE_SHA256_ALLOWLIST: frozenset[str] = frozenset()
BENCHMARK_METRICS = (
    "wall_clock_ns",
    "query_ns",
    "generated_query_ns",
    "handwritten_query_ns",
    "generated_to_handwritten_ratio",
    "correctness_passed",
    "arity",
    "generated_typed_wrapper",
    "direct_fmgr_wrapper_path",
    "planner_custom_scan_path",
    "generated_row_codec_fields",
    "performance_gate_passed",
    "build_qualification_passed",
    "qualification_baseline_kind",
    "live_measurement_attested",
    "run_nonce",
    "generator_cold_ns",
    "generator_noop_ns",
    "generator_peak_rss_bytes",
    "compile_postgresql_bioseq_ns",
    "compile_bioseq_bench_e2e_ns",
    "compile_postgresql_bioseq_delta_percent",
    "compile_bioseq_bench_e2e_delta_percent",
    "generated_sql_bytes",
    "generated_sql_files",
    "generated_json_bytes",
    "generated_json_files",
    "generated_cpp_bytes",
    "generated_cpp_files",
)
MIN_SAMPLES = 3
ENVIRONMENT_FIELDS = (
    "system",
    "release",
    "machine",
    "processor",
    "cpu_model",
    "logical_cpu_count",
    "physical_core_count",
    "cpu_package_count",
    "page_size_bytes",
    "physical_memory_bytes",
    "python",
    "compiler_version",
    "cmake_version",
    "build_tool_version",
    "postgresql_version",
    "codegen_c_compiler",
    "codegen_c_compiler_version",
    "codegen_cxx_compiler",
    "codegen_cxx_compiler_version",
    "codegen_stdlib_modules_json",
    "codegen_stdlib_modules_json_sha256",
    "toolchain_file_sha256",
    "named_environment",
    "cmake_cache",
    "sha256",
)
SOURCE_FINGERPRINT_PATHS = (
    "main/CMakeLists.txt",
    "main/CMakePresets.json",
    "main/build.sh",
    "main/vcpkg-configuration.json",
    "main/vcpkg.json",
    "main/bench",
    "main/bench_framework",
    "main/bioseq",
    "main/bioseq_caching",
    "main/build_mcp",
    "main/cmake",
    "main/codegen",
    "main/common",
    "main/config",
    "main/main",
    "main/overlays",
    "main/vendor",
)
SOURCE_FINGERPRINT_EXCLUDES = (
    ":(exclude)**/__pycache__/**",
    ":(exclude)**/*.pyc",
    ":(exclude)**/.DS_Store",
)
SOURCE_FINGERPRINT_GIT_PATHS = (
    *SOURCE_FINGERPRINT_PATHS,
    *SOURCE_FINGERPRINT_EXCLUDES,
)
SOURCE_FINGERPRINT_POLICY = (
    "doc0388_sql_guardrail_build_inputs_v1"
)
EXPECTED_BENCHMARK_CONFIGURATION_SHA256 = (
    "6b848ab47572e27cca73c37972932db5011b157b7007458c76b3b0a54f909a69"
)
EXPECTED_CALIBRATION_CONFIGURATION_SHA256 = (
    "f2fcc82de9471e3efd0836a8b878565df2288f0a367055d006941604e2cad66d"
)
MCP_STATE_FILE = Path("/tmp/bioseq_build_mcp_state.json")
MCP_PUBLICATION_LEDGER = Path(
    "/tmp/bioseq_sql_guardrail_publications.json"
)


class GuardrailError(RuntimeError):
    """One fail-closed qualification error."""


def fail(message: str) -> NoReturn:
    raise GuardrailError(message)


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        fail(f"required guardrail artifact is missing: {path}")
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read guardrail JSON {path}: {error}")
    if not isinstance(value, dict):
        fail(f"guardrail JSON root must be an object: {path}")
    return value


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(value, indent=2, sort_keys=True) + "\n"
    path.write_text(encoded, encoding="utf-8")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json_sha256(value: dict[str, Any]) -> str:
    encoded = json.dumps(
        value, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return sha256_bytes(encoded)


def validate_content_seal(value: dict[str, Any], context: str) -> None:
    seal = value.get("content_sha256")
    if not re.fullmatch(r"[0-9a-f]{64}", str(seal)):
        fail(f"{context} content_sha256 is invalid")
    unsealed = dict(value)
    unsealed.pop("content_sha256")
    if canonical_json_sha256(unsealed) != seal:
        fail(f"{context} content seal does not match")


def run(
    argv: Sequence[str],
    *,
    cwd: Path | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        list(argv),
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if check and result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        fail(
            f"command failed ({result.returncode}): {' '.join(argv)}"
            + (f"\n{detail}" if detail else "")
        )
    return result


def git_output(source_root: Path, *arguments: str) -> str:
    return run(
        ["git", "-C", str(source_root), *arguments]
    ).stdout.strip()


def source_fingerprint(source_root: Path) -> dict[str, Any]:
    head = git_output(source_root, "rev-parse", "HEAD")
    if not re.fullmatch(r"[0-9a-f]{40}", head):
        fail(f"invalid git HEAD for {source_root}: {head!r}")
    diff = run(
        [
            "git", "-C", str(source_root),
            "diff", "--binary", "HEAD", "--",
            *SOURCE_FINGERPRINT_GIT_PATHS,
        ]
    ).stdout.encode("utf-8")
    tracked_text = run(
        [
            "git", "-C", str(source_root),
            "ls-files", "-z", "--",
            *SOURCE_FINGERPRINT_GIT_PATHS,
        ]
    ).stdout
    untracked_text = run(
        [
            "git",
            "-C",
            str(source_root),
            "ls-files",
            "--others",
            "--exclude-standard",
            "-z",
            "--",
            *SOURCE_FINGERPRINT_GIT_PATHS,
        ]
    ).stdout
    untracked = sorted(item for item in untracked_text.split("\0") if item)
    tracked = sorted(item for item in tracked_text.split("\0") if item)
    inventory_paths = sorted(set(tracked) | set(untracked))
    entries = []
    for relative in inventory_paths:
        path = source_root / relative
        if not path.exists():
            continue
        if not path.is_file() or path.is_symlink():
            fail(
                "build-input inventory contains a non-regular or symlinked "
                f"path: {relative}"
            )
        entries.append({
            "path": relative,
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        })
    if not entries:
        fail("build-input source inventory is empty")
    digest = hashlib.sha256()
    digest.update(b"\0policy\0")
    digest.update(SOURCE_FINGERPRINT_POLICY.encode("ascii"))
    digest.update(b"\0tracked-diff\0")
    digest.update(sha256_bytes(diff).encode("ascii"))
    for entry in entries:
        digest.update(b"\0entry\0")
        digest.update(entry["path"].encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(entry["bytes"]).encode("ascii"))
        digest.update(b"\0")
        digest.update(entry["sha256"].encode("ascii"))
    return {
        "head": head,
        "dirty": bool(diff or untracked),
        "tree_sha256": digest.hexdigest(),
        "tracked_diff_sha256": sha256_bytes(diff),
        "tracked_diff_base64": base64.b64encode(diff).decode("ascii"),
        "untracked_files": untracked,
        "inventory_policy": SOURCE_FINGERPRINT_POLICY,
        "inventory_pathspecs": list(SOURCE_FINGERPRINT_GIT_PATHS),
        "inventory_entries": entries,
    }


def cmake_cache(build_dir: Path) -> dict[str, str]:
    path = build_dir / "CMakeCache.txt"
    if not path.is_file():
        fail(f"dedicated build tree has no CMakeCache.txt: {build_dir}")
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        key = key_and_type.split(":", 1)[0]
        result[key] = value
    return result


def environment_fingerprint(build_dir: Path) -> dict[str, Any]:
    cache = cmake_cache(build_dir)
    compiler = cache.get("CMAKE_CXX_COMPILER", "")
    if not compiler:
        fail(f"CMAKE_CXX_COMPILER is absent from {build_dir}/CMakeCache.txt")
    def version_line(argv: Sequence[str]) -> str:
        result = run(argv, check=False)
        if result.returncode != 0:
            return f"unavailable(exit={result.returncode})"
        lines = result.stdout.splitlines()
        return lines[0].strip() if lines else ""

    def cpu_model() -> str:
        if platform.system() == "Darwin":
            for key in ("machdep.cpu.brand_string", "hw.model"):
                result = run(["sysctl", "-n", key], check=False)
                if result.returncode == 0 and result.stdout.strip():
                    return result.stdout.strip()
        cpuinfo = Path("/proc/cpuinfo")
        if cpuinfo.is_file():
            for line in cpuinfo.read_text(
                encoding="utf-8", errors="replace"
            ).splitlines():
                if line.lower().startswith(("model name", "hardware")):
                    return line.split(":", 1)[-1].strip()
        return platform.processor()

    def cpu_topology() -> tuple[int, int]:
        if platform.system() == "Darwin":
            physical = run(
                ["sysctl", "-n", "hw.physicalcpu"], check=False
            )
            packages = run(
                ["sysctl", "-n", "hw.packages"], check=False
            )
            if (
                physical.returncode == 0
                and packages.returncode == 0
                and physical.stdout.strip().isdigit()
                and packages.stdout.strip().isdigit()
            ):
                return int(physical.stdout.strip()), int(
                    packages.stdout.strip()
                )
        topology_root = Path("/sys/devices/system/cpu")
        core_keys: set[tuple[str, str]] = set()
        package_keys: set[str] = set()
        for cpu_path in sorted(topology_root.glob("cpu[0-9]*")):
            core_path = cpu_path / "topology/core_id"
            package_path = cpu_path / "topology/physical_package_id"
            if not core_path.is_file() or not package_path.is_file():
                continue
            core = core_path.read_text(
                encoding="utf-8", errors="replace"
            ).strip()
            package = package_path.read_text(
                encoding="utf-8", errors="replace"
            ).strip()
            if core and package:
                core_keys.add((package, core))
                package_keys.add(package)
        if core_keys and package_keys:
            return len(core_keys), len(package_keys)
        fail("cannot determine physical CPU/package topology")

    logical_cpus = os.cpu_count()
    if logical_cpus is None or logical_cpus <= 0:
        fail("cannot determine logical CPU topology")
    physical_cores, cpu_packages = cpu_topology()
    if (
        physical_cores <= 0
        or cpu_packages <= 0
        or physical_cores > logical_cpus
    ):
        fail("physical CPU/package topology is invalid")
    try:
        page_size = int(os.sysconf("SC_PAGE_SIZE"))
        physical_pages = int(os.sysconf("SC_PHYS_PAGES"))
        physical_memory = page_size * physical_pages
    except (OSError, ValueError):
        fail("cannot determine physical memory/page size")
    make_program = cache.get("CMAKE_MAKE_PROGRAM", "")
    pg_config = cache.get("PostgreSQL_CONFIG", "")
    codegen_c = cache.get("BIOSEQ_CODEGEN_C_COMPILER", "")
    codegen_cxx = cache.get("BIOSEQ_CODEGEN_CXX_COMPILER", "")
    codegen_modules = cache.get(
        "BIOSEQ_CODEGEN_STDLIB_MODULES_JSON", ""
    )
    if not codegen_c or not Path(codegen_c).is_file():
        fail("BIOSEQ_CODEGEN_C_COMPILER is absent or not a file")
    if not codegen_cxx or not Path(codegen_cxx).is_file():
        fail("BIOSEQ_CODEGEN_CXX_COMPILER is absent or not a file")
    codegen_c_path = str(Path(codegen_c).resolve(strict=True))
    codegen_cxx_path = str(Path(codegen_cxx).resolve(strict=True))
    codegen_c_version = version_line([codegen_c_path, "--version"])
    codegen_cxx_version = version_line([codegen_cxx_path, "--version"])
    codegen_modules_path = ""
    codegen_modules_sha256 = ""
    if codegen_modules:
        modules_path = Path(codegen_modules)
        if not modules_path.is_file():
            fail(
                "BIOSEQ_CODEGEN_STDLIB_MODULES_JSON is not a file: "
                f"{codegen_modules}"
            )
        codegen_modules_path = str(modules_path.resolve(strict=True))
        codegen_modules_sha256 = sha256_file(
            Path(codegen_modules_path)
        )
    if (
        re.search(r"\b(?:gcc|g\+\+)\b", codegen_cxx_version, re.I)
        and not codegen_modules_path
    ):
        fail(
            "GCC code-generation toolchain has no authenticated "
            "standard-library modules JSON"
        )
    toolchain_file = cache.get("CMAKE_TOOLCHAIN_FILE", "")
    toolchain_sha256 = (
        sha256_file(Path(toolchain_file))
        if toolchain_file and Path(toolchain_file).is_file()
        else ""
    )
    relevant_cache = {
        key: cache.get(key, "")
        for key in (
            "CMAKE_CXX_COMPILER",
            "CMAKE_C_COMPILER",
            "CMAKE_CXX_COMPILER_ID",
            "CMAKE_CXX_COMPILER_VERSION",
            "CMAKE_CXX_FLAGS",
            "CMAKE_CXX_FLAGS_DEBUG",
            "CMAKE_CXX_FLAGS_RELEASE",
            "CMAKE_EXE_LINKER_FLAGS",
            "CMAKE_SHARED_LINKER_FLAGS",
            "CMAKE_BUILD_TYPE",
            "CMAKE_GENERATOR",
            "CMAKE_MAKE_PROGRAM",
            "CMAKE_TOOLCHAIN_FILE",
            "CMAKE_CXX_COMPILER_LAUNCHER",
            "CMAKE_INTERPROCEDURAL_OPTIMIZATION",
            "CMAKE_OSX_ARCHITECTURES",
            "CMAKE_OSX_DEPLOYMENT_TARGET",
            "VCPKG_TARGET_TRIPLET",
            "VCPKG_HOST_TRIPLET",
            "PostgreSQL_CONFIG",
            "BIOSEQ_CODEGEN_C_COMPILER",
            "BIOSEQ_CODEGEN_CXX_COMPILER",
            "BIOSEQ_CODEGEN_STDLIB_MODULES_JSON",
            "BIOSEQ_TEST_INDEXES",
            "BIOSEQ_BUILD_TESTS",
        )
    }
    value = {
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "cpu_model": cpu_model(),
        "logical_cpu_count": logical_cpus,
        "physical_core_count": physical_cores,
        "cpu_package_count": cpu_packages,
        "page_size_bytes": page_size,
        "physical_memory_bytes": physical_memory,
        "python": platform.python_version(),
        "compiler_version": version_line([compiler, "--version"]),
        "cmake_version": version_line(["cmake", "--version"]),
        "build_tool_version": (
            version_line([make_program, "--version"])
            if make_program else ""
        ),
        "postgresql_version": (
            version_line([pg_config, "--version"])
            if pg_config else ""
        ),
        "codegen_c_compiler": codegen_c_path,
        "codegen_c_compiler_version": codegen_c_version,
        "codegen_cxx_compiler": codegen_cxx_path,
        "codegen_cxx_compiler_version": codegen_cxx_version,
        "codegen_stdlib_modules_json": codegen_modules_path,
        "codegen_stdlib_modules_json_sha256":
            codegen_modules_sha256,
        "toolchain_file_sha256": toolchain_sha256,
        "named_environment": os.environ.get(
            "CMAKE_NAMED_ENV_TYPE", ""
        ),
        "cmake_cache": relevant_cache,
    }
    value["sha256"] = canonical_json_sha256(value)
    return value


def available_memory_bytes() -> int:
    meminfo = Path("/proc/meminfo")
    if meminfo.is_file():
        for line in meminfo.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines():
            if line.startswith("MemAvailable:"):
                match = re.fullmatch(
                    r"MemAvailable:\s+(\d+)\s+kB", line
                )
                if match:
                    value = int(match.group(1)) * 1024
                    if value > 0:
                        return value
                break
    if "SC_AVPHYS_PAGES" in os.sysconf_names:
        try:
            pages = int(os.sysconf("SC_AVPHYS_PAGES"))
            page_size = int(os.sysconf("SC_PAGE_SIZE"))
        except (OSError, ValueError):
            pages = 0
            page_size = 0
        if pages > 0 and page_size > 0:
            return pages * page_size
    if platform.system() == "Darwin":
        result = run(["vm_stat"], check=False)
        if result.returncode == 0:
            page_match = re.search(
                r"page size of (\d+) bytes", result.stdout
            )
            page_size = int(page_match.group(1)) if page_match else 4096
            available_pages = 0
            for key in (
                "Pages free",
                "Pages inactive",
                "Pages speculative",
                "Pages purgeable",
            ):
                match = re.search(
                    rf"^{re.escape(key)}:\s+(\d+)\.",
                    result.stdout,
                    flags=re.MULTILINE,
                )
                if match:
                    available_pages += int(match.group(1))
            if available_pages > 0:
                return available_pages * page_size
    fail("cannot capture available memory for a timed sample")


def cpu_governor_states() -> list[str]:
    if platform.system() == "Linux":
        states = {
            path.read_text(
                encoding="utf-8", errors="replace"
            ).strip()
            for path in Path("/sys/devices/system/cpu").glob(
                "cpu[0-9]*/cpufreq/scaling_governor"
            )
            if path.is_file()
        }
        states.discard("")
        if states:
            return [f"linux:{state}" for state in sorted(states)]
        fail("Linux CPU scaling-governor state is not exposed")
    if platform.system() == "Darwin":
        source = run(["pmset", "-g", "batt"], check=False)
        settings = run(["pmset", "-g", "custom"], check=False)
        if source.returncode != 0 or settings.returncode != 0:
            fail("cannot capture Darwin CPU power policy with pmset")
        source_match = re.search(
            r"Now drawing from '([^']+)'", source.stdout
        )
        active_source = (
            source_match.group(1).strip()
            if source_match else "unknown-source"
        )
        policy_values = []
        for key in ("lowpowermode", "powermode", "highpowermode"):
            values = sorted(set(re.findall(
                rf"^\s*{key}\s+(\S+)\s*$",
                settings.stdout,
                flags=re.MULTILINE,
            )))
            if values:
                policy_values.append(f"{key}={','.join(values)}")
        if not policy_values:
            fail("pmset did not expose a Darwin CPU power policy")
        return [
            "darwin:"
            + active_source
            + ":"
            + ":".join(policy_values)
        ]
    fail(
        f"CPU governor capture is unsupported on {platform.system()}"
    )


def load_state_snapshot() -> dict[str, Any]:
    try:
        load_1, load_5, load_15 = os.getloadavg()
    except OSError:
        fail("cannot capture load averages for a timed sample")
    logical_cpus = os.cpu_count()
    if logical_cpus is None or logical_cpus <= 0:
        fail("cannot normalize load state without logical CPU count")
    return {
        "captured_monotonic_ns": time.monotonic_ns(),
        "load_1": load_1,
        "load_5": load_5,
        "load_15": load_15,
        "logical_cpu_count": logical_cpus,
        "normalized_load_1": load_1 / logical_cpus,
        "available_memory_bytes": available_memory_bytes(),
        "cpu_governor_states": cpu_governor_states(),
    }


def validate_host_state_snapshot(
    snapshot: Any,
    *,
    environment: dict[str, Any],
    context: str,
) -> dict[str, Any]:
    expected_fields = {
        "captured_monotonic_ns",
        "load_1",
        "load_5",
        "load_15",
        "logical_cpu_count",
        "normalized_load_1",
        "available_memory_bytes",
        "cpu_governor_states",
    }
    if not isinstance(snapshot, dict) or set(snapshot) != expected_fields:
        fail(f"{context} host-state fields are not exact")
    captured = positive_integer(
        snapshot.get("captured_monotonic_ns"),
        f"{context}.captured_monotonic_ns",
    )
    logical_cpus = positive_integer(
        snapshot.get("logical_cpu_count"),
        f"{context}.logical_cpu_count",
    )
    if logical_cpus != environment.get("logical_cpu_count"):
        fail(f"{context} host-state CPU topology differs from environment")
    load_values = {
        metric: nonnegative_number(
            snapshot.get(metric), f"{context}.{metric}"
        )
        for metric in ("load_1", "load_5", "load_15")
    }
    normalized = nonnegative_number(
        snapshot.get("normalized_load_1"),
        f"{context}.normalized_load_1",
    )
    if not math.isclose(
        normalized,
        load_values["load_1"] / logical_cpus,
        rel_tol=1e-12,
        abs_tol=1e-15,
    ):
        fail(f"{context} normalized load is internally inconsistent")
    memory = positive_integer(
        snapshot.get("available_memory_bytes"),
        f"{context}.available_memory_bytes",
    )
    governors = validate_cpu_governor_states(
        snapshot.get("cpu_governor_states"),
        f"{context}.cpu_governor_states",
    )
    return {
        "captured_monotonic_ns": captured,
        "normalized_load_1": normalized,
        "available_memory_bytes": memory,
        "cpu_governor_states": governors,
    }


def command_capture_runtime_host_state(
    arguments: argparse.Namespace,
) -> None:
    measurement = load_json(arguments.measurement)
    validate_measurement(measurement)
    run_nonce = positive_integer(
        arguments.run_nonce, "runtime host-state run nonce"
    )
    if run_nonce >= (1 << 53):
        fail("runtime host-state run nonce is not exactly representable")
    result = {
        "schema_version": SCHEMA_VERSION,
        "artifact_kind": "runtime_host_state",
        "phase": arguments.phase,
        "run_nonce": run_nonce,
        "measurement_sha256": sha256_file(arguments.measurement),
        "measurement_content_sha256": measurement["content_sha256"],
        "source": measurement["source"],
        "environment": measurement["environment"],
        "snapshot": load_state_snapshot(),
    }
    result["content_sha256"] = canonical_json_sha256(result)
    validate_runtime_host_state_artifact(
        result,
        measurement=measurement,
        expected_phase=arguments.phase,
    )
    write_json(arguments.output, result)


def validate_runtime_host_state_artifact(
    value: dict[str, Any],
    *,
    measurement: dict[str, Any],
    expected_phase: str,
) -> dict[str, Any]:
    expected_fields = {
        "schema_version",
        "artifact_kind",
        "phase",
        "run_nonce",
        "measurement_sha256",
        "measurement_content_sha256",
        "source",
        "environment",
        "snapshot",
        "content_sha256",
    }
    if set(value) != expected_fields:
        fail("runtime host-state artifact fields are not exact")
    if (
        value.get("schema_version") != SCHEMA_VERSION
        or value.get("artifact_kind") != "runtime_host_state"
        or value.get("phase") != expected_phase
    ):
        fail("runtime host-state artifact identity is invalid")
    run_nonce = positive_integer(
        value.get("run_nonce"), "runtime host-state run nonce"
    )
    if run_nonce >= (1 << 53):
        fail("runtime host-state run nonce is not exactly representable")
    if value.get("measurement_content_sha256") != (
        measurement["content_sha256"]
    ):
        fail("runtime host-state measurement content binding is invalid")
    if (
        value.get("source") != measurement["source"]
        or value.get("environment") != measurement["environment"]
    ):
        fail("runtime host-state source/environment binding is invalid")
    if not re.fullmatch(
        r"[0-9a-f]{64}", str(value.get("measurement_sha256"))
    ):
        fail("runtime host-state measurement file hash is invalid")
    validate_content_seal(value, "runtime host-state artifact")
    return validate_host_state_snapshot(
        value.get("snapshot"),
        environment=measurement["environment"],
        context=f"runtime host-state {expected_phase}",
    )


def require_dedicated_build(
    repository_root: Path,
    cmake_source_dir: Path,
    build_dir: Path,
) -> dict[str, Any]:
    marker_path = build_dir / MARKER_NAME
    marker = load_json(marker_path)
    if marker.get("schema_version") != SCHEMA_VERSION:
        fail(f"invalid dedicated-build marker schema: {marker_path}")
    if marker.get("purpose") != "doc0388_sql_codegen_guardrail":
        fail(f"invalid dedicated-build marker purpose: {marker_path}")
    expected_repository = str(repository_root.resolve())
    expected_cmake_source = str(cmake_source_dir.resolve())
    expected_build = str(build_dir.resolve())
    if marker.get("repository_root") != expected_repository:
        fail(
            "dedicated-build marker repository mismatch: "
            f"{marker.get('repository_root')!r} != "
            f"{expected_repository!r}"
        )
    if marker.get("cmake_source_dir") != expected_cmake_source:
        fail(
            "dedicated-build marker CMake source mismatch: "
            f"{marker.get('cmake_source_dir')!r} != "
            f"{expected_cmake_source!r}"
        )
    if marker.get("build_dir") != expected_build:
        fail(
            "dedicated-build marker path mismatch: "
            f"{marker.get('build_dir')!r} != {expected_build!r}"
        )
    cache = cmake_cache(build_dir)
    configured_source = cache.get("CMAKE_HOME_DIRECTORY", "")
    if Path(configured_source).resolve() != cmake_source_dir.resolve():
        fail(
            "CMake source root does not match the dedicated marker: "
            f"{configured_source}"
        )
    return marker


def command_mark_build(arguments: argparse.Namespace) -> None:
    repository_root = arguments.repository_root.resolve()
    cmake_source_dir = arguments.cmake_source_dir.resolve()
    build_dir = arguments.build_dir.resolve()
    if not repository_root.is_dir():
        fail(f"repository root is not a git worktree: {repository_root}")
    discovered_repository = Path(
        git_output(repository_root, "rev-parse", "--show-toplevel")
    ).resolve()
    if discovered_repository != repository_root:
        fail(
            "repository root must be the git worktree root: "
            f"{repository_root} != {discovered_repository}"
        )
    if not cmake_source_dir.is_dir():
        fail(f"CMake source directory does not exist: {cmake_source_dir}")
    try:
        cmake_source_dir.relative_to(repository_root)
    except ValueError:
        fail(
            "CMake source directory must be inside the repository: "
            f"{cmake_source_dir}"
        )
    if not build_dir.is_dir():
        fail(f"build directory does not exist: {build_dir}")
    cache = cmake_cache(build_dir)
    configured_source = cache.get("CMAKE_HOME_DIRECTORY", "")
    if Path(configured_source).resolve() != cmake_source_dir:
        fail(
            "configured CMake source does not match --cmake-source-dir: "
            f"{configured_source}"
        )
    marker = {
        "schema_version": SCHEMA_VERSION,
        "purpose": "doc0388_sql_codegen_guardrail",
        "repository_root": str(repository_root),
        "cmake_source_dir": str(cmake_source_dir),
        "build_dir": str(build_dir),
    }
    write_json(build_dir / MARKER_NAME, marker)


def parse_artifact_inventory(path: Path) -> dict[str, list[Path]]:
    result = {group: [] for group in ARTIFACT_GROUPS}
    seen: set[tuple[str, Path]] = set()
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        fail(f"cannot read artifact inventory {path}: {error}")
    for line_number, line in enumerate(lines, start=1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 2 or fields[0] not in result:
            fail(
                f"{path}:{line_number}: expected '<sql|json|cpp>\\t<path>'"
            )
        artifact = Path(fields[1]).resolve()
        key = (fields[0], artifact)
        if key in seen:
            fail(f"{path}:{line_number}: duplicate artifact: {artifact}")
        seen.add(key)
        result[fields[0]].append(artifact)
    for group, paths in result.items():
        if not paths:
            fail(f"artifact inventory has no {group} entries: {path}")
    return result


def parse_generator_output_inventory(
    path: Path,
    *,
    build_dir: Path,
) -> list[Path]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        fail(f"cannot read generator-output inventory {path}: {error}")
    outputs: list[Path] = []
    seen: set[Path] = set()
    for line_number, line in enumerate(lines, start=1):
        if not line:
            continue
        output = Path(line).resolve()
        try:
            output.relative_to(build_dir.resolve())
        except ValueError:
            fail(
                f"{path}:{line_number}: generator output escapes the "
                f"dedicated build tree: {output}"
            )
        if output in seen:
            fail(
                f"{path}:{line_number}: duplicate generator output: "
                f"{output}"
            )
        seen.add(output)
        outputs.append(output)
    if not outputs:
        fail("generator-output inventory is empty")
    return sorted(outputs)


def file_inventory_snapshot(paths: Sequence[Path]) -> dict[str, Any]:
    entries = []
    digest = hashlib.sha256()
    for path in sorted(paths):
        if not path.is_file() or path.is_symlink():
            fail(f"generator boundary input/output is not a file: {path}")
        entry = {
            "path": str(path),
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        }
        entries.append(entry)
        digest.update(entry["path"].encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(entry["bytes"]).encode("ascii"))
        digest.update(b"\0")
        digest.update(entry["sha256"].encode("ascii"))
    return {
        "inventory_sha256": digest.hexdigest(),
        "entries": entries,
    }


def generator_dependency_snapshot(
    build_dir: Path,
    target: str,
    generator_outputs: Sequence[Path],
) -> dict[str, Any]:
    cache = cmake_cache(build_dir)
    make_program = cache.get("CMAKE_MAKE_PROGRAM", "")
    if not make_program or "ninja" not in Path(make_program).name:
        fail("generator dependency capture requires Ninja")
    result = run([
        make_program,
        "-C",
        str(build_dir),
        "-t",
        "inputs",
        target,
    ])
    excluded = set(generator_outputs)
    dependencies: set[Path] = set()
    for line_number, line in enumerate(
        result.stdout.splitlines(), start=1
    ):
        if not line.strip():
            continue
        try:
            fields = shlex.split(line)
        except ValueError as error:
            fail(
                "cannot parse Ninja generator dependency "
                f"{line_number}: {error}"
            )
        if len(fields) != 1:
            fail(
                "Ninja generator dependency output is not one path per "
                f"line: {line!r}"
            )
        raw = Path(fields[0])
        dependency = (
            raw.resolve()
            if raw.is_absolute()
            else (build_dir / raw).resolve()
        )
        if dependency not in excluded:
            dependencies.add(dependency)
    if not dependencies:
        fail("Ninja reported no primed generator dependencies")
    return file_inventory_snapshot(sorted(dependencies))


def remove_exact_generator_outputs(
    outputs: Sequence[Path],
    *,
    build_dir: Path,
) -> dict[str, Any]:
    before = file_inventory_snapshot(outputs)
    build_root = build_dir.resolve()
    for output in outputs:
        output.resolve().relative_to(build_root)
        if output.is_symlink() or not output.is_file():
            fail(f"generator output is unsafe to remove: {output}")
        output.unlink()
    remaining = [str(path) for path in outputs if path.exists()]
    if remaining:
        fail(
            "generator output removal was incomplete: "
            + ", ".join(remaining)
        )
    return before


def measure_artifacts(
    inventory: dict[str, list[Path]]
) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for group, paths in inventory.items():
        digest = hashlib.sha256()
        total = 0
        entries = []
        for path in sorted(paths):
            if not path.is_file():
                fail(f"generated {group} artifact is missing: {path}")
            size = path.stat().st_size
            content_sha = sha256_file(path)
            total += size
            digest.update(str(path).encode("utf-8"))
            digest.update(b"\0")
            digest.update(str(size).encode("ascii"))
            digest.update(b"\0")
            digest.update(content_sha.encode("ascii"))
            entries.append(
                {"path": str(path), "bytes": size, "sha256": content_sha}
            )
        result[group] = {
            "bytes": total,
            "files": len(entries),
            "inventory_sha256": digest.hexdigest(),
            "entries": entries,
        }
    return result


def measure_runtime_artifacts(
    generator_executable: Path,
    production_dso: Path,
    benchmark_executable: Path,
    comparator_dso: Path,
    installed_production_dso: Path | None = None,
) -> dict[str, dict[str, Any]]:
    paths = {
        "generator_executable": generator_executable.resolve(),
        "production_dso": production_dso.resolve(),
        "benchmark_executable": benchmark_executable.resolve(),
        "comparator_dso": comparator_dso.resolve(),
    }
    if installed_production_dso is not None:
        paths["installed_production_dso"] = (
            installed_production_dso.resolve()
        )
    expected_names = (
        RUNTIME_ARTIFACT_NAMES
        if installed_production_dso is not None
        else MEASURED_RUNTIME_ARTIFACT_NAMES
    )
    result: dict[str, dict[str, Any]] = {}
    for name in expected_names:
        path = paths[name]
        if not path.is_file():
            fail(f"required runtime artifact is missing: {path}")
        if (
            name in {"generator_executable", "benchmark_executable"}
            and not os.access(path, os.X_OK)
        ):
            fail(f"required runtime executable is not executable: {path}")
        result[name] = {
            "path": str(path),
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        }
    if installed_production_dso is not None:
        installed = result["installed_production_dso"]
        built = result["production_dso"]
        if (
            installed["bytes"] != built["bytes"]
            or installed["sha256"] != built["sha256"]
        ):
            fail(
                "installed production DSO differs from the exact measured "
                "postgresql_bioseq target"
            )
    return result


def measure_runtime_file(
    path: Path,
    *,
    context: str,
    executable: bool = False,
) -> dict[str, Any]:
    """Hash one exact runtime file for a measurement-coherence join."""
    resolved = path.resolve()
    if not resolved.is_file():
        fail(f"{context} is missing: {resolved}")
    if executable and not os.access(resolved, os.X_OK):
        fail(f"{context} is not executable: {resolved}")
    return {
        "path": str(resolved),
        "bytes": resolved.stat().st_size,
        "sha256": sha256_file(resolved),
    }


def maximum_rss_bytes(time_output: str) -> int:
    mac = re.search(
        r"^\s*(\d+)\s+maximum resident set size\s*$",
        time_output,
        flags=re.MULTILINE,
    )
    if mac:
        return int(mac.group(1))
    gnu = re.search(
        r"Maximum resident set size \(kbytes\):\s*(\d+)",
        time_output,
    )
    if gnu:
        return int(gnu.group(1)) * 1024
    fail("/usr/bin/time output has no recognized maximum-RSS field")


def measured_command(argv: Sequence[str]) -> dict[str, Any]:
    time_binary = Path("/usr/bin/time")
    if not time_binary.is_file():
        fail("required measurement binary is unavailable: /usr/bin/time")
    with tempfile.NamedTemporaryFile(prefix="bioseq-sql-time-", delete=False) as handle:
        output_path = Path(handle.name)
    try:
        time_args = ["-l"] if platform.system() == "Darwin" else ["-v"]
        command = [
            str(time_binary),
            *time_args,
            "-o",
            str(output_path),
            *argv,
        ]
        load_before = load_state_snapshot()
        started = time.monotonic_ns()
        result = run(command, check=False)
        elapsed = time.monotonic_ns() - started
        load_after = load_state_snapshot()
        timing = output_path.read_text(
            encoding="utf-8", errors="replace"
        )
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            fail(
                f"measured command failed ({result.returncode}): "
                f"{' '.join(argv)}"
                + (f"\n{detail}" if detail else "")
            )
        return {
            "elapsed_ns": elapsed,
            "peak_rss_bytes": maximum_rss_bytes(timing),
            "load_before": load_before,
            "load_after": load_after,
        }
    finally:
        output_path.unlink(missing_ok=True)


def _process_table() -> dict[int, tuple[int, str]]:
    result = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,command="],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode != 0:
        fail(
            "process-boundary sampler failed while measuring the "
            "generator target"
        )
    table: dict[int, tuple[int, str]] = {}
    for line in result.stdout.splitlines():
        match = re.match(r"^\s*(\d+)\s+(\d+)\s+(.*)$", line)
        if match:
            table[int(match.group(1))] = (
                int(match.group(2)),
                match.group(3),
            )
    if not table:
        fail(
            "process-boundary sampler returned no process records while "
            "measuring the generator target"
        )
    return table


def measured_generator_build(
    argv: Sequence[str],
    *,
    generator_executable: Path,
    expect_generator: bool,
) -> dict[str, Any]:
    """Measure one Ninja invocation and attest its live process boundary."""
    time_binary = Path("/usr/bin/time")
    if not time_binary.is_file():
        fail("required measurement binary is unavailable: /usr/bin/time")
    with (
        tempfile.NamedTemporaryFile(
            prefix="bioseq-sql-time-", delete=False
        ) as time_handle,
        tempfile.NamedTemporaryFile(
            prefix="bioseq-sql-stdout-", delete=False
        ) as stdout_handle,
        tempfile.NamedTemporaryFile(
            prefix="bioseq-sql-stderr-", delete=False
        ) as stderr_handle,
    ):
        time_path = Path(time_handle.name)
        stdout_path = Path(stdout_handle.name)
        stderr_path = Path(stderr_handle.name)
    try:
        time_args = ["-l"] if platform.system() == "Darwin" else ["-v"]
        command = [
            str(time_binary),
            *time_args,
            "-o",
            str(time_path),
            *argv,
        ]
        observed: dict[int, dict[str, Any]] = {}
        load_before = load_state_snapshot()
        started = time.monotonic_ns()
        with (
            stdout_path.open("wb") as stdout_output,
            stderr_path.open("wb") as stderr_output,
        ):
            process = subprocess.Popen(
                command,
                stdout=stdout_output,
                stderr=stderr_output,
            )
            observed[process.pid] = {
                "pid": process.pid,
                "ppid": os.getpid(),
                "commands": {" ".join(command)},
            }
            stop_monitor = threading.Event()
            monitor_started = threading.Event()
            monitor_errors: list[BaseException] = []

            def sample_process_tree() -> None:
                monitor_started.set()
                try:
                    while not stop_monitor.is_set():
                        table = _process_table()
                        descendants = {process.pid}
                        changed = True
                        while changed:
                            changed = False
                            for pid, (parent, _) in table.items():
                                if (
                                    parent in descendants
                                    and pid not in descendants
                                ):
                                    descendants.add(pid)
                                    changed = True
                        for pid in descendants:
                            parent, process_command = table.get(
                                pid, (0, "")
                            )
                            record = observed.setdefault(pid, {
                                "pid": pid,
                                "ppid": parent,
                                "commands": set(),
                            })
                            if record["ppid"] == 0 and parent > 0:
                                record["ppid"] = parent
                            if process_command:
                                record["commands"].add(process_command)
                        stop_monitor.wait(0.002)
                except BaseException as error:
                    monitor_errors.append(error)
                    stop_monitor.set()

            monitor = threading.Thread(
                target=sample_process_tree,
                name="sql-guardrail-process-monitor",
                daemon=True,
            )
            monitor.start()
            monitor_started.wait()
            returncode = process.wait()
            completed = time.monotonic_ns()
            stop_monitor.set()
            monitor_join_started = time.monotonic_ns()
            monitor.join()
            monitor_join_ns = time.monotonic_ns() - monitor_join_started
            if monitor_errors:
                error = monitor_errors[0]
                if isinstance(error, GuardrailError):
                    raise error
                fail(
                    "process-boundary sampler failed: "
                    f"{type(error).__name__}: {error}"
                )
        elapsed = completed - started
        load_after = load_state_snapshot()
        stdout_text = stdout_path.read_text(
            encoding="utf-8", errors="replace"
        )
        stderr_text = stderr_path.read_text(
            encoding="utf-8", errors="replace"
        )
        transcript = stdout_text + "\n" + stderr_text
        if returncode != 0:
            detail = (stderr_text or stdout_text).strip()
            fail(
                f"measured generator build failed ({returncode}): "
                f"{' '.join(argv)}"
                + (f"\n{detail}" if detail else "")
            )

        generator = generator_executable.resolve(strict=True)

        def executable_token(command_text: str) -> str:
            try:
                tokens = shlex.split(command_text)
            except ValueError:
                tokens = command_text.split()
            return tokens[0] if tokens else ""

        def is_generator_command(command_text: str) -> bool:
            token = executable_token(command_text)
            if not token:
                return False
            token_path = Path(token)
            if token_path.name != generator.name:
                return False
            if token_path.is_absolute():
                return token_path.resolve(strict=False) == generator
            return token == generator.name

        generator_pids = sorted(
            pid
            for pid, record in observed.items()
            if any(
                is_generator_command(command_text)
                for command_text in record["commands"]
            )
        )
        if expect_generator and len(generator_pids) != 1:
            fail(
                "cold generator boundary did not observe exactly one "
                f"generator process: {generator_pids}"
            )
        if not expect_generator and generator_pids:
            fail(
                "no-op generator boundary unexpectedly executed the "
                "generator"
            )
        if not expect_generator:
            no_work_lines = [
                line
                for line in transcript.splitlines()
                if line.strip() == "ninja: no work to do."
            ]
            if (
                len(no_work_lines) != 1
                or str(generator) in transcript
            ):
                fail(
                    "no-op generator boundary lacks deterministic Ninja "
                    "no-work proof or contains a generator edge"
                )
        if expect_generator and str(generator) not in transcript:
            fail(
                "verbose cold-generator transcript has no exact generator "
                "edge command"
            )

        parent_by_pid = {
            pid: int(record["ppid"])
            for pid, record in observed.items()
        }

        def descends_from_generator(pid: int) -> bool:
            if not generator_pids:
                return False
            visited: set[int] = set()
            while pid not in visited and pid > 0:
                if pid in generator_pids:
                    return True
                visited.add(pid)
                pid = parent_by_pid.get(pid, 0)
            return False

        wrapper_names = {
            "time",
            "cmake",
            "ninja",
            "sh",
            "bash",
            "zsh",
        }
        unexpected = []
        for pid, record in observed.items():
            if descends_from_generator(pid):
                continue
            tokens = {
                Path(executable_token(command_text)).name
                for command_text in record["commands"]
                if executable_token(command_text)
            }
            if tokens and not tokens.issubset(wrapper_names):
                unexpected.append({
                    "pid": pid,
                    "executables": sorted(tokens),
                })
        if unexpected:
            fail(
                "generator measurement observed a compiler/prepare sibling "
                f"outside the generator subtree: {unexpected}"
            )
        process_records = [
            {
                "pid": pid,
                "ppid": int(record["ppid"]),
                "commands": sorted(record["commands"]),
            }
            for pid, record in sorted(observed.items())
        ]
        return {
            "elapsed_ns": elapsed,
            "monitor_join_ns": monitor_join_ns,
            "peak_rss_bytes": maximum_rss_bytes(
                time_path.read_text(
                    encoding="utf-8", errors="replace"
                )
            ),
            "load_before": load_before,
            "load_after": load_after,
            "process_boundary": {
                "expected_generator": expect_generator,
                "generator_executable": str(generator),
                "generator_pids": generator_pids,
                "unexpected_pre_generator_processes": unexpected,
                "transcript_sha256":
                    sha256_bytes(transcript.encode("utf-8")),
                "transcript_base64": base64.b64encode(
                    transcript.encode("utf-8")
                ).decode("ascii"),
                "observed_processes": process_records,
            },
        }
    finally:
        time_path.unlink(missing_ok=True)
        stdout_path.unlink(missing_ok=True)
        stderr_path.unlink(missing_ok=True)


def target_clean_command(build_dir: Path, target: str) -> list[str]:
    cache = cmake_cache(build_dir)
    make_program = cache.get("CMAKE_MAKE_PROGRAM", "")
    if not make_program or "ninja" not in Path(make_program).name:
        fail("DOC0388 measurement requires a Ninja dedicated build tree")
    return [make_program, "-C", str(build_dir), "-t", "clean", target]


def target_build_command(
    build_dir: Path,
    target: str,
    *,
    verbose: bool = False,
) -> list[str]:
    cmake = shutil.which("cmake")
    if cmake is None:
        fail("cmake is unavailable on PATH")
    result = [
        cmake,
        "--build",
        str(build_dir),
        "--target",
        target,
        "--parallel",
        "1",
    ]
    if verbose:
        result.append("--verbose")
    return result


def clean_target(build_dir: Path, target: str) -> None:
    run(target_clean_command(build_dir, target))


def median_int(samples: Sequence[int]) -> int:
    if len(samples) < MIN_SAMPLES:
        fail(f"at least {MIN_SAMPLES} samples are required")
    if any(not isinstance(item, int) or item <= 0 for item in samples):
        fail("measurement samples must be positive integers")
    return int(statistics.median(samples))


def command_measure(arguments: argparse.Namespace) -> None:
    repository_root = arguments.repository_root.resolve()
    cmake_source_dir = arguments.cmake_source_dir.resolve()
    build_dir = arguments.build_dir.resolve()
    require_dedicated_build(
        repository_root, cmake_source_dir, build_dir
    )
    source = source_fingerprint(repository_root)
    if arguments.role == "stage0" and source["dirty"]:
        fail("a Stage-0 measurement must use a clean git worktree")
    if arguments.repetitions < MIN_SAMPLES:
        fail(f"--repetitions must be at least {MIN_SAMPLES}")

    generator_cold: list[int] = []
    generator_noop: list[int] = []
    generator_rss: list[int] = []
    compile_samples: dict[str, list[int]] = {
        target: [] for target in arguments.affected_target
    }
    load_state_samples: list[dict[str, Any]] = []
    # Prime the complete target graph once outside every timed boundary.
    # Ninja's recursive clean tool is intentionally forbidden here: it would
    # rebuild compilers/prepare edges and mislabel their time/RSS as generator
    # cost.
    run(target_build_command(build_dir, arguments.generator_target))
    timed_generator_artifact = measure_runtime_file(
        arguments.generator_executable,
        context="timed generator executable",
        executable=True,
    )
    generator_outputs = parse_generator_output_inventory(
        arguments.generator_output_inventory,
        build_dir=build_dir,
    )
    primed_outputs = file_inventory_snapshot(generator_outputs)
    primed_dependencies = generator_dependency_snapshot(
        build_dir,
        arguments.generator_target,
        generator_outputs,
    )
    generator_boundaries = []
    for _ in range(arguments.repetitions):
        if measure_runtime_file(
            arguments.generator_executable,
            context="generator executable before cold timing",
            executable=True,
        ) != timed_generator_artifact:
            fail("generator executable changed before cold timing")
        if generator_dependency_snapshot(
            build_dir,
            arguments.generator_target,
            generator_outputs,
        ) != primed_dependencies:
            fail("primed generator dependencies changed before timing")
        removed_outputs = remove_exact_generator_outputs(
            generator_outputs,
            build_dir=build_dir,
        )
        if removed_outputs != primed_outputs:
            fail("generator outputs changed before the cold boundary")
        cold = measured_generator_build(
            target_build_command(
                build_dir,
                arguments.generator_target,
                verbose=True,
            ),
            generator_executable=arguments.generator_executable,
            expect_generator=True,
        )
        if measure_runtime_file(
            arguments.generator_executable,
            context="generator executable after cold timing",
            executable=True,
        ) != timed_generator_artifact:
            fail("generator executable changed during cold timing")
        regenerated_outputs = file_inventory_snapshot(generator_outputs)
        if regenerated_outputs != primed_outputs:
            fail(
                "cold generator did not byte-reproduce every exact output"
            )
        if generator_dependency_snapshot(
            build_dir,
            arguments.generator_target,
            generator_outputs,
        ) != primed_dependencies:
            fail("primed generator dependencies changed during timing")
        generator_cold.append(cold["elapsed_ns"])
        generator_rss.append(cold["peak_rss_bytes"])
        load_state_samples.append({
            "phase": "generator_cold",
            "before": cold["load_before"],
            "after": cold["load_after"],
        })
        noop = measured_generator_build(
            target_build_command(
                build_dir,
                arguments.generator_target,
                verbose=True,
            ),
            generator_executable=arguments.generator_executable,
            expect_generator=False,
        )
        if measure_runtime_file(
            arguments.generator_executable,
            context="generator executable after no-op timing",
            executable=True,
        ) != timed_generator_artifact:
            fail("generator executable changed during no-op timing")
        if file_inventory_snapshot(generator_outputs) != primed_outputs:
            fail("no-op generator build changed an exact output")
        generator_noop.append(noop["elapsed_ns"])
        load_state_samples.append({
            "phase": "generator_noop",
            "before": noop["load_before"],
            "after": noop["load_after"],
        })
        generator_boundaries.append({
            "removed_output_inventory_sha256":
                removed_outputs["inventory_sha256"],
            "regenerated_output_inventory_sha256":
                regenerated_outputs["inventory_sha256"],
            "dependency_inventory_sha256":
                primed_dependencies["inventory_sha256"],
            "cold_process_boundary": cold["process_boundary"],
            "noop_process_boundary": noop["process_boundary"],
        })

    for target in arguments.affected_target:
        for _ in range(arguments.repetitions):
            clean_target(build_dir, target)
            sample = measured_command(target_build_command(build_dir, target))
            compile_samples[target].append(sample["elapsed_ns"])
            load_state_samples.append({
                "phase": f"compile_{target}",
                "before": sample["load_before"],
                "after": sample["load_after"],
            })

    post_compile_outputs = file_inventory_snapshot(generator_outputs)
    post_compile_dependencies = generator_dependency_snapshot(
        build_dir,
        arguments.generator_target,
        generator_outputs,
    )
    if post_compile_outputs != primed_outputs:
        fail(
            "affected-target rebuilds changed the exact timed generator "
            "outputs"
        )
    if post_compile_dependencies != primed_dependencies:
        fail(
            "affected-target rebuilds changed the exact timed generator "
            "dependencies"
        )
    if measure_runtime_file(
        arguments.generator_executable,
        context="generator executable after affected-target rebuilds",
        executable=True,
    ) != timed_generator_artifact:
        fail(
            "affected-target rebuilds replaced the exact timed generator "
            "executable"
        )

    required_targets = {"postgresql_bioseq", "bioseq_bench_e2e"}
    if set(arguments.affected_target) != required_targets:
        fail(
            "--affected-target must name exactly postgresql_bioseq and "
            "bioseq_bench_e2e"
        )
    samples = {
        "generator_cold_ns": generator_cold,
        "generator_noop_ns": generator_noop,
        "generator_peak_rss_bytes": generator_rss,
        "compile_postgresql_bioseq_ns":
            compile_samples["postgresql_bioseq"],
        "compile_bioseq_bench_e2e_ns":
            compile_samples["bioseq_bench_e2e"],
    }
    aggregates = {
        metric: median_int(values) for metric, values in samples.items()
    }
    artifacts = measure_artifacts(
        parse_artifact_inventory(arguments.artifact_inventory)
    )
    runtime_artifacts = measure_runtime_artifacts(
        arguments.generator_executable,
        arguments.production_dso,
        arguments.benchmark_executable,
        arguments.comparator_dso,
    )
    if (
        runtime_artifacts["generator_executable"]
        != timed_generator_artifact
    ):
        fail(
            "retained generator executable differs from the exact binary "
            "whose time and RSS were measured"
        )
    output = {
        "schema_version": SCHEMA_VERSION,
        "artifact_kind": "measurement_preinstall",
        "role": arguments.role,
        "sample_policy": {
            "repetitions": arguments.repetitions,
            "aggregate": "median",
            "parallelism": 1,
            "generator_target": arguments.generator_target,
            "affected_targets": sorted(arguments.affected_target),
        },
        "source": source,
        "environment": environment_fingerprint(build_dir),
        "samples": samples,
        "aggregates": aggregates,
        "artifacts": artifacts,
        "runtime_artifacts": runtime_artifacts,
        "measurement_context": {
            "load_state_samples": load_state_samples,
            "generator_boundary": {
                "output_inventory_file_sha256": sha256_file(
                    arguments.generator_output_inventory
                ),
                "timed_generator_artifact": timed_generator_artifact,
                "primed_outputs": primed_outputs,
                "primed_dependencies": primed_dependencies,
                "post_compile_outputs": post_compile_outputs,
                "post_compile_dependencies": post_compile_dependencies,
                "samples": generator_boundaries,
            },
        },
    }
    output["content_sha256"] = canonical_json_sha256(output)
    write_json(arguments.output, output)


def command_bind_installed_dso(arguments: argparse.Namespace) -> None:
    """Finalize a measurement after the exact measured DSO is installed."""
    measurement = load_json(arguments.measurement)
    validate_measurement(measurement, require_installed=False)
    recorded = measurement["runtime_artifacts"]
    current = measure_runtime_artifacts(
        Path(recorded["generator_executable"]["path"]),
        Path(recorded["production_dso"]["path"]),
        Path(recorded["benchmark_executable"]["path"]),
        Path(recorded["comparator_dso"]["path"]),
        arguments.installed_production_dso,
    )
    for name in MEASURED_RUNTIME_ARTIFACT_NAMES:
        if current[name] != recorded[name]:
            fail(
                f"measured runtime artifact changed before installation "
                f"attestation: {name}"
            )
    measurement["artifact_kind"] = "measurement"
    measurement["runtime_artifacts"] = current
    measurement.pop("content_sha256", None)
    measurement["content_sha256"] = canonical_json_sha256(measurement)
    write_json(arguments.output, measurement)


def positive_number(value: Any, context: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        fail(f"{context} must be numeric")
    result = float(value)
    if not math.isfinite(result) or result <= 0.0:
        fail(f"{context} must be finite and positive")
    return result


def positive_integer(value: Any, context: str) -> int:
    result = positive_number(value, context)
    if not result.is_integer() or result > (2**64 - 1):
        fail(f"{context} must be a positive 64-bit integer")
    return int(result)


def finite_number(value: Any, context: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        fail(f"{context} must be numeric")
    result = float(value)
    if not math.isfinite(result):
        fail(f"{context} must be finite")
    return result


def nonnegative_number(value: Any, context: str) -> float:
    result = finite_number(value, context)
    if result < 0.0:
        fail(f"{context} must be nonnegative")
    return result


def validate_environment(
    value: Any,
    context: str,
) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != set(
        ENVIRONMENT_FIELDS
    ):
        fail(
            f"{context} environment fields are not exact"
        )
    for field in (
        "system",
        "release",
        "machine",
        "processor",
        "cpu_model",
        "python",
        "compiler_version",
        "cmake_version",
        "build_tool_version",
        "postgresql_version",
        "codegen_c_compiler",
        "codegen_c_compiler_version",
        "codegen_cxx_compiler",
        "codegen_cxx_compiler_version",
        "codegen_stdlib_modules_json",
        "codegen_stdlib_modules_json_sha256",
        "toolchain_file_sha256",
        "named_environment",
    ):
        if not isinstance(value.get(field), str):
            fail(f"{context} environment {field} must be a string")
    for field in (
        "logical_cpu_count",
        "physical_core_count",
        "cpu_package_count",
        "page_size_bytes",
        "physical_memory_bytes",
    ):
        positive_integer(
            value.get(field), f"{context} environment {field}"
        )
    if value["physical_core_count"] > value["logical_cpu_count"]:
        fail(f"{context} environment CPU topology is invalid")
    if value["cpu_package_count"] > value["physical_core_count"]:
        fail(f"{context} environment package topology is invalid")
    if not isinstance(value.get("cmake_cache"), dict):
        fail(f"{context} environment cmake_cache must be an object")
    if not re.fullmatch(
        r"(?:[0-9a-f]{64})?", value["toolchain_file_sha256"]
    ):
        fail(f"{context} environment toolchain hash is invalid")
    if not re.fullmatch(
        r"(?:[0-9a-f]{64})?",
        value["codegen_stdlib_modules_json_sha256"],
    ):
        fail(f"{context} environment codegen modules hash is invalid")
    if bool(value["codegen_stdlib_modules_json"]) != bool(
        value["codegen_stdlib_modules_json_sha256"]
    ):
        fail(
            f"{context} environment codegen modules path/hash disagree"
        )
    recorded_sha = str(value.get("sha256", ""))
    if not re.fullmatch(r"[0-9a-f]{64}", recorded_sha):
        fail(f"{context} environment fingerprint is invalid")
    unhashed = dict(value)
    del unhashed["sha256"]
    if canonical_json_sha256(unhashed) != recorded_sha:
        fail(f"{context} environment fingerprint seal is invalid")
    return value


def validate_source_fingerprint(
    value: Any,
    context: str,
) -> dict[str, Any]:
    expected_fields = {
        "head",
        "dirty",
        "tree_sha256",
        "tracked_diff_sha256",
        "tracked_diff_base64",
        "untracked_files",
        "inventory_policy",
        "inventory_pathspecs",
        "inventory_entries",
    }
    if not isinstance(value, dict) or set(value) != expected_fields:
        fail(f"{context} source fingerprint fields are not exact")
    if not re.fullmatch(r"[0-9a-f]{40}", str(value.get("head"))):
        fail(f"{context} source HEAD is invalid")
    if not isinstance(value.get("dirty"), bool):
        fail(f"{context} source dirty flag is invalid")
    for field in ("tree_sha256", "tracked_diff_sha256"):
        if not re.fullmatch(r"[0-9a-f]{64}", str(value.get(field))):
            fail(f"{context} source {field} is invalid")
    tracked_diff_base64 = value.get("tracked_diff_base64")
    if not isinstance(tracked_diff_base64, str):
        fail(f"{context} source tracked diff payload is invalid")
    try:
        tracked_diff = base64.b64decode(
            tracked_diff_base64.encode("ascii"), validate=True
        )
    except (UnicodeEncodeError, ValueError):
        fail(f"{context} source tracked diff payload is invalid")
    if sha256_bytes(tracked_diff) != value["tracked_diff_sha256"]:
        fail(f"{context} source tracked diff payload hash is invalid")
    if value.get("inventory_policy") != SOURCE_FINGERPRINT_POLICY:
        fail(f"{context} source inventory policy is invalid")
    if value.get("inventory_pathspecs") != list(
        SOURCE_FINGERPRINT_GIT_PATHS
    ):
        fail(f"{context} source inventory pathspecs are not exact")
    untracked = value.get("untracked_files")
    if (
        not isinstance(untracked, list)
        or untracked != sorted(set(untracked))
        or any(not isinstance(path, str) for path in untracked)
    ):
        fail(f"{context} source untracked file list is invalid")
    entries = value.get("inventory_entries")
    if not isinstance(entries, list) or not entries:
        fail(f"{context} source inventory is empty")
    if entries != sorted(entries, key=lambda entry: str(entry.get("path"))):
        fail(f"{context} source inventory is not deterministically ordered")
    seen: set[str] = set()
    digest = hashlib.sha256()
    digest.update(b"\0policy\0")
    digest.update(SOURCE_FINGERPRINT_POLICY.encode("ascii"))
    digest.update(b"\0tracked-diff\0")
    digest.update(value["tracked_diff_sha256"].encode("ascii"))
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict) or set(entry) != {
            "path", "bytes", "sha256"
        }:
            fail(f"{context} source inventory entry {index} is malformed")
        relative = entry.get("path")
        if (
            not isinstance(relative, str)
            or not relative
            or relative.startswith("/")
            or ".." in Path(relative).parts
            or relative in seen
        ):
            fail(f"{context} source inventory path {index} is invalid")
        allowed = any(
            relative == pathspec
            or relative.startswith(pathspec.rstrip("/") + "/")
            for pathspec in SOURCE_FINGERPRINT_PATHS
        )
        if not allowed:
            fail(
                f"{context} source inventory path is outside policy: "
                f"{relative}"
            )
        seen.add(relative)
        size = entry.get("bytes")
        if (
            isinstance(size, bool)
            or not isinstance(size, int)
            or size < 0
        ):
            fail(f"{context} source inventory bytes are invalid")
        content_sha = str(entry.get("sha256", ""))
        if not re.fullmatch(r"[0-9a-f]{64}", content_sha):
            fail(f"{context} source inventory hash is invalid")
        digest.update(b"\0entry\0")
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(size).encode("ascii"))
        digest.update(b"\0")
        digest.update(content_sha.encode("ascii"))
    if digest.hexdigest() != value["tree_sha256"]:
        fail(f"{context} source inventory seal is invalid")
    if value["dirty"] != bool(
        value["tracked_diff_sha256"] != sha256_bytes(b"")
        or untracked
    ):
        fail(f"{context} source dirty flag disagrees with relevant inputs")
    return value


def source_build_identity(value: dict[str, Any]) -> dict[str, Any]:
    """Return only code/build inputs that may invalidate qualification.

    Repository HEAD is retained as report provenance, but deliberately does
    not participate: a later DOC/report-only commit must not invalidate
    unchanged compiled inputs.
    """
    return {
        field: value[field]
        for field in (
            "dirty",
            "tree_sha256",
            "tracked_diff_sha256",
            "tracked_diff_base64",
            "untracked_files",
            "inventory_policy",
            "inventory_pathspecs",
            "inventory_entries",
        )
    }


def validate_cpu_governor_states(
    value: Any,
    context: str,
) -> list[str]:
    if (
        not isinstance(value, list)
        or not value
        or any(
            not isinstance(item, str) or not item.strip()
            for item in value
        )
        or value != sorted(set(value))
    ):
        fail(
            f"{context} must be a nonempty sorted list of unique strings"
        )
    return value


def summarize_measurement_context(
    context: Any,
    *,
    environment: dict[str, Any],
    repetitions: int,
    label: str,
) -> dict[str, Any]:
    if not isinstance(context, dict) or set(context) != {
        "load_state_samples",
        "generator_boundary",
    }:
        fail(f"{label} measurement_context fields are not exact")
    validate_generator_boundary(
        context.get("generator_boundary"),
        repetitions=repetitions,
        label=label,
    )
    load_samples = context.get("load_state_samples")
    if (
        not isinstance(load_samples, list)
        or len(load_samples) != repetitions * 4
    ):
        fail(f"{label} load-state sample count is invalid")
    expected_phase_counts = {
        "generator_cold": repetitions,
        "generator_noop": repetitions,
        "compile_postgresql_bioseq": repetitions,
        "compile_bioseq_bench_e2e": repetitions,
    }
    observed_phase_counts = {
        phase: 0 for phase in expected_phase_counts
    }
    normalized_load_values: list[float] = []
    available_memory_values: list[int] = []
    governor_states: set[str] = set()
    previous_capture: int | None = None
    for index, sample in enumerate(load_samples):
        if (
            not isinstance(sample, dict)
            or set(sample) != {"phase", "before", "after"}
        ):
            fail(f"{label} load_state_samples[{index}] is malformed")
        phase = sample.get("phase")
        if phase not in observed_phase_counts:
            fail(f"{label} load_state_samples[{index}] has invalid phase")
        observed_phase_counts[phase] += 1
        boundary_captures = []
        for boundary in ("before", "after"):
            snapshot = sample.get(boundary)
            expected_fields = {
                "captured_monotonic_ns",
                "load_1",
                "load_5",
                "load_15",
                "logical_cpu_count",
                "normalized_load_1",
                "available_memory_bytes",
                "cpu_governor_states",
            }
            if not isinstance(snapshot, dict) or set(snapshot) != (
                expected_fields
            ):
                fail(
                    f"{label} load_state_samples[{index}].{boundary} "
                    "fields are not exact"
                )
            captured = positive_integer(
                snapshot.get("captured_monotonic_ns"),
                f"{label} load_state_samples[{index}].{boundary}.captured",
            )
            if previous_capture is not None and captured <= previous_capture:
                fail(f"{label} load-state captures are not monotonic")
            previous_capture = captured
            boundary_captures.append(captured)
            for metric in (
                "load_1", "load_5", "load_15", "normalized_load_1"
            ):
                nonnegative_number(
                    snapshot.get(metric),
                    f"{label} load_state_samples[{index}]."
                    f"{boundary}.{metric}",
                )
            logical_cpus = positive_integer(
                snapshot.get("logical_cpu_count"),
                f"{label} load_state_samples[{index}].{boundary}."
                "logical_cpu_count",
            )
            if logical_cpus != environment.get("logical_cpu_count"):
                fail(
                    f"{label} load-state CPU topology differs from "
                    "the environment fingerprint"
                )
            normalized = finite_number(
                snapshot.get("normalized_load_1"),
                f"{label} load_state_samples[{index}].{boundary}."
                "normalized_load_1",
            )
            load_1 = finite_number(
                snapshot.get("load_1"),
                f"{label} load_state_samples[{index}].{boundary}.load_1",
            )
            if not math.isclose(
                normalized,
                load_1 / logical_cpus,
                rel_tol=1e-12,
                abs_tol=1e-15,
            ):
                fail(f"{label} normalized load is internally inconsistent")
            normalized_load_values.append(normalized)
            available_memory_values.append(positive_integer(
                snapshot.get("available_memory_bytes"),
                f"{label} load_state_samples[{index}].{boundary}."
                "available_memory_bytes",
            ))
            governor_states.update(validate_cpu_governor_states(
                snapshot.get("cpu_governor_states"),
                f"{label} load_state_samples[{index}].{boundary}."
                "cpu_governor_states",
            ))
        if boundary_captures[1] <= boundary_captures[0]:
            fail(f"{label} timed sample boundaries are not ordered")
    if observed_phase_counts != expected_phase_counts:
        fail(f"{label} load-state phase counts are invalid")
    return {
        "normalized_load_1_max": max(normalized_load_values),
        "available_memory_bytes_min": min(available_memory_values),
        "cpu_governor_states": sorted(governor_states),
    }


def validate_file_inventory_snapshot(
    value: Any,
    *,
    context: str,
) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != {
        "inventory_sha256", "entries"
    }:
        fail(f"{context} file inventory fields are not exact")
    entries = value.get("entries")
    if not isinstance(entries, list) or not entries:
        fail(f"{context} file inventory is empty")
    paths = [entry.get("path") for entry in entries if isinstance(entry, dict)]
    if len(paths) != len(entries) or paths != sorted(paths):
        fail(f"{context} file inventory paths are not exactly ordered")
    if len(set(paths)) != len(paths):
        fail(f"{context} file inventory paths are not unique")
    digest = hashlib.sha256()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict) or set(entry) != {
            "path", "bytes", "sha256"
        }:
            fail(f"{context} file inventory entry {index} is malformed")
        path = entry.get("path")
        size = entry.get("bytes")
        content_sha = entry.get("sha256")
        if (
            not isinstance(path, str)
            or not path
            or isinstance(size, bool)
            or not isinstance(size, int)
            or size < 0
            or not re.fullmatch(r"[0-9a-f]{64}", str(content_sha))
        ):
            fail(f"{context} file inventory entry {index} is invalid")
        digest.update(path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(size).encode("ascii"))
        digest.update(b"\0")
        digest.update(str(content_sha).encode("ascii"))
    if digest.hexdigest() != value.get("inventory_sha256"):
        fail(f"{context} file inventory seal is invalid")
    return value


def validate_generator_process_boundary(
    value: Any,
    *,
    expect_generator: bool,
    context: str,
) -> None:
    expected_fields = {
        "expected_generator",
        "generator_executable",
        "generator_pids",
        "unexpected_pre_generator_processes",
        "transcript_sha256",
        "transcript_base64",
        "observed_processes",
    }
    if not isinstance(value, dict) or set(value) != expected_fields:
        fail(f"{context} process-boundary fields are not exact")
    if value.get("expected_generator") is not expect_generator:
        fail(f"{context} expected-generator bit is invalid")
    generator_path = value.get("generator_executable")
    if not isinstance(generator_path, str) or not Path(
        generator_path
    ).is_absolute():
        fail(f"{context} generator path is invalid")
    generator_pids = value.get("generator_pids")
    if (
        not isinstance(generator_pids, list)
        or any(
            isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0
            for pid in generator_pids
        )
        or len(set(generator_pids)) != len(generator_pids)
        or len(generator_pids) != (1 if expect_generator else 0)
    ):
        fail(f"{context} generator PID evidence is invalid")
    if value.get("unexpected_pre_generator_processes") != []:
        fail(f"{context} observed a compiler/prepare sibling process")
    if not re.fullmatch(
        r"[0-9a-f]{64}", str(value.get("transcript_sha256"))
    ):
        fail(f"{context} transcript hash is invalid")
    try:
        transcript_bytes = base64.b64decode(
            str(value.get("transcript_base64")).encode("ascii"),
            validate=True,
        )
        transcript = transcript_bytes.decode("utf-8")
    except (UnicodeEncodeError, UnicodeDecodeError, ValueError):
        fail(f"{context} transcript body is not exact UTF-8/base64")
    if sha256_bytes(transcript_bytes) != value["transcript_sha256"]:
        fail(f"{context} transcript body/hash join is invalid")
    if expect_generator:
        if generator_path not in transcript:
            fail(f"{context} transcript has no exact generator edge")
    else:
        if (
            sum(
                line.strip() == "ninja: no work to do."
                for line in transcript.splitlines()
            ) != 1
            or generator_path in transcript
        ):
            fail(
                f"{context} transcript does not prove exact Ninja no-work"
            )
    processes = value.get("observed_processes")
    if not isinstance(processes, list) or not processes:
        fail(f"{context} observed-process list is empty")
    by_pid: dict[int, dict[str, Any]] = {}
    for index, process in enumerate(processes):
        if not isinstance(process, dict) or set(process) != {
            "pid", "ppid", "commands"
        }:
            fail(f"{context} observed process {index} is malformed")
        pid = process.get("pid")
        ppid = process.get("ppid")
        commands = process.get("commands")
        if (
            isinstance(pid, bool)
            or not isinstance(pid, int)
            or pid <= 0
            or isinstance(ppid, bool)
            or not isinstance(ppid, int)
            or ppid < 0
            or not isinstance(commands, list)
            or not commands
            or commands != sorted(commands)
            or len(set(commands)) != len(commands)
            or any(
                not isinstance(command, str) or not command
                for command in commands
            )
            or pid in by_pid
        ):
            fail(f"{context} observed process {index} is invalid")
        by_pid[pid] = process
    if any(pid not in by_pid for pid in generator_pids):
        fail(f"{context} generator PID is absent from process evidence")

    def token(command_text: str) -> str:
        try:
            fields = shlex.split(command_text)
        except ValueError:
            fields = command_text.split()
        return fields[0] if fields else ""

    generator = Path(generator_path)
    for pid in generator_pids:
        if not any(
            Path(token(command)).name == generator.name
            for command in by_pid[pid]["commands"]
        ):
            fail(f"{context} generator PID has no generator command")

    def descends_from_generator(pid: int) -> bool:
        visited: set[int] = set()
        while pid > 0 and pid not in visited:
            if pid in generator_pids:
                return True
            visited.add(pid)
            record = by_pid.get(pid)
            pid = int(record["ppid"]) if record is not None else 0
        return False

    wrappers = {"time", "cmake", "ninja", "sh", "bash", "zsh"}
    for pid, process in by_pid.items():
        if descends_from_generator(pid):
            continue
        executables = {
            Path(token(command)).name
            for command in process["commands"]
            if token(command)
        }
        if executables and not executables.issubset(wrappers):
            fail(
                f"{context} has a non-generator compiler/prepare "
                f"process: {sorted(executables)}"
            )


def validate_generator_boundary(
    value: Any,
    *,
    repetitions: int,
    label: str,
) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != {
        "output_inventory_file_sha256",
        "timed_generator_artifact",
        "primed_outputs",
        "primed_dependencies",
        "post_compile_outputs",
        "post_compile_dependencies",
        "samples",
    }:
        fail(f"{label} generator-boundary fields are not exact")
    if not re.fullmatch(
        r"[0-9a-f]{64}",
        str(value.get("output_inventory_file_sha256")),
    ):
        fail(f"{label} generator-output inventory hash is invalid")
    outputs = validate_file_inventory_snapshot(
        value.get("primed_outputs"),
        context=f"{label} primed generator outputs",
    )
    dependencies = validate_file_inventory_snapshot(
        value.get("primed_dependencies"),
        context=f"{label} primed generator dependencies",
    )
    post_compile_outputs = validate_file_inventory_snapshot(
        value.get("post_compile_outputs"),
        context=f"{label} post-compile generator outputs",
    )
    post_compile_dependencies = validate_file_inventory_snapshot(
        value.get("post_compile_dependencies"),
        context=f"{label} post-compile generator dependencies",
    )
    if post_compile_outputs != outputs:
        fail(
            f"{label} affected-target rebuilds changed the exact timed "
            "generator outputs"
        )
    if post_compile_dependencies != dependencies:
        fail(
            f"{label} affected-target rebuilds changed the exact timed "
            "generator dependencies"
        )
    timed_generator = value.get("timed_generator_artifact")
    if not isinstance(timed_generator, dict) or set(timed_generator) != {
        "path", "bytes", "sha256"
    }:
        fail(f"{label} timed-generator artifact fields are not exact")
    generator_path = timed_generator.get("path")
    if (
        not isinstance(generator_path, str)
        or not Path(generator_path).is_absolute()
    ):
        fail(f"{label} timed-generator path is invalid")
    positive_integer(
        timed_generator.get("bytes"),
        f"{label} timed-generator bytes",
    )
    if not re.fullmatch(
        r"[0-9a-f]{64}", str(timed_generator.get("sha256"))
    ):
        fail(f"{label} timed-generator hash is invalid")
    output_paths = {
        entry["path"] for entry in outputs["entries"]
    }
    dependency_paths = {
        entry["path"] for entry in dependencies["entries"]
    }
    if output_paths.intersection(dependency_paths):
        fail(
            f"{label} generator outputs leaked into the primed "
            "dependency snapshot"
        )
    samples = value.get("samples")
    if not isinstance(samples, list) or len(samples) != repetitions:
        fail(f"{label} generator-boundary sample count is invalid")
    for index, sample in enumerate(samples):
        if not isinstance(sample, dict) or set(sample) != {
            "removed_output_inventory_sha256",
            "regenerated_output_inventory_sha256",
            "dependency_inventory_sha256",
            "cold_process_boundary",
            "noop_process_boundary",
        }:
            fail(
                f"{label} generator-boundary sample {index} is malformed"
            )
        if (
            sample["removed_output_inventory_sha256"]
                != outputs["inventory_sha256"]
            or sample["regenerated_output_inventory_sha256"]
                != outputs["inventory_sha256"]
            or sample["dependency_inventory_sha256"]
                != dependencies["inventory_sha256"]
        ):
            fail(
                f"{label} generator-boundary sample {index} changed "
                "outputs or primed dependencies"
            )
        validate_generator_process_boundary(
            sample["cold_process_boundary"],
            expect_generator=True,
            context=f"{label} cold generator sample {index}",
        )
        validate_generator_process_boundary(
            sample["noop_process_boundary"],
            expect_generator=False,
            context=f"{label} no-op generator sample {index}",
        )
    return {
        "path": generator_path,
        "bytes": timed_generator["bytes"],
        "sha256": timed_generator["sha256"],
    }


def validate_runtime_artifacts(
    value: Any,
    context: str,
    *,
    require_installed: bool = True,
) -> dict[str, dict[str, Any]]:
    expected_names = (
        RUNTIME_ARTIFACT_NAMES
        if require_installed
        else MEASURED_RUNTIME_ARTIFACT_NAMES
    )
    if not isinstance(value, dict) or set(value) != set(expected_names):
        fail(
            f"{context} runtime artifacts must be exactly: "
            + ", ".join(expected_names)
        )
    result: dict[str, dict[str, Any]] = {}
    seen_paths: set[str] = set()
    for name in expected_names:
        item = value[name]
        if not isinstance(item, dict):
            fail(f"{context} runtime artifact {name} is not an object")
        path = str(item.get("path", ""))
        if not path:
            fail(f"{context} runtime artifact {name} has no path")
        if path in seen_paths:
            fail(f"{context} runtime artifact paths are not distinct")
        seen_paths.add(path)
        size = positive_integer(
            item.get("bytes"), f"{context} runtime artifact {name}.bytes"
        )
        digest = str(item.get("sha256", ""))
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            fail(
                f"{context} runtime artifact {name}.sha256 is invalid"
            )
        result[name] = {
            "path": path,
            "bytes": size,
            "sha256": digest,
        }
    if require_installed:
        installed = result["installed_production_dso"]
        built = result["production_dso"]
        if (
            installed["bytes"] != built["bytes"]
            or installed["sha256"] != built["sha256"]
        ):
            fail(
                f"{context} installed production DSO does not equal "
                "the measured production DSO"
            )
    return result


def validate_measurement(
    value: dict[str, Any],
    *,
    require_installed: bool = True,
) -> None:
    expected_fields = {
        "schema_version",
        "artifact_kind",
        "role",
        "sample_policy",
        "source",
        "environment",
        "samples",
        "aggregates",
        "artifacts",
        "runtime_artifacts",
        "measurement_context",
        "content_sha256",
    }
    if set(value) != expected_fields:
        fail("measurement fields are not exact")
    if value.get("schema_version") != SCHEMA_VERSION:
        fail("measurement schema_version is invalid")
    expected_kind = (
        "measurement" if require_installed else "measurement_preinstall"
    )
    if value.get("artifact_kind") != expected_kind:
        fail(f"expected a {expected_kind} artifact")
    if value.get("role") not in {"stage0", "candidate"}:
        fail("measurement role must be stage0 or candidate")
    source = validate_source_fingerprint(
        value.get("source"), "measurement"
    )
    if value["role"] == "stage0" and source.get("dirty") is not False:
        fail("a Stage-0 measurement must record a clean source tree")
    policy = value.get("sample_policy")
    if not isinstance(policy, dict):
        fail("measurement sample_policy must be an object")
    repetitions = policy.get("repetitions")
    if (
        isinstance(repetitions, bool)
        or not isinstance(repetitions, int)
        or repetitions < MIN_SAMPLES
    ):
        fail("measurement repetitions are invalid")
    if (
        policy.get("aggregate") != "median"
        or policy.get("parallelism") != 1
        or policy.get("generator_target")
            != "bioseq_sql_final_production"
        or set(policy.get("affected_targets", ()))
            != {"postgresql_bioseq", "bioseq_bench_e2e"}
    ):
        fail("measurement sample policy is not the pinned DOC0388 policy")
    samples = value.get("samples")
    aggregates = value.get("aggregates")
    if not isinstance(samples, dict) or not isinstance(aggregates, dict):
        fail("measurement samples/aggregates must be objects")
    for metric in BUILD_SAMPLE_METRICS:
        raw = samples.get(metric)
        if not isinstance(raw, list) or len(raw) != repetitions:
            fail(f"measurement has no sample list for {metric}")
        expected = median_int(raw)
        if aggregates.get(metric) != expected:
            fail(f"measurement aggregate does not match samples: {metric}")
    artifacts = value.get("artifacts")
    if not isinstance(artifacts, dict):
        fail("measurement artifacts must be an object")
    for group in ARTIFACT_GROUPS:
        item = artifacts.get(group)
        if not isinstance(item, dict):
            fail(f"measurement has no artifact group: {group}")
        item_bytes = positive_integer(item.get("bytes"), f"{group}.bytes")
        files = positive_integer(item.get("files"), f"{group}.files")
        entries = item.get("entries")
        if not isinstance(entries, list) or len(entries) != files:
            fail(f"{group}.entries do not match the recorded file count")
        if entries != sorted(entries, key=lambda entry: str(entry.get("path"))):
            fail(f"{group}.entries are not deterministically ordered")
        digest = hashlib.sha256()
        total = 0
        for index, entry in enumerate(entries):
            if not isinstance(entry, dict):
                fail(f"{group}.entries[{index}] is not an object")
            path = str(entry.get("path", ""))
            if not path:
                fail(f"{group}.entries[{index}] has no path")
            size = positive_integer(
                entry.get("bytes"), f"{group}.entries[{index}].bytes"
            )
            content_sha = str(entry.get("sha256"))
            if not re.fullmatch(r"[0-9a-f]{64}", content_sha):
                fail(f"{group}.entries[{index}].sha256 is invalid")
            total += size
            digest.update(path.encode("utf-8"))
            digest.update(b"\0")
            digest.update(str(size).encode("ascii"))
            digest.update(b"\0")
            digest.update(content_sha.encode("ascii"))
        if total != item_bytes:
            fail(f"{group}.entries do not sum to the recorded byte count")
        if digest.hexdigest() != item.get("inventory_sha256"):
            fail(f"{group}.inventory_sha256 is invalid")
    environment = validate_environment(
        value.get("environment"), "measurement"
    )
    runtime_artifacts = validate_runtime_artifacts(
        value.get("runtime_artifacts"),
        "measurement",
        require_installed=require_installed,
    )
    boundary = value.get("measurement_context", {}).get(
        "generator_boundary", {}
    )
    boundary_samples = boundary.get("samples", [])
    if any(
        sample.get("cold_process_boundary", {}).get(
            "generator_executable"
        ) != runtime_artifacts["generator_executable"]["path"]
        or sample.get("noop_process_boundary", {}).get(
            "generator_executable"
        ) != runtime_artifacts["generator_executable"]["path"]
        for sample in boundary_samples
        if isinstance(sample, dict)
    ):
        fail(
            "measurement generator process boundary differs from the "
            "hashed generator executable"
        )
    timed_generator = validate_generator_boundary(
        boundary,
        repetitions=repetitions,
        label="measurement",
    )
    if timed_generator != runtime_artifacts["generator_executable"]:
        fail(
            "measurement retained generator differs from the exact binary "
            "whose time and RSS were measured"
        )
    summarize_measurement_context(
        value.get("measurement_context"),
        environment=environment,
        repetitions=repetitions,
        label="measurement",
    )
    validate_content_seal(value, "measurement")


def validate_runtime_samples(value: dict[str, Any]) -> None:
    expected_fields = {
        "schema_version",
        "artifact_kind",
        "sample_unit",
        "measurement_sha256",
        "measurement_content_sha256",
        "source",
        "environment",
        "provenance",
        "samples",
        "content_sha256",
    }
    if set(value) != expected_fields:
        fail("runtime-samples fields are not exact")
    if value.get("schema_version") != SCHEMA_VERSION:
        fail("runtime-samples schema_version is invalid")
    if value.get("artifact_kind") != "runtime_samples":
        fail("expected a runtime_samples artifact")
    if value.get("sample_unit") != "per_report_p50":
        fail("runtime samples must use per_report_p50 sample units")
    for field in (
        "measurement_sha256",
        "measurement_content_sha256",
    ):
        if not re.fullmatch(r"[0-9a-f]{64}", str(value.get(field))):
            fail(f"runtime samples {field} is invalid")
    validate_source_fingerprint(
        value.get("source"), "runtime samples"
    )
    validate_environment(
        value.get("environment"), "runtime samples"
    )
    provenance = value.get("provenance")
    if not isinstance(provenance, dict):
        fail("runtime samples have no provenance")
    if provenance.get("kind") != "bioseq_benchmark_reports":
        fail("runtime samples must come from bioseq benchmark reports")
    reports = provenance.get("reports")
    if not isinstance(reports, list) or len(reports) < MIN_SAMPLES:
        fail(
            f"runtime samples require at least {MIN_SAMPLES} report records"
        )
    for index, report in enumerate(reports):
        if (
            not isinstance(report, dict)
            or set(report) != {
                "path",
                "sha256",
                "meta",
                "run_nonce",
                "host_state",
                "paired_runtime_samples",
            }
        ):
            fail(f"runtime report provenance[{index}] is not an object")
        if not str(report.get("path", "")).strip():
            fail(f"runtime report provenance[{index}] has no path")
        if not re.fullmatch(r"[0-9a-f]{64}", str(report.get("sha256"))):
            fail(f"runtime report provenance[{index}] has no valid sha256")
        positive_integer(
            report.get("run_nonce"),
            f"runtime report provenance[{index}].run_nonce",
        )
        host_state = report.get("host_state")
        if (
            not isinstance(host_state, dict)
            or set(host_state) != {
                "before_file_sha256",
                "before_content_sha256",
                "before",
                "after_file_sha256",
                "after_content_sha256",
                "after",
            }
        ):
            fail(
                f"runtime report provenance[{index}] host state is malformed"
            )
        for field in (
            "before_file_sha256",
            "before_content_sha256",
            "after_file_sha256",
            "after_content_sha256",
        ):
            if not re.fullmatch(
                r"[0-9a-f]{64}", str(host_state.get(field))
            ):
                fail(
                    f"runtime report provenance[{index}] {field} is invalid"
                )
        paired = report.get("paired_runtime_samples")
        if (
            not isinstance(paired, dict)
            or set(paired) != {
                "file_sha256",
                "content_sha256",
            }
            or any(
                not re.fullmatch(
                    r"[0-9a-f]{64}", str(paired.get(field))
                )
                for field in ("file_sha256", "content_sha256")
            )
        ):
            fail(
                f"runtime report provenance[{index}] paired samples "
                "are malformed"
            )
    run_nonces = [report["run_nonce"] for report in reports]
    if len(set(run_nonces)) != len(run_nonces):
        fail("runtime reports do not have independent run nonces")
    samples_value = value.get("samples")
    if not isinstance(samples_value, dict):
        fail("runtime samples payload must be an object")
    if set(samples_value) != set(RUNTIME_SAMPLE_METRICS):
        fail(
            "runtime sample metrics must be exactly: "
            + ", ".join(RUNTIME_SAMPLE_METRICS)
        )
    for metric, samples in samples_value.items():
        if not isinstance(samples, list) or len(samples) != len(reports):
            fail(f"{metric} requires at least {MIN_SAMPLES} samples")
        for index, sample in enumerate(samples):
            positive_number(sample, f"{metric}[{index}]")
    summarize_runtime_context(
        reports,
        environment=value["environment"],
        label="runtime samples",
    )
    validate_content_seal(value, "runtime samples")


def summarize_runtime_context(
    reports: Any,
    *,
    environment: dict[str, Any],
    label: str,
    minimum_reports: int = MIN_SAMPLES,
) -> dict[str, Any]:
    if not isinstance(reports, list) or len(reports) < minimum_reports:
        fail(f"{label} has too few runtime report contexts")
    normalized_load_values: list[float] = []
    available_memory_values: list[int] = []
    governor_states: set[str] = set()
    previous_capture: int | None = None
    for index, report in enumerate(reports):
        if not isinstance(report, dict):
            fail(f"{label} report {index} is malformed")
        host_state = report.get("host_state")
        if not isinstance(host_state, dict):
            fail(f"{label} report {index} has no host state")
        captures = []
        for boundary in ("before", "after"):
            checked = validate_host_state_snapshot(
                host_state.get(boundary),
                environment=environment,
                context=f"{label} report {index} {boundary}",
            )
            captured = checked["captured_monotonic_ns"]
            if previous_capture is not None and captured <= previous_capture:
                fail(f"{label} host-state captures are not monotonic")
            previous_capture = captured
            captures.append(captured)
            normalized_load_values.append(
                checked["normalized_load_1"])
            available_memory_values.append(
                checked["available_memory_bytes"])
            governor_states.update(checked["cpu_governor_states"])
        if captures[1] <= captures[0]:
            fail(f"{label} report {index} boundaries are not ordered")
    return {
        "normalized_load_1_max": max(normalized_load_values),
        "available_memory_bytes_min": min(available_memory_values),
        "cpu_governor_states": sorted(governor_states),
    }


def validate_runtime_context_limits(
    summary: dict[str, Any],
    *,
    baselines: dict[str, float],
    limits: dict[str, Any],
    label: str,
) -> None:
    load_value = nonnegative_number(
        summary.get("normalized_load_1_max"),
        f"{label} normalized load",
    )
    memory_value = positive_number(
        summary.get("available_memory_bytes_min"),
        f"{label} available memory",
    )
    governors = set(validate_cpu_governor_states(
        summary.get("cpu_governor_states"),
        f"{label} CPU governor states",
    ))
    baseline_load = nonnegative_number(
        baselines["measurement_normalized_load_1_max"],
        f"{label} baseline normalized load",
    )
    baseline_memory = positive_number(
        baselines["measurement_available_memory_bytes_min"],
        f"{label} baseline available memory",
    )
    failures = []
    if load_value > float(
        limits["measurement_normalized_load_1_max"]
    ):
        failures.append("absolute normalized load")
    if load_value - baseline_load > float(
        limits["measurement_normalized_load_1_delta_max"]
    ):
        failures.append("normalized-load delta")
    if memory_value < float(
        limits["measurement_available_memory_bytes_min"]
    ):
        failures.append("absolute available memory")
    memory_drop = (
        (baseline_memory - memory_value) / baseline_memory * 100.0
    )
    if memory_drop > float(
        limits["measurement_available_memory_drop_percent_max"]
    ):
        failures.append("available-memory drop")
    if not governors.issubset(
        set(limits["measurement_cpu_governor_states"])
    ):
        failures.append("CPU governor state")
    if failures:
        fail(
            f"{label} violates reviewed host-state limits: "
            + ", ".join(failures)
        )


def report_metric_samples(
    metrics: dict[str, Any],
    metric: str,
    *,
    expected_samples: int = 15,
) -> list[float]:
    """Losslessly reconstruct one exact 15-sample report cell.

    The framework persists the ten smallest values ascending and the ten
    largest descending.  For exactly fifteen finite samples those arrays
    overlap at sorted ranks 6..10, so together they retain the complete
    raw multiset.  Recompute every aggregate from that multiset instead of
    trusting framework-owned percentile or pass/fail fields.
    """
    if expected_samples != 15:
        fail(
            "SQL guardrail raw report reconstruction requires exactly "
            "15 samples"
        )
    value = metrics.get(metric)
    expected_fields = {
        "samples_count",
        "stats",
        "percentiles",
        "k-best",
        "histogram",
    }
    if not isinstance(value, dict) or set(value) != expected_fields:
        fail(f"benchmark report has no metric: {metric}")
    samples_count = value.get("samples_count")
    if (
        isinstance(samples_count, bool)
        or not isinstance(samples_count, int)
        or samples_count != expected_samples
    ):
        fail(
            f"benchmark report metric {metric} has {samples_count} "
            f"samples; expected exactly {expected_samples}"
        )
    k_best = value.get("k-best")
    if not isinstance(k_best, dict) or set(k_best) != {
        "top10", "bottom10"
    }:
        fail(f"benchmark report metric {metric} has malformed k-best data")
    top_raw = k_best.get("top10")
    bottom_raw = k_best.get("bottom10")
    if (
        not isinstance(top_raw, list)
        or not isinstance(bottom_raw, list)
        or len(top_raw) != 10
        or len(bottom_raw) != 10
    ):
        fail(
            f"benchmark report metric {metric} does not retain the exact "
            "top/bottom ten values"
        )
    top = [
        finite_number(item, f"{metric}.k-best.top10[{index}]")
        for index, item in enumerate(top_raw)
    ]
    bottom = [
        finite_number(item, f"{metric}.k-best.bottom10[{index}]")
        for index, item in enumerate(bottom_raw)
    ]
    if any(top[index] < top[index + 1] for index in range(9)):
        fail(f"benchmark report metric {metric} top10 is not descending")
    if any(bottom[index] > bottom[index + 1] for index in range(9)):
        fail(f"benchmark report metric {metric} bottom10 is not ascending")
    upper_ascending = list(reversed(top))
    if bottom[5:] != upper_ascending[:5]:
        fail(
            f"benchmark report metric {metric} top/bottom overlap does "
            "not reconstruct one 15-sample multiset"
        )
    samples = [*bottom[:5], *upper_ascending]
    if (
        len(samples) != expected_samples
        or any(
            samples[index] > samples[index + 1]
            for index in range(expected_samples - 1)
        )
    ):
        fail(
            f"benchmark report metric {metric} reconstructed samples "
            "are malformed"
        )

    def require_close(
        actual: Any,
        expected: float,
        context: str,
    ) -> None:
        checked = finite_number(actual, context)
        if not math.isclose(
            checked, expected, rel_tol=1e-12, abs_tol=1e-9
        ):
            fail(
                f"benchmark report metric {metric} {context}={checked} "
                f"does not recompute to {expected}"
            )

    total = 0.0
    for sample in samples:
        total += sample
    mean = total / expected_samples
    trim = int(math.floor(0.1 * expected_samples))
    trimmed_total = 0.0
    for sample in samples[trim:expected_samples - trim]:
        trimmed_total += sample
    mean90 = trimmed_total / (expected_samples - 2 * trim)
    squared = 0.0
    for sample in samples:
        delta = sample - mean
        squared += delta * delta
    stddev = math.sqrt(squared / (expected_samples - 1))
    stats = value.get("stats")
    if not isinstance(stats, dict) or set(stats) != {
        "min", "max", "mean", "mean90", "sum", "stddev"
    }:
        fail(f"benchmark report metric {metric} has malformed stats")
    for field, recomputed in (
        ("min", samples[0]),
        ("max", samples[-1]),
        ("mean", mean),
        ("mean90", mean90),
        ("sum", total),
        ("stddev", stddev),
    ):
        require_close(
            stats.get(field), recomputed, f"stats.{field}"
        )

    percentiles = value.get("percentiles")
    percentile_fractions = {
        "P01": 0.01,
        "P20": 0.20,
        "P50": 0.50,
        "P80": 0.80,
        "P99": 0.99,
    }
    if (
        not isinstance(percentiles, dict)
        or set(percentiles) != set(percentile_fractions)
    ):
        fail(f"benchmark report metric {metric} has malformed percentiles")
    for name, fraction in percentile_fractions.items():
        index = int(math.floor(fraction * (expected_samples - 1)))
        require_close(
            percentiles.get(name),
            samples[index],
            f"percentiles.{name}",
        )

    histogram = value.get("histogram")
    if not isinstance(histogram, list) or len(histogram) != 10:
        fail(f"benchmark report metric {metric} has malformed histogram")
    width = (samples[-1] - samples[0]) / 10.0
    if not math.isfinite(width):
        fail(f"benchmark report metric {metric} histogram width is invalid")
    expected_counts = [0] * 10
    if width > 0.0:
        for sample in samples:
            bucket = int(math.floor((sample - samples[0]) / width))
            expected_counts[min(bucket, 9)] += 1
    else:
        expected_counts[0] = expected_samples
    observed_count = 0
    for index, bucket in enumerate(histogram):
        if not isinstance(bucket, dict) or set(bucket) != {
            "from", "to", "count"
        }:
            fail(
                f"benchmark report metric {metric} histogram bucket "
                f"{index} is malformed"
            )
        count = bucket.get("count")
        if (
            isinstance(count, bool)
            or not isinstance(count, int)
            or count < 0
            or count != expected_counts[index]
        ):
            fail(
                f"benchmark report metric {metric} histogram bucket "
                f"{index} count does not recompute"
            )
        observed_count += count
        expected_from = samples[0] + width * index
        expected_to = (
            samples[-1]
            if index == 9
            else samples[0] + width * (index + 1)
        )
        require_close(
            bucket.get("from"),
            expected_from,
            f"histogram[{index}].from",
        )
        require_close(
            bucket.get("to"),
            expected_to,
            f"histogram[{index}].to",
        )
    if observed_count != expected_samples:
        fail(
            f"benchmark report metric {metric} histogram does not prove "
            "15 finite samples"
        )
    return samples


def report_metric_p50(
    metrics: dict[str, Any],
    metric: str,
    *,
    positive: bool,
    expected_samples: int = 15,
) -> float:
    samples = report_metric_samples(
        metrics, metric, expected_samples=expected_samples
    )
    result = samples[expected_samples // 2]
    if positive:
        return positive_number(result, f"{metric}.recomputed_P50")
    return finite_number(result, f"{metric}.recomputed_P50")


def require_report_metric_constant(
    metrics: dict[str, Any],
    metric: str,
    expected: float,
    *,
    context: str,
) -> None:
    samples = report_metric_samples(metrics, metric)
    if any(sample != expected for sample in samples):
        fail(
            f"{context} {metric} does not equal {expected} in every "
            "raw iteration"
        )


def require_report_nonce(
    metrics: dict[str, Any],
    expected: int,
    *,
    context: str,
) -> None:
    """Require an exactly represented, iteration-constant workflow nonce."""
    samples = report_metric_samples(metrics, "run_nonce")
    for index, sample in enumerate(samples):
        if (
            not math.isfinite(sample)
            or sample < 0.0
            or sample >= float(1 << 53)
            or not sample.is_integer()
            or int(sample) != expected
        ):
            fail(
                f"{context} run_nonce[{index}] is not the exact workflow "
                f"nonce {expected}"
            )


def extract_report_runtime(
    path: Path,
    measurement: dict[str, Any],
) -> tuple[dict[str, float], dict[str, Any]]:
    report = load_json(path)
    rule_violations = report.get("rule_violations")
    if not isinstance(rule_violations, list):
        fail(f"benchmark report has no rule_violations list: {path}")
    if rule_violations:
        fail(
            "calibration report contains benchmark rule violations: "
            f"{path}"
        )
    regressions = report.get("regressions")
    if not isinstance(regressions, list) or regressions:
        fail(
            "SQL guardrail report has regressions or no regressions list: "
            f"{path}"
        )
    sanity_warnings = report.get("sanity_warnings")
    if not isinstance(sanity_warnings, list) or sanity_warnings:
        fail(
            "SQL guardrail report has sanity warnings or no warning list: "
            f"{path}"
        )
    meta = report.get("meta")
    if not isinstance(meta, dict):
        fail(f"benchmark report has no meta object: {path}")
    if meta.get("history_eligible") is not True:
        fail(f"benchmark report is not history-eligible: {path}")
    if meta.get("commit") != measurement["source"]["head"]:
        fail(
            f"benchmark report commit does not match its measurement: {path}"
        )
    benchmarks = report.get("benchmarks")
    if not isinstance(benchmarks, dict):
        fail(f"benchmark report has no benchmarks object: {path}")
    benchmark = benchmarks.get("bench_sql_codegen_guardrail")
    if not isinstance(benchmark, dict):
        fail(
            "benchmark report does not contain "
            f"bench_sql_codegen_guardrail: {path}"
        )
    tuples = benchmark.get("execution_tuples")
    if not isinstance(tuples, list):
        fail(f"benchmark report has no execution tuples: {path}")

    expected_algorithms = set(BENCHMARK_ALGORITHMS)
    metrics_by_algorithm: dict[str, dict[str, Any]] = {}
    for item in tuples:
        if not isinstance(item, dict):
            fail(f"benchmark report execution tuple is malformed: {path}")
        tuple_value = item.get("tuple")
        if not isinstance(tuple_value, dict):
            fail(f"benchmark report tuple selector is malformed: {path}")
        matches = expected_algorithms.intersection(tuple_value)
        if len(matches) != 1:
            fail(
                "benchmark report tuple must identify exactly one SQL "
                f"guardrail algorithm: {path}"
            )
        algorithm = next(iter(matches))
        if algorithm in metrics_by_algorithm:
            fail(
                f"benchmark report duplicates algorithm {algorithm}: {path}"
            )
        test_cases = item.get("test_cases")
        if not isinstance(test_cases, dict):
            fail(f"benchmark report tuple has no test cases: {path}")
        test_case = test_cases.get("SQL_CODEGEN_BOUNDARY_SMALL")
        if not isinstance(test_case, dict):
            fail(
                "benchmark report tuple has no "
                f"SQL_CODEGEN_BOUNDARY_SMALL case: {path}"
            )
        all_regions = test_case.get("all_regions")
        if not isinstance(all_regions, dict):
            fail(f"benchmark report tuple has no all_regions metrics: {path}")
        if set(all_regions) != set(BENCHMARK_METRICS):
            fail(
                "benchmark report all_regions metric set is not exact: "
                f"{path}:{algorithm}"
            )
        for metric in BENCHMARK_METRICS:
            report_metric_p50(
                all_regions,
                metric,
                positive=False,
                expected_samples=15,
            )
        metrics_by_algorithm[algorithm] = all_regions
    if set(metrics_by_algorithm) != expected_algorithms:
        missing = sorted(expected_algorithms - set(metrics_by_algorithm))
        extra = sorted(set(metrics_by_algorithm) - expected_algorithms)
        fail(
            "benchmark report SQL guardrail matrix is not exact; "
            f"missing={missing}, extra={extra}: {path}"
        )
    exact_metrics = exact_sql_guardrail_report_metrics(
        path,
        expected_source_head=measurement["source"]["head"],
    )
    if exact_metrics != metrics_by_algorithm:
        fail(
            f"runtime calibration report parser views disagree: {path}"
        )
    metrics_by_algorithm = exact_metrics

    # Baseline recovery is an explicitly non-qualifying calibration run.  An
    # ordinary passing qualification report must never be recycled as Stage-0.
    for algorithm, metrics in metrics_by_algorithm.items():
        expected_arity = EXPECTED_ALGORITHM_ARITIES[algorithm]
        context = f"runtime calibration report {path} {algorithm}"
        require_report_metric_constant(
            metrics, "arity", expected_arity, context=context
        )
        require_report_metric_constant(
            metrics,
            "qualification_baseline_kind",
            0.0,
            context=context,
        )
        require_report_metric_constant(
            metrics,
            "build_qualification_passed",
            0.0,
            context=context,
        )
        expected_attestation = (
            0.0 if algorithm == "qualification_reference" else 1.0
        )
        require_report_metric_constant(
            metrics,
            "live_measurement_attested",
            expected_attestation,
            context=context,
        )
        require_report_metric_constant(
            metrics,
            "correctness_passed",
            0.0 if algorithm == "qualification_reference" else 1.0,
            context=context,
        )
        require_report_metric_constant(
            metrics,
            "performance_gate_passed",
            0.0 if algorithm == "qualification_reference" else 1.0,
            context=context,
        )
        wrapper = algorithm in {
            "from_array_arity_2",
            "from_array_arity_3",
            "typed_wrapper_generated_vs_handwritten",
        }
        custom_scan = algorithm in {
            "mmsearch_arity_2",
            "mmsearch_arity_3",
        }
        for metric, expected in (
            ("generated_typed_wrapper", 1.0 if wrapper else 0.0),
            ("direct_fmgr_wrapper_path", 1.0 if wrapper else 0.0),
            (
                "planner_custom_scan_path",
                1.0 if custom_scan else 0.0,
            ),
        ):
            require_report_metric_constant(
                metrics, metric, expected, context=context
            )
        codec_samples = report_metric_samples(
            metrics, "generated_row_codec_fields"
        )
        codec_value = codec_samples[0]
        if any(value != codec_value for value in codec_samples):
            fail(f"{context} row-codec field count is not constant")
        expected_codec_fields = EXPECTED_ROW_CODEC_FIELD_COUNTS.get(
            algorithm,
            0.0,
        )
        if codec_value != expected_codec_fields:
            fail(
                f"{context} row-codec field count is "
                f"{codec_value}, expected {expected_codec_fields}"
            )
        wall_clock_samples = report_metric_samples(
            metrics, "wall_clock_ns"
        )
        generated_samples = report_metric_samples(
            metrics, "generated_query_ns"
        )
        query_samples = report_metric_samples(metrics, "query_ns")
        handwritten_samples = report_metric_samples(
            metrics, "handwritten_query_ns"
        )
        ratio_samples = report_metric_samples(
            metrics, "generated_to_handwritten_ratio"
        )
        if any(value <= 0.0 for value in wall_clock_samples):
            fail(f"{context} has a nonpositive wall clock")
        if algorithm == "qualification_reference":
            if any(
                value != 0.0
                for value in (
                    *generated_samples,
                    *query_samples,
                    *handwritten_samples,
                    *ratio_samples,
                )
            ):
                fail(f"{context} reference has query evidence")
        else:
            if (
                any(value <= 0.0 for value in generated_samples)
                or any(value <= 0.0 for value in query_samples)
                or generated_samples != query_samples
            ):
                fail(f"{context} generated/query timings are invalid")
            comparator = algorithm in {
                "typed_wrapper_generated_vs_handwritten",
                "row_codec_generated_vs_handwritten",
            }
            if comparator:
                if (
                    any(value <= 0.0 for value in handwritten_samples)
                    or any(value <= 0.0 for value in ratio_samples)
                ):
                    fail(f"{context} comparator timings are invalid")
            elif any(
                value != 0.0
                for value in (*handwritten_samples, *ratio_samples)
            ):
                fail(f"{context} has unexpected comparator evidence")
    live_nonces = {
        positive_integer(
            report_metric_samples(
                metrics_by_algorithm[algorithm], "run_nonce"
            )[0],
            f"{path}:{algorithm}:run_nonce",
        )
        for algorithm in BENCHMARK_ALGORITHMS
        if algorithm != "qualification_reference"
    }
    for algorithm in BENCHMARK_ALGORITHMS:
        if algorithm == "qualification_reference":
            continue
        algorithm_nonce = positive_integer(
            report_metric_samples(
                metrics_by_algorithm[algorithm], "run_nonce"
            )[0],
            f"{path}:{algorithm}:run_nonce",
        )
        require_report_nonce(
            metrics_by_algorithm[algorithm],
            algorithm_nonce,
            context=f"runtime calibration report {path} {algorithm}",
        )
    if len(live_nonces) != 1:
        fail(f"runtime calibration report has inconsistent run nonce: {path}")
    require_report_nonce(
        metrics_by_algorithm["qualification_reference"],
        0,
        context=f"runtime calibration report {path} reference",
    )
    run_nonce = next(iter(live_nonces))

    values = {
        "from_array_arity_2_ns": report_metric_p50(
            metrics_by_algorithm["from_array_arity_2"],
            "generated_query_ns",
            positive=True,
        ),
        "from_array_arity_3_ns": report_metric_p50(
            metrics_by_algorithm["from_array_arity_3"],
            "generated_query_ns",
            positive=True,
        ),
        "typed_wrapper_generated_to_handwritten_ratio":
            report_metric_p50(
                metrics_by_algorithm[
                    "typed_wrapper_generated_vs_handwritten"
                ],
                "generated_to_handwritten_ratio",
                positive=True,
            ),
        "mmsearch_arity_2_ns": report_metric_p50(
            metrics_by_algorithm["mmsearch_arity_2"],
            "generated_query_ns",
            positive=True,
        ),
        "mmsearch_arity_3_ns": report_metric_p50(
            metrics_by_algorithm["mmsearch_arity_3"],
            "generated_query_ns",
            positive=True,
        ),
        "row_codec_generated_to_handwritten_ratio":
            report_metric_p50(
                metrics_by_algorithm[
                    "row_codec_generated_vs_handwritten"
                ],
                "generated_to_handwritten_ratio",
                positive=True,
            ),
    }
    provenance = {
        "path": str(path.resolve()),
        "sha256": sha256_file(path),
        "meta": meta,
        "run_nonce": run_nonce,
    }
    return values, provenance


def command_extract_runtime_samples(arguments: argparse.Namespace) -> None:
    if len(arguments.report) < MIN_SAMPLES:
        fail(f"at least {MIN_SAMPLES} independent reports are required")
    if not (
        len(arguments.host_state_before)
        == len(arguments.report)
        == len(arguments.host_state_after)
        == len(arguments.paired_runtime_samples)
    ):
        fail(
            "every runtime report requires one before/after host-state "
            "artifact and one paired runtime-sample artifact"
        )
    measurement = load_json(arguments.measurement)
    validate_measurement(measurement)
    samples = {metric: [] for metric in RUNTIME_SAMPLE_METRICS}
    provenance = []
    seen_hashes: set[str] = set()
    measurement_sha256 = sha256_file(arguments.measurement)
    for report_path, before_path, after_path, paired_path in zip(
        arguments.report,
        arguments.host_state_before,
        arguments.host_state_after,
        arguments.paired_runtime_samples,
        strict=True,
    ):
        values, report_provenance = extract_report_runtime(
            report_path, measurement
        )
        report_hash = report_provenance["sha256"]
        if report_hash in seen_hashes:
            fail(f"duplicate benchmark report content: {report_path}")
        seen_hashes.add(report_hash)
        before = load_json(before_path)
        after = load_json(after_path)
        validate_runtime_host_state_artifact(
            before,
            measurement=measurement,
            expected_phase="before",
        )
        validate_runtime_host_state_artifact(
            after,
            measurement=measurement,
            expected_phase="after",
        )
        if (
            before["run_nonce"] != report_provenance["run_nonce"]
            or after["run_nonce"] != report_provenance["run_nonce"]
        ):
            fail(
                "runtime host-state artifacts are not bound to the "
                "benchmark report run nonce"
            )
        for document, state_path in (
            (before, before_path),
            (after, after_path),
        ):
            if document["measurement_sha256"] != measurement_sha256:
                fail(
                    "runtime host-state artifact is not bound to the "
                    "measurement file"
                )
        before_snapshot = before["snapshot"]
        after_snapshot = after["snapshot"]
        if (
            before_snapshot["captured_monotonic_ns"]
            >= after_snapshot["captured_monotonic_ns"]
        ):
            fail("runtime report host-state boundaries are not ordered")
        report_provenance["host_state"] = {
            "before_file_sha256": sha256_file(before_path),
            "before_content_sha256": before["content_sha256"],
            "before": before_snapshot,
            "after_file_sha256": sha256_file(after_path),
            "after_content_sha256": after["content_sha256"],
            "after": after_snapshot,
        }
        paired = load_json(paired_path)
        validate_paired_runtime_samples(
            paired,
            measurement=measurement,
            measurement_sha256=measurement_sha256,
            report_path=report_path,
            expected_run_nonce=report_provenance["run_nonce"],
        )
        report_provenance["paired_runtime_samples"] = {
            "file_sha256": sha256_file(paired_path),
            "content_sha256": paired["content_sha256"],
        }
        values[
            "typed_wrapper_generated_to_handwritten_ratio"
        ] = float(statistics.median(
            paired_runtime_ratios(
                paired,
                "typed_wrapper_generated_vs_handwritten",
            )
        ))
        values[
            "row_codec_generated_to_handwritten_ratio"
        ] = float(statistics.median(
            paired_runtime_ratios(
                paired,
                "row_codec_generated_vs_handwritten",
            )
        ))
        provenance.append(report_provenance)
        for metric, value in values.items():
            samples[metric].append(value)
    result = {
        "schema_version": SCHEMA_VERSION,
        "artifact_kind": "runtime_samples",
        "sample_unit": "per_report_p50",
        "measurement_sha256": measurement_sha256,
        "measurement_content_sha256": measurement["content_sha256"],
        "source": measurement["source"],
        "environment": measurement["environment"],
        "provenance": {
            "kind": "bioseq_benchmark_reports",
            "reports": provenance,
        },
        "samples": samples,
    }
    result["content_sha256"] = canonical_json_sha256(result)
    validate_runtime_samples(result)
    write_json(arguments.output, result)


def metric_baselines(
    measurement: dict[str, Any],
    runtime_samples: dict[str, Any],
) -> dict[str, float]:
    result = {
        metric: float(measurement["aggregates"][metric])
        for metric in BUILD_SAMPLE_METRICS
    }
    for group in ARTIFACT_GROUPS:
        result[f"generated_{group}_bytes"] = float(
            measurement["artifacts"][group]["bytes"]
        )
    for metric, samples in runtime_samples.items():
        result[metric] = float(statistics.median(samples))
    context_summary = summarize_measurement_context(
        measurement["measurement_context"],
        environment=measurement["environment"],
        repetitions=measurement["sample_policy"]["repetitions"],
        label="baseline",
    )
    result["measurement_normalized_load_1_max"] = float(
        context_summary["normalized_load_1_max"]
    )
    result["measurement_available_memory_bytes_min"] = float(
        context_summary["available_memory_bytes_min"]
    )
    return result


def validate_limits(
    limits: dict[str, Any], baselines: dict[str, float]
) -> dict[str, Any]:
    if set(limits) != set(ALL_LIMIT_METRICS):
        missing = sorted(set(ALL_LIMIT_METRICS) - set(limits))
        extra = sorted(set(limits) - set(ALL_LIMIT_METRICS))
        fail(f"limits mismatch; missing={missing}, extra={extra}")
    if set(baselines) != set(BASELINE_METRICS):
        missing = sorted(set(BASELINE_METRICS) - set(baselines))
        extra = sorted(set(baselines) - set(BASELINE_METRICS))
        fail(f"baselines mismatch; missing={missing}, extra={extra}")
    result: dict[str, Any] = {}
    for metric, value in limits.items():
        if metric == "measurement_cpu_governor_states":
            result[metric] = validate_cpu_governor_states(
                value, f"limit {metric}"
            )
        elif metric in COMPILE_DELTA_METRICS or metric in {
            "measurement_normalized_load_1_max",
            "measurement_normalized_load_1_delta_max",
            "measurement_available_memory_drop_percent_max",
        }:
            result[metric] = nonnegative_number(
                value, f"limit {metric}"
            )
        elif metric == "measurement_available_memory_bytes_min":
            result[metric] = float(positive_integer(
                value, f"limit {metric}"
            ))
        elif metric.endswith("_ratio"):
            result[metric] = positive_number(
                value, f"limit {metric}"
            )
        else:
            result[metric] = float(positive_integer(
                value, f"limit {metric}"
            ))
    for metric in DIRECT_LIMIT_METRICS:
        baseline = positive_number(
            baselines[metric], f"baseline {metric}"
        )
        if result[metric] < baseline:
            fail(
                f"limit {metric}={result[metric]} is below its measured "
                f"baseline {baseline}"
            )
    baseline_load = nonnegative_number(
        baselines["measurement_normalized_load_1_max"],
        "baseline measurement_normalized_load_1_max",
    )
    if result["measurement_normalized_load_1_max"] < baseline_load:
        fail(
            "limit measurement_normalized_load_1_max is below the "
            "measured baseline maximum"
        )
    baseline_memory = positive_number(
        baselines["measurement_available_memory_bytes_min"],
        "baseline measurement_available_memory_bytes_min",
    )
    if (
        result["measurement_available_memory_bytes_min"]
        > baseline_memory
    ):
        fail(
            "limit measurement_available_memory_bytes_min is above the "
            "measured baseline minimum"
        )
    return result


def validate_baselines(
    value: Any,
    context: str,
) -> dict[str, float]:
    if not isinstance(value, dict) or set(value) != set(BASELINE_METRICS):
        missing = sorted(
            set(BASELINE_METRICS) - set(value)
        ) if isinstance(value, dict) else list(BASELINE_METRICS)
        extra = sorted(
            set(value) - set(BASELINE_METRICS)
        ) if isinstance(value, dict) else []
        fail(f"{context} mismatch; missing={missing}, extra={extra}")
    result: dict[str, float] = {}
    for metric in BASELINE_METRICS:
        if metric == "measurement_normalized_load_1_max":
            result[metric] = nonnegative_number(
                value[metric], f"{context} {metric}"
            )
        else:
            result[metric] = positive_number(
                value[metric], f"{context} {metric}"
            )
    return result


def validate_limits_document(
    document: Any,
    *,
    measurement: dict[str, Any],
    measurement_sha256: str,
    runtime_samples: dict[str, Any],
    runtime_samples_sha256: str,
    baselines: dict[str, float],
) -> dict[str, Any]:
    expected_fields = {
        "schema_version",
        "artifact_kind",
        "canonical_environment_key",
        "measurement_sha256",
        "measurement_content_sha256",
        "runtime_samples_sha256",
        "runtime_samples_content_sha256",
        "review_notes",
        "limits",
        "content_sha256",
    }
    if not isinstance(document, dict) or set(document) != expected_fields:
        fail("limits document fields are not exact")
    if (
        document.get("schema_version") != SCHEMA_VERSION
        or document.get("artifact_kind")
            != "doc0388_sql_codegen_guardrail_limits"
    ):
        fail("limits document schema/kind is invalid")
    validate_content_seal(document, "limits document")
    expected_bindings = {
        "canonical_environment_key":
            measurement["environment"]["sha256"],
        "measurement_sha256": measurement_sha256,
        "measurement_content_sha256": measurement["content_sha256"],
        "runtime_samples_sha256": runtime_samples_sha256,
        "runtime_samples_content_sha256":
            runtime_samples["content_sha256"],
    }
    for field, expected in expected_bindings.items():
        if document.get(field) != expected:
            fail(
                f"limits document {field} is not bound to the captured "
                "measurement/runtime evidence"
            )
    if not str(document.get("review_notes", "")).strip():
        fail("limits document review_notes must be nonempty")
    limits_value = document.get("limits")
    if not isinstance(limits_value, dict):
        fail("limits file must contain an object named 'limits'")
    return validate_limits(limits_value, baselines)


def command_review(arguments: argparse.Namespace) -> None:
    measurement = load_json(arguments.measurement)
    validate_measurement(measurement)
    runtime_document = load_json(arguments.runtime_samples)
    validate_runtime_samples(runtime_document)
    measurement_file_sha256 = sha256_file(arguments.measurement)
    if (
        runtime_document["measurement_sha256"]
        != measurement_file_sha256
        or runtime_document["measurement_content_sha256"]
        != measurement["content_sha256"]
        or runtime_document["source"] != measurement["source"]
        or runtime_document["environment"] != measurement["environment"]
    ):
        fail(
            "runtime samples are not bound to the reviewed measurement"
        )
    runtime = runtime_document["samples"]
    baselines = metric_baselines(measurement, runtime)
    limits_document = load_json(arguments.limits)
    limits = validate_limits_document(
        limits_document,
        measurement=measurement,
        measurement_sha256=measurement_file_sha256,
        runtime_samples=runtime_document,
        runtime_samples_sha256=sha256_file(arguments.runtime_samples),
        baselines=baselines,
    )
    context_summary = summarize_measurement_context(
        measurement["measurement_context"],
        environment=measurement["environment"],
        repetitions=measurement["sample_policy"]["repetitions"],
        label="reviewed baseline",
    )
    if not set(context_summary["cpu_governor_states"]).issubset(
        limits["measurement_cpu_governor_states"]
    ):
        fail(
            "reviewed governor-state allowlist excludes a state observed "
            "during baseline measurement"
        )
    runtime_context_summary = summarize_runtime_context(
        runtime_document["provenance"]["reports"],
        environment=measurement["environment"],
        label="reviewed runtime baseline",
    )
    validate_runtime_context_limits(
        runtime_context_summary,
        baselines=baselines,
        limits=limits,
        label="reviewed runtime baseline",
    )

    if arguments.baseline_kind == "stage0":
        if measurement["role"] != "stage0":
            fail("a stage0 baseline requires a role=stage0 measurement")
        if any(
            (
                arguments.spec_deviation,
                arguments.approval_reference,
            )
        ):
            fail("stage0 review must not carry a recovery deviation")
        deviation: dict[str, str] | None = None
    else:
        if measurement["role"] != "candidate":
            fail(
                "recovered_post_cutover requires a role=candidate "
                "measurement"
            )
        if not arguments.spec_deviation:
            fail(
                "recovered_post_cutover requires --spec-deviation"
            )
        if not arguments.approval_reference:
            fail(
                "recovered_post_cutover requires --approval-reference"
            )
        deviation = {
            "reason": arguments.spec_deviation,
            "approval_reference": arguments.approval_reference,
        }

    result = {
        "schema_version": SCHEMA_VERSION,
        "artifact_kind": "reviewed_baseline",
        "status": "reviewed",
        "baseline_kind": arguments.baseline_kind,
        "review": {
            "reviewer": arguments.reviewer,
            "evidence_reference": arguments.evidence_reference,
            "spec_deviation": deviation,
        },
        "measurement_sha256": measurement_file_sha256,
        "runtime_samples_sha256": sha256_file(arguments.runtime_samples),
        "limits_source_sha256": sha256_file(arguments.limits),
        "capture_bundle_sha256": arguments.capture_bundle_sha256,
        "review_request_sha256": arguments.review_request_sha256,
        "review_request_content_sha256":
            arguments.review_request_content_sha256,
        "source": measurement["source"],
        "environment": measurement["environment"],
        "canonical_environment_key": measurement["environment"]["sha256"],
        "sample_policy": measurement["sample_policy"],
        "measurement_context": measurement["measurement_context"],
        "measurement_context_summary": context_summary,
        "baselines": baselines,
        "limits": limits,
        "runtime_samples": runtime,
        "runtime_provenance": runtime_document["provenance"],
        "runtime_context_summary": runtime_context_summary,
        "expected_artifacts": measurement["artifacts"],
        "expected_runtime_artifacts": measurement["runtime_artifacts"],
    }
    result["content_sha256"] = canonical_json_sha256(result)
    validate_reviewed_baseline(result)
    write_json(arguments.output, result)


def validate_reviewed_baseline(value: dict[str, Any]) -> None:
    expected_fields = {
        "schema_version",
        "artifact_kind",
        "status",
        "baseline_kind",
        "review",
        "measurement_sha256",
        "runtime_samples_sha256",
        "limits_source_sha256",
        "capture_bundle_sha256",
        "review_request_sha256",
        "review_request_content_sha256",
        "source",
        "environment",
        "canonical_environment_key",
        "sample_policy",
        "measurement_context",
        "measurement_context_summary",
        "baselines",
        "limits",
        "runtime_samples",
        "runtime_provenance",
        "runtime_context_summary",
        "expected_artifacts",
        "expected_runtime_artifacts",
        "content_sha256",
    }
    if set(value) != expected_fields:
        fail("reviewed baseline fields are not exact")
    if value.get("schema_version") != SCHEMA_VERSION:
        fail("reviewed baseline schema_version is invalid")
    if value.get("artifact_kind") != "reviewed_baseline":
        fail("expected a reviewed_baseline artifact")
    if value.get("status") != "reviewed":
        fail("baseline is not reviewed")
    for field in (
        "measurement_sha256",
        "runtime_samples_sha256",
        "limits_source_sha256",
        "capture_bundle_sha256",
        "review_request_sha256",
        "review_request_content_sha256",
    ):
        if not re.fullmatch(
            r"[0-9a-f]{64}", str(value.get(field))
        ):
            fail(f"reviewed baseline {field} is invalid")
    kind = value.get("baseline_kind")
    if kind not in {"stage0", "recovered_post_cutover"}:
        fail("baseline_kind must be stage0 or recovered_post_cutover")
    review = value.get("review")
    if not isinstance(review, dict) or not str(review.get("reviewer", "")).strip():
        fail("reviewed baseline has no named reviewer")
    if not str(review.get("evidence_reference", "")).strip():
        fail("reviewed baseline has no evidence reference")
    deviation = review.get("spec_deviation")
    if kind == "stage0":
        if deviation is not None:
            fail("stage0 baseline must not have a spec deviation")
    else:
        if not isinstance(deviation, dict):
            fail("recovered baseline has no explicit spec deviation")
        if not str(deviation.get("reason", "")).strip():
            fail("recovered baseline deviation has no reason")
        if not str(deviation.get("approval_reference", "")).strip():
            fail("recovered baseline has no approval reference")
    environment = validate_environment(
        value.get("environment"), "reviewed baseline"
    )
    validate_source_fingerprint(
        value.get("source"), "reviewed baseline"
    )
    if value.get("canonical_environment_key") != environment["sha256"]:
        fail(
            "reviewed baseline canonical_environment_key differs from "
            "its environment fingerprint"
        )
    baselines = value.get("baselines")
    limits = value.get("limits")
    if not isinstance(baselines, dict) or not isinstance(limits, dict):
        fail("reviewed baseline baselines/limits must be objects")
    checked_baselines = validate_baselines(baselines, "baselines")
    checked_limits = validate_limits(limits, checked_baselines)
    runtime = value.get("runtime_samples")
    if not isinstance(runtime, dict) or set(runtime) != set(
        RUNTIME_SAMPLE_METRICS
    ):
        fail("reviewed baseline runtime samples are incomplete")
    provenance = value.get("runtime_provenance")
    if (
        not isinstance(provenance, dict)
        or provenance.get("kind") != "bioseq_benchmark_reports"
    ):
        fail("reviewed baseline runtime provenance is invalid")
    reports = provenance.get("reports")
    if not isinstance(reports, list) or len(reports) < MIN_SAMPLES:
        fail("reviewed baseline has too few runtime reports")
    report_nonces: set[int] = set()
    for index, report in enumerate(reports):
        if (
            not isinstance(report, dict)
            or set(report) != {
                "path",
                "sha256",
                "meta",
                "run_nonce",
                "host_state",
                "paired_runtime_samples",
            }
            or not str(report.get("path", "")).strip()
            or not re.fullmatch(
                r"[0-9a-f]{64}", str(report.get("sha256"))
            )
        ):
            fail(f"reviewed baseline runtime report {index} is invalid")
        paired = report.get("paired_runtime_samples")
        if (
            not isinstance(paired, dict)
            or set(paired) != {
                "file_sha256",
                "content_sha256",
            }
            or any(
                not re.fullmatch(
                    r"[0-9a-f]{64}", str(paired.get(field))
                )
                for field in ("file_sha256", "content_sha256")
            )
        ):
            fail(
                f"reviewed baseline runtime report {index} paired "
                "runtime evidence is invalid"
            )
        nonce = positive_integer(
            report.get("run_nonce"),
            f"reviewed baseline runtime report {index}.run_nonce",
        )
        if nonce in report_nonces:
            fail("reviewed baseline runtime report nonces are not unique")
        report_nonces.add(nonce)
    for metric, samples in runtime.items():
        if not isinstance(samples, list) or len(samples) != len(reports):
            fail(f"reviewed baseline runtime samples are invalid: {metric}")
        checked_samples = [
            positive_number(sample, f"runtime {metric}[{index}]")
            for index, sample in enumerate(samples)
        ]
        if float(statistics.median(checked_samples)) != checked_baselines[metric]:
            fail(
                f"reviewed baseline runtime median disagrees: {metric}"
            )
    expected_artifacts = value.get("expected_artifacts")
    if not isinstance(expected_artifacts, dict):
        fail("reviewed baseline has no expected_artifacts")
    for group in ARTIFACT_GROUPS:
        item = expected_artifacts.get(group)
        if not isinstance(item, dict):
            fail(f"reviewed baseline has no expected {group} artifacts")
        if positive_number(item.get("bytes"), f"{group}.bytes") != (
            checked_baselines[f"generated_{group}_bytes"]
        ):
            fail(f"reviewed {group} artifact bytes disagree with baseline")
    validate_runtime_artifacts(
        value.get("expected_runtime_artifacts"), "reviewed baseline"
    )
    sample_policy = value.get("sample_policy")
    if not isinstance(sample_policy, dict):
        fail("reviewed baseline has no sample_policy")
    context_summary = summarize_measurement_context(
        value.get("measurement_context"),
        environment=environment,
        repetitions=positive_integer(
            sample_policy.get("repetitions"),
            "reviewed baseline sample repetitions",
        ),
        label="reviewed baseline",
    )
    if value.get("measurement_context_summary") != context_summary:
        fail("reviewed baseline context summary is inconsistent")
    if not set(context_summary["cpu_governor_states"]).issubset(
        checked_limits["measurement_cpu_governor_states"]
    ):
        fail(
            "reviewed baseline governor allowlist excludes observed state"
        )
    runtime_context_summary = summarize_runtime_context(
        reports,
        environment=environment,
        label="reviewed baseline runtime context",
    )
    if value.get("runtime_context_summary") != runtime_context_summary:
        fail("reviewed baseline runtime context summary is inconsistent")
    validate_runtime_context_limits(
        runtime_context_summary,
        baselines=checked_baselines,
        limits=checked_limits,
        label="reviewed baseline runtime context",
    )
    validate_content_seal(value, "reviewed baseline")


def command_validate_baseline(arguments: argparse.Namespace) -> None:
    validate_reviewed_baseline(load_json(arguments.baseline))


def command_qualify(arguments: argparse.Namespace) -> None:
    baseline = load_json(arguments.baseline)
    validate_reviewed_baseline(baseline)
    measurement = load_json(arguments.measurement)
    validate_measurement(measurement)
    if measurement["role"] != "candidate":
        fail("qualification requires a role=candidate measurement")
    qualification_request = {
        "file_sha256": arguments.qualification_request_sha256,
        "content_sha256":
            arguments.qualification_request_content_sha256,
        "normalized_sha256":
            arguments.qualification_request_normalized_sha256,
    }
    for field, digest in qualification_request.items():
        if not re.fullmatch(r"[0-9a-f]{64}", str(digest)):
            fail(f"qualification request {field} is invalid")
    expected_environment = baseline["canonical_environment_key"]
    actual_environment = measurement["environment"]["sha256"]
    if actual_environment != expected_environment:
        fail(
            "candidate environment differs from the reviewed baseline: "
            f"{actual_environment} != {expected_environment}"
        )

    observed = {
        metric: float(measurement["aggregates"][metric])
        for metric in BUILD_SAMPLE_METRICS
    }
    for group in ARTIFACT_GROUPS:
        observed[f"generated_{group}_bytes"] = float(
            measurement["artifacts"][group]["bytes"]
        )
    compile_deltas = {}
    for compile_metric, delta_metric in zip(
        COMPILE_METRICS, COMPILE_DELTA_METRICS, strict=True
    ):
        baseline_value = positive_number(
            baseline["baselines"][compile_metric],
            f"baseline {compile_metric}",
        )
        compile_deltas[delta_metric] = (
            (observed[compile_metric] - baseline_value)
            / baseline_value
            * 100.0
        )
    observed.update(compile_deltas)
    context_summary = summarize_measurement_context(
        measurement["measurement_context"],
        environment=measurement["environment"],
        repetitions=measurement["sample_policy"]["repetitions"],
        label="candidate qualification",
    )
    observed_load = float(context_summary["normalized_load_1_max"])
    observed_memory = float(
        context_summary["available_memory_bytes_min"]
    )
    baseline_load = float(
        baseline["baselines"]["measurement_normalized_load_1_max"]
    )
    baseline_memory = float(
        baseline["baselines"]["measurement_available_memory_bytes_min"]
    )
    observed.update({
        "measurement_normalized_load_1_max": observed_load,
        "measurement_available_memory_bytes_min": observed_memory,
        "measurement_normalized_load_1_delta":
            observed_load - baseline_load,
        "measurement_available_memory_drop_percent":
            (baseline_memory - observed_memory)
            / baseline_memory
            * 100.0,
    })
    failures = []
    for metric in BUILD_DIRECT_LIMIT_METRICS:
        value = observed[metric]
        limit = float(baseline["limits"][metric])
        if value > limit:
            failures.append(
                {"metric": metric, "observed": value, "limit": limit}
            )
    for metric in COMPILE_DELTA_METRICS:
        value = observed[metric]
        limit = float(baseline["limits"][metric])
        if value > limit:
            failures.append(
                {"metric": metric, "observed": value, "limit": limit}
            )
    context_limit_checks = (
        (
            "measurement_normalized_load_1_max",
            observed_load,
            float(baseline["limits"][
                "measurement_normalized_load_1_max"
            ]),
            "maximum",
        ),
        (
            "measurement_normalized_load_1_delta_max",
            observed["measurement_normalized_load_1_delta"],
            float(baseline["limits"][
                "measurement_normalized_load_1_delta_max"
            ]),
            "maximum",
        ),
        (
            "measurement_available_memory_bytes_min",
            observed_memory,
            float(baseline["limits"][
                "measurement_available_memory_bytes_min"
            ]),
            "minimum",
        ),
        (
            "measurement_available_memory_drop_percent_max",
            observed["measurement_available_memory_drop_percent"],
            float(baseline["limits"][
                "measurement_available_memory_drop_percent_max"
            ]),
            "maximum",
        ),
    )
    for metric, value, limit, direction in context_limit_checks:
        failed = value > limit if direction == "maximum" else value < limit
        if failed:
            failures.append({
                "metric": metric,
                "observed": value,
                "limit": limit,
                "direction": direction,
            })
    allowed_governors = set(
        baseline["limits"]["measurement_cpu_governor_states"]
    )
    unexpected_governors = sorted(
        set(context_summary["cpu_governor_states"])
        - allowed_governors
    )
    if unexpected_governors:
        failures.append({
            "metric": "measurement_cpu_governor_states",
            "observed": context_summary["cpu_governor_states"],
            "allowed": sorted(allowed_governors),
            "unexpected": unexpected_governors,
        })
    if failures:
        fail(
            "build qualification failed: "
            + json.dumps(failures, sort_keys=True)
        )

    result = {
        "schema_version": SCHEMA_VERSION,
        "artifact_kind": "qualification",
        "status": "pass",
        "baseline_kind": baseline["baseline_kind"],
        "baseline_sha256": sha256_file(arguments.baseline),
        "baseline_content_sha256": baseline["content_sha256"],
        "measurement_sha256": sha256_file(arguments.measurement),
        "qualification_request": qualification_request,
        "baseline_review": baseline["review"],
        "source": measurement["source"],
        "environment": measurement["environment"],
        "canonical_environment_key": actual_environment,
        "sample_policy": measurement["sample_policy"],
        "measurement_context": measurement["measurement_context"],
        "measurement_context_summary": context_summary,
        "baselines": baseline["baselines"],
        "observed": observed,
        "limits": baseline["limits"],
        "expected_artifacts": measurement["artifacts"],
        "expected_runtime_artifacts": measurement["runtime_artifacts"],
    }
    result["content_sha256"] = canonical_json_sha256(result)
    write_json(arguments.output, result)


def validate_qualification(value: dict[str, Any]) -> None:
    expected_fields = {
        "schema_version",
        "artifact_kind",
        "status",
        "baseline_kind",
        "baseline_sha256",
        "baseline_content_sha256",
        "measurement_sha256",
        "qualification_request",
        "baseline_review",
        "source",
        "environment",
        "canonical_environment_key",
        "sample_policy",
        "measurement_context",
        "measurement_context_summary",
        "baselines",
        "observed",
        "limits",
        "expected_artifacts",
        "expected_runtime_artifacts",
        "content_sha256",
    }
    if set(value) != expected_fields:
        fail("qualification fields are not exact")
    if value.get("schema_version") != SCHEMA_VERSION:
        fail("qualification schema_version is invalid")
    if value.get("artifact_kind") != "qualification":
        fail("expected a qualification artifact")
    if value.get("status") != "pass":
        fail("qualification status is not pass")
    for field in (
        "baseline_sha256",
        "baseline_content_sha256",
        "measurement_sha256",
    ):
        if not re.fullmatch(r"[0-9a-f]{64}", str(value.get(field))):
            fail(f"qualification {field} is invalid")
    qualification_request = value.get("qualification_request")
    if (
        not isinstance(qualification_request, dict)
        or set(qualification_request) != {
            "file_sha256",
            "content_sha256",
            "normalized_sha256",
        }
        or any(
            re.fullmatch(r"[0-9a-f]{64}", str(digest)) is None
            for digest in qualification_request.values()
        )
    ):
        fail("qualification workflow-request provenance is invalid")
    if value.get("baseline_kind") not in {
        "stage0", "recovered_post_cutover"
    }:
        fail("qualification baseline_kind is invalid")
    review = value.get("baseline_review")
    if not isinstance(review, dict) or not str(
        review.get("reviewer", "")
    ).strip():
        fail("qualification has no baseline reviewer")
    deviation = review.get("spec_deviation")
    if value["baseline_kind"] == "stage0":
        if deviation is not None:
            fail("Stage-0 qualification carries a spec deviation")
    else:
        if (
            not isinstance(deviation, dict)
            or not str(deviation.get("reason", "")).strip()
            or not str(deviation.get("approval_reference", "")).strip()
        ):
            fail(
                "recovered qualification lacks approved spec-deviation "
                "provenance"
            )
    environment = validate_environment(
        value.get("environment"), "qualification"
    )
    validate_source_fingerprint(
        value.get("source"), "qualification"
    )
    if value.get("canonical_environment_key") != environment["sha256"]:
        fail(
            "qualification canonical_environment_key differs from its "
            "environment fingerprint"
        )
    observed = value.get("observed")
    baselines = value.get("baselines")
    limits = value.get("limits")
    if (
        not isinstance(observed, dict)
        or not isinstance(baselines, dict)
        or not isinstance(limits, dict)
    ):
        fail("qualification observed/baselines/limits must be objects")
    if set(observed) != set(QUALIFICATION_OBSERVED_METRICS):
        fail("qualification observed metric set is not exact")
    checked_baselines = validate_baselines(
        baselines, "qualification baselines"
    )
    checked_limits = validate_limits(limits, checked_baselines)
    for metric in BUILD_SAMPLE_METRICS:
        positive_number(
            observed.get(metric), f"qualification observed {metric}"
        )
    for metric in BUILD_DIRECT_LIMIT_METRICS:
        actual = positive_number(
            observed.get(metric), f"qualification observed {metric}"
        )
        if actual > float(limits[metric]):
            fail(f"qualification contains a failing metric: {metric}")
    for metric in COMPILE_DELTA_METRICS:
        actual = finite_number(
            observed.get(metric), f"qualification observed {metric}"
        )
        if actual > float(limits[metric]):
            fail(f"qualification contains a failing metric: {metric}")
    for compile_metric, delta_metric in zip(
        COMPILE_METRICS, COMPILE_DELTA_METRICS, strict=True
    ):
        expected_delta = (
            (
                float(observed[compile_metric])
                - checked_baselines[compile_metric]
            )
            / checked_baselines[compile_metric]
            * 100.0
        )
        if not math.isclose(
            float(observed[delta_metric]),
            expected_delta,
            rel_tol=1e-12,
            abs_tol=1e-12,
        ):
            fail(
                f"qualification compile delta is inconsistent: "
                f"{delta_metric}"
            )
    observed_load = nonnegative_number(
        observed.get("measurement_normalized_load_1_max"),
        "qualification observed measurement_normalized_load_1_max",
    )
    observed_memory = positive_number(
        observed.get("measurement_available_memory_bytes_min"),
        "qualification observed measurement_available_memory_bytes_min",
    )
    observed_load_delta = finite_number(
        observed.get("measurement_normalized_load_1_delta"),
        "qualification observed measurement_normalized_load_1_delta",
    )
    observed_memory_drop = finite_number(
        observed.get("measurement_available_memory_drop_percent"),
        "qualification observed measurement_available_memory_drop_percent",
    )
    baseline_load = checked_baselines[
        "measurement_normalized_load_1_max"
    ]
    baseline_memory = checked_baselines[
        "measurement_available_memory_bytes_min"
    ]
    if not math.isclose(
        observed_load_delta,
        observed_load - baseline_load,
        rel_tol=1e-12,
        abs_tol=1e-15,
    ):
        fail("qualification normalized-load delta is inconsistent")
    expected_memory_drop = (
        (baseline_memory - observed_memory) / baseline_memory * 100.0
    )
    if not math.isclose(
        observed_memory_drop,
        expected_memory_drop,
        rel_tol=1e-12,
        abs_tol=1e-12,
    ):
        fail("qualification available-memory drop is inconsistent")
    if observed_load > float(
        checked_limits["measurement_normalized_load_1_max"]
    ):
        fail("qualification normalized load exceeds its absolute bound")
    if observed_load_delta > float(
        checked_limits["measurement_normalized_load_1_delta_max"]
    ):
        fail("qualification normalized load exceeds its delta bound")
    if observed_memory < float(
        checked_limits["measurement_available_memory_bytes_min"]
    ):
        fail("qualification available memory is below its absolute bound")
    if observed_memory_drop > float(
        checked_limits[
            "measurement_available_memory_drop_percent_max"
        ]
    ):
        fail("qualification available-memory drop exceeds its bound")
    expected_artifacts = value.get("expected_artifacts")
    if not isinstance(expected_artifacts, dict):
        fail("qualification expected_artifacts must be an object")
    for group in ARTIFACT_GROUPS:
        item = expected_artifacts.get(group)
        if not isinstance(item, dict):
            fail(f"qualification has no expected {group} artifacts")
        bytes_value = positive_number(
            item.get("bytes"), f"expected {group}.bytes"
        )
        positive_number(item.get("files"), f"expected {group}.files")
        if bytes_value != observed[f"generated_{group}_bytes"]:
            fail(
                f"qualification {group} artifact bytes disagree with "
                "the observed metric"
            )
    validate_runtime_artifacts(
        value.get("expected_runtime_artifacts"), "qualification"
    )
    sample_policy = value.get("sample_policy")
    if not isinstance(sample_policy, dict):
        fail("qualification has no sample_policy")
    context_summary = summarize_measurement_context(
        value.get("measurement_context"),
        environment=environment,
        repetitions=positive_integer(
            sample_policy.get("repetitions"),
            "qualification sample repetitions",
        ),
        label="qualification",
    )
    if value.get("measurement_context_summary") != context_summary:
        fail("qualification context summary is inconsistent")
    if (
        context_summary["normalized_load_1_max"] != observed_load
        or context_summary["available_memory_bytes_min"]
        != observed_memory
    ):
        fail("qualification context summary disagrees with observed metrics")
    if not set(context_summary["cpu_governor_states"]).issubset(
        checked_limits["measurement_cpu_governor_states"]
    ):
        fail("qualification includes an unreviewed CPU governor state")
    validate_content_seal(value, "qualification")


def command_validate_qualification(arguments: argparse.Namespace) -> None:
    validate_qualification(load_json(arguments.qualification))


def validate_qualification_baseline_join(
    qualification: dict[str, Any],
    baseline_path: Path,
) -> dict[str, Any]:
    baseline = load_json(baseline_path)
    validate_reviewed_baseline(baseline)
    expected = {
        "baseline_sha256": sha256_file(baseline_path),
        "baseline_content_sha256": baseline["content_sha256"],
        "baseline_kind": baseline["baseline_kind"],
        "baseline_review": baseline["review"],
        "canonical_environment_key":
            baseline["canonical_environment_key"],
        "baselines": baseline["baselines"],
        "limits": baseline["limits"],
    }
    for field, expected_value in expected.items():
        if qualification.get(field) != expected_value:
            fail(
                "qualification is not joined to the exact reviewed "
                f"baseline field: {field}"
            )
    return baseline


def validate_live_baseline_authority(
    qualification: dict[str, Any],
) -> None:
    if (
        qualification["baseline_kind"] == "stage0"
        and qualification["baseline_sha256"]
            not in LIVE_STAGE0_BASELINE_SHA256_ALLOWLIST
    ):
        fail(
            "live Stage-0 qualification is forbidden because no pinned "
            "pre-cutover Stage-0 baseline hash exists"
        )


def validate_live_binding(
    *,
    source: dict[str, Any],
    environment: dict[str, Any],
    artifacts: dict[str, Any],
    runtime_artifacts: dict[str, Any],
    arguments: argparse.Namespace,
    context: str,
) -> None:
    current_source = source_fingerprint(arguments.repository_root.resolve())
    if source_build_identity(current_source) != source_build_identity(source):
        fail(
            f"{context} build-input fingerprint is stale for the live worktree"
        )
    current_environment = environment_fingerprint(
        arguments.build_dir.resolve()
    )
    if current_environment != environment:
        fail(
            f"{context} environment fingerprint is stale for the live build"
        )
    current_artifacts = measure_artifacts(
        parse_artifact_inventory(arguments.artifact_inventory)
    )
    if current_artifacts != artifacts:
        fail(
            f"{context} generated artifact hashes differ from the live files"
        )
    current_runtime_artifacts = measure_runtime_artifacts(
        arguments.generator_executable,
        arguments.production_dso,
        arguments.benchmark_executable,
        arguments.comparator_dso,
        arguments.installed_production_dso,
    )
    if current_runtime_artifacts != runtime_artifacts:
        fail(
            f"{context} executable/DSO hashes differ from the live files"
        )


def command_validate_measurement_live(arguments: argparse.Namespace) -> None:
    measurement = load_json(arguments.measurement)
    validate_measurement(measurement)
    validate_live_binding(
        source=measurement["source"],
        environment=measurement["environment"],
        artifacts=measurement["artifacts"],
        runtime_artifacts=measurement["runtime_artifacts"],
        arguments=arguments,
        context="measurement",
    )


def validate_mcp_worker_state(
    authority: dict[str, Any],
    state: dict[str, Any],
) -> dict[str, Any]:
    """Validate live process/session/request ownership independently."""
    worker_pid = positive_integer(
        authority.get("worker_pid"), "live MCP authority worker PID"
    )
    worker_session_id = positive_integer(
        authority.get("worker_session_id"),
        "live MCP authority worker session",
    )
    if not isinstance(authority.get("worker_started_at"), (int, float)):
        fail("live MCP authority worker start time is invalid")
    request = authority.get("workflow_request")
    entry = state.get("bench-e2e") if isinstance(state, dict) else None
    if (
        not isinstance(request, dict)
        or set(request) != {
            "file_sha256", "content_sha256", "normalized_sha256"
        }
        or any(
            not re.fullmatch(r"[0-9a-f]{64}", str(request.get(field)))
            for field in request
        )
        or not isinstance(entry, dict)
        or entry.get("status") != "running"
        or entry.get("pid") != worker_pid
        or entry.get("started_at") != authority["worker_started_at"]
        or entry.get("sql_guardrail_request_sha256")
            != request["normalized_sha256"]
    ):
        fail(
            "live MCP authority is not joined to the exact running "
            "worker/request"
        )
    try:
        os.kill(worker_pid, 0)
        if (
            os.getsid(worker_pid) != worker_session_id
            or os.getsid(0) != worker_session_id
        ):
            fail("live MCP authority worker session is not current")
    except OSError as error:
        fail(f"live MCP authority worker is not alive/current: {error}")
    ancestor_pid = os.getpid()
    ancestor_chain: set[int] = set()
    for _ in range(64):
        if ancestor_pid <= 0 or ancestor_pid in ancestor_chain:
            break
        ancestor_chain.add(ancestor_pid)
        if ancestor_pid == worker_pid:
            break
        result = subprocess.run(
            ["ps", "-o", "ppid=", "-p", str(ancestor_pid)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode != 0:
            fail(
                "cannot prove live MCP worker ancestry: "
                f"{result.stderr.strip()}"
            )
        try:
            ancestor_pid = int(result.stdout.strip())
        except ValueError:
            fail("cannot parse live MCP worker ancestry")
    if worker_pid not in ancestor_chain:
        fail(
            "qualification validator is not a descendant of the exact "
            "running MCP worker"
        )
    return request


def validate_mcp_live_authority(
    arguments: argparse.Namespace,
    *,
    qualification: dict[str, Any],
    baseline: dict[str, Any],
) -> None:
    """Require the running MCP worker's structural publication authority."""
    authority = load_json(arguments.mcp_authority)
    expected_fields = {
        "schema_version",
        "artifact_kind",
        "phase",
        "worker_mode",
        "worker_pid",
        "worker_session_id",
        "worker_started_at",
        "state_file",
        "broker_socket",
        "workflow_request",
        "run_nonce",
        "qualification",
        "baseline",
        "measurement",
        "baseline_publication",
        "issued_monotonic_ns",
        "content_sha256",
    }
    nonce = positive_integer(
        arguments.expected_run_nonce,
        "MCP live authority expected run nonce",
    )
    if nonce >= (1 << 53):
        fail("MCP live authority nonce is not exactly representable")
    if (
        not isinstance(authority, dict)
        or set(authority) != expected_fields
        or authority.get("schema_version") != SCHEMA_VERSION
        or authority.get("artifact_kind")
            != "doc0388_sql_guardrail_live_mcp_authority"
        or authority.get("phase") != "candidate_qualification"
        or authority.get("worker_mode") != "bench-e2e"
        or authority.get("state_file") != str(MCP_STATE_FILE)
        or authority.get("broker_socket")
            != str(arguments.mcp_authority_socket)
        or authority.get("run_nonce") != nonce
    ):
        fail("live MCP authority identity/schema is invalid")
    validate_content_seal(authority, "live MCP authority")
    positive_integer(
        authority.get("issued_monotonic_ns"),
        "live MCP authority issue timestamp",
    )
    try:
        state = json.loads(MCP_STATE_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read live MCP worker state: {error}")
    request = validate_mcp_worker_state(authority, state)

    def validate_artifact_record(
        record: Any,
        *,
        path: Path,
        value: dict[str, Any],
        context: str,
    ) -> None:
        if (
            not isinstance(record, dict)
            or set(record) != {
                "path", "file_sha256", "content_sha256"
            }
            or record.get("path") != str(path.resolve(strict=True))
            or record.get("file_sha256") != sha256_file(path)
            or record.get("content_sha256")
                != value.get("content_sha256")
        ):
            fail(f"live MCP authority {context} binding is invalid")

    qualification_path = arguments.qualification.resolve(strict=True)
    baseline_path = arguments.baseline.resolve(strict=True)
    validate_artifact_record(
        authority.get("qualification"),
        path=qualification_path,
        value=qualification,
        context="qualification",
    )
    validate_artifact_record(
        authority.get("baseline"),
        path=baseline_path,
        value=baseline,
        context="baseline",
    )
    measurement_record = authority.get("measurement")
    if (
        not isinstance(measurement_record, dict)
        or set(measurement_record) != {
            "path", "file_sha256", "content_sha256"
        }
    ):
        fail("live MCP authority measurement record is malformed")
    measurement_path = Path(str(measurement_record["path"]))
    measurement = load_json(measurement_path)
    validate_measurement(measurement)
    validate_artifact_record(
        measurement_record,
        path=measurement_path,
        value=measurement,
        context="measurement",
    )
    if (
        qualification.get("measurement_sha256")
            != measurement_record["file_sha256"]
        or qualification.get("baseline_sha256")
            != authority["baseline"]["file_sha256"]
        or qualification.get("baseline_content_sha256")
            != baseline.get("content_sha256")
        or qualification.get("qualification_request") != request
    ):
        fail(
            "live MCP authority qualification/baseline/measurement/request "
            "join is invalid"
        )

    publication = authority.get("baseline_publication")
    if (
        not isinstance(publication, dict)
        or set(publication) != {
            "receipt_path", "receipt_file_sha256", "publication_id"
        }
        or not re.fullmatch(
            r"[0-9a-f]{64}", str(publication.get("publication_id"))
        )
    ):
        fail("live MCP authority baseline publication is malformed")
    receipt_path = Path(str(publication["receipt_path"]))
    receipt = load_json(receipt_path)
    receipt_fields = {
        "schema_version",
        "artifact_kind",
        "publication_id",
        "baseline_path",
        "baseline_file_sha256",
        "baseline_content_sha256",
        "capture_bundle_sha256",
        "canonical_environment_key",
        "promotion_request_file_sha256",
        "promotion_request_content_sha256",
        "promotion_request_snapshot_sha256",
        "promotion_request",
        "worker_pid",
        "worker_session_id",
        "worker_started_at",
        "published_monotonic_ns",
        "content_sha256",
    }
    if (
        set(receipt) != receipt_fields
        or receipt.get("schema_version") != SCHEMA_VERSION
        or receipt.get("artifact_kind")
            != "doc0388_sql_guardrail_baseline_publication"
        or receipt.get("publication_id") != publication["publication_id"]
        or sha256_file(receipt_path)
            != publication["receipt_file_sha256"]
        or receipt.get("baseline_file_sha256")
            != authority["baseline"]["file_sha256"]
        or receipt.get("baseline_content_sha256")
            != baseline.get("content_sha256")
        or receipt.get("canonical_environment_key")
            != baseline.get("canonical_environment_key")
        or not isinstance(receipt.get("promotion_request"), dict)
        or canonical_json_sha256(receipt["promotion_request"])
            != receipt.get("promotion_request_snapshot_sha256")
        or receipt["promotion_request"].get("phase")
            != "baseline_promotion"
    ):
        fail("live MCP baseline publication receipt binding is invalid")
    validate_content_seal(
        receipt, "live MCP baseline publication receipt"
    )
    ledger = load_json(MCP_PUBLICATION_LEDGER)
    if (
        set(ledger) != {
            "schema_version",
            "artifact_kind",
            "publications",
            "content_sha256",
        }
        or ledger.get("schema_version") != SCHEMA_VERSION
        or ledger.get("artifact_kind")
            != "doc0388_sql_guardrail_publication_ledger"
        or not isinstance(ledger.get("publications"), dict)
    ):
        fail("live MCP publication ledger schema is invalid")
    validate_content_seal(ledger, "live MCP publication ledger")
    expected_ledger_record = {
        "receipt_path": str(receipt_path.resolve(strict=True)),
        "receipt_file_sha256": sha256_file(receipt_path),
        "baseline_path": receipt["baseline_path"],
        "baseline_file_sha256": receipt["baseline_file_sha256"],
        "canonical_environment_key":
            receipt["canonical_environment_key"],
        "promotion_request_snapshot_sha256":
            receipt["promotion_request_snapshot_sha256"],
    }
    if ledger["publications"].get(
        publication["publication_id"]
    ) != expected_ledger_record:
        fail(
            "live MCP baseline publication is absent from the exact "
            "publication ledger"
        )


def consume_mcp_live_authority(
    arguments: argparse.Namespace,
    *,
    authority: dict[str, Any],
) -> None:
    """Consume the worker's in-memory one-use acceptance callback."""
    socket_path = arguments.mcp_authority_socket
    if (
        not socket_path.is_absolute()
        or str(socket_path) != authority.get("broker_socket")
    ):
        fail("live MCP authority broker path is invalid")
    request = {
        "schema_version": SCHEMA_VERSION,
        "artifact_kind":
            "doc0388_sql_guardrail_authority_claim",
        "run_nonce": authority["run_nonce"],
        "authority_file_sha256": sha256_file(
            arguments.mcp_authority
        ),
        "request_snapshot_sha256":
            authority["workflow_request"]["normalized_sha256"],
    }
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(3.0)
    try:
        client.connect(str(socket_path))
        if hasattr(socket, "SO_PEERCRED"):
            credentials = client.getsockopt(
                socket.SOL_SOCKET,
                socket.SO_PEERCRED,
                struct.calcsize("3i"),
            )
            peer_pid, peer_uid, _ = struct.unpack("3i", credentials)
            if (
                peer_pid != authority["worker_pid"]
                or peer_uid != os.getuid()
            ):
                fail(
                    "live MCP authority broker peer credentials do not "
                    "match the running worker"
                )
        elif platform.system() == "Darwin":
            # Darwin exposes LOCAL_PEERPID as level SOL_LOCAL (0), option 2,
            # but Python's socket module does not currently publish either
            # constant.  Require the peer PID instead of accepting the weaker
            # same-UID-only getpeereid result.
            credentials = client.getsockopt(
                0,
                2,
                struct.calcsize("i"),
            )
            peer_pid = struct.unpack("i", credentials)[0]
            if peer_pid != authority["worker_pid"]:
                fail(
                    "live MCP authority broker peer PID does not match "
                    "the running worker"
                )
        elif hasattr(client, "getpeereid"):
            peer_uid, _ = client.getpeereid()
            if peer_uid != os.getuid():
                fail("live MCP authority broker peer UID is invalid")
        else:
            fail(
                "live MCP authority broker peer credentials are "
                "unavailable on this platform"
            )
        client.sendall(
            (json.dumps(
                request,
                sort_keys=True,
                separators=(",", ":"),
            ) + "\n").encode("utf-8")
        )
        body = bytearray()
        while b"\n" not in body and len(body) <= 4096:
            chunk = client.recv(4096)
            if not chunk:
                break
            body.extend(chunk)
    except OSError as error:
        fail(f"live MCP authority callback failed: {error}")
    finally:
        client.close()
    try:
        response = json.loads(bytes(body).split(b"\n", 1)[0])
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"live MCP authority response is invalid: {error}")
    if response != {
        "schema_version": SCHEMA_VERSION,
        "artifact_kind":
            "doc0388_sql_guardrail_authority_response",
        "accepted": True,
        "run_nonce": authority["run_nonce"],
        "authority_file_sha256": request[
            "authority_file_sha256"
        ],
        "request_snapshot_sha256": request[
            "request_snapshot_sha256"
        ],
        "broker_pid": authority["worker_pid"],
        "broker_session_id": authority["worker_session_id"],
    }:
        fail(
            "live MCP authority callback was rejected, replayed, or "
            "not owned by the exact worker"
        )


def command_validate_qualification_live(
    arguments: argparse.Namespace,
) -> None:
    qualification = load_json(arguments.qualification)
    validate_qualification(qualification)
    baseline = validate_qualification_baseline_join(
        qualification, arguments.baseline
    )
    validate_mcp_live_authority(
        arguments,
        qualification=qualification,
        baseline=baseline,
    )
    validate_live_baseline_authority(qualification)
    if baseline["canonical_environment_key"] != (
        qualification["environment"]["sha256"]
    ):
        fail(
            "qualification environment is not the reviewed baseline "
            "environment"
        )
    validate_live_binding(
        source=qualification["source"],
        environment=qualification["environment"],
        artifacts=qualification["expected_artifacts"],
        runtime_artifacts=qualification["expected_runtime_artifacts"],
        arguments=arguments,
        context="qualification",
    )
    consume_mcp_live_authority(
        arguments,
        authority=load_json(arguments.mcp_authority),
    )


def exact_sql_guardrail_report_metrics(
    path: Path,
    *,
    expected_source_head: str,
) -> dict[str, dict[str, Any]]:
    """Parse the final report independently of framework rule outcomes."""
    report = load_json(path)
    for field in ("rule_violations", "regressions", "sanity_warnings"):
        findings = report.get(field)
        if not isinstance(findings, list) or findings:
            fail(f"qualification report {field} is not an empty list")
    meta = report.get("meta")
    if (
        not isinstance(meta, dict)
        or meta.get("history_eligible") is not True
        or meta.get("commit") != expected_source_head
    ):
        fail(
            "qualification report meta is not history-eligible and "
            "source-bound"
        )
    benchmarks = report.get("benchmarks")
    if (
        not isinstance(benchmarks, dict)
        or set(benchmarks) != {"bench_sql_codegen_guardrail"}
    ):
        fail("qualification report benchmark set is not exact")
    benchmark = benchmarks["bench_sql_codegen_guardrail"]
    tuples = benchmark.get("execution_tuples")
    if not isinstance(tuples, list) or len(tuples) != len(
        BENCHMARK_ALGORITHMS
    ):
        fail("qualification report execution-tuple count is not exact")
    result: dict[str, dict[str, Any]] = {}
    for index, item in enumerate(tuples):
        if (
            not isinstance(item, dict)
            or set(item) != {"tuple", "test_cases"}
        ):
            fail(f"qualification report tuple {index} is malformed")
        selector = item.get("tuple")
        if not isinstance(selector, dict) or len(selector) != 1:
            fail(f"qualification report tuple {index} has no selector")
        algorithm = next(iter(selector))
        if (
            algorithm not in BENCHMARK_ALGORITHMS
            or selector != {algorithm: {"_": 0}}
        ):
            fail(
                f"qualification report tuple {index} selector is not the "
                "exact reviewed algorithm/default payload"
            )
        if algorithm in result:
            fail(
                f"qualification report duplicates algorithm {algorithm}"
            )
        test_cases = item.get("test_cases")
        if (
            not isinstance(test_cases, dict)
            or set(test_cases) != {"SQL_CODEGEN_BOUNDARY_SMALL"}
        ):
            fail(
                f"qualification report test-case set is not exact for "
                f"{algorithm}"
            )
        regions = test_cases[
            "SQL_CODEGEN_BOUNDARY_SMALL"
        ]
        if not isinstance(regions, dict) or set(regions) != {
            "all_regions"
        }:
            fail(
                f"qualification report region set is not exact for "
                f"{algorithm}"
            )
        metrics = regions["all_regions"]
        if not isinstance(metrics, dict) or set(metrics) != set(
            BENCHMARK_METRICS
        ):
            fail(
                f"qualification report metric set is not exact for "
                f"{algorithm}"
            )
        for metric in BENCHMARK_METRICS:
            report_metric_p50(
                metrics,
                metric,
                positive=False,
                expected_samples=15,
            )
        result[algorithm] = metrics
    if tuple(result) != BENCHMARK_ALGORITHMS:
        fail("qualification report algorithm order is not exact")
    return result


def validate_paired_runtime_samples(
    value: dict[str, Any],
    *,
    measurement: dict[str, Any],
    measurement_sha256: str,
    report_path: Path,
    expected_run_nonce: int,
) -> None:
    expected_fields = {
        "schema_version",
        "artifact_kind",
        "run_nonce",
        "measurement_sha256",
        "measurement_content_sha256",
        "report_sha256",
        "raw_file_sha256",
        "source",
        "environment",
        "algorithms",
        "content_sha256",
    }
    if (
        not isinstance(value, dict)
        or set(value) != expected_fields
        or value.get("schema_version") != SCHEMA_VERSION
        or value.get("artifact_kind") != "paired_runtime_samples"
        or value.get("run_nonce") != expected_run_nonce
        or value.get("measurement_sha256") != measurement_sha256
        or value.get("measurement_content_sha256")
            != measurement["content_sha256"]
        or value.get("report_sha256") != sha256_file(report_path)
        or value.get("source") != measurement["source"]
        or value.get("environment") != measurement["environment"]
        or not re.fullmatch(
            r"[0-9a-f]{64}", str(value.get("raw_file_sha256"))
        )
    ):
        fail("paired runtime-sample artifact identity/binding is invalid")
    validate_content_seal(value, "paired runtime-sample artifact")
    algorithms = value.get("algorithms")
    if (
        not isinstance(algorithms, dict)
        or set(algorithms) != set(COMPARATOR_ALGORITHMS)
    ):
        fail("paired runtime-sample algorithm set is not exact")
    metrics_by_algorithm = exact_sql_guardrail_report_metrics(
        report_path,
        expected_source_head=measurement["source"]["head"],
    )
    for algorithm in COMPARATOR_ALGORITHMS:
        records = algorithms.get(algorithm)
        if not isinstance(records, list) or len(records) != 15:
            fail(
                f"paired runtime samples require 15 records: {algorithm}"
            )
        if [record.get("sample_ordinal") for record in records] != list(
            range(1, 16)
        ):
            fail(
                f"paired runtime sample IDs are missing, duplicated, or "
                f"reordered: {algorithm}"
            )
        generated_values = []
        handwritten_values = []
        ratio_values = []
        for index, record in enumerate(records):
            if not isinstance(record, dict) or set(record) != {
                "sample_ordinal",
                "generated_query_ns",
                "handwritten_query_ns",
                "generated_to_handwritten_ratio",
            }:
                fail(
                    f"paired runtime sample {algorithm}[{index}] "
                    "fields are not exact"
                )
            generated = positive_integer(
                record.get("generated_query_ns"),
                f"paired {algorithm}[{index}] generated_query_ns",
            )
            handwritten = positive_integer(
                record.get("handwritten_query_ns"),
                f"paired {algorithm}[{index}] handwritten_query_ns",
            )
            if generated >= (1 << 53) or handwritten >= (1 << 53):
                fail("paired timing is not exactly representable in report")
            ratio = positive_number(
                record.get("generated_to_handwritten_ratio"),
                f"paired {algorithm}[{index}] ratio",
            )
            recomputed = generated / handwritten
            if not math.isclose(
                ratio, recomputed, rel_tol=1e-15, abs_tol=1e-15
            ):
                fail(
                    f"paired runtime ratio does not recompute for "
                    f"{algorithm}[{index}]"
                )
            generated_values.append(float(generated))
            handwritten_values.append(float(handwritten))
            ratio_values.append(recomputed)
        report_metrics = metrics_by_algorithm[algorithm]
        for metric, expected in (
            ("generated_query_ns", sorted(generated_values)),
            ("handwritten_query_ns", sorted(handwritten_values)),
            (
                "generated_to_handwritten_ratio",
                sorted(ratio_values),
            ),
        ):
            reported = report_metric_samples(report_metrics, metric)
            if len(reported) != len(expected) or any(
                not math.isclose(
                    actual,
                    wanted,
                    rel_tol=1e-12,
                    abs_tol=1e-9,
                )
                for actual, wanted in zip(
                    reported, expected, strict=True
                )
            ):
                fail(
                    f"paired raw samples do not reproduce report metric "
                    f"{algorithm}.{metric}"
                )


def paired_runtime_ratios(
    value: dict[str, Any],
    algorithm: str,
) -> list[float]:
    """Recompute ordered ratios from an already validated paired artifact."""
    if algorithm not in COMPARATOR_ALGORITHMS:
        fail(f"unknown paired runtime algorithm: {algorithm}")
    return [
        positive_integer(
            record["generated_query_ns"],
            f"paired {algorithm} generated_query_ns",
        )
        / positive_integer(
            record["handwritten_query_ns"],
            f"paired {algorithm} handwritten_query_ns",
        )
        for record in value["algorithms"][algorithm]
    ]


def command_seal_paired_runtime_samples(
    arguments: argparse.Namespace,
) -> None:
    measurement = load_json(arguments.measurement)
    validate_measurement(measurement)
    nonce = positive_integer(
        arguments.run_nonce, "paired runtime run nonce"
    )
    if nonce >= (1 << 53):
        fail("paired runtime run nonce is not exactly representable")
    records_by_algorithm = {
        algorithm: [] for algorithm in COMPARATOR_ALGORITHMS
    }
    try:
        lines = arguments.raw_jsonl.read_text(
            encoding="utf-8"
        ).splitlines()
    except OSError as error:
        fail(f"cannot read paired runtime JSONL: {error}")
    for line_number, line in enumerate(lines, start=1):
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            fail(
                f"paired runtime JSONL line {line_number} is invalid: "
                f"{error}"
            )
        if not isinstance(record, dict) or set(record) != {
            "schema_version",
            "run_nonce",
            "algorithm",
            "sample_ordinal",
            "generated_query_ns",
            "handwritten_query_ns",
            "generated_to_handwritten_ratio",
        }:
            fail(
                f"paired runtime JSONL line {line_number} fields "
                "are not exact"
            )
        algorithm = record.get("algorithm")
        if (
            record.get("schema_version") != SCHEMA_VERSION
            or record.get("run_nonce") != nonce
            or algorithm not in records_by_algorithm
        ):
            fail(
                f"paired runtime JSONL line {line_number} identity "
                "is invalid"
            )
        records_by_algorithm[algorithm].append({
            key: record[key]
            for key in (
                "sample_ordinal",
                "generated_query_ns",
                "handwritten_query_ns",
                "generated_to_handwritten_ratio",
            )
        })
    result = {
        "schema_version": SCHEMA_VERSION,
        "artifact_kind": "paired_runtime_samples",
        "run_nonce": nonce,
        "measurement_sha256": sha256_file(arguments.measurement),
        "measurement_content_sha256": measurement["content_sha256"],
        "report_sha256": sha256_file(arguments.report),
        "raw_file_sha256": sha256_file(arguments.raw_jsonl),
        "source": measurement["source"],
        "environment": measurement["environment"],
        "algorithms": records_by_algorithm,
    }
    result["content_sha256"] = canonical_json_sha256(result)
    validate_paired_runtime_samples(
        result,
        measurement=measurement,
        measurement_sha256=sha256_file(arguments.measurement),
        report_path=arguments.report,
        expected_run_nonce=nonce,
    )
    write_json(arguments.output, result)


def command_validate_paired_runtime_samples(
    arguments: argparse.Namespace,
) -> None:
    measurement = load_json(arguments.measurement)
    validate_measurement(measurement)
    value = load_json(arguments.paired_runtime_samples)
    validate_paired_runtime_samples(
        value,
        measurement=measurement,
        measurement_sha256=sha256_file(arguments.measurement),
        report_path=arguments.report,
        expected_run_nonce=positive_integer(
            arguments.expected_run_nonce,
            "paired runtime expected run nonce",
        ),
    )


def command_validate_qualification_report(
    arguments: argparse.Namespace,
) -> None:
    measurement = load_json(arguments.measurement)
    validate_measurement(measurement)
    qualification = load_json(arguments.qualification)
    validate_qualification(qualification)
    baseline = validate_qualification_baseline_join(
        qualification, arguments.baseline
    )
    if qualification["baseline_kind"] != "recovered_post_cutover":
        fail(
            "final qualification report cannot consume Stage-0 because "
            "no historical Stage-0 hash is pinned"
        )
    if qualification["measurement_sha256"] != sha256_file(
        arguments.measurement
    ):
        fail("qualification is not bound to the measured candidate file")
    for field, expected in (
        ("source", measurement["source"]),
        ("environment", measurement["environment"]),
        ("expected_artifacts", measurement["artifacts"]),
        (
            "expected_runtime_artifacts",
            measurement["runtime_artifacts"],
        ),
    ):
        if qualification.get(field) != expected:
            fail(
                f"qualification does not reproduce candidate {field}"
            )
    request_expected = {
        "file_sha256": arguments.qualification_request_sha256,
        "content_sha256":
            arguments.qualification_request_content_sha256,
        "normalized_sha256":
            arguments.qualification_request_normalized_sha256,
    }
    if qualification["qualification_request"] != request_expected:
        fail(
            "qualification request provenance differs from the scheduled "
            "candidate workflow"
        )
    nonce = positive_integer(
        arguments.expected_run_nonce,
        "qualification report expected run nonce",
    )
    if nonce >= (1 << 53):
        fail("qualification report nonce is not exactly representable")
    measurement_sha256 = sha256_file(arguments.measurement)
    before_host_state = load_json(arguments.host_state_before)
    after_host_state = load_json(arguments.host_state_after)
    for document, phase in (
        (before_host_state, "before"),
        (after_host_state, "after"),
    ):
        validate_runtime_host_state_artifact(
            document,
            measurement=measurement,
            expected_phase=phase,
        )
        if document["measurement_sha256"] != measurement_sha256:
            fail(
                "qualification report host-state artifact is not bound "
                "to the measured candidate file"
            )
        if document["run_nonce"] != nonce:
            fail(
                "qualification report host-state artifact is not bound "
                "to the benchmark run nonce"
            )
    runtime_context_summary = summarize_runtime_context(
        [{
            "host_state": {
                "before": before_host_state["snapshot"],
                "after": after_host_state["snapshot"],
            },
        }],
        environment=measurement["environment"],
        label="qualification runtime report",
        minimum_reports=1,
    )
    validate_runtime_context_limits(
        runtime_context_summary,
        baselines=baseline["baselines"],
        limits=baseline["limits"],
        label="qualification runtime report",
    )
    metrics_by_algorithm = exact_sql_guardrail_report_metrics(
        arguments.report,
        expected_source_head=measurement["source"]["head"],
    )
    paired_runtime = load_json(arguments.paired_runtime_samples)
    validate_paired_runtime_samples(
        paired_runtime,
        measurement=measurement,
        measurement_sha256=measurement_sha256,
        report_path=arguments.report,
        expected_run_nonce=nonce,
    )

    baseline_kind_value = 2.0
    observed = qualification["observed"]
    artifact_values = {
        f"generated_{group}_{field}": float(
            qualification["expected_artifacts"][group][field]
        )
        for group in ARTIFACT_GROUPS
        for field in ("bytes", "files")
    }
    common_values = {
        "generator_cold_ns": float(observed["generator_cold_ns"]),
        "generator_noop_ns": float(observed["generator_noop_ns"]),
        "generator_peak_rss_bytes":
            float(observed["generator_peak_rss_bytes"]),
        "compile_postgresql_bioseq_ns":
            float(observed["compile_postgresql_bioseq_ns"]),
        "compile_bioseq_bench_e2e_ns":
            float(observed["compile_bioseq_bench_e2e_ns"]),
        "compile_postgresql_bioseq_delta_percent":
            float(observed[
                "compile_postgresql_bioseq_delta_percent"
            ]),
        "compile_bioseq_bench_e2e_delta_percent":
            float(observed["compile_bioseq_bench_e2e_delta_percent"]),
        **artifact_values,
    }
    wrapper_algorithms = {
        "from_array_arity_2",
        "from_array_arity_3",
        "typed_wrapper_generated_vs_handwritten",
    }
    custom_scan_algorithms = {
        "mmsearch_arity_2",
        "mmsearch_arity_3",
    }
    for algorithm, metrics in metrics_by_algorithm.items():
        reference = algorithm == "qualification_reference"
        expected_zero_or_one = 0.0 if reference else 1.0
        exact_values = {
            "correctness_passed": expected_zero_or_one,
            "build_qualification_passed": expected_zero_or_one,
            "qualification_baseline_kind":
                0.0 if reference else baseline_kind_value,
            "live_measurement_attested": expected_zero_or_one,
            "arity": EXPECTED_ALGORITHM_ARITIES[algorithm],
            "generated_typed_wrapper":
                1.0 if algorithm in wrapper_algorithms else 0.0,
            "direct_fmgr_wrapper_path":
                1.0 if algorithm in wrapper_algorithms else 0.0,
            "planner_custom_scan_path":
                1.0 if algorithm in custom_scan_algorithms else 0.0,
        }
        for metric, expected in exact_values.items():
            require_report_metric_constant(
                metrics,
                metric,
                expected,
                context=f"qualification report {algorithm}",
            )
        require_report_nonce(
            metrics,
            0 if reference else nonce,
            context=f"qualification report {algorithm}",
        )
        if reference:
            require_report_metric_constant(
                metrics,
                "performance_gate_passed",
                0.0,
                context=f"qualification report {algorithm}",
            )
        codec_samples = report_metric_samples(
            metrics, "generated_row_codec_fields"
        )
        codec_fields = codec_samples[0]
        if any(value != codec_fields for value in codec_samples):
            fail(
                f"qualification report {algorithm} row-codec field "
                "count changes across raw iterations"
            )
        expected_codec_fields = EXPECTED_ROW_CODEC_FIELD_COUNTS.get(
            algorithm,
            0.0,
        )
        if codec_fields != expected_codec_fields:
            fail(
                f"qualification report {algorithm} row-codec field "
                f"count is {codec_fields}, expected "
                f"{expected_codec_fields}"
            )
        for metric, expected in common_values.items():
            wanted = 0.0 if reference else expected
            require_report_metric_constant(
                metrics,
                metric,
                wanted,
                context=f"qualification report {algorithm}",
            )
        wall_clock_samples = report_metric_samples(
            metrics, "wall_clock_ns"
        )
        if any(value <= 0.0 for value in wall_clock_samples):
            fail("qualification report has a nonpositive wall clock")
        generated_samples = report_metric_samples(
            metrics, "generated_query_ns"
        )
        query_samples = report_metric_samples(
            metrics, "query_ns"
        )
        handwritten_samples = report_metric_samples(
            metrics, "handwritten_query_ns"
        )
        ratio_samples = report_metric_samples(
            metrics, "generated_to_handwritten_ratio"
        )
        if reference:
            if any(
                value != 0.0
                for value in (
                    *generated_samples,
                    *query_samples,
                    *handwritten_samples,
                    *ratio_samples,
                )
            ):
                fail("qualification reference has query measurements")
            continue
        if (
            any(value <= 0.0 for value in generated_samples)
            or any(value <= 0.0 for value in query_samples)
            or generated_samples != query_samples
        ):
            fail(
                f"qualification report {algorithm} raw query timings "
                "are nonpositive or differ from generated timings"
            )
        comparator = algorithm in {
            "typed_wrapper_generated_vs_handwritten",
            "row_codec_generated_vs_handwritten",
        }
        if comparator:
            if (
                any(value <= 0.0 for value in handwritten_samples)
                or any(value <= 0.0 for value in ratio_samples)
            ):
                fail(
                    f"qualification report {algorithm} has nonpositive "
                    "comparator timings/ratios"
                )
        elif any(
            value != 0.0
            for value in (*handwritten_samples, *ratio_samples)
        ):
            fail(
                f"qualification report {algorithm} has unexpected "
                "handwritten timing evidence"
            )

    runtime_checks = {
        "from_array_arity_2": (
            "generated_query_ns", "from_array_arity_2_ns"
        ),
        "from_array_arity_3": (
            "generated_query_ns", "from_array_arity_3_ns"
        ),
        "typed_wrapper_generated_vs_handwritten": (
            "generated_to_handwritten_ratio",
            "typed_wrapper_generated_to_handwritten_ratio",
        ),
        "mmsearch_arity_2": (
            "generated_query_ns", "mmsearch_arity_2_ns"
        ),
        "mmsearch_arity_3": (
            "generated_query_ns", "mmsearch_arity_3_ns"
        ),
        "row_codec_generated_vs_handwritten": (
            "generated_to_handwritten_ratio",
            "row_codec_generated_to_handwritten_ratio",
        ),
    }
    for algorithm, (metric, limit_name) in runtime_checks.items():
        metrics = metrics_by_algorithm[algorithm]
        raw_runtime = (
            sorted(paired_runtime_ratios(paired_runtime, algorithm))
            if metric == "generated_to_handwritten_ratio"
            else report_metric_samples(metrics, metric)
        )
        actual = positive_number(
            raw_runtime[len(raw_runtime) // 2],
            f"qualification report {algorithm}.{metric} recomputed P50",
        )
        limit = positive_number(
            qualification["limits"][limit_name],
            f"qualification limit {limit_name}",
        )
        raw_pass_bits = report_metric_samples(
            metrics, "performance_gate_passed"
        )
        expected_pass_bits = sorted(
            1.0 if sample <= limit else 0.0
            for sample in raw_runtime
        )
        if raw_pass_bits != expected_pass_bits:
            fail(
                f"qualification report {algorithm} performance pass bits "
                "do not recompute from its raw samples and reviewed limit"
            )
        if actual > limit:
            fail(
                f"qualification report runtime limit failed: "
                f"{algorithm}.{metric}={actual} > {limit}"
            )


def synthetic_measurement(role: str) -> dict[str, Any]:
    samples = {
        metric: [100, 110, 120] for metric in BUILD_SAMPLE_METRICS
    }
    artifacts = {}
    for group in ARTIFACT_GROUPS:
        path = f"/synthetic/{group}"
        content_sha = "a" * 64
        digest = hashlib.sha256()
        digest.update(path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(b"100")
        digest.update(b"\0")
        digest.update(content_sha.encode("ascii"))
        artifacts[group] = {
            "bytes": 100,
            "files": 1,
            "inventory_sha256": digest.hexdigest(),
            "entries": [{
                "path": path,
                "bytes": 100,
                "sha256": content_sha,
            }],
        }
    runtime_artifacts = {
        name: {
            "path": f"/synthetic/{name}",
            "bytes": 100,
            "sha256": str(index) * 64,
        }
        for index, name in enumerate(RUNTIME_ARTIFACT_NAMES, start=2)
    }
    runtime_artifacts["installed_production_dso"]["bytes"] = (
        runtime_artifacts["production_dso"]["bytes"]
    )
    runtime_artifacts["installed_production_dso"]["sha256"] = (
        runtime_artifacts["production_dso"]["sha256"]
    )
    synthetic_environment = {
        "system": "SyntheticOS",
        "release": "1",
        "machine": "synthetic64",
        "processor": "synthetic",
        "cpu_model": "Synthetic CPU",
        "logical_cpu_count": 8,
        "physical_core_count": 4,
        "cpu_package_count": 1,
        "page_size_bytes": 4096,
        "physical_memory_bytes": 16 * 1024**3,
        "python": "3.14.0",
        "compiler_version": "Synthetic Compiler 1",
        "cmake_version": "cmake version 4",
        "build_tool_version": "ninja 1",
        "postgresql_version": "PostgreSQL 18",
        "codegen_c_compiler": "/synthetic/cc",
        "codegen_c_compiler_version": "Synthetic C Compiler 1",
        "codegen_cxx_compiler": "/synthetic/c++",
        "codegen_cxx_compiler_version": "Synthetic C++ Compiler 1",
        "codegen_stdlib_modules_json": "",
        "codegen_stdlib_modules_json_sha256": "",
        "toolchain_file_sha256": "",
        "named_environment": "SYNTHETIC",
        "cmake_cache": {"CMAKE_BUILD_TYPE": "Release"},
    }
    synthetic_environment["sha256"] = canonical_json_sha256(
        synthetic_environment
    )
    load_state_samples = []
    capture_ordinal = 0
    for phase in (
        "generator_cold",
        "generator_noop",
        "compile_postgresql_bioseq",
        "compile_bioseq_bench_e2e",
    ):
        for ordinal in range(MIN_SAMPLES):
            def snapshot() -> dict[str, Any]:
                nonlocal capture_ordinal
                capture_ordinal += 1
                return {
                    "captured_monotonic_ns": capture_ordinal,
                    "load_1": 1.0,
                    "load_5": 1.0,
                    "load_15": 1.0,
                    "logical_cpu_count": 8,
                    "normalized_load_1": 0.125,
                    "available_memory_bytes": 8 * 1024**3,
                    "cpu_governor_states": ["synthetic:performance"],
                }
            load_state_samples.append({
                "phase": phase,
                "before": snapshot(),
                "after": snapshot(),
            })
    tracked_diff_sha = (
        sha256_bytes(b"")
        if role == "stage0"
        else sha256_bytes(b"synthetic relevant change")
    )
    source_entry = {
        "path": "main/CMakeLists.txt",
        "bytes": 1,
        "sha256": sha256_bytes(b"x"),
    }
    source_digest = hashlib.sha256()
    source_digest.update(b"\0policy\0")
    source_digest.update(SOURCE_FINGERPRINT_POLICY.encode("ascii"))
    source_digest.update(b"\0tracked-diff\0")
    source_digest.update(tracked_diff_sha.encode("ascii"))
    source_digest.update(b"\0entry\0")
    source_digest.update(source_entry["path"].encode("utf-8"))
    source_digest.update(b"\0")
    source_digest.update(str(source_entry["bytes"]).encode("ascii"))
    source_digest.update(b"\0")
    source_digest.update(source_entry["sha256"].encode("ascii"))

    def synthetic_file_inventory(
        path: str,
        content_sha: str,
    ) -> dict[str, Any]:
        entry = {"path": path, "bytes": 1, "sha256": content_sha}
        digest = hashlib.sha256()
        digest.update(path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(b"1")
        digest.update(b"\0")
        digest.update(content_sha.encode("ascii"))
        return {
            "inventory_sha256": digest.hexdigest(),
            "entries": [entry],
        }

    primed_outputs = synthetic_file_inventory(
        "/synthetic/build/final.stamp", "e" * 64
    )
    primed_dependencies = synthetic_file_inventory(
        "/synthetic/build/primed.dependency", "f" * 64
    )
    generator_boundary_samples = []
    for index in range(MIN_SAMPLES):
        wrapper_pid = 100 + index * 10
        generator_pid = wrapper_pid + 1
        generator_path = runtime_artifacts[
            "generator_executable"
        ]["path"]
        cold_boundary = {
            "expected_generator": True,
            "generator_executable": generator_path,
            "generator_pids": [generator_pid],
            "unexpected_pre_generator_processes": [],
            "transcript_sha256": sha256_bytes(
                (
                    f"{generator_path} sql generate --stage final\n"
                ).encode("utf-8")
            ),
            "transcript_base64": base64.b64encode(
                (
                    f"{generator_path} sql generate --stage final\n"
                ).encode("utf-8")
            ).decode("ascii"),
            "observed_processes": [
                {
                    "pid": wrapper_pid,
                    "ppid": 0,
                    "commands": ["/usr/bin/time cmake --build"],
                },
                {
                    "pid": generator_pid,
                    "ppid": wrapper_pid,
                    "commands": [
                        f"{generator_path} sql generate --stage final"
                    ],
                },
            ],
        }
        noop_boundary = {
            "expected_generator": False,
            "generator_executable": generator_path,
            "generator_pids": [],
            "unexpected_pre_generator_processes": [],
            "transcript_sha256": sha256_bytes(
                b"ninja: no work to do.\n"
            ),
            "transcript_base64": base64.b64encode(
                b"ninja: no work to do.\n"
            ).decode("ascii"),
            "observed_processes": [{
                "pid": wrapper_pid + 2,
                "ppid": 0,
                "commands": ["/usr/bin/time cmake --build"],
            }],
        }
        generator_boundary_samples.append({
            "removed_output_inventory_sha256":
                primed_outputs["inventory_sha256"],
            "regenerated_output_inventory_sha256":
                primed_outputs["inventory_sha256"],
            "dependency_inventory_sha256":
                primed_dependencies["inventory_sha256"],
            "cold_process_boundary": cold_boundary,
            "noop_process_boundary": noop_boundary,
        })
    return sealed({
        "schema_version": SCHEMA_VERSION,
        "artifact_kind": "measurement",
        "role": role,
        "sample_policy": {
            "repetitions": 3,
            "aggregate": "median",
            "parallelism": 1,
            "generator_target": "bioseq_sql_final_production",
            "affected_targets": [
                "bioseq_bench_e2e", "postgresql_bioseq"
            ],
        },
        "source": {
            "head": "b" * 40,
            "dirty": role != "stage0",
            "tree_sha256": source_digest.hexdigest(),
            "tracked_diff_sha256": tracked_diff_sha,
            "tracked_diff_base64": base64.b64encode(
                b""
                if role == "stage0"
                else b"synthetic relevant change"
            ).decode("ascii"),
            "untracked_files": [],
            "inventory_policy": SOURCE_FINGERPRINT_POLICY,
            "inventory_pathspecs": list(SOURCE_FINGERPRINT_GIT_PATHS),
            "inventory_entries": [source_entry],
        },
        "environment": synthetic_environment,
        "samples": samples,
        "aggregates": {
            metric: 110 for metric in BUILD_SAMPLE_METRICS
        },
        "artifacts": artifacts,
        "runtime_artifacts": runtime_artifacts,
        "measurement_context": {
            "load_state_samples": load_state_samples,
            "generator_boundary": {
                "output_inventory_file_sha256": "d" * 64,
                "timed_generator_artifact":
                    runtime_artifacts["generator_executable"],
                "primed_outputs": primed_outputs,
                "primed_dependencies": primed_dependencies,
                "post_compile_outputs": primed_outputs,
                "post_compile_dependencies": primed_dependencies,
                "samples": generator_boundary_samples,
            },
        },
    })


def sealed(value: dict[str, Any]) -> dict[str, Any]:
    result = dict(value)
    result["content_sha256"] = canonical_json_sha256(result)
    return result


def validate_benchmark_configuration(
    path: Path,
    *,
    calibration: bool,
) -> None:
    value = load_json(path)
    expected_sha256 = (
        EXPECTED_CALIBRATION_CONFIGURATION_SHA256
        if calibration
        else EXPECTED_BENCHMARK_CONFIGURATION_SHA256
    )
    if canonical_json_sha256(value) != expected_sha256:
        fail(
            "configuration content differs from the reviewed exact "
            f"{'calibration' if calibration else 'qualification'} "
            "rule map"
        )
    test_cases = value.get("test_cases")
    if (
        not isinstance(test_cases, dict)
        or set(test_cases) != {"SQL_CODEGEN_BOUNDARY_SMALL"}
        or test_cases["SQL_CODEGEN_BOUNDARY_SMALL"].get(
            "concurrency"
        ) != 1
    ):
        fail("configuration test-case concurrency is not pinned to one")
    benchmark = value.get("bench_sql_codegen_guardrail")
    if not isinstance(benchmark, dict):
        fail("configuration has no bench_sql_codegen_guardrail object")
    if "external" in str(benchmark.get("description", "")).lower():
        fail("configuration describes guardrail evidence as external")
    if benchmark.get("concurrency") != {
        "bench_lock": True,
        "concurrency": False,
    }:
        fail("configuration benchmark concurrency is not strictly serial")
    if benchmark.get("iteration_order") != {
        "mode": "interleaved_seeded",
        "seed": 388,
    }:
        fail("configuration iteration order is not exact")
    matrix = benchmark.get("matrix")
    if not isinstance(matrix, dict):
        fail("configuration has no SQL guardrail matrix")
    if matrix.get("for") != {"SQL_CODEGEN_BOUNDARY_SMALL": 15}:
        fail("configuration matrix.for is not pinned to 15")
    if matrix.get("by_regions") is not False:
        fail("configuration by_regions must remain false")
    over = matrix.get("over")
    if not isinstance(over, dict) or tuple(over) != BENCHMARK_ALGORITHMS:
        fail("configuration algorithm matrix is not exact or ordered")
    acceptance = matrix.get("acceptance")
    if not isinstance(acceptance, dict):
        fail("configuration has no matrix acceptance contract")
    if tuple(acceptance.get("exact_algorithm_rows", ())) != (
        BENCHMARK_ALGORITHMS
    ):
        fail("configuration exact_algorithm_rows is not exact or ordered")
    iterations = acceptance.get("exact_test_case_iterations")
    if iterations != {"SQL_CODEGEN_BOUNDARY_SMALL": 15}:
        fail("configuration iteration acceptance is not pinned to 15")
    metrics = matrix.get("metrics")
    if not isinstance(metrics, dict) or tuple(metrics) != BENCHMARK_METRICS:
        fail("configuration metric set is not exact or ordered")
    rule_names: set[str] = set()
    allowed_comparisons = {"<", ">", "="}
    allowed_aggregates = {"min", "max", "mean", "p50"}
    allowed_pairings = {
        "independent",
        "by_iteration_exact",
    }
    for metric in BENCHMARK_METRICS:
        item = metrics[metric]
        if not isinstance(item, dict):
            fail(f"configuration metric {metric} is not an object")
        rules = item.get("rules")
        if not isinstance(rules, list):
            fail(f"configuration metric {metric} has no rules list")
        if not rules:
            fail(f"qualification metric {metric} has empty rules")
        for index, rule in enumerate(rules):
            if not isinstance(rule, list) or len(rule) != 4:
                fail(
                    f"configuration metric {metric} rule {index} "
                    "is not a four-part comparison"
                )
            name, left, comparator, right = rule
            if not isinstance(name, str) or not name.strip():
                fail(
                    f"configuration metric {metric} rule {index} "
                    "has no name"
                )
            if name in rule_names:
                fail(f"configuration duplicates rule name: {name}")
            rule_names.add(name)
            for side_name, selector in (("left", left), ("right", right)):
                if not isinstance(selector, dict):
                    fail(
                        f"configuration rule {name} {side_name} selector "
                        "is not an object"
                    )
                if set(selector) - {"key", "test_case"}:
                    fail(
                        f"configuration rule {name} {side_name} selector "
                        "has unknown fields"
                    )
                if selector.get("key") not in BENCHMARK_ALGORITHMS:
                    fail(
                        f"configuration rule {name} {side_name} selector "
                        "has an unknown algorithm"
                    )
                if selector.get(
                    "test_case", "SQL_CODEGEN_BOUNDARY_SMALL"
                ) != "SQL_CODEGEN_BOUNDARY_SMALL":
                    fail(
                        f"configuration rule {name} {side_name} selector "
                        "has an unknown test case"
                    )
            if not isinstance(comparator, dict):
                fail(f"configuration rule {name} comparator is not an object")
            if set(comparator) - {
                "compare",
                "threshold%",
                "tolerance%",
                "aggregate",
                "pairing",
                "blocking",
            }:
                fail(
                    f"configuration rule {name} comparator has "
                    "unknown fields"
                )
            if comparator.get("compare") not in allowed_comparisons:
                fail(f"configuration rule {name} compare is invalid")
            if comparator.get("aggregate") not in allowed_aggregates:
                fail(f"configuration rule {name} aggregate is invalid")
            pairing = comparator.get("pairing")
            if pairing is not None and pairing not in allowed_pairings:
                fail(f"configuration rule {name} pairing is invalid")
            if comparator.get("blocking", True) is not True:
                fail(f"configuration rule {name} is not blocking")
            for numeric_field in ("threshold%", "tolerance%"):
                if numeric_field in comparator:
                    nonnegative_number(
                        comparator[numeric_field],
                        f"configuration rule {name} {numeric_field}",
                    )


def command_self_test(arguments: argparse.Namespace) -> None:
    worker_state_authority = {
        "worker_pid": os.getpid(),
        "worker_session_id": os.getsid(0),
        "worker_started_at": 1.0,
        "workflow_request": {
            "file_sha256": "1" * 64,
            "content_sha256": "2" * 64,
            "normalized_sha256": "3" * 64,
        },
    }
    running_worker_state = {
        "bench-e2e": {
            "status": "running",
            "pid": os.getpid(),
            "started_at": 1.0,
            "sql_guardrail_request_sha256": "3" * 64,
        },
    }
    validate_mcp_worker_state(
        worker_state_authority, running_worker_state
    )
    completed_worker_state = json.loads(
        json.dumps(running_worker_state)
    )
    completed_worker_state["bench-e2e"]["status"] = "completed"
    try:
        validate_mcp_worker_state(
            worker_state_authority, completed_worker_state
        )
    except GuardrailError:
        pass
    else:
        fail(
            "self-test accepted replay after the MCP worker completed"
        )
    wrong_session_authority = dict(worker_state_authority)
    wrong_session_authority["worker_session_id"] = os.getsid(0) + 1
    try:
        validate_mcp_worker_state(
            wrong_session_authority, running_worker_state
        )
    except GuardrailError:
        pass
    else:
        fail(
            "self-test accepted an MCP authority from the wrong session"
        )

    global _process_table
    original_process_table = _process_table

    def delayed_process_table() -> dict[int, tuple[int, str]]:
        time.sleep(0.15)
        return {}

    _process_table = delayed_process_table
    try:
        short_probe = measured_generator_build(
            [
                sys.executable,
                "-c",
                "print('ninja: no work to do.')",
            ],
            generator_executable=Path(sys.executable),
            expect_generator=False,
        )
    finally:
        _process_table = original_process_table
    if (
        short_probe["monitor_join_ns"] < 50_000_000
        or short_probe["elapsed_ns"] >= short_probe["monitor_join_ns"]
    ):
        fail(
            "self-test found that delayed process monitoring inflated a "
            "short-lived command's elapsed time"
        )

    stage0 = synthetic_measurement("stage0")
    validate_measurement(stage0)
    compiler_child = json.loads(json.dumps(stage0))
    compiler_child["measurement_context"]["generator_boundary"][
        "samples"
    ][0]["cold_process_boundary"]["observed_processes"].append({
        "pid": 999,
        "ppid": 100,
        "commands": ["/synthetic/clang++ -c escaped.cpp"],
    })
    compiler_child = sealed({
        key: value
        for key, value in compiler_child.items()
        if key != "content_sha256"
    })
    try:
        validate_measurement(compiler_child)
    except GuardrailError:
        pass
    else:
        fail(
            "self-test accepted a compiler process outside the generator "
            "subtree"
        )
    stale_output = json.loads(json.dumps(stage0))
    stale_output["measurement_context"]["generator_boundary"][
        "samples"
    ][0]["regenerated_output_inventory_sha256"] = "0" * 64
    stale_output = sealed({
        key: value
        for key, value in stale_output.items()
        if key != "content_sha256"
    })
    try:
        validate_measurement(stale_output)
    except GuardrailError:
        pass
    else:
        fail(
            "self-test accepted a stale output escaping the cold "
            "generator boundary"
        )
    replaced_generator = json.loads(json.dumps(stage0))
    replaced_generator["measurement_context"]["generator_boundary"][
        "timed_generator_artifact"
    ]["sha256"] = "0" * 64
    replaced_generator = sealed({
        key: value
        for key, value in replaced_generator.items()
        if key != "content_sha256"
    })
    try:
        validate_measurement(replaced_generator)
    except GuardrailError:
        pass
    else:
        fail(
            "self-test accepted the same generator path with different "
            "timed and retained bytes"
        )

    def synthetic_host_snapshot(ordinal: int) -> dict[str, Any]:
        return {
            "captured_monotonic_ns": ordinal,
            "load_1": 1.0,
            "load_5": 1.0,
            "load_15": 1.0,
            "logical_cpu_count": 8,
            "normalized_load_1": 0.125,
            "available_memory_bytes": 8 * 1024**3,
            "cpu_governor_states": ["synthetic:performance"],
        }

    def synthetic_host_provenance(index: int) -> dict[str, Any]:
        return {
            "before_file_sha256": str(index) * 64,
            "before_content_sha256": str(index + 1) * 64,
            "before": synthetic_host_snapshot(index * 2 - 1),
            "after_file_sha256": str(index + 2) * 64,
            "after_content_sha256": str(index + 3) * 64,
            "after": synthetic_host_snapshot(index * 2),
        }

    runtime = sealed({
        "schema_version": SCHEMA_VERSION,
        "artifact_kind": "runtime_samples",
        "sample_unit": "per_report_p50",
        "measurement_sha256": "9" * 64,
        "measurement_content_sha256": stage0["content_sha256"],
        "source": stage0["source"],
        "environment": stage0["environment"],
        "provenance": {
            "kind": "bioseq_benchmark_reports",
            "reports": [
                {
                    "path": f"/synthetic/report_{index}.json",
                    "sha256": str(index) * 64,
                    "run_nonce": index,
                    "meta": {"history_eligible": True},
                    "host_state": synthetic_host_provenance(index),
                    "paired_runtime_samples": {
                        "file_sha256": str(index + 4) * 64,
                        "content_sha256": str(index + 5) * 64,
                    },
                }
                for index in range(1, 4)
            ],
        },
        "samples": {
            metric: (
                [1.0, 1.1, 1.2]
                if metric.endswith("_ratio")
                else [1_000, 1_100, 1_200]
            )
            for metric in RUNTIME_SAMPLE_METRICS
        },
    })
    validate_runtime_samples(runtime)
    baselines = metric_baselines(stage0, runtime["samples"])
    limits = {
        metric: baselines[metric] * 2.0
        for metric in DIRECT_LIMIT_METRICS
    }
    limits.update({
        metric: 100.0 for metric in COMPILE_DELTA_METRICS
    })
    limits.update({
        "measurement_normalized_load_1_max": 0.25,
        "measurement_normalized_load_1_delta_max": 0.125,
        "measurement_available_memory_bytes_min": 4 * 1024**3,
        "measurement_available_memory_drop_percent_max": 50.0,
        "measurement_cpu_governor_states": [
            "synthetic:performance"
        ],
    })
    limits_document = sealed({
        "schema_version": SCHEMA_VERSION,
        "artifact_kind": "doc0388_sql_codegen_guardrail_limits",
        "canonical_environment_key": stage0["environment"]["sha256"],
        "measurement_sha256": "9" * 64,
        "measurement_content_sha256": stage0["content_sha256"],
        "runtime_samples_sha256": "8" * 64,
        "runtime_samples_content_sha256": runtime["content_sha256"],
        "review_notes": "synthetic reviewed tolerances",
        "limits": limits,
    })
    if validate_limits_document(
        limits_document,
        measurement=stage0,
        measurement_sha256="9" * 64,
        runtime_samples=runtime,
        runtime_samples_sha256="8" * 64,
        baselines=baselines,
    ) != limits:
        fail("self-test limits document validation changed reviewed limits")
    stage0_context_summary = summarize_measurement_context(
        stage0["measurement_context"],
        environment=stage0["environment"],
        repetitions=stage0["sample_policy"]["repetitions"],
        label="self-test baseline",
    )
    baseline = sealed({
        "schema_version": SCHEMA_VERSION,
        "artifact_kind": "reviewed_baseline",
        "status": "reviewed",
        "baseline_kind": "stage0",
        "review": {
            "reviewer": "self-test",
            "evidence_reference": "synthetic",
            "spec_deviation": None,
        },
        "measurement_sha256": "e" * 64,
        "runtime_samples_sha256": "f" * 64,
        "limits_source_sha256": "1" * 64,
        "capture_bundle_sha256": "4" * 64,
        "review_request_sha256": "5" * 64,
        "review_request_content_sha256": "6" * 64,
        "source": stage0["source"],
        "environment": stage0["environment"],
        "canonical_environment_key": stage0["environment"]["sha256"],
        "sample_policy": stage0["sample_policy"],
        "measurement_context": stage0["measurement_context"],
        "measurement_context_summary": stage0_context_summary,
        "baselines": baselines,
        "limits": limits,
        "runtime_samples": runtime["samples"],
        "runtime_provenance": runtime["provenance"],
        "runtime_context_summary": summarize_runtime_context(
            runtime["provenance"]["reports"],
            environment=stage0["environment"],
            label="self-test runtime baseline",
        ),
        "expected_artifacts": stage0["artifacts"],
        "expected_runtime_artifacts": stage0["runtime_artifacts"],
    })
    validate_reviewed_baseline(baseline)

    recovered = json.loads(json.dumps(baseline))
    recovered["baseline_kind"] = "recovered_post_cutover"
    recovered["review"]["spec_deviation"] = None
    recovered = sealed({
        key: value for key, value in recovered.items()
        if key != "content_sha256"
    })
    try:
        validate_reviewed_baseline(recovered)
    except GuardrailError:
        pass
    else:
        fail("self-test accepted recovered data without a deviation")

    tampered = json.loads(json.dumps(baseline))
    tampered["limits"]["generator_cold_ns"] += 1.0
    try:
        validate_reviewed_baseline(tampered)
    except GuardrailError:
        pass
    else:
        fail("self-test accepted a tampered baseline seal")

    candidate = synthetic_measurement("candidate")
    validate_measurement(candidate)
    observed = {
        metric: float(candidate["aggregates"][metric])
        for metric in BUILD_SAMPLE_METRICS
    }
    for group in ARTIFACT_GROUPS:
        observed[f"generated_{group}_bytes"] = float(
            candidate["artifacts"][group]["bytes"]
        )
    observed.update({
        metric: 0.0 for metric in COMPILE_DELTA_METRICS
    })
    candidate_context_summary = summarize_measurement_context(
        candidate["measurement_context"],
        environment=candidate["environment"],
        repetitions=candidate["sample_policy"]["repetitions"],
        label="self-test qualification",
    )
    observed.update({
        "measurement_normalized_load_1_max":
            candidate_context_summary["normalized_load_1_max"],
        "measurement_available_memory_bytes_min":
            candidate_context_summary["available_memory_bytes_min"],
        "measurement_normalized_load_1_delta": 0.0,
        "measurement_available_memory_drop_percent": 0.0,
    })
    qualification = sealed({
        "schema_version": SCHEMA_VERSION,
        "artifact_kind": "qualification",
        "status": "pass",
        "baseline_kind": "stage0",
        "baseline_sha256": "2" * 64,
        "baseline_content_sha256": baseline["content_sha256"],
        "measurement_sha256": "3" * 64,
        "qualification_request": {
            "file_sha256": "7" * 64,
            "content_sha256": "8" * 64,
            "normalized_sha256": "9" * 64,
        },
        "baseline_review": baseline["review"],
        "source": candidate["source"],
        "environment": candidate["environment"],
        "canonical_environment_key":
            candidate["environment"]["sha256"],
        "sample_policy": candidate["sample_policy"],
        "measurement_context": candidate["measurement_context"],
        "measurement_context_summary": candidate_context_summary,
        "baselines": baselines,
        "observed": observed,
        "limits": limits,
        "expected_artifacts": candidate["artifacts"],
        "expected_runtime_artifacts": candidate["runtime_artifacts"],
    })
    validate_qualification(qualification)
    try:
        validate_live_baseline_authority(qualification)
    except GuardrailError:
        pass
    else:
        fail(
            "self-test accepted a Stage-0-labelled live qualification "
            "without a pinned historical baseline hash"
        )

    with tempfile.TemporaryDirectory(
        prefix="bioseq-sql-baseline-join-self-test-"
    ) as temporary:
        baseline_path = Path(temporary) / "baseline.json"
        write_json(baseline_path, baseline)
        joined_qualification = json.loads(json.dumps(qualification))
        joined_qualification["baseline_sha256"] = sha256_file(
            baseline_path
        )
        joined_qualification = sealed({
            key: value
            for key, value in joined_qualification.items()
            if key != "content_sha256"
        })
        validate_qualification_baseline_join(
            joined_qualification, baseline_path
        )
        inflated = json.loads(json.dumps(joined_qualification))
        inflated["limits"]["generator_cold_ns"] *= 10.0
        inflated = sealed({
            key: value
            for key, value in inflated.items()
            if key != "content_sha256"
        })
        validate_qualification(inflated)
        try:
            validate_qualification_baseline_join(
                inflated, baseline_path
            )
        except GuardrailError:
            pass
        else:
            fail(
                "self-test accepted a self-sealed qualification with "
                "limits that differ from the exact reviewed baseline"
            )

    qualification["observed"]["generator_cold_ns"] = (
        limits["generator_cold_ns"] + 1.0
    )
    qualification = sealed({
        key: value for key, value in qualification.items()
        if key != "content_sha256"
    })
    try:
        validate_qualification(qualification)
    except GuardrailError:
        pass
    else:
        fail("self-test accepted an over-limit qualification")

    def report_metric_from_samples(
        raw_samples: list[float],
    ) -> dict[str, Any]:
        if len(raw_samples) != 15:
            fail("self-test report metric requires exactly 15 samples")
        values = sorted(float(value) for value in raw_samples)
        total = sum(values)
        mean = total / len(values)
        trim = int(math.floor(0.1 * len(values)))
        mean90 = sum(values[trim:-trim]) / (
            len(values) - 2 * trim
        )
        stddev = math.sqrt(sum(
            (value - mean) ** 2 for value in values
        ) / (len(values) - 1))
        width = (values[-1] - values[0]) / 10.0
        counts = [0] * 10
        if width > 0.0:
            for value in values:
                bucket = int(math.floor(
                    (value - values[0]) / width
                ))
                counts[min(bucket, 9)] += 1
        else:
            counts[0] = len(values)
        return {
            "samples_count": len(values),
            "stats": {
                "min": values[0],
                "max": values[-1],
                "mean": mean,
                "mean90": mean90,
                "sum": total,
                "stddev": stddev,
            },
            "percentiles": {
                name: values[int(math.floor(
                    fraction * (len(values) - 1)
                ))]
                for name, fraction in {
                    "P01": 0.01,
                    "P20": 0.20,
                    "P50": 0.50,
                    "P80": 0.80,
                    "P99": 0.99,
                }.items()
            },
            "k-best": {
                "top10": list(reversed(values[-10:])),
                "bottom10": values[:10],
            },
            "histogram": [
                {
                    "from": values[0] + width * index,
                    "to": (
                        values[-1]
                        if index == 9
                        else values[0] + width * (index + 1)
                    ),
                    "count": counts[index],
                }
                for index in range(10)
            ],
        }

    def report_metric(value: float) -> dict[str, Any]:
        return report_metric_from_samples([value] * 15)

    reconstructed_probe = {"probe": report_metric(7.0)}
    if report_metric_p50(
        reconstructed_probe, "probe", positive=True
    ) != 7.0:
        fail("self-test raw report reconstruction changed the P50")
    forged_percentile = json.loads(json.dumps(reconstructed_probe))
    forged_percentile["probe"]["percentiles"]["P50"] = 8.0
    try:
        report_metric_p50(
            forged_percentile, "probe", positive=True
        )
    except GuardrailError:
        pass
    else:
        fail("self-test accepted a forged framework P50")
    median_only_pass = {
        "probe": report_metric_from_samples(
            [0.0] * 7 + [1.0] * 8
        ),
    }
    if report_metric_p50(
        median_only_pass, "probe", positive=False
    ) != 1.0:
        fail("self-test median-only pass fixture is malformed")
    try:
        require_report_metric_constant(
            median_only_pass,
            "probe",
            1.0,
            context="self-test every-iteration invariant",
        )
    except GuardrailError:
        pass
    else:
        fail(
            "self-test accepted failed raw iterations behind a passing "
            "median"
        )
    large_nonce = (1 << 53) - 4096
    forged_nonce = {
        "run_nonce": report_metric_from_samples(
            [float(large_nonce)] * 14
            + [float(large_nonce + 1000)]
        ),
    }
    try:
        require_report_nonce(
            forged_nonce,
            large_nonce,
            context="self-test large workflow nonce",
        )
    except GuardrailError:
        pass
    else:
        fail(
            "self-test accepted a near-2^53 nonce mutation hidden by "
            "relative floating-point tolerance"
        )

    with tempfile.TemporaryDirectory(
        prefix="bioseq-sql-guardrail-self-test-"
    ) as temporary:
        directory = Path(temporary)
        measurement_path = directory / "measurement.json"
        write_json(measurement_path, stage0)
        report_paths = []
        host_state_before_paths = []
        host_state_after_paths = []
        paired_runtime_paths = []
        for report_index in range(MIN_SAMPLES):
            execution_tuples = []
            for algorithm in BENCHMARK_ALGORITHMS:
                metrics = {
                    metric: report_metric(0.0)
                    for metric in BENCHMARK_METRICS
                }
                metrics.update({
                    "wall_clock_ns": report_metric(
                        2_000.0 + report_index
                    ),
                    "query_ns": report_metric(
                        0.0
                        if algorithm == "qualification_reference"
                        else 1_000.0 + report_index
                    ),
                    "qualification_baseline_kind": report_metric(0.0),
                    "build_qualification_passed": report_metric(0.0),
                    "correctness_passed": report_metric(
                        0.0
                        if algorithm == "qualification_reference"
                        else 1.0
                    ),
                    "performance_gate_passed": report_metric(
                        0.0
                        if algorithm == "qualification_reference"
                        else 1.0
                    ),
                    "live_measurement_attested": report_metric(
                        0.0
                        if algorithm == "qualification_reference"
                        else 1.0
                    ),
                    "run_nonce": report_metric(
                        0.0
                        if algorithm == "qualification_reference"
                        else float(report_index + 1)
                    ),
                    "arity": report_metric(
                        EXPECTED_ALGORITHM_ARITIES[algorithm]
                    ),
                    "generated_query_ns": report_metric(
                        0.0
                        if algorithm == "qualification_reference"
                        else 1_000.0 + report_index
                    ),
                    "handwritten_query_ns": report_metric(
                        900.0 + report_index
                        if algorithm in {
                            "typed_wrapper_generated_vs_handwritten",
                            "row_codec_generated_vs_handwritten",
                        }
                        else 0.0
                    ),
                    "generated_to_handwritten_ratio": report_metric(
                        (
                            1_000.0 + report_index
                        ) / (
                            900.0 + report_index
                        )
                        if algorithm in {
                            "typed_wrapper_generated_vs_handwritten",
                            "row_codec_generated_vs_handwritten",
                        }
                        else 0.0
                    ),
                    "generated_typed_wrapper": report_metric(
                        1.0
                        if algorithm in {
                            "from_array_arity_2",
                            "from_array_arity_3",
                            "typed_wrapper_generated_vs_handwritten",
                        }
                        else 0.0
                    ),
                    "direct_fmgr_wrapper_path": report_metric(
                        1.0
                        if algorithm in {
                            "from_array_arity_2",
                            "from_array_arity_3",
                            "typed_wrapper_generated_vs_handwritten",
                        }
                        else 0.0
                    ),
                    "planner_custom_scan_path": report_metric(
                        1.0
                        if algorithm in {
                            "mmsearch_arity_2",
                            "mmsearch_arity_3",
                        }
                        else 0.0
                    ),
                    "generated_row_codec_fields": report_metric(
                        EXPECTED_ROW_CODEC_FIELD_COUNTS.get(
                            algorithm,
                            0.0,
                        )
                    ),
                })
                execution_tuples.append({
                    "tuple": {algorithm: {"_": 0}},
                    "test_cases": {
                        "SQL_CODEGEN_BOUNDARY_SMALL": {
                            "all_regions": metrics,
                        },
                    },
                })
            report_path = directory / f"report_{report_index}.json"
            write_json(
                report_path,
                {
                    "meta": {
                        "self_test_run": report_index,
                        "commit": stage0["source"]["head"],
                        "dirty": stage0["source"]["dirty"],
                        "history_eligible": True,
                    },
                    "benchmarks": {
                        "bench_sql_codegen_guardrail": {
                            "execution_tuples": execution_tuples,
                        },
                    },
                    "rule_violations": [],
                    "regressions": [],
                    "sanity_warnings": [],
                },
            )
            report_paths.append(report_path)
            paired_raw_path = (
                directory / f"report_{report_index}_paired_raw.jsonl"
            )
            paired_records = []
            for algorithm in COMPARATOR_ALGORITHMS:
                for ordinal in range(1, 16):
                    generated = 1_000 + report_index
                    handwritten = 900 + report_index
                    paired_records.append({
                        "schema_version": SCHEMA_VERSION,
                        "run_nonce": report_index + 1,
                        "algorithm": algorithm,
                        "sample_ordinal": ordinal,
                        "generated_query_ns": generated,
                        "handwritten_query_ns": handwritten,
                        "generated_to_handwritten_ratio":
                            generated / handwritten,
                    })
            paired_raw_path.write_text(
                "".join(
                    json.dumps(record, sort_keys=True) + "\n"
                    for record in paired_records
                ),
                encoding="utf-8",
            )
            paired_path = (
                directory / f"report_{report_index}_paired.json"
            )
            command_seal_paired_runtime_samples(argparse.Namespace(
                raw_jsonl=paired_raw_path,
                report=report_path,
                measurement=measurement_path,
                run_nonce=report_index + 1,
                output=paired_path,
            ))
            paired_runtime_paths.append(paired_path)
            for phase, ordinal, target_list in (
                (
                    "before",
                    report_index * 2 + 1,
                    host_state_before_paths,
                ),
                (
                    "after",
                    report_index * 2 + 2,
                    host_state_after_paths,
                ),
            ):
                state_path = (
                    directory
                    / f"report_{report_index}_{phase}_state.json"
                )
                write_json(
                    state_path,
                    sealed({
                        "schema_version": SCHEMA_VERSION,
                        "artifact_kind": "runtime_host_state",
                        "phase": phase,
                        "run_nonce": report_index + 1,
                        "measurement_sha256":
                            sha256_file(measurement_path),
                        "measurement_content_sha256":
                            stage0["content_sha256"],
                        "source": stage0["source"],
                        "environment": stage0["environment"],
                        "snapshot": synthetic_host_snapshot(ordinal),
                    }),
                )
                target_list.append(state_path)
        extracted_path = directory / "runtime-samples.json"
        command_extract_runtime_samples(argparse.Namespace(
            report=report_paths,
            host_state_before=host_state_before_paths,
            host_state_after=host_state_after_paths,
            paired_runtime_samples=paired_runtime_paths,
            measurement=measurement_path,
            output=extracted_path,
        ))
        validate_runtime_samples(load_json(extracted_path))
        forged_pairs = load_json(paired_runtime_paths[0])
        forged_pairs["algorithms"][
            "typed_wrapper_generated_vs_handwritten"
        ][0]["generated_to_handwritten_ratio"] += 0.25
        forged_pairs = sealed({
            key: value
            for key, value in forged_pairs.items()
            if key != "content_sha256"
        })
        try:
            validate_paired_runtime_samples(
                forged_pairs,
                measurement=stage0,
                measurement_sha256=sha256_file(measurement_path),
                report_path=report_paths[0],
                expected_run_nonce=1,
            )
        except GuardrailError:
            pass
        else:
            fail(
                "self-test accepted a forged paired ratio that does not "
                "recompute from its exact generated/handwritten timings"
            )
        extra_axis_report = load_json(report_paths[0])
        extra_axis_report["benchmarks"][
            "bench_sql_codegen_guardrail"
        ]["execution_tuples"][0]["tuple"]["unreviewed_axis"] = {
            "_": 0
        }
        extra_axis_path = directory / "report_extra_axis.json"
        write_json(extra_axis_path, extra_axis_report)
        try:
            exact_sql_guardrail_report_metrics(
                extra_axis_path,
                expected_source_head=stage0["source"]["head"],
            )
        except GuardrailError:
            pass
        else:
            fail(
                "self-test accepted an unreviewed extra benchmark matrix "
                "axis"
            )

    with tempfile.TemporaryDirectory(
        prefix="bioseq-sql-source-fingerprint-self-test-"
    ) as temporary:
        repository = Path(temporary)
        relevant = repository / "main/CMakeLists.txt"
        documentation = (
            repository
            / "main/DOCS/CURRENT/REPORTS/synthetic-report.md"
        )
        relevant.parent.mkdir(parents=True, exist_ok=True)
        documentation.parent.mkdir(parents=True, exist_ok=True)
        relevant.write_text("cmake_minimum_required(VERSION 3.30)\n")
        documentation.write_text("initial report\n")
        run(["git", "init", str(repository)])
        run(["git", "-C", str(repository), "add", "."])
        run([
            "git", "-C", str(repository),
            "-c", "user.name=guardrail-self-test",
            "-c", "user.email=guardrail-self-test@example.invalid",
            "commit", "-m", "synthetic baseline",
        ])
        initial_source = source_fingerprint(repository)
        documentation.write_text("report-only edit\n")
        if source_fingerprint(repository) != initial_source:
            fail(
                "self-test found that a DOC/report-only edit invalidates "
                "the build-input fingerprint"
            )
        run(["git", "-C", str(repository), "add", "."])
        run([
            "git", "-C", str(repository),
            "-c", "user.name=guardrail-self-test",
            "-c", "user.email=guardrail-self-test@example.invalid",
            "commit", "-m", "documentation-only commit",
        ])
        documented_source = source_fingerprint(repository)
        if (
            documented_source["head"] == initial_source["head"]
            or source_build_identity(documented_source)
                != source_build_identity(initial_source)
        ):
            fail(
                "self-test found that a DOC/report-only commit invalidates "
                "the build-input identity"
            )
        relevant.write_text(
            "cmake_minimum_required(VERSION 3.31)\n"
        )
        changed_source = source_fingerprint(repository)
        if (
            changed_source == initial_source
            or changed_source["dirty"] is not True
        ):
            fail(
                "self-test found that a build-relevant source edit does "
                "not invalidate the fingerprint"
            )

    if arguments.configuration is not None:
        validate_benchmark_configuration(
            arguments.configuration, calibration=False
        )
        ordinary_value = load_json(arguments.configuration)
        ordinary_value["bench_sql_codegen_guardrail"]["matrix"][
            "metrics"
        ]["arity"]["rules"][1][2]["compare"] = "="
        with tempfile.TemporaryDirectory(
            prefix="bioseq-sql-config-self-test-"
        ) as temporary:
            mutated_path = Path(temporary) / "mutated.json"
            write_json(mutated_path, ordinary_value)
            try:
                validate_benchmark_configuration(
                    mutated_path, calibration=False
                )
            except GuardrailError:
                pass
            else:
                fail(
                    "self-test accepted a semantically weakened "
                    "qualification comparator"
                )
    if arguments.calibration_configuration is not None:
        validate_benchmark_configuration(
            arguments.calibration_configuration,
            calibration=True,
        )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    subparsers = result.add_subparsers(dest="command", required=True)

    mark = subparsers.add_parser(
        "mark-build",
        help="mark an already-configured build tree as destructive-test-only",
    )
    mark.add_argument("--repository-root", type=Path, required=True)
    mark.add_argument("--cmake-source-dir", type=Path, required=True)
    mark.add_argument("--build-dir", type=Path, required=True)
    mark.set_defaults(function=command_mark_build)

    measure = subparsers.add_parser(
        "measure",
        help="collect raw build/generator/size samples",
    )
    measure.add_argument("--repository-root", type=Path, required=True)
    measure.add_argument("--cmake-source-dir", type=Path, required=True)
    measure.add_argument("--build-dir", type=Path, required=True)
    measure.add_argument(
        "--role", choices=("stage0", "candidate"), required=True
    )
    measure.add_argument(
        "--generator-target",
        default="bioseq_sql_final_production",
    )
    measure.add_argument(
        "--affected-target",
        action="append",
        default=[],
    )
    measure.add_argument(
        "--artifact-inventory", type=Path, required=True
    )
    measure.add_argument(
        "--generator-output-inventory", type=Path, required=True
    )
    measure.add_argument(
        "--generator-executable", type=Path, required=True
    )
    measure.add_argument(
        "--production-dso", type=Path, required=True
    )
    measure.add_argument(
        "--benchmark-executable", type=Path, required=True
    )
    measure.add_argument("--comparator-dso", type=Path, required=True)
    measure.add_argument("--repetitions", type=int, default=3)
    measure.add_argument("--output", type=Path, required=True)
    measure.set_defaults(function=command_measure)

    bind_installed = subparsers.add_parser(
        "bind-installed-dso",
        help=(
            "finalize a pre-install measurement by proving the installed "
            "PostgreSQL DSO equals the measured production target"
        ),
    )
    bind_installed.add_argument(
        "--measurement", type=Path, required=True
    )
    bind_installed.add_argument(
        "--installed-production-dso", type=Path, required=True
    )
    bind_installed.add_argument("--output", type=Path, required=True)
    bind_installed.set_defaults(function=command_bind_installed_dso)

    capture_host_state = subparsers.add_parser(
        "capture-runtime-host-state",
        help=(
            "content-seal one before/after host-state boundary around a "
            "runtime benchmark report"
        ),
    )
    capture_host_state.add_argument(
        "--measurement", type=Path, required=True
    )
    capture_host_state.add_argument(
        "--phase", choices=("before", "after"), required=True
    )
    capture_host_state.add_argument(
        "--run-nonce", type=int, required=True
    )
    capture_host_state.add_argument("--output", type=Path, required=True)
    capture_host_state.set_defaults(
        function=command_capture_runtime_host_state
    )

    runtime_samples = subparsers.add_parser(
        "extract-runtime-samples",
        help=(
            "extract calibration P50 samples from independent "
            "bioseq benchmark reports"
        ),
    )
    runtime_samples.add_argument(
        "--report", type=Path, action="append", default=[]
    )
    runtime_samples.add_argument(
        "--host-state-before",
        type=Path,
        action="append",
        default=[],
    )
    runtime_samples.add_argument(
        "--host-state-after",
        type=Path,
        action="append",
        default=[],
    )
    runtime_samples.add_argument(
        "--paired-runtime-samples",
        type=Path,
        action="append",
        default=[],
    )
    runtime_samples.add_argument(
        "--measurement", type=Path, required=True
    )
    runtime_samples.add_argument("--output", type=Path, required=True)
    runtime_samples.set_defaults(function=command_extract_runtime_samples)

    seal_pairs = subparsers.add_parser(
        "seal-paired-runtime-samples",
        help=(
            "seal exact per-iteration generated/handwritten timing pairs "
            "and bind them to one benchmark report"
        ),
    )
    seal_pairs.add_argument("--raw-jsonl", type=Path, required=True)
    seal_pairs.add_argument("--report", type=Path, required=True)
    seal_pairs.add_argument("--measurement", type=Path, required=True)
    seal_pairs.add_argument("--run-nonce", type=int, required=True)
    seal_pairs.add_argument("--output", type=Path, required=True)
    seal_pairs.set_defaults(
        function=command_seal_paired_runtime_samples
    )

    validate_pairs = subparsers.add_parser(
        "validate-paired-runtime-samples"
    )
    validate_pairs.add_argument(
        "--paired-runtime-samples", type=Path, required=True
    )
    validate_pairs.add_argument("--report", type=Path, required=True)
    validate_pairs.add_argument(
        "--measurement", type=Path, required=True
    )
    validate_pairs.add_argument(
        "--expected-run-nonce", type=int, required=True
    )
    validate_pairs.set_defaults(
        function=command_validate_paired_runtime_samples
    )

    review = subparsers.add_parser(
        "review",
        help="seal reviewed Stage-0 or explicitly recovered evidence",
    )
    review.add_argument("--measurement", type=Path, required=True)
    review.add_argument("--runtime-samples", type=Path, required=True)
    review.add_argument("--limits", type=Path, required=True)
    review.add_argument("--capture-bundle-sha256", required=True)
    review.add_argument("--review-request-sha256", required=True)
    review.add_argument(
        "--review-request-content-sha256", required=True
    )
    review.add_argument(
        "--baseline-kind",
        choices=("stage0", "recovered_post_cutover"),
        required=True,
    )
    review.add_argument("--reviewer", required=True)
    review.add_argument("--evidence-reference", required=True)
    review.add_argument("--spec-deviation")
    review.add_argument("--approval-reference")
    review.add_argument("--output", type=Path, required=True)
    review.set_defaults(function=command_review)

    validate_baseline = subparsers.add_parser("validate-baseline")
    validate_baseline.add_argument("--baseline", type=Path, required=True)
    validate_baseline.set_defaults(function=command_validate_baseline)

    qualify = subparsers.add_parser(
        "qualify",
        help="compare current raw measurements with a reviewed baseline",
    )
    qualify.add_argument("--baseline", type=Path, required=True)
    qualify.add_argument("--measurement", type=Path, required=True)
    qualify.add_argument(
        "--qualification-request-sha256", required=True
    )
    qualify.add_argument(
        "--qualification-request-content-sha256", required=True
    )
    qualify.add_argument(
        "--qualification-request-normalized-sha256", required=True
    )
    qualify.add_argument("--output", type=Path, required=True)
    qualify.set_defaults(function=command_qualify)

    validate_qualification_parser = subparsers.add_parser(
        "validate-qualification"
    )
    validate_qualification_parser.add_argument(
        "--qualification", type=Path, required=True
    )
    validate_qualification_parser.set_defaults(
        function=command_validate_qualification
    )

    def add_live_arguments(
        live_parser: argparse.ArgumentParser,
    ) -> None:
        live_parser.add_argument(
            "--repository-root", type=Path, required=True
        )
        live_parser.add_argument("--build-dir", type=Path, required=True)
        live_parser.add_argument(
            "--artifact-inventory", type=Path, required=True
        )
        live_parser.add_argument(
            "--generator-executable", type=Path, required=True
        )
        live_parser.add_argument(
            "--production-dso", type=Path, required=True
        )
        live_parser.add_argument(
            "--benchmark-executable", type=Path, required=True
        )
        live_parser.add_argument(
            "--comparator-dso", type=Path, required=True
        )
        live_parser.add_argument(
            "--installed-production-dso", type=Path, required=True
        )

    validate_measurement_live = subparsers.add_parser(
        "validate-measurement-live"
    )
    validate_measurement_live.add_argument(
        "--measurement", type=Path, required=True
    )
    add_live_arguments(validate_measurement_live)
    validate_measurement_live.set_defaults(
        function=command_validate_measurement_live
    )

    validate_qualification_live = subparsers.add_parser(
        "validate-qualification-live"
    )
    validate_qualification_live.add_argument(
        "--qualification", type=Path, required=True
    )
    validate_qualification_live.add_argument(
        "--baseline", type=Path, required=True
    )
    validate_qualification_live.add_argument(
        "--mcp-authority", type=Path, required=True
    )
    validate_qualification_live.add_argument(
        "--mcp-authority-socket", type=Path, required=True
    )
    validate_qualification_live.add_argument(
        "--expected-run-nonce", type=int, required=True
    )
    add_live_arguments(validate_qualification_live)
    validate_qualification_live.set_defaults(
        function=command_validate_qualification_live
    )

    validate_report = subparsers.add_parser(
        "validate-qualification-report",
        help=(
            "independently validate the exact final qualification report "
            "matrix, nonce, evidence values, and reviewed limits"
        ),
    )
    validate_report.add_argument("--report", type=Path, required=True)
    validate_report.add_argument(
        "--measurement", type=Path, required=True
    )
    validate_report.add_argument(
        "--qualification", type=Path, required=True
    )
    validate_report.add_argument("--baseline", type=Path, required=True)
    validate_report.add_argument(
        "--host-state-before", type=Path, required=True
    )
    validate_report.add_argument(
        "--host-state-after", type=Path, required=True
    )
    validate_report.add_argument(
        "--paired-runtime-samples", type=Path, required=True
    )
    validate_report.add_argument(
        "--expected-run-nonce", type=int, required=True
    )
    validate_report.add_argument(
        "--qualification-request-sha256", required=True
    )
    validate_report.add_argument(
        "--qualification-request-content-sha256", required=True
    )
    validate_report.add_argument(
        "--qualification-request-normalized-sha256", required=True
    )
    validate_report.set_defaults(
        function=command_validate_qualification_report
    )

    self_test = subparsers.add_parser("self-test")
    self_test.add_argument("--configuration", type=Path)
    self_test.add_argument(
        "--calibration-configuration", type=Path
    )
    self_test.set_defaults(function=command_self_test)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    try:
        arguments.function(arguments)
    except GuardrailError as error:
        print(f"sql_codegen_guardrail: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
