#!/usr/bin/env bash
set -euo pipefail

# version: explicit workflow_dispatch input wins; else from Cargo.toml
PKGVER="${INPUT_VERSION:-}"
[ -n "$PKGVER" ] || PKGVER="$(grep -m1 '^version = ' Cargo.toml | sed -E 's/version = "(.+)"/\1/')"
echo "Publishing ${PKGNAME} ${PKGVER}"

BUILD_DIR="$(mktemp -d)"
sed "s/__PKGVER__/$PKGVER/" .github/aur/PKGBUILD.tmpl > "$BUILD_DIR/PKGBUILD"
chown -R builder:builder "$BUILD_DIR"

# fill sha256sums (downloads the GitHub tarball) + derive .SRCINFO; makepkg refuses root
sudo -u builder -H bash -c "cd '$BUILD_DIR' && updpkgsums && makepkg --printsrcinfo > .SRCINFO"

echo '--- PKGBUILD ---'; cat "$BUILD_DIR/PKGBUILD"
echo '--- .SRCINFO ---'; cat "$BUILD_DIR/.SRCINFO"

KEYFILE="$(mktemp)"
printf '%s\n' "$AUR_SSH_PRIVATE_KEY" | tr -d '\r' > "$KEYFILE"
chmod 600 "$KEYFILE"
export GIT_SSH_COMMAND="ssh -i $KEYFILE -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=accept-new"

AUR_DIR="$(mktemp -d)"
git clone "ssh://aur@aur.archlinux.org/${PKGNAME}.git" "$AUR_DIR"
cp "$BUILD_DIR/PKGBUILD" "$BUILD_DIR/.SRCINFO" "$AUR_DIR/"
cd "$AUR_DIR"
git config user.name  "Oleg Antonyan"
git config user.email "oleg.b.antonyan@gmail.com"
git add PKGBUILD .SRCINFO
if git diff --cached --quiet; then
  echo "No changes to push."
  exit 0
fi
git commit -m "Update to ${PKGVER}-1"
git push
