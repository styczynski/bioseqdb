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
# Tests use the isolated `test-opt` tree (`BIOSEQ_TEST_INDEXES=ON`); benchmarks
# use `bench-opt` (`BIOSEQ_TEST_INDEXES=OFF`). Both keep the same -O3 + NDEBUG
# optimization flags, but test-only descriptors and failure branches can never
# leak into benchmark binaries. Override the preset names with
# BIOSEQ_TEST_BUILD_PRESET / BIOSEQ_BUILD_PRESET for custom local families.
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
PROD_PRESET="${BIOSEQ_BUILD_PRESET:-bench-opt}"
TEST_PRESET="${BIOSEQ_TEST_BUILD_PRESET:-test-opt}"

if [[ "${PROD_PRESET}" == "${TEST_PRESET}" ]]; then
    echo "production and test presets must use distinct build trees" >&2
    exit 2
fi

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_ROOT="${PROJECT_ROOT}/build"
PROD_BUILD_DIR="${BUILD_ROOT}/${PROD_PRESET}"
TEST_BUILD_DIR="${BUILD_ROOT}/${TEST_PRESET}"

export CMAKE_EXPORT_COMPILE_COMMANDS=1
cd "${PROJECT_ROOT}"

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

verify_test_index_flavor() {
    local build_dir="$1"
    local expected="$2"
    local observed
    observed="$(awk -F= '$1 == "BIOSEQ_TEST_INDEXES:BOOL" { print toupper($2) }' \
        "${build_dir}/CMakeCache.txt")"
    if [[ "${observed}" != "${expected}" ]]; then
        echo "BIOSEQ_TEST_INDEXES invariant failed: dir=${build_dir} expected=${expected} observed=${observed:-missing}" >&2
        exit 2
    fi
}

restart_postgresql_after_install() {
    local build_dir="$1"
    local pg_config pg_bindir pg_ctl psql data_dir
    pg_config="$(awk -F= '$1 == "PostgreSQL_CONFIG:FILEPATH" { print $2 }' \
        "${build_dir}/CMakeCache.txt")"
    if [[ -z "${pg_config}" || ! -x "${pg_config}" ]]; then
        echo "PostgreSQL_CONFIG is unavailable after extension install; refusing to run integration tests against a potentially cached dylib" >&2
        exit 2
    fi
    pg_bindir="$("${pg_config}" --bindir)"
    pg_ctl="${pg_bindir}/pg_ctl"
    psql="${pg_bindir}/psql"
    if [[ ! -x "${pg_ctl}" || ! -x "${psql}" ]]; then
        echo "PostgreSQL restart tools are unavailable under ${pg_bindir}; refusing to run integration tests" >&2
        exit 2
    fi
    data_dir="$(PGCONNECT_TIMEOUT="${BIOSEQ_TEST_PGCONNECT_TIMEOUT:-10}" \
        PGPASSWORD="${BIOSEQ_TEST_PGPASSWORD:-postgres}" \
        "${psql}" -Atq \
        --host "${BIOSEQ_TEST_PGHOST:-localhost}" \
        --port "${BIOSEQ_TEST_PGPORT:-5432}" \
        --username "${BIOSEQ_TEST_PGUSER:-postgres}" \
        --dbname postgres \
        --command 'SHOW data_directory;')"
    if [[ -z "${data_dir}" || ! -d "${data_dir}" ]]; then
        echo "PostgreSQL data_directory is unavailable (${data_dir:-missing}); refusing to run integration tests" >&2
        exit 2
    fi
    if [[ "$(uname -s)" == "Darwin" ]]; then
        arch -arm64 "${pg_ctl}" -D "${data_dir}" restart -m fast -w
    else
        "${pg_ctl}" -D "${data_dir}" restart -m fast -w
    fi
}

TEST_TARGETS=()
PROD_TARGETS=()
if has_token unit || has_token lol; then
    TEST_TARGETS+=(bioseq_aligner_tests_exec)
fi
if has_token integration; then
    TEST_TARGETS+=(postgresql_bioseq bioseq_aligner_int_tests_exec)
fi
if has_token bench; then PROD_TARGETS+=(bioseq_bench); fi
if has_token io; then PROD_TARGETS+=(bioseq_io_bench); fi
if has_token framework_example; then
    PROD_TARGETS+=(bioseq_bench_framework_example)
fi

if [[ "${#TEST_TARGETS[@]}" -gt 0 ]]; then
    cmake --preset "${TEST_PRESET}"
    verify_test_index_flavor "${TEST_BUILD_DIR}" "ON"
    for tgt in "${TEST_TARGETS[@]}"; do
        cmake --build "${TEST_BUILD_DIR}" --target "${tgt}" --parallel "${JOBS}"
    done
fi

if [[ "${#PROD_TARGETS[@]}" -gt 0 ]]; then
    cmake --preset "${PROD_PRESET}"
    verify_test_index_flavor "${PROD_BUILD_DIR}" "OFF"
    for tgt in "${PROD_TARGETS[@]}"; do
        cmake --build "${PROD_BUILD_DIR}" --target "${tgt}" --parallel "${JOBS}"
    done
fi

# Prefer production compile commands for a combined invocation; otherwise use
# the isolated test tree so guarded sources remain visible to clangd.
if [[ "${#PROD_TARGETS[@]}" -gt 0 && -f "${PROD_BUILD_DIR}/compile_commands.json" ]]; then
    ln -sfn "${PROD_PRESET}/compile_commands.json" "${BUILD_ROOT}/compile_commands.json"
elif [[ "${#TEST_TARGETS[@]}" -gt 0 && -f "${TEST_BUILD_DIR}/compile_commands.json" ]]; then
    ln -sfn "${TEST_PRESET}/compile_commands.json" "${BUILD_ROOT}/compile_commands.json"
fi

if has_token unit; then
    (cd "${TEST_BUILD_DIR}" && \
        ./bioseq/bioseq_aligner_tests_exec --reporter console::out=-::colour-mode=ansi)
fi

if has_token integration; then
    cmake --install "${TEST_BUILD_DIR}" --component "bioseq"
    restart_postgresql_after_install "${TEST_BUILD_DIR}"
    (cd "${TEST_BUILD_DIR}" && \
        ./bioseq/bioseq_aligner_int_tests_exec --reporter console::out=-::colour-mode=ansi)
fi

if has_token bench; then
    # bench_framework binary is NOT a Catch2 binary — pass extra args
    # through verbatim so `--fast`, `--threads`, `--report` reach the
    # framework's CLI parser.
    if [ "$#" -gt 1 ]; then
        (cd "${PROD_BUILD_DIR}" && ./bioseq/bioseq_bench "${@:2}")
    else
        (cd "${PROD_BUILD_DIR}" && ./bioseq/bioseq_bench)
    fi
fi

if has_token io; then
    # bench_framework binary, same shape as `bench` above.
    if [ "$#" -gt 1 ]; then
        (cd "${PROD_BUILD_DIR}" && ./bioseq/bioseq_io_bench "${@:2}")
    else
        (cd "${PROD_BUILD_DIR}" && ./bioseq/bioseq_io_bench)
    fi
fi

if has_token framework_example; then
    # bench_framework binary is NOT a Catch2 binary — pass the extra args
    # through verbatim, no `--reporter` injection.
    if [ "$#" -gt 1 ]; then
        (cd "${PROD_BUILD_DIR}" && \
            ./bioseq/bioseq_bench_framework_example "${@:2}")
    else
        (cd "${PROD_BUILD_DIR}" && ./bioseq/bioseq_bench_framework_example)
    fi
fi

if has_token lol; then
    (cd "${TEST_BUILD_DIR}" && \
        ./bioseq/bioseq_aligner_tests_exec \
        --reporter console::out=-::colour-mode=ansi "[lol]")
fi
