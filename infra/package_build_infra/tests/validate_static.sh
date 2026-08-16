#!/usr/bin/env bash
# shellcheck disable=SC2016 # Assertions intentionally match unexpanded source literals.
set -Eeuo pipefail

STACK_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MAIN_TF="$STACK_DIR/main.tf"
VARIABLES_TF="$STACK_DIR/variables.tf"
OUTPUTS_TF="$STACK_DIR/outputs.tf"
BOOTSTRAP="$STACK_DIR/scripts/bootstrap.sh"
USER_DATA="$STACK_DIR/scripts/user_data.sh.tftpl"
CONTROLLER="$STACK_DIR/../package_builder.py"

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
assert_literal "$MAIN_TF" 'disable_api_stop                     = true'
assert_literal "$MAIN_TF" 'disable_api_termination              = true'
assert_literal "$MAIN_TF" 'instance_initiated_shutdown_behavior = "stop"'
assert_literal "$MAIN_TF" 'delete_on_termination = false'
assert_literal "$MAIN_TF" 'Lifecycle           = "persistent-build-cache"'
assert_literal "$MAIN_TF" 'prevent_destroy = true'
assert_literal "$MAIN_TF" 'ignore_changes = ['
assert_literal "$MAIN_TF" 'ami,'
assert_literal "$MAIN_TF" 'associate_public_ip_address,'
assert_literal "$MAIN_TF" 'user_data,'
assert_literal "$MAIN_TF" 'user_data_replace_on_change = false'
assert_literal "$OUTPUTS_TF" 'output "builder_user_data"'
assert_literal "$MAIN_TF" 'block_public_policy     = true'
assert_literal "$MAIN_TF" 'status = "Enabled"'
assert_literal "$MAIN_TF" 'sse_algorithm = "AES256"'
assert_literal "$MAIN_TF" 'resources = ["${aws_s3_bucket.artifacts.arn}/${var.artifact_prefix}/*"]'
assert_literal "$MAIN_TF" '"s3:GetObjectVersion"'
assert_literal "$MAIN_TF" 'resource "aws_secretsmanager_secret" "omnipackage_signing_key"'
if rg --quiet 'resource[[:space:]]+"aws_secretsmanager_secret_version"' "$MAIN_TF"; then
  fail "OpenTofu must not put signing-key values into state"
fi

assert_literal "$VARIABLES_TF" 'root_volume_size_gb >= 500'
assert_literal "$VARIABLES_TF" 'can(regex("/32$", var.ssh_ingress_cidr))'
assert_literal "$VARIABLES_TF" 'var.ssh_ingress_cidr != "0.0.0.0/0"'
if ! rg --multiline --quiet \
    'variable "ami_schema_revision" \{[^}]*default[[:space:]]*=[[:space:]]*2' \
    "$VARIABLES_TF"; then
  fail "$VARIABLES_TF must default ami_schema_revision to revision 2"
fi

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
assert_literal "$BOOTSTRAP" 'python3 python3-poetry'
assert_literal "$BOOTSTRAP" 'python3-venv rpm rpm2cpio rsync'
assert_literal "$BOOTSTRAP" 'caches/poetry-venvs'
assert_literal "$BOOTSTRAP" 'python3 -m venv'
assert_literal "$BOOTSTRAP" 'python3 --version && poetry --version'
assert_literal "$BOOTSTRAP" '[[ $BIOSEQ_AMI_SCHEMA_REVISION == 2 ]]'

if rg --quiet --ignore-case 'rustup|rustc --version|cargo --version|\.cargo/bin' "$BOOTSTRAP"; then
  fail "$BOOTSTRAP must not install, expose, or verify the retired Rust/Cargo toolchain"
fi

# Bootstrap paths never change instance lifecycle. Release cleanup may issue
# only the controller's constant SSM poweroff command, which maps to the
# instance's configured stop behavior and preserves its EBS cache.
for lifecycle_file in "$BOOTSTRAP" "$USER_DATA"; do
  for forbidden_lifecycle in \
    stop-instances terminate-instances \
    'sudo shutdown' 'sudo poweroff' 'sudo halt' 'sudo reboot' \
    '/sbin/shutdown' '/sbin/poweroff' '/sbin/halt' '/sbin/reboot' \
    'systemctl poweroff' 'systemctl halt' 'systemctl reboot' \
    'tofu destroy'; do
    if rg --fixed-strings --quiet -- "$forbidden_lifecycle" "$lifecycle_file"; then
      fail "forbidden instance lifecycle operation found in $lifecycle_file: $forbidden_lifecycle"
    fi
  done
done
for forbidden_lifecycle in \
  stop-instances terminate-instances \
  'sudo shutdown' 'sudo poweroff' 'sudo halt' 'sudo reboot' \
  '/sbin/shutdown' '/sbin/poweroff' '/sbin/halt' '/sbin/reboot' \
  'systemctl halt' 'systemctl reboot' 'tofu destroy'; do
  if rg --fixed-strings --quiet -- "$forbidden_lifecycle" "$CONTROLLER"; then
    fail "forbidden instance lifecycle operation found in $CONTROLLER: $forbidden_lifecycle"
  fi
done
assert_literal "$CONTROLLER" '"AWS-RunShellScript"'
assert_literal "$CONTROLLER" '{"commands": ["/usr/bin/systemctl poweroff"]}'
assert_literal "$CONTROLLER" 'committed_source_tree_sha256'
assert_literal "$CONTROLLER" '.bioseq-source-tree-sha256'
assert_literal "$CONTROLLER" 'record_builder_stop_ownership'
assert_literal "$CONTROLLER" 'if payload["builders"][name]["stop_owned"]'
assert_literal "$CONTROLLER" 'REMOTE_OPTIMIZATION_EVIDENCE_SCRIPT'
assert_literal "$CONTROLLER" 'production package compile command must resolve to only -O3'
assert_literal "$CONTROLLER" 'require_compiler_optimization_evidence'
assert_literal "$CONTROLLER" '"optimization_flags": ["-O3"]'
assert_literal "$CONTROLLER" '"forbidden_optimization_flags": []'
assert_literal "$CONTROLLER" 'force_remote_package_reconfigure'
assert_literal "$CONTROLLER" '*/build-repository-root/CMakeCache.txt'
assert_literal "$CONTROLLER" '.pre-o3-${token}'
assert_literal "$CONTROLLER" 'cmake/verify_o3_compile_database.py'
assert_literal "$CONTROLLER" '"language_compile_commands"'
assert_literal "$CONTROLLER" '"cmake_cache_paths"'
assert_literal "$CONTROLLER" '"compile_databases"'

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
