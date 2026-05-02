#!/bin/bash
# Build script for the bioseq Postgres extension and its test binaries.
#
# **AI agents should NOT invoke this script directly.** The supported entry
# point for AI-driven workflows is the `bioseq-build` MCP server (see
# `main/build_mcp/server.py` and the workflow described in CLAUDE.md). This
# shell script is kept around for human operators who want a one-shot from a
# terminal — it shells out to the same cmake commands the MCP server uses.
#
# Usage:
#   ./build.sh [tests]
#
#   tests   "+"-separated list of test components to run after the build.
#           Recognised tokens: unit, integration, bench. Default: "unit+integration".
#
# The build always uses the `bench-opt` cmake preset (-O3 + NDEBUG). We used to
# accept a second positional `mode` argument, but flipping presets fragmented
# the per-mode build trees and caused unnecessary rebuilds — every workflow
# (unit, integration, bench) is now compiled with the same release-grade flags.
#
# Each requested test component compiles only its own target — `unit` builds
# `bioseq_aligner_tests_exec`, `integration` builds `bioseq_aligner_int_tests_exec`,
# `bench` builds `bioseq_bench`. We never run the cmake "all" target or
# `--target install` here, because none of the test/bench binaries depend on
# the Postgres extension dylib being installed.
#
# Examples:
#   ./build.sh                              # build + run unit + integration suites
#   ./build.sh integration+bench+unit       # build + run all three
#   ./build.sh bench --fast                 # build bench, run with --fast
#   ./build.sh ""                           # build nothing, run nothing (no-op)
#
# Optional env overrides:
#   JOBS=N    forwarded as `--parallel N` to cmake/ninja. Defaults to 32, which
#             oversubscribes the 10-core box on purpose — C++ module compiles
#             stall often enough that the extra slack helps. Lower it if you
#             want headroom (e.g. while running a benchmark on the same box).
set -euo pipefail

TESTS="${1:-unit+integration}"
PRESET="bench-opt"

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_ROOT="${PROJECT_ROOT}/build"
BUILD_DIR="${BUILD_ROOT}/${PRESET}"
mkdir -p "${BUILD_DIR}"

export CMAKE_EXPORT_COMPILE_COMMANDS=1
cd "${PROJECT_ROOT}"
cmake --preset "${PRESET}"

JOBS="${JOBS:-32}"

# Tokenise the requested test list once so each component runs at most once.
IFS='+' read -ra TEST_TOKENS <<< "${TESTS}"
has_token() {
    local needle="$1"
    local token
    for token in "${TEST_TOKENS[@]}"; do
        if [[ "${token}" == "${needle}" ]]; then
            return 0
        fi
    done
    return 1
}

# Map the requested components to the cmake targets we actually need to
# compile. Building only the required targets is what makes incremental
# `bench` cycles fast — the previous "build everything then install" path
# also recompiled the Postgres dylib and the unit-test binary on every run.
TARGETS=()
if has_token unit;        then TARGETS+=(bioseq_aligner_tests_exec); fi
if has_token integration; then TARGETS+=(bioseq_aligner_int_tests_exec); fi
if has_token bench;       then TARGETS+=(bioseq_bench); fi
if has_token io;       then TARGETS+=(bioseq_io_bench); fi

for tgt in "${TARGETS[@]}"; do
    cmake --build "${BUILD_DIR}" --target "${tgt}" --parallel "${JOBS}"
done

# Point build/compile_commands.json at the active preset so clangd / IDE
# integrations always see the same flags the build used.
if [ -f "${BUILD_DIR}/compile_commands.json" ]; then
    ln -sfn "${PRESET}/compile_commands.json" "${BUILD_ROOT}/compile_commands.json"
fi

# Tests run from the per-mode build dir — keeps `./bioseq/...` working.
# The bench writes its report into `main/reporting/` regardless of cwd
# (the path is baked in via BIOSEQ_BENCH_REPORT_DIR at compile time);
# only the unit / integration runners care about cwd.
cd "${BUILD_DIR}"

if has_token unit; then
    ./bioseq/bioseq_aligner_tests_exec --reporter console::out=-::colour-mode=ansi
fi

if has_token integration; then
    ./bioseq/bioseq_aligner_int_tests_exec --reporter console::out=-::colour-mode=ansi
fi

if has_token bench; then
    # Forward any extra args after the test list (so `./build.sh bench --fast`
    # reaches the bench). The `${...:+...}` form keeps `set -u` happy when no
    # extras were passed.
    if [ "$#" -gt 1 ]; then
        ./bioseq/bioseq_bench --reporter console::out=-::colour-mode=ansi "${@:2}"
    else
        ./bioseq/bioseq_bench --reporter console::out=-::colour-mode=ansi
    fi
fi

if has_token io; then
    # Forward any extra args after the test list
    if [ "$#" -gt 1 ]; then
        ./bioseq/bioseq_io_bench --reporter console::out=-::colour-mode=ansi "${@:2}"
    else
        ./bioseq/bioseq_io_bench --reporter console::out=-::colour-mode=ansi
    fi
fi

if has_token lol; then
    ./bioseq/bioseq_aligner_tests_exec --reporter console::out=-::colour-mode=ansi "[lol]"
fi
