#!/usr/bin/env bash
# Bootstraps cargo on distros where the OS-shipped rust is too old.
# Runs inside the build container; safe to re-run.

set -xEeuo pipefail

if cargo --version; then
    exit 0
fi

RUST_VERSION=1.95.0
ARCH=$(uname -m)

curl -O https://static.rust-lang.org/dist/rust-${RUST_VERSION}-${ARCH}-unknown-linux-gnu.tar.gz
tar -xzf rust-*.tar.gz
cd rust-*/
./install.sh --prefix=/usr
cd ..
rm -rf rust-*
