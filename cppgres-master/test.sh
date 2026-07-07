#!/usr/bin/env bash

set -Eeuo pipefail

if [ -z "${PG_CONFIG:-}" ]; then
  echo "PG_CONFIG must be configured"
  exit 1
fi

if [ -z "${TEST_MODULE_PATH:-}" ]; then
  if [ -z "${BUILD_DIR:-}" ]; then
    echo "TEST_MODULE_PATH or BUILD_DIR must be configured"
    exit 1
  fi
  TEST_MODULE_PATH="${BUILD_DIR}/libcppgres_tests.so"
fi

if [ ! -f "${TEST_MODULE_PATH}" ]; then
  echo "test module not found: ${TEST_MODULE_PATH}"
  exit 1
fi

_pg_bindir="$("${PG_CONFIG}" --bindir)"
testdb="$(mktemp -d "${TMPDIR:-/tmp}/cppgres-testdb.XXXXXX")"
socket_dir="$(cd "${testdb}" && pwd -P)"
server_started=0

cleanup() {
  local status=$?
  trap - EXIT INT TERM
  if [ "${server_started}" -eq 1 ]; then
    "${_pg_bindir}/pg_ctl" -D "${testdb}" stop -m fast >/dev/null 2>&1 || true
  fi
  rm -rf "${testdb}"
  exit "${status}"
}

trap cleanup EXIT INT TERM

"${_pg_bindir}/initdb" -D "${testdb}" --no-sync --locale=C --encoding=UTF8
"${_pg_bindir}/pg_ctl" -D "${testdb}" start \
  -o "-c listen_addresses='' -c unix_socket_directories='${socket_dir}'"
server_started=1

"${_pg_bindir}/psql" -v ON_ERROR_STOP=1 -h "${socket_dir}" -d postgres \
  -c "load '${TEST_MODULE_PATH}';"
"${_pg_bindir}/psql" -v ON_ERROR_STOP=1 -h "${socket_dir}" -d postgres \
  -c "call cppgres_tests();"
