#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export CC=clang CXX=clang++
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=clang
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=clang
if ! command -v cargo >/dev/null; then sudo apt-get install -y rustc cargo; fi
revision=$(python3 -c 'import json; print(json.load(open("source_lock.json"))["revision"])')
cmake -S source -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DNATIVE_ARCH=ON -DVERSION_OVERRIDE="$revision" \
  -DCMAKE_C_FLAGS_RELEASE='-O3 -DNDEBUG -march=native' \
  -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG -march=native'
cmake --build build --target mmseqs --parallel 4
mkdir -p bin
cp build/src/mmseqs bin/mmseqs
