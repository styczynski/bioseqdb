#!/usr/bin/env bash
# Reuse the existing package-builder zram setup, without disk swap.
set -Eeuo pipefail
cat > /usr/local/sbin/bioseq-zram-swap <<'ZRAM'
#!/usr/bin/env bash
set -Eeuo pipefail
# shellcheck disable=SC1091
BIOSEQ_ZRAM_FRACTION_PERCENT=100
BIOSEQ_ZRAM_SWAP_PRIORITY=100
device=/dev/zram0

wait_for_zram_device() {
  local attempt
  for attempt in {1..50}; do
    [[ -b $device && -r /sys/block/zram0/comp_algorithm ]] && return 0
    sleep 0.1
  done
  return 1
}

ensure_zram_device() {
  modprobe zram num_devices=1
  # modprobe may create zram0 asynchronously, or current AWS kernels may expose
  # only the dynamic hot-add interface. First give the legacy device time to
  # settle; allocate explicitly only when it never appears.
  if ! wait_for_zram_device; then
    allocated_device=$(zramctl --find)
    [[ $allocated_device == "$device" ]] || {
      printf 'expected %s but zram allocated %s\n' "$device" "$allocated_device" >&2
      return 1
    }
    wait_for_zram_device || {
      printf 'zram device %s did not become ready after hot-add\n' "$device" >&2
      return 1
    }
  fi
}

case "${1:-start}" in
  start)
    active_priority=$(awk -v device="$device" '$1 == device {print $5}' /proc/swaps)
    if [[ ${active_priority:-} == "$BIOSEQ_ZRAM_SWAP_PRIORITY" ]]; then
      exit 0
    elif [[ -n ${active_priority:-} ]]; then
      swapoff "$device"
    fi
    ensure_zram_device
    # A nonzero inactive device is stale. util-linux removes dynamically
    # hot-added devices when resetting them, so always ensure it again after a
    # reset instead of assuming the block node survived.
    if [[ $(cat /sys/block/zram0/disksize) -ne 0 ]]; then
      zramctl --reset "$device" 2>/dev/null || true
      ensure_zram_device
    fi
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

systemctl daemon-reload
systemctl enable --now bioseq-zram-swap.service
zramctl
cat /proc/swaps
