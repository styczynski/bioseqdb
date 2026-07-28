#!/usr/bin/env bash
# Shared native build/install recipe used by RPM, DEB, and pacman templates.
set -euo pipefail

die() {
  echo "bioseq package build: $*" >&2
  exit 1
}

[[ "${BIOSEQ_OPTTIMIZATION_LEVEL:-}" == "0" ]] \
  || die "every package build must explicitly set BIOSEQ_OPTTIMIZATION_LEVEL=0"

readonly action="${1:-}"
readonly package_metadata_dir="${2:-}"
readonly build_dir=/opt/bioseq-package-cmake-cache/build
[[ "${action}" == "build" || "${action}" == "install" ]] \
  || die "usage: build_and_stage.sh {build BUILD_DIR PG_MAJOR|install BUILD_DIR DESTDIR PG_MAJOR}"
[[ -n "${package_metadata_dir}" ]] || die "package metadata directory is required"

if [[ "${action}" == "build" ]]; then
  readonly postgres_major="${3:-}"
  readonly destdir=""
else
  readonly destdir="${3:-}"
  readonly postgres_major="${4:-}"
  [[ -n "${destdir}" && "${destdir}" == /* ]] \
    || die "install DESTDIR must be an absolute path"
fi
[[ "${postgres_major}" =~ ^[0-9]+$ ]] || die "PostgreSQL major must be decimal"

readonly tool_root=/opt/bioseq-package-toolchain
readonly llvm_root="${tool_root}/llvm-22"
readonly cmake_bin="${tool_root}/cmake/bin/cmake"
readonly vcpkg_root="${tool_root}/vcpkg"
readonly compiler_launcher="${PWD}/infra/packaging/scripts/compiler_launcher.sh"
readonly pg_state="${tool_root}/state/pg_config-${postgres_major}"

[[ -x "${cmake_bin}" ]] || die "prepared CMake is missing at ${cmake_bin}"
[[ -x "${llvm_root}/bin/clang++" ]] || die "prepared Clang is missing"
[[ -x "${vcpkg_root}/vcpkg" ]] || die "prepared vcpkg is missing"
[[ -x "${compiler_launcher}" ]] || die "compiler launcher is missing"
[[ -f "${pg_state}" ]] || die "prepared pg_config state is missing for PostgreSQL ${postgres_major}"
pg_config="$(<"${pg_state}")"
readonly pg_config
[[ -x "${pg_config}" ]] || die "prepared pg_config is no longer executable"

actual_pg_major="$("${pg_config}" --version | sed -nE 's/^PostgreSQL ([0-9]+).*/\1/p')"
[[ "${actual_pg_major}" == "${postgres_major}" ]] \
  || die "pg_config major ${actual_pg_major:-unknown} does not equal requested ${postgres_major}"

case "$(uname -m)" in
  x86_64)
    readonly vcpkg_triplet=x64-linux-libcxx
    readonly vcpkg_arch=x64
    readonly expected_arch=x64
    ;;
  aarch64|arm64)
    readonly vcpkg_triplet=arm64-linux-libcxx
    readonly vcpkg_arch=arm64
    readonly expected_arch=arm64
    ;;
  *) die "unsupported native architecture $(uname -m)" ;;
esac
[[ "${BIOSEQ_ARCHITECTURE:-${expected_arch}}" == "${expected_arch}" ]] \
  || die "requested architecture does not match native architecture"

libcxx_modules="$(find "${llvm_root}" -name libc++.modules.json -type f -print -quit)"
readonly libcxx_modules
[[ -f "${libcxx_modules}" ]] || die "libc++.modules.json is missing"
libcxx_archive="$(find "${llvm_root}" -name libc++.a -type f -print -quit)"
libcxxabi_archive="$(find "${llvm_root}" -name libc++abi.a -type f -print -quit)"
libunwind_archive="$(find "${llvm_root}" -name libunwind.a -type f -print -quit)"
readonly libcxx_archive libcxxabi_archive libunwind_archive
[[ -f "${libcxx_archive}" ]] || die "pinned static libc++.a is missing"
[[ -f "${libcxxabi_archive}" ]] || die "pinned static libc++abi.a is missing"
[[ -f "${libunwind_archive}" ]] || die "pinned static libunwind.a is missing"

# This preflight is especially important for pacman builds, where makepkg runs
# this script as an unprivileged user. The chrt file capability prepared in the
# root setup phase must survive that UID transition.
"${compiler_launcher}" /bin/true

export CMAKE_NAMED_ENV_TYPE=AWS_MACHINE_LINUX_CLANG22
export BIOSEQ_LLVM_ROOT="${llvm_root}"
export BIOSEQ_LIBCXX_MODULES_JSON="${libcxx_modules}"
export BIOSEQ_CODEGEN_LLVM_CONFIG="${llvm_root}/bin/llvm-config"
export BIOSEQ_CODEGEN_CLANG_BINARY="${llvm_root}/bin/clang++"
export BIOSEQ_COMPILER_LAUNCHER="${compiler_launcher}"
export BIOSEQ_COMPILER_RT_PRIORITY=1
export VCPKG_ROOT="${vcpkg_root}"
export VCPKG_DEFAULT_BINARY_CACHE=/opt/bioseq-package-toolchain/vcpkg-binary-cache
export VCPKG_DISABLE_METRICS=1
export POSTGRESQL_VERSION="${postgres_major}"

if [[ "${action}" == "build" ]]; then
  mkdir -p "${build_dir}" "${package_metadata_dir}"

  python3 infra/packaging/scripts/prove_static_runtime.py \
    --cxx "${llvm_root}/bin/clang++" \
    --libcxx "${libcxx_archive}" \
    --libcxxabi "${libcxxabi_archive}" \
    --libunwind "${libunwind_archive}" \
    --architecture "${expected_arch}"

  "${cmake_bin}" -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${vcpkg_root}/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_C_COMPILER_LAUNCHER="${compiler_launcher}" \
    -DCMAKE_CXX_COMPILER_LAUNCHER="${compiler_launcher}" \
    -DCMAKE_C_FLAGS= \
    -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
    -DCMAKE_C_FLAGS_RELEASE="-O0 -DNDEBUG -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O0 -DNDEBUG -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++ -static-libstdc++ -static-libgcc" \
    -DCMAKE_SKIP_RPATH=ON \
    -DPostgreSQL_CONFIG="${pg_config}" \
    -DBIOSEQ_PACKAGE_BUILD=ON \
    -DBIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON="${libcxx_modules}" \
    -DBIOSEQ_PACKAGE_LIBCXX_ARCHIVE="${libcxx_archive}" \
    -DBIOSEQ_PACKAGE_LIBCXXABI_ARCHIVE="${libcxxabi_archive}" \
    -DBIOSEQ_PACKAGE_LIBUNWIND_ARCHIVE="${libunwind_archive}" \
    -DBIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE=0 \
    -DBIOSEQ_BUILD_CORE=ON \
    -DBIOSEQ_BUILD_CACHE=ON \
    -DBIOSEQ_BUILD_PAPERS=OFF \
    -DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=OFF \
    -DBIOSEQ_BUILD_WEBSITE=OFF \
    -DBIOSEQ_TEST_INDEXES=OFF \
    -DBIOSEQ_CACHE_BUILD_TESTS=OFF \
    -DBIOSEQ_CACHE_TESTING=OFF \
    -DBIOSEQ_HEAVY_JOBS=1 \
    -DVCPKG_TARGET_TRIPLET="${vcpkg_triplet}" \
    -DVCPKG_TARGET_ARCHITECTURE="${vcpkg_arch}" \
    -DVCPKG_OVERLAY_TRIPLETS="${PWD}/cmake/triplets" \
    -DVCPKG_OVERLAY_PORTS="${PWD}/cmake/ports"

  python3 infra/packaging/scripts/verify_compile_optimization.py \
    "${build_dir}/compile_commands.json"

  grep -Fqx 'BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE:STRING=0' "${build_dir}/CMakeCache.txt" \
    || die "configured build did not preserve BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE=0"
  grep -Fqx "PostgreSQL_CONFIG:FILEPATH=${pg_config}" "${build_dir}/CMakeCache.txt" \
    || die "configured build did not preserve exact PostgreSQL_CONFIG"
  grep -Fqx 'BIOSEQ_PACKAGE_BUILD:BOOL=ON' "${build_dir}/CMakeCache.txt" \
    || die "configured build is not marked as a package build"

  readonly parallelism="${BIOSEQ_PACKAGE_PARALLELISM:-$(nproc)}"
  [[ "${parallelism}" =~ ^[1-9][0-9]*$ ]] || die "invalid package parallelism"
  "${cmake_bin}" --build "${build_dir}" --parallel "${parallelism}" \
    --target postgresql_bioseq postgresql_bioseq_cache

  mapfile -t compile_databases < <(find "${build_dir}" \
    -name compile_commands.json -type f -print)
  [[ "${#compile_databases[@]}" -gt 0 ]] \
    || die "package build emitted no compile_commands.json"
  python3 infra/packaging/scripts/verify_compile_optimization.py \
    "${compile_databases[@]}"

  mapfile -t built_dsos < <(find "${build_dir}" -type f \
    \( -name bioseq.so -o -name bioseq_cache.so \) -print)
  [[ "${#built_dsos[@]}" == "2" ]] \
    || die "expected exactly two built extension DSOs, found ${#built_dsos[@]}"
  for dso in "${built_dsos[@]}"; do
    python3 infra/packaging/scripts/validate_package.py \
      --dso "${dso}" --architecture "${expected_arch}"
  done
  exit 0
fi

grep -Fqx 'BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE:STRING=0' "${build_dir}/CMakeCache.txt" \
  || die "install attempted from a build without optimization level 0"
grep -Fqx "PostgreSQL_CONFIG:FILEPATH=${pg_config}" "${build_dir}/CMakeCache.txt" \
  || die "install attempted from the wrong PostgreSQL build"

mkdir -p "${destdir}"
DESTDIR="${destdir}" "${cmake_bin}" --install "${build_dir}" --component bioseq
python3 infra/packaging/scripts/validate_package.py \
  --staged-root "${destdir}" \
  --pg-config "${pg_config}" \
  --postgres-major "${postgres_major}" \
  --architecture "${expected_arch}"

find "${destdir}" -type f -printf '/%P\n' | LC_ALL=C sort \
  > "${package_metadata_dir}/package-files.list"
[[ "$(wc -l < "${package_metadata_dir}/package-files.list")" == "6" ]] \
  || die "staged package must contain exactly the six extension artifacts"
