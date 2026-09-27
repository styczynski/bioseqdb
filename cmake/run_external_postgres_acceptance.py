#!/usr/bin/env python3
"""Fail-closed live PostgreSQL acceptance for the external SQL profile.

The runner stages only the current build's generated install SQL, control file,
and DSO into a disposable PostgreSQL 17 tree.  It then creates the extension in
an unusual schema and records exact catalog, ACL, read-only, storage-free, and
DDL lifecycle evidence in a hash-sealed receipt.
"""

from __future__ import annotations

import argparse
import copy
import csv
import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import uuid
from typing import Any, Mapping, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

from private_postgres import (  # noqa: E402
    ClusterInputs,
    UniquePrivateCluster,
)


class ExternalAcceptanceError(RuntimeError):
    """The external-enabled package or its live evidence is incomplete."""


RECEIPT_SCHEMA = "bioseq.external_postgres.acceptance_receipt"
RECEIPT_SCHEMA_REVISION = 1
STAGE_MANIFEST_SCHEMA = "bioseq.private_postgres.stage_manifest"
STAGE_MANIFEST_SCHEMA_REVISION = 3
ARTIFACT_SPEC_SCHEMA = "bioseq.private_postgres.artifacts"
ARTIFACT_SPEC_SCHEMA_REVISION = 1
GENERATED_SQL_MANIFEST_FIELDS = frozenset(
    {
        "schema_revision",
        "authority_mode",
        "compiler_profile",
        "clang_version",
        "compilation_signature",
        "extension_name",
        "extension_schema",
        "generated_module_prefix",
        "fmgr_boundary_module",
        "script_sha256",
        "statement_count",
        "statements",
        "entity_count",
        "public_entity_count",
        "public_stable_identities",
        "public_installed_identities",
        "routine_entity_count",
        "c_routine_identity_count",
        "c_link_symbol_inventory_count",
        "c_finfo_symbol_inventory_count",
        "c_wrapper_entity_count",
        "generated_wrapper_symbol_count",
        "handwritten_fmgr_export_symbol_count",
        "handwritten_fmgr_export_external_symbol_count",
        "handwritten_fmgr_export_owner_count",
        "c_symbol_sharing_group_count",
        "generated_cpp_profile_invariant",
        "entity_handle_count",
        "entity_handle_module_count",
        "entity_handle_modules",
        "row_codec_count",
        "row_codec_module_count",
        "row_codec_modules",
        "routine_boundary_contract_count",
        "routine_boundary_contracts",
        "result_shape_contract_count",
        "result_shape_contracts",
        "row_codec_contract_count",
        "row_codec_contracts",
        "c_routine_identities",
        "c_link_symbols",
        "c_finfo_symbols",
        "generated_wrapper_symbols",
        "handwritten_fmgr_export_symbols",
        "handwritten_fmgr_export_external_symbols",
        "fmgr_entrypoint_module_count",
        "fmgr_entrypoint_modules",
        "handwritten_fmgr_export_owners",
        "c_symbol_owners",
        "entity_counts_by_kind",
        "entities",
        "guc_generation",
    }
)
RUNTIME_CLOSURE_ALGORITHM = "postgresql.materialized_package"
RUNTIME_CLOSURE_SCHEMA_REVISION = 1
INSTALL_SCHEMA = "external_acceptance_install"
DENIED_ROLE = "external_acceptance_denied"
LIFECYCLE_TABLE = f"{INSTALL_SCHEMA}.external_lifecycle_probe"
INVALID_ENTITY_TABLE = (
    f"{INSTALL_SCHEMA}.external_invalid_entity_probe"
)
MISSING_ENTITY_TABLE = (
    f"{INSTALL_SCHEMA}.external_missing_entity_probe"
)
INVALID_ENTITY_SQLSTATE = "22023"
MISSING_ENTITY_SQLSTATE = "22023"
MISSING_ENTITY_MESSAGE = (
    'bioseq_external relation "external_missing_entity_probe" is missing '
    'the required "entity" option'
)
EXTERNAL_OPTIONS_TABLE = "bioseq_internal.external_relation_options"
EXTERNAL_OPTIONS_TABLE_IDENTITY = f"sql:table:{EXTERNAL_OPTIONS_TABLE}"
EXTERNAL_OPTIONS_CPP_NAME = (
    "bioseq::storage::metadata::catalog_rows::external_relation_options"
)
EXTERNAL_OPTIONS_SOURCE_MODULE = (
    "bioseq.postgres.storage.metadata.sql_rows"
)
EXTERNAL_OPTIONS_SOURCE_PATH = (
    "bioseq/backend/pg/column/metadata/sql_rows.cppm"
)
EXTERNAL_OPTIONS_ROW_CODEC_MODULE = (
    "bioseq.postgres.storage.metadata.sql_row_codecs"
)
EXTERNAL_OPTIONS_ROW_CODEC_FIELDS = (
    (0, "regclass", "regclass", "Oid", False, "row_codec.oid.required"),
    (
        1, "entity", "text", "std::optional<text*>", True,
        "row_codec.text_pointer.nullable",
    ),
    (
        2, "alphabet", "text", "std::optional<text*>", True,
        "row_codec.text_pointer.nullable",
    ),
    (
        3, "storage_width", "integer", "std::int32_t", False,
        "row_codec.std::int32_t.required",
    ),
    (
        4, "chunk_bases", "integer", "std::int32_t", False,
        "row_codec.std::int32_t.required",
    ),
)
EXPECTED_RELATION_ENTITIES = {
    "catalog": "catalog",
    "record_genes": "record_gene",
    "record_locations": "record_location",
    "sequence_chunks": "sequence_chunk",
    "sequences": "sequence",
    "taxonomy": "taxonomy",
}
EXTERNAL_SOURCE_PATHS = frozenset({
    "bioseq/backend/pg/external/relation.cppm",
    "bioseq/backend/pg/external/sql_catalog.cppm",
})
EXTERNAL_AUTHORITY_SOURCE_PATHS = frozenset({
    *EXTERNAL_SOURCE_PATHS,
    EXTERNAL_OPTIONS_SOURCE_PATH,
})
EXTERNAL_CATALOG_DEPENDENCIES_IDENTITY = (
    "sql:catalog_dependencies:$extension."
    "bioseq::postgres::external::sql_routines.bioseq_external_handler"
)
EXTERNAL_ACCESS_METHOD_IDENTITY = (
    "sql:access_method:$extension."
    "bioseq::postgres::external::sql_routines.bioseq_external"
)
EXPECTED_EXTERNAL_TOP_LEVEL_IDENTITIES = frozenset({
    "sql:function:bioseq.bioseq_external_handler(internal)",
    EXTERNAL_ACCESS_METHOD_IDENTITY,
    EXTERNAL_CATALOG_DEPENDENCIES_IDENTITY,
    "sql:schema:bioseq_external",
    *(
        f"sql:table:bioseq_external.{table}"
        for table in (
            "catalog",
            "record_genes",
            "record_locations",
            "sequence_chunks",
            "sequences",
            "taxonomy",
        )
    ),
    (
        "sql:revoke:bioseq_external:"
        "from=bioseq::sql::public_role:"
        "privileges=bioseq::sql::privileges::all"
    ),
    (
        "sql:revoke:bioseq_external:"
        "from=bioseq::sql::public_role:"
        "privileges=bioseq::sql::privileges::all:"
        "scope=bioseq::sql::acl_scope::all_tables_in_schema"
    ),
    (
        "sql:revoke:bioseq_external_handler:"
        "from=bioseq::sql::public_role:"
        "privileges=bioseq::sql::privileges::all"
    ),
})

# (column name, pg_type namespace, pg_type leaf, NOT NULL)
EXPECTED_TABLES: dict[str, tuple[tuple[str, str, str, bool], ...]] = {
    "taxonomy": (
        ("provider", "pg_catalog", "text", True),
        ("taxon_id", "pg_catalog", "int8", False),
        ("provider_taxon_id", "pg_catalog", "text", True),
        ("scientific_name", "pg_catalog", "text", False),
        ("common_name", "pg_catalog", "text", False),
        ("rank", "pg_catalog", "text", False),
        ("parent_taxon_id", "pg_catalog", "int8", False),
        ("lineage", "pg_catalog", "_int8", False),
        ("synonyms", "pg_catalog", "_text", False),
        ("provider_attributes", "pg_catalog", "jsonb", False),
        ("provider_version", "pg_catalog", "text", False),
        ("provenance", "pg_catalog", "jsonb", True),
        ("retrieved_at", "pg_catalog", "timestamptz", True),
    ),
    "catalog": (
        ("provider", "pg_catalog", "text", True),
        ("domain", "pg_catalog", "text", True),
        ("entity_kind", "pg_catalog", "text", True),
        ("molecule_kind", "pg_catalog", "text", False),
        ("namespace_name", "pg_catalog", "text", False),
        ("original_identifier", "pg_catalog", "text", True),
        ("accession", "pg_catalog", "text", True),
        ("accession_version", "pg_catalog", "int4", False),
        ("aliases", "pg_catalog", "jsonb", False),
        ("taxon_id", "pg_catalog", "int8", False),
        ("scientific_name", "pg_catalog", "text", False),
        ("title", "pg_catalog", "text", False),
        ("genes", "pg_catalog", "jsonb", False),
        ("keywords", "pg_catalog", "_text", False),
        ("locations", "pg_catalog", "jsonb", False),
        ("host_taxon_id", "pg_catalog", "int8", False),
        ("geography", "pg_catalog", "text", False),
        ("lineage_name", "pg_catalog", "text", False),
        ("sequence_length", "pg_catalog", "int8", False),
        ("assembly_accession", "pg_catalog", "text", False),
        ("assembly_database", "pg_catalog", "text", False),
        ("assembly_source", "pg_catalog", "text", False),
        ("assembly_level", "pg_catalog", "text", False),
        ("assembly_status", "pg_catalog", "text", False),
        ("release_date", "pg_catalog", "date", False),
        ("update_date", "pg_catalog", "date", False),
        ("is_reference", "pg_catalog", "bool", False),
        ("is_reviewed", "pg_catalog", "bool", False),
        ("is_annotated", "pg_catalog", "bool", False),
        ("is_fragment", "pg_catalog", "bool", False),
        ("retrievable", "pg_catalog", "bool", True),
        ("provider_attributes", "pg_catalog", "jsonb", False),
        ("provider_version", "pg_catalog", "text", False),
        ("provenance", "pg_catalog", "jsonb", True),
        ("retrieved_at", "pg_catalog", "timestamptz", True),
    ),
    "record_genes": (
        ("provider", "pg_catalog", "text", True),
        ("domain", "pg_catalog", "text", True),
        ("namespace_name", "pg_catalog", "text", False),
        ("original_identifier", "pg_catalog", "text", True),
        ("accession", "pg_catalog", "text", True),
        ("accession_version", "pg_catalog", "int4", False),
        ("gene_index", "pg_catalog", "int4", True),
        ("gene_count", "pg_catalog", "int4", True),
        ("gene_namespace", "pg_catalog", "text", True),
        ("gene_id", "pg_catalog", "text", True),
        ("gene_symbol", "pg_catalog", "text", False),
        ("gene_name", "pg_catalog", "text", False),
        ("gene_type", "pg_catalog", "text", False),
        ("locus_tag", "pg_catalog", "text", False),
        ("provider_version", "pg_catalog", "text", False),
        ("provenance", "pg_catalog", "jsonb", True),
        ("retrieved_at", "pg_catalog", "timestamptz", True),
    ),
    "record_locations": (
        ("provider", "pg_catalog", "text", True),
        ("domain", "pg_catalog", "text", True),
        ("namespace_name", "pg_catalog", "text", False),
        ("original_identifier", "pg_catalog", "text", True),
        ("accession", "pg_catalog", "text", True),
        ("accession_version", "pg_catalog", "int4", False),
        ("location_index", "pg_catalog", "int4", True),
        ("location_count", "pg_catalog", "int4", True),
        ("assembly_accession", "pg_catalog", "text", False),
        ("assembly_database", "pg_catalog", "text", False),
        ("sequence_alias", "pg_catalog", "text", True),
        ("start0", "pg_catalog", "int8", True),
        ("end0", "pg_catalog", "int8", True),
        ("strand", "pg_catalog", "int2", True),
        ("provider_version", "pg_catalog", "text", False),
        ("provenance", "pg_catalog", "jsonb", True),
        ("retrieved_at", "pg_catalog", "timestamptz", True),
    ),
    "sequences": (
        ("provider", "pg_catalog", "text", True),
        ("domain", "pg_catalog", "text", True),
        ("namespace_name", "pg_catalog", "text", False),
        ("original_identifier", "pg_catalog", "text", True),
        ("accession", "pg_catalog", "text", True),
        ("accession_version", "pg_catalog", "int4", False),
        ("molecule_kind", "pg_catalog", "text", True),
        ("taxon_id", "pg_catalog", "int8", False),
        ("assembly_accession", "pg_catalog", "text", False),
        ("assembly_database", "pg_catalog", "text", False),
        ("chromosome", "pg_catalog", "text", False),
        ("start0", "pg_catalog", "int8", False),
        ("end0", "pg_catalog", "int8", False),
        ("strand", "pg_catalog", "int2", False),
        ("sequence", "bioseq", "seq", True),
        ("provider_version", "pg_catalog", "text", False),
        ("provenance", "pg_catalog", "jsonb", True),
        ("retrieved_at", "pg_catalog", "timestamptz", True),
    ),
    "sequence_chunks": (
        ("provider", "pg_catalog", "text", True),
        ("domain", "pg_catalog", "text", True),
        ("namespace_name", "pg_catalog", "text", False),
        ("original_identifier", "pg_catalog", "text", True),
        ("accession", "pg_catalog", "text", True),
        ("accession_version", "pg_catalog", "int4", False),
        ("molecule_kind", "pg_catalog", "text", True),
        ("taxon_id", "pg_catalog", "int8", False),
        ("assembly_accession", "pg_catalog", "text", False),
        ("assembly_database", "pg_catalog", "text", False),
        ("chromosome", "pg_catalog", "text", False),
        ("start0", "pg_catalog", "int8", False),
        ("end0", "pg_catalog", "int8", False),
        ("strand", "pg_catalog", "int2", False),
        ("chunk_index", "pg_catalog", "int8", True),
        ("base_offset", "pg_catalog", "int8", True),
        ("sequence_chunk", "bioseq", "seq", True),
        ("end_of_record", "pg_catalog", "bool", True),
        ("provider_version", "pg_catalog", "text", False),
        ("provenance", "pg_catalog", "jsonb", True),
        ("retrieved_at", "pg_catalog", "timestamptz", True),
    ),
}
EXPECTED_COLUMN_COUNT = sum(len(columns) for columns in EXPECTED_TABLES.values())
POSTGRESQL_TOOLS = frozenset({
    "postgres",
    "initdb",
    "pg_ctl",
    "createdb",
    "dropdb",
    "psql",
})
BASE_INPUT_FILE_KEYS = frozenset({
    "python",
    "cmake",
    "git",
    "pg_config",
    "stage_script",
    "config_template",
    "control_file",
    "install_sql",
    "manifest",
    "dso",
    "static_contract_script",
    "cmake_fragment",
    "mode_source",
    "runner",
    "private_postgres_helper",
})
EXPECTED_INPUT_FILE_KEYS = frozenset({
    *BASE_INPUT_FILE_KEYS,
    *(f"external_source:{path}" for path in EXTERNAL_AUTHORITY_SOURCE_PATHS),
})
READ_ONLY_INSERT_SQL = {
    "catalog": """
INSERT INTO bioseq_external.catalog
  (provider, domain, entity_kind, original_identifier, accession,
   retrievable, provenance, retrieved_at)
VALUES
  ('acceptance', 'sequence', 'nucleotide', 'probe', 'probe', true,
   '{}'::pg_catalog.jsonb, pg_catalog.now());
""",
    "record_genes": """
INSERT INTO bioseq_external.record_genes
  (provider, domain, original_identifier, accession, gene_index,
   gene_count, gene_namespace, gene_id, provenance, retrieved_at)
VALUES
  ('acceptance', 'sequence', 'probe', 'probe', 0, 1, 'acceptance',
   'probe', '{}'::pg_catalog.jsonb, pg_catalog.now());
""",
    "record_locations": """
INSERT INTO bioseq_external.record_locations
  (provider, domain, original_identifier, accession, location_index,
   location_count, sequence_alias, start0, end0, strand, provenance,
   retrieved_at)
VALUES
  ('acceptance', 'sequence', 'probe', 'probe', 0, 1, 'probe', 0, 1, 1,
   '{}'::pg_catalog.jsonb, pg_catalog.now());
""",
    "sequence_chunks": """
INSERT INTO bioseq_external.sequence_chunks
  (provider, domain, original_identifier, accession, molecule_kind,
   chunk_index, base_offset, sequence_chunk, end_of_record, provenance,
   retrieved_at)
VALUES
  ('acceptance', 'sequence', 'probe', 'probe', 'dna', 0, 0,
   'A'::bioseq.seq('dna4', 64), true, '{}'::pg_catalog.jsonb,
   pg_catalog.now());
""",
    "sequences": """
INSERT INTO bioseq_external.sequences
  (provider, domain, original_identifier, accession, molecule_kind,
   sequence, provenance, retrieved_at)
VALUES
  ('acceptance', 'sequence', 'probe', 'probe', 'dna',
   'A'::bioseq.seq('dna4', 64), '{}'::pg_catalog.jsonb,
   pg_catalog.now());
""",
    "taxonomy": """
INSERT INTO bioseq_external.taxonomy
  (provider, provider_taxon_id, provenance, retrieved_at)
VALUES
  ('acceptance', '0', '{}'::pg_catalog.jsonb, pg_catalog.now());
""",
}


def _canonical_json(value: object) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n"
    ).encode("utf-8")


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _regular_file(
    value: str | Path,
    label: str,
    *,
    executable: bool = False,
) -> Path:
    path = Path(value).expanduser().absolute()
    try:
        metadata = path.lstat()
    except OSError as error:
        raise ExternalAcceptanceError(f"{label} is missing: {path}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise ExternalAcceptanceError(
            f"{label} must be a regular non-symlink file: {path}"
        )
    if metadata.st_nlink != 1:
        raise ExternalAcceptanceError(f"{label} is hard-linked: {path}")
    resolved = path.resolve(strict=True)
    if resolved != path:
        raise ExternalAcceptanceError(
            f"{label} path is not canonical: {path}"
        )
    if executable and not os.access(resolved, os.X_OK):
        raise ExternalAcceptanceError(f"{label} is not executable: {resolved}")
    return resolved


def _real_directory(
    value: str | Path,
    label: str,
    *,
    create: bool,
) -> Path:
    path = Path(value).expanduser().absolute()
    if create:
        path.mkdir(parents=True, exist_ok=True)
    try:
        metadata = path.lstat()
    except OSError as error:
        raise ExternalAcceptanceError(f"{label} is missing: {path}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise ExternalAcceptanceError(
            f"{label} must be a real non-symlink directory: {path}"
        )
    resolved = path.resolve(strict=True)
    if resolved != path:
        raise ExternalAcceptanceError(
            f"{label} path is not canonical: {path}"
        )
    return resolved


def _write_bytes(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() or path.is_symlink():
        raise ExternalAcceptanceError(
            f"refusing to overwrite evidence file: {path}"
        )
    with path.open("xb") as stream:
        stream.write(value)


def _write_json(path: Path, value: object) -> None:
    _write_bytes(path, _canonical_json(value))


def _copy_file(source: Path, destination: Path) -> None:
    _write_bytes(destination, source.read_bytes())


def _atomic_write(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(
        f".{path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp"
    )
    with temporary.open("xb") as stream:
        stream.write(value)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def _load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ExternalAcceptanceError(
            f"cannot read {label}: {path}: {error}"
        ) from error
    if not isinstance(value, dict):
        raise ExternalAcceptanceError(f"{label} must be one JSON object")
    return value


def _terminate_group(process: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        pass
    try:
        process.wait(timeout=2)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass


def _run_capture(
    command: Sequence[str],
    *,
    timeout: int,
    env: Mapping[str, str] | None = None,
    input_bytes: bytes | None = None,
) -> tuple[int, bytes, bytes]:
    process = subprocess.Popen(
        list(command),
        stdin=subprocess.PIPE if input_bytes is not None else subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=None if env is None else dict(env),
        start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(input=input_bytes, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        _terminate_group(process)
        stdout, stderr = process.communicate()
        raise ExternalAcceptanceError(
            f"command timed out after {timeout}s: {' '.join(command)}"
        ) from error
    return int(process.returncode), stdout or b"", stderr or b""


def _run_checked(
    command: Sequence[str],
    *,
    timeout: int,
    env: Mapping[str, str] | None = None,
    input_bytes: bytes | None = None,
    stdout_path: Path | None = None,
    stderr_path: Path | None = None,
) -> bytes:
    returncode, stdout, stderr = _run_capture(
        command,
        timeout=timeout,
        env=env,
        input_bytes=input_bytes,
    )
    if stdout_path is not None:
        _write_bytes(stdout_path, stdout)
    if stderr_path is not None:
        _write_bytes(stderr_path, stderr)
    if returncode != 0:
        raise ExternalAcceptanceError(
            f"command failed with exit code {returncode}: "
            f"{' '.join(command)}\n"
            f"{stderr.decode('utf-8', errors='replace')}"
            f"{stdout.decode('utf-8', errors='replace')}"
        )
    return stdout


def _csv_records(value: bytes, label: str) -> list[dict[str, str]]:
    try:
        text = value.decode("utf-8", errors="strict")
        records = list(csv.DictReader(io.StringIO(text)))
    except (UnicodeError, csv.Error) as error:
        raise ExternalAcceptanceError(
            f"{label} is not valid UTF-8 CSV"
        ) from error
    if not records:
        raise ExternalAcceptanceError(f"{label} returned no rows")
    return [{str(key): str(item) for key, item in row.items()}
            for row in records]


def _one_csv_record(value: bytes, label: str) -> dict[str, str]:
    records = _csv_records(value, label)
    if len(records) != 1:
        raise ExternalAcceptanceError(
            f"{label} returned {len(records)} rows, expected one"
        )
    return records[0]


def _file_record(
    path: Path,
    *,
    repository_root: Path | None = None,
) -> dict[str, object]:
    record: dict[str, object] = {
        "path": str(path),
        "sha256": _sha256(path),
        "size": path.stat().st_size,
    }
    if repository_root is not None:
        try:
            record["repository_path"] = path.relative_to(
                repository_root
            ).as_posix()
        except ValueError:
            pass
    return record


def _expected_catalog_rows() -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for table in sorted(EXPECTED_TABLES):
        for ordinal, (column, type_schema, type_name, not_null) in enumerate(
            EXPECTED_TABLES[table],
            start=1,
        ):
            rows.append({
                "schema_name": "bioseq_external",
                "table_name": table,
                "relkind": "r",
                "persistence": "p",
                "access_method": "bioseq_external",
                "ordinal": ordinal,
                "column_name": column,
                "type_schema": type_schema,
                "type_name": type_name,
                "not_null": not_null,
                "relation_size": 0,
            })
    return rows


def _expected_external_option_rows() -> list[dict[str, object]]:
    if set(EXPECTED_RELATION_ENTITIES) != set(EXPECTED_TABLES):
        raise ExternalAcceptanceError(
            "external entity routing does not close over the six public tables"
        )
    return [
        {
            "relation": f"bioseq_external.{table}",
            "entity": EXPECTED_RELATION_ENTITIES[table],
            "alphabet": None,
            "storage_width": 0,
            "chunk_bases": 0,
        }
        for table in sorted(EXPECTED_RELATION_ENTITIES)
    ]


EXPECTED_CATALOG_SHA256 = _sha256_bytes(
    _canonical_json(_expected_catalog_rows())
)


def _canonical_repository_source(
    repository_root: Path,
    relative_text: str,
    label: str,
) -> Path:
    relative = Path(relative_text)
    if (
        not relative_text
        or relative.is_absolute()
        or relative_text != relative.as_posix()
        or "\\" in relative_text
        or ".." in relative.parts
    ):
        raise ExternalAcceptanceError(
            f"{label} is not a canonical repository-relative path: "
            f"{relative_text!r}"
        )
    candidate = (repository_root / relative).absolute()
    source = _regular_file(candidate, label)
    try:
        canonical_relative = source.relative_to(repository_root)
    except ValueError as error:
        raise ExternalAcceptanceError(
            f"{label} escapes the repository: {source}"
        ) from error
    if canonical_relative != relative:
        raise ExternalAcceptanceError(
            f"{label} differs from its canonical repository path"
        )
    return source


def _compact_cpp_type(value: object) -> str:
    if not isinstance(value, str):
        return ""
    return "".join(value.split())


def _validate_external_options_manifest(
    manifest: Mapping[str, Any],
    *,
    entities: Sequence[Mapping[str, Any]],
    repository_root: Path,
) -> dict[str, object]:
    options_entities = [
        entity
        for entity in entities
        if (
            entity.get("source_path") == EXTERNAL_OPTIONS_SOURCE_PATH
            and (
                entity.get("stable_identity")
                == EXTERNAL_OPTIONS_TABLE_IDENTITY
                or entity.get("cpp_declaration")
                == EXTERNAL_OPTIONS_CPP_NAME
                or str(entity.get("cpp_declaration", "")).startswith(
                    EXTERNAL_OPTIONS_CPP_NAME + "::"
                )
            )
        )
    ]
    kind_counts = {
        kind: sum(entity.get("kind") == kind for entity in options_entities)
        for kind in ("table", "column", "primary_key", "check", "config_dump")
    }
    if kind_counts != {
        "table": 1,
        "column": 5,
        "primary_key": 1,
        "check": 3,
        "config_dump": 1,
    } or len(options_entities) != 11:
        raise ExternalAcceptanceError(
            "generated manifest does not contain the exact private external "
            "option table/column/PK/check/config-dump closure"
        )
    source_hashes = {
        entity.get("source_sha256") for entity in options_entities
    }
    if (
        any(
            entity.get("source_module") != EXTERNAL_OPTIONS_SOURCE_MODULE
            for entity in options_entities
        )
        or len(source_hashes) != 1
        or re.fullmatch(r"[0-9a-f]{64}", str(next(iter(source_hashes))))
        is None
    ):
        raise ExternalAcceptanceError(
            "private external option catalog source provenance is malformed"
        )
    source = _canonical_repository_source(
        repository_root,
        EXTERNAL_OPTIONS_SOURCE_PATH,
        "external option catalog authority",
    )
    if _sha256(source) != next(iter(source_hashes)):
        raise ExternalAcceptanceError(
            "private external option catalog source hash is stale"
        )
    by_kind = {
        kind: [
            entity
            for entity in options_entities
            if entity.get("kind") == kind
        ]
        for kind in kind_counts
    }
    table = by_kind["table"][0]
    if (
        table.get("stable_identity") != EXTERNAL_OPTIONS_TABLE_IDENTITY
        or table.get("installed_identity") != EXTERNAL_OPTIONS_TABLE_IDENTITY
        or table.get("schema") != "bioseq_internal"
        or table.get("schema_mode") != "fixed"
        or table.get("name") != "external_relation_options"
        or table.get("cpp_declaration") != EXTERNAL_OPTIONS_CPP_NAME
    ):
        raise ExternalAcceptanceError(
            "private external option table identity/schema is not exact"
        )
    columns = {
        str(entity.get("stable_identity"))
        for entity in by_kind["column"]
    }
    constraints = {
        str(entity.get("stable_identity"))
        for entity in by_kind["check"]
    }
    if (
        set(table.get("columns", [])) != columns
        or set(table.get("constraints", [])) != constraints
        or len(table.get("columns", [])) != 5
        or len(table.get("constraints", [])) != 3
    ):
        raise ExternalAcceptanceError(
            "private external option table child closure is incomplete"
        )
    for companion in by_kind["column"]:
        if set(companion.get("dependencies", [])) != {
            "sql:schema:bioseq_internal",
            EXTERNAL_OPTIONS_TABLE_IDENTITY,
        }:
            raise ExternalAcceptanceError(
                "private external option column lost its exact schema/table "
                "dependencies"
            )
    for companion in (
        *by_kind["primary_key"],
        *by_kind["check"],
        *by_kind["config_dump"],
    ):
        if set(companion.get("dependencies", [])) != {
            EXTERNAL_OPTIONS_TABLE_IDENTITY
        }:
            raise ExternalAcceptanceError(
                "private external option companion lost its exact table "
                "dependency"
            )
    generated_cpp = table.get("generated_cpp")
    if (
        not isinstance(generated_cpp, dict)
        or generated_cpp.get("facet") != "row_codec"
        or generated_cpp.get("owner_key") != "storage_metadata"
        or generated_cpp.get("source_module")
        != EXTERNAL_OPTIONS_SOURCE_MODULE
        or generated_cpp.get("module_name")
        != EXTERNAL_OPTIONS_ROW_CODEC_MODULE
        or generated_cpp.get("row_cpp_name") != EXTERNAL_OPTIONS_CPP_NAME
        or generated_cpp.get("profile_invariant") is not True
    ):
        raise ExternalAcceptanceError(
            "private external option table lost its generated typed row codec"
        )
    modules = manifest.get("row_codec_modules")
    if not isinstance(modules, list):
        raise ExternalAcceptanceError(
            "generated manifest has no row-codec module inventory"
        )
    matching_modules = [
        module
        for module in modules
        if (
            isinstance(module, dict)
            and module.get("module_name") == EXTERNAL_OPTIONS_ROW_CODEC_MODULE
        )
    ]
    if len(matching_modules) != 1:
        raise ExternalAcceptanceError(
            "storage-metadata row-codec module identity is not unique"
        )
    module = matching_modules[0]
    row_names = module.get("row_cpp_names")
    row_identities = module.get("row_entity_identities")
    if (
        module.get("owner_key") != "storage_metadata"
        or module.get("source_module") != EXTERNAL_OPTIONS_SOURCE_MODULE
        or module.get("profile_invariant") is not True
        or not isinstance(row_names, list)
        or not isinstance(row_identities, list)
        or len(row_names) != len(row_identities)
        or [
            identity
            for name, identity in zip(row_names, row_identities, strict=True)
            if name == EXTERNAL_OPTIONS_CPP_NAME
        ] != [EXTERNAL_OPTIONS_TABLE_IDENTITY]
    ):
        raise ExternalAcceptanceError(
            "storage-metadata codec module does not bind the external option "
            "row exactly"
        )
    contracts = manifest.get("row_codec_contracts")
    if not isinstance(contracts, list):
        raise ExternalAcceptanceError(
            "generated manifest has no row-codec contract inventory"
        )
    matching_contracts = [
        contract
        for contract in contracts
        if (
            isinstance(contract, dict)
            and contract.get("row_identity")
            == EXTERNAL_OPTIONS_TABLE_IDENTITY
        )
    ]
    if len(matching_contracts) != 1:
        raise ExternalAcceptanceError(
            "private external option row-codec contract is not unique"
        )
    contract = matching_contracts[0]
    fields = contract.get("fields")
    if (
        contract.get("row_cpp_name") != EXTERNAL_OPTIONS_CPP_NAME
        or contract.get("module_name") != EXTERNAL_OPTIONS_ROW_CODEC_MODULE
        or not isinstance(fields, list)
    ):
        raise ExternalAcceptanceError(
            "private external option row-codec identity is stale"
        )
    actual_fields = tuple(
        (
            field.get("ordinal"),
            field.get("sql_name"),
            field.get("sql_type"),
            _compact_cpp_type(field.get("cpp_type")),
            field.get("nullable"),
            field.get("converter_key"),
        )
        for field in fields
        if isinstance(field, dict)
    )
    if (
        len(actual_fields) != len(fields)
        or actual_fields != EXTERNAL_OPTIONS_ROW_CODEC_FIELDS
    ):
        raise ExternalAcceptanceError(
            "private external option row-codec fields differ in exact "
            "name/order/type/nullability/converter"
        )
    return _file_record(source, repository_root=repository_root)


def _validate_generated_manifest(
    manifest: Mapping[str, Any],
    *,
    install_sql: Path,
    repository_root: Path,
) -> dict[str, object]:
    unknown = sorted(set(manifest) - GENERATED_SQL_MANIFEST_FIELDS)
    if unknown:
        raise ExternalAcceptanceError(
            f"generated manifest has unknown fields: {unknown}"
        )
    if (
        type(manifest.get("schema_revision")) is not int
        or manifest["schema_revision"] != 1
    ):
        raise ExternalAcceptanceError(
            "generated manifest schema revision is not exactly 1"
        )
    if manifest.get("authority_mode") != "annotated_cpp":
        raise ExternalAcceptanceError(
            "generated manifest is not the complete natural C++ authority"
        )
    install_hash = _sha256(install_sql)
    if manifest.get("script_sha256") != install_hash:
        raise ExternalAcceptanceError(
            "generated manifest does not bind the install SQL bytes"
        )
    entities = manifest.get("entities")
    if not isinstance(entities, list) or not entities:
        raise ExternalAcceptanceError(
            "generated manifest has no entity inventory"
        )
    if manifest.get("entity_count") != len(entities):
        raise ExternalAcceptanceError(
            "generated manifest entity_count is stale"
        )
    identities: list[str] = []
    external: list[dict[str, Any]] = []
    for index, raw_entity in enumerate(entities):
        if not isinstance(raw_entity, dict):
            raise ExternalAcceptanceError(
                f"generated manifest entity {index} is not an object"
            )
        identity = raw_entity.get("stable_identity")
        if not isinstance(identity, str) or not identity:
            raise ExternalAcceptanceError(
                f"generated manifest entity {index} has no stable identity"
            )
        identities.append(identity)
        if raw_entity.get("source_path") in EXTERNAL_SOURCE_PATHS:
            external.append(raw_entity)
    if len(identities) != len(set(identities)):
        raise ExternalAcceptanceError(
            "generated manifest repeats a stable identity"
        )
    metadata_source_record = _validate_external_options_manifest(
        manifest,
        entities=entities,
        repository_root=repository_root,
    )
    external_top_level = {
        str(entity["stable_identity"])
        for entity in external
        if entity.get("kind") != "column"
    }
    if external_top_level != EXPECTED_EXTERNAL_TOP_LEVEL_IDENTITIES:
        raise ExternalAcceptanceError(
            "external generated entity set is not the exact reviewed "
            "handler/AM/schema/six-table/ACL surface"
        )
    global_external_top_level = {
        str(entity["stable_identity"])
        for entity in entities
        if (
            isinstance(entity, dict)
            and entity.get("kind") != "column"
            and "bioseq_external"
            in str(entity.get("stable_identity", ""))
        )
    }
    if global_external_top_level != EXPECTED_EXTERNAL_TOP_LEVEL_IDENTITIES:
        raise ExternalAcceptanceError(
            "an authority outside the reviewed external sources owns an "
            "external SQL identity"
        )

    by_identity = {
        str(entity["stable_identity"]): entity for entity in entities
    }
    catalog_dependencies = by_identity.get(
        EXTERNAL_CATALOG_DEPENDENCIES_IDENTITY
    )
    if (
        not isinstance(catalog_dependencies, dict)
        or catalog_dependencies.get("kind") != "catalog_dependencies"
        or set(catalog_dependencies.get("dependencies", [])) != {
            "sql:function:bioseq.bioseq_external_handler(internal)",
            EXTERNAL_OPTIONS_TABLE_IDENTITY,
        }
    ):
        raise ExternalAcceptanceError(
            "AM catalog dependency does not root exactly the handler and "
            "typed external option catalog"
        )
    for table_name in EXPECTED_TABLES:
        table_identity = f"sql:table:bioseq_external.{table_name}"
        expected_dependencies = {
            "sql:schema:bioseq_external",
            EXTERNAL_ACCESS_METHOD_IDENTITY,
            EXTERNAL_OPTIONS_TABLE_IDENTITY,
        }
        if table_name in {"sequences", "sequence_chunks"}:
            expected_dependencies.add("sql:type:bioseq.seq")
        table = by_identity.get(table_identity)
        if (
            not isinstance(table, dict)
            or set(table.get("dependencies", [])) != expected_dependencies
        ):
            raise ExternalAcceptanceError(
                f"external table dependencies are not exact: {table_identity}"
            )

    source_provenance: dict[str, dict[str, object]] = {}
    for entity in external:
        source_path = entity.get("source_path")
        source_hash = entity.get("source_sha256")
        if (
            not isinstance(source_path, str)
            or source_path not in EXTERNAL_SOURCE_PATHS
            or not isinstance(source_hash, str)
            or re.fullmatch(r"[0-9a-f]{64}", source_hash) is None
        ):
            raise ExternalAcceptanceError(
                "external entity has malformed source provenance"
            )
        source = _canonical_repository_source(
            repository_root,
            source_path,
            f"external authority {source_path}",
        )
        actual_hash = _sha256(source)
        if actual_hash != source_hash:
            raise ExternalAcceptanceError(
                f"external authority hash is stale: {source_path}"
            )
        condition = entity.get("active_condition")
        if (
            not isinstance(condition, str)
            or "BIOSEQ_EXTERNAL_PG" not in condition
        ):
            raise ExternalAcceptanceError(
                f"external authority escaped its feature gate: "
                f"{entity['stable_identity']}"
            )
        record = _file_record(source, repository_root=repository_root)
        existing = source_provenance.setdefault(source_path, record)
        if existing != record:
            raise ExternalAcceptanceError(
                f"inconsistent source provenance for {source_path}"
            )
    if set(source_provenance) != EXTERNAL_SOURCE_PATHS:
        raise ExternalAcceptanceError(
            "external source provenance does not close over both authorities"
        )
    source_provenance[EXTERNAL_OPTIONS_SOURCE_PATH] = metadata_source_record
    if set(source_provenance) != EXTERNAL_AUTHORITY_SOURCE_PATHS:
        raise ExternalAcceptanceError(
            "external source provenance does not close over the public and "
            "typed metadata authorities"
        )

    statements = manifest.get("statements")
    if not isinstance(statements, list):
        raise ExternalAcceptanceError(
            "generated manifest has no statement inventory"
        )
    if manifest.get("statement_count") != len(statements):
        raise ExternalAcceptanceError(
            "generated manifest statement_count is stale"
        )
    external_statement_owners = {
        str(statement.get("owner_identity"))
        for statement in statements
        if (
            isinstance(statement, dict)
            and statement.get("source_path") in EXTERNAL_SOURCE_PATHS
        )
    }
    if external_statement_owners != EXPECTED_EXTERNAL_TOP_LEVEL_IDENTITIES:
        raise ExternalAcceptanceError(
            "external statement inventory does not exactly cover the "
            "reviewed external owner set"
        )
    statement_positions = {
        str(statement.get("owner_identity")): index
        for index, statement in enumerate(statements)
        if isinstance(statement, dict)
    }
    metadata_position = statement_positions.get(
        EXTERNAL_OPTIONS_TABLE_IDENTITY
    )
    for table_name in EXPECTED_TABLES:
        table_position = statement_positions.get(
            f"sql:table:bioseq_external.{table_name}"
        )
        if (
            not isinstance(metadata_position, int)
            or not isinstance(table_position, int)
            or metadata_position >= table_position
        ):
            raise ExternalAcceptanceError(
                "typed external option metadata was rendered after public "
                f"table {table_name}"
            )
    if manifest.get("handwritten_fmgr_export_external_symbols") != [
        "bioseq_external_handler"
    ] or manifest.get("handwritten_fmgr_export_external_symbol_count") != 1:
        raise ExternalAcceptanceError(
            "handwritten external FMGR ownership is not exactly the handler"
        )
    external_column_count = sum(
        entity.get("kind") == "column" for entity in external
    )
    if external_column_count != EXPECTED_COLUMN_COUNT:
        raise ExternalAcceptanceError(
            "external manifest column inventory is not the exact "
            f"{EXPECTED_COLUMN_COUNT}-column surface"
        )
    return {
        "install_sql_sha256": install_hash,
        "manifest_script_sha256": manifest["script_sha256"],
        "manifest_entity_count": len(entities),
        "manifest_statement_count": len(statements),
        "external_top_level_identity_count": len(external_top_level),
        "external_column_identity_count": external_column_count,
        "external_options_table_identity": EXTERNAL_OPTIONS_TABLE_IDENTITY,
        "external_options_row_codec_module": (
            EXTERNAL_OPTIONS_ROW_CODEC_MODULE
        ),
        "external_options_field_count": len(
            EXTERNAL_OPTIONS_ROW_CODEC_FIELDS
        ),
        "external_statement_owners": sorted(external_statement_owners),
        "external_source_provenance": source_provenance,
    }


def _postgresql_provenance(
    pg_config: Path,
    *,
    timeout: int,
) -> dict[str, object]:
    values: dict[str, str] = {}
    for flag in (
        "--version",
        "--configure",
        "--bindir",
        "--sharedir",
        "--pkglibdir",
    ):
        output = _run_checked(
            [str(pg_config), flag],
            timeout=timeout,
        ).decode("utf-8", errors="strict").rstrip()
        if not output:
            raise ExternalAcceptanceError(
                f"pg_config {flag} returned no value"
            )
        values[flag.removeprefix("--")] = output
    if not values["version"].startswith("PostgreSQL 17"):
        raise ExternalAcceptanceError(
            f"external acceptance requires PostgreSQL 17, got "
            f"{values['version']!r}"
        )
    bindir = _real_directory(
        values["bindir"], "PostgreSQL bindir", create=False
    )
    tools: dict[str, object] = {}
    for name in sorted(POSTGRESQL_TOOLS):
        tool = _regular_file(
            bindir / name,
            f"PostgreSQL {name}",
            executable=True,
        )
        tools[name] = _file_record(tool)
    return {
        "pg_config": values,
        "tools": tools,
    }


def _git_provenance(
    git: Path,
    repository_root: Path,
    source_paths: Sequence[str],
    *,
    timeout: int,
) -> dict[str, object]:
    revision = _run_checked(
        [str(git), "-C", str(repository_root), "rev-parse", "HEAD"],
        timeout=timeout,
    ).decode("ascii", errors="strict").strip()
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise ExternalAcceptanceError(
            f"Git returned a noncanonical revision: {revision!r}"
        )
    status = _run_checked(
        [
            str(git),
            "-C",
            str(repository_root),
            "status",
            "--short",
            "--untracked-files=all",
            "--",
            *source_paths,
        ],
        timeout=timeout,
    ).decode("utf-8", errors="strict").splitlines()
    return {
        "revision": revision,
        "source_status": status,
    }


def _artifact_spec_document(
    *,
    control_file: Path,
    install_sql: Path,
    dso: Path,
) -> dict[str, object]:
    return {
        "schema": ARTIFACT_SPEC_SCHEMA,
        "schema_revision": ARTIFACT_SPEC_SCHEMA_REVISION,
        "artifacts": [{
            "name": "bioseq",
            "control": str(control_file),
            "install_sql": str(install_sql),
            "library": str(dso),
        }],
    }


def _validate_artifact_spec(
    value: object,
    *,
    control_file: Path,
    install_sql: Path,
    dso: Path,
) -> dict[str, Any]:
    spec = _exact_mapping(
        value,
        {"schema", "schema_revision", "artifacts"},
        "external acceptance artifact specification",
    )
    revision = _nonnegative_integer(
        spec["schema_revision"],
        "external acceptance artifact specification schema revision",
    )
    if (
        spec["schema"] != ARTIFACT_SPEC_SCHEMA
        or revision != ARTIFACT_SPEC_SCHEMA_REVISION
    ):
        raise ExternalAcceptanceError(
            "external acceptance artifact specification schema is invalid"
        )
    artifacts = spec["artifacts"]
    if not isinstance(artifacts, list) or len(artifacts) != 1:
        raise ExternalAcceptanceError(
            "external acceptance requires exactly one staged artifact"
        )
    artifact = _exact_mapping(
        artifacts[0],
        {"name", "control", "install_sql", "library"},
        "external acceptance artifact",
    )
    expected = _artifact_spec_document(
        control_file=control_file,
        install_sql=install_sql,
        dso=dso,
    )["artifacts"][0]
    if artifact != expected:
        raise ExternalAcceptanceError(
            "external acceptance artifact specification is inconsistent"
        )
    return spec


def _stage_package_aggregate(
    *,
    control_file: Path,
    install_sql: Path,
    dso: Path,
    control_sha256: str,
    install_sql_sha256: str,
    library_sha256: str,
) -> str:
    record = (
        "artifact:bioseq\n"
        "module_pathname_override:\n"
        f"control:{control_file.name}:{control_sha256}\n"
        f"install_sql:{install_sql.name}:{install_sql_sha256}\n"
        f"library:{dso.name}:{library_sha256}\n"
    ).encode("utf-8")
    return _sha256_bytes(record)


def _runtime_entry_record(entry: Mapping[str, Any]) -> str:
    return (
        f"root:{entry['root']}\n"
        f"path:{entry['path']}\n"
        f"kind:{entry['kind']}\n"
        f"source_kind:{entry['source_kind']}\n"
        f"source_path:{entry['source_path']}\n"
        "source_link_target:"
        f"{entry['source_link_target'] if entry['source_link_target'] is not None else '-'}\n"
        f"resolved_source_path:{entry['resolved_source_path']}\n"
        f"size:{entry['size'] if entry['size'] is not None else '-'}\n"
        f"sha256:{entry['sha256'] if entry['sha256'] is not None else '-'}\n"
    )


def _loader_entry_record(entry: Mapping[str, Any]) -> str:
    return (
        f"source:{entry['source']}\n"
        f"staged:{entry['staged']}\n"
        f"size:{entry['size']}\n"
        f"sha256:{entry['sha256']}\n"
    )


def _validate_runtime_closure(
    value: object,
    *,
    source: Mapping[str, Any],
    staged: Mapping[str, Any],
) -> dict[str, object]:
    closure = _exact_mapping(
        value,
        {
            "algorithm",
            "schema_revision",
            "entry_count",
            "aggregate_sha256",
            "roots",
            "required_sentinels",
            "loader_dependencies",
            "entries",
        },
        "stage runtime closure",
    )
    revision = _nonnegative_integer(
        closure["schema_revision"], "stage runtime closure schema revision"
    )
    if (
        closure["algorithm"] != RUNTIME_CLOSURE_ALGORITHM
        or revision != RUNTIME_CLOSURE_SCHEMA_REVISION
    ):
        raise ExternalAcceptanceError(
            "stage runtime closure algorithm/schema is invalid"
        )
    roots = _exact_mapping(
        closure["roots"],
        {"bindir", "sharedir", "pkglibdir"},
        "stage runtime roots",
    )
    for name in ("bindir", "sharedir", "pkglibdir"):
        root = _exact_mapping(
            roots[name],
            {"reported_source", "canonical_source", "staged"},
            f"stage runtime {name} root",
        )
        if (
            root["reported_source"] != source[name]
            or root["staged"] != staged[name]
            or not isinstance(root["canonical_source"], str)
            or not Path(root["canonical_source"]).is_absolute()
        ):
            raise ExternalAcceptanceError(
                f"stage runtime {name} root is inconsistent"
            )

    entries = closure["entries"]
    entry_count = _nonnegative_integer(
        closure["entry_count"], "stage runtime closure entry count"
    )
    if not isinstance(entries, list) or len(entries) != entry_count:
        raise ExternalAcceptanceError(
            "stage runtime closure entry count is inconsistent"
        )
    runtime_records: list[str] = []
    runtime_keys: list[tuple[int, str]] = []
    root_order = {"bindir": 0, "sharedir": 1, "pkglibdir": 2}
    for index, raw_entry in enumerate(entries):
        entry = _exact_mapping(
            raw_entry,
            {
                "root",
                "path",
                "kind",
                "source_kind",
                "source_path",
                "source_link_target",
                "resolved_source_path",
                "size",
                "sha256",
            },
            f"stage runtime entry {index}",
        )
        relative = Path(entry["path"]) if isinstance(entry["path"], str) else Path()
        if (
            not isinstance(entry["root"], str)
            or entry["root"] not in roots
            or not isinstance(entry["path"], str)
            or not entry["path"]
            or relative.is_absolute()
            or any(part in {"", ".", ".."} for part in relative.parts)
            or "\\" in entry["path"]
            or not isinstance(entry["kind"], str)
            or entry["kind"] not in {"file", "dir"}
            or not isinstance(entry["source_kind"], str)
            or entry["source_kind"] not in {"file", "dir", "symlink"}
            or not isinstance(entry["source_path"], str)
            or not Path(entry["source_path"]).is_absolute()
            or not isinstance(entry["resolved_source_path"], str)
            or not Path(entry["resolved_source_path"]).is_absolute()
        ):
            raise ExternalAcceptanceError(
                f"stage runtime entry {index} is malformed"
            )
        runtime_keys.append((root_order[entry["root"]], entry["path"]))
        if entry["kind"] == "dir":
            if entry["size"] is not None or entry["sha256"] is not None:
                raise ExternalAcceptanceError(
                    f"stage runtime directory entry {index} is malformed"
                )
        else:
            _nonnegative_integer(
                entry["size"], f"stage runtime entry {index} size"
            )
            _digest(entry["sha256"], f"stage runtime entry {index} hash")
        if entry["source_kind"] == "symlink":
            if not isinstance(entry["source_link_target"], str) or not entry[
                "source_link_target"
            ]:
                raise ExternalAcceptanceError(
                    f"stage runtime symlink entry {index} is malformed"
                )
        elif entry["source_link_target"] is not None:
            raise ExternalAcceptanceError(
                f"stage runtime entry {index} has a spurious link target"
            )
        runtime_records.append(_runtime_entry_record(entry))
    if len(set(runtime_keys)) != len(runtime_keys) or runtime_keys != sorted(
        runtime_keys
    ):
        raise ExternalAcceptanceError(
            "stage runtime closure entries are not canonical"
        )
    aggregate = _digest(
        closure["aggregate_sha256"], "stage runtime closure aggregate"
    )
    if aggregate != _sha256_bytes("".join(runtime_records).encode("utf-8")):
        raise ExternalAcceptanceError(
            "stage runtime closure aggregate is inconsistent"
        )

    sentinels = closure["required_sentinels"]
    if not isinstance(sentinels, dict):
        raise ExternalAcceptanceError(
            "stage runtime required sentinels are malformed"
        )
    for name, raw_sentinel in sentinels.items():
        sentinel = _exact_mapping(
            raw_sentinel,
            {"root", "path", "kind", "sha256"},
            f"stage runtime sentinel {name}",
        )
        if (
            not isinstance(name, str)
            or not name
            or not isinstance(sentinel["root"], str)
            or sentinel["root"] not in roots
            or not isinstance(sentinel["path"], str)
            or not sentinel["path"]
            or not isinstance(sentinel["kind"], str)
            or sentinel["kind"] not in {"file", "dir"}
        ):
            raise ExternalAcceptanceError(
                f"stage runtime sentinel {name!r} is malformed"
            )
        if sentinel["kind"] == "file":
            _digest(sentinel["sha256"], f"stage runtime sentinel {name} hash")
        elif sentinel["sha256"] is not None:
            raise ExternalAcceptanceError(
                f"stage runtime directory sentinel {name} has a hash"
            )

    loader = _exact_mapping(
        closure["loader_dependencies"],
        {"entry_count", "aggregate_sha256", "entries"},
        "stage runtime loader closure",
    )
    loader_entries = loader["entries"]
    loader_count = _nonnegative_integer(
        loader["entry_count"], "stage runtime loader entry count"
    )
    if not isinstance(loader_entries, list) or len(loader_entries) != loader_count:
        raise ExternalAcceptanceError(
            "stage runtime loader entry count is inconsistent"
        )
    loader_records: list[str] = []
    loader_keys: list[tuple[bytes, bytes]] = []
    for index, raw_entry in enumerate(loader_entries):
        entry = _exact_mapping(
            raw_entry,
            {"source", "staged", "size", "sha256"},
            f"stage runtime loader entry {index}",
        )
        if (
            not isinstance(entry["source"], str)
            or not Path(entry["source"]).is_absolute()
            or not isinstance(entry["staged"], str)
            or not entry["staged"]
        ):
            raise ExternalAcceptanceError(
                f"stage runtime loader entry {index} is malformed"
            )
        _nonnegative_integer(
            entry["size"], f"stage runtime loader entry {index} size"
        )
        _digest(
            entry["sha256"], f"stage runtime loader entry {index} hash"
        )
        loader_keys.append(
            (entry["staged"].encode("utf-8"), entry["source"].encode("utf-8"))
        )
        loader_records.append(_loader_entry_record(entry))
    if len(set(loader_keys)) != len(loader_keys) or loader_keys != sorted(
        loader_keys
    ):
        raise ExternalAcceptanceError(
            "stage runtime loader entries are not canonical"
        )
    loader_aggregate = _digest(
        loader["aggregate_sha256"], "stage runtime loader aggregate"
    )
    if loader_aggregate != _sha256_bytes(
        "".join(loader_records).encode("utf-8")
    ):
        raise ExternalAcceptanceError(
            "stage runtime loader aggregate is inconsistent"
        )
    return {
        "algorithm": RUNTIME_CLOSURE_ALGORITHM,
        "schema_revision": RUNTIME_CLOSURE_SCHEMA_REVISION,
        "entry_count": entry_count,
        "aggregate_sha256": aggregate,
    }


def _stage_member(
    stage_root: Path,
    manifest: Mapping[str, Any],
    member: str,
) -> Path:
    staged = manifest.get("staged")
    if not isinstance(staged, dict) or not isinstance(
        staged.get(member), str
    ):
        raise ExternalAcceptanceError(
            f"stage manifest is missing staged.{member}"
        )
    relative = Path(staged[member])
    if relative.is_absolute() or ".." in relative.parts:
        raise ExternalAcceptanceError(
            f"stage manifest has unsafe staged.{member}: {relative}"
        )
    return _real_directory(
        stage_root / relative,
        f"staged {member}",
        create=False,
    )


def _validate_stage_manifest(
    manifest: Mapping[str, Any],
    *,
    stage_root: Path,
    aggregate_path: Path,
    control_file: Path,
    install_sql: Path,
    dso: Path,
) -> dict[str, object]:
    manifest = _exact_mapping(
        manifest,
        {
            "schema",
            "schema_revision",
            "postgresql_version",
            "postgresql_configure",
            "source",
            "staged",
            "runtime_closure",
            "extension_artifacts",
            "expected_tool_paths",
            "tool_signatures",
        },
        "external stage manifest",
    )
    revision = _nonnegative_integer(
        manifest["schema_revision"], "external stage manifest schema revision"
    )
    if (
        manifest["schema"] != STAGE_MANIFEST_SCHEMA
        or revision != STAGE_MANIFEST_SCHEMA_REVISION
    ):
        raise ExternalAcceptanceError("stage manifest schema is invalid")
    version = manifest["postgresql_version"]
    if not isinstance(version, str) or not version.startswith("PostgreSQL 17"):
        raise ExternalAcceptanceError(
            f"staged server is not PostgreSQL 17: {version!r}"
        )
    configure = manifest["postgresql_configure"]
    if not isinstance(configure, str) or not configure:
        raise ExternalAcceptanceError(
            "stage manifest PostgreSQL configure identity is invalid"
        )
    source = _exact_mapping(
        manifest["source"],
        {"bindir", "sharedir", "pkglibdir"},
        "external stage source roots",
    )
    staged = _exact_mapping(
        manifest["staged"],
        {"bindir", "sharedir", "pkglibdir"},
        "external staged roots",
    )
    for name in ("bindir", "sharedir", "pkglibdir"):
        if (
            not isinstance(source[name], str)
            or not Path(source[name]).is_absolute()
            or not isinstance(staged[name], str)
            or not staged[name]
            or Path(staged[name]).is_absolute()
            or ".." in Path(staged[name]).parts
        ):
            raise ExternalAcceptanceError(
                f"stage manifest {name} root is malformed"
            )
    runtime_summary = _validate_runtime_closure(
        manifest["runtime_closure"], source=source, staged=staged
    )

    extension_closure = _exact_mapping(
        manifest["extension_artifacts"],
        {"aggregate_sha256", "artifacts"},
        "external stage artifact closure",
    )
    artifacts = extension_closure["artifacts"]
    if not isinstance(artifacts, list) or len(artifacts) != 1:
        raise ExternalAcceptanceError(
            "stage manifest must contain exactly one Bioseq artifact"
        )
    artifact = _exact_mapping(
        artifacts[0],
        {
            "name",
            "module_pathname_override",
            "effective_module_name",
            "control",
            "install_sql",
            "library",
        },
        "external staged artifact",
    )
    control = _exact_mapping(
        artifact["control"],
        {"source", "filename", "staged", "source_sha256", "sha256"},
        "external staged control file",
    )
    sql = _exact_mapping(
        artifact["install_sql"],
        {"source", "filename", "staged", "version", "sha256"},
        "external staged install SQL",
    )
    library = _exact_mapping(
        artifact["library"],
        {"source", "filename", "staged", "sha256"},
        "external staged library",
    )
    package_hashes = {
        "control": _sha256(control_file),
        "install_sql": _sha256(install_sql),
        "library": _sha256(dso),
    }
    expected_artifact = {
        "name": "bioseq",
        "module_pathname_override": None,
        "effective_module_name": dso.name.split(".", 1)[0],
        "control": {
            "source": str(control_file),
            "filename": control_file.name,
            "staged": f"{staged['sharedir']}/extension/{control_file.name}",
            "source_sha256": package_hashes["control"],
            "sha256": package_hashes["control"],
        },
        "install_sql": {
            "source": str(install_sql),
            "filename": install_sql.name,
            "staged": f"{staged['sharedir']}/extension/{install_sql.name}",
            "version": "1.0",
            "sha256": package_hashes["install_sql"],
        },
        "library": {
            "source": str(dso),
            "filename": dso.name,
            "staged": f"{staged['pkglibdir']}/{dso.name}",
            "sha256": package_hashes["library"],
        },
    }
    if {
        **artifact,
        "control": control,
        "install_sql": sql,
        "library": library,
    } != expected_artifact:
        raise ExternalAcceptanceError(
            "staged Bioseq artifact does not match the current build input"
        )
    expected_aggregate = _stage_package_aggregate(
        control_file=control_file,
        install_sql=install_sql,
        dso=dso,
        control_sha256=package_hashes["control"],
        install_sql_sha256=package_hashes["install_sql"],
        library_sha256=package_hashes["library"],
    )
    if extension_closure["aggregate_sha256"] != expected_aggregate:
        raise ExternalAcceptanceError(
            "stage manifest aggregate does not bind the current artifact set"
        )
    aggregate_text = aggregate_path.read_text(
        encoding="ascii", errors="strict"
    )
    if aggregate_text != expected_aggregate + "\n":
        raise ExternalAcceptanceError(
            "stage aggregate file differs from the independently "
            "recomputed package closure"
        )
    tool_paths = manifest["expected_tool_paths"]
    signatures = manifest["tool_signatures"]
    if not isinstance(tool_paths, dict) or not isinstance(signatures, list):
        raise ExternalAcceptanceError(
            "stage manifest has no exact tool closure"
        )
    signatures_by_name: dict[str, Mapping[str, Any]] = {}
    for index, raw_signature in enumerate(signatures):
        signature = _exact_mapping(
            raw_signature,
            {"tool", "staged_path", "version", "sha256"},
            f"external stage tool signature {index}",
        )
        name = signature["tool"]
        if not isinstance(name, str) or name in signatures_by_name:
            raise ExternalAcceptanceError(
                "stage manifest has an invalid or duplicate tool signature"
            )
        signatures_by_name[name] = signature
    expected_tools = POSTGRESQL_TOOLS
    if set(tool_paths) != expected_tools or set(signatures_by_name) != expected_tools:
        raise ExternalAcceptanceError(
            "stage manifest tool closure is not exact"
        )
    staged_tool_hashes: dict[str, str] = {}
    for name in sorted(expected_tools):
        relative_text = tool_paths[name]
        if not isinstance(relative_text, str):
            raise ExternalAcceptanceError(
                f"stage tool path is malformed: {name}"
            )
        relative = Path(relative_text)
        if relative.is_absolute() or ".." in relative.parts:
            raise ExternalAcceptanceError(
                f"stage tool path is unsafe: {relative}"
            )
        tool = _regular_file(
            stage_root / relative,
            f"staged PostgreSQL {name}",
            executable=True,
        )
        digest = _sha256(tool)
        signature = signatures_by_name[name]
        if (
            signature["staged_path"] != relative_text
            or not isinstance(signature["version"], str)
            or not signature["version"]
            or signature["sha256"] != digest
        ):
            raise ExternalAcceptanceError(
                f"staged PostgreSQL {name} signature is stale"
            )
        staged_tool_hashes[name] = digest
    return {
        "schema": STAGE_MANIFEST_SCHEMA,
        "schema_revision": STAGE_MANIFEST_SCHEMA_REVISION,
        "postgresql_version": version,
        "postgresql_configure": configure,
        "runtime_closure": runtime_summary,
        "package_aggregate_sha256": expected_aggregate,
        "stage_manifest_sha256": None,
        "staged_tool_sha256": staged_tool_hashes,
    }


def _psql_command(
    cluster: UniquePrivateCluster,
    bindir: Path,
    script: Path,
) -> list[str]:
    return [
        str(bindir / "psql"),
        "-X",
        "-q",
        "-v",
        "ON_ERROR_STOP=1",
        "--host",
        str(cluster.socket_dir),
        "--port",
        str(cluster.port),
        "--username",
        cluster.database_user,
        "--dbname",
        cluster.database,
        "--file",
        str(script),
    ]


def _run_psql(
    *,
    label: str,
    sql: str,
    cluster: UniquePrivateCluster,
    bindir: Path,
    retained_root: Path,
    timeout: int,
) -> bytes:
    query_path = retained_root / "queries" / f"{label}.sql"
    stdout_path = retained_root / "results" / f"{label}.stdout"
    stderr_path = retained_root / "results" / f"{label}.stderr"
    _write_bytes(query_path, sql.encode("utf-8"))
    return _run_checked(
        _psql_command(cluster, bindir, query_path),
        timeout=timeout,
        env=cluster.child_environment(),
        stdout_path=stdout_path,
        stderr_path=stderr_path,
    )


def _validate_preflight(
    row: Mapping[str, str],
    *,
    data_directory: Path,
    socket_directory: Path,
) -> dict[str, object]:
    expected_zero = (
        "extension_count",
        "fixed_schema_count",
        "access_method_count",
        "handler_count",
    )
    expected_fields = {
        "server_version",
        "server_version_num",
        "system_identifier",
        "data_directory",
        "socket_directory",
        "unix_socket_connection",
        *expected_zero,
    }
    if set(row) != expected_fields:
        raise ExternalAcceptanceError(
            "clean-cluster preflight fields are not exact"
        )
    version_num = str(row.get("server_version_num", ""))
    if (
        re.fullmatch(r"[0-9]{6}", version_num) is None
        or not version_num.startswith("17")
    ):
        raise ExternalAcceptanceError(
            f"live server is not PostgreSQL 17: {dict(row)}"
        )
    for field in expected_zero:
        if row.get(field) != "0":
            raise ExternalAcceptanceError(
                f"clean-cluster preflight found pre-existing {field}: "
                f"{row.get(field)!r}"
            )
    system_identifier = row.get("system_identifier", "")
    if re.fullmatch(r"[0-9]+", system_identifier) is None:
        raise ExternalAcceptanceError(
            "clean cluster returned no canonical system identifier"
        )
    expected_data_directory = str(data_directory.resolve(strict=True))
    expected_socket_directory = str(socket_directory.resolve(strict=True))
    if row.get("data_directory") != expected_data_directory:
        raise ExternalAcceptanceError(
            "live PostgreSQL data_directory is not the owned private directory"
        )
    if row.get("socket_directory") != expected_socket_directory:
        raise ExternalAcceptanceError(
            "live PostgreSQL unix_socket_directories is not the owned socket"
        )
    if row.get("unix_socket_connection") != "t":
        raise ExternalAcceptanceError(
            "live acceptance connection did not use the private Unix socket"
        )
    return {
        "server_version": row.get("server_version"),
        "server_version_num": version_num,
        "system_identifier": system_identifier,
        "data_directory": expected_data_directory,
        "socket_directory": expected_socket_directory,
        "unix_socket_connection": True,
        **{field: 0 for field in expected_zero},
    }


def _validate_handler(row: Mapping[str, str]) -> dict[str, object]:
    expected = {
        "schema_name": "bioseq",
        "routine_name": "bioseq_external_handler",
        "argument_types": "internal",
        "result_schema": "pg_catalog",
        "result_type": "table_am_handler",
        "language_name": "c",
        "source_symbol": "bioseq_external_handler",
        "binary_path": "$libdir/bioseq",
        "strict": "t",
        "routine_kind": "f",
    }
    if dict(row) != expected:
        raise ExternalAcceptanceError(
            "live external handler catalog row differs from the exact "
            f"contract: {dict(row)}"
        )
    return dict(expected)


def _validate_access_method(row: Mapping[str, str]) -> dict[str, object]:
    expected = {
        "access_method": "bioseq_external",
        "access_method_type": "t",
        "handler_schema": "bioseq",
        "handler_name": "bioseq_external_handler",
        "handler_arguments": "internal",
    }
    if dict(row) != expected:
        raise ExternalAcceptanceError(
            "live pg_am row differs from the exact external contract: "
            f"{dict(row)}"
        )
    return dict(expected)


def _normalize_table_rows(
    rows: Sequence[Mapping[str, str]],
) -> list[dict[str, object]]:
    normalized: list[dict[str, object]] = []
    for row in rows:
        try:
            normalized.append({
                "schema_name": row["schema_name"],
                "table_name": row["table_name"],
                "relkind": row["relkind"],
                "persistence": row["persistence"],
                "access_method": row["access_method"],
                "ordinal": int(row["ordinal"]),
                "column_name": row["column_name"],
                "type_schema": row["type_schema"],
                "type_name": row["type_name"],
                "not_null": row["not_null"] == "t",
                "relation_size": int(row["relation_size"]),
            })
        except (KeyError, TypeError, ValueError) as error:
            raise ExternalAcceptanceError(
                f"malformed live table catalog row: {dict(row)}"
            ) from error
    return normalized


def _validate_table_catalog(
    rows: Sequence[Mapping[str, str]],
) -> dict[str, object]:
    normalized = _normalize_table_rows(rows)
    expected = _expected_catalog_rows()
    if normalized != expected:
        raise ExternalAcceptanceError(
            "live external table names, order, types, nullability, AM, "
            "persistence, or zero-storage contract differs"
        )
    return {
        "table_count": len(EXPECTED_TABLES),
        "column_count": EXPECTED_COLUMN_COUNT,
        "ordered_catalog_sha256": _sha256_bytes(
            _canonical_json(normalized)
        ),
        "all_relation_sizes_zero": all(
            row["relation_size"] == 0 for row in normalized
        ),
    }


def _validate_external_option_catalog(
    rows: Sequence[Mapping[str, str]],
) -> dict[str, object]:
    normalized: list[dict[str, object]] = []
    for row in rows:
        if set(row) != {
            "relation",
            "entity",
            "alphabet_is_null",
            "storage_width",
            "chunk_bases",
        }:
            raise ExternalAcceptanceError(
                "live external option catalog row fields are not exact"
            )
        try:
            alphabet_is_null = _boolean(
                row["alphabet_is_null"],
                "external option alphabet nullability",
            )
            normalized.append({
                "relation": row["relation"],
                "entity": row["entity"],
                "alphabet": None if alphabet_is_null else "<present>",
                "storage_width": int(row["storage_width"]),
                "chunk_bases": int(row["chunk_bases"]),
            })
        except (KeyError, TypeError, ValueError) as error:
            raise ExternalAcceptanceError(
                f"malformed live external option row: {dict(row)}"
            ) from error
    expected = _expected_external_option_rows()
    if normalized != expected:
        raise ExternalAcceptanceError(
            "generated external route-to-entity catalog rows or typed defaults "
            "differ from the exact six-table contract"
        )
    return {
        "catalog_table": EXTERNAL_OPTIONS_TABLE,
        "row_count": len(expected),
        "rows": normalized,
    }


def _validate_extension(row: Mapping[str, str]) -> dict[str, object]:
    expected = {
        "extension_name": "bioseq",
        "extension_schema": INSTALL_SCHEMA,
        "relocatable": "t",
        "version": "1.0",
        "bioseq_schema_count": "1",
        "external_schema_count": "1",
        "external_table_count": str(len(EXPECTED_TABLES)),
        "external_tables_in_install_schema": "0",
        "handler_in_install_schema": "0",
    }
    if dict(row) != expected:
        raise ExternalAcceptanceError(
            "extension relocation/fixed-schema catalog differs: "
            f"{dict(row)}"
        )
    return {
        **expected,
        "relocatable": True,
        "bioseq_schema_count": 1,
        "external_schema_count": 1,
        "external_table_count": len(EXPECTED_TABLES),
        "external_tables_in_install_schema": 0,
        "handler_in_install_schema": 0,
    }


def _validate_acl(row: Mapping[str, str]) -> dict[str, object]:
    expected = {
        "schema_usage": "f",
        "handler_execute": "f",
        "selectable_table_count": "0",
        "table_count": str(len(EXPECTED_TABLES)),
    }
    if dict(row) != expected:
        raise ExternalAcceptanceError(
            f"non-owner effective ACLs differ: {dict(row)}"
        )
    return {
        "schema_usage": False,
        "handler_execute": False,
        "selectable_table_count": 0,
        "table_count": len(EXPECTED_TABLES),
    }


def _extract_sqlstate(value: bytes, marker: str) -> str:
    text = value.decode("utf-8", errors="strict")
    matches = re.findall(
        rf"(?m)^{re.escape(marker)}=([0-9A-Z]{{5}})$",
        text,
    )
    if len(matches) != 1:
        raise ExternalAcceptanceError(
            f"expected exactly one {marker} marker, got {matches!r}"
        )
    return matches[0]


def _extract_error_message(value: bytes, marker: str) -> str:
    text = value.decode("utf-8", errors="strict")
    matches = re.findall(
        rf"(?m)^{re.escape(marker)}=(.+)$",
        text,
    )
    if len(matches) != 1:
        raise ExternalAcceptanceError(
            f"expected exactly one {marker} marker, got {matches!r}"
        )
    return matches[0]


def _evidence_file_inventory(root: Path) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    for path in sorted(root.rglob("*")):
        metadata = path.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            raise ExternalAcceptanceError(
                f"evidence bundle contains a symlink: {path}"
            )
        if stat.S_ISDIR(metadata.st_mode):
            continue
        if not stat.S_ISREG(metadata.st_mode):
            raise ExternalAcceptanceError(
                f"evidence bundle contains a non-regular entry: {path}"
            )
        if metadata.st_nlink != 1:
            raise ExternalAcceptanceError(
                f"evidence bundle contains a hard-linked file: {path}"
            )
        relative = path.relative_to(root).as_posix()
        if relative in {"receipt.json", "receipt.sha256"}:
            continue
        result[relative] = {
            "sha256": _sha256(path),
            "size": path.stat().st_size,
        }
    return result


def _validate_evidence_files(
    bundle_root: Path,
    inventory: Mapping[str, Any],
) -> None:
    actual = _evidence_file_inventory(bundle_root)
    if dict(inventory) != actual:
        raise ExternalAcceptanceError(
            "receipt evidence-file closure is stale or incomplete"
        )


def _exact_mapping(
    value: object,
    fields: set[str] | frozenset[str],
    label: str,
) -> Mapping[str, Any]:
    if not isinstance(value, dict) or set(value) != set(fields):
        raise ExternalAcceptanceError(
            f"{label} fields are not exact"
        )
    return value


def _digest(value: object, label: str) -> str:
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise ExternalAcceptanceError(f"{label} is not a canonical SHA-256")
    return value


def _nonnegative_integer(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ExternalAcceptanceError(f"{label} is not a nonnegative integer")
    return value


def _validate_receipt_file_record(
    value: object,
    label: str,
    *,
    require_repository_path: bool = False,
    allow_repository_path: bool = True,
) -> Path:
    if require_repository_path:
        expected_fields = {"path", "sha256", "size", "repository_path"}
    elif allow_repository_path:
        if not isinstance(value, dict):
            raise ExternalAcceptanceError(f"{label} record is not an object")
        expected_fields = {"path", "sha256", "size"}
        if "repository_path" in value:
            expected_fields.add("repository_path")
    else:
        expected_fields = {"path", "sha256", "size"}
    record = _exact_mapping(value, expected_fields, f"{label} record")
    if not isinstance(record["path"], str) or not record["path"]:
        raise ExternalAcceptanceError(f"{label} path is malformed")
    path = _regular_file(record["path"], label)
    if _digest(record["sha256"], f"{label} hash") != _sha256(path):
        raise ExternalAcceptanceError(f"{label} hash is stale")
    size = _nonnegative_integer(record["size"], f"{label} size")
    if size != path.stat().st_size:
        raise ExternalAcceptanceError(f"{label} size is stale")
    repository_path = record.get("repository_path")
    if repository_path is not None:
        if not isinstance(repository_path, str):
            raise ExternalAcceptanceError(
                f"{label} repository path is malformed"
            )
        relative = Path(repository_path)
        if (
            not repository_path
            or relative.is_absolute()
            or repository_path != relative.as_posix()
            or "\\" in repository_path
            or ".." in relative.parts
        ):
            raise ExternalAcceptanceError(
                f"{label} repository path is not canonical"
            )
    return path


def _repository_root_from_inputs(
    files: Mapping[str, Any],
    paths: Mapping[str, Path],
) -> Path:
    roots: set[Path] = set()
    for source_path in sorted(EXTERNAL_AUTHORITY_SOURCE_PATHS):
        key = f"external_source:{source_path}"
        record = files[key]
        if record.get("repository_path") != source_path:
            raise ExternalAcceptanceError(
                "external source input has the wrong repository path"
            )
        candidate = paths[key]
        for _ in Path(source_path).parts:
            candidate = candidate.parent
        candidate = _real_directory(
            candidate, "receipt repository root", create=False
        )
        if (candidate / source_path).resolve(strict=True) != paths[key]:
            raise ExternalAcceptanceError(
                "external source input does not resolve under one repository"
            )
        roots.add(candidate)
    if len(roots) != 1:
        raise ExternalAcceptanceError(
            "external source inputs do not share one repository root"
        )
    return next(iter(roots))


def _validate_postgresql_base(
    value: object,
    *,
    pg_config_path: Path,
) -> Mapping[str, Any]:
    provenance = _exact_mapping(
        value, {"pg_config", "tools"}, "input PostgreSQL provenance"
    )
    pg_config = _exact_mapping(
        provenance["pg_config"],
        {"version", "configure", "bindir", "sharedir", "pkglibdir"},
        "input pg_config provenance",
    )
    for field in ("version", "configure", "bindir", "sharedir", "pkglibdir"):
        if not isinstance(pg_config[field], str) or not pg_config[field]:
            raise ExternalAcceptanceError(
                f"input pg_config {field} is malformed"
            )
    if not pg_config["version"].startswith("PostgreSQL 17"):
        raise ExternalAcceptanceError(
            "input pg_config does not identify PostgreSQL 17"
        )
    bindir = _real_directory(
        pg_config["bindir"], "receipt PostgreSQL bindir", create=False
    )
    _real_directory(
        pg_config["sharedir"], "receipt PostgreSQL sharedir", create=False
    )
    _real_directory(
        pg_config["pkglibdir"], "receipt PostgreSQL pkglibdir", create=False
    )
    if pg_config_path != bindir / "pg_config":
        raise ExternalAcceptanceError(
            "input pg_config executable does not belong to its declared bindir"
        )
    tools = _exact_mapping(
        provenance["tools"], POSTGRESQL_TOOLS, "input PostgreSQL tools"
    )
    tool_paths: set[Path] = set()
    for name in sorted(POSTGRESQL_TOOLS):
        path = _validate_receipt_file_record(
            tools[name],
            f"input PostgreSQL {name}",
            allow_repository_path=False,
        )
        if path != bindir / name:
            raise ExternalAcceptanceError(
                f"input PostgreSQL {name} is outside the declared bindir"
            )
        tool_paths.add(path)
    if len(tool_paths) != len(POSTGRESQL_TOOLS):
        raise ExternalAcceptanceError(
            "input PostgreSQL tool paths are not one-to-one"
        )
    return provenance


def _validate_input_provenance(
    value: object,
) -> tuple[Mapping[str, Any], dict[str, Path], Path]:
    inputs = _exact_mapping(
        value,
        {"files", "git", "postgresql", "seal_sha256"},
        "receipt input provenance",
    )
    input_core = {
        "files": inputs["files"],
        "git": inputs["git"],
        "postgresql": inputs["postgresql"],
    }
    if _digest(inputs["seal_sha256"], "input provenance seal") != (
        _sha256_bytes(_canonical_json(input_core))
    ):
        raise ExternalAcceptanceError(
            "acceptance receipt input-provenance seal is stale"
        )
    files = _exact_mapping(
        inputs["files"], EXPECTED_INPUT_FILE_KEYS, "receipt input files"
    )
    paths: dict[str, Path] = {}
    for name in sorted(EXPECTED_INPUT_FILE_KEYS):
        paths[name] = _validate_receipt_file_record(
            files[name],
            f"receipt input {name}",
            require_repository_path=name.startswith("external_source:"),
        )
    git = _exact_mapping(
        inputs["git"], {"revision", "source_status"}, "receipt Git provenance"
    )
    if (
        not isinstance(git["revision"], str)
        or re.fullmatch(r"[0-9a-f]{40}", git["revision"]) is None
        or not isinstance(git["source_status"], list)
        or not all(isinstance(line, str) for line in git["source_status"])
    ):
        raise ExternalAcceptanceError("receipt Git provenance is malformed")
    _validate_postgresql_base(
        inputs["postgresql"], pg_config_path=paths["pg_config"]
    )
    repository_root = _repository_root_from_inputs(files, paths)
    return inputs, paths, repository_root


def _validate_receipt_package(
    value: object,
    *,
    input_files: Mapping[str, Any],
    input_paths: Mapping[str, Path],
    repository_root: Path,
) -> None:
    manifest = _load_json(
        input_paths["manifest"], "receipt generated SQL manifest"
    )
    expected = _validate_generated_manifest(
        manifest,
        install_sql=input_paths["install_sql"],
        repository_root=repository_root,
    )
    if not isinstance(value, dict) or value != expected:
        raise ExternalAcceptanceError(
            "receipt generated-package projection is not exact"
        )
    source_provenance = value["external_source_provenance"]
    for source_path in sorted(EXTERNAL_AUTHORITY_SOURCE_PATHS):
        if source_provenance[source_path] != input_files[
            f"external_source:{source_path}"
        ]:
            raise ExternalAcceptanceError(
                "receipt source provenance does not join its input file"
            )


def _validate_receipt_postgresql(
    value: object,
    *,
    input_postgresql: Mapping[str, Any],
) -> Mapping[str, Any]:
    postgresql = _exact_mapping(
        value, {"pg_config", "tools", "live"}, "receipt PostgreSQL section"
    )
    if {
        "pg_config": postgresql["pg_config"],
        "tools": postgresql["tools"],
    } != dict(input_postgresql):
        raise ExternalAcceptanceError(
            "receipt PostgreSQL provenance does not join input provenance"
        )
    live = _exact_mapping(
        postgresql["live"],
        {
            "server_version",
            "server_version_num",
            "system_identifier",
            "data_directory",
            "socket_directory",
            "unix_socket_connection",
            "database",
            "database_user",
        },
        "receipt live PostgreSQL provenance",
    )
    if (
        not isinstance(live["server_version"], str)
        or not live["server_version"].startswith("17")
        or not isinstance(live["server_version_num"], str)
        or re.fullmatch(r"17[0-9]{4}", live["server_version_num"]) is None
        or not isinstance(live["system_identifier"], str)
        or re.fullmatch(r"[0-9]+", live["system_identifier"]) is None
        or live["unix_socket_connection"] is not True
        or live["database"] != "external_acceptance"
        or not isinstance(live["database_user"], str)
        or not live["database_user"]
    ):
        raise ExternalAcceptanceError(
            "receipt live PostgreSQL provenance is malformed"
        )
    for field in ("data_directory", "socket_directory"):
        if (
            not isinstance(live[field], str)
            or not Path(live[field]).is_absolute()
        ):
            raise ExternalAcceptanceError(
                f"receipt live PostgreSQL {field} is malformed"
            )
    return live


def _expected_read_only_receipt() -> dict[str, object]:
    if set(READ_ONLY_INSERT_SQL) != set(EXPECTED_TABLES):
        raise ExternalAcceptanceError(
            "read-only probe SQL does not close over the six external tables"
        )
    return {
        "operation": "insert",
        "expected_sqlstate": "0A000",
        "table_count": len(EXPECTED_TABLES),
        "table_sqlstates": {
            table: "0A000" for table in sorted(EXPECTED_TABLES)
        },
    }


def _validate_receipt_checks(
    value: object,
    *,
    live: Mapping[str, Any],
) -> Mapping[str, Any]:
    checks = _exact_mapping(
        value,
        {
            "clean_cluster",
            "extension",
            "handler",
            "access_method",
            "tables",
            "relation_options",
            "acl",
            "read_only",
            "empty_relation",
            "create_drop_using_am",
            "drop_extension",
        },
        "receipt checks",
    )
    clean = _exact_mapping(
        checks["clean_cluster"],
        {
            "server_version",
            "server_version_num",
            "system_identifier",
            "data_directory",
            "socket_directory",
            "unix_socket_connection",
            "extension_count",
            "fixed_schema_count",
            "access_method_count",
            "handler_count",
        },
        "receipt clean-cluster check",
    )
    if (
        any(clean[field] != 0 for field in (
            "extension_count",
            "fixed_schema_count",
            "access_method_count",
            "handler_count",
        ))
        or {
            field: clean[field]
            for field in (
                "server_version",
                "server_version_num",
                "system_identifier",
                "data_directory",
                "socket_directory",
                "unix_socket_connection",
            )
        } != {
            field: live[field]
            for field in (
                "server_version",
                "server_version_num",
                "system_identifier",
                "data_directory",
                "socket_directory",
                "unix_socket_connection",
            )
        }
    ):
        raise ExternalAcceptanceError(
            "receipt clean-cluster evidence does not join live provenance"
        )
    expected_handler = {
        "schema_name": "bioseq",
        "routine_name": "bioseq_external_handler",
        "argument_types": "internal",
        "result_schema": "pg_catalog",
        "result_type": "table_am_handler",
        "language_name": "c",
        "source_symbol": "bioseq_external_handler",
        "binary_path": "$libdir/bioseq",
        "strict": "t",
        "routine_kind": "f",
    }
    expected_access_method = {
        "access_method": "bioseq_external",
        "access_method_type": "t",
        "handler_schema": "bioseq",
        "handler_name": "bioseq_external_handler",
        "handler_arguments": "internal",
    }
    exact_checks = {
        "extension": {
            "extension_name": "bioseq",
            "extension_schema": INSTALL_SCHEMA,
            "relocatable": True,
            "version": "1.0",
            "bioseq_schema_count": 1,
            "external_schema_count": 1,
            "external_table_count": len(EXPECTED_TABLES),
            "external_tables_in_install_schema": 0,
            "handler_in_install_schema": 0,
        },
        "handler": expected_handler,
        "access_method": expected_access_method,
        "tables": {
            "table_count": len(EXPECTED_TABLES),
            "column_count": EXPECTED_COLUMN_COUNT,
            "ordered_catalog_sha256": EXPECTED_CATALOG_SHA256,
            "all_relation_sizes_zero": True,
        },
        "relation_options": {
            "catalog_table": EXTERNAL_OPTIONS_TABLE,
            "row_count": len(EXPECTED_RELATION_ENTITIES),
            "rows": _expected_external_option_rows(),
        },
        "acl": {
            "schema_usage": False,
            "handler_execute": False,
            "selectable_table_count": 0,
            "table_count": len(EXPECTED_TABLES),
            "denied_sqlstate": "42501",
        },
        "read_only": _expected_read_only_receipt(),
        "empty_relation": {
            "relation_size": 0,
            "false_predicate_row_count": 0,
        },
        "create_drop_using_am": {
            "created_with_access_method": "bioseq_external",
            "stored_entity": "taxonomy",
            "stored_alphabet": None,
            "stored_storage_width": 0,
            "stored_chunk_bases": 0,
            "relation_size": 0,
            "dropped": True,
            "stale_catalog_row_count": 0,
            "invalid_entity_sqlstate": INVALID_ENTITY_SQLSTATE,
            "invalid_relation_absent": True,
            "invalid_catalog_row_count": len(EXPECTED_RELATION_ENTITIES),
            "missing_entity_sqlstate": MISSING_ENTITY_SQLSTATE,
            "missing_entity_message": MISSING_ENTITY_MESSAGE,
            "missing_stored_entity": None,
            "missing_stored_alphabet": None,
            "missing_stored_storage_width": 0,
            "missing_stored_chunk_bases": 0,
            "missing_relation_dropped": True,
            "missing_stale_catalog_row_count": 0,
        },
        "drop_extension": {
            "extension_count": 0,
            "fixed_schema_count": 0,
            "access_method_count": 0,
            "handler_count": 0,
            "external_table_count": 0,
            "install_schema_count": 1,
        },
    }
    for name, expected in exact_checks.items():
        if checks[name] != expected:
            raise ExternalAcceptanceError(
                f"receipt {name} evidence is not exact"
            )
    return clean


def _validate_receipt_cluster(
    value: object,
    *,
    live: Mapping[str, Any],
    clean: Mapping[str, Any],
) -> None:
    cluster = _exact_mapping(
        value,
        {
            "fresh_data_directory",
            "private_socket",
            "ambient_pg_environment_stripped",
            "install_schema",
            "data_directory",
            "socket_directory",
            "data_directory_removed",
            "socket_directory_removed",
        },
        "receipt cluster section",
    )
    if (
        cluster["fresh_data_directory"] is not True
        or cluster["private_socket"] is not True
        or cluster["ambient_pg_environment_stripped"] is not True
        or cluster["install_schema"] != INSTALL_SCHEMA
        or cluster["data_directory_removed"] is not True
        or cluster["socket_directory_removed"] is not True
        or cluster["data_directory"] != live["data_directory"]
        or cluster["socket_directory"] != live["socket_directory"]
        or cluster["data_directory"] != clean["data_directory"]
        or cluster["socket_directory"] != clean["socket_directory"]
    ):
        raise ExternalAcceptanceError(
            "receipt private-cluster isolation/cleanup evidence is not exact"
        )


def _validate_evidence_inventory_schema(
    value: object,
) -> Mapping[str, Any]:
    if not isinstance(value, dict) or not value:
        raise ExternalAcceptanceError(
            "receipt has no transcript/file closure"
        )
    for relative, raw_record in value.items():
        if not isinstance(relative, str):
            raise ExternalAcceptanceError(
                "receipt evidence path is malformed"
            )
        path = Path(relative)
        if (
            not relative
            or path.is_absolute()
            or relative != path.as_posix()
            or "\\" in relative
            or ".." in path.parts
        ):
            raise ExternalAcceptanceError(
                "receipt evidence path is not canonical"
            )
        record = _exact_mapping(
            raw_record, {"sha256", "size"}, f"receipt evidence {relative}"
        )
        _digest(record["sha256"], f"receipt evidence {relative} hash")
        _nonnegative_integer(
            record["size"], f"receipt evidence {relative} size"
        )
    required = {
        "static/external-contract.stamp",
        "stage/artifact-spec.json",
        "stage/stage-manifest.json",
        "stage/stage-artifacts.sha256",
        *(
            item
            for table in EXPECTED_TABLES
            for item in (
                f"queries/read_only_insert_{table}.sql",
                f"results/read_only_insert_{table}.stdout",
                f"results/read_only_insert_{table}.stderr",
            )
        ),
        *(
            f"{directory}/{label}.{suffix}"
            for label in (
                "relation_options_catalog",
                "create_using_am",
                "created_using_am_catalog",
                "drop_using_am",
                "dropped_using_am_catalog",
                "invalid_entity_option",
                "invalid_entity_cleanup_catalog",
                "create_missing_entity",
                "missing_entity_catalog",
                "missing_entity_option",
                "missing_entity_cleanup_catalog",
            )
            for directory, suffix in (
                ("queries", "sql"),
                ("results", "stdout"),
                ("results", "stderr"),
            )
        ),
    }
    if not required.issubset(value):
        raise ExternalAcceptanceError(
            "receipt evidence omits an external read-only or stage transcript"
        )
    return value


def _validate_retained_stage_manifest(
    document: Mapping[str, Any],
    *,
    stage: Mapping[str, Any],
    pg_config: Mapping[str, Any],
    input_files: Mapping[str, Any],
) -> None:
    manifest = _exact_mapping(
        document,
        {
            "schema",
            "schema_revision",
            "postgresql_version",
            "postgresql_configure",
            "source",
            "staged",
            "runtime_closure",
            "extension_artifacts",
            "expected_tool_paths",
            "tool_signatures",
        },
        "retained stage manifest",
    )
    source = {
        "bindir": pg_config["bindir"],
        "sharedir": pg_config["sharedir"],
        "pkglibdir": pg_config["pkglibdir"],
    }
    staged = {
        key: str(value).lstrip("/")
        for key, value in source.items()
    }
    revision = _nonnegative_integer(
        manifest["schema_revision"], "retained stage manifest schema revision"
    )
    if (
        manifest["schema"] != STAGE_MANIFEST_SCHEMA
        or revision != STAGE_MANIFEST_SCHEMA_REVISION
        or manifest["postgresql_version"] != pg_config["version"]
        or manifest["postgresql_configure"] != pg_config["configure"]
        or manifest["source"] != source
        or manifest["staged"] != staged
    ):
        raise ExternalAcceptanceError(
            "retained stage manifest PostgreSQL projection is inconsistent"
        )
    runtime_summary = _validate_runtime_closure(
        manifest["runtime_closure"], source=source, staged=staged
    )
    if runtime_summary != stage["runtime_closure"]:
        raise ExternalAcceptanceError(
            "retained stage runtime closure does not join the receipt"
        )
    package_hashes = stage["package_input_sha256"]
    control_path = Path(input_files["control_file"]["path"])
    install_path = Path(input_files["install_sql"]["path"])
    library_path = Path(input_files["dso"]["path"])
    expected_artifacts = {
        "aggregate_sha256": stage["package_aggregate_sha256"],
        "artifacts": [{
            "name": "bioseq",
            "module_pathname_override": None,
            "effective_module_name": library_path.name.split(".", 1)[0],
            "control": {
                "source": str(control_path),
                "filename": control_path.name,
                "staged": (
                    f"{staged['sharedir']}/extension/{control_path.name}"
                ),
                "source_sha256": package_hashes["control"],
                "sha256": package_hashes["control"],
            },
            "install_sql": {
                "source": str(install_path),
                "filename": install_path.name,
                "staged": (
                    f"{staged['sharedir']}/extension/{install_path.name}"
                ),
                "version": "1.0",
                "sha256": package_hashes["install_sql"],
            },
            "library": {
                "source": str(library_path),
                "filename": library_path.name,
                "staged": f"{staged['pkglibdir']}/{library_path.name}",
                "sha256": package_hashes["library"],
            },
        }],
    }
    extension_artifacts = _exact_mapping(
        manifest["extension_artifacts"],
        {"aggregate_sha256", "artifacts"},
        "retained stage artifact closure",
    )
    if extension_artifacts != expected_artifacts:
        raise ExternalAcceptanceError(
            "retained stage manifest package closure is inconsistent"
        )
    expected_tool_paths = {
        name: f"{staged['bindir']}/{name}"
        for name in sorted(POSTGRESQL_TOOLS)
    }
    if manifest["expected_tool_paths"] != expected_tool_paths:
        raise ExternalAcceptanceError(
            "retained stage manifest tool paths are not exact"
        )
    signatures = manifest["tool_signatures"]
    if not isinstance(signatures, list) or len(signatures) != len(
        POSTGRESQL_TOOLS
    ):
        raise ExternalAcceptanceError(
            "retained stage manifest tool signatures are incomplete"
        )
    by_name: dict[str, Mapping[str, Any]] = {}
    for raw_signature in signatures:
        signature = _exact_mapping(
            raw_signature,
            {"tool", "staged_path", "version", "sha256"},
            "retained stage tool signature",
        )
        name = signature["tool"]
        if (
            not isinstance(name, str)
            or name not in POSTGRESQL_TOOLS
            or name in by_name
            or signature["staged_path"] != expected_tool_paths[name]
            or not isinstance(signature["version"], str)
            or not signature["version"]
            or signature["sha256"] != stage["staged_tool_sha256"][name]
        ):
            raise ExternalAcceptanceError(
                "retained stage tool signature is inconsistent"
            )
        by_name[name] = signature
    if set(by_name) != POSTGRESQL_TOOLS:
        raise ExternalAcceptanceError(
            "retained stage tool signatures are not exact"
        )


def _validate_receipt_stage(
    value: object,
    *,
    postgresql: Mapping[str, Any],
    input_files: Mapping[str, Any],
    evidence: Mapping[str, Any],
    bundle_root: Path | None,
) -> None:
    stage = _exact_mapping(
        value,
        {
            "schema",
            "schema_revision",
            "postgresql_version",
            "postgresql_configure",
            "artifact_spec",
            "runtime_closure",
            "package_aggregate_sha256",
            "stage_manifest_sha256",
            "staged_tool_sha256",
            "package_input_sha256",
        },
        "receipt stage section",
    )
    pg_config = postgresql["pg_config"]
    package_hashes = {
        "control": input_files["control_file"]["sha256"],
        "install_sql": input_files["install_sql"]["sha256"],
        "library": input_files["dso"]["sha256"],
    }
    control_path = Path(input_files["control_file"]["path"])
    install_path = Path(input_files["install_sql"]["path"])
    library_path = Path(input_files["dso"]["path"])
    aggregate = _stage_package_aggregate(
        control_file=control_path,
        install_sql=install_path,
        dso=library_path,
        control_sha256=package_hashes["control"],
        install_sql_sha256=package_hashes["install_sql"],
        library_sha256=package_hashes["library"],
    )
    revision = _nonnegative_integer(
        stage["schema_revision"], "receipt stage schema revision"
    )
    artifact_spec = _exact_mapping(
        stage["artifact_spec"],
        {"schema", "schema_revision", "sha256"},
        "receipt stage artifact specification",
    )
    artifact_revision = _nonnegative_integer(
        artifact_spec["schema_revision"],
        "receipt artifact specification schema revision",
    )
    artifact_spec_hash = _digest(
        artifact_spec["sha256"], "receipt artifact specification hash"
    )
    runtime = _exact_mapping(
        stage["runtime_closure"],
        {"algorithm", "schema_revision", "entry_count", "aggregate_sha256"},
        "receipt stage runtime closure",
    )
    runtime_revision = _nonnegative_integer(
        runtime["schema_revision"], "receipt runtime closure schema revision"
    )
    _nonnegative_integer(
        runtime["entry_count"], "receipt runtime closure entry count"
    )
    _digest(runtime["aggregate_sha256"], "receipt runtime closure aggregate")
    expected_tool_hashes = {
        name: postgresql["tools"][name]["sha256"]
        for name in sorted(POSTGRESQL_TOOLS)
    }
    if (
        stage["schema"] != STAGE_MANIFEST_SCHEMA
        or revision != STAGE_MANIFEST_SCHEMA_REVISION
        or artifact_spec["schema"] != ARTIFACT_SPEC_SCHEMA
        or artifact_revision != ARTIFACT_SPEC_SCHEMA_REVISION
        or runtime["algorithm"] != RUNTIME_CLOSURE_ALGORITHM
        or runtime_revision != RUNTIME_CLOSURE_SCHEMA_REVISION
        or stage["postgresql_version"] != pg_config["version"]
        or stage["postgresql_configure"] != pg_config["configure"]
        or stage["package_aggregate_sha256"] != aggregate
        or stage["package_input_sha256"] != package_hashes
        or stage["staged_tool_sha256"] != expected_tool_hashes
    ):
        raise ExternalAcceptanceError(
            "receipt stage provenance/cross-joins are inconsistent"
        )
    manifest_hash = _digest(
        stage["stage_manifest_sha256"], "receipt stage manifest hash"
    )
    if evidence["stage/stage-manifest.json"]["sha256"] != manifest_hash:
        raise ExternalAcceptanceError(
            "receipt stage manifest hash does not join evidence"
        )
    if evidence["stage/artifact-spec.json"]["sha256"] != artifact_spec_hash:
        raise ExternalAcceptanceError(
            "receipt artifact specification hash does not join evidence"
        )
    aggregate_bytes = (aggregate + "\n").encode("ascii")
    aggregate_evidence = evidence["stage/stage-artifacts.sha256"]
    if aggregate_evidence != {
        "sha256": _sha256_bytes(aggregate_bytes),
        "size": len(aggregate_bytes),
    }:
        raise ExternalAcceptanceError(
            "receipt stage aggregate does not join evidence"
        )
    if bundle_root is not None:
        artifact_spec_path = _regular_file(
            bundle_root / "stage" / "artifact-spec.json",
            "retained artifact specification",
        )
        stage_manifest_path = _regular_file(
            bundle_root / "stage" / "stage-manifest.json",
            "retained stage manifest",
        )
        aggregate_path = _regular_file(
            bundle_root / "stage" / "stage-artifacts.sha256",
            "retained stage aggregate",
        )
        if aggregate_path.read_bytes() != aggregate_bytes:
            raise ExternalAcceptanceError(
                "retained stage aggregate content is inconsistent"
            )
        _validate_artifact_spec(
            _load_json(
                artifact_spec_path, "retained artifact specification"
            ),
            control_file=control_path,
            install_sql=install_path,
            dso=library_path,
        )
        _validate_retained_stage_manifest(
            _load_json(stage_manifest_path, "retained stage manifest"),
            stage=stage,
            pg_config=pg_config,
            input_files=input_files,
        )


def _validate_receipt(
    receipt: Mapping[str, Any],
    *,
    bundle_root: Path | None = None,
) -> None:
    _exact_mapping(
        receipt,
        {
            "schema",
            "schema_revision",
            "outcome",
            "receipt_id",
            "generated_at_utc",
            "run_nonce",
            "input_provenance",
            "generated_package",
            "postgresql",
            "stage",
            "cluster",
            "checks",
            "evidence_files",
        },
        "acceptance receipt",
    )
    revision = _nonnegative_integer(
        receipt["schema_revision"], "acceptance receipt schema revision"
    )
    if (
        receipt["schema"] != RECEIPT_SCHEMA
        or revision != RECEIPT_SCHEMA_REVISION
        or receipt["outcome"] != "passed"
    ):
        raise ExternalAcceptanceError(
            "acceptance receipt outcome/schema is malformed"
        )
    generated_at = receipt["generated_at_utc"]
    try:
        parsed_generated_at = dt.datetime.fromisoformat(generated_at)
    except (TypeError, ValueError) as error:
        raise ExternalAcceptanceError(
            "acceptance receipt timestamp is malformed"
        ) from error
    if (
        not isinstance(generated_at, str)
        or parsed_generated_at.tzinfo is None
        or parsed_generated_at.utcoffset() != dt.timedelta(0)
        or parsed_generated_at.isoformat() != generated_at
    ):
        raise ExternalAcceptanceError(
            "acceptance receipt timestamp is not canonical UTC"
        )
    nonce = receipt["run_nonce"]
    try:
        parsed_nonce = uuid.UUID(hex=nonce)
    except (AttributeError, TypeError, ValueError) as error:
        raise ExternalAcceptanceError(
            "acceptance receipt nonce is malformed"
        ) from error
    if (
        not isinstance(nonce, str)
        or parsed_nonce.hex != nonce
        or parsed_nonce.version != 4
        or parsed_nonce.variant != uuid.RFC_4122
    ):
        raise ExternalAcceptanceError(
            "acceptance receipt nonce is not a canonical UUIDv4"
        )
    inputs, input_paths, repository_root = _validate_input_provenance(
        receipt["input_provenance"]
    )
    expected_receipt_id = _sha256_bytes(_canonical_json({
        "input_seal_sha256": inputs["seal_sha256"],
        "run_nonce": nonce,
    }))
    if (
        _digest(receipt["receipt_id"], "acceptance receipt ID")
        != expected_receipt_id
    ):
        raise ExternalAcceptanceError(
            "acceptance receipt ID does not bind inputs and nonce"
        )
    input_files = inputs["files"]
    _validate_receipt_package(
        receipt["generated_package"],
        input_files=input_files,
        input_paths=input_paths,
        repository_root=repository_root,
    )
    live = _validate_receipt_postgresql(
        receipt["postgresql"],
        input_postgresql=inputs["postgresql"],
    )
    clean = _validate_receipt_checks(receipt["checks"], live=live)
    _validate_receipt_cluster(
        receipt["cluster"], live=live, clean=clean
    )
    evidence = _validate_evidence_inventory_schema(
        receipt["evidence_files"]
    )
    _validate_receipt_stage(
        receipt["stage"],
        postgresql=receipt["postgresql"],
        input_files=input_files,
        evidence=evidence,
        bundle_root=bundle_root,
    )
    if bundle_root is not None:
        _validate_evidence_files(bundle_root, evidence)


def _stage_command(
    *,
    cmake: Path,
    pg_config: Path,
    stage_script: Path,
    stage_root: Path,
    stage_parent: Path,
    artifact_spec: Path,
    manifest_path: Path,
    aggregate_path: Path,
) -> list[str]:
    return [
        str(cmake),
        f"-DPG_CONFIG={pg_config}",
        f"-DSTAGE_ROOT={stage_root}",
        f"-DSTAGE_PARENT={stage_parent}",
        f"-DARTIFACT_SPEC={artifact_spec}",
        f"-DMANIFEST_OUT={manifest_path}",
        f"-DHASH_OUT={aggregate_path}",
        "-P",
        str(stage_script),
    ]


def _stage_package(
    *,
    cmake: Path,
    pg_config: Path,
    stage_script: Path,
    control_file: Path,
    install_sql: Path,
    dso: Path,
    stage_parent: Path,
    retained_root: Path,
    timeout: int,
) -> tuple[Path, dict[str, Any], dict[str, object]]:
    stage_root = stage_parent / "external"
    manifest_path = stage_root / "stage-manifest.json"
    aggregate_path = stage_root / "stage-artifacts.sha256"
    artifact_spec_path = stage_parent / "external-artifact-spec.json"
    artifact_spec = _artifact_spec_document(
        control_file=control_file,
        install_sql=install_sql,
        dso=dso,
    )
    _validate_artifact_spec(
        artifact_spec,
        control_file=control_file,
        install_sql=install_sql,
        dso=dso,
    )
    _write_json(artifact_spec_path, artifact_spec)
    _copy_file(
        artifact_spec_path,
        retained_root / "stage" / "artifact-spec.json",
    )
    _run_checked(
        _stage_command(
            cmake=cmake,
            pg_config=pg_config,
            stage_script=stage_script,
            stage_root=stage_root,
            stage_parent=stage_parent,
            artifact_spec=artifact_spec_path,
            manifest_path=manifest_path,
            aggregate_path=aggregate_path,
        ),
        timeout=timeout,
        stdout_path=retained_root / "results" / "stage.stdout",
        stderr_path=retained_root / "results" / "stage.stderr",
    )
    manifest_path = _regular_file(
        manifest_path, "external stage manifest"
    )
    aggregate_path = _regular_file(
        aggregate_path, "external stage aggregate"
    )
    manifest = _load_json(manifest_path, "external stage manifest")
    summary = _validate_stage_manifest(
        manifest,
        stage_root=stage_root,
        aggregate_path=aggregate_path,
        control_file=control_file,
        install_sql=install_sql,
        dso=dso,
    )
    _copy_file(
        manifest_path,
        retained_root / "stage" / "stage-manifest.json",
    )
    _copy_file(
        aggregate_path,
        retained_root / "stage" / "stage-artifacts.sha256",
    )
    summary["artifact_spec"] = {
        "schema": ARTIFACT_SPEC_SCHEMA,
        "schema_revision": ARTIFACT_SPEC_SCHEMA_REVISION,
        "sha256": _sha256(artifact_spec_path),
    }
    summary["stage_manifest_sha256"] = _sha256(manifest_path)
    return stage_root.resolve(strict=True), manifest, summary


def _boolean(value: str, label: str) -> bool:
    if value not in {"t", "f"}:
        raise ExternalAcceptanceError(
            f"{label} is not a PostgreSQL boolean: {value!r}"
        )
    return value == "t"


def _integer(value: str, label: str) -> int:
    try:
        return int(value)
    except ValueError as error:
        raise ExternalAcceptanceError(
            f"{label} is not an integer: {value!r}"
        ) from error


def _live_checks(
    *,
    cluster: UniquePrivateCluster,
    bindir: Path,
    retained_root: Path,
    timeout: int,
) -> tuple[dict[str, object], dict[str, object]]:
    preflight = _validate_preflight(
        _one_csv_record(
            _run_psql(
                label="preflight",
                cluster=cluster,
                bindir=bindir,
                retained_root=retained_root,
                timeout=timeout,
                sql="""
COPY (
  SELECT
    current_setting('server_version') AS server_version,
    current_setting('server_version_num') AS server_version_num,
    (SELECT system_identifier::text
       FROM pg_catalog.pg_control_system()) AS system_identifier,
    current_setting('data_directory') AS data_directory,
    current_setting('unix_socket_directories') AS socket_directory,
    pg_catalog.inet_server_addr() IS NULL AS unix_socket_connection,
    (SELECT count(*) FROM pg_catalog.pg_extension
      WHERE extname = 'bioseq') AS extension_count,
    (SELECT count(*) FROM pg_catalog.pg_namespace
      WHERE nspname IN (
        'bioseq', 'bioseq_external', 'external_acceptance_install'
      )) AS fixed_schema_count,
    (SELECT count(*) FROM pg_catalog.pg_am
      WHERE amname = 'bioseq_external') AS access_method_count,
    (SELECT count(*)
       FROM pg_catalog.pg_proc AS p
       JOIN pg_catalog.pg_namespace AS n ON n.oid = p.pronamespace
      WHERE n.nspname = 'bioseq'
        AND p.proname = 'bioseq_external_handler') AS handler_count
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
            ),
            "clean-cluster preflight",
        ),
        data_directory=cluster.data_dir,
        socket_directory=cluster.socket_dir,
    )

    _run_psql(
        label="install",
        cluster=cluster,
        bindir=bindir,
        retained_root=retained_root,
        timeout=timeout,
        sql=f"""
CREATE SCHEMA {INSTALL_SCHEMA};
CREATE ROLE {DENIED_ROLE} NOLOGIN;
CREATE EXTENSION bioseq SCHEMA {INSTALL_SCHEMA};
""",
    )

    extension = _validate_extension(_one_csv_record(
        _run_psql(
            label="extension_catalog",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql=f"""
COPY (
  SELECT
    e.extname AS extension_name,
    en.nspname AS extension_schema,
    e.extrelocatable AS relocatable,
    e.extversion AS version,
    (SELECT count(*) FROM pg_catalog.pg_namespace
      WHERE nspname = 'bioseq') AS bioseq_schema_count,
    (SELECT count(*) FROM pg_catalog.pg_namespace
      WHERE nspname = 'bioseq_external') AS external_schema_count,
    (SELECT count(*)
       FROM pg_catalog.pg_class AS c
       JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
      WHERE n.nspname = 'bioseq_external'
        AND c.relkind = 'r') AS external_table_count,
    (SELECT count(*)
       FROM pg_catalog.pg_class AS c
       JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
      WHERE n.nspname = '{INSTALL_SCHEMA}'
        AND c.relname IN (
          'taxonomy', 'catalog', 'record_genes', 'record_locations',
          'sequences', 'sequence_chunks'
        )) AS external_tables_in_install_schema,
    (SELECT count(*)
       FROM pg_catalog.pg_proc AS p
       JOIN pg_catalog.pg_namespace AS n ON n.oid = p.pronamespace
      WHERE n.nspname = '{INSTALL_SCHEMA}'
        AND p.proname = 'bioseq_external_handler')
      AS handler_in_install_schema
  FROM pg_catalog.pg_extension AS e
  JOIN pg_catalog.pg_namespace AS en ON en.oid = e.extnamespace
  WHERE e.extname = 'bioseq'
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
        ),
        "extension catalog",
    ))

    handler = _validate_handler(_one_csv_record(
        _run_psql(
            label="handler_catalog",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql="""
COPY (
  SELECT
    n.nspname AS schema_name,
    p.proname AS routine_name,
    pg_catalog.oidvectortypes(p.proargtypes) AS argument_types,
    rn.nspname AS result_schema,
    rt.typname AS result_type,
    l.lanname AS language_name,
    p.prosrc AS source_symbol,
    p.probin AS binary_path,
    p.proisstrict AS strict,
    p.prokind AS routine_kind
  FROM pg_catalog.pg_proc AS p
  JOIN pg_catalog.pg_namespace AS n ON n.oid = p.pronamespace
  JOIN pg_catalog.pg_type AS rt ON rt.oid = p.prorettype
  JOIN pg_catalog.pg_namespace AS rn ON rn.oid = rt.typnamespace
  JOIN pg_catalog.pg_language AS l ON l.oid = p.prolang
  WHERE n.nspname = 'bioseq'
    AND p.proname = 'bioseq_external_handler'
    AND pg_catalog.oidvectortypes(p.proargtypes) = 'internal'
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
        ),
        "handler catalog",
    ))

    access_method = _validate_access_method(_one_csv_record(
        _run_psql(
            label="access_method_catalog",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql="""
COPY (
  SELECT
    am.amname AS access_method,
    am.amtype AS access_method_type,
    n.nspname AS handler_schema,
    p.proname AS handler_name,
    pg_catalog.oidvectortypes(p.proargtypes) AS handler_arguments
  FROM pg_catalog.pg_am AS am
  JOIN pg_catalog.pg_proc AS p ON p.oid = am.amhandler
  JOIN pg_catalog.pg_namespace AS n ON n.oid = p.pronamespace
  WHERE am.amname = 'bioseq_external'
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
        ),
        "access method catalog",
    ))

    tables = _validate_table_catalog(_csv_records(
        _run_psql(
            label="table_catalog",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql="""
COPY (
  SELECT
    n.nspname AS schema_name,
    c.relname AS table_name,
    c.relkind,
    c.relpersistence AS persistence,
    am.amname AS access_method,
    a.attnum AS ordinal,
    a.attname AS column_name,
    tn.nspname AS type_schema,
    t.typname AS type_name,
    a.attnotnull AS not_null,
    pg_catalog.pg_relation_size(c.oid) AS relation_size
  FROM pg_catalog.pg_class AS c
  JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
  JOIN pg_catalog.pg_am AS am ON am.oid = c.relam
  JOIN pg_catalog.pg_attribute AS a
    ON a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
  JOIN pg_catalog.pg_type AS t ON t.oid = a.atttypid
  JOIN pg_catalog.pg_namespace AS tn ON tn.oid = t.typnamespace
  WHERE n.nspname = 'bioseq_external'
    AND c.relkind = 'r'
  ORDER BY c.relname, a.attnum
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
        ),
        "external table catalog",
    ))

    relation_options = _validate_external_option_catalog(_csv_records(
        _run_psql(
            label="relation_options_catalog",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql="""
COPY (
  SELECT
    pg_catalog.format('%I.%I', n.nspname, c.relname) AS relation,
    o.entity,
    o.alphabet IS NULL AS alphabet_is_null,
    o.storage_width,
    o.chunk_bases
  FROM bioseq_internal.external_relation_options AS o
  JOIN pg_catalog.pg_class AS c ON c.oid = o.regclass
  JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
  WHERE n.nspname = 'bioseq_external'
  ORDER BY c.relname
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
        ),
        "external relation option catalog",
    ))

    acl = _validate_acl(_one_csv_record(
        _run_psql(
            label="acl_summary",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql=f"""
COPY (
  SELECT
    pg_catalog.has_schema_privilege(
      '{DENIED_ROLE}', 'bioseq_external', 'USAGE'
    ) AS schema_usage,
    pg_catalog.has_function_privilege(
      '{DENIED_ROLE}',
      'bioseq.bioseq_external_handler(internal)',
      'EXECUTE'
    ) AS handler_execute,
    (SELECT count(*)
       FROM pg_catalog.pg_class AS c
       JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
      WHERE n.nspname = 'bioseq_external'
        AND c.relkind = 'r'
        AND pg_catalog.has_table_privilege(
          '{DENIED_ROLE}', c.oid, 'SELECT'
        )) AS selectable_table_count,
    (SELECT count(*)
       FROM pg_catalog.pg_class AS c
       JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
      WHERE n.nspname = 'bioseq_external'
        AND c.relkind = 'r') AS table_count
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
        ),
        "external ACL summary",
    ))
    denied_state = _extract_sqlstate(
        _run_psql(
            label="acl_denial",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql=f"""
SET ROLE {DENIED_ROLE};
\\set ON_ERROR_STOP off
SELECT count(*) FROM bioseq_external.taxonomy;
\\echo __BIOSEQ_ACL_SQLSTATE__=:SQLSTATE
\\set ON_ERROR_STOP on
RESET ROLE;
""",
        ),
        "__BIOSEQ_ACL_SQLSTATE__",
    )
    if denied_state != "42501":
        raise ExternalAcceptanceError(
            f"non-owner SELECT returned SQLSTATE {denied_state}, expected 42501"
        )
    acl["denied_sqlstate"] = denied_state

    read_only_sqlstates: dict[str, str] = {}
    for table in sorted(READ_ONLY_INSERT_SQL):
        marker = (
            "__BIOSEQ_READ_ONLY_"
            + table.upper()
            + "_SQLSTATE__"
        )
        state = _extract_sqlstate(
            _run_psql(
                label=f"read_only_insert_{table}",
                cluster=cluster,
                bindir=bindir,
                retained_root=retained_root,
                timeout=timeout,
                sql=(
                    "\\set ON_ERROR_STOP off\n"
                    + READ_ONLY_INSERT_SQL[table]
                    + f"\\echo {marker}=:SQLSTATE\n"
                    + "\\set ON_ERROR_STOP on\n"
                ),
            ),
            marker,
        )
        if state != "0A000":
            raise ExternalAcceptanceError(
                f"external INSERT into {table} returned SQLSTATE {state}, "
                "expected 0A000"
            )
        read_only_sqlstates[table] = state

    empty_row = _one_csv_record(
        _run_psql(
            label="empty_relation",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql="""
COPY (
  SELECT
    pg_catalog.pg_relation_size(
      'bioseq_external.taxonomy'::pg_catalog.regclass
    ) AS relation_size,
    (SELECT count(*) FROM bioseq_external.taxonomy WHERE false)
      AS false_predicate_row_count
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
        ),
        "empty external relation",
    )
    empty = {
        "relation_size": _integer(
            empty_row["relation_size"], "empty relation size"
        ),
        "false_predicate_row_count": _integer(
            empty_row["false_predicate_row_count"],
            "empty false-predicate row count",
        ),
    }
    if empty != {"relation_size": 0, "false_predicate_row_count": 0}:
        raise ExternalAcceptanceError(
            f"external relation is not storage-free and empty: {empty}"
        )

    _run_psql(
        label="create_using_am",
        cluster=cluster,
        bindir=bindir,
        retained_root=retained_root,
        timeout=timeout,
        sql=f"""
LOAD '$libdir/bioseq';
CREATE TABLE {LIFECYCLE_TABLE}
  (provider pg_catalog.text NOT NULL)
USING bioseq_external
WITH (entity = 'taxonomy');
""",
    )
    lifecycle_row = _one_csv_record(
        _run_psql(
            label="created_using_am_catalog",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql=f"""
COPY (
  SELECT
    c.oid::pg_catalog.text AS relation_oid,
    am.amname AS access_method,
    pg_catalog.pg_relation_size(c.oid) AS relation_size,
    o.entity AS stored_entity,
    o.alphabet IS NULL AS stored_alphabet_is_null,
    o.storage_width AS stored_storage_width,
    o.chunk_bases AS stored_chunk_bases
  FROM pg_catalog.pg_class AS c
  JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
  JOIN pg_catalog.pg_am AS am ON am.oid = c.relam
  JOIN bioseq_internal.external_relation_options AS o
    ON o.regclass = c.oid
  WHERE n.nspname = '{INSTALL_SCHEMA}'
    AND c.relname = 'external_lifecycle_probe'
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
        ),
        "created USING AM relation",
    )
    lifecycle_oid = _integer(
        lifecycle_row["relation_oid"], "lifecycle relation OID"
    )
    lifecycle = {
        "created_with_access_method": lifecycle_row["access_method"],
        "stored_entity": lifecycle_row["stored_entity"],
        "stored_alphabet": (
            None
            if _boolean(
                lifecycle_row["stored_alphabet_is_null"],
                "lifecycle stored alphabet nullability",
            )
            else "<present>"
        ),
        "stored_storage_width": _integer(
            lifecycle_row["stored_storage_width"],
            "lifecycle stored storage width",
        ),
        "stored_chunk_bases": _integer(
            lifecycle_row["stored_chunk_bases"],
            "lifecycle stored chunk bases",
        ),
        "relation_size": _integer(
            lifecycle_row["relation_size"], "lifecycle relation size"
        ),
        "dropped": False,
        "stale_catalog_row_count": -1,
    }
    _run_psql(
        label="drop_using_am",
        cluster=cluster,
        bindir=bindir,
        retained_root=retained_root,
        timeout=timeout,
        sql=f"LOAD '$libdir/bioseq';\nDROP TABLE {LIFECYCLE_TABLE};\n",
    )
    dropped_row = _one_csv_record(
        _run_psql(
            label="dropped_using_am_catalog",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql=f"""
COPY (
  SELECT
    pg_catalog.to_regclass('{LIFECYCLE_TABLE}') IS NULL AS dropped,
    (SELECT count(*)
       FROM bioseq_internal.external_relation_options AS o
      WHERE o.regclass = {lifecycle_oid}::pg_catalog.oid)
      AS stale_catalog_row_count
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
        ),
        "dropped USING AM relation",
    )
    lifecycle["dropped"] = _boolean(
        dropped_row["dropped"], "lifecycle dropped"
    )
    lifecycle["stale_catalog_row_count"] = _integer(
        dropped_row["stale_catalog_row_count"],
        "lifecycle stale catalog row count",
    )
    if lifecycle != {
        "created_with_access_method": "bioseq_external",
        "stored_entity": "taxonomy",
        "stored_alphabet": None,
        "stored_storage_width": 0,
        "stored_chunk_bases": 0,
        "relation_size": 0,
        "dropped": True,
        "stale_catalog_row_count": 0,
    }:
        raise ExternalAcceptanceError(
            f"create/drop USING AM contract differs: {lifecycle}"
        )

    invalid_marker = "__BIOSEQ_INVALID_ENTITY_SQLSTATE__"
    invalid_entity_sqlstate = _extract_sqlstate(
        _run_psql(
            label="invalid_entity_option",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql=f"""
LOAD '$libdir/bioseq';
\\set ON_ERROR_STOP off
CREATE TABLE {INVALID_ENTITY_TABLE}
  (provider pg_catalog.text NOT NULL)
USING bioseq_external
WITH (entity = 'unsupported');
\\echo {invalid_marker}=:SQLSTATE
\\set ON_ERROR_STOP on
""",
        ),
        invalid_marker,
    )
    if invalid_entity_sqlstate != INVALID_ENTITY_SQLSTATE:
        raise ExternalAcceptanceError(
            "invalid external entity option returned SQLSTATE "
            f"{invalid_entity_sqlstate}, expected {INVALID_ENTITY_SQLSTATE}"
        )
    invalid_cleanup = _one_csv_record(
        _run_psql(
            label="invalid_entity_cleanup_catalog",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql=f"""
COPY (
  SELECT
    pg_catalog.to_regclass('{INVALID_ENTITY_TABLE}') IS NULL
      AS relation_absent,
    (SELECT count(*) FROM bioseq_internal.external_relation_options)
      AS catalog_row_count
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
        ),
        "invalid external entity cleanup",
    )
    invalid_relation_absent = _boolean(
        invalid_cleanup["relation_absent"],
        "invalid external entity relation absence",
    )
    invalid_catalog_row_count = _integer(
        invalid_cleanup["catalog_row_count"],
        "invalid external entity catalog row count",
    )
    if (
        not invalid_relation_absent
        or invalid_catalog_row_count != len(EXPECTED_RELATION_ENTITIES)
    ):
        raise ExternalAcceptanceError(
            "invalid external entity CREATE mutated relation/catalog state"
        )

    _run_psql(
        label="create_missing_entity",
        cluster=cluster,
        bindir=bindir,
        retained_root=retained_root,
        timeout=timeout,
        sql=f"""
LOAD '$libdir/bioseq';
CREATE TABLE {MISSING_ENTITY_TABLE}
  (provider pg_catalog.text NOT NULL)
USING bioseq_external;
""",
    )
    missing_catalog_row = _one_csv_record(
        _run_psql(
            label="missing_entity_catalog",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql=f"""
COPY (
  SELECT
    c.oid::pg_catalog.text AS relation_oid,
    o.entity IS NULL AS entity_is_null,
    o.alphabet IS NULL AS alphabet_is_null,
    o.storage_width,
    o.chunk_bases
  FROM pg_catalog.pg_class AS c
  JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
  JOIN bioseq_internal.external_relation_options AS o
    ON o.regclass = c.oid
  WHERE n.nspname = '{INSTALL_SCHEMA}'
    AND c.relname = 'external_missing_entity_probe'
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
        ),
        "missing external entity typed catalog row",
    )
    missing_oid = _integer(
        missing_catalog_row["relation_oid"],
        "missing external entity relation OID",
    )
    missing_stored_entity = (
        None
        if _boolean(
            missing_catalog_row["entity_is_null"],
            "missing external entity nullability",
        )
        else "<present>"
    )
    missing_stored_alphabet = (
        None
        if _boolean(
            missing_catalog_row["alphabet_is_null"],
            "missing external alphabet nullability",
        )
        else "<present>"
    )
    missing_stored_storage_width = _integer(
        missing_catalog_row["storage_width"],
        "missing external entity storage width",
    )
    missing_stored_chunk_bases = _integer(
        missing_catalog_row["chunk_bases"],
        "missing external entity chunk bases",
    )

    missing_state_marker = "__BIOSEQ_MISSING_ENTITY_SQLSTATE__"
    missing_message_marker = "__BIOSEQ_MISSING_ENTITY_MESSAGE__"
    missing_entity_output = _run_psql(
        label="missing_entity_option",
        cluster=cluster,
        bindir=bindir,
        retained_root=retained_root,
        timeout=timeout,
        sql=f"""
\\set ON_ERROR_STOP off
SELECT count(*) FROM {MISSING_ENTITY_TABLE};
\\echo {missing_state_marker}=:SQLSTATE
\\echo {missing_message_marker}=:LAST_ERROR_MESSAGE
\\set ON_ERROR_STOP on
DROP TABLE {MISSING_ENTITY_TABLE};
""",
    )
    missing_entity_sqlstate = _extract_sqlstate(
        missing_entity_output, missing_state_marker
    )
    missing_entity_message = _extract_error_message(
        missing_entity_output, missing_message_marker
    )
    if (
        missing_entity_sqlstate != MISSING_ENTITY_SQLSTATE
        or missing_entity_message != MISSING_ENTITY_MESSAGE
    ):
        raise ExternalAcceptanceError(
            "missing external entity option did not fail closed: "
            f"SQLSTATE={missing_entity_sqlstate!r}, "
            f"message={missing_entity_message!r}"
        )
    missing_cleanup = _one_csv_record(
        _run_psql(
            label="missing_entity_cleanup_catalog",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql=f"""
COPY (
  SELECT
    pg_catalog.to_regclass('{MISSING_ENTITY_TABLE}') IS NULL
      AS relation_dropped,
    (SELECT count(*)
       FROM bioseq_internal.external_relation_options AS o
      WHERE o.regclass = {missing_oid}::pg_catalog.oid)
      AS stale_catalog_row_count
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
""",
        ),
        "missing external entity cleanup",
    )
    missing_relation_dropped = _boolean(
        missing_cleanup["relation_dropped"],
        "missing external entity relation drop",
    )
    missing_stale_catalog_row_count = _integer(
        missing_cleanup["stale_catalog_row_count"],
        "missing external entity stale catalog row count",
    )
    if (
        missing_stored_entity is not None
        or missing_stored_alphabet is not None
        or missing_stored_storage_width != 0
        or missing_stored_chunk_bases != 0
        or not missing_relation_dropped
        or missing_stale_catalog_row_count != 0
    ):
        raise ExternalAcceptanceError(
            "missing external entity typed defaults or DROP cleanup differ"
        )

    lifecycle.update({
        "invalid_entity_sqlstate": invalid_entity_sqlstate,
        "invalid_relation_absent": invalid_relation_absent,
        "invalid_catalog_row_count": invalid_catalog_row_count,
        "missing_entity_sqlstate": missing_entity_sqlstate,
        "missing_entity_message": missing_entity_message,
        "missing_stored_entity": missing_stored_entity,
        "missing_stored_alphabet": missing_stored_alphabet,
        "missing_stored_storage_width": missing_stored_storage_width,
        "missing_stored_chunk_bases": missing_stored_chunk_bases,
        "missing_relation_dropped": missing_relation_dropped,
        "missing_stale_catalog_row_count": missing_stale_catalog_row_count,
    })

    _run_psql(
        label="drop_extension",
        cluster=cluster,
        bindir=bindir,
        retained_root=retained_root,
        timeout=timeout,
        sql="DROP EXTENSION bioseq;\n",
    )
    cleanup_row = _one_csv_record(
        _run_psql(
            label="drop_extension_catalog",
            cluster=cluster,
            bindir=bindir,
            retained_root=retained_root,
            timeout=timeout,
            sql=f"""
COPY (
  SELECT
    (SELECT count(*) FROM pg_catalog.pg_extension
      WHERE extname = 'bioseq') AS extension_count,
    (SELECT count(*) FROM pg_catalog.pg_namespace
      WHERE nspname IN ('bioseq', 'bioseq_external'))
      AS fixed_schema_count,
    (SELECT count(*) FROM pg_catalog.pg_am
      WHERE amname = 'bioseq_external') AS access_method_count,
    (SELECT count(*)
       FROM pg_catalog.pg_proc AS p
       JOIN pg_catalog.pg_namespace AS n ON n.oid = p.pronamespace
      WHERE n.nspname = 'bioseq'
        AND p.proname = 'bioseq_external_handler') AS handler_count,
    (SELECT count(*)
       FROM pg_catalog.pg_class AS c
       JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace
      WHERE n.nspname = 'bioseq_external'
        AND c.relkind = 'r') AS external_table_count,
    (SELECT count(*) FROM pg_catalog.pg_namespace
      WHERE nspname = '{INSTALL_SCHEMA}') AS install_schema_count
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
DROP ROLE {DENIED_ROLE};
""",
        ),
        "DROP EXTENSION catalog",
    )
    cleanup = {
        field: _integer(cleanup_row[field], f"cleanup {field}")
        for field in (
            "extension_count",
            "fixed_schema_count",
            "access_method_count",
            "handler_count",
            "external_table_count",
            "install_schema_count",
        )
    }
    expected_cleanup = {
        "extension_count": 0,
        "fixed_schema_count": 0,
        "access_method_count": 0,
        "handler_count": 0,
        "external_table_count": 0,
        "install_schema_count": 1,
    }
    if cleanup != expected_cleanup:
        raise ExternalAcceptanceError(
            f"DROP EXTENSION left external catalog residue: {cleanup}"
        )

    checks = {
        "clean_cluster": preflight,
        "extension": extension,
        "handler": handler,
        "access_method": access_method,
        "tables": tables,
        "relation_options": relation_options,
        "acl": acl,
        "read_only": {
            "operation": "insert",
            "expected_sqlstate": "0A000",
            "table_count": len(EXPECTED_TABLES),
            "table_sqlstates": read_only_sqlstates,
        },
        "empty_relation": empty,
        "create_drop_using_am": lifecycle,
        "drop_extension": cleanup,
    }
    live = {
        "server_version": preflight["server_version"],
        "server_version_num": preflight["server_version_num"],
        "system_identifier": preflight["system_identifier"],
        "data_directory": preflight["data_directory"],
        "socket_directory": preflight["socket_directory"],
        "unix_socket_connection": preflight["unix_socket_connection"],
        "database": cluster.database,
        "database_user": cluster.database_user,
    }
    return checks, live


def _validate_receipt_seal(receipt_path: Path, seal_path: Path) -> None:
    expected = f"{_sha256(receipt_path)}  receipt.json\n"
    actual = seal_path.read_text(encoding="ascii", errors="strict")
    if actual != expected:
        raise ExternalAcceptanceError(
            "acceptance receipt seal differs from receipt.json"
        )


def _run_live(args: argparse.Namespace) -> Path:
    timeout = int(args.timeout_seconds)
    if timeout < 1 or timeout > 600:
        raise ExternalAcceptanceError(
            "timeout-seconds must be in the range 1..600"
        )
    repository_root = _real_directory(
        args.repository_root, "repository root", create=False
    )
    output_root = _real_directory(
        args.output_root, "external acceptance output root", create=True
    )
    raw_paths = {
        "python": (Path(sys.executable).resolve(strict=True), True),
        "cmake": (args.cmake, True),
        "git": (args.git, True),
        "pg_config": (args.pg_config, True),
        "stage_script": (args.stage_script, False),
        "config_template": (args.config_template, False),
        "control_file": (args.control_file, False),
        "install_sql": (args.install_sql, False),
        "manifest": (args.manifest, False),
        "dso": (args.dso, False),
        "static_contract_script": (args.static_contract_script, False),
        "cmake_fragment": (args.cmake_fragment, False),
        "mode_source": (args.mode_source, False),
        "runner": (__file__, False),
        "private_postgres_helper": (
            Path(__file__).resolve().parent / "private_postgres.py",
            False,
        ),
    }
    paths = {
        name: _regular_file(value, name.replace("_", " "), executable=executable)
        for name, (value, executable) in raw_paths.items()
    }
    manifest = _load_json(paths["manifest"], "generated SQL manifest")
    package = _validate_generated_manifest(
        manifest,
        install_sql=paths["install_sql"],
        repository_root=repository_root,
    )
    pg_provenance = _postgresql_provenance(
        paths["pg_config"], timeout=timeout
    )
    source_paths = sorted({
        *EXTERNAL_AUTHORITY_SOURCE_PATHS,
        *(
            str(record["repository_path"])
            for record in package["external_source_provenance"].values()
        ),
        "cmake/run_external_postgres_acceptance.py",
        "cmake/bioseq_external_postgres_acceptance.cmake",
        "build_mcp/server.py",
        "codegen/sql/tests/contract/test_external_feature.py",
    })
    git_provenance = _git_provenance(
        paths["git"],
        repository_root,
        source_paths,
        timeout=timeout,
    )
    input_files = {
            name: _file_record(path, repository_root=repository_root)
            for name, path in sorted(paths.items())
    }
    for source_path, record in sorted(
        package["external_source_provenance"].items()
    ):
        input_files[f"external_source:{source_path}"] = dict(record)
    input_core = {
        "files": input_files,
        "git": git_provenance,
        "postgresql": pg_provenance,
    }
    input_provenance = {
        **input_core,
        "seal_sha256": _sha256_bytes(_canonical_json(input_core)),
    }
    run_nonce = uuid.uuid4().hex
    receipt_id = _sha256_bytes(_canonical_json({
        "input_seal_sha256": input_provenance["seal_sha256"],
        "run_nonce": run_nonce,
    }))
    partial = output_root / f".partial-{receipt_id}-{os.getpid()}"
    final = output_root / "runs" / receipt_id
    if partial.exists() or partial.is_symlink() or final.exists() or final.is_symlink():
        raise ExternalAcceptanceError(
            "fresh external acceptance destination collided"
        )
    partial.mkdir(parents=True)
    try:
        static_stamp = partial / "static" / "external-contract.stamp"
        static_stamp.parent.mkdir(parents=True)
        _run_checked(
            [
                str(paths["python"]),
                str(paths["static_contract_script"]),
                "--install-sql",
                str(paths["install_sql"]),
                "--manifest",
                str(paths["manifest"]),
                "--expect",
                "enabled",
                "--stamp",
                str(static_stamp),
            ],
            timeout=timeout,
            stdout_path=partial / "results" / "static-contract.stdout",
            stderr_path=partial / "results" / "static-contract.stderr",
        )
        _regular_file(static_stamp, "static external contract stamp")

        stage_workspace_root = _real_directory(
            output_root / ".stage-work",
            "external acceptance stage workspace",
            create=True,
        )
        with tempfile.TemporaryDirectory(
            prefix=f"{receipt_id[:12]}-",
            dir=stage_workspace_root,
        ) as raw_workspace:
            stage_parent = Path(raw_workspace) / "stages"
            stage_parent.mkdir()
            stage_root, stage_manifest, stage_summary = _stage_package(
                cmake=paths["cmake"],
                pg_config=paths["pg_config"],
                stage_script=paths["stage_script"],
                control_file=paths["control_file"],
                install_sql=paths["install_sql"],
                dso=paths["dso"],
                stage_parent=stage_parent,
                retained_root=partial,
                timeout=timeout,
            )
            bindir = _stage_member(stage_root, stage_manifest, "bindir")
            cluster = UniquePrivateCluster(ClusterInputs(
                bindir=bindir,
                database="external_acceptance",
                config_template=paths["config_template"],
                output_root=partial / "cluster",
                run_label="external_acceptance",
                library_dirs=(
                    _stage_member(stage_root, stage_manifest, "pkglibdir"),
                ),
                init_command_timeout=timeout,
                bootstrap_timeout=timeout,
                startup_timeout=timeout,
                shutdown_timeout=timeout,
            ))
            expected_connection_environment = {
                "PGHOST": str(cluster.socket_dir),
                "PGPORT": str(cluster.port),
                "PGDATABASE": cluster.database,
                "PGUSER": cluster.database_user,
                "BIOSEQ_TEST_PGHOST": str(cluster.socket_dir),
                "BIOSEQ_TEST_PGPORT": str(cluster.port),
                "BIOSEQ_TEST_PGDATABASE": cluster.database,
                "BIOSEQ_TEST_PGUSER": cluster.database_user,
                "BIOSEQ_TEST_PG_HOST": str(cluster.socket_dir),
                "BIOSEQ_TEST_PG_PORT": str(cluster.port),
                "BIOSEQ_TEST_PG_DATABASE": cluster.database,
                "BIOSEQ_TEST_PG_USER": cluster.database_user,
                "BIOSEQ_TEST_PG_RUN_DIR": str(cluster.run_dir),
                "BIOSEQ_TEST_PG_DATA_DIR": str(cluster.data_dir),
            }
            actual_connection_environment = {
                key: value
                for key, value in cluster.child_environment().items()
                if key.startswith("PG") or key.startswith("BIOSEQ_TEST_PG")
            }
            if actual_connection_environment != expected_connection_environment:
                raise ExternalAcceptanceError(
                    "private cluster child environment retained an ambient "
                    "PostgreSQL connection override"
                )
            cluster.start()
            live_error: BaseException | None = None
            try:
                checks, live = _live_checks(
                    cluster=cluster,
                    bindir=bindir,
                    retained_root=partial,
                    timeout=timeout,
                )
            except BaseException as error:
                live_error = error
                raise
            finally:
                try:
                    cluster.stop()
                except BaseException as stop_error:
                    if live_error is not None:
                        raise ExternalAcceptanceError(
                            "live acceptance failed and private-cluster "
                            f"shutdown also failed: live={live_error}; "
                            f"shutdown={stop_error}"
                        ) from live_error
                    raise
            data_directory_removed = not cluster.data_dir.exists()
            socket_directory_removed = not cluster.socket_dir.exists()
            if not data_directory_removed or not socket_directory_removed:
                raise ExternalAcceptanceError(
                    "private PostgreSQL shutdown left an owned data or socket "
                    "directory behind"
                )
            cluster_summary = {
                "fresh_data_directory": True,
                "private_socket": live["unix_socket_connection"],
                "ambient_pg_environment_stripped": True,
                "install_schema": INSTALL_SCHEMA,
                "data_directory": live["data_directory"],
                "socket_directory": live["socket_directory"],
                "data_directory_removed": data_directory_removed,
                "socket_directory_removed": socket_directory_removed,
            }

        stage_summary["package_input_sha256"] = {
            "control": _sha256(paths["control_file"]),
            "install_sql": _sha256(paths["install_sql"]),
            "library": _sha256(paths["dso"]),
        }
        evidence_files = _evidence_file_inventory(partial)
        receipt = {
            "schema": RECEIPT_SCHEMA,
            "schema_revision": RECEIPT_SCHEMA_REVISION,
            "outcome": "passed",
            "receipt_id": receipt_id,
            "generated_at_utc": dt.datetime.now(
                dt.timezone.utc
            ).isoformat(),
            "run_nonce": run_nonce,
            "input_provenance": input_provenance,
            "generated_package": package,
            "postgresql": {
                **pg_provenance,
                "live": live,
            },
            "stage": stage_summary,
            "cluster": cluster_summary,
            "checks": checks,
            "evidence_files": evidence_files,
        }
        _validate_receipt(receipt, bundle_root=partial)
        receipt_path = partial / "receipt.json"
        seal_path = partial / "receipt.sha256"
        receipt_bytes = _canonical_json(receipt)
        _write_bytes(receipt_path, receipt_bytes)
        _write_bytes(
            seal_path,
            f"{_sha256(receipt_path)}  receipt.json\n".encode("ascii"),
        )
        _validate_receipt_seal(receipt_path, seal_path)
        final.parent.mkdir(parents=True, exist_ok=True)
        partial.rename(final)
        _atomic_write(output_root / "latest-receipt.json", receipt_bytes)
        _atomic_write(
            output_root / "latest-receipt.sha256",
            (
                f"{_sha256(final / 'receipt.json')}  "
                "latest-receipt.json\n"
            ).encode("ascii"),
        )
        return final / "receipt.json"
    except BaseException as error:
        if partial.exists() and partial.is_dir() and not partial.is_symlink():
            with (partial / "failure.txt").open(
                "w", encoding="utf-8"
            ) as stream:
                stream.write(f"{type(error).__name__}: {error}\n")
            failed = output_root / "failures" / (
                f"{receipt_id}-{uuid.uuid4().hex[:8]}"
            )
            failed.parent.mkdir(parents=True, exist_ok=True)
            partial.rename(failed)
        raise


def _synthetic_receipt(root: Path) -> dict[str, Any]:
    root = _real_directory(root, "synthetic receipt root", create=True)
    _write_bytes(root / "queries" / "probe.sql", b"SELECT 1;\n")
    _write_bytes(
        root / "static" / "external-contract.stamp",
        b"synthetic external contract\n",
    )
    for table in sorted(EXPECTED_TABLES):
        _write_bytes(
            root / "queries" / f"read_only_insert_{table}.sql",
            READ_ONLY_INSERT_SQL[table].encode("utf-8"),
        )
        _write_bytes(
            root / "results" / f"read_only_insert_{table}.stdout",
            (
                f"__BIOSEQ_READ_ONLY_{table.upper()}_SQLSTATE__=0A000\n"
            ).encode("ascii"),
        )
        _write_bytes(
            root / "results" / f"read_only_insert_{table}.stderr",
            b"",
        )
    _write_bytes(
        root / "queries" / "invalid_entity_option.sql",
        b"-- synthetic invalid entity probe\n",
    )
    _write_bytes(
        root / "results" / "invalid_entity_option.stdout",
        (
            "__BIOSEQ_INVALID_ENTITY_SQLSTATE__="
            f"{INVALID_ENTITY_SQLSTATE}\n"
        ).encode("ascii"),
    )
    _write_bytes(
        root / "results" / "invalid_entity_option.stderr",
        b"",
    )
    _write_bytes(
        root / "queries" / "missing_entity_option.sql",
        b"-- synthetic missing entity probe\n",
    )
    _write_bytes(
        root / "results" / "missing_entity_option.stdout",
        (
            "__BIOSEQ_MISSING_ENTITY_SQLSTATE__="
            f"{MISSING_ENTITY_SQLSTATE}\n"
            "__BIOSEQ_MISSING_ENTITY_MESSAGE__="
            f"{MISSING_ENTITY_MESSAGE}\n"
        ).encode("utf-8"),
    )
    _write_bytes(
        root / "results" / "missing_entity_option.stderr",
        b"",
    )
    for label in (
        "relation_options_catalog",
        "create_using_am",
        "created_using_am_catalog",
        "drop_using_am",
        "dropped_using_am_catalog",
        "invalid_entity_cleanup_catalog",
        "create_missing_entity",
        "missing_entity_catalog",
        "missing_entity_cleanup_catalog",
    ):
        _write_bytes(
            root / "queries" / f"{label}.sql",
            f"-- synthetic {label} probe\n".encode("ascii"),
        )
        _write_bytes(
            root / "results" / f"{label}.stdout",
            f"synthetic {label} receipt\n".encode("ascii"),
        )
        _write_bytes(root / "results" / f"{label}.stderr", b"")

    inputs_root = root / "inputs"
    repository_root = inputs_root / "repository"
    source_paths: dict[str, Path] = {}
    for index, source_path in enumerate(
        sorted(EXTERNAL_AUTHORITY_SOURCE_PATHS), 1
    ):
        source = repository_root / source_path
        _write_bytes(
            source,
            f"synthetic external authority {index}\n".encode("utf-8"),
        )
        source_paths[source_path] = source

    install_sql = inputs_root / "bioseq--1.0.sql"
    control_file = inputs_root / "bioseq.control"
    dso = inputs_root / "bioseq.so"
    _write_bytes(install_sql, b"-- synthetic generated install SQL\n")
    _write_bytes(control_file, b"default_version = '1.0'\nrelocatable = true\n")
    _write_bytes(dso, b"synthetic DSO\n")

    pg_root = inputs_root / "postgresql"
    pg_bindir = pg_root / "bin"
    pg_sharedir = pg_root / "share"
    pg_pkglibdir = pg_root / "lib"
    pg_bindir.mkdir(parents=True)
    pg_sharedir.mkdir()
    pg_pkglibdir.mkdir()
    pg_config_path = pg_bindir / "pg_config"
    _write_bytes(pg_config_path, b"synthetic pg_config\n")
    tools: dict[str, dict[str, object]] = {}
    for name in sorted(POSTGRESQL_TOOLS):
        tool = pg_bindir / name
        _write_bytes(tool, f"synthetic {name}\n".encode("ascii"))
        tools[name] = _file_record(tool)
    pg_config = {
        "version": "PostgreSQL 17.5",
        "configure": "--with-synthetic-acceptance",
        "bindir": str(pg_bindir),
        "sharedir": str(pg_sharedir),
        "pkglibdir": str(pg_pkglibdir),
    }
    pg_provenance = {"pg_config": pg_config, "tools": tools}

    top_level_entities: list[dict[str, object]] = []
    sorted_top_level = sorted(EXPECTED_EXTERNAL_TOP_LEVEL_IDENTITIES)
    for identity in sorted_top_level:
        source_path = (
            "bioseq/backend/pg/external/relation.cppm"
            if (
                "handler" in identity
                or "access_method" in identity
            )
            else "bioseq/backend/pg/external/sql_catalog.cppm"
        )
        dependencies: list[str] = []
        kind = "surface"
        if identity == EXTERNAL_CATALOG_DEPENDENCIES_IDENTITY:
            kind = "catalog_dependencies"
            dependencies = [
                "sql:function:bioseq.bioseq_external_handler(internal)",
                EXTERNAL_OPTIONS_TABLE_IDENTITY,
            ]
        elif identity.startswith("sql:table:bioseq_external."):
            kind = "table"
            table_name = identity.rsplit(".", 1)[-1]
            dependencies = [
                "sql:schema:bioseq_external",
                EXTERNAL_ACCESS_METHOD_IDENTITY,
                EXTERNAL_OPTIONS_TABLE_IDENTITY,
            ]
            if table_name in {"sequences", "sequence_chunks"}:
                dependencies.append("sql:type:bioseq.seq")
        top_level_entities.append({
            "stable_identity": identity,
            "kind": kind,
            "source_path": source_path,
            "source_sha256": _sha256(source_paths[source_path]),
            "active_condition": "defined(BIOSEQ_EXTERNAL_PG)",
            "dependencies": sorted(dependencies),
        })
    catalog_source_path = (
        "bioseq/backend/pg/external/sql_catalog.cppm"
    )
    column_entities = [
        {
            "stable_identity": f"sql:column:synthetic_external_{index:03d}",
            "kind": "column",
            "source_path": catalog_source_path,
            "source_sha256": _sha256(source_paths[catalog_source_path]),
            "active_condition": "defined(BIOSEQ_EXTERNAL_PG)",
        }
        for index in range(1, EXPECTED_COLUMN_COUNT + 1)
    ]
    metadata_hash = _sha256(
        source_paths[EXTERNAL_OPTIONS_SOURCE_PATH]
    )
    metadata_column_names = (
        "regclass",
        "entity",
        "alphabet",
        "storage_width",
        "chunk_bases",
    )
    metadata_column_ids = [
        f"sql:column:bioseq_internal.synthetic_external_options.{name}"
        for name in metadata_column_names
    ]
    metadata_constraint_ids = [
        "sql:primary_key:bioseq_internal.synthetic_external_options.pk",
        "sql:check:bioseq_internal.synthetic_external_options.entity",
        "sql:check:bioseq_internal.synthetic_external_options.storage_width",
        "sql:check:bioseq_internal.synthetic_external_options.chunk_bases",
    ]
    metadata_config_dump_id = (
        "sql:config_dump:bioseq_internal.synthetic_external_options.dump"
    )
    metadata_table = {
        "stable_identity": EXTERNAL_OPTIONS_TABLE_IDENTITY,
        "installed_identity": EXTERNAL_OPTIONS_TABLE_IDENTITY,
        "kind": "table",
        "schema": "bioseq_internal",
        "schema_mode": "fixed",
        "name": "external_relation_options",
        "cpp_declaration": EXTERNAL_OPTIONS_CPP_NAME,
        "source_module": EXTERNAL_OPTIONS_SOURCE_MODULE,
        "source_path": EXTERNAL_OPTIONS_SOURCE_PATH,
        "source_sha256": metadata_hash,
        "columns": metadata_column_ids,
        "constraints": metadata_constraint_ids[1:],
        "generated_cpp": {
            "facet": "row_codec",
            "owner_key": "storage_metadata",
            "source_module": EXTERNAL_OPTIONS_SOURCE_MODULE,
            "module_name": EXTERNAL_OPTIONS_ROW_CODEC_MODULE,
            "row_cpp_name": EXTERNAL_OPTIONS_CPP_NAME,
            "profile_invariant": True,
        },
    }
    metadata_companions: list[dict[str, object]] = []
    for name, identity in zip(
        metadata_column_names, metadata_column_ids, strict=True
    ):
        metadata_companions.append({
            "stable_identity": identity,
            "kind": "column",
            "cpp_declaration": f"{EXTERNAL_OPTIONS_CPP_NAME}::{name}",
            "source_module": EXTERNAL_OPTIONS_SOURCE_MODULE,
            "source_path": EXTERNAL_OPTIONS_SOURCE_PATH,
            "source_sha256": metadata_hash,
            "dependencies": [
                "sql:schema:bioseq_internal",
                EXTERNAL_OPTIONS_TABLE_IDENTITY,
            ],
        })
    for identity, kind in (
        (metadata_constraint_ids[0], "primary_key"),
        *((identity, "check") for identity in metadata_constraint_ids[1:]),
        (metadata_config_dump_id, "config_dump"),
    ):
        metadata_companions.append({
            "stable_identity": identity,
            "kind": kind,
            "cpp_declaration": EXTERNAL_OPTIONS_CPP_NAME,
            "source_module": EXTERNAL_OPTIONS_SOURCE_MODULE,
            "source_path": EXTERNAL_OPTIONS_SOURCE_PATH,
            "source_sha256": metadata_hash,
            "dependencies": [EXTERNAL_OPTIONS_TABLE_IDENTITY],
        })
    statements: list[dict[str, str]] = []
    statements.append({
        "statement_id": "S000",
        "source_path": EXTERNAL_OPTIONS_SOURCE_PATH,
        "owner_identity": EXTERNAL_OPTIONS_TABLE_IDENTITY,
    })
    for index, owner_identity in enumerate(sorted_top_level, 1):
        source_path = (
            "bioseq/backend/pg/external/relation.cppm"
            if (
                "handler" in owner_identity
                or "access_method" in owner_identity
            )
            else catalog_source_path
        )
        statements.append({
            "statement_id": f"S{index:03d}",
            "source_path": source_path,
            "owner_identity": owner_identity,
        })
    statements.append({
        "statement_id": f"S{len(statements):03d}",
        "source_path": EXTERNAL_OPTIONS_SOURCE_PATH,
        "owner_identity": metadata_config_dump_id,
    })
    entities = [
        *top_level_entities,
        *column_entities,
        metadata_table,
        *metadata_companions,
    ]
    row_codec_fields = [
        {
            "ordinal": ordinal,
            "cpp_name": sql_name,
            "sql_name": sql_name,
            "cpp_type": cpp_type,
            "sql_type": sql_type,
            "nullable": nullable,
            "converter_key": converter_key,
        }
        for (
            ordinal,
            sql_name,
            sql_type,
            cpp_type,
            nullable,
            converter_key,
        ) in EXTERNAL_OPTIONS_ROW_CODEC_FIELDS
    ]
    manifest = {
        "schema_revision": 1,
        "authority_mode": "annotated_cpp",
        "script_sha256": _sha256(install_sql),
        "statement_count": len(statements),
        "entity_count": len(entities),
        "entities": entities,
        "statements": statements,
        "row_codec_modules": [{
            "owner_key": "storage_metadata",
            "source_module": EXTERNAL_OPTIONS_SOURCE_MODULE,
            "module_name": EXTERNAL_OPTIONS_ROW_CODEC_MODULE,
            "profile_invariant": True,
            "row_cpp_names": [EXTERNAL_OPTIONS_CPP_NAME],
            "row_entity_identities": [EXTERNAL_OPTIONS_TABLE_IDENTITY],
        }],
        "row_codec_contracts": [{
            "row_identity": EXTERNAL_OPTIONS_TABLE_IDENTITY,
            "row_cpp_name": EXTERNAL_OPTIONS_CPP_NAME,
            "module_name": EXTERNAL_OPTIONS_ROW_CODEC_MODULE,
            "fields": row_codec_fields,
        }],
        "handwritten_fmgr_export_external_symbol_count": 1,
        "handwritten_fmgr_export_external_symbols": ["bioseq_external_handler"],
    }
    legacy_revision_manifest = dict(manifest)
    legacy_revision_manifest["manifest_version"] = 1
    try:
        _validate_generated_manifest(
            legacy_revision_manifest,
            install_sql=install_sql,
            repository_root=repository_root,
        )
    except ExternalAcceptanceError as error:
        if "unknown fields: ['manifest_version']" not in str(error):
            raise
    else:
        raise AssertionError("legacy manifest_version was accepted")
    manifest_path = inputs_root / "bioseq--1.0.manifest.json"
    _write_json(manifest_path, manifest)

    raw_input_paths = {
        "python": inputs_root / "python",
        "cmake": inputs_root / "cmake",
        "git": inputs_root / "git",
        "pg_config": pg_config_path,
        "stage_script": inputs_root / "stage_private_postgres.cmake",
        "config_template": inputs_root / "postgresql.conf",
        "control_file": control_file,
        "install_sql": install_sql,
        "manifest": manifest_path,
        "dso": dso,
        "static_contract_script": inputs_root / "verify_external.py",
        "cmake_fragment": inputs_root / "external-acceptance.cmake",
        "mode_source": inputs_root / "server.py",
        "runner": inputs_root / "run_external_acceptance.py",
        "private_postgres_helper": inputs_root / "private_postgres.py",
    }
    precreated = {
        "pg_config",
        "control_file",
        "install_sql",
        "manifest",
        "dso",
    }
    for name, path in raw_input_paths.items():
        if name not in precreated:
            _write_bytes(path, f"synthetic {name}\n".encode("utf-8"))
    files = {
        name: _file_record(path)
        for name, path in sorted(raw_input_paths.items())
    }
    for source_path, source in sorted(source_paths.items()):
        files[f"external_source:{source_path}"] = _file_record(
            source, repository_root=repository_root
        )
    if set(files) != EXPECTED_INPUT_FILE_KEYS:
        raise AssertionError("synthetic input-file closure is stale")

    package = _validate_generated_manifest(
        manifest,
        install_sql=install_sql,
        repository_root=repository_root,
    )
    handler = _validate_handler({
        "schema_name": "bioseq",
        "routine_name": "bioseq_external_handler",
        "argument_types": "internal",
        "result_schema": "pg_catalog",
        "result_type": "table_am_handler",
        "language_name": "c",
        "source_symbol": "bioseq_external_handler",
        "binary_path": "$libdir/bioseq",
        "strict": "t",
        "routine_kind": "f",
    })
    access_method = _validate_access_method({
        "access_method": "bioseq_external",
        "access_method_type": "t",
        "handler_schema": "bioseq",
        "handler_name": "bioseq_external_handler",
        "handler_arguments": "internal",
    })
    input_core = {
        "files": files,
        "git": {"revision": "f" * 40, "source_status": []},
        "postgresql": pg_provenance,
    }
    input_provenance = {
        **input_core,
        "seal_sha256": _sha256_bytes(_canonical_json(input_core)),
    }
    run_nonce = "1234567890ab4def8abc1234567890ab"
    receipt_id = _sha256_bytes(_canonical_json({
        "input_seal_sha256": input_provenance["seal_sha256"],
        "run_nonce": run_nonce,
    }))

    package_hashes = {
        "control": _sha256(control_file),
        "install_sql": _sha256(install_sql),
        "library": _sha256(dso),
    }
    aggregate = _stage_package_aggregate(
        control_file=control_file,
        install_sql=install_sql,
        dso=dso,
        control_sha256=package_hashes["control"],
        install_sql_sha256=package_hashes["install_sql"],
        library_sha256=package_hashes["library"],
    )
    source_roots = {
        "bindir": pg_config["bindir"],
        "sharedir": pg_config["sharedir"],
        "pkglibdir": pg_config["pkglibdir"],
    }
    staged = {
        "bindir": str(pg_bindir).lstrip("/"),
        "sharedir": str(pg_sharedir).lstrip("/"),
        "pkglibdir": str(pg_pkglibdir).lstrip("/"),
    }
    expected_tool_paths = {
        name: f"{staged['bindir']}/{name}"
        for name in sorted(POSTGRESQL_TOOLS)
    }
    runtime_closure = {
        "algorithm": RUNTIME_CLOSURE_ALGORITHM,
        "schema_revision": RUNTIME_CLOSURE_SCHEMA_REVISION,
        "entry_count": 0,
        "aggregate_sha256": _sha256_bytes(b""),
        "roots": {
            name: {
                "reported_source": source_roots[name],
                "canonical_source": source_roots[name],
                "staged": staged[name],
            }
            for name in ("bindir", "sharedir", "pkglibdir")
        },
        "required_sentinels": {},
        "loader_dependencies": {
            "entry_count": 0,
            "aggregate_sha256": _sha256_bytes(b""),
            "entries": [],
        },
        "entries": [],
    }
    stage_manifest = {
        "schema": STAGE_MANIFEST_SCHEMA,
        "schema_revision": STAGE_MANIFEST_SCHEMA_REVISION,
        "postgresql_version": pg_config["version"],
        "postgresql_configure": pg_config["configure"],
        "source": source_roots,
        "staged": staged,
        "runtime_closure": runtime_closure,
        "extension_artifacts": {
            "aggregate_sha256": aggregate,
            "artifacts": [{
                "name": "bioseq",
                "module_pathname_override": None,
                "effective_module_name": dso.name.split(".", 1)[0],
                "control": {
                    "source": str(control_file),
                    "filename": control_file.name,
                    "staged": (
                        f"{staged['sharedir']}/extension/{control_file.name}"
                    ),
                    "source_sha256": package_hashes["control"],
                    "sha256": package_hashes["control"],
                },
                "install_sql": {
                    "source": str(install_sql),
                    "filename": install_sql.name,
                    "staged": (
                        f"{staged['sharedir']}/extension/{install_sql.name}"
                    ),
                    "version": "1.0",
                    "sha256": package_hashes["install_sql"],
                },
                "library": {
                    "source": str(dso),
                    "filename": dso.name,
                    "staged": f"{staged['pkglibdir']}/{dso.name}",
                    "sha256": package_hashes["library"],
                },
            }],
        },
        "expected_tool_paths": expected_tool_paths,
        "tool_signatures": [
            {
                "tool": name,
                "staged_path": expected_tool_paths[name],
                "version": f"{name} (PostgreSQL) 17.5",
                "sha256": tools[name]["sha256"],
            }
            for name in sorted(POSTGRESQL_TOOLS)
        ],
    }
    artifact_spec = _artifact_spec_document(
        control_file=control_file,
        install_sql=install_sql,
        dso=dso,
    )
    artifact_spec_path = root / "stage" / "artifact-spec.json"
    _write_json(artifact_spec_path, artifact_spec)
    stage_manifest_path = root / "stage" / "stage-manifest.json"
    _write_json(stage_manifest_path, stage_manifest)
    aggregate_path = root / "stage" / "stage-artifacts.sha256"
    _write_bytes(aggregate_path, (aggregate + "\n").encode("ascii"))
    stage = {
        "schema": STAGE_MANIFEST_SCHEMA,
        "schema_revision": STAGE_MANIFEST_SCHEMA_REVISION,
        "postgresql_version": pg_config["version"],
        "postgresql_configure": pg_config["configure"],
        "artifact_spec": {
            "schema": ARTIFACT_SPEC_SCHEMA,
            "schema_revision": ARTIFACT_SPEC_SCHEMA_REVISION,
            "sha256": _sha256(artifact_spec_path),
        },
        "runtime_closure": {
            "algorithm": RUNTIME_CLOSURE_ALGORITHM,
            "schema_revision": RUNTIME_CLOSURE_SCHEMA_REVISION,
            "entry_count": runtime_closure["entry_count"],
            "aggregate_sha256": runtime_closure["aggregate_sha256"],
        },
        "package_aggregate_sha256": aggregate,
        "stage_manifest_sha256": _sha256(stage_manifest_path),
        "staged_tool_sha256": {
            name: tools[name]["sha256"]
            for name in sorted(POSTGRESQL_TOOLS)
        },
        "package_input_sha256": package_hashes,
    }
    data_directory = str(root / "retired" / "private-data")
    socket_directory = str(root / "retired" / "private-socket")
    live = {
        "server_version": "17.5",
        "server_version_num": "170005",
        "system_identifier": "123",
        "data_directory": data_directory,
        "socket_directory": socket_directory,
        "unix_socket_connection": True,
        "database": "external_acceptance",
        "database_user": "synthetic_user",
    }
    clean = {
        "server_version": live["server_version"],
        "server_version_num": live["server_version_num"],
        "system_identifier": live["system_identifier"],
        "data_directory": data_directory,
        "socket_directory": socket_directory,
        "unix_socket_connection": True,
        "extension_count": 0,
        "fixed_schema_count": 0,
        "access_method_count": 0,
        "handler_count": 0,
    }
    return {
        "schema": RECEIPT_SCHEMA,
        "schema_revision": RECEIPT_SCHEMA_REVISION,
        "outcome": "passed",
        "receipt_id": receipt_id,
        "generated_at_utc": "2026-07-26T00:00:00+00:00",
        "run_nonce": run_nonce,
        "input_provenance": input_provenance,
        "generated_package": package,
        "postgresql": {
            **pg_provenance,
            "live": live,
        },
        "stage": stage,
        "cluster": {
            "fresh_data_directory": True,
            "private_socket": True,
            "ambient_pg_environment_stripped": True,
            "install_schema": INSTALL_SCHEMA,
            "data_directory": data_directory,
            "socket_directory": socket_directory,
            "data_directory_removed": True,
            "socket_directory_removed": True,
        },
        "checks": {
            "clean_cluster": clean,
            "extension": {
                "extension_name": "bioseq",
                "extension_schema": INSTALL_SCHEMA,
                "relocatable": True,
                "version": "1.0",
                "bioseq_schema_count": 1,
                "external_schema_count": 1,
                "external_table_count": len(EXPECTED_TABLES),
                "external_tables_in_install_schema": 0,
                "handler_in_install_schema": 0,
            },
            "handler": handler,
            "access_method": access_method,
            "tables": {
                "table_count": len(EXPECTED_TABLES),
                "column_count": EXPECTED_COLUMN_COUNT,
                "ordered_catalog_sha256": EXPECTED_CATALOG_SHA256,
                "all_relation_sizes_zero": True,
            },
            "relation_options": {
                "catalog_table": EXTERNAL_OPTIONS_TABLE,
                "row_count": len(EXPECTED_RELATION_ENTITIES),
                "rows": _expected_external_option_rows(),
            },
            "acl": {
                "schema_usage": False,
                "handler_execute": False,
                "selectable_table_count": 0,
                "table_count": len(EXPECTED_TABLES),
                "denied_sqlstate": "42501",
            },
            "read_only": _expected_read_only_receipt(),
            "empty_relation": {
                "relation_size": 0,
                "false_predicate_row_count": 0,
            },
            "create_drop_using_am": {
                "created_with_access_method": "bioseq_external",
                "stored_entity": "taxonomy",
                "stored_alphabet": None,
                "stored_storage_width": 0,
                "stored_chunk_bases": 0,
                "relation_size": 0,
                "dropped": True,
                "stale_catalog_row_count": 0,
                "invalid_entity_sqlstate": INVALID_ENTITY_SQLSTATE,
                "invalid_relation_absent": True,
                "invalid_catalog_row_count": len(
                    EXPECTED_RELATION_ENTITIES
                ),
                "missing_entity_sqlstate": MISSING_ENTITY_SQLSTATE,
                "missing_entity_message": MISSING_ENTITY_MESSAGE,
                "missing_stored_entity": None,
                "missing_stored_alphabet": None,
                "missing_stored_storage_width": 0,
                "missing_stored_chunk_bases": 0,
                "missing_relation_dropped": True,
                "missing_stale_catalog_row_count": 0,
            },
            "drop_extension": {
                "extension_count": 0,
                "fixed_schema_count": 0,
                "access_method_count": 0,
                "handler_count": 0,
                "external_table_count": 0,
                "install_schema_count": 1,
            },
        },
        "evidence_files": _evidence_file_inventory(root),
    }


def _expect_failure(label: str, function) -> None:
    try:
        function()
    except ExternalAcceptanceError:
        return
    raise AssertionError(f"mutation was accepted: {label}")


def _reseal_receipt_identity(receipt: dict[str, Any]) -> None:
    inputs = receipt.get("input_provenance")
    if isinstance(inputs, dict):
        input_core = {
            key: value
            for key, value in inputs.items()
            if key != "seal_sha256"
        }
        inputs["seal_sha256"] = _sha256_bytes(_canonical_json(input_core))
        receipt["receipt_id"] = _sha256_bytes(_canonical_json({
            "input_seal_sha256": inputs["seal_sha256"],
            "run_nonce": receipt.get("run_nonce"),
        }))


def _expect_resealed_receipt_failure(
    label: str,
    receipt: dict[str, Any],
    *,
    evidence_root: Path,
    mutation_root: Path,
    reseal_identity: bool = True,
) -> None:
    if reseal_identity:
        _reseal_receipt_identity(receipt)
    destination = mutation_root / re.sub(r"[^a-z0-9]+", "-", label.lower())
    destination.mkdir(parents=True, exist_ok=False)
    receipt_path = destination / "receipt.json"
    seal_path = destination / "receipt.sha256"
    _write_json(receipt_path, receipt)
    _write_bytes(
        seal_path,
        f"{_sha256(receipt_path)}  receipt.json\n".encode("ascii"),
    )
    _validate_receipt_seal(receipt_path, seal_path)
    _expect_failure(
        label,
        lambda: _validate_receipt(receipt, bundle_root=evidence_root),
    )


def run_self_tests() -> None:
    stage_command_paths = {
        "cmake": Path("cmake"),
        "pg_config": Path("pg_config"),
        "stage_script": Path("stage_private_postgres.cmake"),
        "stage_root": Path("stage"),
        "stage_parent": Path("stages"),
        "artifact_spec": Path("artifact-spec.json"),
        "manifest_path": Path("stage-manifest.json"),
        "aggregate_path": Path("stage-artifacts.sha256"),
    }
    stage_command = _stage_command(**stage_command_paths)
    expected_stage_command = [
        "cmake",
        "-DPG_CONFIG=pg_config",
        "-DSTAGE_ROOT=stage",
        "-DSTAGE_PARENT=stages",
        "-DARTIFACT_SPEC=artifact-spec.json",
        "-DMANIFEST_OUT=stage-manifest.json",
        "-DHASH_OUT=stage-artifacts.sha256",
        "-P",
        "stage_private_postgres.cmake",
    ]
    if stage_command != expected_stage_command:
        raise AssertionError("external acceptance stage command is stale")
    forbidden_stage_arguments = (
        "-DCONTROL_FILE=",
        "-DINSTALL_SQL=",
        "-DLIBRARY_FILE=",
        "-DEXTENSION_VERSION=",
    )
    if any(
        argument.startswith(forbidden_stage_arguments)
        for argument in stage_command
    ):
        raise AssertionError("legacy stage argument survived")

    # Raw live-catalog validators reject independent handler/AM/table mutation.
    _expect_failure(
        "handler schema",
        lambda: _validate_handler({
            "schema_name": "public",
            "routine_name": "bioseq_external_handler",
            "argument_types": "internal",
            "result_schema": "pg_catalog",
            "result_type": "table_am_handler",
            "language_name": "c",
            "source_symbol": "bioseq_external_handler",
            "binary_path": "$libdir/bioseq",
            "strict": "t",
            "routine_kind": "f",
        }),
    )
    _expect_failure(
        "pg_am handler",
        lambda: _validate_access_method({
            "access_method": "bioseq_external",
            "access_method_type": "t",
            "handler_schema": "public",
            "handler_name": "bioseq_external_handler",
            "handler_arguments": "internal",
        }),
    )
    live_rows = [
        {
            key: (
                "t" if value is True
                else "f" if value is False
                else str(value)
            )
            for key, value in row.items()
        }
        for row in _expected_catalog_rows()
    ]
    _validate_table_catalog(live_rows)
    table_mutation = copy.deepcopy(live_rows)
    table_mutation[0]["column_name"] = "provider_mutated"
    _expect_failure(
        "ordered table column",
        lambda: _validate_table_catalog(table_mutation),
    )

    with tempfile.TemporaryDirectory() as temporary:
        temporary_root = Path(temporary).resolve(strict=True)
        evidence_root = temporary_root / "evidence"
        mutation_root = temporary_root / "mutations"
        receipt = _synthetic_receipt(evidence_root)
        _validate_receipt(receipt, bundle_root=evidence_root)

        input_files = receipt["input_provenance"]["files"]
        control_path = Path(input_files["control_file"]["path"])
        install_path = Path(input_files["install_sql"]["path"])
        library_path = Path(input_files["dso"]["path"])
        artifact_spec_path = evidence_root / "stage" / "artifact-spec.json"
        artifact_spec = _load_json(
            artifact_spec_path, "synthetic artifact specification"
        )
        invalid_artifact_specs: dict[str, dict[str, Any]] = {}
        invalid_artifact_specs["artifact specification current schema"] = (
            copy.deepcopy(artifact_spec)
        )
        invalid_artifact_specs[
            "artifact specification current schema"
        ]["schema"] = "invalid.artifacts"
        invalid_artifact_specs["artifact specification numeric revision"] = (
            copy.deepcopy(artifact_spec)
        )
        invalid_artifact_specs[
            "artifact specification numeric revision"
        ]["schema_revision"] = True
        invalid_artifact_specs["artifact specification unknown key"] = (
            copy.deepcopy(artifact_spec)
        )
        invalid_artifact_specs[
            "artifact specification unknown key"
        ]["unexpected"] = None
        for label, invalid_spec in invalid_artifact_specs.items():
            _expect_failure(
                label,
                lambda invalid_spec=invalid_spec: _validate_artifact_spec(
                    invalid_spec,
                    control_file=control_path,
                    install_sql=install_path,
                    dso=library_path,
                ),
            )

        stage_manifest_path = evidence_root / "stage" / "stage-manifest.json"
        stage_manifest = _load_json(
            stage_manifest_path, "synthetic stage manifest"
        )
        invalid_stage_manifests: dict[str, dict[str, Any]] = {}
        invalid_stage_manifests["stage manifest current schema"] = (
            copy.deepcopy(stage_manifest)
        )
        invalid_stage_manifests[
            "stage manifest current schema"
        ]["schema"] = "invalid.stage_manifest"
        invalid_stage_manifests["stage manifest numeric revision"] = (
            copy.deepcopy(stage_manifest)
        )
        invalid_stage_manifests[
            "stage manifest numeric revision"
        ]["schema_revision"] = True
        invalid_stage_manifests["stage manifest runtime algorithm"] = (
            copy.deepcopy(stage_manifest)
        )
        invalid_stage_manifests[
            "stage manifest runtime algorithm"
        ]["runtime_closure"]["algorithm"] = "invalid.runtime"
        invalid_stage_manifests["stage manifest unknown key"] = (
            copy.deepcopy(stage_manifest)
        )
        invalid_stage_manifests[
            "stage manifest unknown key"
        ]["unexpected"] = None
        for label, invalid_manifest in invalid_stage_manifests.items():
            _expect_failure(
                label,
                lambda invalid_manifest=invalid_manifest: (
                    _validate_retained_stage_manifest(
                        invalid_manifest,
                        stage=receipt["stage"],
                        pg_config=receipt["postgresql"]["pg_config"],
                        input_files=input_files,
                    )
                ),
            )

        path_mutations: dict[str, tuple[object, ...]] = {
            "receipt schema": ("schema", "invalid.acceptance_receipt"),
            "receipt schema revision": ("schema_revision", 2),
            "header timestamp schema": (
                "generated_at_utc", "2026-07-26T00:00:00"
            ),
            "header UUIDv4": ("run_nonce", "0" * 32),
            "input file content hash": (
                "input_provenance", "files", "install_sql", "sha256",
                "0" * 64,
            ),
            "input Git schema": (
                "input_provenance", "git", "revision", "0" * 39,
            ),
            "input PostgreSQL cross-join": (
                "input_provenance", "postgresql", "pg_config", "configure",
                "--mutated",
            ),
            "input PostgreSQL tool hash": (
                "input_provenance", "postgresql", "tools", "psql", "sha256",
                "1" * 64,
            ),
            "generated package install join": (
                "generated_package", "install_sql_sha256", "2" * 64,
            ),
            "generated package statement count": (
                "generated_package", "manifest_statement_count", 0,
            ),
            "generated package typed codec": (
                "generated_package", "external_options_row_codec_module",
                "bioseq.postgres.invalid.sql_row_codecs",
            ),
            "generated package source join": (
                "generated_package", "external_source_provenance",
                sorted(EXTERNAL_SOURCE_PATHS)[0], "sha256", "3" * 64,
            ),
            "generated package metadata source join": (
                "generated_package", "external_source_provenance",
                EXTERNAL_OPTIONS_SOURCE_PATH, "sha256", "4" * 64,
            ),
            "PostgreSQL input projection join": (
                "postgresql", "pg_config", "configure", "--mutated",
            ),
            "PostgreSQL tool projection join": (
                "postgresql", "tools", "psql", "sha256", "5" * 64,
            ),
            "PostgreSQL live major": (
                "postgresql", "live", "server_version_num", "160000",
            ),
            "PostgreSQL clean identity join": (
                "postgresql", "live", "system_identifier", "456",
            ),
            "PostgreSQL clean data path join": (
                "postgresql", "live", "data_directory", "/mutated/data",
            ),
            "stage schema": ("stage", "schema", "invalid.stage_manifest"),
            "stage schema revision": ("stage", "schema_revision", 2),
            "artifact specification schema": (
                "stage", "artifact_spec", "schema", "invalid.artifacts",
            ),
            "artifact specification schema revision": (
                "stage", "artifact_spec", "schema_revision", 2,
            ),
            "artifact specification evidence join": (
                "stage", "artifact_spec", "sha256", "4" * 64,
            ),
            "runtime closure algorithm": (
                "stage", "runtime_closure", "algorithm", "invalid.runtime",
            ),
            "runtime closure schema revision": (
                "stage", "runtime_closure", "schema_revision", 2,
            ),
            "runtime closure aggregate": (
                "stage", "runtime_closure", "aggregate_sha256", "5" * 64,
            ),
            "stage PostgreSQL configure join": (
                "stage", "postgresql_configure", "--mutated",
            ),
            "stage package aggregate join": (
                "stage", "package_aggregate_sha256", "6" * 64,
            ),
            "stage package input join": (
                "stage", "package_input_sha256", "install_sql", "7" * 64,
            ),
            "stage PostgreSQL tool join": (
                "stage", "staged_tool_sha256", "psql", "8" * 64,
            ),
            "stage manifest evidence join": (
                "stage", "stage_manifest_sha256", "9" * 64,
            ),
            "cluster freshness": (
                "cluster", "fresh_data_directory", False,
            ),
            "cluster private socket": (
                "cluster", "private_socket", False,
            ),
            "cluster environment": (
                "cluster", "ambient_pg_environment_stripped", False,
            ),
            "cluster install schema": (
                "cluster", "install_schema", "public",
            ),
            "cluster data path join": (
                "cluster", "data_directory", "/mutated/data",
            ),
            "cluster data cleanup": (
                "cluster", "data_directory_removed", False,
            ),
            "clean cluster zero count": (
                "checks", "clean_cluster", "extension_count", 1,
            ),
            "extension exact schema": (
                "checks", "extension", "external_table_count", 5,
            ),
            "handler exact schema": (
                "checks", "handler", "schema_name", "public",
            ),
            "access method exact schema": (
                "checks", "access_method", "handler_schema", "public",
            ),
            "table catalog digest": (
                "checks", "tables", "ordered_catalog_sha256", "a" * 64,
            ),
            "typed route entity": (
                "checks", "relation_options", "rows", 0, "entity", "mutated",
            ),
            "typed route width": (
                "checks", "relation_options", "rows", 0,
                "storage_width", 1,
            ),
            "ACL SQLSTATE": (
                "checks", "acl", "denied_sqlstate", "00000",
            ),
            "read-only table count": (
                "checks", "read_only", "table_count", 5,
            ),
            "read-only table SQLSTATE": (
                "checks", "read_only", "table_sqlstates", "taxonomy", "XX000",
            ),
            "empty size": (
                "checks", "empty_relation", "relation_size", 1,
            ),
            "lifecycle drop": (
                "checks", "create_drop_using_am", "dropped", False,
            ),
            "lifecycle typed width": (
                "checks", "create_drop_using_am", "stored_storage_width", 1,
            ),
            "lifecycle stale catalog row": (
                "checks", "create_drop_using_am",
                "stale_catalog_row_count", 1,
            ),
            "invalid entity SQLSTATE": (
                "checks", "create_drop_using_am",
                "invalid_entity_sqlstate", "00000",
            ),
            "missing entity message": (
                "checks", "create_drop_using_am",
                "missing_entity_message", "mutated",
            ),
            "missing entity typed row": (
                "checks", "create_drop_using_am",
                "missing_stored_entity", "taxonomy",
            ),
            "missing entity stale catalog row": (
                "checks", "create_drop_using_am",
                "missing_stale_catalog_row_count", 1,
            ),
            "cleanup residue": (
                "checks", "drop_extension", "access_method_count", 1,
            ),
            "evidence record hash": (
                "evidence_files", "queries/probe.sql", "sha256", "b" * 64,
            ),
            "typed catalog transcript hash": (
                "evidence_files", "queries/relation_options_catalog.sql",
                "sha256", "c" * 64,
            ),
        }
        for label, (*keys, replacement) in path_mutations.items():
            mutated = copy.deepcopy(receipt)
            target: Any = mutated
            for key in keys[:-1]:
                target = target[key]
            target[keys[-1]] = replacement
            _expect_resealed_receipt_failure(
                label,
                mutated,
                evidence_root=evidence_root,
                mutation_root=mutation_root,
            )

        exact_schema_mutations: list[tuple[str, tuple[str, ...]]] = [
            ("top-level exact schema", ()),
            ("input exact schema", ("input_provenance",)),
            ("package exact schema", ("generated_package",)),
            ("PostgreSQL exact schema", ("postgresql",)),
            ("stage exact schema", ("stage",)),
            (
                "artifact specification exact schema",
                ("stage", "artifact_spec"),
            ),
            (
                "runtime closure exact schema",
                ("stage", "runtime_closure"),
            ),
            ("cluster exact schema", ("cluster",)),
            ("checks exact schema", ("checks",)),
            (
                "evidence exact schema",
                ("evidence_files", "queries/probe.sql"),
            ),
        ]
        for label, keys in exact_schema_mutations:
            mutated = copy.deepcopy(receipt)
            target = mutated
            for key in keys:
                target = target[key]
            target["unexpected"] = True
            _expect_resealed_receipt_failure(
                label,
                mutated,
                evidence_root=evidence_root,
                mutation_root=mutation_root,
            )

        missing_read_only = copy.deepcopy(receipt)
        del missing_read_only[
            "checks"
        ]["read_only"]["table_sqlstates"]["taxonomy"]
        _expect_resealed_receipt_failure(
            "read-only six-table closure",
            missing_read_only,
            evidence_root=evidence_root,
            mutation_root=mutation_root,
        )

        missing_input = copy.deepcopy(receipt)
        del missing_input["input_provenance"]["files"]["mode_source"]
        _expect_resealed_receipt_failure(
            "input file exact closure",
            missing_input,
            evidence_root=evidence_root,
            mutation_root=mutation_root,
        )

        stale_input_seal = copy.deepcopy(receipt)
        stale_input_seal["input_provenance"]["seal_sha256"] = "c" * 64
        _expect_resealed_receipt_failure(
            "input seal binding",
            stale_input_seal,
            evidence_root=evidence_root,
            mutation_root=mutation_root,
            reseal_identity=False,
        )
        stale_receipt_id = copy.deepcopy(receipt)
        stale_receipt_id["receipt_id"] = "d" * 64
        _expect_resealed_receipt_failure(
            "receipt ID binding",
            stale_receipt_id,
            evidence_root=evidence_root,
            mutation_root=mutation_root,
            reseal_identity=False,
        )

        seal_test_root = temporary_root / "seal-test"
        receipt_path = seal_test_root / "receipt.json"
        seal_path = seal_test_root / "receipt.sha256"
        _write_json(receipt_path, receipt)
        _write_bytes(
            seal_path,
            f"{_sha256(receipt_path)}  receipt.json\n".encode("ascii"),
        )
        _validate_receipt_seal(receipt_path, seal_path)
        seal_path.write_text("0" * 64 + "  receipt.json\n", encoding="ascii")
        _expect_failure(
            "receipt seal",
            lambda: _validate_receipt_seal(receipt_path, seal_path),
        )

        (evidence_root / "queries" / "probe.sql").write_text(
            "SELECT 2;\n", encoding="utf-8"
        )
        _expect_failure(
            "on-disk transcript hash",
            lambda: _validate_receipt(receipt, bundle_root=evidence_root),
        )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("self-test")
    run = subparsers.add_parser("run")
    for name in (
        "repository_root",
        "output_root",
        "cmake",
        "git",
        "pg_config",
        "stage_script",
        "config_template",
        "control_file",
        "install_sql",
        "manifest",
        "dso",
        "static_contract_script",
        "cmake_fragment",
        "mode_source",
    ):
        run.add_argument(
            "--" + name.replace("_", "-"),
            type=Path,
            required=True,
        )
    run.add_argument("--timeout-seconds", type=int, default=180)
    return parser


def main() -> int:
    args = _parser().parse_args()
    if args.command == "self-test":
        run_self_tests()
        print("external PostgreSQL acceptance mutation self-tests: passed")
        return 0
    receipt = _run_live(args)
    print(f"external PostgreSQL acceptance receipt: {receipt}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ExternalAcceptanceError as error:
        print(f"external PostgreSQL acceptance failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
