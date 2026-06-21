#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# bootstrap.sh — install the full bioseq build toolchain on Ubuntu 24.04 x86_64.
#
# Installs: system/X11/Wayland build deps (for the vcpkg Vulkan ports), clang +
# libc++ 22, CMake 4.2.1 (the version whose import-std UUID is hardcoded in
# CMakeLists.txt's AWS_MACHINE_X64 block), a recent Ninja, PostgreSQL 17
# (pg_config + server headers + libpq), and vcpkg. Then it persists a shared
# environment and (optionally) runs verify.sh.
#
# Runs as root from user-data; idempotent and safe to re-run over SSH.
# ---------------------------------------------------------------------------
set -uo pipefail
exec > >(tee -a /var/log/bioseq-provision.log) 2>&1
echo "=============================================================="
echo "[bootstrap] $(date -u) starting heavy provisioning"

[ -f /opt/bioseq/provision.env ] && . /opt/bioseq/provision.env || true
TARGET_USER="${TARGET_USER:-ubuntu}"
USER_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
REPO_DEST="${REPO_DEST:-$USER_HOME/bioseqdb}"
RUN_VERIFY="${RUN_VERIFY:-1}"
LLVM_VERSION=22
CMAKE_VERSION=4.2.1
NINJA_VERSION=1.12.1
PG_VERSION=17
export DEBIAN_FRONTEND=noninteractive

mkdir -p /opt/bioseq
status() { echo "$1" > /opt/bioseq/PROVISION_STATUS; }
fail()   { echo "[bootstrap] FATAL: $*" >&2; status "BOOTSTRAP_FAILED:$*"; exit 1; }
status "BOOTSTRAP_RUNNING"

# --- 1. System build dependencies (incl. vcpkg Vulkan WSI prerequisites) -----
echo "[bootstrap] apt: base + build + graphics deps"
for i in 1 2 3 4 5; do apt-get update -y && break || sleep 15; done
apt-get install -y --no-install-recommends \
  build-essential g++ gcc make pkg-config ca-certificates curl wget git unzip zip tar \
  gnupg lsb-release software-properties-common \
  python3 python3-pip python3-venv python3-jinja2 python3-setuptools \
  bison flex autoconf automake autoconf-archive libtool m4 gettext \
  rsync ninja-build \
  zlib1g-dev libssl-dev \
  xorg-dev libx11-dev libxext-dev libxrandr-dev libxrender-dev libxi-dev \
  libxfixes-dev libxcursor-dev libxinerama-dev \
  libxcb1-dev libxcb-util-dev libxcb-keysyms1-dev libxcb-icccm4-dev libxcb-image0-dev \
  libxcb-randr0-dev libxcb-xinerama0-dev libxcb-xkb-dev libxcb-shape0-dev libxcb-sync-dev \
  libxcb-render-util0-dev libxcb-cursor-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev \
  || fail "apt base deps"

# --- 2. clang / libc++ 22 (apt.llvm.org, with prebuilt-tarball fallback) -----
echo "[bootstrap] LLVM/clang $LLVM_VERSION"
install_llvm() {
  [ -x "/usr/lib/llvm-$LLVM_VERSION/bin/clang++" ] && return 0
  # Preferred: apt.llvm.org (installs into the canonical /usr/lib/llvm-N path).
  if curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh; then
    chmod +x /tmp/llvm.sh
    /tmp/llvm.sh "$LLVM_VERSION" all || true
    apt-get install -y \
      "clang-$LLVM_VERSION" "lld-$LLVM_VERSION" "llvm-$LLVM_VERSION-dev" \
      "libc++-$LLVM_VERSION-dev" "libc++abi-$LLVM_VERSION-dev" "libunwind-$LLVM_VERSION-dev" || true
    [ -x "/usr/lib/llvm-$LLVM_VERSION/bin/clang++" ] && return 0
  fi
  echo "[bootstrap] apt.llvm.org unavailable; falling back to official LLVM release tarball"
  for v in 22.1.6 22.1.5 22.1.4 22.1.3 22.1.2 22.1.1 22.1.0; do
    for name in "LLVM-$v-Linux-X64.tar.xz" \
                "clang+llvm-$v-x86_64-linux-gnu-ubuntu-24.04.tar.xz" \
                "clang+llvm-$v-x86_64-linux-gnu-ubuntu-22.04.tar.xz" \
                "clang+llvm-$v-x86_64-linux-gnu.tar.xz"; do
      url="https://github.com/llvm/llvm-project/releases/download/llvmorg-$v/$name"
      curl -fIL "$url" >/dev/null 2>&1 || continue
      echo "[bootstrap] downloading $url"
      curl -fL "$url" -o /tmp/llvm.tar.xz || continue
      rm -rf "/opt/llvm-$LLVM_VERSION"; mkdir -p "/opt/llvm-$LLVM_VERSION"
      tar --strip-components=1 -xJf /tmp/llvm.tar.xz -C "/opt/llvm-$LLVM_VERSION" || continue
      ln -sfn "/opt/llvm-$LLVM_VERSION" "/usr/lib/llvm-$LLVM_VERSION"
      [ -x "/usr/lib/llvm-$LLVM_VERSION/bin/clang++" ] && return 0
    done
  done
  return 1
}
install_llvm || fail "llvm install (apt + tarball both failed)"
"/usr/lib/llvm-$LLVM_VERSION/bin/clang" --version | head -1 || fail "clang not runnable"

# --- 3. CMake (pinned: its import-std UUID is hardcoded in CMakeLists.txt) ----
echo "[bootstrap] CMake $CMAKE_VERSION"
if ! /opt/cmake/bin/cmake --version 2>/dev/null | grep -q "$CMAKE_VERSION"; then
  url="https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz"
  curl -fL "$url" -o /tmp/cmake.tgz || fail "download cmake"
  rm -rf /opt/cmake; mkdir -p /opt/cmake
  tar --strip-components=1 -xzf /tmp/cmake.tgz -C /opt/cmake || fail "extract cmake"
fi
for t in cmake ctest cpack; do ln -sf "/opt/cmake/bin/$t" "/usr/local/bin/$t"; done
/opt/cmake/bin/cmake --version | head -1

# --- 4. Ninja (recent build for robust C++ module dyndep) --------------------
echo "[bootstrap] Ninja $NINJA_VERSION"
if ! /usr/local/bin/ninja --version 2>/dev/null | grep -q "$NINJA_VERSION"; then
  curl -fL "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VERSION/ninja-linux.zip" -o /tmp/ninja.zip || fail "download ninja"
  unzip -o /tmp/ninja.zip -d /usr/local/bin || fail "unzip ninja"
  chmod +x /usr/local/bin/ninja
fi
/usr/local/bin/ninja --version

# --- 5. PostgreSQL 17 (pg_config + server headers + libpq) -------------------
echo "[bootstrap] PostgreSQL $PG_VERSION (PGDG)"
if [ ! -x "/usr/lib/postgresql/$PG_VERSION/bin/pg_config" ]; then
  install -d /usr/share/postgresql-common/pgdg
  curl -fsSL https://www.postgresql.org/media/keys/ACCC4CF8.asc \
    -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc || fail "pgdg key"
  echo "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] https://apt.postgresql.org/pub/repos/apt $(lsb_release -cs)-pgdg main" \
    > /etc/apt/sources.list.d/pgdg.list
  apt-get update -y
  apt-get install -y "postgresql-$PG_VERSION" "postgresql-server-dev-$PG_VERSION" libpq-dev || fail "postgres install"
fi
"/usr/lib/postgresql/$PG_VERSION/bin/pg_config" --version

# --- 6. vcpkg ----------------------------------------------------------------
echo "[bootstrap] vcpkg"
VCPKG_ROOT=/opt/vcpkg
if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
  rm -rf "$VCPKG_ROOT"
  git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT" || fail "clone vcpkg"
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics || fail "bootstrap vcpkg"
fi
chown -R "$TARGET_USER:$TARGET_USER" "$VCPKG_ROOT" 2>/dev/null || true
# Shared binary cache so reconfigures (and a clean re-provision sharing a snapshot) stay fast.
mkdir -p /opt/vcpkg-cache && chmod 0777 /opt/vcpkg-cache

# --- 7. Discover the libc++ modules.json (multiarch sub-path varies) ---------
MODULES_JSON="$(find "/usr/lib/llvm-$LLVM_VERSION/" -name 'libc++.modules.json' 2>/dev/null | head -1)"
echo "[bootstrap] libc++ modules.json = ${MODULES_JSON:-<NOT FOUND>}"
[ -n "$MODULES_JSON" ] || echo "[bootstrap] WARNING: libc++.modules.json not found; import std may fail to configure"

# --- 8. Persist environment for SSH sessions + verify.sh --------------------
cat > /etc/profile.d/bioseq.sh <<EOF
export VCPKG_ROOT=$VCPKG_ROOT
export VCPKG_DEFAULT_BINARY_CACHE=/opt/vcpkg-cache
export CMAKE_NAMED_ENV_TYPE=AWS_MACHINE_X64
export POSTGRESQL_VERSION=$PG_VERSION
export BIOSEQ_LIBCXX_MODULES_JSON=${MODULES_JSON}
export PATH=/opt/cmake/bin:/usr/local/bin:/usr/lib/postgresql/$PG_VERSION/bin:/usr/lib/llvm-$LLVM_VERSION/bin:\$PATH
EOF
chmod 0644 /etc/profile.d/bioseq.sh

# Build user owns the tree; /opt/bioseq is world-writable for status/logs.
chown -R "$TARGET_USER:$TARGET_USER" "$REPO_DEST" 2>/dev/null || true
chmod -R a+rwX /opt/bioseq 2>/dev/null || true

echo "[bootstrap] toolchain installed"
status "TOOLCHAIN_READY"

# --- 9. Optional verification ------------------------------------------------
if [ "$RUN_VERIFY" = "1" ]; then
  VERIFY="$REPO_DEST/main/infra/testing_infra/scripts/verify.sh"
  if [ -f "$VERIFY" ]; then
    echo "[bootstrap] running verify.sh as $TARGET_USER"
    chmod +x "$VERIFY" 2>/dev/null || true
    sudo -u "$TARGET_USER" -H bash -lc "'$VERIFY'"
    echo "[bootstrap] verify.sh finished; status=$(cat /opt/bioseq/PROVISION_STATUS 2>/dev/null)"
  else
    echo "[bootstrap] verify.sh missing at $VERIFY"; status "VERIFY_SCRIPT_MISSING"
  fi
else
  echo "[bootstrap] RUN_VERIFY=0; skipping verify"
fi

echo "[bootstrap] $(date -u) done"
