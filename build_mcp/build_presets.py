"""Build preset and mode definitions."""

from __future__ import annotations

from typing import List, Dict, Set, Optional, Any, Mapping

import argparse
import base64
import contextlib
import ctypes
import errno
import fcntl
import hashlib
import json
import math
import os
import platform as host_platform
import re
import select
import secrets
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass, field
from pathlib import Path




import package_linux

_DEFAULT_REPOSITORY_ROOT = Path(__file__).resolve().parent.parent

REPOSITORY_ROOT = Path(
    os.environ.get(
        "BIOSEQ_BUILD_REPOSITORY_ROOT", str(_DEFAULT_REPOSITORY_ROOT))
).resolve(strict=False)

_HOST_DEFAULT_BUILD_PRESET = (
    "aws-x64-linux" if sys.platform.startswith("linux") else "bench-opt"
)

DEFAULT_BUILD_PRESET = os.environ.get(
    "BIOSEQ_BUILD_PRESET", _HOST_DEFAULT_BUILD_PRESET)

def _default_debug_preset(production_preset: str) -> str:
    mapped = {
        "bench-opt": "bench-opt-debug",
        "aws-x64-linux": "aws-x64-linux-debug",
        "aws-x64-linux-gcc161": "aws-x64-linux-gcc161-debug",
    }.get(production_preset)
    if mapped is not None:
        return mapped
    if production_preset.endswith("-debug"):
        return production_preset
    return f"{production_preset}-debug"

DEBUG_BUILD_PRESET = os.environ.get(
    "BIOSEQ_BUILD_PRESET_DEBUG",
    _default_debug_preset(DEFAULT_BUILD_PRESET),
)

def _default_test_preset(production_preset: str) -> str:
    mapped = {
        "bench-opt": "test-opt",
        "bench-opt-debug": "test-opt-debug",
        "aws-x64-linux": "test-aws-x64-linux",
        "aws-x64-linux-debug": "test-aws-x64-linux-debug",
        "aws-x64-linux-gcc161": "test-aws-x64-linux-gcc161",
        "aws-x64-linux-gcc161-debug":
            "test-aws-x64-linux-gcc161-debug",
    }.get(production_preset)
    if mapped is not None:
        return mapped
    if production_preset.startswith("test-"):
        return production_preset
    # Custom production presets follow the same fail-closed naming contract.
    # If test-<preset> is not declared, configure fails instead of silently
    # compiling guarded tests against the production tree.
    return f"test-{production_preset}"

TEST_BUILD_PRESET = os.environ.get(
    "BIOSEQ_TEST_BUILD_PRESET",
    _default_test_preset(DEFAULT_BUILD_PRESET),
)

TEST_DEBUG_BUILD_PRESET = os.environ.get(
    "BIOSEQ_TEST_BUILD_PRESET_DEBUG",
    _default_test_preset(DEBUG_BUILD_PRESET),
)

CACHE_BUILD_PRESET_OVERRIDE = os.environ.get("BIOSEQ_CACHE_BUILD_PRESET")

CACHE_DEBUG_BUILD_PRESET_OVERRIDE = os.environ.get(
    "BIOSEQ_CACHE_BUILD_PRESET_DEBUG")

CACHE_BIOSEQ_BUILD_PRESET_OVERRIDE = os.environ.get(
    "BIOSEQ_CACHE_BIOSEQ_BUILD_PRESET")

CACHE_BIOSEQ_DEBUG_BUILD_PRESET_OVERRIDE = os.environ.get(
    "BIOSEQ_CACHE_BIOSEQ_BUILD_PRESET_DEBUG")

def _external_api_build_preset(
        environment: Mapping[str, str], platform: str,
        default_build_preset: str) -> str:
    host_default = (
        "aws-x64-linux-gcc161"
        if platform.startswith("linux")
        else default_build_preset
    )
    return environment.get("BIOSEQ_EXTERNAL_API_BUILD_PRESET", host_default)

EXTERNAL_API_BUILD_PRESET = _external_api_build_preset(
    os.environ, sys.platform, DEFAULT_BUILD_PRESET)

WEBSITE_BUILD_PRESET = os.environ.get(
    "BIOSEQ_WEBSITE_BUILD_PRESET", "website-aws-x64-linux")

WEBSITE_DEBUG_BUILD_PRESET = os.environ.get(
    "BIOSEQ_WEBSITE_DEBUG_BUILD_PRESET", "website-aws-x64-linux-debug")

POSTGRES_ERROR_SAFETY_MODE = "postgres-error-safety"

PACKAGE_LINUX_MODE = package_linux.MODE

LINT_FOCUSED_MODE = "lint-focused"

LINT_FULL_MODE = "lint-full"

POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET = (
    "postgres-error-safety-production")

POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET = (
    "postgres-error-safety-test-indexes")

POSTGRES_ERROR_SAFETY_CASE_TEMPLATE = (
    REPOSITORY_ROOT
    / "codegen"
    / "safety"
    / "tests"
    / "integration"
    / "fixtures"
    / "postgres_error_safety_cases.json")

POSTGRES_ERROR_SAFETY_PEER_BUILD_DIR_CACHE_KEY = (
    "BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR")

DEFAULT_BUILD_DIR = REPOSITORY_ROOT / "build" / DEFAULT_BUILD_PRESET

REPORTING_DIR = REPOSITORY_ROOT / "reporting"

def _default_cache_preset(core_preset: str, *, combined: bool) -> str:
    """Map a complete core toolchain preset to its inherited cache variant."""
    mapped = {
        ("bench-opt", False): "cache-opt",
        ("bench-opt-debug", False): "cache-opt-debug",
        ("bench-opt", True): "cache-bioseq-opt",
        ("bench-opt-debug", True): "cache-bioseq-opt-debug",
        ("aws-x64-linux", False): "aws-x64-linux-cache",
        ("aws-x64-linux-debug", False): "aws-x64-linux-cache-debug",
        ("aws-x64-linux", True): "aws-x64-linux-cache-bioseq",
        ("aws-x64-linux-debug", True): "aws-x64-linux-cache-bioseq-debug",
        ("aws-x64-linux-gcc161", False): "aws-x64-linux-gcc161-cache",
        ("aws-x64-linux-gcc161-debug", False):
            "aws-x64-linux-gcc161-cache-debug",
        ("aws-x64-linux-gcc161", True):
            "aws-x64-linux-gcc161-cache-bioseq",
        ("aws-x64-linux-gcc161-debug", True):
            "aws-x64-linux-gcc161-cache-bioseq-debug",
    }.get((core_preset, combined))
    if mapped is not None:
        return mapped

    debug_suffix = "-debug" if core_preset.endswith("-debug") else ""
    base = core_preset.removesuffix("-debug")
    family_suffix = "-cache-bioseq" if combined else "-cache"
    return f"{base}{family_suffix}{debug_suffix}"

def preset_for(mode: str, debug: bool) -> str:
    # A mode may pin an explicit preset (e.g. external-api → GCC16 toolchain);
    # an override wins outright and is not split into debug/production families.
    mode_details = MODES.get(mode)
    if mode_details is None:
        raise RuntimeError(f"unknown build mode {mode!r}")
    if mode_details.preset_override:
        return mode_details.preset_override

    family = mode_details.preset_family
    production = DEBUG_BUILD_PRESET if debug else DEFAULT_BUILD_PRESET
    if family == "core":
        return production
    if family == "test":
        selected = TEST_DEBUG_BUILD_PRESET if debug else TEST_BUILD_PRESET
        if selected == production:
            raise RuntimeError(
                f"{mode} requires an isolated test preset; "
                f"selected test preset equals production preset {production!r}. "
                "Set BIOSEQ_TEST_BUILD_PRESET[_DEBUG] to a distinct preset "
                "whose BIOSEQ_TEST_INDEXES cache value is ON.")
        return selected
    if family == "cache":
        override = (CACHE_DEBUG_BUILD_PRESET_OVERRIDE if debug
                    else CACHE_BUILD_PRESET_OVERRIDE)
        return override or _default_cache_preset(
            production, combined=False)
    if family == "cache-bioseq":
        override = (CACHE_BIOSEQ_DEBUG_BUILD_PRESET_OVERRIDE if debug
                    else CACHE_BIOSEQ_BUILD_PRESET_OVERRIDE)
        return override or _default_cache_preset(
            production, combined=True)
    if family == "website":
        return WEBSITE_DEBUG_BUILD_PRESET if debug else WEBSITE_BUILD_PRESET
    raise RuntimeError(
        f"mode {mode!r} has unsupported preset family {family!r}")

def build_dir_for(mode: str, debug: bool) -> Path:
    return REPOSITORY_ROOT / "build" / preset_for(mode, debug)

def build_dirs_for(mode: str, debug: bool) -> tuple[Path, ...]:
    """Return every CMake tree a mode may configure or build.

    Ordinary modes own one tree. PostgreSQL error-safety qualification owns
    two fixed, isolated profile trees and must serialize against either one.
    """
    if mode == POSTGRES_ERROR_SAFETY_MODE:
        return (
            REPOSITORY_ROOT / "build" / POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
            REPOSITORY_ROOT / "build" / POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET,
        )
    return (build_dir_for(mode, debug),)

def bin_dir_for(mode: str, debug: bool) -> Path:
    return build_dir_for(mode, debug) / MODES[mode].binary_subdir

@dataclass
class ModeDefinition:
    """
    Two runs with the same lock group cannot run in parallel. benchmark and
    benchmark-fast share the same binary + reporting/ directory, so they're
    grouped together; unit-integration and external-api share the "tests"
    group. Modes resolving to the same CMake build directory also conflict
    across lock groups. Runtime modes that install/restart the shared PostgreSQL
    instance conflict as well, while unrelated build-only runs remain
    independent.
    """
    lock_group: str
    """
    Per-mode predicted wall clock (minutes). Used for ETAs in messages and
    kept slightly under the hard timeout so a healthy run never reports a
    negative ETA before being killed.
    """
    expected_duration_min: float
    """Same as expected_duration_min but estimates execution time only for compilation"""
    build_only_expected_duration_min: float
    """Human-readable name of this mode"""
    label: str
    """Adjusts verbs so messages read naturally"""
    is_verb_plular: bool
    """Targets we need to build via CMake"""
    targets: List[str]
    """
    Commands we want to run. First element of each command is the binary
    basename — resolved against the active mode's `binary_subdir` directory
    at run time (see `bin_dir_for(mode, debug)`). This lets the same MODE
    definition drive isolated production, test, cache-only, and combined
    preset families, including their debug/inspect variants.
    """
    commands: List[List[str]]
    """Extra documentation"""
    docs: str
    """Submit every ordinary target to one CMake/Ninja invocation.

    This lets independent target/module DAGs share the same global parallel-job
    budget.  It changes compilation scheduling only: installation, PostgreSQL
    restart, and runtime commands still begin after the complete build group.
    """
    build_targets_together: bool = False
    """Optional subdirectory within REPORTING_DIR for a native run report."""
    reporting_subdir_name: Optional[str] = None
    """This applies only to benchmark. Is this benchmark target?"""
    is_bench_mode: bool = False
    """If true, install the freshly-built postgresql_bioseq extension (dylib +
    .control + .sql) into the running PostgreSQL before executing the run
    commands. Integration tests connect to that server and DROP/CREATE EXTENSION
    from the installed files, so without this they would silently exercise a
    stale, previously-installed extension. Requires 'postgresql_bioseq' to be in
    `targets` so the dylib exists at install time."""
    install_extension: bool = False
    """Optional dedicated CMake preset instead of the server-wide preset."""
    preset_override: Optional[str] = None
    """Cache entries that must be set before this mode's targets exist."""
    required_cache_variables: Optional[Dict[str, str]] = None
    """Preset family: core, isolated core tests, cache-only, or combined."""
    preset_family: str = "core"
    """Subdirectory below the active build tree containing command binaries."""
    binary_subdir: str = "bioseq"
    """CMake-owned test/benchmark targets run only after ordinary compilation."""
    post_build_targets: tuple[str, ...] = ()
    """True when all PostgreSQL work occurs in a disposable private cluster."""
    uses_private_cluster: bool = False
    """Optional mode-wide wall-clock ceiling, including build and cleanup."""
    hard_timeout_min: Optional[float] = None
    """True for fixed local compile gates that must never execute runtime work."""
    requires_build_only: bool = False

    def __post_init__(self) -> None:
        if self.preset_family not in {
                "core", "test", "cache", "cache-bioseq", "website"}:
            raise ValueError(
                f"unsupported preset family {self.preset_family!r}")
        if self.uses_private_cluster and self.install_extension:
            raise ValueError(
                "private-cluster modes must not install/restart the shared "
                "PostgreSQL extension")
        if self.hard_timeout_min is not None and self.hard_timeout_min <= 0:
            raise ValueError("mode hard_timeout_min must be positive")
        if self.requires_build_only and (
                self.commands or self.post_build_targets
                or self.install_extension):
            raise ValueError(
                "fixed build-only modes cannot own runtime commands, "
                "post-build targets, or extension installation")

    @property
    def verb_is(self) -> str:
        return "are" if self.is_verb_plular else "is"

    @property
    def verb_was(self) -> str:
        return "were" if self.is_verb_plular else "was"

    @property
    def reporting_dir(self) -> Path:
        if not self.reporting_subdir_name:
            return REPORTING_DIR / "__does_not_exist__"
        return REPORTING_DIR / self.reporting_subdir_name

MODES: Dict[str, ModeDefinition] = {
    "external-api": ModeDefinition(
        lock_group="tests",
        expected_duration_min=120.0,
        build_only_expected_duration_min=60.0,
        label="External API and PostgreSQL integration tests",
        docs=(
            "Build and run generated external API clients, then install the "
            "external-enabled generated extension package in a clean private "
            "PostgreSQL 17 cluster and emit a hash-sealed catalog/behavior "
            "acceptance receipt"),
        is_verb_plular=True,
        targets=[
            "bioseq_external_api_integration_tests",
            "postgresql_bioseq",
        ],
        commands=[
            ["bioseq_external_api_integration_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
        ],
        post_build_targets=("bioseq_external_postgres_acceptance",),
        uses_private_cluster=True,
        preset_override=EXTERNAL_API_BUILD_PRESET,
        required_cache_variables={
            "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "ON",
            "BIOSEQ_BUILD_WEBSITE": "OFF",
            "BIOSEQ_WEBSITE_BROWSER_TESTS": "OFF",
        },
    ),
    "website": ModeDefinition(
        lock_group="website",
        expected_duration_min=15.0,
        build_only_expected_duration_min=8.0,
        label="Static documentation website",
        docs=(
            "Build the isolated website code generator (GCC 16.1 ExternalProject) "
            "against the Clang-22 core/cache tree, then build bioseq_cache before "
            "bioseq and stage both production extensions. A full run drives the "
            "private-cluster runner and bioseq_website target: Doxygen + Clang index, "
            "private-cluster runnable SQL, render, independent html5lib/§20.7 "
            "validation, pinned-browser acceptance, and atomic publish. build_only "
            "stops after the three build targets for fast iteration."),
        is_verb_plular=False,
        # The generator plus both production DSOs, provider before consumer.
        targets=["bioseq_codegen_project", "postgresql_bioseq_cache",
                 "postgresql_bioseq"],
        commands=[],
        # §17.3/§17.5: on a full (non-build_only) run, drive the private-cluster
        # runner test and then the whole render/validate/browser/publish pipeline.
        # bioseq_website self-stages its own private PostgreSQL, so no external
        # cluster is provisioned here; uses_private_cluster is the reporting label.
        post_build_targets=("bioseq_private_postgres_tests", "bioseq_website"),
        uses_private_cluster=True,
        preset_family="website",
        binary_subdir="website",
        # §726/§17.5: the website MCP mode forces the pinned-browser acceptance layer
        # ON. It runs only when the full bioseq_website target is built (the browser
        # step degrades to a clean SKIP when Chromium is unprovisioned), so enabling it
        # here never affects the generator-only build_only iteration path.
        required_cache_variables={
            "BIOSEQ_BUILD_WEBSITE": "ON",
            "BIOSEQ_BUILD_CACHE": "ON",
            "BIOSEQ_WEBSITE_BROWSER_TESTS": "ON",
            "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
        },
    ),
    POSTGRES_ERROR_SAFETY_MODE: ModeDefinition(
        lock_group=POSTGRES_ERROR_SAFETY_MODE,
        expected_duration_min=180.0,
        build_only_expected_duration_min=30.0,
        label="PostgreSQL error-safety qualification",
        docs=(
            "Build isolated production and test-index extension profiles, "
            "seal their runtime plans, and validate both in private "
            "PostgreSQL clusters"),
        is_verb_plular=False,
        targets=[],
        commands=[],
        preset_override=POSTGRES_ERROR_SAFETY_PRODUCTION_PRESET,
        uses_private_cluster=True,
        required_cache_variables={
            "BIOSEQ_BUILD_TESTS": "OFF",
            "BIOSEQ_TEST_INDEXES": "OFF",
            "BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE":
                str(POSTGRES_ERROR_SAFETY_CASE_TEMPLATE),
            "BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE": "isolated",
            POSTGRES_ERROR_SAFETY_PEER_BUILD_DIR_CACHE_KEY: str(
                REPOSITORY_ROOT
                / "build"
                / POSTGRES_ERROR_SAFETY_TEST_INDEXES_PRESET),
            "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
            "BIOSEQ_BUILD_WEBSITE": "OFF",
        },
    ),
    PACKAGE_LINUX_MODE: ModeDefinition(
        lock_group=PACKAGE_LINUX_MODE,
        expected_duration_min=240.0,
        build_only_expected_duration_min=240.0,
        label="Local Debian package qualification",
        docs=(
            "Build and extract the fixed native ARM64 Debian 13/PostgreSQL 17 "
            "DEB from clean committed source and seal its exact six-file payload"),
        is_verb_plular=False,
        targets=[],
        commands=[],
        preset_override=PACKAGE_LINUX_MODE,
        reporting_subdir_name=package_linux.REPORTING_SUBDIR,
        hard_timeout_min=240.0,
    ),
    LINT_FOCUSED_MODE: ModeDefinition(
        lock_group="lint",
        expected_duration_min=30.0,
        build_only_expected_duration_min=30.0,
        label="Focused Clang-Tidy analysis",
        docs=(
            "Build the production extension module graph, then lint only the "
            "explicit repository- or build-relative compile units in filter.paths"
        ),
        is_verb_plular=False,
        targets=["postgresql_bioseq"],
        commands=[],
        requires_build_only=True,
    ),
    LINT_FULL_MODE: ModeDefinition(
        lock_group="lint",
        expected_duration_min=220.0,
        build_only_expected_duration_min=220.0,
        label="Full Clang-Tidy analysis",
        docs=(
            "Build the isolated test preset's complete default target graph, "
            "then lint every authored and generated first-party compile unit"
        ),
        is_verb_plular=False,
        targets=["all"],
        commands=[],
        preset_family="test",
        hard_timeout_min=240.0,
        requires_build_only=True,
    ),
    "unit-integration": ModeDefinition(
        lock_group="tests",
        expected_duration_min=120.0,
        build_only_expected_duration_min=10.0,
        label="Unit/integration tests",
        docs="Run all bioseq unit and integration tests",
        is_verb_plular=True,
        # postgresql_bioseq MUST be built first: the integration tests connect to
        # the running PostgreSQL and CREATE EXTENSION from the *installed* files,
        # so the extension dylib + SQL must be (re)built and installed before the
        # int-test client runs. Without this the int tests exercise whatever
        # stale extension happens to be installed in the server.
        targets=["postgresql_bioseq_cache", "postgresql_bioseq",
                 "bioseq_unit_tests", "bioseq_integration_tests",
                 "bioseq_benchmark_e2e_contract_tests",
                 "bioseq_benchmark_framework_contract_tests"],
        build_targets_together=True,
        install_extension=True,
        commands=[
            ["bioseq_unit_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_integration_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_benchmark_e2e_contract_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_benchmark_framework_contract_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
        ],
        preset_family="test",
        # Optional component modes may be deliberately pointed at this test
        # preset for local qualification.  Restore the ordinary test tree
        # before any unit/integration target is built so their sticky CMake
        # cache entries cannot pull unavailable website/codegen toolchains
        # into an otherwise incremental test run.
        required_cache_variables={
            "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
            "BIOSEQ_BUILD_WEBSITE": "OFF",
        },
    ),
    "highway-scheduler-all-alphabets-uint64": ModeDefinition(
        lock_group="tests",
        expected_duration_min=120.0,
        build_only_expected_duration_min=120.0,
        label="Highway all-alphabets uint64 scheduler compile gate",
        docs=(
            "Build the production extension in the isolated all-alphabets "
            "uint64 tree, proving the 14-unit scheduler materialization profile"
        ),
        is_verb_plular=False,
        targets=["postgresql_bioseq"],
        commands=[],
        preset_override="test-optimized-all-alphabets-uint64",
        preset_family="test",
        required_cache_variables={
            "BIOSEQ_TEMPLATE_FLATTEN_PROFILE": "all_alphabets_uint64",
            "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
            "BIOSEQ_BUILD_WEBSITE": "OFF",
        },
        requires_build_only=True,
    ),
    "highway-scheduler-all-coordinates": ModeDefinition(
        lock_group="tests",
        expected_duration_min=180.0,
        build_only_expected_duration_min=180.0,
        label="Highway all-coordinates scheduler compile gate",
        docs=(
            "Build the production extension in the isolated all-coordinates "
            "tree, proving the 56-unit scheduler materialization profile"
        ),
        is_verb_plular=False,
        targets=["postgresql_bioseq"],
        commands=[],
        preset_override="test-optimized-all-coordinates",
        preset_family="test",
        required_cache_variables={
            "BIOSEQ_TEMPLATE_FLATTEN_PROFILE": "all_coordinates",
            "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
            "BIOSEQ_BUILD_WEBSITE": "OFF",
        },
        requires_build_only=True,
    ),
    "highway-scheduler-full-specialization": ModeDefinition(
        lock_group="tests",
        expected_duration_min=240.0,
        build_only_expected_duration_min=240.0,
        label="Highway full-specialization scheduler compile gate",
        docs=(
            "Build the production extension in the isolated full-specialization "
            "tree, proving the complete scheduler materialization profile"
        ),
        is_verb_plular=False,
        targets=["postgresql_bioseq"],
        commands=[],
        preset_override="test-optimized-full-specialization",
        preset_family="test",
        required_cache_variables={
            "BIOSEQ_TEMPLATE_FLATTEN_PROFILE": "full_specialization",
            "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
            "BIOSEQ_BUILD_WEBSITE": "OFF",
        },
        requires_build_only=True,
    ),
    "highway-gcc-canary": ModeDefinition(
        lock_group="tests",
        expected_duration_min=30.0,
        build_only_expected_duration_min=30.0,
        label="Local Homebrew GCC 16 generated-module canary",
        docs=(
            "Compile and link a generated-style conservative two-target Highway "
            "dispatch table, a third compile-only attributed target probe, and "
            "the generic namespace-materialization closure. The sole "
            "HWY_EXPORT/HWY_DYNAMIC_DISPATCH registry is compiled and linked in "
            "an isolated empty-environment, no-vcpkg-toolchain Ninja sub-build "
            "with an authenticated paired Homebrew gcc/g++ >= 16.1, "
            "libstdc++.modules.json, sealed Highway/libstdc++ headers, and "
            "audited GCC dyld runtimes. With build_only=true it executes neither "
            "binary and proves only the GCC Highway module/attribute/dispatch "
            "boundary. With build_only=false the local builder additionally "
            "executes the imported-binder canary in an exact PATH/locale-only "
            "environment and records both selected target IDs and names. Neither "
            "form proves the full external-api build or production DSO."
        ),
        is_verb_plular=False,
        targets=["bioseq_highway_gcc_canary"],
        commands=[["bioseq_highway_gcc_module_canary"]],
        preset_override="highway-gcc-canary",
        binary_subdir=(
            "codegen/namespace_materialization/tests/integration/module_canary/"
            "highway-gcc-canary/nested/bin"
        ),
        required_cache_variables={
            "BIOSEQ_HIGHWAY_GCC_CANARY": "ON",
            "BIOSEQ_BUILD_CORE": "ON",
            "BIOSEQ_BUILD_CACHE": "OFF",
            "BIOSEQ_BUILD_PAPERS": "OFF",
            "BIOSEQ_BUILD_EXTERNAL_API_CLIENTS": "OFF",
            "BIOSEQ_BUILD_WEBSITE": "OFF",
            "BUILD_TESTING": "OFF",
        },
        requires_build_only=False,
    ),
    "benchmark": ModeDefinition(
        lock_group="benchmark",
        expected_duration_min=25.0,
         build_only_expected_duration_min=1.5,
        label="Benchmark for bioseq",
        docs="Run primary bioseq benchmark",
        is_verb_plular=False,
        targets=["bioseq_benchmark"],
        commands=[["bioseq_benchmark"]],
        reporting_subdir_name="bioseq_benchmark",
        is_bench_mode=True,
    ),
    "benchmark-fast": ModeDefinition(
        lock_group="benchmark",
        expected_duration_min=20.0,
        build_only_expected_duration_min=1.5,
        label="Benchmark for bioseq (--fast)",
        docs="Run primary bioseq benchmark in fast mode for faster iteration",
        is_verb_plular=False,
        targets=["bioseq_benchmark"],
        commands=[["bioseq_benchmark", "--fast"]],
        reporting_subdir_name="bioseq_benchmark",
        is_bench_mode=True,
    ),
    "benchmark-io": ModeDefinition(
        lock_group="benchmark",
        expected_duration_min=8.0,
        build_only_expected_duration_min=1.5,
        label="Benchmark for bioseq IO module",
        docs="Run benchmark for IO module residing in io/ directory. Note this is separate benchmark dedicated only to IO interfaces",
        is_verb_plular=False,
        targets=["bioseq_io_benchmark"],
        commands=[["bioseq_io_benchmark"]],
        reporting_subdir_name="bioseq_io_benchmark",
        is_bench_mode=True,
    ),
    "benchmark-e2e": ModeDefinition(
        lock_group="benchmark",
        expected_duration_min=45.0,
        build_only_expected_duration_min=3.0,
        label="End-to-end SQL benchmark (bioseq.load + mmsearch vs minimap2)",
        docs=("End-to-end SQL-driven benchmark. Drives PostgreSQL "
              "over libpq (tests/integration/support/index.cppm): "
              "bioseq.load + SELECT "
              "mmsearch vs minimap2 over the 2/6 Gbp x dna4/dna5 x {no_index, "
              "just_index} matrix. Like the integration tests it installs the "
              "freshly-built extension first (the bench's bioseq.load/mmsearch "
              "must hit current code, not a stale installed copy)."),
        is_verb_plular=False,
        # postgresql_bioseq MUST be built + installed first: the bench connects to
        # the running PostgreSQL and CREATE EXTENSIONs from the *installed* files.
        targets=["postgresql_bioseq_cache", "postgresql_bioseq",
                 "bioseq_benchmark_e2e"],
        install_extension=True,
        commands=[["bioseq_benchmark_e2e"]],
        reporting_subdir_name="bioseq_benchmark_e2e",
        is_bench_mode=True,
    ),
    "cache-unit": ModeDefinition(
        lock_group="cache-tests",
        expected_duration_min=2.0,
        build_only_expected_duration_min=1.0,
        label="bioseq_cache unit tests",
        docs="Build bioseq_cache and run no-Postgres unit tests",
        is_verb_plular=True,
        preset_family="cache",
        binary_subdir="bioseq_caching",
        targets=[
            "postgresql_bioseq_cache",
            "bioseq_cache_unit_tests",
            "bioseq_postgres_test_support_contract_tests",
            "bioseq_cache_benchmark_report_contract_tests",
            "bioseq_durable_file_io_contract_tests",
            "bioseq_checked_arithmetic_contract_tests",
            "bioseq_build_mcp_configure_freshness_contract_tests",
        ],
        commands=[
            ["bioseq_cache_unit_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_postgres_test_support_contract_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_cache_benchmark_report_contract_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_durable_file_io_contract_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
            ["bioseq_checked_arithmetic_contract_tests",
             "--reporter", "console::out=-::colour-mode=ansi"],
        ],
    ),
    "cache-integration": ModeDefinition(
        lock_group="cache-tests",
        expected_duration_min=8.0,
        build_only_expected_duration_min=1.0,
        label="bioseq_cache temporary-cluster tests",
        docs="Run native cache Catch2 integration tests in a private cluster",
        is_verb_plular=True,
        preset_family="cache",
        binary_subdir="bioseq_caching",
        targets=[
            "postgresql_bioseq_cache",
            "bioseq_cache_production_postgres_stage",
            "postgresql_bioseq_cache_test_support",
            "bioseq_cache_integration_tests",
        ],
        commands=[
            [
                "bioseq_cache_integration_tests",
                "--postgres-bin-dir",
                "{cache_test_support_postgres_bin_dir}",
                "--postgres-stage-root",
                "{cache_test_support_postgres_stage_root}",
                "--postgres-stage-manifest",
                "{cache_test_support_postgres_stage_manifest}",
                "--cache-control", "{cache_test_control}",
                "--cache-install-sql", "{cache_test_install_sql}",
                "--cache-dso", "{cache_test_dso}",
                "--report", "{report}",
                "--case-timeout-seconds", "480",
                "--reporter", "console::out=-::colour-mode=ansi",
            ],
        ],
        reporting_subdir_name="bioseq_cache_integration",
        uses_private_cluster=True,
    ),
    "cache-benchmark": ModeDefinition(
        lock_group="benchmark",
        expected_duration_min=20.0,
        build_only_expected_duration_min=1.0,
        label="bioseq_cache benchmark",
        docs="Run cache core and private-cluster SQL benchmarks",
        is_verb_plular=False,
        preset_family="cache",
        binary_subdir="bioseq_caching",
        targets=["postgresql_bioseq_cache", "bioseq_cache_benchmark"],
        commands=[
            [
                "bioseq_cache_benchmark",
                "--postgres-bin-dir",
                "{cache_production_postgres_bin_dir}",
                "--postgres-stage-root",
                "{cache_production_postgres_stage_root}",
                "--postgres-stage-manifest",
                "{cache_production_postgres_stage_manifest}",
                "--cache-control", "{cache_control}",
                "--cache-install-sql", "{cache_install_sql}",
                "--cache-dso", "{cache_dso}",
                "--scenario", "standalone",
                "--samples", "7",
                "--warmup-samples", "1",
                "--report", "{report}",
            ],
        ],
        reporting_subdir_name="bioseq_cache_benchmark",
        is_bench_mode=True,
        uses_private_cluster=True,
    ),
    "cache-bioseq-integration": ModeDefinition(
        lock_group="cache-tests",
        expected_duration_min=140.0,
        build_only_expected_duration_min=10.0,
        label="bioseq_cache provider integration tests",
        docs=(
            "Run support-profile Bioseq/cache diagnostics, then gate the exact "
            "2 Gbp case against both shipping production extensions"
        ),
        is_verb_plular=True,
        preset_family="cache-bioseq",
        binary_subdir="bioseq_caching",
        targets=[
            "postgresql_bioseq_cache",
            "postgresql_bioseq_cache_test_support",
            "postgresql_bioseq",
            "bioseq_cache_bioseq_production_dso_qualification",
            "bioseq_cache_bioseq_support_integration_tests",
            "bioseq_cache_bioseq_large_input_e2e_tests",
        ],
        commands=[
            [
                "bioseq_cache_bioseq_support_integration_tests",
                "--postgres-bin-dir",
                "{cache_bioseq_support_postgres_bin_dir}",
                "--postgres-stage-root",
                "{cache_bioseq_support_postgres_stage_root}",
                "--postgres-stage-manifest",
                "{cache_bioseq_support_postgres_stage_manifest}",
                "--bioseq-only-postgres-bin-dir",
                "{cache_bioseq_only_postgres_bin_dir}",
                "--bioseq-only-postgres-stage-root",
                "{cache_bioseq_only_postgres_stage_root}",
                "--bioseq-only-postgres-stage-manifest",
                "{cache_bioseq_only_postgres_stage_manifest}",
                "--cache-control", "{cache_test_control}",
                "--cache-install-sql", "{cache_test_install_sql}",
                "--cache-dso", "{cache_test_dso}",
                "--bioseq-control", "{bioseq_control}",
                "--bioseq-install-sql", "{bioseq_install_sql}",
                "--bioseq-dso", "{bioseq_dso}",
                "--provider-peer-test-dso", "{provider_peer_test_dso}",
                "--report", "{report}",
                "--case-timeout-seconds", "480",
                "--reporter", "console::out=-::colour-mode=ansi",
            ],
            [
                "bioseq_cache_bioseq_large_input_e2e_tests",
                "--postgres-bin-dir",
                "{cache_bioseq_production_postgres_bin_dir}",
                "--postgres-stage-root",
                "{cache_bioseq_production_postgres_stage_root}",
                "--postgres-stage-manifest",
                "{cache_bioseq_production_postgres_stage_manifest}",
                "--cache-control", "{cache_control}",
                "--cache-install-sql", "{cache_install_sql}",
                "--cache-dso", "{cache_dso}",
                "--bioseq-control", "{bioseq_control}",
                "--bioseq-install-sql", "{bioseq_install_sql}",
                "--bioseq-dso", "{bioseq_dso}",
                "--report", "{report}",
                "--case-timeout-seconds", "6300",
                "--reporter", "console::out=-::colour-mode=ansi",
            ],
        ],
        reporting_subdir_name="bioseq_cache_bioseq_integration",
        uses_private_cluster=True,
        hard_timeout_min=140.0,
    ),
    "cache-bioseq-benchmark": ModeDefinition(
        lock_group="benchmark",
        expected_duration_min=180.0,
        build_only_expected_duration_min=10.0,
        label="bioseq_cache provider benchmark",
        docs="Run Bioseq cache benchmarks with both DSOs in a private cluster",
        is_verb_plular=False,
        preset_family="cache-bioseq",
        binary_subdir="bioseq_caching",
        targets=[
            "postgresql_bioseq_cache",
            "postgresql_bioseq",
            "bioseq_cache_bioseq_production_dso_qualification",
            "bioseq_cache_bioseq_benchmark",
        ],
        commands=[
            [
                "bioseq_cache_bioseq_benchmark",
                "--postgres-bin-dir",
                "{cache_bioseq_production_postgres_bin_dir}",
                "--postgres-stage-root",
                "{cache_bioseq_production_postgres_stage_root}",
                "--postgres-stage-manifest",
                "{cache_bioseq_production_postgres_stage_manifest}",
                "--cache-control", "{cache_control}",
                "--cache-install-sql", "{cache_install_sql}",
                "--cache-dso", "{cache_dso}",
                "--bioseq-control", "{bioseq_control}",
                "--bioseq-install-sql", "{bioseq_install_sql}",
                "--bioseq-dso", "{bioseq_dso}",
                "--scenario", "bioseq-2gbp",
                "--samples", "3",
                "--warmup-samples", "1",
                "--report", "{report}",
            ],
        ],
        reporting_subdir_name="bioseq_cache_bioseq_benchmark",
        is_bench_mode=True,
        uses_private_cluster=True,
    ),
}

VALID_MODES: Set[str] = set(MODES.keys())

LINT_MODES: frozenset[str] = frozenset({LINT_FOCUSED_MODE, LINT_FULL_MODE})

# Catch2-focused correctness modes share one closed filter contract.  Every
# executable in a selected mode receives the same OR-combined test spec and
# permits zero local matches so a tag owned by a sibling executable remains a
# valid focused iteration request.
CATCH_FILTER_MODES: frozenset[str] = frozenset({
    "unit-integration",
    "cache-unit",
    "cache-integration",
    "cache-bioseq-integration",
})

FIXED_BUILD_ONLY_MODES: frozenset[str] = frozenset(
    mode for mode, details in MODES.items()
    if details.requires_build_only)

REMOTE_RUN_REJECTED_MODES: Set[str] = {
    "website",
    "highway-gcc-canary",
    POSTGRES_ERROR_SAFETY_MODE,
    PACKAGE_LINUX_MODE,
    *FIXED_BUILD_ONLY_MODES,
}

REMOTE_RUN_BUILD_MODES: tuple[str, ...] = tuple(sorted(
    VALID_MODES - REMOTE_RUN_REJECTED_MODES))

BENCH_CLI_MODES: tuple[str, ...] = tuple(sorted(
    mode for mode, details in MODES.items()
    if (details.is_bench_mode and details.commands
        and not mode.startswith("cache-"))
))

def get_modes_short_documentation() -> str:
    entries: list[str] = []
    for mode in sorted(VALID_MODES):
        details = MODES[mode]
        if details.requires_build_only:
            entries.append(
                f"'{mode}' (~{int(details.build_only_expected_duration_min)} "
                "min, local build-only only)")
            continue
        entries.append(
            f"'{mode}' (~{int(details.expected_duration_min)} min and "
            f"~{int(details.build_only_expected_duration_min)} min for "
            "build-only)")
    modes_list_str = ", ".join(entries)
    return f"Modes: {modes_list_str}"

def post_build_targets_for(mode: str, build_only: bool) -> tuple[str, ...]:
    """Return CMake-owned run targets, suppressing all execution for build-only."""
    if build_only:
        return ()
    return MODES[mode].post_build_targets

def supports_bench_cli_options(mode: str) -> bool:
    """Whether filter/debug_report can be forwarded to a mode's CLI."""
    return mode in BENCH_CLI_MODES

def _is_aws_clang_preset(preset: str) -> bool:
    normalized = preset.removeprefix("test-")
    return (
        normalized.startswith("aws-x64-linux")
        and "gcc161" not in normalized
    )

def _is_aws_gcc_preset(preset: str) -> bool:
    normalized = preset.removeprefix("test-")
    return normalized.startswith("aws-x64-linux-gcc161")

def _is_aws_preset(preset: str) -> bool:
    return _is_aws_clang_preset(preset) or _is_aws_gcc_preset(preset)

def _is_debug_preset(preset: str) -> bool:
    return preset.removeprefix("test-").endswith("-debug")



_FILTER_AXES = (("tags", "tags"), ("benchmarks", "benchmark"), ("cases", "cases"))

_MAX_CATCH2_TEST_SPECS = 32

_MAX_CATCH2_TEST_SPEC_LENGTH = 512

_MAX_CATCH2_COMBINED_TEST_SPEC_LENGTH = 4096

_MAX_LINT_PATHS = 64

_MAX_LINT_PATH_LENGTH = 512


def lint_filter_to_paths(lint_filter: object) -> tuple[list[str], str | None]:
    """Validate the focused lint filter and return normalized relative paths.

    @param lint_filter Closed object containing only a nonempty ``paths`` array.
    @return Normalized paths and no error, or an empty list and a diagnostic.
    """

    if not isinstance(lint_filter, dict):
        return [], "filter must be an object containing paths"
    unknown = sorted(set(lint_filter) - {"paths"})
    if unknown:
        return [], "focused lint filter rejects keys: " + ", ".join(unknown)
    raw_paths = lint_filter.get("paths")
    if not isinstance(raw_paths, list) or not raw_paths:
        return [], "filter.paths must be a nonempty array"
    if len(raw_paths) > _MAX_LINT_PATHS:
        return [], f"filter.paths accepts at most {_MAX_LINT_PATHS} entries"

    normalized: list[str] = []
    seen: set[str] = set()
    for raw_path in raw_paths:
        if not isinstance(raw_path, str) or not raw_path:
            return [], "every filter.paths entry must be a nonempty string"
        if len(raw_path) > _MAX_LINT_PATH_LENGTH:
            return [], (
                "filter.paths entries may contain at most "
                f"{_MAX_LINT_PATH_LENGTH} characters"
            )
        if (
            raw_path.startswith("/")
            or "\\" in raw_path
            or any(ord(character) < 32 for character in raw_path)
        ):
            return [], f"filter.paths entry is not a safe relative path: {raw_path!r}"
        components = raw_path.split("/")
        if any(component in {"", ".", ".."} for component in components):
            return [], f"filter.paths entry is not normalized: {raw_path!r}"
        if not raw_path.endswith((".c", ".cc", ".cpp", ".cppm", ".cxx")):
            return [], f"filter.paths entry is not a C/C++ compile unit: {raw_path!r}"
        if raw_path not in seen:
            normalized.append(raw_path)
            seen.add(raw_path)
    return normalized, None

def _filter_to_cli_args(bench_filter):
    """Translate the structured `filter` object into bench CLI tokens.

    Returns (extra_argv, error). extra_argv is a flat list like
    ['--filter','tags=a,b','--filter','benchmark=k']. error is a human string or
    None. Validates value shape (non-empty arrays / strings, no ',' or '=').
    """
    if not bench_filter:
        return [], None
    if not isinstance(bench_filter, dict):
        return [], "filter must be an object"
    unknown = sorted(set(bench_filter) - {axis for axis, _ in _FILTER_AXES})
    if unknown:
        return [], "filter contains unknown field(s): " + ", ".join(unknown)
    extra: list = []
    for obj_key, cli_key in _FILTER_AXES:
        if obj_key not in bench_filter:
            continue
        values = bench_filter[obj_key]
        if not isinstance(values, list) or not values:
            return [], f"filter.{obj_key} must be a non-empty array"
        for v in values:
            if not isinstance(v, str) or not v:
                return [], f"filter.{obj_key} entries must be non-empty strings"
            if "," in v or "=" in v:
                return [], f"filter.{obj_key} value '{v}' may not contain ',' or '='"
        extra += ["--filter", f"{cli_key}=" + ",".join(values)]
    return extra, None

def _unit_test_filter_to_cli_args(test_filter):
    """Translate ``filter.tags`` into one safe Catch2 OR test-spec.

    Catch2 treats commas inside one positional test-spec as OR.  We therefore
    append the same deterministic argument to every executable in the selected
    Catch correctness mode. ``--allow-running-no-tests`` is also appended
    because a tag commonly exists in only one binary; absence from the others
    is not a failure of the focused workflow.

    Returns ``(extra_argv, error)``.  An omitted/empty filter deliberately
    returns no argv so the historical full-suite command lines are unchanged.
    """
    if test_filter is None or test_filter == {}:
        return [], None
    if not isinstance(test_filter, dict):
        return [], "filter must be an object"

    unknown = sorted(set(test_filter) - {"tags", "benchmarks", "cases"})
    if unknown:
        return [], "filter contains unknown field(s): " + ", ".join(unknown)
    disallowed = [axis for axis in ("benchmarks", "cases") if axis in test_filter]
    if disallowed:
        return [], (
            "Catch correctness modes accept only filter.tags Catch2 test-spec "
            "expressions; unsupported field(s): " + ", ".join(disallowed)
        )
    if "tags" not in test_filter:
        return [], None

    values = test_filter["tags"]
    if not isinstance(values, list) or not values:
        return [], "filter.tags must be a non-empty array"
    if len(values) > _MAX_CATCH2_TEST_SPECS:
        return [], (
            f"filter.tags accepts at most {_MAX_CATCH2_TEST_SPECS} Catch2 "
            "test-spec expressions"
        )

    # Stable first-occurrence de-duplication keeps the effective argv compact
    # without reordering user-written Catch2 expressions.
    expressions: list[str] = []
    seen: set[str] = set()
    for index, value in enumerate(values):
        if not isinstance(value, str) or not value:
            return [], "filter.tags entries must be non-empty strings"
        if value != value.strip():
            return [], (
                f"filter.tags[{index}] may not have leading or trailing whitespace"
            )
        if len(value) > _MAX_CATCH2_TEST_SPEC_LENGTH:
            return [], (
                f"filter.tags[{index}] exceeds the "
                f"{_MAX_CATCH2_TEST_SPEC_LENGTH}-character limit"
            )
        if value.startswith("-"):
            return [], (
                f"filter.tags[{index}] may not begin with '-' (Catch2 option "
                "injection is not allowed)"
            )
        if any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in value):
            return [], f"filter.tags[{index}] may not contain control characters"
        if value not in seen:
            seen.add(value)
            expressions.append(value)

    combined = ",".join(expressions)
    if len(combined) > _MAX_CATCH2_COMBINED_TEST_SPEC_LENGTH:
        return [], (
            "combined Catch2 test-spec exceeds the "
            f"{_MAX_CATCH2_COMBINED_TEST_SPEC_LENGTH}-character limit"
        )
    return ["--allow-running-no-tests", combined], None



CMAKE_PRESETS_FILE = REPOSITORY_ROOT / "CMakePresets.json"

_CMAKE_CONFIGURE_STAMP_NAME = ".bioseq-build-mcp-configure.json"

_CMAKE_CONFIGURE_STAMP_KIND = "bioseq-build-mcp-configure"

def _cmake_cache_value(build_dir: Path, key: str) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    prefix = f"{key}:"
    try:
        with cache.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith(prefix):
                    return line.split("=", 1)[1].strip()
    except OSError:
        return None
    return None

def _cmake_cache_bool(build_dir: Path, key: str) -> bool | None:
    value = _cmake_cache_value(build_dir, key)
    if value is None:
        return None
    normalized = value.strip().upper()
    if normalized in {"1", "ON", "TRUE", "YES", "Y"}:
        return True
    if normalized in {"0", "OFF", "FALSE", "NO", "N", "IGNORE", "NOTFOUND"}:
        return False
    return None

def _cmake_cache_contains(build_dir: Path, needle: str) -> bool:
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return False
    try:
        return needle in cache.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False

def _expand_cmake_preset_include(
        value: str,
        *,
        source_dir: Path,
        file_dir: Path,
        environment: Mapping[str, str]) -> Path:
    """Resolve the bounded macro set accepted in project preset includes."""
    expanded = value
    replacements = {
        "${sourceDir}": str(source_dir),
        "${fileDir}": str(file_dir),
        "${dollar}": "$",
        "${pathListSep}": os.pathsep,
    }
    for macro, replacement in replacements.items():
        expanded = expanded.replace(macro, replacement)

    def replace_environment(match: re.Match[str]) -> str:
        return environment.get(match.group(1), "")

    expanded = re.sub(
        r"\$(?:p?env)\{([A-Za-z_][A-Za-z0-9_]*)\}",
        replace_environment,
        expanded,
    )
    if re.search(r"\$\{[^}]+\}|\$(?:p?env)\{[^}]+\}", expanded):
        raise RuntimeError(
            "unsupported dynamic macro in CMake preset include "
            f"{value!r}")
    path = Path(expanded)
    return path if path.is_absolute() else file_dir / path

def _cmake_preset_payload_graph(
        presets_file: Path,
        *,
        environment: Mapping[str, str] | None = None,
) -> tuple[tuple[Path, Path, dict[str, object], bytes], ...]:
    """Load the project/user preset files and their transitive includes."""
    effective_environment = (
        os.environ if environment is None else environment)
    source_dir = presets_file.parent.resolve(strict=False)
    records: list[
        tuple[Path, Path, dict[str, object], bytes]
    ] = []
    visited: set[Path] = set()
    lexical_sources: dict[Path, Path] = {}
    active: list[Path] = []

    def visit(path: Path) -> None:
        lexical = Path(os.path.abspath(path))
        try:
            lexical_metadata = lexical.lstat()
            if (
                stat.S_ISLNK(lexical_metadata.st_mode)
                or not stat.S_ISREG(lexical_metadata.st_mode)
            ):
                raise RuntimeError(
                    "CMake preset source must be a regular non-symlink "
                    f"file: {lexical}")
            canonical = path.resolve(strict=True)
        except OSError as exc:
            raise RuntimeError(
                f"cannot resolve CMake preset include {path}: {exc}") from exc
        previous_lexical = lexical_sources.get(canonical)
        if previous_lexical is not None and previous_lexical != lexical:
            raise RuntimeError(
                "CMake preset graph aliases one file through multiple "
                f"lexical paths: {previous_lexical} and {lexical}")
        if canonical in active:
            cycle = " -> ".join(str(item) for item in (*active, canonical))
            raise RuntimeError(f"CMake preset include cycle: {cycle}")
        if canonical in visited:
            return
        lexical_sources[canonical] = lexical
        try:
            metadata = canonical.stat()
            if not stat.S_ISREG(metadata.st_mode):
                raise RuntimeError(
                    "CMake preset source must resolve to a regular file: "
                    f"{canonical}")
            body = canonical.read_bytes()
            payload = json.loads(body)
        except (OSError, json.JSONDecodeError) as exc:
            raise RuntimeError(
                f"cannot read CMake configure presets from {canonical}: "
                f"{exc}") from exc
        if not isinstance(payload, dict):
            raise RuntimeError(
                f"CMake preset source must be a regular JSON object: "
                f"{canonical}")

        active.append(canonical)
        records.append((lexical, canonical, payload, body))
        raw_includes = payload.get("include", [])
        if isinstance(raw_includes, str):
            includes = [raw_includes]
        elif (
            isinstance(raw_includes, list)
            and all(isinstance(item, str) and item for item in raw_includes)
        ):
            includes = raw_includes
        else:
            raise RuntimeError(
                f"{canonical} contains an invalid CMake preset include list")
        for include in includes:
            visit(_expand_cmake_preset_include(
                include,
                source_dir=source_dir,
                file_dir=lexical.parent,
                environment=effective_environment,
            ))
        active.pop()
        visited.add(canonical)

    visit(presets_file)
    user_presets = presets_file.with_name("CMakeUserPresets.json")
    if presets_file.name == "CMakePresets.json" and user_presets.exists():
        visit(user_presets)
    return tuple(records)

def _resolved_cmake_preset_cache_variables(
        preset: str,
        *,
        presets_file: Path = CMAKE_PRESETS_FILE,
        environment: Mapping[str, str] | None = None) -> dict[str, object]:
    """Resolve configure-preset cache variables with CMake inheritance rules.

    Earlier entries in an ``inherits`` array have priority, the selected
    preset overrides every parent, and an explicit ``null`` remains a
    tombstone so a later parent cannot restore the value.
    """
    presets: dict[str, dict] = {}
    for _, source, payload, _ in _cmake_preset_payload_graph(
            presets_file,
            environment=environment):
        raw_presets = payload.get("configurePresets", [])
        if not isinstance(raw_presets, list):
            raise RuntimeError(
                f"{source} does not contain a configurePresets array")
        for raw_preset in raw_presets:
            if not isinstance(raw_preset, dict):
                raise RuntimeError(
                    f"{source} contains a non-object configure preset")
            name = raw_preset.get("name")
            if not isinstance(name, str) or not name:
                raise RuntimeError(
                    f"{source} contains a configure preset without a name")
            if name in presets:
                raise RuntimeError(
                    f"CMake preset graph contains duplicate configure "
                    f"preset {name!r}")
            presets[name] = raw_preset

    resolved: dict[str, dict[str, object]] = {}

    def resolve(name: str, stack: tuple[str, ...]) -> dict[str, object]:
        cached = resolved.get(name)
        if cached is not None:
            return cached
        if name in stack:
            cycle = " -> ".join((*stack, name))
            raise RuntimeError(
                f"CMake configure preset inheritance cycle: {cycle}")
        raw_preset = presets.get(name)
        if raw_preset is None:
            raise RuntimeError(
                f"CMake configure preset {name!r} is not declared in "
                f"{presets_file}")

        raw_parents = raw_preset.get("inherits", [])
        if isinstance(raw_parents, str):
            parents = [raw_parents]
        elif (
            isinstance(raw_parents, list)
            and all(isinstance(parent, str) and parent
                    for parent in raw_parents)
        ):
            parents = raw_parents
        else:
            raise RuntimeError(
                f"CMake configure preset {name!r} has invalid inherits")

        variables: dict[str, object] = {}
        # Later parents are merged first so the documented earlier-parent
        # priority is preserved by the subsequent updates.
        for parent in reversed(parents):
            variables.update(resolve(parent, (*stack, name)))

        own_variables = raw_preset.get("cacheVariables", {})
        if not isinstance(own_variables, dict):
            raise RuntimeError(
                f"CMake configure preset {name!r} has invalid "
                "cacheVariables")
        for key, value in own_variables.items():
            if not isinstance(key, str) or not key:
                raise RuntimeError(
                    f"CMake configure preset {name!r} has an invalid "
                    "cache-variable name")
            variables[key] = value

        resolved[name] = variables
        return variables

    return dict(resolve(preset, ()))

def _cmake_preset_cache_contract(
        preset: str,
        keys: tuple[str, ...],
        *,
        presets_file: Path = CMAKE_PRESETS_FILE,
        environment: Mapping[str, str] | None = None) -> dict[str, str]:
    """Return exact, non-null values selected by a configure preset."""
    variables = _resolved_cmake_preset_cache_variables(
        preset,
        presets_file=presets_file,
        environment=environment,
    )
    contract: dict[str, str] = {}
    for key in keys:
        raw_value = variables.get(key)
        if raw_value is None:
            continue
        if isinstance(raw_value, bool):
            value = "TRUE" if raw_value else "FALSE"
        elif isinstance(raw_value, str):
            value = raw_value
        elif isinstance(raw_value, dict):
            value = raw_value.get("value")
            if isinstance(value, bool):
                value = "TRUE" if value else "FALSE"
            elif not isinstance(value, str):
                raise RuntimeError(
                    f"CMake configure preset {preset!r} has invalid value "
                    f"for cache variable {key}")
        else:
            raise RuntimeError(
                f"CMake configure preset {preset!r} has invalid value for "
                f"cache variable {key}")
        contract[key] = value
    return contract

def _cmake_preset_cache_repairs(
        build_dir: Path,
        preset: str,
        *,
        keys: tuple[str, ...] = ("PostgreSQL_CONFIG",),
        presets_file: Path = CMAKE_PRESETS_FILE,
        environment: Mapping[str, str] | None = None) -> dict[str, str]:
    """Plan exact repairs for preset-owned cache values in one build tree."""
    contract = _cmake_preset_cache_contract(
        preset,
        keys,
        presets_file=presets_file,
        environment=environment,
    )
    return {
        key: expected
        for key, expected in contract.items()
        if _cmake_cache_value(build_dir, key) != expected
    }
