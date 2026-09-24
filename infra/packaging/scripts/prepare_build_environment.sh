#!/usr/bin/env bash
# Prepare one clean Bioseq package-build container. The project-owned Python
# worker invokes this script as root before rpmbuild/dpkg-buildpackage/makepkg
# changes to an unprivileged build user. Everything is native to the worker
# architecture; no emulation or cross-compilation is permitted.
set -euo pipefail

readonly llvm_major=22
readonly llvm_version=22.1.8
readonly cmake_version=4.2.1
readonly vcpkg_baseline=296b89248ad13be09e9324488eb4f74dd322d26c
readonly tool_root=/opt/bioseq-package-toolchain
readonly llvm_root="${tool_root}/llvm-${llvm_major}"
readonly cmake_root="${tool_root}/cmake"
readonly vcpkg_root="${tool_root}/vcpkg"
readonly vcpkg_cache="${tool_root}/vcpkg-binary-cache"
readonly vcpkg_registry_cache="${tool_root}/vcpkg-registry-cache"
readonly cmake_cache_root=/opt/bioseq-package-cmake-cache

die() {
  echo "bioseq package preparation: $*" >&2
  exit 1
}

[[ "${BIOSEQ_TEMPLATE_FLATTEN_PROFILE:-}" == "all_alphabets_uint64" ]] \
  || die "BIOSEQ_TEMPLATE_FLATTEN_PROFILE must be all_alphabets_uint64"
[[ "${BIOSEQ_POSTGRES_MAJOR:-}" =~ ^[0-9]+$ ]] \
  || die "BIOSEQ_POSTGRES_MAJOR must be a decimal major version"
(( BIOSEQ_POSTGRES_MAJOR >= 17 )) \
  || die "PostgreSQL majors below 17 are unsupported"

case "$(uname -m)" in
  x86_64)
    native_arch=x64
    cmake_arch=x86_64
    llvm_arch=X64
    cmake_sha256=c059bff1e97a2b6b5b0c0872263627486345ad0ed083298cb21cff2eda883980
    llvm_sha256=df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384
    ;;
  aarch64|arm64)
    native_arch=arm64
    cmake_arch=aarch64
    llvm_arch=ARM64
    cmake_sha256=3e178207a2c42af4cd4883127f8800b6faf99f3f5187dccc68bfb2cc7808f5f7
    llvm_sha256=805efad2bb91cb4967fa569e0881d10c0f69c04461cf671cccbae19f547acc34
    ;;
  *) die "unsupported native architecture $(uname -m)" ;;
esac
readonly cmake_sha256 llvm_sha256
[[ "${BIOSEQ_ARCHITECTURE:-}" == "${native_arch}" ]] \
  || die "requested architecture ${BIOSEQ_ARCHITECTURE:-<unset>} does not match native ${native_arch}"

# Every supported package image provides this file.
# shellcheck disable=SC1091
. /etc/os-release
readonly distro_id="${ID:-unknown}"

install_debian_dependencies() {
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends \
    autoconf automake autoconf-archive bison build-essential ca-certificates \
    cpio curl flex gettext git gnupg libssl-dev libtool libltdl-dev m4 \
    libcap2-bin ninja-build pkg-config procps python3 rsync tar unzip util-linux wget xz-utils \
    zip zlib1g-dev binutils \
    libx11-dev libxext-dev libxrandr-dev libxrender-dev libxi-dev \
    libxfixes-dev libxcursor-dev libxinerama-dev libxcb1-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols

  install -d -m 0755 /usr/share/postgresql-common/pgdg
  curl -fsSL https://www.postgresql.org/media/keys/ACCC4CF8.asc \
    -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc
  [[ -n "${VERSION_CODENAME:-}" ]] \
    || die "VERSION_CODENAME is required to configure the PostgreSQL repository"
  printf '%s\n' \
    "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] https://apt.postgresql.org/pub/repos/apt ${VERSION_CODENAME}-pgdg main" \
    > /etc/apt/sources.list.d/pgdg.list
  apt-get update
  apt-get install -y --no-install-recommends \
    "postgresql-${BIOSEQ_POSTGRES_MAJOR}" \
    "postgresql-server-dev-${BIOSEQ_POSTGRES_MAJOR}" libpq-dev
}

install_fedora_dependencies() {
  dnf install -y --allowerasing \
    autoconf autoconf-archive automake bison binutils ca-certificates cpio curl findutils flex \
    gcc gcc-c++ gettext git gzip kernel-headers libcap libtool m4 make ninja-build openssl-devel \
    patch perl pkgconf-pkg-config procps-ng python3 rpm-build rsync tar unzip util-linux wget \
    xz zip zlib-devel \
    libX11-devel libXext-devel libXrandr-devel libXrender-devel libXi-devel \
    libXfixes-devel libXcursor-devel libXinerama-devel libxcb-devel \
    libxkbcommon-devel wayland-devel wayland-protocols-devel

  # Fedora's versioned server headers and pg_config are supplied by the
  # official server-devel package. Do not fall back to PGDG here: Fedora 44's
  # PGDG repository does not publish the requested aarch64 repo RPM.
  dnf install -y --allowerasing \
    "postgresql${BIOSEQ_POSTGRES_MAJOR}-server-devel" \
    "postgresql${BIOSEQ_POSTGRES_MAJOR}-server"
}

install_opensuse_dependencies() {
  zypper --non-interactive install -y --allow-vendor-change \
    autoconf autoconf-archive automake bison binutils ca-certificates cpio curl flex gcc gcc-c++ \
    gettext-tools git gzip libcap-progs libopenssl-devel libtool m4 make ninja patch \
    pkg-config procps python3 rpm-build rsync tar unzip util-linux wget xz zip \
    libX11-devel libXext-devel libXrandr-devel libXrender-devel libXi-devel \
    libXfixes-devel libXcursor-devel libXinerama-devel libxcb-devel \
    libxkbcommon-devel wayland-devel wayland-protocols-devel

  if ! zypper --non-interactive install -y --allow-vendor-change \
      "postgresql${BIOSEQ_POSTGRES_MAJOR}-devel" \
      "postgresql${BIOSEQ_POSTGRES_MAJOR}-server-devel" \
      "postgresql${BIOSEQ_POSTGRES_MAJOR}-server"; then
    zypper --non-interactive addrepo --refresh \
      "https://download.opensuse.org/repositories/server:database:postgresql/${VERSION_ID}/server:database:postgresql.repo" \
      bioseq-postgresql
    zypper --gpg-auto-import-keys --non-interactive refresh
    zypper --non-interactive install -y --allow-vendor-change \
      "postgresql${BIOSEQ_POSTGRES_MAJOR}-devel" \
      "postgresql${BIOSEQ_POSTGRES_MAJOR}-server-devel" \
      "postgresql${BIOSEQ_POSTGRES_MAJOR}-server"
  fi
}

install_arch_dependencies() {
  pacman -Syu --noconfirm --needed \
    autoconf autoconf-archive automake base-devel binutils bison ca-certificates cpio curl flex \
    gettext git libcap libtool m4 ninja openssl patch pkgconf procps-ng python rsync tar unzip \
    util-linux wget xz zip zlib libx11 libxext libxrandr libxrender libxi \
    libxfixes libxcursor libxinerama libxcb libxkbcommon wayland-protocols

  if [[ "${BIOSEQ_POSTGRES_MAJOR}" == "17" ]]; then
    pacman -S --noconfirm --needed postgresql-old-upgrade
  else
    # On rolling Arch, `postgresql` is the current major. The strict pg_config
    # check below makes this future-facing strategy fail closed when the
    # requested matrix major is not the repository's current major.
    pacman -S --noconfirm --needed postgresql
  fi
}

case "${distro_id}" in
  debian|ubuntu) install_debian_dependencies ;;
  fedora) install_fedora_dependencies ;;
  opensuse*|sles) install_opensuse_dependencies ;;
  arch|archarm) install_arch_dependencies ;;
  *) die "unsupported distro ID ${distro_id}" ;;
esac

# Arch's makepkg drops to an unprivileged user. Give only the ephemeral chrt
# executable the capability it needs so the launcher can set SCHED_RR/1 and
# immediately exec the unprivileged compiler. The container's SYS_NICE
# bounding capability is still required; failure to set/retain it is fatal.
chrt_binary="$(command -v chrt)"
readonly chrt_binary
setcap cap_sys_nice=ep "${chrt_binary}"
getcap "${chrt_binary}" | grep -Eq 'cap_sys_nice(=|\+)ep' \
  || die "chrt did not retain the required cap_sys_nice file capability"

mkdir -p "${tool_root}"

download_verified() {
  local url="$1"
  local destination="$2"
  local expected_sha256="$3"
  local partial="${destination}.partial"
  local actual_sha256=

  rm -f "${partial}"
  curl --fail --location --retry 5 --retry-all-errors \
    "${url}" -o "${partial}"
  actual_sha256="$(sha256sum "${partial}" | awk '{print $1}')"
  if [[ "${actual_sha256}" != "${expected_sha256}" ]]; then
    rm -f "${partial}"
    die "SHA256 mismatch for ${url}: expected ${expected_sha256}, got ${actual_sha256}"
  fi
  mv -f "${partial}" "${destination}"
}

install_cmake() {
  if [[ -x "${cmake_root}/bin/cmake" ]] \
      && "${cmake_root}/bin/cmake" --version | head -n 1 | grep -Fq "${cmake_version}"; then
    return
  fi
  local archive=/tmp/bioseq-cmake.tar.gz
  download_verified \
    "https://github.com/Kitware/CMake/releases/download/v${cmake_version}/cmake-${cmake_version}-linux-${cmake_arch}.tar.gz" \
    "${archive}" "${cmake_sha256}"
  rm -rf "${cmake_root}"
  mkdir -p "${cmake_root}"
  tar --strip-components=1 -xzf "${archive}" -C "${cmake_root}"
  "${cmake_root}/bin/cmake" --version | head -n 1 \
    | grep -F "${cmake_version}" >/dev/null \
    || die "CMake ${cmake_version} installation failed"
}

normalize_libcxx_module_metadata() {
  local modules_json=
  modules_json="$(find "${llvm_root}" -name libc++.modules.json -type f -print -quit)"
  [[ -n "${modules_json}" ]] || die "LLVM archive lacks libc++.modules.json"
  LLVM_ROOT="${llvm_root}" MODULES_JSON="${modules_json}" python3 - <<'PY'
import json
import os
from pathlib import Path

llvm_root = Path(os.environ["LLVM_ROOT"]).resolve()
metadata_path = Path(os.environ["MODULES_JSON"]).resolve()
include_path = llvm_root / "include" / "c++" / "v1"
if not (include_path / "__config").is_file():
    raise SystemExit(f"libc++ headers are incomplete under {include_path}")
target_include_path = (
    llvm_root / "include" / metadata_path.parent.name / "c++" / "v1"
)
if not (target_include_path / "__config_site").is_file():
    raise SystemExit(
        "libc++ target headers are incomplete under "
        f"{target_include_path}"
    )
# CMake's libc++ module-metadata consumer currently forwards one include root
# for the standard-library module compile.  Keep the target configuration next
# to the generic libc++ headers so that the single portable metadata root is
# complete on both x86_64 and aarch64 installations.
generic_config_site = include_path / "__config_site"
target_config_site = target_include_path / "__config_site"
if (not generic_config_site.is_file()
        or generic_config_site.read_bytes() != target_config_site.read_bytes()):
    temporary_config_site = generic_config_site.with_suffix(".tmp")
    temporary_config_site.write_bytes(target_config_site.read_bytes())
    os.replace(temporary_config_site, generic_config_site)
document = json.loads(metadata_path.read_text(encoding="utf-8"))
relative_include = os.path.relpath(include_path, metadata_path.parent)
for module in document.get("modules", []):
    local_arguments = module.setdefault("local-arguments", {})
    local_arguments["system-include-directories"] = [relative_include]
temporary = metadata_path.with_suffix(metadata_path.suffix + ".tmp")
temporary.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
os.replace(temporary, metadata_path)
PY
}

install_llvm() {
  local current=
  if [[ -x "${llvm_root}/bin/clang++" ]]; then
    current="$("${llvm_root}/bin/clang++" --version \
      | sed -nE 's/.*version ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' | head -n 1)"
  fi
  if [[ "${current}" == "${llvm_version}" ]] \
      && find "${llvm_root}" -name libc++.a -type f -print -quit | grep -q . \
      && find "${llvm_root}" -name libc++abi.a -type f -print -quit | grep -q . \
      && find "${llvm_root}" -name libunwind.a -type f -print -quit | grep -q .; then
    normalize_libcxx_module_metadata
    return
  fi

  local archive=/tmp/bioseq-llvm.tar.xz
  local url="https://github.com/llvm/llvm-project/releases/download/llvmorg-${llvm_version}/LLVM-${llvm_version}-Linux-${llvm_arch}.tar.xz"
  download_verified "${url}" "${archive}" "${llvm_sha256}"
  rm -rf "${llvm_root}"
  mkdir -p "${llvm_root}"
  tar --strip-components=1 -xJf "${archive}" -C "${llvm_root}"

  current="$("${llvm_root}/bin/clang++" --version \
    | sed -nE 's/.*version ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' | head -n 1)"
  [[ "${current}" == "${llvm_version}" ]] \
    || die "installed Clang version ${current:-unknown} is not pinned ${llvm_version}"
  [[ -x "${llvm_root}/bin/llvm-config" ]] || die "LLVM archive lacks llvm-config"
  [[ -x "${llvm_root}/bin/clang-scan-deps" ]] || die "LLVM archive lacks clang-scan-deps"
  find "${llvm_root}" -name libc++.modules.json -type f -print -quit | grep -q . \
    || die "LLVM archive lacks libc++.modules.json"
  find "${llvm_root}" \( -name 'libclang.so' -o -name 'libclang.so.*' \) -print -quit | grep -q . \
    || die "LLVM archive lacks libclang"
  find "${llvm_root}" -name 'libc++.a' -type f -print -quit | grep -q . \
    || die "LLVM archive lacks static libc++ runtime"
  find "${llvm_root}" -name 'libc++abi.a' -type f -print -quit | grep -q . \
    || die "LLVM archive lacks static libc++abi runtime"
  find "${llvm_root}" -name 'libunwind.a' -type f -print -quit | grep -q . \
    || die "LLVM archive lacks static libunwind runtime"
  normalize_libcxx_module_metadata
}

install_vcpkg() {
  local installed_commit=
  if [[ -d "${vcpkg_root}/.git" ]]; then
    installed_commit="$(git -C "${vcpkg_root}" rev-parse HEAD 2>/dev/null || true)"
  fi
  if [[ ! -x "${vcpkg_root}/vcpkg" \
      || "${installed_commit}" != "${vcpkg_baseline}" ]]; then
    rm -rf "${vcpkg_root}"
    git clone https://github.com/microsoft/vcpkg "${vcpkg_root}"
    git -c advice.detachedHead=false -C "${vcpkg_root}" \
      checkout --detach "${vcpkg_baseline}"
    [[ "$(git -C "${vcpkg_root}" rev-parse HEAD)" == "${vcpkg_baseline}" ]] \
      || die "vcpkg checkout did not resolve the pinned baseline"
    "${vcpkg_root}/bootstrap-vcpkg.sh" -disableMetrics
  fi
  mkdir -p "${vcpkg_cache}" "${vcpkg_registry_cache}"
  if [[ ! -d "${vcpkg_registry_cache}/git/.git" ]]; then
    rm -rf "${vcpkg_registry_cache}/git"
    git clone --local --no-checkout \
      "${vcpkg_root}" "${vcpkg_registry_cache}/git"
  fi
  chmod -R a+rwX \
    "${vcpkg_root}" "${vcpkg_cache}" "${vcpkg_registry_cache}"
}

install_cmake
install_llvm
install_vcpkg

# This bind mount lives on the builder's persistent EBS volume. Package tools
# later drop privileges, so make the cell-specific cache traversable and
# writable before rpmbuild/dpkg-buildpackage/makepkg starts.
mkdir -p "${cmake_cache_root}/build"
chmod -R a+rwX "${cmake_cache_root}"

pg_config_path="$(BIOSEQ_POSTGRES_MAJOR="${BIOSEQ_POSTGRES_MAJOR}" \
  python3 /source/infra/packaging/scripts/resolve_pg_config.py)"
[[ -x "${pg_config_path}" ]] || die "resolved pg_config is not executable"

mkdir -p /opt/bioseq-package-toolchain/state
printf '%s\n' "${pg_config_path}" \
  > "/opt/bioseq-package-toolchain/state/pg_config-${BIOSEQ_POSTGRES_MAJOR}"
chmod 0644 "/opt/bioseq-package-toolchain/state/pg_config-${BIOSEQ_POSTGRES_MAJOR}"

echo "bioseq package preparation complete: distro=${distro_id} arch=${native_arch} PostgreSQL=${BIOSEQ_POSTGRES_MAJOR}"
