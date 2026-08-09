#!/usr/bin/env python3
"""Verify the feature-gated generated external PostgreSQL catalog surface."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any


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

EXPECTED_TOP_LEVEL_IDENTITIES = {
    "sql:function:bioseq.bioseq_external_handler(internal)",
    (
        "sql:access_method:$extension."
        "bioseq::external::sql_routines.bioseq_external"
    ),
    "sql:schema:bioseq_external",
    *(
        f"sql:table:bioseq_external.{name}"
        for name in EXPECTED_TABLES
    ),
    (
        "sql:revoke:schema:bioseq_external:"
        "all:public:ordinary"
    ),
    (
        "sql:revoke:all tables in schema:bioseq_external:"
        "all:public:ordinary"
    ),
    (
        "sql:revoke:function:"
        "bioseq.bioseq_external_handler(internal):"
        "execute:public:ordinary"
    ),
}

EXTERNAL_SOURCE_PATHS = {
    "main/bioseq/backend/pg/external/relation.cppm",
    "main/bioseq/backend/pg/external/sql_catalog.cppm",
}

KSW_TEST_SOURCE_PATH = (
    "main/bioseq/backend/pg/highway_ksw_target_test_support.cppm"
)
KSW_TEST_SOURCE_MODULE = "bioseq.highway_ksw_target_test_support"
KSW_TEST_FUNCTION_IDENTITY = (
    "sql:function:bioseq_internal."
    "highway_ksw_boundary_receipt_for_test()"
)
KSW_TEST_REVOKE_IDENTITY = (
    "sql:revoke:function:bioseq_internal."
    "highway_ksw_boundary_receipt_for_test():"
    "execute:public:ordinary"
)
KSW_TEST_SQL_SUFFIX = (
    b"\n"
    b"CREATE OR REPLACE FUNCTION "
    b"bioseq_internal.highway_ksw_boundary_receipt_for_test()\n"
    b"RETURNS text\n"
    b"AS 'MODULE_PATHNAME', 'highway_ksw_boundary_receipt_for_test'\n"
    b"LANGUAGE C\n"
    b"VOLATILE\n"
    b"PARALLEL UNSAFE\n"
    b"SECURITY INVOKER;\n"
    b"\n"
    b"REVOKE ALL ON FUNCTION "
    b"bioseq_internal.highway_ksw_boundary_receipt_for_test() "
    b"FROM PUBLIC;\n"
)


def _read_manifest(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError(f"{path}: manifest must be one JSON object")
    return value


def _normalize_profile_sql(
    sql_bytes: bytes,
    manifest: dict[str, Any],
) -> tuple[bytes, int]:
    profile = manifest.get("compiler_profile")
    if profile not in {"production", "test_indexes"}:
        raise RuntimeError(
            "external feature contract received an unknown compiler profile: "
            f"{profile!r}"
        )

    entities = manifest.get("entities")
    statements = manifest.get("physical_statements")
    if not isinstance(entities, list):
        raise RuntimeError("generated SQL manifest has no entity array")
    if not isinstance(statements, list):
        raise RuntimeError(
            "generated SQL manifest has no physical statement array"
        )
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
            or KSW_TEST_SQL_SUFFIX in sql_bytes
        ):
            raise RuntimeError(
                "production SQL contains the test-indexes-only KSW receipt"
            )
        return sql_bytes, 0

    if (
        sql_bytes.count(KSW_TEST_SQL_SUFFIX) != 1
        or not sql_bytes.endswith(KSW_TEST_SQL_SUFFIX)
    ):
        raise RuntimeError(
            "test-indexes SQL does not end in exactly the reviewed KSW "
            "receipt projection"
        )

    physical_count = manifest.get("physical_statement_count")
    if not isinstance(physical_count, int) or physical_count < 2:
        raise RuntimeError(
            "test-indexes manifest has an invalid physical statement count"
        )
    function_statement_id = f"S{physical_count - 1:03d}"
    revoke_statement_id = f"S{physical_count:03d}"
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
            entity.get("typed_install_owner"),
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
            False,
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
            False,
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
            statement.get("baseline_physical_id"),
            statement.get("baseline_ordinal"),
            statement.get("kind"),
            statement.get("facet"),
            statement.get("owner_identity"),
            statement.get("render_source"),
            statement.get("text_sha256"),
            statement.get("baseline_text_sha256"),
            statement.get("order_identity"),
            statement.get("source_module"),
        )
        for statement in ksw_statements
    )
    expected_statement_projection = (
        (
            function_statement_id,
            None,
            None,
            "function",
            "primary",
            KSW_TEST_FUNCTION_IDENTITY,
            "natural_projection",
            "6865781db32b7798d1313dc1baa1f1223c9961fc4ca37d4c3299f72d613b14d7",
            None,
            "",
            KSW_TEST_SOURCE_MODULE,
        ),
        (
            revoke_statement_id,
            None,
            None,
            "revoke",
            "primary",
            KSW_TEST_REVOKE_IDENTITY,
            "natural_projection",
            "7c424756b149781cd013a53381106ef4328ef1d4eb1e8d3abcbf7f1d772999cc",
            None,
            "",
            KSW_TEST_SOURCE_MODULE,
        ),
    )
    if statement_projection != expected_statement_projection:
        raise RuntimeError(
            "test-indexes manifest does not contain the exact reviewed KSW "
            "receipt physical projection"
        )
    if statements[-2:] != ksw_statements:
        raise RuntimeError(
            "test-indexes KSW receipt is not the terminal natural SQL delta"
        )
    return sql_bytes[: -len(KSW_TEST_SQL_SUFFIX)], 2


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
        r"\((.*?)\)\s*USING bioseq_external\s*;",
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
    for match in table_pattern.finditer(sql):
        name = match.group(1)
        if name in actual_tables:
            raise RuntimeError(
                f"generated external package repeats table {name}"
            )
        actual_tables[name] = _parse_table_columns(
            match.group(2), name
        )
    if actual_tables != EXPECTED_TABLES:
        raise RuntimeError(
            "generated external table names, column order/types/nullability, "
            "or access-method clauses differ from the reviewed six-table "
            "surface"
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
    profile_statement_delta: int,
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
        if entity.get("typed_install_owner") is not False:
            raise RuntimeError(
                "external SQL entity is not naturally C++ owned"
            )
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
    access_method_identity = (
        "sql:access_method:$extension."
        "bioseq::external::sql_routines.bioseq_external"
    )
    access_method = by_identity[access_method_identity]
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
    for table_name in EXPECTED_TABLES:
        identity = f"sql:table:bioseq_external.{table_name}"
        if set(by_identity[identity].get("dependencies", [])) != {
            "sql:schema:bioseq_external",
            access_method_identity,
        }:
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

    statements = manifest.get("physical_statements")
    if not isinstance(statements, list):
        raise RuntimeError(
            "generated SQL manifest has no physical statement array"
        )
    external_statement_ids = [
        statement.get("baseline_physical_id")
        for statement in statements
        if isinstance(statement, dict)
        and statement.get("source_path") in EXTERNAL_SOURCE_PATHS
    ]
    expected_statement_ids = [
        f"P{ordinal:03d}" for ordinal in range(492, 505)
    ]
    if external_statement_ids != expected_statement_ids:
        raise RuntimeError(
            "external physical ledger projection is not exactly P492-P504"
        )
    if manifest.get("physical_statement_count") != (
        504 + profile_statement_delta
    ):
        raise RuntimeError(
            "enabled package does not close over the expected physical "
            "statements for its compiler profile"
        )


def verify(args: argparse.Namespace) -> str:
    sql_path = args.install_sql.resolve(strict=True)
    manifest_path = args.manifest.resolve(strict=True)
    base_golden_path = args.base_golden.resolve(strict=True)
    sql_bytes = sql_path.read_bytes()
    base_golden_bytes = base_golden_path.read_bytes()
    if not base_golden_bytes.endswith(b"\n"):
        raise RuntimeError(
            "checked base SQL golden must end with one newline"
        )
    manifest = _read_manifest(manifest_path)
    digest = hashlib.sha256(sql_bytes).hexdigest()
    if manifest.get("script_sha256") != digest:
        raise RuntimeError(
            "external feature contract received a stale SQL manifest"
        )
    base_sql_bytes, profile_statement_delta = _normalize_profile_sql(
        sql_bytes, manifest
    )
    sql = base_sql_bytes.decode("utf-8")
    if manifest.get("external_legacy_macro_symbols") != [
        "bioseq_external_handler"
    ]:
        raise RuntimeError(
            "external handler legacy-macro ownership is not exact"
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
        expected_base_prefix = base_golden_bytes[:-1] + b"\n\n"
        if not base_sql_bytes.startswith(expected_base_prefix):
            raise RuntimeError(
                "enabled external package changed the checked P001-P491 "
                "base SQL projection"
            )
        _verify_enabled_sql(sql)
        _verify_enabled_manifest(manifest, profile_statement_delta)
    else:
        if base_sql_bytes != base_golden_bytes:
            raise RuntimeError(
                "disabled external package differs from the checked "
                "P001-P491 base SQL golden"
            )
        if "bioseq_external" in sql:
            raise RuntimeError(
                "disabled SQL profile contains external catalog SQL"
            )
        if _external_entities(manifest):
            raise RuntimeError(
                "disabled SQL profile manifest contains external entities"
            )
        if manifest.get("physical_statement_count") != (
            491 + profile_statement_delta
        ):
            raise RuntimeError(
                "disabled package changed the expected base closure for its "
                "compiler profile"
            )
    return digest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--install-sql", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--base-golden", type=Path, required=True)
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
        "bioseq.external_generated_sql_contract/v1\n"
        f"expect={args.expect}\n"
        f"sha256={digest}\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
