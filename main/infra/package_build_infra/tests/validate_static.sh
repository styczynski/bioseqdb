#!/usr/bin/env bash
set -Eeuo pipefail

STACK_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MAIN_TF="$STACK_DIR/main.tf"
VARIABLES_TF="$STACK_DIR/variables.tf"
OUTPUTS_TF="$STACK_DIR/outputs.tf"
BOOTSTRAP="$STACK_DIR/scripts/bootstrap.sh"
USER_DATA="$STACK_DIR/scripts/user_data.sh.tftpl"

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

assert_literal() {
  local file=$1
  local literal=$2
  rg --fixed-strings --quiet -- "$literal" "$file" || \
    fail "$file is missing: $literal"
}

bash -n "$BOOTSTRAP"
bash -n "$USER_DATA"

assert_literal "$MAIN_TF" 'instance_type    = "c7g.xlarge"'
assert_literal "$MAIN_TF" 'instance_type    = "m4.xlarge"'
assert_literal "$MAIN_TF" '/arm64/hvm/ebs-gp3/ami-id'
assert_literal "$MAIN_TF" '/amd64/hvm/ebs-gp3/ami-id'
assert_literal "$MAIN_TF" 'for_each = local.builders'
assert_literal "$MAIN_TF" 'volume_type           = "gp3"'
assert_literal "$MAIN_TF" 'encrypted             = true'
assert_literal "$MAIN_TF" 'disable_api_termination              = true'
assert_literal "$MAIN_TF" 'instance_initiated_shutdown_behavior = "stop"'
assert_literal "$MAIN_TF" 'prevent_destroy = true'
assert_literal "$MAIN_TF" 'ignore_changes = ['
assert_literal "$MAIN_TF" 'associate_public_ip_address,'
assert_literal "$MAIN_TF" 'user_data,'
assert_literal "$OUTPUTS_TF" 'output "builder_user_data"'
assert_literal "$MAIN_TF" 'block_public_policy     = true'
assert_literal "$MAIN_TF" 'status = "Enabled"'
assert_literal "$MAIN_TF" 'sse_algorithm = "AES256"'
assert_literal "$MAIN_TF" 'resources = ["${aws_s3_bucket.artifacts.arn}/${var.artifact_prefix}/*"]'
assert_literal "$MAIN_TF" 'resource "aws_secretsmanager_secret" "omnipackage_signing_key"'
if rg --quiet 'resource[[:space:]]+"aws_secretsmanager_secret_version"' "$MAIN_TF"; then
  fail "OpenTofu must not put signing-key values into state"
fi

assert_literal "$VARIABLES_TF" 'root_volume_size_gb >= 500'
assert_literal "$VARIABLES_TF" 'can(regex("/32$", var.ssh_ingress_cidr))'
assert_literal "$VARIABLES_TF" 'var.ssh_ingress_cidr != "0.0.0.0/0"'

for output_name in \
  builders ssh_private_key_path region package_bucket_name package_bucket_arn \
  signing_key_secret_arn; do
  assert_literal "$OUTPUTS_TF" "output \"$output_name\""
done

assert_literal "$BOOTSTRAP" 'BIOSEQ_ZRAM_SWAP_PRIORITY'
assert_literal "$BOOTSTRAP" 'BIOSEQ_DISK_SWAP_PRIORITY'
assert_literal "$BOOTSTRAP" 'allocated_device=$(zramctl --find)'
assert_literal "$BOOTSTRAP" 'if [[ $(cat /sys/block/zram0/disksize) -ne 0 ]]'
assert_literal "$BOOTSTRAP" 'zram device %s did not become ready after hot-add'
assert_literal "$BOOTSTRAP" 'CPUSchedulingPriority=$BIOSEQ_SSH_RT_PRIORITY'
assert_literal "$BOOTSTRAP" '/usr/bin/chrt --rr "$BIOSEQ_COMPILER_RT_PRIORITY"'
assert_literal "$BOOTSTRAP" 'CPUSchedulingResetOnFork=true'
assert_literal "$BOOTSTRAP" '/usr/local/sbin/bioseq-package-builder-verify'
assert_literal "$BOOTSTRAP" 'BIOSEQ_SOURCE_STAGING_DIR'
assert_literal "$BOOTSTRAP" 'status READY'
assert_literal "$BOOTSTRAP" 'rpm rpm2cpio rsync'
assert_literal "$BOOTSTRAP" 'cpio dpkg-deb readelf rpm rpm2cpio tar zstd'
assert_literal "$BOOTSTRAP" 'chmod 0755 "$RUSTUP_TMP"'

# Protected development instances must never become targets of this isolated
# stack. Their ids belong only in the orchestrator's explicit denylist.
if rg --quiet --glob '!**/tests/validate_static.sh' \
    'i-05b18218430e9fecb|i-05d1fa02e86288637' "$STACK_DIR"; then
  fail "protected development instance id leaked into package-builder resources"
fi

if command -v shellcheck >/dev/null 2>&1; then
  shellcheck "$BOOTSTRAP"
else
  printf 'SKIP: shellcheck is not installed\n'
fi

TOFU_BIN=""
if command -v tofu >/dev/null 2>&1; then
  TOFU_BIN=$(command -v tofu)
fi

if [[ -n $TOFU_BIN ]]; then
  "$TOFU_BIN" -chdir="$STACK_DIR" fmt -check -recursive
  "$TOFU_BIN" -chdir="$STACK_DIR" init -backend=false -input=false
  "$TOFU_BIN" -chdir="$STACK_DIR" validate
else
  printf 'SKIP: OpenTofu is not installed\n'
fi

# Keep this stack and its validation path OpenTofu-only. Construct the forbidden
# legacy executable name so the test does not flag its own guard expression.
NON_TOFU_CLI=$(printf '%s%s' terra form)
if rg --quiet --fixed-strings --glob '!versions.tf' --glob '!.gitignore' \
    "$NON_TOFU_CLI" "$STACK_DIR"; then
  fail "non-OpenTofu CLI reference found"
fi

printf 'PASS: package_build_infra static validation\n'
