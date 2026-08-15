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
LLVM_MIN_VERSION=22.1.6
LLVM_RELEASE_CANDIDATES=(22.1.8 22.1.7 22.1.6)
LLVM_ROOT="${LLVM_ROOT:-/usr/lib/llvm-$LLVM_VERSION}"
CMAKE_VERSION=4.2.1
NINJA_VERSION=1.12.1
PG_VERSION=17
SWAP_SIZE_GB="${SWAP_SIZE_GB:-96}"
export DEBIAN_FRONTEND=noninteractive

mkdir -p /opt/bioseq
status() { echo "$1" > /opt/bioseq/PROVISION_STATUS; }
fail()   { echo "[bootstrap] FATAL: $*" >&2; status "BOOTSTRAP_FAILED:$*"; exit 1; }
status "BOOTSTRAP_RUNNING"

# --- 1. Host survivability: keep SSH responsive and add swap backstop --------
echo "[bootstrap] SSH priority + ${SWAP_SIZE_GB}GiB swap backstop"
mkdir -p /etc/systemd/system/ssh.service.d
cat > /etc/systemd/system/ssh.service.d/10-bioseq-priority.conf <<'EOF'
[Service]
# Heavy clang++ module compiles can saturate CPU and memory. Keep sshd
# responsive enough for diagnostics instead of letting build jobs starve it.
Nice=-10
IOSchedulingClass=best-effort
IOSchedulingPriority=0
OOMScoreAdjust=-900
EOF
systemctl daemon-reload || true

# Apply priority to already-running sshd processes without restarting ssh.service
# (restarting during an interactive bootstrap rerun could sever that session).
if command -v pgrep >/dev/null 2>&1; then
  for pid in $(pgrep -x sshd 2>/dev/null || true); do
    renice -n -10 -p "$pid" >/dev/null 2>&1 || true
    ionice -c 2 -n 0 -p "$pid" >/dev/null 2>&1 || true
    echo "[bootstrap] prioritized sshd pid=$pid"
  done
fi

if [ ! -f /swapfile ]; then
  echo "[bootstrap] creating /swapfile (${SWAP_SIZE_GB}GiB)"
  fallocate -l "${SWAP_SIZE_GB}G" /swapfile 2>/dev/null \
    || dd if=/dev/zero of=/swapfile bs=1M count=$((SWAP_SIZE_GB * 1024)) status=progress \
    || fail "create swapfile"
  chmod 600 /swapfile
  mkswap /swapfile || fail "mkswap /swapfile"
fi
chmod 600 /swapfile
if ! grep -q '^/swapfile ' /proc/swaps 2>/dev/null; then
  swapon /swapfile || { mkswap /swapfile && swapon /swapfile; } || fail "enable /swapfile"
fi
grep -qE '^/swapfile[[:space:]]+none[[:space:]]+swap[[:space:]]' /etc/fstab \
  || echo '/swapfile none swap sw 0 0' >> /etc/fstab
cat > /etc/sysctl.d/90-bioseq-swap.conf <<'EOF'
vm.swappiness=10
EOF
sysctl --system >/dev/null 2>&1 || true
swapon --show || true

# --- 2. System build dependencies (incl. vcpkg Vulkan WSI prerequisites) -----
echo "[bootstrap] apt: base + build + graphics deps"
for i in 1 2 3 4 5; do apt-get update -y && break || sleep 15; done
apt-get install -y --no-install-recommends \
  build-essential g++ gcc make pkg-config ca-certificates curl wget git unzip zip tar \
  gnupg lsb-release software-properties-common \
  python3 python3-pip python3-venv python3-jinja2 python3-setuptools \
  bison flex autoconf automake autoconf-archive libtool libltdl-dev m4 gettext \
  rsync ninja-build \
  zlib1g-dev libssl-dev \
  minimap2 \
  doxygen \
  xorg-dev libx11-dev libxext-dev libxrandr-dev libxrender-dev libxi-dev \
  libxfixes-dev libxcursor-dev libxinerama-dev \
  libxcb1-dev libxcb-util-dev libxcb-keysyms1-dev libxcb-icccm4-dev libxcb-image0-dev \
  libxcb-randr0-dev libxcb-xinerama0-dev libxcb-xkb-dev libxcb-shape0-dev libxcb-sync-dev \
  libxcb-render-util0-dev libxcb-cursor-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev \
  mesa-vulkan-drivers vulkan-tools \
  || fail "apt base deps"

# --- 3. clang / libc++ 22 (apt.llvm.org, with prebuilt-tarball fallback) -----
echo "[bootstrap] LLVM/clang $LLVM_VERSION (min $LLVM_MIN_VERSION)"
clang_version() {
  "$1" --version 2>/dev/null \
    | sed -nE 's/.*version ([0-9]+[.][0-9]+[.][0-9]+).*/\1/p' \
    | head -1
}

llvm_root_meets_min() {
  local root="$1"
  local clang="$root/bin/clang++"
  local clang_tidy="$root/bin/clang-tidy"
  local clang_have clang_tidy_have
  [ -x "$clang" ] && [ -x "$clang_tidy" ] || return 1
  clang_have="$(clang_version "$clang")"
  clang_tidy_have="$(clang_version "$clang_tidy")"
  [ -n "$clang_have" ] && [ -n "$clang_tidy_have" ] || return 1
  [ "${clang_have%%.*}" = "${clang_tidy_have%%.*}" ] || return 1
  dpkg --compare-versions "$clang_have" ge "$LLVM_MIN_VERSION"
}

install_llvm() {
  if llvm_root_meets_min "/usr/lib/llvm-$LLVM_VERSION"; then
    LLVM_ROOT="/usr/lib/llvm-$LLVM_VERSION"
    return 0
  fi
  # Preferred: apt.llvm.org (installs into the canonical /usr/lib/llvm-N path).
  if curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh; then
    chmod +x /tmp/llvm.sh
    /tmp/llvm.sh "$LLVM_VERSION" all || true
    apt-get install -y \
      "clang-$LLVM_VERSION" "lld-$LLVM_VERSION" "llvm-$LLVM_VERSION-dev" \
      "clang-tidy-$LLVM_VERSION" "clang-tools-$LLVM_VERSION" \
      "libc++-$LLVM_VERSION-dev" "libc++abi-$LLVM_VERSION-dev" "libunwind-$LLVM_VERSION-dev" || true
    if llvm_root_meets_min "/usr/lib/llvm-$LLVM_VERSION"; then
      LLVM_ROOT="/usr/lib/llvm-$LLVM_VERSION"
      return 0
    fi
  fi
  echo "[bootstrap] apt.llvm.org unavailable or below $LLVM_MIN_VERSION; falling back to official LLVM release tarball"
  for v in "${LLVM_RELEASE_CANDIDATES[@]}"; do
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
      if llvm_root_meets_min "/opt/llvm-$LLVM_VERSION"; then
        LLVM_ROOT="/opt/llvm-$LLVM_VERSION"
        return 0
      fi
    done
  done
  return 1
}
install_llvm || fail "llvm install (apt + tarball both failed)"
CLANG_ACTUAL_VERSION="$(clang_version "$LLVM_ROOT/bin/clang++")"
CLANG_TIDY_ACTUAL_VERSION="$(clang_version "$LLVM_ROOT/bin/clang-tidy")"
[ -n "$CLANG_ACTUAL_VERSION" ] || fail "clang version not parseable at $LLVM_ROOT/bin/clang++"
[ -n "$CLANG_TIDY_ACTUAL_VERSION" ] \
  || fail "clang-tidy missing or version not parseable at $LLVM_ROOT/bin/clang-tidy"
dpkg --compare-versions "$CLANG_ACTUAL_VERSION" ge "$LLVM_MIN_VERSION" \
  || fail "clang $CLANG_ACTUAL_VERSION is below required $LLVM_MIN_VERSION"
[ "${CLANG_ACTUAL_VERSION%%.*}" = "${CLANG_TIDY_ACTUAL_VERSION%%.*}" ] \
  || fail "clang-tidy major $CLANG_TIDY_ACTUAL_VERSION does not match clang $CLANG_ACTUAL_VERSION"
"$LLVM_ROOT/bin/clang++" --version | head -1 || fail "clang++ not runnable"
"$LLVM_ROOT/bin/clang-tidy" --version | head -1 || fail "clang-tidy not runnable"

# --- 4. CMake (pinned: its import-std UUID is hardcoded in CMakeLists.txt) ----
echo "[bootstrap] CMake $CMAKE_VERSION"
if ! /opt/cmake/bin/cmake --version 2>/dev/null | grep -q "$CMAKE_VERSION"; then
  url="https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz"
  curl -fL "$url" -o /tmp/cmake.tgz || fail "download cmake"
  rm -rf /opt/cmake; mkdir -p /opt/cmake
  tar --strip-components=1 -xzf /tmp/cmake.tgz -C /opt/cmake || fail "extract cmake"
fi
for t in cmake ctest cpack; do ln -sf "/opt/cmake/bin/$t" "/usr/local/bin/$t"; done
/opt/cmake/bin/cmake --version | head -1

# --- 5. Ninja (recent build for robust C++ module dyndep) --------------------
echo "[bootstrap] Ninja $NINJA_VERSION"
if ! /usr/local/bin/ninja --version 2>/dev/null | grep -q "$NINJA_VERSION"; then
  curl -fL "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VERSION/ninja-linux.zip" -o /tmp/ninja.zip || fail "download ninja"
  unzip -o /tmp/ninja.zip -d /usr/local/bin || fail "unzip ninja"
  chmod +x /usr/local/bin/ninja
fi
/usr/local/bin/ninja --version

# --- 6. PostgreSQL 17 (pg_config + server headers + libpq) -------------------
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

echo "[bootstrap] PostgreSQL test auth/database"
systemctl start postgresql 2>/dev/null || service postgresql start || fail "start postgres"
sudo -u postgres "/usr/lib/postgresql/$PG_VERSION/bin/psql" \
  -v ON_ERROR_STOP=1 --dbname postgres \
  --command "ALTER ROLE postgres WITH LOGIN PASSWORD 'postgres';" \
  || fail "set postgres test password"
if ! sudo -u postgres "/usr/lib/postgresql/$PG_VERSION/bin/psql" \
    -Atq --dbname postgres \
    --command "SELECT 1 FROM pg_database WHERE datname = 'bioseq';" \
    | grep -qx 1; then
  sudo -u postgres "/usr/lib/postgresql/$PG_VERSION/bin/createdb" \
    --owner=postgres bioseq || fail "create bioseq test database"
fi

# --- 7. vcpkg ----------------------------------------------------------------
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

# --- 7b. Website validation toolchain (§4.4/§11.1): Doxygen 1.9.8 is installed
# above via apt (Ubuntu Noble). The generated-site validators (html5lib) and the
# pinned Playwright browser acceptance run from a hash-locked virtualenv the build
# creates from website/tests/requirements.lock; pre-cache its wheels and the
# Chromium browser here so an offline `bioseq_website` build never reaches the
# network. Non-fatal: the build re-resolves against the lock if the cache is cold.
echo "[bootstrap] website validation toolchain (Doxygen $(doxygen --version 2>/dev/null || echo '?'), Playwright, html5lib)"
WEBSITE_LOCK="$REPO_DEST/website/tests/requirements.lock"
PLAYWRIGHT_BROWSERS_PATH=/opt/playwright-browsers
mkdir -p /opt/pip-cache "$PLAYWRIGHT_BROWSERS_PATH" && chmod 0777 /opt/pip-cache "$PLAYWRIGHT_BROWSERS_PATH"
if [ -f "$WEBSITE_LOCK" ]; then
  python3 -m pip download --require-hashes --dest /opt/pip-cache \
    -r "$WEBSITE_LOCK" 2>/dev/null \
    || echo "[bootstrap] WARNING: website wheel pre-cache incomplete; build will re-resolve"
  PW_VENV=/opt/website-playwright-venv
  python3 -m venv "$PW_VENV" 2>/dev/null \
    && "$PW_VENV/bin/pip" install --no-index --find-links /opt/pip-cache \
         --require-hashes -r "$WEBSITE_LOCK" 2>/dev/null \
    && PLAYWRIGHT_BROWSERS_PATH="$PLAYWRIGHT_BROWSERS_PATH" \
         "$PW_VENV/bin/playwright" install chromium 2>/dev/null \
    || echo "[bootstrap] WARNING: Playwright chromium pre-install skipped; browser tests SKIP until provisioned"
  chmod -R a+rwX "$PLAYWRIGHT_BROWSERS_PATH" /opt/pip-cache 2>/dev/null || true
else
  echo "[bootstrap] WARNING: $WEBSITE_LOCK missing; website toolchain not pre-cached"
fi

# --- 8. Discover the libc++ modules.json (multiarch sub-path varies) ---------
MODULES_JSON="$(find "$LLVM_ROOT/" -name 'libc++.modules.json' 2>/dev/null | head -1)"
echo "[bootstrap] libc++ modules.json = ${MODULES_JSON:-<NOT FOUND>}"
[ -n "$MODULES_JSON" ] || echo "[bootstrap] WARNING: libc++.modules.json not found; import std may fail to configure"

# --- 9. Persist environment for SSH sessions + verify.sh --------------------
cat > /etc/profile.d/bioseq.sh <<EOF
export VCPKG_ROOT=$VCPKG_ROOT
export VCPKG_DEFAULT_BINARY_CACHE=/opt/vcpkg-cache
export CMAKE_NAMED_ENV_TYPE=AWS_MACHINE_X64
export BIOSEQ_LLVM_ROOT=$LLVM_ROOT
export POSTGRESQL_VERSION=$PG_VERSION
export BIOSEQ_LIBCXX_MODULES_JSON=${MODULES_JSON}
export PATH=/opt/cmake/bin:/usr/local/bin:/usr/lib/postgresql/$PG_VERSION/bin:$LLVM_ROOT/bin:\$PATH
EOF
chmod 0644 /etc/profile.d/bioseq.sh

# Build user owns the tree; /opt/bioseq is world-writable for status/logs.
chown -R "$TARGET_USER:$TARGET_USER" "$REPO_DEST" 2>/dev/null || true
chmod -R a+rwX /opt/bioseq 2>/dev/null || true

echo "[bootstrap] toolchain installed"
status "TOOLCHAIN_READY"

# --- 10. Optional verification -----------------------------------------------
if [ "$RUN_VERIFY" = "1" ]; then
  VERIFY="$REPO_DEST/infra/testing_infra/scripts/verify.sh"
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
