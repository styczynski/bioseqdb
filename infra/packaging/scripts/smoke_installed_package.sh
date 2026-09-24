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

apt_install_refreshed() {
  local dependency_attempt
  for dependency_attempt in 1 2 3; do
    apt-get -o Acquire::Retries=3 -o Acquire::http::No-Cache=true \
      -o Acquire::https::No-Cache=true update
    if apt-get -o Acquire::Retries=3 install -y --no-install-recommends "$@"; then
      return 0
    fi
    if (( dependency_attempt == 3 )); then return 100; fi
    printf 'Refreshing package indexes after dependency download failure\n' >&2
  done
}

install_debian_package() {
  export DEBIAN_FRONTEND=noninteractive
  printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
  chmod 0755 /usr/sbin/policy-rc.d
  apt_install_refreshed ca-certificates curl gnupg libvulkan1 \
    locales postgresql-common util-linux
  install -d -m 0755 /usr/share/postgresql-common/pgdg
  curl -fsSL https://www.postgresql.org/media/keys/ACCC4CF8.asc \
    -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc
  [[ -n "${VERSION_CODENAME:-}" ]] || die "VERSION_CODENAME is missing"
  local pgdg_suite="${VERSION_CODENAME}-pgdg"
  local pgdg_components=main
  local -a pgdg_install_options=()
  if (( postgres_major >= 19 )); then
    pgdg_components="main ${postgres_major}"
  fi
  if (( postgres_major >= 20 )); then
    pgdg_suite="${VERSION_CODENAME}-pgdg-snapshot"
    pgdg_install_options=(-t "${pgdg_suite}")
  fi
  printf '%s\n' \
    "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] https://apt.postgresql.org/pub/repos/apt ${pgdg_suite} ${pgdg_components}" \
    > /etc/apt/sources.list.d/pgdg.list
  apt_install_refreshed "${pgdg_install_options[@]}" \
    "postgresql-${postgres_major}"
  apt_install_refreshed "${package_path}"
  dpkg-query -W -f='${Status}\n' "${package_name}" | grep -Fx 'install ok installed'
}

install_fedora_package() {
  dnf install -y --allowerasing ca-certificates curl shadow-utils util-linux
  if ! dnf install -y --allowerasing \
      "postgresql${postgres_major}" "postgresql${postgres_major}-server"; then
    (( postgres_major >= 19 )) || die "requested Fedora PostgreSQL packages are unavailable"
    local pgdg_url="https://download.postgresql.org/pub/repos/yum/testing/${postgres_major}/fedora/fedora-${VERSION_ID}-$(uname -m)"
    curl -fsSL --retry 3 "${pgdg_url}/repodata/repomd.xml" >/dev/null \
      || die "no native PGDG prerelease repository at ${pgdg_url}"
    dnf -y \
      --repofrompath="bioseq-pgdg,${pgdg_url}" \
      --setopt=bioseq-pgdg.gpgcheck=1 \
      --setopt=bioseq-pgdg.gpgkey=https://download.postgresql.org/pub/repos/yum/keys/PGDG-RPM-GPG-KEY-Fedora \
      install --allowerasing "postgresql${postgres_major}" "postgresql${postgres_major}-server"
  fi
  dnf install -y --allowerasing --nogpgcheck "${package_path}"
  rpm -q "${package_name}"
}

install_opensuse_package() {
  if ! zypper --non-interactive install -y --allow-vendor-change \
      "postgresql${postgres_major}" "postgresql${postgres_major}-server" \
      libvulkan1 shadow util-linux; then
    zypper --non-interactive addrepo --refresh \
      "https://download.opensuse.org/repositories/server:/database:/postgresql/${VERSION_ID}/" \
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
  # pacman 7's Landlock downloader sandbox cannot initialize under Docker's
  # default seccomp profile. The smoke itself is already container-isolated.
  pacman_conf=/etc/pacman.conf
  sandbox_option=DisableSandbox
  options_headers="$(awk '$0 ~ /^\[options\][[:space:]]*$/ { count++ } END { print count + 0 }' "${pacman_conf}")"
  if [[ "${options_headers}" == 0 ]]; then
    printf 'Expected at least one [options] section in pacman.conf\n' >&2
    return 64
  fi
  sandbox_entries="$(awk -v option="${sandbox_option}" '$1 == option { count++ } END { print count + 0 }' "${pacman_conf}")"
  if [[ "${sandbox_entries}" == 0 ]]; then
    first_options_line="$(awk '$0 ~ /^\[options\][[:space:]]*$/ { print NR; exit }' "${pacman_conf}")"
    sed -i "${first_options_line}a ${sandbox_option}" "${pacman_conf}"
  elif [[ "${sandbox_entries}" != 1 ]]; then
    printf 'pacman.conf contains duplicate sandbox settings\n' >&2
    return 64
  fi
  if ! awk -v option="${sandbox_option}" '
    /^\[options\][[:space:]]*$/ { in_options = 1; next }
    /^\[/ { in_options = 0 }
    in_options && $1 == option { count++ }
    END { exit count == 1 ? 0 : 1 }
  ' "${pacman_conf}"; then
    printf 'Sandbox setting is not present exactly once under [options]\n' >&2
    return 64
  fi
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
  if [[ "${candidate_version}" =~ PostgreSQL[^0-9]*${postgres_major}([^0-9]|$) ]]; then
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
shared_preload_libraries = 'bioseq'
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
[[ "${preload}" == "bioseq" ]] \
  || die "bioseq is not the sole preload entry: ${preload}"
defaults="$(run_pg "${psql_command[@]}" -Atqc "SELECT
  current_setting('bioseq.aligner.minimizer.algorithm') = 'bounded_nthash_simd'
  AND current_setting('bioseq.aligner.chaining.algorithm') = 'simd_minimap2'
  AND current_setting('bioseq.aligner.base_alignment.algorithm') = 'block_aligner'
  AND current_setting('bioseq.column_cache.enabled') = 'on'
  AND current_setting('bioseq.vectorization.enabled') = 'on'
  AND current_setting('bioseq.scheduler.pipeline.mode') = 'task_parallel'
  AND current_setting('bioseq.aligner.grouped_soa_generation.enabled') = 'on'
  AND current_setting('bioseq.aligner.fragment_minimizer.lookup_route') = 'direct_compact_hash_lookup'
  AND current_setting('bioseq.index.insert_maintenance.enabled') = 'off'
  AND current_setting('bioseq.index.async_persistence.enabled') = 'off'
  AND current_setting('bioseq.index.recompute_async_persistence.enabled') = 'off'
  AND current_setting('bioseq_cache.query_cache.mode') = 'disabled'")"
[[ "${defaults}" == "t" ]] || die "package defaults or cache autoload do not match E2E policy"
# A briefly enabled setting proves autoload happened during preload and
# registered shared memory, rather than merely creating a placeholder GUC.
run_pg "${psql_command[@]}" -c \
  "SET bioseq_cache.query_cache.mode = 'exact_query'; RESET bioseq_cache.query_cache.mode;"
# Cover the PostgreSQL planner ABI used by the companion: generic-plan
# partition pruning, SubqueryScan and both SetOp inputs. Repeat the prepared
# request after an INSERT so a stale cache result also fails the package check.
run_pg "${psql_command[@]}" <<'SQL'
SET statement_timeout = '30s';
CREATE TABLE bioseq_package_cache_probe (id integer) PARTITION BY RANGE (id);
CREATE TABLE bioseq_package_cache_probe_low PARTITION OF bioseq_package_cache_probe FOR VALUES FROM (0) TO (10);
CREATE TABLE bioseq_package_cache_probe_high PARTITION OF bioseq_package_cache_probe FOR VALUES FROM (10) TO (20);
INSERT INTO bioseq_package_cache_probe VALUES (1), (11), (12);
SET bioseq_cache.query_cache.mode = 'exact_query';
SET plan_cache_mode = force_generic_plan;
PREPARE bioseq_package_cache_probe_query(integer) AS
SELECT sum(id) AS cache_sum FROM (
  SELECT id FROM (SELECT id FROM bioseq_package_cache_probe WHERE id > $1 OFFSET 0) AS selected
  INTERSECT
  SELECT id FROM bioseq_package_cache_probe WHERE id >= 11
) AS matched;
EXPLAIN (COSTS OFF) EXECUTE bioseq_package_cache_probe_query(10);
EXECUTE bioseq_package_cache_probe_query(10) \gset
SELECT 1 / (:cache_sum = 23)::integer AS cache_initial_result_valid;
EXECUTE bioseq_package_cache_probe_query(10) \gset
SELECT 1 / (:cache_sum = 23)::integer AS cache_repeated_result_valid;
INSERT INTO bioseq_package_cache_probe VALUES (13);
EXECUTE bioseq_package_cache_probe_query(10) \gset
SELECT 1 / (:cache_sum = 36)::integer AS cache_changed_result_valid;
DEALLOCATE bioseq_package_cache_probe_query;
RESET bioseq_cache.query_cache.mode;
RESET plan_cache_mode;
DROP TABLE bioseq_package_cache_probe;
SQL
extension_count="$(run_pg "${psql_command[@]}" -Atqc \
  "SELECT count(*) FROM pg_extension WHERE extname IN ('bioseq','bioseq_cache')")"
[[ "${extension_count}" == "2" ]] || die "both extensions were not created"
roundtrip="$(run_pg "${psql_command[@]}" -Atqc \
  "SELECT 'ACGT'::bioseq.seq('dna4', 8)::text")"
[[ -n "${roundtrip}" && "${roundtrip}" == "ACGT" ]] \
  || die "nonempty Bioseq roundtrip failed: ${roundtrip:-<empty>}"
roundtrip_dna5="$(run_pg "${psql_command[@]}" -Atqc \
  "SELECT 'ACGTN'::bioseq.seq('dna5', 8)::text")"
[[ "${roundtrip_dna5}" == "ACGTN" ]] || die "DNA5 package roundtrip failed"

# Exercise the default combination, not just each GUC in isolation. Each SQL
# statement commits independently: grouped publication reopens committed rows.
# A later INSERT must remain searchable without silently losing the new row.
run_pg "${psql_command[@]}" <<'SQL'
SET statement_timeout = '60s';
SELECT string_agg(translate(md5(i::text), '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS smoke_query
FROM generate_series(1, 32) AS i \gset
CREATE TABLE bioseq_package_smoke (id integer PRIMARY KEY, seq bioseq.seq('dna4', 64)) USING bioseq;
INSERT INTO bioseq_package_smoke VALUES (1, :'smoke_query'::bioseq.seq('dna4', 64));
INSERT INTO bioseq_package_smoke VALUES (2, repeat('A', 1024)::bioseq.seq('dna4', 64));
CREATE TEMP TABLE bioseq_package_observed AS
SELECT id FROM bioseq_package_smoke WHERE public.mmsearch(seq, :'smoke_query'::bioseq.seq('dna4', 64));
DO $$
DECLARE found bigint;
BEGIN
  SELECT count(*) INTO found FROM bioseq_package_observed;
  IF found <> 1 THEN RAISE EXCEPTION 'default transient search: expected 1 match, got %', found; END IF;
END $$;
DROP TABLE bioseq_package_observed;
SELECT bioseq.update_index('bioseq_package_smoke'::regclass, 'minimizers_per_fragment');
SELECT bioseq.set_active_read_index('bioseq_package_smoke'::regclass, 'minimizers_per_fragment');
-- Native persisted readers are retained by BioseqBatchJoin. An ordinary
-- column scan deliberately rejects index_read_requirement=2.
CREATE TABLE bioseq_package_queries (id integer PRIMARY KEY, seq bioseq.seq('dna4', 64)) USING bioseq;
INSERT INTO bioseq_package_queries SELECT id, seq FROM bioseq_package_smoke WHERE id = 1;
CREATE TEMP TABLE bioseq_package_observed AS
SELECT t.id FROM bioseq_package_queries AS q CROSS JOIN bioseq_package_smoke AS t
WHERE public.mmsearch(t.seq, q.seq, aligner_options(index_read_requirement => 2));
DO $$
DECLARE found bigint;
BEGIN
  SELECT count(*) INTO found FROM bioseq_package_observed;
  IF found <> 1 THEN RAISE EXCEPTION 'default persisted batch search: expected 1 match, got %', found; END IF;
END $$;
DROP TABLE bioseq_package_observed;
DROP TABLE bioseq_package_queries;
INSERT INTO bioseq_package_smoke SELECT 3, seq FROM bioseq_package_smoke WHERE id = 1;
CREATE TEMP TABLE bioseq_package_observed AS
SELECT id FROM bioseq_package_smoke WHERE public.mmsearch(seq, :'smoke_query'::bioseq.seq('dna4', 64));
DO $$
DECLARE found bigint;
BEGIN
  SELECT count(*) INTO found FROM bioseq_package_observed;
  IF found <> 2 THEN RAISE EXCEPTION 'search after INSERT: expected 2 matches, got %', found; END IF;
END $$;
DROP TABLE bioseq_package_observed;
DROP TABLE bioseq_package_smoke;
SQL

# PG19 changed TableAM mutation/maintenance signatures; PG20 additionally moved
# index iteration into the table AM. Exercise those interfaces in the installed
# extension, without adding benchmark runs or rebuilding accepted PG17 packages.
if (( postgres_major >= 19 )); then
  run_pg "${psql_command[@]}" <<'SQL'
SET statement_timeout = '60s';
CREATE TABLE bioseq_package_tableam_probe (id integer PRIMARY KEY, value integer) USING bioseq;
INSERT INTO bioseq_package_tableam_probe VALUES (1, 10), (2, 20), (3, 30);
ALTER TABLE bioseq_package_tableam_probe ADD COLUMN wide_value bigint DEFAULT 1000000000000;
EXPLAIN (ANALYZE, COSTS OFF)
SELECT avg(value), avg(wide_value) FROM bioseq_package_tableam_probe;
SELECT avg(value) AS integer_average, avg(wide_value) AS bigint_average
FROM bioseq_package_tableam_probe \gset
SELECT 1 / (:'integer_average'::numeric = 20 AND :'bigint_average'::numeric = 1000000000000)::integer
AS numeric_average_valid;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) SELECT value FROM bioseq_package_tableam_probe WHERE id = 2;
SELECT value AS indexed_value FROM bioseq_package_tableam_probe WHERE id = 2 \gset
SELECT 1 / (:indexed_value = 20)::integer AS index_fetch_valid;
SET enable_indexonlyscan = on;
EXPLAIN (COSTS OFF) SELECT id FROM bioseq_package_tableam_probe WHERE id = 2;
DO $$
DECLARE observed_plan json;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON, COSTS OFF) SELECT id FROM bioseq_package_tableam_probe WHERE id = 2'
    INTO observed_plan;
  IF observed_plan::text LIKE '%"Index Only Scan"%' THEN
    RAISE EXCEPTION 'columnar relation-info hook allowed an unsupported index-only path: %', observed_plan;
  END IF;
END $$;
SELECT id AS indexed_id FROM bioseq_package_tableam_probe WHERE id = 2 \gset
SELECT 1 / (:indexed_id = 2)::integer AS projected_index_result_valid;
DO $$
BEGIN
  INSERT INTO bioseq_package_tableam_probe (id, value) VALUES (2, 999);
  RAISE EXCEPTION 'duplicate primary key was accepted';
EXCEPTION WHEN unique_violation THEN NULL;
END $$;
UPDATE bioseq_package_tableam_probe SET value = 21 WHERE id = 2;
DELETE FROM bioseq_package_tableam_probe WHERE id = 1;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexonlyscan;
VACUUM (FULL, ANALYZE) bioseq_package_tableam_probe;
SELECT count(*) AS row_count, sum(value) AS value_sum FROM bioseq_package_tableam_probe \gset
SELECT 1 / (:row_count = 2 AND :value_sum = 51)::integer AS rewritten_rows_valid;
DROP TABLE bioseq_package_tableam_probe;
-- PG19 replaced CopyFormatOptions.binary with an explicit format enum.
-- Binary input still carries its alphabet; untyped text input must fail before
-- PostgreSQL attempts to open the deliberately nonexistent input file.
CREATE TABLE bioseq_package_letter_copy (value bioseq.letter);
COPY (SELECT 'A'::bioseq.letter('dna4')) TO '/tmp/bioseq-package-letter.bin' (FORMAT binary);
COPY bioseq_package_letter_copy FROM '/tmp/bioseq-package-letter.bin' (FORMAT binary);
SELECT count(*) = 1 AND min(value::text) = 'A' AS binary_letter_valid
FROM bioseq_package_letter_copy \gset
SELECT 1 / (:'binary_letter_valid'::boolean)::integer AS binary_letter_copy_valid;
DO $$
BEGIN
  COPY bioseq_package_letter_copy FROM '/tmp/bioseq-package-missing-letter.txt' (FORMAT text);
  RAISE EXCEPTION 'typmodless letter text COPY was accepted';
EXCEPTION WHEN invalid_parameter_value THEN NULL;
END $$;
DROP TABLE bioseq_package_letter_copy;
SQL
fi

# Users can also load Bioseq without a server-wide preload. The companion must
# still load automatically in that backend and remain disabled, without
# claiming shared-memory query caching is available before a server restart.
run_pg "${pg_ctl}" -D "${data_dir}" -m fast -w stop
started=false
printf "\nshared_preload_libraries = ''\n" >> "${data_dir}/postgresql.conf"
run_pg "${pg_ctl}" -D "${data_dir}" -l "${log_file}" -w start
started=true
backend_load="$(run_pg "${psql_command[@]}" -Atqc \
  "LOAD 'bioseq'; SELECT 'ACGTN'::bioseq.seq('dna5', 8)::text = 'ACGTN'
   AND current_setting('bioseq_cache.query_cache.mode') = 'disabled'
   AND current_setting('shared_preload_libraries') = ''")"
[[ "${backend_load}" == "t" ]] || die "backend-only companion autoload failed"

printf '{"passed":true,"package_installed":true,"preload_active":true,"bioseq_extension_created":true,"bioseq_cache_extension_created":true,"nonempty_roundtrip":true,"roundtrip":"ACGT","dna5_roundtrip":true,"release_defaults":true,"default_insert_search":true,"default_persisted_search":true,"search_after_insert":true,"backend_only_cache_autoload":true,"distro":"%s","architecture":"%s","postgres_major":%s}\n' \
  "${distro}" "${requested_arch}" "${postgres_major}" >&3
