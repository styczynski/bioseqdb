#!/usr/bin/env bash
# Run compiler processes at the deliberately minimal real-time priority granted
# to OmniPackage's typed SYS_NICE container. Keeping this in a launcher makes
# the policy apply to CMake's main compile graph without altering compiler
# command lines or generated module dependency information.
set -euo pipefail

readonly requested_priority="${BIOSEQ_COMPILER_RT_PRIORITY:-1}"
if [[ "${requested_priority}" != "1" ]]; then
  echo "bioseq package compiler priority must be exactly 1" >&2
  exit 64
fi
if ! command -v chrt >/dev/null 2>&1; then
  echo "chrt is required for the bioseq package compiler launcher" >&2
  exit 69
fi

exec chrt --rr "${requested_priority}" "$@"
