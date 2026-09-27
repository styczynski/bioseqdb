#!/usr/bin/env bash
# Runtime-only replica bootstrap: no repository build, no algorithm changes.
set -Eeuo pipefail
exec > >(tee -a /var/log/bioseq-e2e-replica-bootstrap.log) 2>&1
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y --no-install-recommends ca-certificates curl gnupg rsync zstd \
  python3 python3-yaml python3-jsonschema python3-psutil python3-requests \
  libicu74 libssl3t64 libxml2 libxslt1.1 libreadline8t64 liblz4-1 libzstd1 \
  libnuma1 libtinfo6 libedit2 libffi8 libvulkan1 libatomic1 binutils git
install -d -m 0755 /usr/share/postgresql-common/pgdg
curl -fsSL https://www.postgresql.org/media/keys/ACCC4CF8.asc \
  -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc
printf '%s\n' 'deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] https://apt.postgresql.org/pub/repos/apt noble-pgdg main' \
  > /etc/apt/sources.list.d/pgdg.list
apt-get update -q
apt-get install -y --no-install-recommends postgresql-17=17.11-1.pgdg24.04+2 \
  postgresql-client-17=17.11-1.pgdg24.04+2 libpq5=18.6-1.pgdg24.04+2
# Match original fleet: 96 GiB disk swap at priority 10; RAM-sized zstd zram
# at priority 100. Only these new empty replicas use this provisioning script.
if [ ! -f /swapfile ]; then
  fallocate -l 96G /swapfile
  chmod 600 /swapfile
  mkswap /swapfile
fi
grep -q '^/swapfile ' /etc/fstab || printf '/swapfile none swap sw,pri=10 0 0\n' >> /etc/fstab
swapon --show=NAME --noheadings | grep -qx /swapfile || swapon -p 10 /swapfile
printf 'vm.swappiness=80\nvm.page-cluster=0\n' > /etc/sysctl.d/90-bioseq-replica.conf
sysctl --system >/dev/null
install -d -m 0755 -o ubuntu -g ubuntu /home/ubuntu/bioseqdb
# zram service and settings are copied byte-for-byte from the original host
# after bootstrap, without its account-specific credentials/configuration.
touch /var/lib/bioseq-e2e-replica-bootstrap.ready
