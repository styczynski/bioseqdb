#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export CC=clang CXX=clang++ CGO_ENABLED=1
if ! command -v go >/dev/null; then sudo apt-get install -y golang-go; fi
mkdir -p bin
cd source
# Go's pinned module toolchain is downloaded and authenticated by the Go tool.
GOTOOLCHAIN=auto go build -trimpath -o ../bin/lexicmap ./lexicmap
