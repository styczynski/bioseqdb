#!/usr/bin/env bash
# tests/e2e/run.sh — end-to-end happy-path test for `omnipackage init` + build
# + install. Generates per-type hello-world packages, installs them in a
# fresh distro container, runs the binary, asserts the output line.
#
# This is a manual developer tool — NOT run by `cargo test`.
#
# Usage:
#   bash tests/e2e/run.sh
#   TYPES=rust DISTROS=fedora_42 bash tests/e2e/run.sh
#   RUNTIME=docker VERBOSE=1 bash tests/e2e/run.sh
#   TMP_ROOT=/var/scratch/omni-e2e bash tests/e2e/run.sh   # override /tmp
#
# All artifacts (source, build, repo, logs) are kept under
# $TMP_ROOT/<type>/ for inspection. Re-running a type wipes its dir fresh,
# so there's no stale-state concern — clean up by `rm -rf $TMP_ROOT` when
# you're done.
#
# Defaults: 8 project types × 8 distros (one recent per family) = 64 builds.
# Plan on ~2 hours for a full clean run; subsequent runs are faster if you
# kept the omnipackage image cache primed.

set -euo pipefail

TYPES_DEFAULT="c cpp cmake rust go python ruby crystal"
DISTROS_DEFAULT="opensuse_16.0 fedora_42 debian_13 ubuntu_24.04 almalinux_10 rockylinux_9 mageia_9 arch"

TYPES="${TYPES:-$TYPES_DEFAULT}"
DISTROS="${DISTROS:-$DISTROS_DEFAULT}"
RUNTIME="${RUNTIME:-podman}"
VERBOSE="${VERBOSE:-0}"
# Where workdirs / build dirs / logs go. Default /tmp can fill up fast on
# a full run (build_dir per type holds container output + rpms + debs).
# Point this somewhere with room (e.g. /var/scratch/omni-e2e) for full runs.
TMP_ROOT="${TMP_ROOT:-/tmp/omnipackage-e2e}"
mkdir -p "$TMP_ROOT"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OMNIPACKAGE_BIN="${OMNIPACKAGE_BIN:-$REPO_ROOT/target/debug/omnipackage}"

if [ ! -x "$OMNIPACKAGE_BIN" ]; then
    echo "Building omnipackage binary..."
    (cd "$REPO_ROOT" && cargo build)
fi

if ! command -v "$RUNTIME" >/dev/null 2>&1; then
    echo "Container runtime '$RUNTIME' not found. Set RUNTIME=docker if you don't have podman." >&2
    exit 1
fi

# ---------- helpers ----------

distro_image() {
    case "$1" in
        opensuse_16.0)          echo "opensuse/leap:16.0" ;;
        fedora_42)              echo "fedora:42" ;;
        debian_13)              echo "debian:trixie" ;;
        ubuntu_24.04)           echo "ubuntu:24.04" ;;
        almalinux_10)           echo "almalinux:10" ;;
        rockylinux_9)           echo "rockylinux/rockylinux:9" ;;
        mageia_9)               echo "mageia:9" ;;
        arch)                   echo "archlinux:latest" ;;
        manjaro)                echo "manjarolinux/base:latest" ;;
        *) echo "unknown distro: $1" >&2; exit 1 ;;
    esac
}

distro_pkg_type() {
    case "$1" in
        debian_*|ubuntu_*) echo "deb" ;;
        arch|manjaro)      echo "pkg.tar.zst" ;;
        *)                 echo "rpm" ;;
    esac
}

# Install command operating on /pkg.<ext> inside the verify container.
# Uses the native package manager so runtime deps get pulled.
distro_install_cmd() {
    case "$1" in
        debian_*|ubuntu_*)
            echo "apt-get update -qq && apt-get install -y /pkg.deb"
            ;;
        opensuse_*)
            echo "zypper --non-interactive --no-gpg-checks install /pkg.rpm"
            ;;
        mageia_*)
            # Mageia 9 ships dnf alongside urpmi; either works on a local rpm.
            echo "dnf install -y --nogpgcheck /pkg.rpm"
            ;;
        arch|manjaro)
            # LocalFileSigLevel defaults to Optional, so installing an unsigned
            # local file is allowed — we mount only the package, not its .sig.
            # -Sy refreshes the sync db so any declared runtime deps resolve.
            echo "pacman -Sy --noconfirm && pacman -U --noconfirm /pkg.pkg.tar.zst"
            ;;
        *)
            # fedora, almalinux, rockylinux
            echo "dnf install -y --nogpgcheck /pkg.rpm"
            ;;
    esac
}

# ---------- main loop ----------

PASS=()
FAIL=()
START_TIME=$(date +%s)
echo "Working dir: $TMP_ROOT"
echo "Per-type layout: $TMP_ROOT/<type>/{source,build,repo,logs}/"
echo

for type in $TYPES; do
    echo "===== $type ====="
    src="$SCRIPT_DIR/projects/$type"
    if [ ! -d "$src" ]; then
        echo "  fixture not found: $src"
        FAIL+=("$type:no-fixture")
        continue
    fi

    type_dir="$TMP_ROOT/$type"
    workdir="$type_dir/source"
    build_dir="$type_dir/build"
    repo_dir="$type_dir/repo"
    log_dir="$type_dir/logs"

    # Clean prior runs so deterministic names don't pick up stale artifacts.
    rm -rf "$type_dir"
    mkdir -p "$workdir" "$build_dir" "$repo_dir" "$log_dir"

    cp -r "$src/." "$workdir/"

    # 1. init — no --type, so detection is also under test. --package-name is
    # pinned so the spec's binary path matches the fixture's install target;
    # without it C/Cpp/Python would slug-name from the tmpdir basename.
    if ! "$OMNIPACKAGE_BIN" init "$workdir" \
            --package-name "helloworld-$type" \
            --maintainer "E2E" --email "e2e@example.com" --force \
            > "$log_dir/init.log" 2>&1; then
        echo "  init FAILED — see $log_dir/init.log"
        FAIL+=("$type:init")
        continue
    fi
    detected=$(grep -A1 "Detected project type:" "$log_dir/init.log" | tail -1 | awk '{print $1}')
    if [ "$detected" != "$type" ]; then
        echo "  detection MISMATCH: expected $type, got $detected"
        FAIL+=("$type:detect-$detected")
        continue
    fi

    # 2. redirect localfs path → TMP_ROOT. The init template uses
    # ${HOME}/<package_name>-repo, and we passed --package-name above, so the
    # rendered path is ${HOME}/helloworld-<type>-repo.
    config="$workdir/.omnipackage/config.yml"
    sed -i "s|\${HOME}/helloworld-$type-repo|$repo_dir|g" "$config"

    # 3. ephemeral GPG key → .env so the publish stage can sign packages.
    if ! gpg_key=$("$OMNIPACKAGE_BIN" gpg generate -n "E2E" -e "e2e@example.com" --format base64 2> "$log_dir/gpg.log"); then
        echo "  gpg generate FAILED — see $log_dir/gpg.log"
        FAIL+=("$type:gpg")
        continue
    fi
    printf 'GPG_KEY=%s\n' "$(printf '%s' "$gpg_key" | tr -d '\n')" > "$workdir/.env"

    # 4. release = build + publish to localfs repo at $repo_dir/<distro>/
    # --env-file is resolved against cwd by default, so pass it explicitly.
    echo "  releasing for: $DISTROS"
    release_args=("release" "$workdir"
        "--build-dir" "$build_dir"
        "--env-file" "$workdir/.env"
        "--repository" "Local test"
        "--distros" $DISTROS)
    if [ "$VERBOSE" = "1" ]; then
        if ! "$OMNIPACKAGE_BIN" "${release_args[@]}" 2>&1 | tee "$log_dir/release.log"; then
            echo "  release FAILED — see $log_dir/release.log"
            for d in $DISTROS; do FAIL+=("$type/$d:release"); done
            continue
        fi
    else
        if ! "$OMNIPACKAGE_BIN" "${release_args[@]}" --container-output null \
                > "$log_dir/release.log" 2>&1; then
            echo "  release FAILED — tail of $log_dir/release.log:"
            tail -30 "$log_dir/release.log" | sed 's/^/    /'
            for d in $DISTROS; do FAIL+=("$type/$d:release"); done
            continue
        fi
    fi

    # 5. verify each distro by installing the published package in a fresh container.
    for distro in $DISTROS; do
        pkg_type=$(distro_pkg_type "$distro")
        image=$(distro_image "$distro")
        install_cmd=$(distro_install_cmd "$distro")

        artifact=$(find "$repo_dir/$distro" -name "*.$pkg_type" -type f 2>/dev/null | head -1)
        if [ -z "$artifact" ]; then
            echo "  $distro: NO ARTIFACT (expected $repo_dir/$distro/**/*.$pkg_type)"
            FAIL+=("$type/$distro:no-artifact")
            continue
        fi

        printf "  %-22s install+run... " "$distro"
        verify_log="$log_dir/$distro.verify.log"
        set +e
        "$RUNTIME" run --rm \
            -v "$artifact:/pkg.$pkg_type:ro" \
            "$image" sh -c "$install_cmd && helloworld-$type" \
            > "$verify_log" 2>&1
        rc=$?
        set -e

        expected="hello from $type 1.2.3"
        if [ $rc -eq 0 ] && grep -qxF "$expected" "$verify_log"; then
            echo "OK"
            PASS+=("$type/$distro")
        else
            echo "FAIL (rc=$rc)"
            echo "    expected: $expected"
            echo "    last lines of $verify_log:"
            tail -5 "$verify_log" | sed 's/^/      /'
            FAIL+=("$type/$distro:assert")
        fi
    done

    # Everything stays under $type_dir for inspection. Re-running this type
    # wipes it fresh at the top of the loop, so stale state isn't an issue.
done

# ---------- summary ----------

ELAPSED=$(( $(date +%s) - START_TIME ))
echo
echo "===== SUMMARY ====="
printf "Elapsed: %dm %ds\n" $((ELAPSED / 60)) $((ELAPSED % 60))
echo "Passed: ${#PASS[@]}"
echo "Failed: ${#FAIL[@]}"

# When running under GitHub Actions, also append a markdown summary to the
# job's Summary tab. Gated on $GITHUB_STEP_SUMMARY so local runs are unaffected.
# Must precede the `exit 1` below so it fires on failure too.
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        printf '### E2E — `%s`\n\n' "$DISTROS"
        printf '| Type | Result |\n|------|--------|\n'
        for entry in ${PASS[@]+"${PASS[@]}"}; do
            printf '| %s | ✅ |\n' "${entry%%/*}"
        done
        for entry in ${FAIL[@]+"${FAIL[@]}"}; do
            # entry: type/distro:reason  or  type:reason
            left="${entry%%:*}"
            reason="${entry#*:}"
            printf '| %s | ❌ %s |\n' "${left%%/*}" "$reason"
        done
        printf '\n**Passed:** %d &nbsp;&nbsp; **Failed:** %d\n' "${#PASS[@]}" "${#FAIL[@]}"
    } >> "$GITHUB_STEP_SUMMARY"
fi

if [ ${#FAIL[@]} -gt 0 ]; then
    printf '  - %s\n' "${FAIL[@]}"
    echo
    echo "Logs preserved at: $TMP_ROOT/<type>/logs/"
    exit 1
fi

echo "All clear."
