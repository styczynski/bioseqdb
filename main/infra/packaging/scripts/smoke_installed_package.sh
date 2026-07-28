#!/usr/bin/env bash
# Runs inside a newly created distro container. All diagnostic output goes to
# stderr; stdout is reserved for one machine-readable success attestation.
set -euo pipefail

exec 3>&1
exec 1>&2

die() {
  echo "bioseq installed-package smoke: $*" >&2
  exit 1
}

readonly distro="${BIOSEQ_SMOKE_DISTRO:-}"
readonly postgres_major="${BIOSEQ_POSTGRES_MAJOR:-}"
readonly requested_arch="${BIOSEQ_ARCHITECTURE:-}"
readonly package_path="${BIOSEQ_PACKAGE_PATH:-}"
readonly package_name="bioseq-postgresql${postgres_major}"

if ! [[ "${postgres_major}" =~ ^[0-9]+$ ]] || (( postgres_major < 17 )); then
  die "BIOSEQ_POSTGRES_MAJOR must be 17 or newer"
fi
[[ -f "${package_path}" ]] || die "package mount is missing: ${package_path}"

case "$(uname -m)" in
  x86_64) native_arch=x64 ;;
  aarch64|arm64) native_arch=arm64 ;;
  *) die "unsupported native architecture $(uname -m)" ;;
esac
[[ "${requested_arch}" == "${native_arch}" ]] \
  || die "requested ${requested_arch} on native ${native_arch}; emulation is forbidden"

# Every supported smoke image provides this file.
# shellcheck disable=SC1091
. /etc/os-release
case "${distro}:${ID:-}" in
  debian_13:debian)
    [[ "${VERSION_ID:-}" =~ ^13([.]|$) ]] || die "Debian image is not release 13"
    ;;
  ubuntu_26.04:ubuntu)
    [[ "${VERSION_ID:-}" == "26.04" ]] || die "Ubuntu image is not release 26.04"
    ;;
  fedora_44:fedora)
    [[ "${VERSION_ID:-}" == "44" ]] || die "Fedora image is not release 44"
    ;;
  opensuse_16.0:opensuse*|opensuse_16.0:sles)
    [[ "${VERSION_ID:-}" =~ ^16[.]0([.]|$) ]] || die "openSUSE image is not release 16.0"
    ;;
  arch:arch|arch:archarm) ;;
  *) die "container OS ${ID:-unknown} does not match ${distro}" ;;
esac

install_debian_package() {
  export DEBIAN_FRONTEND=noninteractive
  printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
  chmod 0755 /usr/sbin/policy-rc.d
  apt-get update
  apt-get install -y --no-install-recommends ca-certificates curl gnupg libvulkan1 \
    locales postgresql-common util-linux
  install -d -m 0755 /usr/share/postgresql-common/pgdg
  curl -fsSL https://www.postgresql.org/media/keys/ACCC4CF8.asc \
    -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc
  [[ -n "${VERSION_CODENAME:-}" ]] || die "VERSION_CODENAME is missing"
  printf '%s\n' \
    "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] https://apt.postgresql.org/pub/repos/apt ${VERSION_CODENAME}-pgdg main" \
    > /etc/apt/sources.list.d/pgdg.list
  apt-get update
  apt-get install -y --no-install-recommends "postgresql-${postgres_major}"
  apt-get install -y --no-install-recommends "${package_path}"
  dpkg-query -W -f='${Status}\n' "${package_name}" | grep -Fx 'install ok installed'
}

install_fedora_package() {
  dnf install -y --allowerasing ca-certificates shadow-utils util-linux \
    "postgresql${postgres_major}" "postgresql${postgres_major}-server"
  dnf install -y --allowerasing --nogpgcheck "${package_path}"
  rpm -q "${package_name}"
}

install_opensuse_package() {
  if ! zypper --non-interactive install -y --allow-vendor-change \
      "postgresql${postgres_major}" "postgresql${postgres_major}-server" \
      libvulkan1 shadow util-linux; then
    zypper --non-interactive addrepo --refresh \
      "https://download.opensuse.org/repositories/server:database:postgresql/${VERSION_ID}/server:database:postgresql.repo" \
      bioseq-postgresql
    zypper --gpg-auto-import-keys --non-interactive refresh
    zypper --non-interactive install -y --allow-vendor-change \
      "postgresql${postgres_major}" "postgresql${postgres_major}-server" \
      libvulkan1 shadow util-linux
  fi
  zypper --no-gpg-checks --non-interactive install -y --allow-vendor-change \
    "${package_path}"
  rpm -q "${package_name}"
}

install_arch_package() {
  pacman -Syu --noconfirm --needed ca-certificates util-linux vulkan-icd-loader postgresql
  if [[ "${postgres_major}" == "17" ]]; then
    pacman -S --noconfirm --needed postgresql-old-upgrade
  fi
  pacman -U --noconfirm "${package_path}"
  pacman -Q "${package_name}"
}

case "${distro}" in
  debian_13|ubuntu_26.04) install_debian_package ;;
  fedora_44) install_fedora_package ;;
  opensuse_16.0) install_opensuse_package ;;
  arch) install_arch_package ;;
  *) die "unsupported distro ${distro}" ;;
esac

pg_bindir_candidates=(
  "/usr/lib/postgresql/${postgres_major}/bin"
  "/usr/pgsql-${postgres_major}/bin"
  "/opt/pgsql-${postgres_major}/bin"
  "/usr/lib/postgresql${postgres_major}/bin"
  "/usr/lib64/postgresql${postgres_major}/bin"
  "/usr/lib64/pgsql/postgresql-${postgres_major}/bin"
  "/usr/bin"
)
while IFS= read -r postgres_binary; do
  pg_bindir_candidates+=("$(dirname "${postgres_binary}")")
done < <(find /usr /opt -type f -name postgres -perm -u+x 2>/dev/null || true)

pg_bindir=
for candidate in "${pg_bindir_candidates[@]}"; do
  [[ -x "${candidate}/postgres" && -x "${candidate}/initdb" \
      && -x "${candidate}/pg_ctl" ]] || continue
  candidate_version="$("${candidate}/postgres" --version 2>/dev/null || true)"
  if [[ "${candidate_version}" =~ PostgreSQL[[:space:]]+${postgres_major}([^0-9]|$) ]]; then
    pg_bindir="${candidate}"
    break
  fi
done
[[ -n "${pg_bindir}" ]] \
  || die "exact PostgreSQL ${postgres_major} server bindir is missing from runtime packages"

pg_libdir="${pg_bindir%/bin}/lib"
if [[ -x "${pg_bindir}/pg_config" ]]; then
  pg_libdir="$("${pg_bindir}/pg_config" --libdir)"
fi
readonly pg_bindir pg_libdir
readonly initdb="${pg_bindir}/initdb"
readonly pg_ctl="${pg_bindir}/pg_ctl"
[[ -x "${initdb}" && -x "${pg_ctl}" ]] \
  || die "PostgreSQL ${postgres_major} server tools are missing under ${pg_bindir}"

if ! getent group postgres >/dev/null; then
  groupadd --system postgres
fi
if ! id postgres >/dev/null 2>&1; then
  useradd --system --gid postgres --home-dir /var/lib/postgres --shell /bin/bash postgres
fi

readonly data_dir=/tmp/bioseq-package-pgdata
readonly socket_dir=/tmp/bioseq-package-pgsocket
readonly log_file=/tmp/bioseq-package-postgres.log
install -d -m 0700 -o postgres -g postgres "${data_dir}"
install -d -m 0770 -o postgres -g postgres "${socket_dir}"

run_pg() {
  runuser -u postgres -- env \
    "LD_LIBRARY_PATH=${pg_libdir}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    "$@"
}

started=false
cleanup() {
  if [[ "${started}" == true ]]; then
    run_pg "${pg_ctl}" -D "${data_dir}" -m fast -w stop >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

run_pg "${initdb}" -D "${data_dir}" --no-locale --encoding=UTF8 --auth=trust
cat >> "${data_dir}/postgresql.conf" <<'EOF'
listen_addresses = ''
port = 55432
unix_socket_directories = '/tmp/bioseq-package-pgsocket'
shared_preload_libraries = 'bioseq_cache'
EOF
chown postgres:postgres "${data_dir}/postgresql.conf"
run_pg "${pg_ctl}" -D "${data_dir}" -l "${log_file}" -w start
started=true

psql="${pg_bindir}/psql"
if [[ ! -x "${psql}" ]]; then
  psql="$(command -v psql || true)"
fi
[[ -x "${psql}" ]] || die "psql is missing"
psql_command=("${psql}" -X -v ON_ERROR_STOP=1 -h "${socket_dir}" -p 55432 -d postgres)
run_pg "${psql_command[@]}" -c 'CREATE EXTENSION bioseq_cache;'
run_pg "${psql_command[@]}" -c 'CREATE EXTENSION bioseq;'

preload="$(run_pg "${psql_command[@]}" -Atqc 'SHOW shared_preload_libraries')"
[[ ",${preload}," == *",bioseq_cache,"* ]] \
  || die "bioseq_cache is not active in shared_preload_libraries: ${preload}"
extension_count="$(run_pg "${psql_command[@]}" -Atqc \
  "SELECT count(*) FROM pg_extension WHERE extname IN ('bioseq','bioseq_cache')")"
[[ "${extension_count}" == "2" ]] || die "both extensions were not created"
roundtrip="$(run_pg "${psql_command[@]}" -Atqc \
  "SELECT 'ACGT'::bioseq.seq('dna4', 8)::text")"
[[ -n "${roundtrip}" && "${roundtrip}" == "ACGT" ]] \
  || die "nonempty Bioseq roundtrip failed: ${roundtrip:-<empty>}"

printf '{"passed":true,"package_installed":true,"preload_active":true,"bioseq_extension_created":true,"bioseq_cache_extension_created":true,"nonempty_roundtrip":true,"roundtrip":"ACGT","distro":"%s","architecture":"%s","postgres_major":%s}\n' \
  "${distro}" "${requested_arch}" "${postgres_major}" >&3
