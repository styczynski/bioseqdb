#!/usr/bin/env bash
# Toolchain-only user data for the external benchmark fleet. No repository build.
set -euo pipefail
exec > >(tee -a /var/log/bioseq-provision.log) 2>&1
export DEBIAN_FRONTEND=noninteractive
mkdir -p /opt/bioseq /home/ubuntu/bioseqdb
apt-get update -y
apt-get install -y --no-install-recommends clang lld llvm libomp-dev cmake ninja-build make \
  git rsync curl ca-certificates python3 python3-venv python3-yaml python3-pip \
  zlib1g-dev libbz2-dev liblzma-dev libzstd-dev libsqlite3-dev libssl-dev libcurl4-openssl-dev \
  libpcre2-dev libgomp1 libncurses-dev liblmdb-dev time pkg-config unzip
chown -R ubuntu:ubuntu /home/ubuntu/bioseqdb
printf 'ubuntu ALL=(ALL) NOPASSWD: ALL\n' > /etc/sudoers.d/90-external-benchmark
chmod 0440 /etc/sudoers.d/90-external-benchmark
printf 'TOOLCHAIN_READY\n' > /opt/bioseq/PROVISION_STATUS
