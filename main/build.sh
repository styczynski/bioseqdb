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
# Examples:
#   ./build.sh                              # build + run unit + integration suites
#   ./build.sh integration+bench+unit       # all three suites in order
#   ./build.sh bench+unit dev-opt           # different preset, no integration tests
#   ./build.sh "" dev                       # build only, no tests
set -euo pipefail

TESTS="${1:-unit+integration}"
MODE="${2:-dev-opt-pg}"

mkdir build 2>/dev/null >/dev/null || true

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
cd build
cmake --preset "${MODE}" ..
cmake --build .
cmake --build . --target install

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

if has_token lol; then
    ./bioseq/bioseq_aligner_tests_exec --reporter console::out=-::colour-mode=ansi "[lol]"
fi
