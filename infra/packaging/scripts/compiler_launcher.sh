#!/usr/bin/env bash
# Run every package compiler in its own process group at the deliberately
# minimal real-time priority granted to the package container.  The watchdog
# bounds a wedged compiler without changing its optimization or target flags.
set -Eeuo pipefail

readonly requested_priority="${BIOSEQ_COMPILER_RT_PRIORITY:-1}"
readonly test_mode="${BIOSEQ_COMPILER_WATCHDOG_TEST_MODE:-0}"
readonly timeout_override="${BIOSEQ_COMPILER_WATCHDOG_TIMEOUT_SEC-}"
readonly grace_override="${BIOSEQ_COMPILER_WATCHDOG_GRACE_SEC-}"

usage_error() {
  printf '%s\n' "$1" >&2
  exit 64
}

if [[ "${requested_priority}" != "1" ]]; then
  usage_error "bioseq package compiler priority must be exactly 1"
fi
if [[ "${test_mode}" != "0" && "${test_mode}" != "1" ]]; then
  usage_error "BIOSEQ_COMPILER_WATCHDOG_TEST_MODE must be exactly 1 when enabled"
fi
if [[ "${test_mode}" != "1" && ( -n "${timeout_override}" || -n "${grace_override}" ) ]]; then
  usage_error "bioseq production compiler watchdog limits cannot be overridden"
fi

timeout_sec=3300
grace_sec=60
if [[ "${test_mode}" == "1" ]]; then
  timeout_sec="${timeout_override:-${timeout_sec}}"
  grace_sec="${grace_override:-${grace_sec}}"
fi
if [[ ! "${timeout_sec}" =~ ^[1-9][0-9]*$ || ! "${grace_sec}" =~ ^[1-9][0-9]*$ ]]; then
  usage_error "bioseq compiler watchdog limits must be positive integer seconds"
fi
if (( $# == 0 )); then
  usage_error "bioseq compiler launcher requires a compiler argv"
fi
for tool in chrt setsid ps awk sort sleep uname; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    printf '%s is required for the bioseq package compiler launcher\n' "${tool}" >&2
    exit 69
  fi
done
if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
  printf 'sha256sum or shasum is required for the bioseq package compiler launcher\n' >&2
  exit 69
fi

source_name="unknown"
for argument in "$@"; do
  case "${argument}" in
    *.c|*.cc|*.cpp|*.cppm|*.cxx|*.ixx|*.m|*.mm)
      source_name="${argument##*/}"
      ;;
  esac
done
source_name="$(printf '%s' "${source_name}" | tr -c 'A-Za-z0-9._+-' '_')"
profile="$(printf '%s' "${BIOSEQ_TEMPLATE_FLATTEN_PROFILE:-unknown}" | tr -c 'A-Za-z0-9._+-' '_')"
architecture="$(uname -m | tr -c 'A-Za-z0-9._+-' '_')"
if command -v sha256sum >/dev/null 2>&1; then
  argv_sha256="$(printf '%s\0' "$@" | sha256sum | awk '{print $1}')"
else
  argv_sha256="$(printf '%s\0' "$@" | shasum -a 256 | awk '{print $1}')"
fi

root_pid=""
compiler_pgid=""
timer_pid=""
timer_pgid=""

group_is_alive() {
  [[ -n "${compiler_pgid}" ]] && kill -0 -- "-${compiler_pgid}" 2>/dev/null
}

terminate_compiler_group() {
  local wait_seconds="$1"
  local attempts
  if ! group_is_alive; then
    return
  fi
  kill -TERM -- "-${compiler_pgid}" 2>/dev/null || true
  attempts=$((wait_seconds * 10))
  while (( attempts > 0 )) && group_is_alive; do
    sleep 0.1
    attempts=$((attempts - 1))
  done
  if group_is_alive; then
    kill -KILL -- "-${compiler_pgid}" 2>/dev/null || true
  fi
  attempts=100
  while (( attempts > 0 )) && group_is_alive; do
    sleep 0.1
    attempts=$((attempts - 1))
  done
}

cancel_timer() {
  if [[ -n "${timer_pid}" ]]; then
    if [[ -n "${timer_pgid}" ]]; then
      kill -TERM -- "-${timer_pgid}" 2>/dev/null || true
    else
      kill "${timer_pid}" 2>/dev/null || true
    fi
    wait "${timer_pid}" 2>/dev/null || true
    timer_pid=""
    timer_pgid=""
  fi
}

emergency_cleanup() {
  local status=$?
  trap - EXIT
  cancel_timer
  if group_is_alive; then
    kill -KILL -- "-${compiler_pgid}" 2>/dev/null || true
  fi
  exit "${status}"
}
trap emergency_cleanup EXIT

cancel_for_signal() {
  local exit_status="$1"
  trap - HUP INT TERM
  cancel_timer
  terminate_compiler_group "${grace_sec}"
  if [[ -n "${root_pid}" ]]; then
    wait "${root_pid}" 2>/dev/null || true
  fi
  root_pid=""
  compiler_pgid=""
  exit "${exit_status}"
}
trap 'cancel_for_signal 129' HUP
trap 'cancel_for_signal 130' INT
trap 'cancel_for_signal 143' TERM

timeout_compiler() {
  trap - USR1 HUP INT TERM
  local process_rows pids rss_kib compiler_version
  process_rows="$(ps -eo pid=,pgid=,rss= | awk -v group="${compiler_pgid}" '$2 == group {print $1, $3}')"
  pids="$(printf '%s\n' "${process_rows}" | awk 'NF == 2 {print $1}' | sort -n | awk 'BEGIN {separator=""} {printf "%s%s", separator, $1; separator=","} END {if (separator == "") printf "none"}')"
  rss_kib="$(printf '%s\n' "${process_rows}" | awk 'NF == 2 {sum += $2} END {print sum + 0}')"
  cancel_timer
  terminate_compiler_group "${grace_sec}"
  if [[ -n "${root_pid}" ]]; then
    wait "${root_pid}" 2>/dev/null || true
  fi
  compiler_version="$("$1" --version </dev/null 2>&1 | awk 'NR == 1 {print; exit}' || true)"
  compiler_version="$(printf '%s' "${compiler_version:-unknown}" | tr '[:space:]' '_' | tr -c 'A-Za-z0-9._+:-' '_')"
  printf 'BIOSEQ_COMPILER_WATCHDOG_TIMEOUT source=%s profile=%s arch=%s compiler_version=%s argv_sha256=%s root_pid=%s pgid=%s pids=%s rss_kib=%s timeout_sec=%s grace_sec=%s\n' \
    "${source_name}" "${profile}" "${architecture}" "${compiler_version}" \
    "${argv_sha256}" "${root_pid}" "${compiler_pgid}" "${pids}" \
    "${rss_kib}" "${timeout_sec}" "${grace_sec}" >&2
  root_pid=""
  compiler_pgid=""
  exit 124
}
trap 'timeout_compiler "$@"' USR1

setsid chrt --rr "${requested_priority}" "$@" &
root_pid=$!
compiler_pgid="${root_pid}"

launcher_pid=$$
setsid /bin/bash -c '
  sleep "$1"
  if kill -0 "$2" 2>/dev/null; then
    kill -USR1 "$2" 2>/dev/null || true
  fi
' bioseq-compiler-watchdog "${timeout_sec}" "${launcher_pid}" &
timer_pid=$!
timer_pgid="${timer_pid}"

set +e
wait "${root_pid}"
compiler_status=$?
set -e
cancel_timer

if group_is_alive; then
  terminate_compiler_group "${grace_sec}"
  if (( compiler_status == 0 )); then
    compiler_status=70
  fi
fi
root_pid=""
compiler_pgid=""
exit "${compiler_status}"
