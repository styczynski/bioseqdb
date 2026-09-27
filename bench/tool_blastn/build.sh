#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export CC=clang CXX=clang++
cmake -S source/c++/src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DNCBI_PTBCFG_PROJECT_TARGETS='blastn;makeblastdb' \
  -DNCBI_PTBCFG_PROJECT_COMPONENTS='-VDB;-GNUTLS;-GCRYPT' \
  -DCMAKE_C_FLAGS_RELEASE='-O3 -DNDEBUG -march=native' \
  -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG -march=native'
cmake --build build --target blastn makeblastdb --parallel 4
mkdir -p bin
find build -type f \( -name blastn -o -name makeblastdb \) -executable -exec cp '{}' bin/ \;
test -x bin/blastn
test -x bin/makeblastdb
