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

# Tests run from the per-mode build dir — keeps `./bioseq/...` working and
# ensures BIOSEQ_BENCH_REPORT_PATH defaults land alongside that mode's binary.
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
    # error bucket in parallel (cpu/2 threads by default — see
    # `bench_thread_count` in matrix_runner.cppm). The
    # `[bench-full-matrix]` TEST_CASE drives the runner; on completion
    # bench_main.cpp loads `regression_thresholds.json`, computes
    # regression violations, writes them into the next available
    # `report_build_<N>.json` (commit hash + violations array
    # embedded), prints a console table, and returns a non-zero exit
    # code if any rule was violated.
    #
    # We deliberately do NOT pin BIOSEQ_BENCH_REPORT_PATH: each run
    # produces a fresh report_build_<N>.json so before/after diffs
    # stay possible after agents redirect stdout. Override only when
    # you have a specific external pipeline that needs a fixed path.
    #
    # BIOSEQ_GIT_COMMIT is read from `git rev-parse HEAD` and stamped
    # into the report so reviewers can match a JSON file back to the
    # source state that produced it. Falls back to "unknown" outside
    # a git checkout.
    GIT_COMMIT_HASH="$(git -C "${PROJECT_ROOT}" rev-parse HEAD 2>/dev/null || echo unknown)"
    BIOSEQ_GIT_COMMIT="${BIOSEQ_GIT_COMMIT:-${GIT_COMMIT_HASH}}" \
    BIOSEQ_BENCH_REGRESSION_THRESHOLDS="${BIOSEQ_BENCH_REGRESSION_THRESHOLDS:-${PROJECT_ROOT}/bench/regression_thresholds.json}" \
        ./bioseq/bioseq_bench --reporter console::out=-::colour-mode=ansi
fi

if has_token lol; then
    ./bioseq/bioseq_aligner_tests_exec --reporter console::out=-::colour-mode=ansi "[lol]"
fi
