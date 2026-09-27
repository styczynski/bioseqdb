#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export CC=clang CXX=clang++
export CFLAGS='-O3 -march=native -DNDEBUG -fsigned-char'
export CXXFLAGS='-O3 -march=native -DNDEBUG -fsigned-char'
if ! command -v make >/dev/null; then sudo apt-get install -y make; fi
if ! pkg-config --exists libcurl; then sudo apt-get install -y libcurl4-openssl-dev; fi
flags='-O3 -march=native -DNDEBUG -D_FILE_OFFSET_BITS=64 -fsigned-char -DOMP_PER_READ_THREADS=1 -DHAVE_KALLOC -fopenmp -std=c++11 -Wno-sign-compare -Wno-write-strings -Wno-unused-but-set-variable'
arch=()
if [ "$(uname -m)" = aarch64 ]; then arch=(arm_neon=1 aarch64=1); fi
mkdir -p build
recipe=$(sha256sum build.sh | cut -d ' ' -f 1)
if [ ! -f build/recipe ] || [ "$(cat build/recipe)" != "$recipe" ]; then
  make -C source clean CC=clang CXX=clang++ BUILDOPTIMIZED=1 CPPFLAGS="$flags" "${arch[@]}"
fi
make -C source -j4 CC=clang CXX=clang++ BUILDOPTIMIZED=1 CPPFLAGS="$flags" "${arch[@]}"
mkdir -p bin
cp source/bin/winnowmap source/bin/meryl bin/
printf '%s\n' "$recipe" > build/recipe
