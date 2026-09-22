#!/usr/bin/env bash
# Register the copied Clang runtime for PostgreSQL services, whose environment
# does not inherit the benchmark launcher's LD_LIBRARY_PATH.
set -Eeuo pipefail
find /opt/bioseq-package-toolchain/llvm-22/lib -type f \
  \( -name '*.so' -o -name '*.so.*' \) -printf '%h\n' | sort -u \
  > /etc/ld.so.conf.d/bioseq-frozen-runtime.conf
ldconfig
