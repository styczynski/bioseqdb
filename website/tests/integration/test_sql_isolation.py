#!/usr/bin/env python3
"""Exercise the website SQL executor in a disposable PostgreSQL cluster.

The checks cover ordered visible results, retained errors, PostgreSQL type names,
restricted clone roles, and cleanup after ordinary errors or a dead backend. The
Bioseq extension is not needed for these executor-level checks.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

HOST = os.environ["PGHOST"]
PORT = os.environ["PGPORT"]
USER = os.environ["PGUSER"]
TEMPLATE = "wtmpl"

_failures = 0


def _report(name: str, ok: bool, detail: str = "") -> None:
    global _failures
    marker = "ok" if ok else "not ok"
    if not ok:
        _failures += 1
    print(f"{marker} - {name}{': ' + detail if detail else ''}")


def _admin_conninfo() -> str:
    return f"host={HOST} port={PORT} dbname=postgres user={USER} sslmode=disable"


def _direct_conninfo(database: str) -> str:
    return f"host={HOST} port={PORT} dbname={database} user={USER} sslmode=disable"


def _run_generator(generator: str, args: list[str]) -> subprocess.CompletedProcess:
    # This focused harness deliberately clones a plain PostgreSQL template. Real
    # website execution requires and verifies the staged bioseq extension ACLs.
    if "--isolated" in args:
        args = [*args, "--allow-plain-template"]
    return subprocess.run(
        [generator, "website", "execute-sql", *args],
        capture_output=True, text=True, timeout=120)


def _psql(pg_bin: Path, database: str, sql: str, user: str = USER) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(pg_bin / "psql"), "-tAqX", "--host", HOST, "--port", PORT,
         "--username", user, "--dbname", database, "-c", sql],
        capture_output=True, text=True, timeout=60)


def _scalar_int(result: subprocess.CompletedProcess, operation: str) -> int:
    if result.returncode != 0:
        raise RuntimeError(
            f"{operation} failed with rc={result.returncode}: {result.stderr.strip()}")
    value = result.stdout.strip()
    try:
        return int(value)
    except ValueError as error:
        raise RuntimeError(
            f"{operation} returned a non-integer scalar: {value!r}") from error


def _leftover_counts(pg_bin: Path) -> tuple[int, int]:
    databases = _psql(pg_bin, "postgres",
                      "SELECT count(*) FROM pg_database WHERE datname LIKE 'biosite_d\\_%'")
    roles = _psql(pg_bin, "postgres",
                  "SELECT count(*) FROM pg_roles "
                  "WHERE rolname LIKE 'biosite_r\\_%' OR rolname LIKE 'biosite_p\\_%'")
    return (_scalar_int(databases, "leftover database count"),
            _scalar_int(roles, "leftover role count"))


def _prepared_count(pg_bin: Path) -> int:
    prepared = _psql(
        pg_bin, "postgres",
        "SELECT count(*) FROM pg_prepared_xacts "
        "WHERE left(database::text, 10) = 'biosite_d_'")
    return _scalar_int(prepared, "leftover prepared-transaction count")


def check_direct_multi_result(generator: str) -> None:
    """A direct (non-isolated) multi-statement visible phase yields two results."""
    result = _run_generator(generator, [
        "--conninfo", _direct_conninfo(os.environ["PGDATABASE"]),
        "--visible", "SELECT 1; SELECT 2, 3"])
    row_lines = [line for line in result.stdout.splitlines() if line.endswith("rows)")]
    ok = (result.returncode == 0 and len(row_lines) == 2
          and "1" in result.stdout and "2 | 3" in result.stdout)
    _report("Direct execution renders two result sets", ok,
            f"rc={result.returncode} results={len(row_lines)}\n{result.stdout.strip()}")


def check_direct_cleanup_on_error(generator: str, pg_bin: Path) -> None:
    """Direct teardown clears tutorial state after visible SQL fails."""
    table = "website_direct_cleanup_probe"
    temp_table = "website_direct_cleanup_temp"
    prepared = "website_direct_cleanup_plan"
    audit = "website_direct_cleanup_audit"
    database = os.environ["PGDATABASE"]
    _psql(
        pg_bin,
        database,
        f"DROP TABLE IF EXISTS {table}; DROP TABLE IF EXISTS {audit}; "
        f"CREATE TABLE {audit}(persistent_removed boolean, "
        "temp_removed boolean, prepared_removed boolean, setting_reset boolean)",
    )
    result = _run_generator(generator, [
        "--conninfo", _direct_conninfo(database),
        "--setup", f"CREATE TABLE {table}(id integer); "
                   f"CREATE TEMP TABLE {temp_table}(id integer); "
                   "SET application_name = 'website_direct_cleanup'; "
                   f"PREPARE {prepared} AS SELECT 1",
        "--visible", "SELECT * FROM website_missing_relation",
        "--teardown", f"DROP TABLE IF EXISTS {table}; "
                      f"DROP TABLE IF EXISTS {temp_table}; "
                      f"DEALLOCATE {prepared}; "
                      "RESET application_name; "
                      f"INSERT INTO {audit} "
                      "SELECT "
                      f"to_regclass('{table}') IS NULL, "
                      f"to_regclass('pg_temp.{temp_table}') IS NULL, "
                      "NOT EXISTS (SELECT 1 FROM pg_prepared_statements "
                      f"WHERE name = '{prepared}'), "
                      "current_setting('application_name') "
                      "<> 'website_direct_cleanup'",
    ])
    cleanup_state = _psql(
        pg_bin, database,
        f"SELECT persistent_removed, temp_removed, prepared_removed, setting_reset "
        f"FROM {audit}",
    )
    clean = (
        cleanup_state.returncode == 0
        and cleanup_state.stdout.strip() == "t|t|t|t"
    )
    _report(
        "Direct cleanup: visible failure clears objects and session state",
        result.returncode == 3 and clean,
        f"rc={result.returncode} cleanup={cleanup_state.stdout.strip()!r}\n"
        f"stderr:\n{result.stderr.strip()}",
    )
    _psql(
        pg_bin, database,
        f"DROP TABLE IF EXISTS {table}; DROP TABLE IF EXISTS {audit}",
    )


def check_direct_cleanup_on_setup_error(generator: str, pg_bin: Path) -> None:
    """Direct teardown runs when setup SQL fails."""
    audit = "website_direct_setup_failure_audit"
    database = os.environ["PGDATABASE"]
    _psql(
        pg_bin, database,
        f"DROP TABLE IF EXISTS {audit}; CREATE TABLE {audit}(teardown_ran boolean)",
    )
    result = _run_generator(generator, [
        "--conninfo", _direct_conninfo(database),
        "--setup", "SELECT * FROM website_missing_setup_relation",
        "--visible", "SELECT 1 AS must_not_run",
        "--teardown", f"INSERT INTO {audit} VALUES (true)",
    ])
    teardown_state = _psql(
        pg_bin, database,
        f"SELECT teardown_ran FROM {audit}",
    )
    clean = teardown_state.returncode == 0 and teardown_state.stdout.strip() == "t"
    _report(
        "Direct cleanup: setup failure still executes teardown",
        result.returncode == 3 and clean and "must_not_run" not in result.stdout,
        f"rc={result.returncode} teardown={teardown_state.stdout.strip()!r}\n"
        f"stderr:\n{result.stderr.strip()}",
    )
    _psql(pg_bin, database, f"DROP TABLE IF EXISTS {audit}")


def check_direct_teardown_failure(generator: str) -> None:
    """A direct teardown error fails an otherwise successful example."""
    result = _run_generator(generator, [
        "--conninfo", _direct_conninfo(os.environ["PGDATABASE"]),
        "--visible", "SELECT 1 AS visible_result",
        "--teardown", "SELECT * FROM website_missing_teardown_relation",
    ])
    reported = (
        result.returncode == 3
        and "tutorial teardown failed" in result.stderr
        and "website_missing_teardown_relation" in result.stderr
    )
    _report(
        "Direct cleanup: teardown failures are reported",
        reported,
        f"rc={result.returncode}\nstderr:\n{result.stderr.strip()}",
    )


def check_isolated_multi_result(generator: str) -> None:
    """An isolated multi-statement visible phase yields two results."""
    result = _run_generator(generator, [
        "--isolated", "--admin-conninfo", _admin_conninfo(),
        "--socket-dir", HOST, "--port", PORT, "--template-db", TEMPLATE,
        "--setup", "CREATE TABLE t(id int); INSERT INTO t VALUES (10),(20)",
        "--visible", "SELECT id FROM t ORDER BY id; SELECT count(*) AS n FROM t",
        "--teardown", "DROP TABLE t"])
    row_lines = [line for line in result.stdout.splitlines() if line.endswith("rows)")]
    ok = (result.returncode == 0 and len(row_lines) == 2
          and "10" in result.stdout and "20" in result.stdout and "2" in result.stdout)
    _report("Isolated execution renders two result sets and passes isolation checks", ok,
            f"rc={result.returncode}\nstdout:\n{result.stdout.strip()}\n"
            f"stderr:\n{result.stderr.strip()}")


def check_preflight_failure_skips_teardown(generator: str) -> None:
    """A missing-manifest preflight never executes author teardown SQL."""
    started = time.monotonic()
    result = subprocess.run(
        [generator, "website", "execute-sql",
         "--isolated", "--admin-conninfo", _admin_conninfo(),
         "--socket-dir", HOST, "--port", PORT, "--template-db", TEMPLATE,
         "--visible", "SELECT 1",
         "--teardown", "SELECT pg_sleep(10)"],
        capture_output=True, text=True, timeout=30)
    elapsed = time.monotonic() - started
    ok = (result.returncode == 3
          and "extension-backed website isolation requires --sql-manifest "
              "for ACL target discovery" in result.stderr
          and elapsed < 5.0)
    _report("Manifest preflight failure skips author teardown", ok,
            f"rc={result.returncode} elapsed={elapsed:.2f}s\n"
            f"stderr:\n{result.stderr.strip()}")


def check_error_retention(generator: str) -> None:
    """expect=error retains the success before the failure, then the error."""
    result = _run_generator(generator, [
        "--isolated", "--admin-conninfo", _admin_conninfo(),
        "--socket-dir", HOST, "--port", PORT, "--template-db", TEMPLATE,
        "--visible", "SELECT 7; SELECT 1 / 0",
        "--expect-sqlstate", "22012"])
    ok = (result.returncode == 0
          and "7" in result.stdout
          and "ERROR SQLSTATE=22012" in result.stdout)
    _report("Error retention renders the completed result before the error",
            ok, f"rc={result.returncode}\n{result.stdout.strip()}")


def check_result_metadata(generator: str) -> None:
    """Clone-local type names and PQcmdTuples survive projection and rendering."""
    result = _run_generator(generator, [
        "--isolated", "--admin-conninfo", _admin_conninfo(),
        "--socket-dir", HOST, "--port", PORT, "--template-db", TEMPLATE,
        # PostgreSQL's RowDescription protocol reports the base type for a domain
        # column, so use a clone-local enum whose OID remains visible to PQftype.
        "--setup", "CREATE TYPE local_positive AS ENUM ('3', '4'); "
                   "CREATE TABLE typed_result(value local_positive)",
        "--visible", "INSERT INTO typed_result VALUES ('3'),('4'); "
                     "SELECT value FROM typed_result ORDER BY value",
        "--teardown", "DROP TABLE typed_result; DROP TYPE local_positive"])
    ok = (result.returncode == 0
          and "INSERT 0 2 (2 rows affected)" in result.stdout
          and "value [local_positive]" in result.stdout)
    _report("Clone-local type names and affected-row counts are rendered", ok,
            f"rc={result.returncode}\nstdout:\n{result.stdout.strip()}\n"
            f"stderr:\n{result.stderr.strip()}")


def check_prepared_transaction_cleanup(generator: str, pg_bin: Path) -> None:
    """A committed 2PC state is rolled back before clone/role teardown."""
    result = _run_generator(generator, [
        "--isolated", "--admin-conninfo", _admin_conninfo(),
        "--socket-dir", HOST, "--port", PORT, "--template-db", TEMPLATE,
        "--visible", "BEGIN; SELECT 41 AS before_prepare; "
                     "PREPARE TRANSACTION 'biosite_prepared_''quote'; "
                     "SELECT 42 AS after_prepare"])
    databases, roles = _leftover_counts(pg_bin)
    prepared = _prepared_count(pg_bin)
    ok = (result.returncode == 0
          and "41" in result.stdout and "42" in result.stdout
          and databases == 0 and roles == 0 and prepared == 0)
    _report("Prepared transaction cleanup leaves no clone, role, or transaction",
            ok, f"rc={result.returncode} leftover_dbs={databases} "
                f"leftover_roles={roles} prepared={prepared}\n"
                f"stdout:\n{result.stdout.strip()}\n"
                f"stderr:\n{result.stderr.strip()}")


def check_cleanup_on_error(generator: str, pg_bin: Path) -> None:
    """A failing isolated example still leaves 0 leftover databases/roles."""
    result = _run_generator(generator, [
        "--isolated", "--admin-conninfo", _admin_conninfo(),
        "--socket-dir", HOST, "--port", PORT, "--template-db", TEMPLATE,
        "--setup", "CREATE TABLE t(id int)",
        "--visible", "SELECT 1; SELECT * FROM this_table_does_not_exist",
        "--teardown", "DROP TABLE t"])
    databases, roles = _leftover_counts(pg_bin)
    ok = result.returncode == 3 and databases == 0 and roles == 0
    _report("Failed isolated example leaves no clone or role", ok,
            f"rc={result.returncode} leftover_dbs={databases} leftover_roles={roles}\n"
            f"stderr:\n{result.stderr.strip()}")


def check_teardown_recovery(generator: str, pg_bin: Path) -> None:
    """An example that kills its own backend still cleans up (recovery path)."""
    result = _run_generator(generator, [
        "--isolated", "--admin-conninfo", _admin_conninfo(),
        "--socket-dir", HOST, "--port", PORT, "--template-db", TEMPLATE,
        "--setup", "CREATE TABLE t(id int)",
        "--visible", "SELECT pg_terminate_backend(pg_backend_pid())",
        "--teardown", "DROP TABLE t"])
    databases, roles = _leftover_counts(pg_bin)
    ok = result.returncode != 0 and databases == 0 and roles == 0
    _report("Backend failure recovery leaves no clone or role",
            ok, f"rc={result.returncode} leftover_dbs={databases} leftover_roles={roles}")


def check_acl_transcript(pg_bin: Path) -> None:
    """Print the direct PostgreSQL transcript for the role restrictions."""
    print("\n--- direct PostgreSQL role restriction transcript ---")
    role, clone, peer = "biosite_r_probe", "biosite_d_probe", "biosite_p_probe"
    # CREATE DATABASE cannot run inside a transaction block, so each administrative
    # statement is issued as its own autocommit psql command (exactly as the
    # executor issues them over separate nonblocking round-trips).
    for stmt in [
        f"REVOKE CONNECT ON DATABASE {TEMPLATE} FROM PUBLIC",
        f"ALTER DATABASE {TEMPLATE} IS_TEMPLATE true",
        f"ALTER DATABASE {TEMPLATE} ALLOW_CONNECTIONS false",
        "REVOKE CONNECT ON DATABASE postgres FROM PUBLIC",
        "REVOKE CONNECT ON DATABASE template0 FROM PUBLIC",
        "REVOKE CONNECT ON DATABASE template1 FROM PUBLIC",
        f"CREATE ROLE {role} LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE",
        f"CREATE DATABASE {clone} TEMPLATE {TEMPLATE} OWNER {role}",
        f"REVOKE CONNECT ON DATABASE {clone} FROM PUBLIC",
        f"GRANT CONNECT ON DATABASE {clone} TO {role}",
        f"CREATE ROLE {peer} LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE",
    ]:
        prep = _psql(pg_bin, "postgres", stmt)
        if prep.returncode != 0:
            _report("ACL transcript setup", False, f"{stmt}: {prep.stderr.strip()}")
            return

    datacl = _psql(pg_bin, "postgres",
                   f"SELECT datacl FROM pg_database WHERE datname = '{clone}'")
    print(f"clone datacl = {datacl.stdout.strip()}")

    own = _psql(pg_bin, clone, "SELECT current_user, current_database()", user=role)
    _report("role CAN connect to its own clone", own.returncode == 0, own.stdout.strip())

    for target, label in [("postgres", "postgres"),
                          ("template0", "template0"),
                          ("template1", "template1"),
                          (TEMPLATE, "the template")]:
        probe = _psql(pg_bin, target, "SELECT 1", user=role)
        rejected = probe.returncode != 0
        _report(f"role is REJECTED connecting to {label}", rejected,
                probe.stderr.strip().splitlines()[-1] if probe.stderr.strip() else "")

    peer_probe = _psql(pg_bin, clone, "SELECT 1", user=peer)
    _report("foreign role is REJECTED connecting to the clone",
            peer_probe.returncode != 0,
            peer_probe.stderr.strip().splitlines()[-1] if peer_probe.stderr.strip() else "")

    for stmt, label in [("CREATE ROLE biosite_escalate LOGIN", "CREATE ROLE"),
                        ("CREATE DATABASE biosite_escalate", "CREATE DATABASE")]:
        denied = _psql(pg_bin, clone, stmt, user=role)
        _report(f"role CANNOT {label}", denied.returncode != 0,
                denied.stderr.strip().splitlines()[-1] if denied.stderr.strip() else "")

    _psql(pg_bin, "postgres", f"DROP DATABASE IF EXISTS {clone} WITH (FORCE)")
    _psql(pg_bin, "postgres", f"DROP ROLE IF EXISTS {role}")
    _psql(pg_bin, "postgres", f"DROP ROLE IF EXISTS {peer}")
    _psql(pg_bin, "postgres", f"ALTER DATABASE {TEMPLATE} ALLOW_CONNECTIONS true")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--pg-bin-dir", required=True)
    args = parser.parse_args()
    pg_bin = Path(args.pg_bin_dir).expanduser().resolve()

    created = _psql(pg_bin, "postgres", f"CREATE DATABASE {TEMPLATE}")
    if created.returncode != 0 and "already exists" not in created.stderr:
        print(f"failed to create template database: {created.stderr}", file=sys.stderr)
        return 2

    check_direct_multi_result(args.generator)
    check_direct_cleanup_on_error(args.generator, pg_bin)
    check_direct_cleanup_on_setup_error(args.generator, pg_bin)
    check_direct_teardown_failure(args.generator)
    check_isolated_multi_result(args.generator)
    check_preflight_failure_skips_teardown(args.generator)
    check_error_retention(args.generator)
    check_result_metadata(args.generator)
    check_prepared_transaction_cleanup(args.generator, pg_bin)
    check_cleanup_on_error(args.generator, pg_bin)
    check_teardown_recovery(args.generator, pg_bin)
    check_acl_transcript(pg_bin)

    databases, roles = _leftover_counts(pg_bin)
    prepared = _prepared_count(pg_bin)
    _report("Final cleanup leaves no clone, role, or prepared transaction",
            databases == 0 and roles == 0 and prepared == 0,
            f"leftover_dbs={databases} leftover_roles={roles} prepared={prepared}")

    print(f"\n{'PASS' if _failures == 0 else 'FAIL'}: "
          f"{_failures} failing check(s)")
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())
