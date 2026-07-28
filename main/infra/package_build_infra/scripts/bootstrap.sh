#!/usr/bin/env bash
# Architecture-neutral Ubuntu package-builder bootstrap. This script is
# installed by EC2 user-data and is safe to re-run as root after an AMI restore.
set -Eeuo pipefail
umask 022

STATE_DIR=/opt/bioseq-package-builder
LOG_FILE=/var/log/bioseq-package-builder-bootstrap.log
ENV_FILE=/etc/bioseq-package-builder.env
mkdir -p "$STATE_DIR"
if [[ ${BIOSEQ_BOOTSTRAP_LOGGED:-0} != 1 ]]; then
  exec > >(tee -a "$LOG_FILE") 2>&1
fi

status() {
  printf '%s\n' "$1" > "$STATE_DIR/PROVISION_STATUS"
}

on_error() {
  local rc=$?
  local line=${BASH_LINENO[0]:-unknown}
  trap - ERR
  status "BOOTSTRAP_FAILED:${line}:${rc}"
  printf '[bootstrap] failed at line %s with exit %s\n' "$line" "$rc" >&2
  exit "$rc"
}
trap on_error ERR

[[ $EUID -eq 0 ]] || { printf '[bootstrap] must run as root\n' >&2; exit 1; }
[[ -r $ENV_FILE ]] || { printf '[bootstrap] missing %s\n' "$ENV_FILE" >&2; exit 1; }
set -a
# shellcheck disable=SC1090,SC1091
source "$ENV_FILE"
set +a

status BOOTSTRAP_RUNNING
rm -f "$STATE_DIR/READY"
printf '[bootstrap] starting %s architecture=%s aws_architecture=%s schema=%s\n' \
  "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$BIOSEQ_BUILD_ARCHITECTURE" \
  "$AWS_ARCHITECTURE" "$BIOSEQ_AMI_SCHEMA_VERSION"

case "$(uname -m):$AWS_ARCHITECTURE" in
  aarch64:arm64) RUST_TARGET=aarch64-unknown-linux-gnu ;;
  x86_64:x86_64) RUST_TARGET=x86_64-unknown-linux-gnu ;;
  *)
    printf '[bootstrap] architecture mismatch: kernel=%s expected=%s\n' \
      "$(uname -m)" "$AWS_ARCHITECTURE" >&2
    exit 1
    ;;
esac

getent passwd "$TARGET_USER" >/dev/null || {
  printf '[bootstrap] target user %s does not exist\n' "$TARGET_USER" >&2
  exit 1
}
TARGET_GROUP=$(id -gn "$TARGET_USER")
install -d -m 0755 -o "$TARGET_USER" -g "$TARGET_GROUP" \
  "$BIOSEQ_SOURCE_STAGING_DIR"

retry() {
  local attempt
  for attempt in 1 2 3 4 5; do
    "$@" && return 0
    printf '[bootstrap] attempt %s failed: %s\n' "$attempt" "$*" >&2
    sleep $((attempt * 5))
  done
  return 1
}

export DEBIAN_FRONTEND=noninteractive
retry apt-get update -y
retry apt-get install -y --no-install-recommends \
  binutils build-essential ca-certificates clang cmake cpio curl docker.io file \
  git gnupg jq lsb-release ninja-build pkg-config python3 rpm rpm2cpio rsync \
  sudo unzip util-linux xz-utils zip zstd

systemctl enable --now docker
usermod -aG docker "$TARGET_USER"

# AWS CLI v2 has separate official archives for the two native architectures.
if ! command -v aws >/dev/null 2>&1; then
  AWSCLI_TMP=$(mktemp -d /tmp/bioseq-awscli.XXXXXX)
  curl -fsSL "https://awscli.amazonaws.com/awscli-exe-linux-${RUST_TARGET%%-*}.zip" \
    -o "$AWSCLI_TMP/awscliv2.zip"
  unzip -q "$AWSCLI_TMP/awscliv2.zip" -d "$AWSCLI_TMP"
  "$AWSCLI_TMP/aws/install"
  rm -rf "$AWSCLI_TMP"
fi
aws --version

# Install a current stable Rust toolchain for the unprivileged build identity.
if ! sudo -u "$TARGET_USER" -H bash -lc 'command -v rustc >/dev/null 2>&1'; then
  RUSTUP_TMP=$(mktemp -d /tmp/bioseq-rustup.XXXXXX)
  # mktemp creates a root-only directory.  The installer itself runs as the
  # unprivileged build identity, so make the directory traversable without
  # transferring ownership of the root-downloaded executable.
  chmod 0755 "$RUSTUP_TMP"
  curl -fsSL "https://static.rust-lang.org/rustup/dist/${RUST_TARGET}/rustup-init" \
    -o "$RUSTUP_TMP/rustup-init"
  chmod 0755 "$RUSTUP_TMP/rustup-init"
  sudo -u "$TARGET_USER" -H "$RUSTUP_TMP/rustup-init" \
    -y --profile minimal --default-toolchain stable
  rm -rf "$RUSTUP_TMP"
fi

# High-priority compressed swap absorbs bursts before the deliberately lower
# priority EBS swapfile. zram is recreated at every boot by this oneshot unit.
cat > /usr/local/sbin/bioseq-zram-swap <<'ZRAM'
#!/usr/bin/env bash
set -Eeuo pipefail
# shellcheck disable=SC1091
source /etc/bioseq-package-builder.env
device=/dev/zram0

case "${1:-start}" in
  start)
    active_priority=$(awk -v device="$device" '$1 == device {print $5}' /proc/swaps)
    if [[ ${active_priority:-} == "$BIOSEQ_ZRAM_SWAP_PRIORITY" ]]; then
      exit 0
    elif [[ -n ${active_priority:-} ]]; then
      swapoff "$device"
    fi
    modprobe zram num_devices=1
    # Current AWS kernels can expose zram's hot-add interface without honoring
    # the legacy num_devices module option. Allocate the first device explicitly
    # when modprobe did not create it.
    if [[ ! -b $device ]]; then
      allocated_device=$(zramctl --find)
      [[ $allocated_device == "$device" ]] || {
        printf 'expected %s but zram allocated %s\n' "$device" "$allocated_device" >&2
        exit 1
      }
    fi
    zramctl --reset "$device" 2>/dev/null || true
    memory_kib=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)
    zram_bytes=$((memory_kib * 1024 * BIOSEQ_ZRAM_FRACTION_PERCENT / 100))
    algorithm=lz4
    grep -qw zstd /sys/block/zram0/comp_algorithm && algorithm=zstd
    zramctl --algorithm "$algorithm" --size "$zram_bytes" "$device"
    mkswap --label bioseq-zram "$device" >/dev/null
    swapon --priority "$BIOSEQ_ZRAM_SWAP_PRIORITY" "$device"
    ;;
  stop)
    swapoff "$device" 2>/dev/null || true
    zramctl --reset "$device" 2>/dev/null || true
    ;;
  *)
    printf 'usage: %s [start|stop]\n' "$0" >&2
    exit 2
    ;;
esac
ZRAM
chown root:root /usr/local/sbin/bioseq-zram-swap
chmod 0755 /usr/local/sbin/bioseq-zram-swap

cat > /etc/systemd/system/bioseq-zram-swap.service <<'EOF'
[Unit]
Description=Bioseq high-priority compressed zram swap
DefaultDependencies=no
After=systemd-modules-load.service systemd-remount-fs.service
Before=swap.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/bioseq-zram-swap start
ExecStop=/usr/local/sbin/bioseq-zram-swap stop

[Install]
WantedBy=swap.target
EOF

# Recreate a wrongly sized stale swapfile only after taking it offline.
DISK_SWAP_BYTES=$((BIOSEQ_DISK_SWAP_SIZE_GB * 1024 * 1024 * 1024))
CURRENT_SWAP_BYTES=0
[[ -f /swapfile ]] && CURRENT_SWAP_BYTES=$(stat -c %s /swapfile)
if [[ $CURRENT_SWAP_BYTES -ne $DISK_SWAP_BYTES ]]; then
  swapoff /swapfile 2>/dev/null || true
  rm -f /swapfile
  fallocate -l "${BIOSEQ_DISK_SWAP_SIZE_GB}G" /swapfile || \
    dd if=/dev/zero of=/swapfile bs=1M \
      count=$((BIOSEQ_DISK_SWAP_SIZE_GB * 1024)) status=progress
  chmod 0600 /swapfile
  mkswap /swapfile >/dev/null
fi
chmod 0600 /swapfile
if ! blkid -p -s TYPE -o value /swapfile 2>/dev/null | grep -qx swap; then
  mkswap /swapfile >/dev/null
fi
sed -i '\|^/swapfile[[:space:]]|d' /etc/fstab
printf '/swapfile none swap sw,pri=%s 0 0\n' "$BIOSEQ_DISK_SWAP_PRIORITY" >> /etc/fstab
ACTIVE_DISK_PRIORITY=$(awk '$1 == "/swapfile" {print $5}' /proc/swaps)
if [[ -n ${ACTIVE_DISK_PRIORITY:-} && $ACTIVE_DISK_PRIORITY -ne $BIOSEQ_DISK_SWAP_PRIORITY ]]; then
  swapoff /swapfile
  ACTIVE_DISK_PRIORITY=""
fi
if [[ -z ${ACTIVE_DISK_PRIORITY:-} ]]; then
  swapon --priority "$BIOSEQ_DISK_SWAP_PRIORITY" /swapfile
fi

cat > /etc/sysctl.d/90-bioseq-package-builder.conf <<'EOF'
# Prefer compressed swap under pressure while retaining an EBS backstop.
vm.swappiness=80
vm.page-cluster=0
# Keep ten percent of every period available to ordinary scheduling even when
# all compiler workers are using the lowest real-time round-robin priority.
kernel.sched_rt_period_us=1000000
kernel.sched_rt_runtime_us=900000
EOF
sysctl --system >/dev/null

# sshd runs one RT priority above compilers, but reset-on-fork prevents login
# sessions and builds from inheriting sshd's scheduler class.
mkdir -p /etc/systemd/system/ssh.service.d
cat > /etc/systemd/system/ssh.service.d/20-bioseq-package-builder.conf <<EOF
[Service]
Nice=-15
IOSchedulingClass=best-effort
IOSchedulingPriority=0
OOMScoreAdjust=-1000
CPUSchedulingPolicy=rr
CPUSchedulingPriority=$BIOSEQ_SSH_RT_PRIORITY
CPUSchedulingResetOnFork=true
LimitRTPRIO=$BIOSEQ_SSH_RT_PRIORITY
EOF

# The public launcher has no privilege. sudo delegates only to the root-owned
# validator, which accepts root-owned compiler paths, sets RR/1, then drops back
# to the invoking uid/gid before the compiler executes.
install -d -m 0755 /usr/local/libexec
cat > /usr/local/libexec/bioseq-compiler-rt-exec <<'COMPILER_EXEC'
#!/usr/bin/env bash
set -Eeuo pipefail
# shellcheck disable=SC1091
source /etc/bioseq-package-builder.env

[[ $EUID -eq 0 ]] || { printf 'privileged launcher must run through sudo\n' >&2; exit 126; }
caller_uid=${SUDO_UID:-}
caller_gid=${SUDO_GID:-}
[[ $caller_uid =~ ^[0-9]+$ && $caller_gid =~ ^[0-9]+$ && $caller_uid -ne 0 ]] || {
  printf 'invalid unprivileged caller identity\n' >&2
  exit 126
}

if [[ ${1:-} == --preflight ]]; then
  compiler=/usr/bin/true
  shift
else
  [[ $# -ge 1 ]] || { printf 'compiler path is required\n' >&2; exit 2; }
  compiler=$(readlink -f -- "$1")
  shift
  compiler_base=$(basename -- "$compiler")
  case "$compiler:$compiler_base" in
    /usr/bin/*:gcc|/usr/bin/*:g++|/usr/bin/*:gcc-[0-9]*|/usr/bin/*:g++-[0-9]*|\
    /usr/bin/*:*-gcc|/usr/bin/*:*-g++|/usr/bin/*:*-gcc-[0-9]*|/usr/bin/*:*-g++-[0-9]*|\
    /usr/bin/*:clang|/usr/bin/*:clang++|/usr/bin/*:clang-[0-9]*|/usr/bin/*:clang++-[0-9]*|\
    /usr/lib/llvm-*/bin/*:clang|/usr/lib/llvm-*/bin/*:clang++|\
    /usr/lib/llvm-*/bin/*:clang-[0-9]*|/usr/lib/llvm-*/bin/*:clang++-[0-9]*) ;;
    *) printf 'refusing non-compiler executable: %s\n' "$compiler" >&2; exit 126 ;;
  esac
fi

[[ -f $compiler && -x $compiler ]] || { printf 'compiler is not executable\n' >&2; exit 126; }
[[ $(stat -c %u "$compiler") -eq 0 ]] || { printf 'compiler is not root-owned\n' >&2; exit 126; }
compiler_mode=$(stat -c %a "$compiler")
(( (8#$compiler_mode & 022) == 0 )) || {
  printf 'compiler is group- or world-writable\n' >&2
  exit 126
}

exec /usr/bin/ionice -c 2 -n 2 \
  /usr/bin/chrt --rr "$BIOSEQ_COMPILER_RT_PRIORITY" \
  /usr/bin/setpriv --reuid "$caller_uid" --regid "$caller_gid" --init-groups \
  "$compiler" "$@"
COMPILER_EXEC
chown root:root /usr/local/libexec/bioseq-compiler-rt-exec
chmod 0755 /usr/local/libexec/bioseq-compiler-rt-exec

cat > /usr/local/bin/bioseq-compiler-rt-launcher <<'COMPILER_LAUNCHER'
#!/usr/bin/env bash
set -Eeuo pipefail
exec /usr/bin/sudo -n /usr/local/libexec/bioseq-compiler-rt-exec "$@"
COMPILER_LAUNCHER
chown root:root /usr/local/bin/bioseq-compiler-rt-launcher
chmod 0755 /usr/local/bin/bioseq-compiler-rt-launcher

cat > /etc/sudoers.d/bioseq-compiler-rt <<EOF
$TARGET_USER ALL=(root) NOPASSWD: /usr/local/libexec/bioseq-compiler-rt-exec *
EOF
chmod 0440 /etc/sudoers.d/bioseq-compiler-rt
visudo -cf /etc/sudoers.d/bioseq-compiler-rt

cat > /etc/profile.d/bioseq-package-builder.sh <<'PROFILE'
# shellcheck disable=SC1091
source /etc/bioseq-package-builder.env
export AWS_DEFAULT_REGION="$AWS_REGION"
export BIOSEQ_COMPILER_RT_LAUNCHER=/usr/local/bin/bioseq-compiler-rt-launcher
export CMAKE_C_COMPILER_LAUNCHER="$BIOSEQ_COMPILER_RT_LAUNCHER"
export CMAKE_CXX_COMPILER_LAUNCHER="$BIOSEQ_COMPILER_RT_LAUNCHER"
export PATH="$HOME/.cargo/bin:$PATH"
PROFILE
chmod 0644 /etc/profile.d/bioseq-package-builder.sh

cat > /usr/local/sbin/bioseq-package-builder-verify <<'VERIFY'
#!/usr/bin/env bash
set -Eeuo pipefail
# shellcheck disable=SC1091
source /etc/bioseq-package-builder.env

fail() {
  printf '[verify] FAIL: %s\n' "$*" >&2
  exit 1
}

case "$(uname -m):$AWS_ARCHITECTURE" in
  aarch64:arm64|x86_64:x86_64) ;;
  *) fail "architecture mismatch: kernel=$(uname -m) expected=$AWS_ARCHITECTURE" ;;
esac

[[ $(stat -c %U "$BIOSEQ_SOURCE_STAGING_DIR") == "$TARGET_USER" ]] || \
  fail "$BIOSEQ_SOURCE_STAGING_DIR is not owned by $TARGET_USER"
[[ -w $BIOSEQ_SOURCE_STAGING_DIR ]] || \
  fail "$BIOSEQ_SOURCE_STAGING_DIR is not writable"

systemctl is-active --quiet docker || fail "Docker is not active"
systemctl is-active --quiet bioseq-zram-swap.service || fail "zram service is not active"
sudo -u "$TARGET_USER" -H /usr/local/bin/bioseq-compiler-rt-launcher --preflight
sudo -u "$TARGET_USER" -H bash -lc 'rustc --version && cargo --version'
sudo -u "$TARGET_USER" -H bash -lc 'docker version --format "{{.Server.Version}}"'
gpg --version | head -1
for tool in cpio dpkg-deb readelf rpm rpm2cpio tar zstd; do
  command -v "$tool" >/dev/null || fail "package validation tool is missing: $tool"
done

aws_ready=0
for attempt in 1 2 3 4 5 6; do
  if aws s3api get-bucket-location --bucket "$ARTIFACT_BUCKET" \
      --region "$AWS_REGION" >/dev/null 2>&1 && \
    aws secretsmanager describe-secret --secret-id "$BIOSEQ_SIGNING_KEY_SECRET_ARN" \
      --region "$AWS_REGION" >/dev/null 2>&1; then
    aws_ready=1
    break
  fi
  sleep $((attempt * 5))
done
[[ $aws_ready -eq 1 ]] || fail "instance-role S3/Secrets Manager preflight failed"

zram_priority=$(awk '$1 == "/dev/zram0" {print $5}' /proc/swaps)
disk_priority=$(awk '$1 == "/swapfile" {print $5}' /proc/swaps)
[[ $zram_priority =~ ^-?[0-9]+$ ]] || fail "zram swap is not active"
[[ $disk_priority =~ ^-?[0-9]+$ ]] || fail "disk swap is not active"
[[ $zram_priority -eq $BIOSEQ_ZRAM_SWAP_PRIORITY ]] || fail "wrong zram priority"
[[ $disk_priority -eq $BIOSEQ_DISK_SWAP_PRIORITY ]] || fail "wrong disk priority"
[[ $zram_priority -gt $disk_priority ]] || fail "zram priority is not above disk swap"

printf '[verify] OK architecture=%s staging=%s zram_priority=%s disk_priority=%s\n' \
  "$BIOSEQ_BUILD_ARCHITECTURE" "$BIOSEQ_SOURCE_STAGING_DIR" \
  "$zram_priority" "$disk_priority"
VERIFY
chown root:root /usr/local/sbin/bioseq-package-builder-verify
chmod 0755 /usr/local/sbin/bioseq-package-builder-verify

systemctl daemon-reload
systemctl enable bioseq-zram-swap.service
systemctl restart bioseq-zram-swap.service
systemctl try-restart ssh.service

# Fail closed unless every survivability/toolchain primitive and the scoped AWS
# identity are usable. No object or secret value is read or written here.
/usr/local/sbin/bioseq-package-builder-verify

ZRAM_PRIORITY=$(awk '$1 == "/dev/zram0" {print $5}' /proc/swaps)
DISK_PRIORITY=$(awk '$1 == "/swapfile" {print $5}' /proc/swaps)
[[ $ZRAM_PRIORITY -eq $BIOSEQ_ZRAM_SWAP_PRIORITY ]]
[[ $DISK_PRIORITY -eq $BIOSEQ_DISK_SWAP_PRIORITY ]]
[[ $ZRAM_PRIORITY -gt $DISK_PRIORITY ]]

cat > "$STATE_DIR/READY" <<EOF
status=READY
ready_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
architecture=$BIOSEQ_BUILD_ARCHITECTURE
aws_architecture=$AWS_ARCHITECTURE
ami_schema_version=$BIOSEQ_AMI_SCHEMA_VERSION
artifact_bucket=$ARTIFACT_BUCKET
artifact_prefix=$ARTIFACT_PREFIX
signing_key_secret_arn=$BIOSEQ_SIGNING_KEY_SECRET_ARN
source_staging_dir=$BIOSEQ_SOURCE_STAGING_DIR
zram_swap_priority=$ZRAM_PRIORITY
disk_swap_priority=$DISK_PRIORITY
compiler_rt_priority=$BIOSEQ_COMPILER_RT_PRIORITY
ssh_rt_priority=$BIOSEQ_SSH_RT_PRIORITY
EOF
chmod 0644 "$STATE_DIR/READY"
status READY
printf '[bootstrap] READY at %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
