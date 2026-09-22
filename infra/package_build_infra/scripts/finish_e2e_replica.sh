#!/usr/bin/env bash
# Runs only on fresh replica after exact original runtime/configuration copy.
set -Eeuo pipefail
printf 'BIOSEQ_ZRAM_FRACTION_PERCENT=100\nBIOSEQ_ZRAM_SWAP_PRIORITY=100\n' \
  > /etc/bioseq-package-builder.env
chown -R ubuntu:ubuntu /home/ubuntu/bioseqdb
chown -R postgres:postgres /etc/postgresql/17/main
bash /home/ubuntu/bioseqdb/infra/package_build_infra/scripts/register_frozen_runtime_libraries.sh
systemctl daemon-reload
systemctl enable --now bioseq-zram-swap.service
systemctl restart postgresql@17-main
swapon --show
zramctl
df -h /
