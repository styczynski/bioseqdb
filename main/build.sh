#!/bin/bash
# Build script for the bioseq Postgres extension and its test binaries.
#
# Usage:
#   ./build.sh [tests] [mode]
#
#   tests   "+"-separated list of test components to run after the build.
#           Recognised tokens: unit, integration, bench. Default: "unit+integration".
#   mode    cmake configure preset (see CMakePresets.json). Default: "dev-opt-pg".
#
# Each preset gets its own build tree under build/<mode>/, so flipping between
# e.g. dev-opt-pg ↔ bench-opt no longer triggers a full rebuild — incremental
# state is preserved per preset. The active mode's compile_commands.json is
# symlinked at build/compile_commands.json so clangd / IDE integrations keep
# working without any reconfiguration.
#
# Examples:
#   ./build.sh                              # build + run unit + integration suites
#   ./build.sh integration+bench+unit       # all three suites in order
#   ./build.sh bench+unit dev-opt           # different preset, no integration tests
#   ./build.sh "" dev                       # build only, no tests
#
# Optional env overrides:
#   JOBS=N    forwarded as `--parallel N` to cmake/ninja. Defaults to 32, which
#             oversubscribes the 10-core box on purpose — C++ module compiles
#             stall often enough that the extra slack helps. Lower it if you
#             want headroom (e.g. while running a benchmark on the same box).
set -euo pipefail

TESTS="${1:-unit+integration}"
# Default to bench-opt (-O3 + NDEBUG) when the user runs anything that
# matches a `bench` token, so the long-sequence benchmark always uses the
# release-grade optimisation level by default. Other workflows keep the
# safer dev-opt-pg (-O2) preset.
DEFAULT_MODE="dev-opt-pg"
case "${TESTS}" in
    *bench*) DEFAULT_MODE="bench-opt" ;;
esac
MODE="${2:-${DEFAULT_MODE}}"

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_ROOT="${PROJECT_ROOT}/build"
BUILD_DIR="${BUILD_ROOT}/${MODE}"
mkdir -p "${BUILD_DIR}"

# Wipe any previously-installed extension artefacts from the active Postgres tree
# so that `cmake --build . --target install` writes a fresh copy. We resolve the
# directories from `pg_config` so this works for whichever version the
# CMakeLists.txt picked up (Postgres.app 18 today, but version-agnostic).
PG_CONFIG="${PG_CONFIG:-}"
if [ -z "${PG_CONFIG}" ]; then
    for candidate in \
        /Applications/Postgres.app/Contents/Versions/latest/bin/pg_config \
        /opt/homebrew/opt/postgresql@18/bin/pg_config \
        /usr/local/opt/postgresql@18/bin/pg_config; do
        if [ -x "${candidate}" ]; then
            PG_CONFIG="${candidate}"
            break
        fi
    done
fi
if [ -z "${PG_CONFIG}" ] && command -v pg_config >/dev/null 2>&1; then
    PG_CONFIG="$(command -v pg_config)"
fi
if [ -n "${PG_CONFIG}" ]; then
    PG_PKGLIBDIR="$("${PG_CONFIG}" --pkglibdir)"
    PG_SHAREDIR="$("${PG_CONFIG}" --sharedir)"
    rm -f "${PG_PKGLIBDIR}/bioseq.dylib"
    rm -f "${PG_SHAREDIR}/extension/bioseq.control"
    rm -f "${PG_SHAREDIR}/extension/bioseq--1.0.sql"
fi

export CMAKE_EXPORT_COMPILE_COMMANDS=1
cd "${PROJECT_ROOT}"
cmake --preset "${MODE}"

JOBS="${JOBS:-32}"
cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
cmake --build "${BUILD_DIR}" --target install --parallel "${JOBS}"

# Point build/compile_commands.json at the most-recently-built mode so clangd
# (which probes build/ by default) always sees the active preset's flags.
if [ -f "${BUILD_DIR}/compile_commands.json" ]; then
    ln -sfn "${MODE}/compile_commands.json" "${BUILD_ROOT}/compile_commands.json"
fi

# Tests run from the per-mode build dir — keeps `./bioseq/...` working.
# The bench writes its report into `main/reporting/` regardless of cwd
# (the path is baked in via BIOSEQ_BENCH_REPORT_DIR at compile time);
# only the unit / integration runners care about cwd.
cd "${BUILD_DIR}"

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

if has_token unit; then
    ./bioseq/bioseq_aligner_tests_exec --reporter console::out=-::colour-mode=ansi
fi

if has_token integration; then
    ./bioseq/bioseq_aligner_int_tests_exec --reporter console::out=-::colour-mode=ansi
fi

if has_token bench; then
    # Single bench command — runs the full algorithm grid across every
    # length bucket (SHORT/MEDIUM/LONG/VERY_LONG/XL/XXL) and every
    # error bucket in parallel. The `[bench-full-matrix]` TEST_CASE
    # drives the runner; on completion bench_main.cpp computes regression
    # violations, writes a numbered `report_build_<N>.json` into
    # `main/reporting/` (commit hash + violations array embedded),
    # prints a console table, and returns a non-zero exit code if any
    # rule was violated.
    #
    # The bench accepts two project-specific flags (parsed before
    # everything else is forwarded to Catch2):
    #   --everything   include the (k, w) Cartesian sweep and noisy×long
    #                  error/length combos that are otherwise skipped.
    #   --fast         3-4× fewer samples + full hardware concurrency.
    #
    # All other arguments to this script after the test list and the
    # build mode are forwarded to the bench binary, so `./build.sh bench
    # bench-opt --fast` is a one-liner for "build and run a fast bench".
    #
    # All previous BIOSEQ_BENCH_* env vars are gone — every knob lives
    # on the CLI or in `regression_thresholds.json`. The git commit is
    # resolved by the bench binary itself via `git -C $repo rev-parse
    # HEAD`, so we no longer pre-export it here.
    # Forward any extra args after the test list and build mode (so
    # `./build.sh bench bench-opt --fast` reaches the bench). The
    # `${...:+...}` form keeps `set -u` happy when no extras were passed.
    if [ "$#" -gt 2 ]; then
        ./bioseq/bioseq_bench --reporter console::out=-::colour-mode=ansi "${@:3}"
    else
        ./bioseq/bioseq_bench --reporter console::out=-::colour-mode=ansi
    fi
fi

if has_token lol; then
    ./bioseq/bioseq_aligner_tests_exec --reporter console::out=-::colour-mode=ansi "[lol]"
fi
