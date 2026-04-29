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
    ./bioseq/bioseq_bench --reporter console::out=-::colour-mode=ansi
fi

if has_token bench-long; then
    # Quick iteration sweep — runs only LONG / VERY_LONG buckets for the three
    # optimisation-target configs (minimap2 DP, bioseq scalar, bioseq SIMD).
    # Defined in bench_test.cpp under tag [.quick-long-sequence-benchmark].
    BIOSEQ_BENCH_REPORT_PATH="${BIOSEQ_BENCH_REPORT_PATH:-report-long.json}" \
        ./bioseq/bioseq_bench --reporter console::out=-::colour-mode=ansi \
        "[.quick-long-sequence-benchmark]"
fi

if has_token bench-two-stack; then
    # Hyper-quick V4 iteration sweep — runs only VERY_LONG bucket for
    # scalar / SIMD / two-stack-SIMD minimizer configs. Used to drive
    # the SIMD-Minimizers paper port iteration loop.
    BIOSEQ_BENCH_REPORT_PATH="${BIOSEQ_BENCH_REPORT_PATH:-report-two-stack.json}" \
        ./bioseq/bioseq_bench --reporter console::out=-::colour-mode=ansi \
        "[.quick-long-sequence-benchmark-two-stack]"
fi

if has_token lol; then
    ./bioseq/bioseq_aligner_tests_exec --reporter console::out=-::colour-mode=ansi "[lol]"
fi
