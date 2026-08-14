#!/usr/bin/env python3
"""Verify the feature-gated generated external PostgreSQL catalog surface."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any


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


EXPECTED_TABLES: dict[str, tuple[tuple[str, str, bool], ...]] = {
    "taxonomy": (
        ("provider", "text", True),
        ("taxon_id", "bigint", False),
        ("provider_taxon_id", "text", True),
        ("scientific_name", "text", False),
        ("common_name", "text", False),
        ("rank", "text", False),
        ("parent_taxon_id", "bigint", False),
        ("lineage", "bigint[]", False),
        ("synonyms", "text[]", False),
        ("provider_attributes", "jsonb", False),
        ("provider_version", "text", False),
        ("provenance", "jsonb", True),
        ("retrieved_at", "timestamptz", True),
    ),
    "catalog": (
        ("provider", "text", True),
        ("domain", "text", True),
        ("entity_kind", "text", True),
        ("molecule_kind", "text", False),
        ("namespace_name", "text", False),
        ("original_identifier", "text", True),
        ("accession", "text", True),
        ("accession_version", "integer", False),
        ("aliases", "jsonb", False),
        ("taxon_id", "bigint", False),
        ("scientific_name", "text", False),
        ("title", "text", False),
        ("genes", "jsonb", False),
        ("keywords", "text[]", False),
        ("locations", "jsonb", False),
        ("host_taxon_id", "bigint", False),
        ("geography", "text", False),
        ("lineage_name", "text", False),
        ("sequence_length", "bigint", False),
        ("assembly_accession", "text", False),
        ("assembly_database", "text", False),
        ("assembly_source", "text", False),
        ("assembly_level", "text", False),
        ("assembly_status", "text", False),
        ("release_date", "date", False),
        ("update_date", "date", False),
        ("is_reference", "boolean", False),
        ("is_reviewed", "boolean", False),
        ("is_annotated", "boolean", False),
        ("is_fragment", "boolean", False),
        ("retrievable", "boolean", True),
        ("provider_attributes", "jsonb", False),
        ("provider_version", "text", False),
        ("provenance", "jsonb", True),
        ("retrieved_at", "timestamptz", True),
    ),
    "record_genes": (
        ("provider", "text", True),
        ("domain", "text", True),
        ("namespace_name", "text", False),
        ("original_identifier", "text", True),
        ("accession", "text", True),
        ("accession_version", "integer", False),
        ("gene_index", "integer", True),
        ("gene_count", "integer", True),
        ("gene_namespace", "text", True),
        ("gene_id", "text", True),
        ("gene_symbol", "text", False),
        ("gene_name", "text", False),
        ("gene_type", "text", False),
        ("locus_tag", "text", False),
        ("provider_version", "text", False),
        ("provenance", "jsonb", True),
        ("retrieved_at", "timestamptz", True),
    ),
    "record_locations": (
        ("provider", "text", True),
        ("domain", "text", True),
        ("namespace_name", "text", False),
        ("original_identifier", "text", True),
        ("accession", "text", True),
        ("accession_version", "integer", False),
        ("location_index", "integer", True),
        ("location_count", "integer", True),
        ("assembly_accession", "text", False),
        ("assembly_database", "text", False),
        ("sequence_alias", "text", True),
        ("start0", "bigint", True),
        ("end0", "bigint", True),
        ("strand", "smallint", True),
        ("provider_version", "text", False),
        ("provenance", "jsonb", True),
        ("retrieved_at", "timestamptz", True),
    ),
    "sequences": (
        ("provider", "text", True),
        ("domain", "text", True),
        ("namespace_name", "text", False),
        ("original_identifier", "text", True),
        ("accession", "text", True),
        ("accession_version", "integer", False),
        ("molecule_kind", "text", True),
        ("taxon_id", "bigint", False),
        ("assembly_accession", "text", False),
        ("assembly_database", "text", False),
        ("chromosome", "text", False),
        ("start0", "bigint", False),
        ("end0", "bigint", False),
        ("strand", "smallint", False),
        ("sequence", "bioseq.seq", True),
        ("provider_version", "text", False),
        ("provenance", "jsonb", True),
        ("retrieved_at", "timestamptz", True),
    ),
    "sequence_chunks": (
        ("provider", "text", True),
        ("domain", "text", True),
        ("namespace_name", "text", False),
        ("original_identifier", "text", True),
        ("accession", "text", True),
        ("accession_version", "integer", False),
        ("molecule_kind", "text", True),
        ("taxon_id", "bigint", False),
        ("assembly_accession", "text", False),
        ("assembly_database", "text", False),
        ("chromosome", "text", False),
        ("start0", "bigint", False),
        ("end0", "bigint", False),
        ("strand", "smallint", False),
        ("chunk_index", "bigint", True),
        ("base_offset", "bigint", True),
        ("sequence_chunk", "bioseq.seq", True),
        ("end_of_record", "boolean", True),
        ("provider_version", "text", False),
        ("provenance", "jsonb", True),
        ("retrieved_at", "timestamptz", True),
    ),
}

EXPECTED_RELATION_ENTITIES: dict[str, str] = {
    "taxonomy": "taxonomy",
    "catalog": "catalog",
    "record_genes": "record_gene",
    "record_locations": "record_location",
    "sequences": "sequence",
    "sequence_chunks": "sequence_chunk",
}

EXTERNAL_OPTIONS_TABLE_IDENTITY = (
    "sql:table:bioseq_internal.external_relation_options"
)
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
        1,
        "entity",
        "text",
        "std::optional<text*>",
        True,
        "row_codec.text_pointer.nullable",
    ),
    (
        2,
        "alphabet",
        "text",
        "std::optional<text*>",
        True,
        "row_codec.text_pointer.nullable",
    ),
    (
        3,
        "storage_width",
        "integer",
        "std::int32_t",
        False,
        "row_codec.std::int32_t.required",
    ),
    (
        4,
        "chunk_bases",
        "integer",
        "std::int32_t",
        False,
        "row_codec.std::int32_t.required",
    ),
)

EXTERNAL_ACCESS_METHOD_IDENTITY = (
    "sql:access_method:$extension."
    "bioseq::postgres::external::sql_routines.bioseq_external"
)
EXTERNAL_CATALOG_DEPENDENCIES_IDENTITY = (
    "sql:catalog_dependencies:$extension."
    "bioseq::postgres::external::sql_routines.bioseq_external_handler"
)

EXPECTED_TOP_LEVEL_IDENTITIES = {
    "sql:function:bioseq.bioseq_external_handler(internal)",
    EXTERNAL_ACCESS_METHOD_IDENTITY,
    EXTERNAL_CATALOG_DEPENDENCIES_IDENTITY,
    "sql:schema:bioseq_external",
    *(
        f"sql:table:bioseq_external.{name}"
        for name in EXPECTED_TABLES
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
}

EXTERNAL_SOURCE_PATHS = {
    "bioseq/backend/pg/external/relation.cppm",
    "bioseq/backend/pg/external/sql_catalog.cppm",
}

KSW_TEST_SOURCE_PATH = (
    "bioseq/backend/pg/highway_ksw_target_test_support.cppm"
)
KSW_TEST_SOURCE_MODULE = "bioseq.highway_ksw_target_test_support"
KSW_TEST_FUNCTION_IDENTITY = (
    "sql:function:bioseq_internal."
    "highway_ksw_boundary_receipt_for_test()"
)
KSW_TEST_REVOKE_IDENTITY = (
    "sql:revoke:highway_ksw_boundary_receipt_for_test:"
    "from=bioseq::sql::public_role:"
    "privileges=bioseq::sql::privileges::all"
)
KSW_TEST_FUNCTION_SQL = (
    b"\n"
    b"CREATE OR REPLACE FUNCTION "
    b"bioseq_internal.highway_ksw_boundary_receipt_for_test()\n"
    b"RETURNS text\n"
    b"AS 'MODULE_PATHNAME', 'highway_ksw_boundary_receipt_for_test'\n"
    b"LANGUAGE C\n"
    b"VOLATILE\n"
    b"PARALLEL UNSAFE\n"
    b"SECURITY INVOKER;\n"
)
KSW_TEST_REVOKE_SQL = (
    b"\n"
    b"REVOKE ALL ON FUNCTION "
    b"bioseq_internal.highway_ksw_boundary_receipt_for_test() "
    b"FROM PUBLIC;\n"
)
# Natural graph order separates the function from its dependent revoke, so
# validate and remove the two exact profile-only statements independently.
KSW_TEST_SQL_STATEMENTS = (
    KSW_TEST_FUNCTION_SQL,
    KSW_TEST_REVOKE_SQL,
)


def _read_manifest(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError(f"{path}: manifest must be one JSON object")
    unknown = sorted(set(value) - GENERATED_SQL_MANIFEST_FIELDS)
    if unknown:
        raise RuntimeError(
            f"{path}: generated SQL manifest has unknown fields {unknown}"
        )
    if (
        type(value.get("schema_revision")) is not int
        or value["schema_revision"] != 1
    ):
        raise RuntimeError(
            f"{path}: generated SQL manifest schema_revision must be 1"
        )
    return value


def _normalize_profile_sql(
    sql_bytes: bytes,
    manifest: dict[str, Any],
) -> bytes:
    profile = manifest.get("compiler_profile")
    if profile not in {"production", "test_indexes"}:
        raise RuntimeError(
            "external feature contract received an unknown compiler profile: "
            f"{profile!r}"
        )

    entities = manifest.get("entities")
    statements = manifest.get("statements")
    if not isinstance(entities, list):
        raise RuntimeError("generated SQL manifest has no entity array")
    if not isinstance(statements, list):
        raise RuntimeError("generated SQL manifest has no statement array")
    ksw_entities = [
        entity
        for entity in entities
        if isinstance(entity, dict)
        and entity.get("source_path") == KSW_TEST_SOURCE_PATH
    ]
    ksw_statements = [
        statement
        for statement in statements
        if isinstance(statement, dict)
        and statement.get("source_path") == KSW_TEST_SOURCE_PATH
    ]

    if profile == "production":
        if (
            ksw_entities
            or ksw_statements
            or any(
                statement in sql_bytes
                for statement in KSW_TEST_SQL_STATEMENTS
            )
        ):
            raise RuntimeError(
                "production SQL contains the test-indexes-only KSW receipt"
            )
        return sql_bytes

    if any(
        sql_bytes.count(statement) != 1
        for statement in KSW_TEST_SQL_STATEMENTS
    ):
        raise RuntimeError(
            "test-indexes SQL does not contain exactly the reviewed KSW "
            "receipt projection"
        )

    if len(ksw_statements) != 2:
        raise RuntimeError(
            "test-indexes manifest does not have exactly two KSW statements"
        )
    function_statement_id = ksw_statements[0].get("statement_id")
    revoke_statement_id = ksw_statements[1].get("statement_id")
    if not isinstance(function_statement_id, str) or not isinstance(
        revoke_statement_id, str
    ):
        raise RuntimeError("test-indexes KSW statements have invalid identities")
    active_condition = "(defined(BIOSEQ_TEST_INDEXES))"

    entity_projection = tuple(
        (
            entity.get("stable_identity"),
            entity.get("kind"),
            entity.get("source_module"),
            entity.get("active_condition"),
            tuple(entity.get("dependencies", []))
            if isinstance(entity.get("dependencies"), list)
            else None,
            tuple(entity.get("statement_ids", []))
            if isinstance(entity.get("statement_ids"), list)
            else None,
            entity.get("result_sql_type"),
            (
                entity.get("routine_properties", {}).get("c_symbol"),
                entity.get("routine_properties", {}).get(
                    "generated_wrapper"
                ),
                entity.get("routine_properties", {}).get("create_mode"),
                entity.get("routine_properties", {}).get("volatility"),
                entity.get("routine_properties", {}).get("null_input"),
                entity.get("routine_properties", {}).get("security"),
                entity.get("routine_properties", {}).get("parallel"),
            )
            if isinstance(entity.get("routine_properties"), dict)
            else None,
            (
                entity.get("c_binding", {}).get("symbol"),
                entity.get("c_binding", {}).get("finfo_symbol"),
                entity.get("c_binding", {}).get("routine_identity"),
                entity.get("c_binding", {}).get(
                    "implementation_declaration"
                ),
                entity.get("c_binding", {}).get("generated_wrapper"),
            )
            if isinstance(entity.get("c_binding"), dict)
            else None,
        )
        for entity in ksw_entities
    )
    expected_entity_projection = (
        (
            KSW_TEST_FUNCTION_IDENTITY,
            "function",
            KSW_TEST_SOURCE_MODULE,
            active_condition,
            ("sql:schema:bioseq_internal",),
            (function_statement_id,),
            "text",
            (
                "highway_ksw_boundary_receipt_for_test",
                True,
                "or_replace",
                "volatile",
                "called",
                "invoker",
                "unsafe",
            ),
            (
                "highway_ksw_boundary_receipt_for_test",
                "pg_finfo_highway_ksw_boundary_receipt_for_test",
                "bioseq_internal.highway_ksw_boundary_receipt_for_test()",
                (
                    "bioseq::pg::sql_routines::"
                    "highway_ksw_boundary_receipt_for_test"
                ),
                True,
            ),
        ),
        (
            KSW_TEST_REVOKE_IDENTITY,
            "revoke",
            KSW_TEST_SOURCE_MODULE,
            active_condition,
            (KSW_TEST_FUNCTION_IDENTITY,),
            (revoke_statement_id,),
            "",
            None,
            None,
        ),
    )
    if entity_projection != expected_entity_projection:
        raise RuntimeError(
            "test-indexes manifest does not contain the exact reviewed KSW "
            "receipt entity projection"
        )

    statement_projection = tuple(
        (
            statement.get("statement_id"),
            statement.get("kind"),
            statement.get("owner_identity"),
            statement.get("text_sha256"),
            statement.get("source_module"),
        )
        for statement in ksw_statements
    )
    expected_statement_projection = (
        (
            function_statement_id,
            "function",
            KSW_TEST_FUNCTION_IDENTITY,
            "6865781db32b7798d1313dc1baa1f1223c9961fc4ca37d4c3299f72d613b14d7",
            KSW_TEST_SOURCE_MODULE,
        ),
        (
            revoke_statement_id,
            "revoke",
            KSW_TEST_REVOKE_IDENTITY,
            "7c424756b149781cd013a53381106ef4328ef1d4eb1e8d3abcbf7f1d772999cc",
            KSW_TEST_SOURCE_MODULE,
        ),
    )
    if statement_projection != expected_statement_projection:
        raise RuntimeError(
            "test-indexes manifest does not contain the exact reviewed KSW "
            "receipt statement projection"
        )
    base_sql_bytes = sql_bytes
    for statement in KSW_TEST_SQL_STATEMENTS:
        base_sql_bytes = base_sql_bytes.replace(statement, b"", 1)
    return base_sql_bytes


def _parse_table_columns(body: str, table: str) -> tuple[tuple[str, str, bool], ...]:
    result: list[tuple[str, str, bool]] = []
    for raw_column in body.split(","):
        column = " ".join(raw_column.split())
        match = re.fullmatch(
            r"([a-z_][a-z0-9_]*) (.+?)( NOT NULL)?",
            column,
        )
        if match is None:
            raise RuntimeError(
                f"cannot parse generated bioseq_external.{table} column: "
                f"{column!r}"
            )
        result.append(
            (
                match.group(1),
                match.group(2),
                match.group(3) is not None,
            )
        )
    return tuple(result)


def _compact_cpp_type(value: object) -> str:
    if not isinstance(value, str):
        return ""
    return "".join(value.split())


def _verify_external_options_sql(
    sql: str,
    *,
    expect_public_tables: bool,
) -> None:
    table_pattern = re.compile(
        r"CREATE TABLE bioseq_internal\.external_relation_options\s*"
        r"\((.*?)\)\s*USING heap\s*"
        r"WITH\s*\(\s*user_catalog_table\s*=\s*true\s*\)\s*;",
        re.IGNORECASE | re.DOTALL,
    )
    matches = list(table_pattern.finditer(sql))
    if len(matches) != 1:
        raise RuntimeError(
            "generated package must create exactly one typed private "
            "external-relation option catalog"
        )
    actual_body = " ".join(matches[0].group(1).split())
    expected_body = (
        "regclass regclass NOT NULL, "
        "entity text, "
        "alphabet text, "
        "storage_width integer NOT NULL DEFAULT 0, "
        "chunk_bases integer NOT NULL DEFAULT 0, "
        "PRIMARY KEY (regclass), "
        "CHECK (entity IS NULL OR entity IN ( "
        "'taxonomy', 'catalog', 'record_gene', 'record_location', "
        "'sequence', 'sequence_chunk')), "
        "CHECK (storage_width BETWEEN 0 AND 64), "
        "CHECK (chunk_bases >= 0)"
    )
    if actual_body != expected_body:
        raise RuntimeError(
            "private external-relation option catalog columns, typed "
            "defaults, primary key, or checks differ from the exact contract"
        )
    config_dump = (
        "SELECT pg_catalog.pg_extension_config_dump("
        "'bioseq_internal.external_relation_options', 'WHERE FALSE');"
    )
    if sql.count(config_dump) != 1:
        raise RuntimeError(
            "private external-relation option catalog must have exactly one "
            "schema-only extension config-dump registration"
        )
    if expect_public_tables:
        metadata_position = matches[0].start()
        for table_name in EXPECTED_TABLES:
            public_position = sql.find(
                f"CREATE TABLE bioseq_external.{table_name}"
            )
            if public_position < 0 or metadata_position >= public_position:
                raise RuntimeError(
                    "private external-relation option metadata must render "
                    f"before bioseq_external.{table_name}"
                )


def _verify_enabled_sql(sql: str) -> None:
    function = re.compile(
        r"CREATE FUNCTION bioseq\.bioseq_external_handler"
        r"\((?:fcinfo\s+)?internal\)\s+RETURNS table_am_handler\s+"
        r"AS 'MODULE_PATHNAME', 'bioseq_external_handler'\s+"
        r"LANGUAGE C\b.*?\bSTRICT\s*;",
        re.IGNORECASE | re.DOTALL,
    )
    if function.search(sql) is None:
        raise RuntimeError(
            "generated external package is missing the strict C handler"
        )
    handler_comment = (
        "COMMENT ON FUNCTION "
        "bioseq.bioseq_external_handler(internal) IS "
        "'Returns the table access-method handler for external relations.';"
    )
    if sql.count(handler_comment) != 1:
        raise RuntimeError(
            "generated external handler comment is missing or not concise"
        )
    if not re.search(
        r"CREATE ACCESS METHOD bioseq_external\s+"
        r"TYPE TABLE\s+HANDLER bioseq\.bioseq_external_handler\s*;",
        sql,
        re.IGNORECASE,
    ):
        raise RuntimeError(
            "generated external package is missing the table access method"
        )
    if len(
        re.findall(
            r"CREATE SCHEMA bioseq_external\s*;",
            sql,
            re.IGNORECASE,
        )
    ) != 1:
        raise RuntimeError(
            "generated external package must create its schema exactly once"
        )

    table_pattern = re.compile(
        r"CREATE TABLE bioseq_external\.([a-z_][a-z0-9_]*)\s*"
        r"\((.*?)\)\s*USING bioseq_external\s*"
        r"WITH\s*\(\s*entity\s*=\s*'([a-z_][a-z0-9_]*)'\s*\)\s*;",
        re.IGNORECASE | re.DOTALL,
    )
    all_external_tables = re.findall(
        r"\bCREATE TABLE bioseq_external\.([a-z_][a-z0-9_]*)\b",
        sql,
        re.IGNORECASE,
    )
    if (
        len(all_external_tables) != len(EXPECTED_TABLES)
        or set(all_external_tables) != set(EXPECTED_TABLES)
    ):
        raise RuntimeError(
            "generated external package does not create exactly the "
            "reviewed six tables"
        )
    actual_tables: dict[str, tuple[tuple[str, str, bool], ...]] = {}
    actual_relation_entities: dict[str, str] = {}
    for match in table_pattern.finditer(sql):
        name = match.group(1)
        if name in actual_tables:
            raise RuntimeError(
                f"generated external package repeats table {name}"
            )
        actual_tables[name] = _parse_table_columns(
            match.group(2), name
        )
        actual_relation_entities[name] = match.group(3)
    if actual_tables != EXPECTED_TABLES:
        raise RuntimeError(
            "generated external table names, column order/types/nullability, "
            "or access-method clauses differ from the reviewed six-table "
            "surface"
        )
    if actual_relation_entities != EXPECTED_RELATION_ENTITIES:
        raise RuntimeError(
            "generated external table relation entity options differ from "
            "the reviewed six-table routing surface"
        )

    required_acls = (
        "REVOKE ALL ON SCHEMA bioseq_external FROM PUBLIC;",
        (
            "REVOKE ALL ON ALL TABLES IN SCHEMA "
            "bioseq_external FROM PUBLIC;"
        ),
        (
            "REVOKE ALL ON FUNCTION "
            "bioseq.bioseq_external_handler(internal) FROM PUBLIC;"
        ),
    )
    for acl in required_acls:
        if sql.count(acl) != 1:
            raise RuntimeError(
                f"generated external package has wrong ACL count for {acl}"
            )
    if "public.bioseq_external" in sql.lower():
        raise RuntimeError(
            "external fixed-schema SQL is polluted by the extension schema"
        )


def _verify_external_options_manifest(manifest: dict[str, Any]) -> None:
    raw_entities = manifest.get("entities")
    if not isinstance(raw_entities, list):
        raise RuntimeError("generated SQL manifest has no entity array")
    entities = [
        entity
        for entity in raw_entities
        if (
            isinstance(entity, dict)
            and entity.get("source_path") == EXTERNAL_OPTIONS_SOURCE_PATH
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
        kind: sum(entity.get("kind") == kind for entity in entities)
        for kind in ("table", "column", "primary_key", "check", "config_dump")
    }
    if kind_counts != {
        "table": 1,
        "column": 5,
        "primary_key": 1,
        "check": 3,
        "config_dump": 1,
    } or len(entities) != 11:
        raise RuntimeError(
            "private external-relation option catalog does not have exactly "
            "one table, five columns, one PK, three checks, and one config dump"
        )
    if any(
        entity.get("source_module") != EXTERNAL_OPTIONS_SOURCE_MODULE
        or entity.get("source_path") != EXTERNAL_OPTIONS_SOURCE_PATH
        for entity in entities
    ):
        raise RuntimeError(
            "private external-relation option catalog lost its existing "
            "storage-metadata source authority"
        )
    by_kind = {
        kind: [entity for entity in entities if entity.get("kind") == kind]
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
        raise RuntimeError(
            "private external-relation option catalog identity/schema contract "
            "is not exact"
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
        raise RuntimeError(
            "private external-relation option table child closure is stale"
        )
    for companion in (
        *by_kind["column"],
        *by_kind["primary_key"],
        *by_kind["check"],
        *by_kind["config_dump"],
    ):
        if set(companion.get("dependencies", [])) != {
            EXTERNAL_OPTIONS_TABLE_IDENTITY
        }:
            raise RuntimeError(
                "private external-relation option companion lost its exact "
                "table dependency"
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
        raise RuntimeError(
            "private external-relation option table lost its generated typed "
            "row-codec owner"
        )
    modules = manifest.get("row_codec_modules")
    if not isinstance(modules, list):
        raise RuntimeError("generated SQL manifest has no row-codec modules")
    matching_modules = [
        module
        for module in modules
        if (
            isinstance(module, dict)
            and module.get("module_name") == EXTERNAL_OPTIONS_ROW_CODEC_MODULE
        )
    ]
    if len(matching_modules) != 1:
        raise RuntimeError(
            "generated SQL manifest does not have exactly one storage-metadata "
            "row-codec module"
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
        raise RuntimeError(
            "storage-metadata row-codec module does not bind the private "
            "external option row exactly"
        )
    contracts = manifest.get("row_codec_contracts")
    if not isinstance(contracts, list):
        raise RuntimeError("generated SQL manifest has no row-codec contracts")
    matching_contracts = [
        contract
        for contract in contracts
        if (
            isinstance(contract, dict)
            and contract.get("row_identity") == EXTERNAL_OPTIONS_TABLE_IDENTITY
        )
    ]
    if len(matching_contracts) != 1:
        raise RuntimeError(
            "private external-relation option row has no unique typed codec "
            "contract"
        )
    contract = matching_contracts[0]
    fields = contract.get("fields")
    if (
        contract.get("row_cpp_name") != EXTERNAL_OPTIONS_CPP_NAME
        or contract.get("module_name") != EXTERNAL_OPTIONS_ROW_CODEC_MODULE
        or not isinstance(fields, list)
    ):
        raise RuntimeError(
            "private external-relation option row-codec identity is stale"
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
    if len(actual_fields) != len(fields) or actual_fields != (
        EXTERNAL_OPTIONS_ROW_CODEC_FIELDS
    ):
        raise RuntimeError(
            "private external-relation option typed row-codec fields differ "
            "in name/order/type/nullability/converter"
        )


def _external_entities(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    entities = manifest.get("entities")
    if not isinstance(entities, list):
        raise RuntimeError("generated SQL manifest has no entity array")
    result: list[dict[str, Any]] = []
    for index, raw_entity in enumerate(entities):
        if not isinstance(raw_entity, dict):
            raise RuntimeError(
                f"generated SQL manifest entities[{index}] is not an object"
            )
        if raw_entity.get("source_path") in EXTERNAL_SOURCE_PATHS:
            result.append(raw_entity)
    return result


def _verify_enabled_manifest(
    manifest: dict[str, Any],
) -> None:
    entities = _external_entities(manifest)
    top_level = {
        entity.get("stable_identity")
        for entity in entities
        if entity.get("kind") != "column"
    }
    if top_level != EXPECTED_TOP_LEVEL_IDENTITIES:
        raise RuntimeError(
            "enabled external manifest does not have the exact natural "
            "handler/AM/schema/six-table/ACL owner set"
        )
    all_manifest_entities = manifest.get("entities")
    if not isinstance(all_manifest_entities, list):
        raise RuntimeError("generated SQL manifest has no entity array")
    global_external_surface = {
        entity.get("stable_identity")
        for entity in all_manifest_entities
        if (
            isinstance(entity, dict)
            and entity.get("kind") != "column"
            and "bioseq_external"
            in str(entity.get("stable_identity", ""))
        )
    }
    if global_external_surface != EXPECTED_TOP_LEVEL_IDENTITIES:
        raise RuntimeError(
            "an owner outside the reviewed external sources polluted the "
            "generated external SQL surface"
        )
    for entity in entities:
        condition = entity.get("active_condition")
        if (
            not isinstance(condition, str)
            or "BIOSEQ_EXTERNAL_PG" not in condition
        ):
            raise RuntimeError(
                "external SQL entity escaped the BIOSEQ_EXTERNAL_PG gate"
            )

    by_identity = {
        entity["stable_identity"]: entity
        for entity in entities
    }
    handler = by_identity[
        "sql:function:bioseq.bioseq_external_handler(internal)"
    ]
    access_method_identity = EXTERNAL_ACCESS_METHOD_IDENTITY
    access_method = by_identity[access_method_identity]
    catalog_dependencies = by_identity[
        EXTERNAL_CATALOG_DEPENDENCIES_IDENTITY
    ]
    properties = handler.get("routine_properties")
    binding = handler.get("c_binding")
    if (
        not isinstance(properties, dict)
        or properties.get("c_symbol") != "bioseq_external_handler"
        or properties.get("generated_wrapper") is not False
        or not isinstance(binding, dict)
        or binding.get("generated_wrapper") is not False
    ):
        raise RuntimeError(
            "external handler lost its handwritten fmgr ownership exception"
        )
    if set(access_method.get("dependencies", [])) != {
        "sql:function:bioseq.bioseq_external_handler(internal)"
    }:
        raise RuntimeError(
            "external access method does not depend exactly on its handler"
        )
    if (
        catalog_dependencies.get("kind") != "catalog_dependencies"
        or set(catalog_dependencies.get("dependencies", [])) != {
            "sql:function:bioseq.bioseq_external_handler(internal)",
            EXTERNAL_OPTIONS_TABLE_IDENTITY,
        }
    ):
        raise RuntimeError(
            "external access-method catalog dependency does not root exactly "
            "the handler and typed option catalog"
        )
    for table_name in EXPECTED_TABLES:
        identity = f"sql:table:bioseq_external.{table_name}"
        expected_dependencies = {
            "sql:schema:bioseq_external",
            access_method_identity,
            EXTERNAL_OPTIONS_TABLE_IDENTITY,
        }
        if table_name in {"sequences", "sequence_chunks"}:
            expected_dependencies.add("sql:type:bioseq.seq")
        if set(by_identity[identity].get("dependencies", [])) != expected_dependencies:
            raise RuntimeError(
                f"external table has incomplete natural dependencies: "
                f"{identity}"
            )
    for identity, entity in by_identity.items():
        if identity.startswith(
            (
                "sql:function:bioseq.bioseq_external_handler",
                "sql:table:bioseq_external.",
            )
        ):
            if entity.get("schema_mode") != "fixed":
                raise RuntimeError(
                    f"fixed external entity became relocatable: {identity}"
                )
            if entity.get("stable_identity") != entity.get(
                "installed_identity"
            ):
                raise RuntimeError(
                    f"fixed external identity changed on installation: "
                    f"{identity}"
                )
            if "$extension" in identity:
                raise RuntimeError(
                    f"fixed external identity references extension schema: "
                    f"{identity}"
                )

    statements = manifest.get("statements")
    if not isinstance(statements, list):
        raise RuntimeError("generated SQL manifest has no statement array")
    external_statement_owners = {
        statement.get("owner_identity")
        for statement in statements
        if isinstance(statement, dict)
        and statement.get("source_path") in EXTERNAL_SOURCE_PATHS
    }
    if external_statement_owners != EXPECTED_TOP_LEVEL_IDENTITIES:
        raise RuntimeError(
            "enabled package statements do not exactly cover the reviewed "
            "external owner set"
        )
    statement_positions = {
        statement.get("owner_identity"): index
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
            raise RuntimeError(
                "typed external option metadata was rendered after public "
                f"table {table_name}"
            )
    seq_position = statement_positions.get("sql:type:bioseq.seq")
    for table_name in ("sequences", "sequence_chunks"):
        table_position = statement_positions.get(
            f"sql:table:bioseq_external.{table_name}"
        )
        if (
            not isinstance(seq_position, int)
            or not isinstance(table_position, int)
            or seq_position >= table_position
        ):
            raise RuntimeError(
                f"external table {table_name} was rendered before bioseq.seq"
            )


def verify(args: argparse.Namespace) -> str:
    sql_path = args.install_sql.resolve(strict=True)
    manifest_path = args.manifest.resolve(strict=True)
    sql_bytes = sql_path.read_bytes()
    manifest = _read_manifest(manifest_path)
    digest = hashlib.sha256(sql_bytes).hexdigest()
    if manifest.get("script_sha256") != digest:
        raise RuntimeError(
            "external feature contract received a stale SQL manifest"
        )
    base_sql_bytes = _normalize_profile_sql(sql_bytes, manifest)
    sql = base_sql_bytes.decode("utf-8")
    _verify_external_options_sql(
        sql,
        expect_public_tables=args.expect == "enabled",
    )
    _verify_external_options_manifest(manifest)
    handwritten_owners = manifest.get("handwritten_fmgr_export_owners")
    if (
        manifest.get("handwritten_fmgr_export_symbols") != []
        or manifest.get("handwritten_fmgr_export_symbol_count") != 0
        or manifest.get("handwritten_fmgr_export_external_symbols")
        != ["bioseq_external_handler"]
        or manifest.get("handwritten_fmgr_export_external_symbol_count") != 1
        or manifest.get("handwritten_fmgr_export_owner_count") != 1
        or not isinstance(handwritten_owners, list)
        or len(handwritten_owners) != 1
    ):
        raise RuntimeError(
            "external handler handwritten FMgr ownership is not exact"
        )
    handwritten_owner = handwritten_owners[0]
    if (
        not isinstance(handwritten_owner, dict)
        or set(handwritten_owner)
        != {"symbol", "macro", "source_module", "source_path", "source_line"}
        or {
            key: handwritten_owner.get(key)
            for key in ("symbol", "macro", "source_module", "source_path")
        }
        != {
            "symbol": "bioseq_external_handler",
            "macro": "POSTGRES_FN",
            "source_module": "bioseq.postgres.external.relation",
            "source_path": "bioseq/backend/pg/external/relation.cppm",
        }
        or type(handwritten_owner.get("source_line")) is not int
        or handwritten_owner["source_line"] <= 0
    ):
        raise RuntimeError(
            "external handler handwritten FMgr owner provenance is not exact"
        )
    symbol_owners = manifest.get("c_symbol_owners")
    if not isinstance(symbol_owners, list):
        raise RuntimeError(
            "generated SQL manifest has no C symbol owner array"
        )
    if any(
        isinstance(owner, dict)
        and owner.get("symbol") == "bioseq_external_handler"
        for owner in symbol_owners
    ):
        raise RuntimeError(
            "external handler polluted the generated main-wrapper inventory"
        )

    if args.expect == "enabled":
        _verify_enabled_sql(sql)
        _verify_enabled_manifest(manifest)
    else:
        if "bioseq_external" in sql:
            raise RuntimeError(
                "disabled SQL profile contains external catalog SQL"
            )
        if _external_entities(manifest):
            raise RuntimeError(
                "disabled SQL profile manifest contains external entities"
            )
    return digest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--install-sql", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument(
        "--expect",
        choices=("enabled", "disabled"),
        required=True,
    )
    parser.add_argument("--stamp", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    digest = verify(args)
    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    args.stamp.write_text(
        "bioseq.external_generated_sql_contract\n"
        "revision=1\n"
        f"expect={args.expect}\n"
        f"sha256={digest}\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
