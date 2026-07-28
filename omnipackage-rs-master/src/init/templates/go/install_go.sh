#!/usr/bin/env bash
# Bootstraps Go on distros where the OS-shipped go is too old or missing.
# Runs inside the build container; safe to re-run.

set -xEeuo pipefail

if go version; then
    exit 0
fi

curl -L https://go.dev/dl/go1.26.2.linux-amd64.tar.gz | tar -zx -C /usr/local
ln -s /usr/local/go/bin/* /usr/bin/
