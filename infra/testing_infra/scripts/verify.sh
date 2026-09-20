#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# verify.sh — prove the box is correctly provisioned.
#
# Success requires both configuration and the selected target build to succeed.
# Source-path classification is diagnostic only: project compiler errors must
# never be reported as successful provisioning.
#
# Steps:
#   1) cmake --preset aws-x64-linux   (configure => vcpkg builds every dep)
#   2) cmake --build ... bioseq_modules_nopg -k 0  (compile our Domain-N modules)
#   3) classify the errors by file path and record a verdict in
#      /opt/bioseq/PROVISION_STATUS.
#
# Re-runnable as the build user:  bash -lc ~/bioseqdb/infra/testing_infra/scripts/verify.sh
# ---------------------------------------------------------------------------
set -uo pipefail
[ -f /etc/profile.d/bioseq.sh ] && . /etc/profile.d/bioseq.sh
[ -f /opt/bioseq/provision.env ] && . /opt/bioseq/provision.env || true

REPO_DEST="${REPO_DEST:-$HOME/bioseqdb}"
REPOSITORY_ROOT="$REPO_DEST"
PRESET="aws-x64-linux"
BUILD_DIR="$REPOSITORY_ROOT/build/$PRESET"
TARGET="bioseq_modules_nopg"
CFG_LOG=/opt/bioseq/verify_configure.log
BLD_LOG=/opt/bioseq/verify_build.log

status() { echo "$1" > /opt/bioseq/PROVISION_STATUS 2>/dev/null || sudo bash -c "echo '$1' > /opt/bioseq/PROVISION_STATUS"; }

cd "$REPOSITORY_ROOT" 2>/dev/null || { echo "[verify] repository root not found at $REPOSITORY_ROOT"; status "VERIFY_NO_REPO"; exit 1; }
status "VERIFY_RUNNING"
echo "[verify] cmake   : $(command -v cmake) -> $(cmake --version | head -1)"
echo "[verify] ninja   : $(command -v ninja) -> $(ninja --version 2>/dev/null)"
echo "[verify] vcpkg   : ${VCPKG_ROOT:-<unset>}"
echo "[verify] env type: ${CMAKE_NAMED_ENV_TYPE:-<unset>}"
echo "[verify] llvm    : ${BIOSEQ_LLVM_ROOT:-<unset>}"
echo "[verify] clang   : $(command -v clang++) -> $(clang++ --version | head -1)"
echo "[verify] modules : ${BIOSEQ_LIBCXX_MODULES_JSON:-<unset>}"

# ---- 1. Configure (this is the long step: vcpkg builds all dependencies) -----
echo "[verify] configuring with preset '$PRESET' ..."
if ! cmake --preset "$PRESET" 2>&1 | tee "$CFG_LOG"; then
  echo "[verify] CONFIGURE FAILED — third-party/toolchain problem (NOT our code)."
  echo "[verify] tail:"; tail -n 40 "$CFG_LOG"
  status "VERIFY_CONFIGURE_FAILED"
  exit 1
fi
echo "[verify] configure OK (all dependencies resolved)"

# ---- 2. Build a core target ------------------------------------------------
echo "[verify] building target '$TARGET' (keep-going) ..."
timeout 5400 cmake --build "$BUILD_DIR" --target "$TARGET" -j "$(nproc)" -- -k 0 2>&1 | tee "$BLD_LOG"
build_rc=${PIPESTATUS[0]}

# ---- 3. Classify -----------------------------------------------------------
errlines="$(grep -E ' (error|fatal error): ' "$BLD_LOG" 2>/dev/null || true)"
thirdparty="$(printf '%s\n' "$errlines" | grep -Ei '(vcpkg_installed|/opt/vcpkg|/vendor/|/usr/lib/llvm|/usr/include/|/opt/llvm)' | grep -Fv "$REPOSITORY_ROOT/bioseq/" || true)"
ourcode="$(printf '%s\n' "$errlines" | grep -F "$REPOSITORY_ROOT/bioseq/" || true)"
n_third=$(printf '%s' "$thirdparty" | grep -c . || true)
n_our=$(printf '%s' "$ourcode" | grep -c . || true)

echo "============================================================"
echo "[verify] build exit          : $build_rc"
echo "[verify] our-code error lines : $n_our"
echo "[verify] 3rd-party error lines: $n_third"
echo "============================================================"

if [ "$build_rc" -eq 0 ]; then
  echo "[verify] selected core target built successfully; this is not E2E qualification."
  status "VERIFY_BUILD_SUCCEEDED"; exit 0
fi
if [ "$n_third" -gt 0 ]; then
  echo "[verify] FAIL: third-party/toolchain compile errors present:"; printf '%s\n' "$thirdparty" | head -20
  status "VERIFY_3RD_PARTY_FAILURE"; exit 1
fi
if [ "$n_our" -gt 0 ]; then
  echo "[verify] FAIL: project compilation failed. Source location does not make this a provisioning pass. Sample:"
  printf '%s\n' "$ourcode" | head -20
  status "VERIFY_PROJECT_BUILD_FAILED"; exit 1
fi
echo "[verify] build failed but no 'error:' lines were classified — inspect $BLD_LOG"
tail -n 40 "$BLD_LOG"
status "VERIFY_UNCLASSIFIED_FAILURE"; exit 1
