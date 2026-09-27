#!/usr/bin/env bash
# Runs only on fresh replica after exact original runtime/configuration copy.
set -Eeuo pipefail
printf 'BIOSEQ_ZRAM_FRACTION_PERCENT=100\nBIOSEQ_ZRAM_SWAP_PRIORITY=100\n' \
  > /etc/bioseq-package-builder.env
chown -R ubuntu:ubuntu /home/ubuntu/bioseqdb
chown -R postgres:postgres /etc/postgresql/17/main
bash /home/ubuntu/bioseqdb/infra/package_build_infra/scripts/register_frozen_runtime_libraries.sh
install -d /etc/systemd/system/postgresql@17-main.service.d
install -m 0644 /home/ubuntu/bioseqdb/infra/package_build_infra/scripts/postgresql_e2e_priority.conf \
  /etc/systemd/system/postgresql@17-main.service.d/bioseq-e2e-priority.conf
systemctl daemon-reload
systemctl enable --now bioseq-zram-swap.service
systemctl restart postgresql@17-main
swapon --show
zramctl
df -h /
