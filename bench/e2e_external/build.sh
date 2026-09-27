#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
test "$(uname -s)" = Linux
mkdir -p build
clang++ -std=c++20 -O3 -march=native --precompile index.cppm -o build/e2e_external.pcm
clang++ -std=c++20 -O3 -march=native -c build/e2e_external.pcm -o build/index.o
clang++ -std=c++20 -O3 -march=native -x c++ main.cppm -x none build/index.o \
  -fmodule-file=bioseq.bench.e2e_external=build/e2e_external.pcm -o build/e2e_external
python3 test_audit.py
