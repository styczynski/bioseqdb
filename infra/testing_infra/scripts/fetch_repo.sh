#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# fetch_repo.sh — clone/update the bioseq GitHub repo using credentials stored
# in AWS Secrets Manager. Separate + re-runnable, as required.
#
# The instance role is scoped to read exactly one secret. That secret has two
# fields:
#   gh_ssh_private  - SSH deploy key  -> enables git push/pull over SSH
#   gh_access_token - gh PAT          -> enables `gh` (read the repo)
#
# Secret handling: values live ONLY in shell variables here. The SSH key is
# written to the user's ~/.ssh with mode 600; nothing is echoed or committed.
#
# Configurable via env (or /opt/bioseq/provision.env):
#   REPO          owner/name          (default styczynski/bioseqdb)
#   BRANCH        branch              (default main)
#   SECRET_NAME   secret id           (default github_secrets)
#   SECRET_REGION secret region       (default eu-north-1)
#   TARGET_USER   OS user to set up   (default current user)
#   REPO_DEST     clone destination   (default ~TARGET_USER/bioseqdb)
#
# Examples:
#   sudo REPO=styczynski/bioseqdb BRANCH=main ./fetch_repo.sh     # from root
#   ./fetch_repo.sh                                               # as ubuntu
# ---------------------------------------------------------------------------
set -euo pipefail

[ -f /opt/bioseq/provision.env ] && . /opt/bioseq/provision.env || true

REPO="${REPO:-styczynski/bioseqdb}"
BRANCH="${BRANCH:-main}"
SECRET_NAME="${SECRET_NAME:-github_secrets}"
SECRET_REGION="${SECRET_REGION:-eu-north-1}"
TARGET_USER="${TARGET_USER:-$(id -un)}"
USER_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
REPO_DEST="${REPO_DEST:-$USER_HOME/bioseqdb}"

log() { echo "[fetch_repo] $*"; }
SUDO=""; [ "$(id -u)" -eq 0 ] || SUDO="sudo"

# Run a command as the target user (directly if we are already them).
as_user() {
  if [ "$(id -un)" = "$TARGET_USER" ]; then "$@"; else sudo -u "$TARGET_USER" -H "$@"; fi
}

# Some secret stores flatten a PEM private key to a SINGLE line, replacing the
# newlines with spaces (e.g. when the key is pasted into the console as one
# line). OpenSSH/libcrypto then reject it with "error in libcrypto". When we
# detect that the BEGIN marker is not on its own line, rebuild a well-formed PEM:
# strip the markers, drop all whitespace from the base64 body, re-wrap at 64, and
# re-emit header/body/footer on their own lines. Already-valid keys pass through.
normalize_private_key() {
  local k="$1"
  if printf '%s\n' "$k" | grep -qxE '\-{5}BEGIN [A-Z0-9 ]+\-{5}'; then
    printf '%s\n' "$k"
    return
  fi
  local header footer body
  header="$(printf '%s' "$k" | grep -oE '\-{5}BEGIN [A-Z0-9 ]+\-{5}' | head -1)"
  footer="$(printf '%s' "$k" | grep -oE '\-{5}END [A-Z0-9 ]+\-{5}' | head -1)"
  if [ -z "$header" ] || [ -z "$footer" ]; then
    printf '%s\n' "$k"
    return
  fi
  body="$(printf '%s' "$k" | sed "s|$header||; s|$footer||" | tr -d ' \t\r\n')"
  printf '%s\n' "$header"
  printf '%s' "$body" | fold -w 64
  printf '\n%s\n' "$footer"
}

# --- gh CLI (install once; needed to honour the "configure gh" requirement) --
if ! command -v gh >/dev/null 2>&1; then
  log "installing gh CLI"
  curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
    | $SUDO dd of=/usr/share/keyrings/githubcli-archive-keyring.gpg status=none
  $SUDO chmod go+r /usr/share/keyrings/githubcli-archive-keyring.gpg
  echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
    | $SUDO tee /etc/apt/sources.list.d/github-cli.list >/dev/null
  $SUDO env DEBIAN_FRONTEND=noninteractive apt-get update -y
  $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y gh
fi

# --- Read the secret (values stay in shell variables only) -------------------
log "reading secret '$SECRET_NAME' from $SECRET_REGION"
SECRET_JSON="$(aws secretsmanager get-secret-value \
  --secret-id "$SECRET_NAME" --region "$SECRET_REGION" \
  --query SecretString --output text)"
GH_SSH_PRIVATE="$(printf '%s' "$SECRET_JSON" | jq -r '.gh_ssh_private')"
GH_TOKEN="$(printf '%s' "$SECRET_JSON" | jq -r '.gh_access_token')"
unset SECRET_JSON
[ -n "$GH_SSH_PRIVATE" ] && [ "$GH_SSH_PRIVATE" != "null" ] || { log "ERROR: gh_ssh_private missing"; exit 1; }

# --- Install the SSH deploy key for the target user --------------------------
SSH_DIR="$USER_HOME/.ssh"
KEY_FILE="$SSH_DIR/id_github"
$SUDO install -d -m 700 -o "$TARGET_USER" -g "$TARGET_USER" "$SSH_DIR"
# Write key as the target user with a tight umask, so it is never group/other readable.
# normalize_private_key repairs single-line/space-flattened PEMs before writing.
normalize_private_key "$GH_SSH_PRIVATE" | as_user bash -c "umask 077; cat > '$KEY_FILE'"
unset GH_SSH_PRIVATE
as_user chmod 600 "$KEY_FILE"

# Pin GitHub host keys and force this identity for github.com.
as_user bash -c "ssh-keyscan -t rsa,ecdsa,ed25519 github.com 2>/dev/null > '$SSH_DIR/known_hosts' || true"
as_user bash -c "cat > '$SSH_DIR/config' <<EOF
Host github.com
  HostName github.com
  User git
  IdentityFile $KEY_FILE
  IdentitiesOnly yes
  StrictHostKeyChecking accept-new
EOF
chmod 600 '$SSH_DIR/config'"

# --- git identity + gh auth (token) for the target user ----------------------
as_user git config --global user.name  "bioseq-bench-aws" || true
as_user git config --global user.email "bioseq-bench-aws@users.noreply.github.com" || true
as_user git config --global --add safe.directory "$REPO_DEST" || true
if [ -n "${GH_TOKEN:-}" ] && [ "$GH_TOKEN" != "null" ]; then
  log "configuring gh auth (token)"
  printf '%s' "$GH_TOKEN" | as_user gh auth login --with-token \
    && as_user gh auth setup-git \
    || log "gh auth step failed (continuing; SSH key already enables git)"
fi
unset GH_TOKEN

# --- Clone or update ---------------------------------------------------------
if [ -d "$REPO_DEST/.git" ]; then
  log "updating $REPO_DEST -> branch $BRANCH"
  as_user git -C "$REPO_DEST" remote set-url origin "git@github.com:$REPO.git"
  as_user git -C "$REPO_DEST" fetch origin "$BRANCH"
  as_user git -C "$REPO_DEST" checkout -B "$BRANCH" "origin/$BRANCH"
else
  log "cloning git@github.com:$REPO.git ($BRANCH) -> $REPO_DEST"
  as_user git clone --branch "$BRANCH" "git@github.com:$REPO.git" "$REPO_DEST"
fi
log "ready: $REPO_DEST @ $(as_user git -C "$REPO_DEST" rev-parse --short HEAD)"
