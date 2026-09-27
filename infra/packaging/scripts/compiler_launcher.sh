#!/usr/bin/env bash
# Stable CMake launcher entry point; one supervisor owns compiler wait/timeout.
set -euo pipefail
exec python3 -S "${BASH_SOURCE[0]%/*}/compiler_launcher.py" "$@"
