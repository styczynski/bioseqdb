#!/usr/bin/env python3
"""Check provider read grants in an extension-backed disposable example clone.

No provider request is made: constant-false scans still check PostgreSQL ACLs.
The fixture is a copy of the website template, leaving that template untouched.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--pg-bin-dir", type=Path, required=True)
    parser.add_argument("--sql-manifest", action="append", required=True)
    args = parser.parse_args()
    host, port, user, database = (
        os.environ[key] for key in ("PGHOST", "PGPORT", "PGUSER", "PGDATABASE")
    )
    fixture = "website_external_acl_template"

    def identifier(value: str) -> str:
        return '"' + value.replace('"', '""') + '"'

    def psql(db: str, sql: str) -> str:
        completed = subprocess.run(
            [str(args.pg_bin_dir / "psql"), "-XAtq", "-v", "ON_ERROR_STOP=1",
             "--host", host, "--port", port, "--username", user,
             "--dbname", db, "-c", sql],
            capture_output=True, text=True, timeout=30,
        )
        if completed.returncode != 0:
            raise RuntimeError(f"ACL fixture SQL failed: {completed.stderr.strip()}")
        return completed.stdout.strip()

    if psql(database, "SELECT to_regnamespace('bioseq_external') IS NOT NULL") != "t":
        print("ok - external example ACL check skipped: provider component not installed")
        return

    snapshot_sql = """
        SELECT nspacl::text FROM pg_namespace WHERE nspname = 'bioseq_external';
        SELECT relname || ':' || coalesce(relacl::text, '')
        FROM pg_class WHERE relnamespace = 'bioseq_external'::regnamespace
        ORDER BY relname;
    """
    original_acl = psql(database, snapshot_sql)
    # External TAM relations intentionally have no physical relation file;
    # match the isolated executor's directory-copy strategy. Before PostgreSQL
    # 15 this is the default and the STRATEGY option is not available.
    strategy = " STRATEGY FILE_COPY" if int(psql(database, "SHOW server_version_num")) >= 150000 else ""
    psql("postgres", f"CREATE DATABASE {fixture} TEMPLATE {identifier(database)}{strategy}")
    try:
        psql(fixture, "CREATE TABLE bioseq_external.website_private_probe(value integer)")
        fixture_acl = psql(fixture, snapshot_sql)
        # Check every supported table without downloading external records.
        reads = "\n".join(
            f"PERFORM 1 FROM bioseq_external.{table} WHERE false;"
            for table in ("taxonomy", "catalog", "record_genes", "record_locations", "sequences")
        )
        denied_commands = (
            "CREATE TABLE bioseq_external.website_denied_probe(value integer)",
            "INSERT INTO bioseq_external.catalog(provider, domain, entity_kind, "
            "original_identifier, accession, retrievable, provenance, retrieved_at) "
            "SELECT 'NCBI', 'genome', 'assembly', 'probe', 'probe', true, '{}'::jsonb, now() WHERE false",
            "UPDATE bioseq_external.catalog SET title = 'probe' WHERE false",
            "DELETE FROM bioseq_external.catalog WHERE false",
            "TRUNCATE bioseq_external.catalog",
            "ALTER TABLE bioseq_external.catalog ADD COLUMN website_probe integer",
            "SELECT * FROM bioseq_external.website_private_probe WHERE false",
        )
        denials = []
        for command in denied_commands:
            quoted = "'" + command.replace("'", "''") + "'"
            denials.append(
                "BEGIN\n"
                f"  EXECUTE {quoted};\n"
                "  RAISE EXCEPTION 'forbidden external operation was allowed';\n"
                "EXCEPTION WHEN insufficient_privilege THEN NULL;\n"
                "END;"
            )
        visible = (
            "DO $acl$ BEGIN\n" + reads + "\n" + "\n".join(denials)
            + "\nEND $acl$;\nSELECT 'external_acl_verified' AS result;"
        )
        command = [
            args.generator, "website", "execute-sql", "--isolated",
            "--admin-conninfo", f"host={host} port={port} dbname=postgres user={user}",
            "--socket-dir", host, "--port", port, "--template-db", fixture,
            "--visible", visible,
        ]
        for manifest in args.sql_manifest:
            command.extend(("--sql-manifest", manifest))
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if result.returncode != 0 or "external_acl_verified" not in result.stdout:
            raise RuntimeError(
                f"external example ACL integration failed (rc={result.returncode}):\n"
                + result.stdout + result.stderr
            )
        psql("postgres", f"ALTER DATABASE {fixture} ALLOW_CONNECTIONS true")
        if psql(fixture, snapshot_sql) != fixture_acl:
            raise RuntimeError("example grants changed the fixture template ACLs")
        if psql(database, snapshot_sql) != original_acl:
            raise RuntimeError("example grants changed the website template ACLs")
        print("ok - external examples can read all five provider tables")
        print("ok - external examples cannot write, alter, create, or read private tables")
        print("ok - extension and template ACLs remain unchanged")
    finally:
        psql("postgres", f"ALTER DATABASE {fixture} IS_TEMPLATE false")
        psql("postgres", f"DROP DATABASE {fixture} WITH (FORCE)")


if __name__ == "__main__":
    main()
