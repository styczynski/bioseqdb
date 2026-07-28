#!/usr/bin/env python3
"""Generate and validate the fast, immutable DOC0388 ledger projections.

This tool deliberately has no PostgreSQL or build-tree dependency.  It reads
the immutable install SQL with ``git show`` and, when supplied, joins a
candidate SQL manifest.  It never reads the generated SQL golden as authority.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any, Iterable, Mapping, Sequence


class LedgerError(RuntimeError):
    """A DOC0388 ledger contract is incomplete or inconsistent."""


_REVISION = "e5e8a939a9e007c5640fc29da4302a9c92701dcf"
_LEGACY_PATH = "main/bioseq/sql/bioseq--1.0.sql"
_LEGACY_SHA256 = "0eb3aa6da75d80fc07c92dc1f6213ef27ddf1da7f4eaed33e6c69e6c8a7dfa71"
_STATEMENT_SCHEMA = "bioseq_doc0388_statement_disposition_ledger/v1"
_JOIN_SCHEMA = "bioseq_doc0388_catalog_join_projection/v1"
_FMGR_SCHEMA = "bioseq_doc0388_fmgr_migration_inventory/v1"
_RESULT_SCHEMA = "bioseq_doc0388_result_identity_ledger/v1"
_ROW_SCHEMA = "bioseq_doc0388_row_authority_migration_ledger/v1"
_DISPOSITIONS = frozenset({"retain", "fold", "replace", "remove"})
_ROUTINE_KINDS = frozenset({"create_function", "create_procedure"})
_ID_RE = re.compile(r"^([PS])(\d{3})$")
_INDEX_TABLE_SHARD_IDENTITY = "table:bioseq_internal.index_table_shard"
_INDEX_TABLE_SHARD_CODEC = "index_table_shard_codec"
_INDEX_TABLE_SHARD_OWNER = (
    "main/bioseq/backend/pg/storage_pg/indexing/sql_rows.cppm"
)
_INDEX_TABLE_SHARD_RUNTIME_SUPPORT = (
    "main/bioseq/backend/pg/pg_utils/row_codec_runtime_test_support.cppm"
)
_INDEX_TABLE_SHARD_RUNTIME_SYMBOL = (
    "bioseq::pg_utils::row_codec_runtime_test_support::"
    "exercise_all"
)
_INDEX_TABLE_SHARD_HISTORY = (
    "cae7c7021af8ac03680c531823630df4ab70ecce",
    _REVISION,
)
_INDEX_TABLE_SHARD_RUNTIME_OPERATIONS = (
    "catalog_delete",
    "catalog_insert",
    "catalog_update",
    "deform",
    "encode",
    "form_tuple",
    "modify",
    "scan_key",
    "update",
    "validate_descriptor",
)
_RULE_SCHEMA = "bioseq_doc0388_statement_disposition_rules/v1"
_RULE_TOP_LEVEL_FIELDS = frozenset({
    "schema",
    "legacy_sql",
    "default",
    "disposition_rules",
    "category_rules",
})
_RULE_DEFAULT_FIELDS = frozenset({
    "disposition",
    "reason",
    "approved_delta_categories",
})
_RULE_DISPOSITION_FIELDS = frozenset({
    "source_ids",
    "disposition",
    "reason",
    "replacement_or_absorbing_source_ids",
    "approved_delta_categories",
    "evidence_refs",
})
_RULE_CATEGORY_FIELDS = frozenset({
    "source_ids",
    "approved_delta_categories",
})
_APPROVED_DELTA_CATEGORIES = frozenset({
    "access-method-compatibility-replacement",
    "am-dependency-action-replacement",
    "ambient-search-path-removal",
    "bpchar-vector-repair",
    "dead-routine-removal",
    "derived-input-name",
    "derived-routine-symbol",
    "fixed-internal-schema",
    "fresh-install-backfill-removal",
    "historical-drop-removal",
    "intermediate-acl-fold",
    "native-routine-replacement",
    "native-table-option-path",
    "obsolete-search-path-reset-removal",
    "required-table-name-error-normalization",
    "staging-acl-removal",
    "vacuum-error-isolation-fix",
    "vacuum-private-temp-removal",
    "vector-null-boundary-hardening",
})
_EXPECTED_SCHEMA_REGISTRY: dict[str, Any] = {
    "schema": "bioseq_doc0388_ledger_schemas/v1",
    "artifacts": {
        _STATEMENT_SCHEMA: {
            "exact_row_count": 502,
            "required_row_fields": [
                "source_id",
                "byte_span",
                "line_span",
                "normalized_sha256",
                "statement_kind",
                "semantic_target_identities",
                "target_physical_ids",
                "disposition",
                "reason",
                "replacement_or_absorbing_source_ids",
                "approved_delta_categories",
                "evidence_refs",
            ],
        },
        _JOIN_SCHEMA: {
            "exact_counts": {
                "derived_routine_symbols": 118,
                "bpchar_identity_repairs": 6,
                "derived_input_names": 309,
                "derived_input_name_identities": 169,
            },
            "derived_input_name_fields": [
                "source_id",
                "routine_identity",
                "input_ordinal",
                "input_type",
                "target_cpp_identifier",
                "target_sql_name",
            ],
        },
        _FMGR_SCHEMA: {
            "exact_counts": {
                "legacy_physical_declarations": 268,
                "legacy_effective_owners": 266,
                "target_owners": 270,
            },
            "target_owner_fields": [
                "sql_identity",
                "legacy_source_id",
                "cpp_declaration",
                "source",
                "target_symbol",
                "inputs",
                "result",
                "injected_context",
            ],
        },
        _RESULT_SCHEMA: {
            "exact_counts": {
                "returns_table": 18,
                "named_out": 7,
                "total": 25,
            },
            "required_row_fields": [
                "source_id",
                "sql_identity",
                "wrapper_kind",
                "cpp_function",
                "row_cpp_declaration",
                "source",
                "result_fields",
            ],
        },
        _ROW_SCHEMA: {
            "exact_row_count": 14,
            "required_row_fields": [
                "sql_identity",
                "cpp_row_declaration",
                "source",
                "generated_codec",
                "ordered_fields",
                "legacy_authorities",
                "migrated_consumers",
            ],
            "conditional_consumer_dispositions": {
                "no_production_consumer_exists": {
                    "exact_sql_identity": _INDEX_TABLE_SHARD_IDENTITY,
                    "required_row_fields": [
                        "consumer_disposition",
                        "consumer_evidence",
                    ],
                },
            },
        },
    },
}
_ROW_LEGACY_AUTHORITIES: dict[str, tuple[str, ...]] = {
    "table:bioseq_internal.row_mask": (
        "RowMaskWriteStateEntry",
        "Natts_bioseq_row_mask/Anum_bioseq_row_mask_*",
        "handwritten tuple build/deform/update/scan keys",
    ),
    "table:bioseq_internal.options": (
        "FormDataBioseqOptions",
        "Natts_bioseq_options/Anum_bioseq_options_*",
        "handwritten tuple build/deform/update/scan keys",
    ),
    "table:bioseq_internal.stripe": (
        "StripeMetadata SQL projection",
        "Natts_bioseq_stripe/Anum_bioseq_stripe_*",
        "handwritten tuple build/deform/update/scan keys",
    ),
    "table:bioseq_internal.chunk_group": (
        "Natts_bioseq_chunkgroup/Anum_bioseq_chunkgroup_*",
        "handwritten tuple build/deform/update/scan keys",
    ),
    "table:bioseq_internal.chunk": (
        "Natts_bioseq_chunk/Anum_bioseq_chunk_*",
        "handwritten tuple build/deform/scan keys",
    ),
    "table:bioseq_internal.index_policy": (
        "raw SPI index-policy row shape",
    ),
    "table:bioseq_internal.metadata_age_log": (
        "string-built metadata-age row shape",
    ),
    _INDEX_TABLE_SHARD_IDENTITY: (
        "declared index-table-shard row shape with no production consumer",
    ),
    "table:bioseq_internal.seq_sub_table": (
        "SubTableRow",
        "anum_seq_sub_table",
        "handwritten tuple build/deform/update/scan keys",
    ),
    "table:bioseq_internal.minimizer_generation_storage": (
        "MinimizerGenerationStorageDescriptorPg",
        "handwritten descriptor conversion",
    ),
    "table:bioseq_internal.minimizer_generation_reaper_state": (
        "handwritten descriptor-array/SPI row path",
    ),
    "table:bioseq_internal.sequence_source_version": (
        "SequenceSourceVersionRow SQL projection",
        "handwritten tuple build/deform/update/scan keys",
    ),
    "type:$extension.bioseq_aligner_options": (
        "handwritten 36-entry expected_attributes/deform arrays",
    ),
    "type:bioseq.column_mapping": (
        "handwritten composite-array descriptor/deform path",
    ),
}
_ROW_PRODUCTION_CODEC_ALIASES: dict[str, tuple[str, ...]] = {
    "minimizer_generation_reaper_state_codec": (
        "minimizer_generation_reaper_codec",
    ),
    "bioseq_aligner_options_codec": (
        "aligner_options_codec",
    ),
}


def _sha256(data: bytes | str) -> str:
    if isinstance(data, str):
        data = data.encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def _json_bytes(value: object) -> bytes:
    return (json.dumps(
        value, indent=2, sort_keys=True, ensure_ascii=True,
    ) + "\n").encode("utf-8")


def _load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise LedgerError(f"cannot read {label} {path}: {error}") from error
    if not isinstance(value, dict):
        raise LedgerError(f"{label} {path} must contain a JSON object")
    return value


def _validate_schema_registry(registry: Mapping[str, Any]) -> None:
    """Require the checked schema registry to equal the code contract."""
    if dict(registry) != _EXPECTED_SCHEMA_REGISTRY:
        raise LedgerError(
            "DOC0388 ledger schema registry differs from the exact "
            "code-owned contract")


def _require_exact_fields(
    value: object,
    fields: Sequence[str],
    label: str,
) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise LedgerError(f"{label} must be an object")
    expected = set(fields)
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        unknown = sorted(actual - expected)
        raise LedgerError(
            f"{label} fields differ; missing={missing}, unknown={unknown}")
    return value


def _artifact_schema_contract(
    registry: Mapping[str, Any],
    schema: str,
) -> Mapping[str, Any]:
    _validate_schema_registry(registry)
    artifacts = registry.get("artifacts")
    if not isinstance(artifacts, dict):
        raise LedgerError("DOC0388 ledger schema registry has no artifacts")
    contract = artifacts.get(schema)
    if not isinstance(contract, dict):
        raise LedgerError(f"DOC0388 schema registry has no {schema} contract")
    return contract


def _validate_artifact_schema(
    value: Mapping[str, Any],
    registry: Mapping[str, Any],
) -> None:
    """Apply registry-declared exact counts and exact row field sets."""
    schema = value.get("schema")
    if not isinstance(schema, str):
        raise LedgerError("DOC0388 artifact has no schema")
    contract = _artifact_schema_contract(registry, schema)
    if schema == _STATEMENT_SCHEMA:
        rows = value.get("rows")
        if not isinstance(rows, list) or len(rows) != contract.get(
                "exact_row_count"):
            raise LedgerError(
                "statement ledger row count differs from schema registry")
        fields = contract.get("required_row_fields")
        if not isinstance(fields, list):
            raise LedgerError(
                "statement ledger schema has no exact row fields")
        for index, row in enumerate(rows):
            _require_exact_fields(
                row, fields, f"statement ledger row {index}")
        return
    if schema == _JOIN_SCHEMA:
        if value.get("counts") != contract.get("exact_counts"):
            raise LedgerError(
                "catalog join counts differ from schema registry")
        fields = contract.get("derived_input_name_fields")
        rows = value.get("derived_input_names")
        if not isinstance(fields, list) or not isinstance(rows, list):
            raise LedgerError(
                "catalog join schema/rows lack input-name fields")
        for index, row in enumerate(rows):
            _require_exact_fields(
                row, fields, f"catalog join input row {index}")
        return
    if schema == _FMGR_SCHEMA:
        if value.get("counts") != contract.get("exact_counts"):
            raise LedgerError(
                "fmgr inventory counts differ from schema registry")
        fields = contract.get("target_owner_fields")
        rows = value.get("target_owners")
        if not isinstance(fields, list) or not isinstance(rows, list):
            raise LedgerError(
                "fmgr schema/inventory lacks target-owner fields")
        for index, row in enumerate(rows):
            _require_exact_fields(
                row, fields, f"fmgr target owner {index}")
        return
    if schema == _RESULT_SCHEMA:
        if value.get("counts") != contract.get("exact_counts"):
            raise LedgerError(
                "result inventory counts differ from schema registry")
        fields = contract.get("required_row_fields")
        rows = value.get("rows")
        if not isinstance(fields, list) or not isinstance(rows, list):
            raise LedgerError(
                "result schema/inventory lacks exact row fields")
        for index, row in enumerate(rows):
            _require_exact_fields(
                row, fields, f"result inventory row {index}")
        return
    if schema == _ROW_SCHEMA:
        rows = value.get("rows")
        if not isinstance(rows, list) or len(rows) != contract.get(
                "exact_row_count"):
            raise LedgerError(
                "row authority count differs from schema registry")
        fields = contract.get("required_row_fields")
        conditional = contract.get("conditional_consumer_dispositions")
        if not isinstance(fields, list) or not isinstance(conditional, dict):
            raise LedgerError(
                "row authority schema lacks exact row/disposition fields")
        seen_dispositions: set[str] = set()
        for index, row in enumerate(rows):
            if not isinstance(row, dict):
                raise LedgerError(
                    f"row authority row {index} must be an object")
            disposition = row.get("consumer_disposition")
            exact_fields = list(fields)
            if disposition is not None:
                disposition_contract = conditional.get(disposition)
                if not isinstance(disposition_contract, dict):
                    raise LedgerError(
                        f"row authority row {index} has unsupported "
                        "consumer disposition")
                if row.get("sql_identity") != disposition_contract.get(
                        "exact_sql_identity"):
                    raise LedgerError(
                        f"row authority row {index} disposition identity "
                        "differs from schema registry")
                extra = disposition_contract.get("required_row_fields")
                if not isinstance(extra, list):
                    raise LedgerError(
                        "row disposition schema has no exact fields")
                exact_fields.extend(extra)
                seen_dispositions.add(str(disposition))
            _require_exact_fields(
                row, exact_fields, f"row authority row {index}")
        if seen_dispositions != set(conditional):
            raise LedgerError(
                "row authority conditional dispositions are not exact")
        return
    raise LedgerError(f"schema registry cannot validate artifact {schema}")


def _git_blob(repository_root: Path, git: str = "git") -> bytes:
    command = [git, "show", f"{_REVISION}:{_LEGACY_PATH}"]
    try:
        result = subprocess.run(
            command,
            cwd=repository_root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as error:
        raise LedgerError(f"cannot execute {git}: {error}") from error
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise LedgerError(f"{' '.join(command)} failed: {detail}")
    if _sha256(result.stdout) != _LEGACY_SHA256:
        raise LedgerError("immutable DOC0388 legacy SQL hash mismatch")
    if result.stdout.count(b"\n") != 3502:
        raise LedgerError("immutable DOC0388 legacy SQL must contain 3502 lines")
    return result.stdout


def _line_at(text: str, offset: int) -> int:
    return 1 + text.count("\n", 0, offset)


def _dollar_delimiter(text: str, offset: int) -> str | None:
    if offset >= len(text) or text[offset] != "$":
        return None
    cursor = offset + 1
    while cursor < len(text):
        value = text[cursor]
        if value == "$":
            return text[offset:cursor + 1]
        if not (value.isascii() and (value.isalnum() or value == "_")):
            return None
        cursor += 1
    return None


def _normalize_sql(text: str) -> str:
    result: list[str] = []
    pending_space = False
    single = False
    double = False
    line_comment = False
    block_depth = 0
    dollar: str | None = None
    index = 0
    while index < len(text):
        value = text[index]
        next_value = text[index + 1] if index + 1 < len(text) else "\0"
        if line_comment:
            if value == "\n":
                line_comment = False
                pending_space = True
            index += 1
            continue
        if block_depth:
            if value == "/" and next_value == "*":
                block_depth += 1
                index += 2
            elif value == "*" and next_value == "/":
                block_depth -= 1
                index += 2
                pending_space = True
            else:
                index += 1
            continue
        if dollar is not None:
            if text.startswith(dollar, index):
                result.append(dollar)
                index += len(dollar)
                dollar = None
            else:
                result.append(value)
                index += 1
            continue
        if single:
            result.append(value)
            if value == "'" and next_value == "'":
                result.append(next_value)
                index += 2
            else:
                if value == "'":
                    single = False
                index += 1
            continue
        if double:
            result.append(value)
            if value == '"' and next_value == '"':
                result.append(next_value)
                index += 2
            else:
                if value == '"':
                    double = False
                index += 1
            continue
        if value == "-" and next_value == "-":
            line_comment = True
            index += 2
            continue
        if value == "/" and next_value == "*":
            block_depth = 1
            index += 2
            continue
        if value in "'\"":
            if pending_space and result:
                result.append(" ")
            pending_space = False
            result.append(value)
            single = value == "'"
            double = value == '"'
            index += 1
            continue
        if value == "$":
            delimiter = _dollar_delimiter(text, index)
            if delimiter is not None:
                if pending_space and result:
                    result.append(" ")
                pending_space = False
                result.append(delimiter)
                dollar = delimiter
                index += len(delimiter)
                continue
        if value.isspace():
            pending_space = True
            index += 1
            continue
        if pending_space and result:
            result.append(" ")
        pending_space = False
        result.append(value.lower() if "A" <= value <= "Z" else value)
        index += 1
    return "".join(result).strip()


def _kind(normalized: str) -> str:
    checks = (
        ("alter", "alter"),
        ("comment", "comment"),
        ("create access method", "create_access_method"),
        ("create aggregate", "create_aggregate"),
        ("create cast", "create_cast"),
        ("create or replace function", "create_function"),
        ("create function", "create_function"),
        ("create unique index", "create_index"),
        ("create index", "create_index"),
        ("create operator class", "create_operator_class"),
        ("create operator", "create_operator"),
        ("create or replace procedure", "create_procedure"),
        ("create procedure", "create_procedure"),
        ("create schema", "create_schema"),
        ("create sequence", "create_sequence"),
        ("create unlogged table", "create_table"),
        ("create temporary table", "create_table"),
        ("create table", "create_table"),
        ("create type", "create_type"),
        ("create or replace view", "create_view"),
        ("create view", "create_view"),
        ("grant", "grant"),
        ("insert", "insert"),
        ("reset", "reset"),
        ("revoke", "revoke"),
        ("select", "select"),
        ("set", "set"),
    )
    for prefix, kind in checks:
        if normalized == prefix or normalized.startswith(prefix + " "):
            return kind
    return "unknown"


def _split_legacy(blob: bytes) -> tuple[str, list[dict[str, Any]]]:
    try:
        source = blob.decode("utf-8")
    except UnicodeDecodeError as error:
        raise LedgerError(f"legacy SQL is not UTF-8: {error}") from error
    if "\r" in source or "\0" in source:
        raise LedgerError("legacy SQL must be LF UTF-8 without NUL")
    byte_offsets = [0]
    for value in source:
        byte_offsets.append(byte_offsets[-1] + len(value.encode("utf-8")))
    guard_marker = source.find("\\echo")
    guard_end = source.find("\n", guard_marker)
    if not source.startswith("-- complain if script is sourced in psql"):
        raise LedgerError("legacy SQL has no canonical psql guard")
    if guard_marker < 0 or guard_end < 0:
        raise LedgerError("legacy SQL psql guard is unterminated")
    preamble = source[:guard_end + 1]
    statements: list[dict[str, Any]] = []
    begin = guard_end + 1
    single = double = line_comment = False
    block_depth = 0
    dollar: str | None = None
    index = begin
    while index < len(source):
        value = source[index]
        next_value = source[index + 1] if index + 1 < len(source) else "\0"
        if line_comment:
            if value == "\n":
                line_comment = False
            index += 1
            continue
        if block_depth:
            if value == "/" and next_value == "*":
                block_depth += 1
                index += 2
            elif value == "*" and next_value == "/":
                block_depth -= 1
                index += 2
            else:
                index += 1
            continue
        if dollar is not None:
            if source.startswith(dollar, index):
                index += len(dollar)
                dollar = None
            else:
                index += 1
            continue
        if single:
            if value == "'" and next_value == "'":
                index += 2
            else:
                if value == "'":
                    single = False
                index += 1
            continue
        if double:
            if value == '"' and next_value == '"':
                index += 2
            else:
                if value == '"':
                    double = False
                index += 1
            continue
        if value == "-" and next_value == "-":
            line_comment = True
            index += 2
            continue
        if value == "/" and next_value == "*":
            block_depth = 1
            index += 2
            continue
        if value == "'":
            single = True
            index += 1
            continue
        if value == '"':
            double = True
            index += 1
            continue
        if value == "$":
            delimiter = _dollar_delimiter(source, index)
            if delimiter is not None:
                dollar = delimiter
                index += len(delimiter)
                continue
        if value != ";":
            index += 1
            continue
        end = index + 1
        exact = source[begin:end]
        normalized = _normalize_sql(exact)
        if normalized:
            ordinal = len(statements) + 1
            statements.append({
                "source_id": f"S{ordinal:03d}",
                "byte_begin": byte_offsets[begin],
                "byte_end": byte_offsets[end],
                "line_begin": _line_at(source, begin),
                "line_end": _line_at(source, end),
                "exact": exact,
                "normalized": normalized,
                "normalized_sha256": _sha256(normalized),
                "statement_kind": _kind(normalized),
            })
        begin = end
        index = end
    if any((single, double, line_comment, block_depth, dollar is not None)):
        raise LedgerError("legacy SQL ends inside a quote or comment")
    if _normalize_sql(source[begin:]):
        raise LedgerError("legacy SQL has content after its final semicolon")
    if len(statements) != 501:
        raise LedgerError(
            f"legacy SQL produced {len(statements)} statements, expected 501")
    return preamble, statements


def _expand_id_tokens(tokens: object) -> list[str]:
    if not isinstance(tokens, list) or not all(
            isinstance(token, str) for token in tokens):
        raise LedgerError("source_ids must be a JSON string array")
    result: list[str] = []
    for token in tokens:
        match = re.fullmatch(r"([PS])(\d{3})(?:-([PS]?)(\d{3}))?", token)
        if match is None:
            raise LedgerError(f"invalid source ID selector {token!r}")
        prefix, start_text, end_prefix, end_text = match.groups()
        if end_text is None:
            result.append(token)
            continue
        if end_prefix and end_prefix != prefix:
            raise LedgerError(f"mixed-prefix source ID range {token!r}")
        start, end = int(start_text), int(end_text)
        if start > end:
            raise LedgerError(f"descending source ID range {token!r}")
        result.extend(f"{prefix}{value:03d}" for value in range(start, end + 1))
    if len(set(result)) != len(result):
        raise LedgerError("one source-id selector contains a duplicate")
    return result


def _string_array(
    value: object,
    label: str,
    *,
    allow_empty: bool,
) -> list[str]:
    if not isinstance(value, list) or not all(
            isinstance(item, str) and item for item in value):
        raise LedgerError(f"{label} must be a JSON string array")
    if not allow_empty and not value:
        raise LedgerError(f"{label} must not be empty")
    if len(set(value)) != len(value):
        raise LedgerError(f"{label} must not contain duplicates")
    return value


def _approved_categories(
    value: object,
    label: str,
    *,
    allow_empty: bool,
) -> list[str]:
    categories = _string_array(
        value, label, allow_empty=allow_empty)
    unknown = sorted(set(categories) - _APPROVED_DELTA_CATEGORIES)
    if unknown:
        raise LedgerError(
            f"{label} contains unknown category enums {unknown}")
    if categories != sorted(categories):
        raise LedgerError(f"{label} category enums must be sorted")
    return categories


def _disposition_rule_payload(
    value: object,
    label: str,
    *,
    default: bool,
) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise LedgerError(f"{label} must be an object")
    allowed = _RULE_DEFAULT_FIELDS if default else _RULE_DISPOSITION_FIELDS
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise LedgerError(f"{label} contains unknown fields {unknown}")
    required = (
        _RULE_DEFAULT_FIELDS
        if default else frozenset({"source_ids", "disposition", "reason"})
    )
    missing = sorted(required - set(value))
    if missing:
        raise LedgerError(f"{label} is missing required fields {missing}")
    disposition = value.get("disposition")
    if disposition not in _DISPOSITIONS:
        raise LedgerError(f"{label} has invalid disposition enum")
    reason = value.get("reason")
    if not isinstance(reason, str) or not reason.strip():
        raise LedgerError(f"{label} has no non-empty reason")
    if default:
        _approved_categories(
            value.get("approved_delta_categories"),
            f"{label}.approved_delta_categories",
            allow_empty=True,
        )
        return value
    _expand_id_tokens(value.get("source_ids"))
    if "approved_delta_categories" in value:
        _approved_categories(
            value["approved_delta_categories"],
            f"{label}.approved_delta_categories",
            allow_empty=True,
        )
    if "replacement_or_absorbing_source_ids" in value:
        replacements = _string_array(
            value["replacement_or_absorbing_source_ids"],
            f"{label}.replacement_or_absorbing_source_ids",
            allow_empty=False,
        )
        if any(_ID_RE.fullmatch(source_id) is None for source_id in replacements):
            raise LedgerError(
                f"{label} replacement/absorber IDs must be exact source IDs")
    if "evidence_refs" in value:
        _string_array(
            value["evidence_refs"],
            f"{label}.evidence_refs",
            allow_empty=False,
        )
    return value


def _compile_rules(rules: Mapping[str, Any]) -> dict[str, dict[str, Any]]:
    unknown_top_level = sorted(set(rules) - _RULE_TOP_LEVEL_FIELDS)
    missing_top_level = sorted(_RULE_TOP_LEVEL_FIELDS - set(rules))
    if unknown_top_level or missing_top_level:
        raise LedgerError(
            "statement rule top-level fields differ; "
            f"missing={missing_top_level}, unknown={unknown_top_level}")
    if rules.get("schema") != _RULE_SCHEMA:
        raise LedgerError("unsupported statement disposition rule schema")
    if rules.get("legacy_sql") != {
        "git_revision": _REVISION,
        "repository_path": _LEGACY_PATH,
        "sha256": _LEGACY_SHA256,
    }:
        raise LedgerError(
            "statement rules legacy_sql provenance differs from the exact "
            "pinned revision/path/hash")
    expected = {"P000"} | {f"S{value:03d}" for value in range(1, 502)}
    default = _disposition_rule_payload(
        rules.get("default"), "statement rule default", default=True)
    expanded = {source_id: copy.deepcopy(default) for source_id in expected}
    disposition_rules = rules.get("disposition_rules")
    category_rules = rules.get("category_rules")
    if not isinstance(disposition_rules, list):
        raise LedgerError("disposition_rules must be an array")
    if not isinstance(category_rules, list):
        raise LedgerError("category_rules must be an array")
    claimed: set[str] = set()
    for index, raw_rule in enumerate(disposition_rules):
        rule = _disposition_rule_payload(
            raw_rule, f"disposition rule {index}", default=False)
        ids = _expand_id_tokens(rule.get("source_ids"))
        overlap = claimed.intersection(ids)
        if overlap:
            raise LedgerError(
                "overlapping disposition rules for " + ", ".join(sorted(overlap)))
        claimed.update(ids)
        for source_id in ids:
            if source_id not in expected:
                raise LedgerError(f"out-of-domain source ID {source_id}")
            expanded[source_id].update({
                key: copy.deepcopy(value)
                for key, value in rule.items() if key != "source_ids"
            })
    for index, rule in enumerate(category_rules):
        if not isinstance(rule, dict):
            raise LedgerError("category rule must be an object")
        unknown = sorted(set(rule) - _RULE_CATEGORY_FIELDS)
        missing = sorted(_RULE_CATEGORY_FIELDS - set(rule))
        if unknown or missing:
            raise LedgerError(
                f"category rule {index} fields differ; "
                f"missing={missing}, unknown={unknown}")
        categories = _approved_categories(
            rule.get("approved_delta_categories"),
            f"category rule {index}.approved_delta_categories",
            allow_empty=False,
        )
        for source_id in _expand_id_tokens(rule.get("source_ids")):
            if source_id not in expected:
                raise LedgerError(f"out-of-domain source ID {source_id}")
            current = expanded[source_id].setdefault(
                "approved_delta_categories", [])
            current.extend(categories)
            expanded[source_id]["approved_delta_categories"] = sorted(set(current))
    for source_id, rule in expanded.items():
        disposition = rule.get("disposition")
        replacements = rule.get(
            "replacement_or_absorbing_source_ids", [])
        if any(replacement not in expected for replacement in replacements):
            raise LedgerError(
                f"{source_id} has an out-of-domain replacement/absorber ID")
        if disposition == "remove" and (
            not rule.get("approved_delta_categories")
            or not rule.get("evidence_refs")
        ):
            raise LedgerError(
                f"{source_id} removal rule lacks category/evidence")
    return expanded


def _manifest_indexes(
    manifest: Mapping[str, Any] | None,
) -> tuple[dict[str, dict[str, Any]], dict[str, str]]:
    if manifest is None:
        return {}, {}
    entities = manifest.get("entities")
    physical = manifest.get("physical_statements")
    if not isinstance(entities, list) or not isinstance(physical, list):
        raise LedgerError("candidate manifest lacks entities/physical_statements")
    by_identity: dict[str, dict[str, Any]] = {}
    for entity in entities:
        if not isinstance(entity, dict):
            raise LedgerError("candidate manifest entity is not an object")
        identity = entity.get("identity")
        if isinstance(identity, str):
            by_identity[identity.removeprefix("sql:")] = entity
    target_ids: dict[str, str] = {}
    for statement in physical:
        if not isinstance(statement, dict):
            continue
        statement_id = statement.get("statement_id")
        physical_id = statement.get("baseline_physical_id")
        if isinstance(statement_id, str) and isinstance(physical_id, str):
            target_ids[statement_id] = physical_id
    return by_identity, target_ids


def _statement_manifest_index(
    manifest: Mapping[str, Any] | None,
) -> dict[str, Any]:
    """Validate and index the exact candidate physical-statement closure."""
    if manifest is None:
        raise LedgerError(
            "candidate manifest is required for the statement source/target join")
    entities = manifest.get("entities")
    physical = manifest.get("physical_statements")
    if not isinstance(entities, list) or not isinstance(physical, list):
        raise LedgerError("candidate manifest lacks entities/physical_statements")
    if manifest.get("entity_count") != len(entities):
        raise LedgerError("candidate manifest entity_count is stale")
    if len(entities) != 721:
        raise LedgerError(
            "production candidate manifest must contain exactly 721 entities")
    if manifest.get("physical_statement_count") != len(physical):
        raise LedgerError("candidate manifest physical_statement_count is stale")
    if manifest.get("baseline_physical_statement_count") != len(physical):
        raise LedgerError(
            "candidate manifest baseline physical statement count is stale")
    if len(physical) != 487:
        raise LedgerError(
            "candidate manifest must contain exactly 487 physical statements")

    by_identity: dict[str, Mapping[str, Any]] = {}
    for ordinal, entity in enumerate(entities):
        if not isinstance(entity, dict):
            raise LedgerError(
                f"candidate manifest entity {ordinal} is not an object")
        identity = entity.get("identity")
        stable_identity = entity.get("stable_identity")
        statement_ids = entity.get("statement_ids")
        if not isinstance(identity, str) or not identity.startswith("sql:"):
            raise LedgerError(
                f"candidate manifest entity {ordinal} has no stable SQL identity")
        if stable_identity != identity:
            raise LedgerError(
                f"candidate manifest entity {identity} stable identity drifted")
        if identity in by_identity:
            raise LedgerError(
                f"candidate manifest contains duplicate entity {identity}")
        if not isinstance(statement_ids, list) or not all(
                isinstance(value, str) for value in statement_ids):
            raise LedgerError(
                f"candidate manifest entity {identity} has invalid statement IDs")
        if len(statement_ids) != len(set(statement_ids)):
            raise LedgerError(
                f"candidate manifest entity {identity} repeats a statement ID")
        by_identity[identity] = entity

    by_physical_id: dict[str, Mapping[str, Any]] = {}
    by_statement_id: dict[str, Mapping[str, Any]] = {}
    by_owner_facet: dict[
        tuple[str, str], Mapping[str, Any],
    ] = {}
    actual_statement_ids_by_owner: dict[str, list[str]] = {}
    for ordinal, statement in enumerate(physical, 1):
        if not isinstance(statement, dict):
            raise LedgerError(
                f"candidate physical statement {ordinal} is not an object")
        physical_id = statement.get("baseline_physical_id")
        statement_id = statement.get("statement_id")
        baseline_ordinal = statement.get("baseline_ordinal")
        owner_identity = statement.get("owner_identity")
        facet = statement.get("facet")
        expected_physical_id = f"P{ordinal:03d}"
        expected_statement_id = f"S{ordinal:03d}"
        if (
            physical_id != expected_physical_id
            or statement_id != expected_statement_id
            or baseline_ordinal != ordinal
        ):
            raise LedgerError(
                "candidate physical statement ordering/identity drift at "
                f"ordinal {ordinal}")
        if not isinstance(owner_identity, str) or owner_identity not in by_identity:
            raise LedgerError(
                f"{physical_id} has missing manifest owner {owner_identity!r}")
        if not isinstance(facet, str) or not facet:
            raise LedgerError(f"{physical_id} has no physical facet")
        owner_facet = (owner_identity, facet)
        if owner_facet in by_owner_facet:
            raise LedgerError(
                f"candidate manifest repeats physical owner/facet {owner_facet}")
        by_physical_id[str(physical_id)] = statement
        by_statement_id[str(statement_id)] = statement
        by_owner_facet[owner_facet] = statement
        actual_statement_ids_by_owner.setdefault(owner_identity, []).append(
            str(statement_id))

    if len(by_physical_id) != 487 or len(by_statement_id) != 487:
        raise LedgerError(
            "candidate manifest physical/statement IDs are not unique")
    for identity, entity in by_identity.items():
        declared = entity.get("statement_ids")
        actual = actual_statement_ids_by_owner.get(identity, [])
        if declared != actual:
            raise LedgerError(
                f"candidate manifest entity {identity} physical closure "
                f"differs; declared={declared}, actual={actual}")
    return {
        "entities": entities,
        "by_identity": by_identity,
        "by_physical_id": by_physical_id,
        "by_statement_id": by_statement_id,
        "by_owner_facet": by_owner_facet,
        "physical_ids": [f"P{ordinal:03d}" for ordinal in range(1, 488)],
    }


def _statement_ledger(
    blob: bytes,
    rules: Mapping[str, Any],
    manifest: Mapping[str, Any] | None,
    delta: Mapping[str, Any],
) -> dict[str, Any]:
    preamble, statements = _split_legacy(blob)
    compiled = _compile_rules(rules)
    targets, manifest_index = _derive_statement_targets(
        statements, compiled, manifest, delta)
    rows: list[dict[str, Any]] = []
    preamble_rule = compiled["P000"]
    rows.append({
        "source_id": "P000",
        "byte_span": {"begin": 0, "end": len(preamble.encode("utf-8"))},
        "line_span": {"begin": 1, "end": _line_at(preamble, len(preamble))},
        "normalized_sha256": _sha256(_normalize_sql(preamble)),
        "statement_kind": "psql_guard",
        "semantic_target_identities": ["psql-guard:bioseq"],
        "target_physical_ids": ["P000"],
        "disposition": preamble_rule["disposition"],
        "reason": preamble_rule["reason"],
        "replacement_or_absorbing_source_ids":
            preamble_rule.get("replacement_or_absorbing_source_ids", []),
        "approved_delta_categories":
            preamble_rule.get("approved_delta_categories", []),
        "evidence_refs": preamble_rule.get("evidence_refs", []),
    })
    for statement in statements:
        source_id = str(statement["source_id"])
        rule = compiled[source_id]
        identities, physical_ids = targets[source_id]
        rows.append({
            "source_id": source_id,
            "byte_span": {
                "begin": statement["byte_begin"],
                "end": statement["byte_end"],
            },
            "line_span": {
                "begin": statement["line_begin"],
                "end": statement["line_end"],
            },
            "normalized_sha256": statement["normalized_sha256"],
            "statement_kind": statement["statement_kind"],
            "semantic_target_identities": identities,
            "target_physical_ids": physical_ids,
            "disposition": rule["disposition"],
            "reason": rule["reason"],
            "replacement_or_absorbing_source_ids":
                rule.get("replacement_or_absorbing_source_ids", []),
            "approved_delta_categories":
                rule.get("approved_delta_categories", []),
            "evidence_refs": rule.get("evidence_refs", []),
        })
    result = {
        "schema": _STATEMENT_SCHEMA,
        "complete": True,
        "data_gaps": [],
        "legacy_sql": {
            "git_revision": _REVISION,
            "repository_path": _LEGACY_PATH,
            "sha256": _LEGACY_SHA256,
            "line_count": 3502,
        },
        "counts": {
            "preamble": 1,
            "statements": 501,
            "total": 502,
            "target_physical_statements":
                len(manifest_index["physical_ids"]),
            "statement_target_links": sum(
                len(physical_ids)
                for _, physical_ids in targets.values()
            ),
            "by_disposition": {
                name: sum(row["disposition"] == name for row in rows)
                for name in sorted(_DISPOSITIONS)
            },
        },
        "rows": rows,
    }
    _validate_statement_ledger(
        result, blob, manifest_index, targets)
    return result


def _validate_statement_ledger(
    ledger: Mapping[str, Any],
    blob: bytes,
    manifest_index: Mapping[str, Any] | None = None,
    expected_targets: Mapping[
        str, tuple[Sequence[str], Sequence[str]],
    ] | None = None,
) -> None:
    if ledger.get("schema") != _STATEMENT_SCHEMA:
        raise LedgerError("unsupported statement ledger schema")
    if ledger.get("complete") is not True or ledger.get("data_gaps") != []:
        raise LedgerError(
            "statement ledger must be complete and contain no data gaps")
    rows = ledger.get("rows")
    if not isinstance(rows, list) or len(rows) != 502:
        raise LedgerError("statement ledger must contain exactly 502 rows")
    expected_counts = {
        "preamble": 1,
        "statements": 501,
        "total": 502,
        "target_physical_statements": 487,
        "statement_target_links": 495,
        "by_disposition": {
            "fold": 4,
            "remove": 12,
            "replace": 7,
            "retain": 479,
        },
    }
    if ledger.get("counts") != expected_counts:
        raise LedgerError(
            f"statement ledger exact counts differ: {ledger.get('counts')!r}")
    expected_ids = ["P000"] + [f"S{value:03d}" for value in range(1, 502)]
    ids = [row.get("source_id") if isinstance(row, dict) else None for row in rows]
    if ids != expected_ids:
        raise LedgerError("statement ledger source IDs are not exact and ordered")
    _, parsed = _split_legacy(blob)
    row_by_id = {
        str(row.get("source_id")): row
        for row in rows if isinstance(row, dict)
    }
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise LedgerError("statement ledger row is not an object")
        disposition = row.get("disposition")
        if disposition not in _DISPOSITIONS:
            raise LedgerError(f"{row.get('source_id')} has invalid disposition")
        if not isinstance(row.get("reason"), str) or not row["reason"]:
            raise LedgerError(f"{row['source_id']} has no disposition reason")
        targets = row.get("semantic_target_identities")
        physical_ids = row.get("target_physical_ids")
        if not isinstance(targets, list) or not all(
                isinstance(value, str) and value for value in targets):
            raise LedgerError(
                f"{row['source_id']} has invalid semantic targets")
        if not isinstance(physical_ids, list) or not all(
                isinstance(value, str) and value for value in physical_ids):
            raise LedgerError(
                f"{row['source_id']} has invalid physical targets")
        if len(targets) != len(set(targets)):
            raise LedgerError(
                f"{row['source_id']} repeats a semantic target")
        if len(physical_ids) != len(set(physical_ids)):
            raise LedgerError(
                f"{row['source_id']} repeats a physical target")
        if disposition != "remove" and (not targets or not physical_ids):
            raise LedgerError(f"{row['source_id']} has no semantic target")
        if disposition == "remove":
            if targets or physical_ids:
                raise LedgerError(
                    f"{row['source_id']} removal has generated targets")
            categories = row.get("approved_delta_categories")
            evidence = row.get("evidence_refs")
            if not isinstance(categories, list) or not categories:
                raise LedgerError(f"{row['source_id']} removal has no category")
            if not isinstance(evidence, list) or not evidence:
                raise LedgerError(f"{row['source_id']} removal has no evidence")
        replacement_ids = row.get("replacement_or_absorbing_source_ids")
        if not isinstance(replacement_ids, list) or not all(
                isinstance(value, str) and value in row_by_id
                for value in replacement_ids):
            raise LedgerError(
                f"{row['source_id']} has an invalid replacement/absorber ID")
        if index == 0:
            continue
        source = parsed[index - 1]
        if row.get("normalized_sha256") != source["normalized_sha256"]:
            raise LedgerError(f"{row['source_id']} normalized hash drift")
        if row.get("byte_span") != {
            "begin": source["byte_begin"], "end": source["byte_end"],
        }:
            raise LedgerError(f"{row['source_id']} byte span drift")
        if row.get("line_span") != {
            "begin": source["line_begin"], "end": source["line_end"],
        }:
            raise LedgerError(f"{row['source_id']} line span drift")
        if row.get("statement_kind") != source["statement_kind"]:
            raise LedgerError(f"{row['source_id']} statement kind drift")
        if expected_targets is not None:
            expected = expected_targets.get(str(row["source_id"]))
            if expected is None:
                raise LedgerError(
                    f"{row['source_id']} has no derived target contract")
            expected_identities, expected_physical_ids = expected
            if targets != list(expected_identities):
                raise LedgerError(
                    f"{row['source_id']} semantic target identity drift")
            if physical_ids != list(expected_physical_ids):
                raise LedgerError(
                    f"{row['source_id']} target physical ID drift")
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(source_id: str) -> None:
        if source_id in visiting:
            raise LedgerError(
                f"statement replacement/absorption cycle reaches {source_id}")
        if source_id in visited:
            return
        visiting.add(source_id)
        for target_id in row_by_id[source_id].get(
                "replacement_or_absorbing_source_ids", []):
            visit(str(target_id))
        visiting.remove(source_id)
        visited.add(source_id)

    for source_id in expected_ids:
        visit(source_id)

    if manifest_index is None:
        return
    by_identity = manifest_index.get("by_identity")
    by_physical_id = manifest_index.get("by_physical_id")
    expected_physical_ids = manifest_index.get("physical_ids")
    if (
        not isinstance(by_identity, dict)
        or not isinstance(by_physical_id, dict)
        or not isinstance(expected_physical_ids, list)
    ):
        raise LedgerError("candidate statement manifest index is invalid")
    joined_physical_ids: set[str] = set()
    for row in rows[1:]:
        if not isinstance(row, dict):
            raise LedgerError("statement ledger row is not an object")
        identities = row["semantic_target_identities"]
        physical_ids = row["target_physical_ids"]
        for identity in identities:
            if identity not in by_identity:
                raise LedgerError(
                    f"{row['source_id']} has unknown target identity {identity}")
        represented_identities: set[str] = set()
        last_ordinal = 0
        for physical_id in physical_ids:
            target = by_physical_id.get(physical_id)
            if not isinstance(target, dict):
                raise LedgerError(
                    f"{row['source_id']} has unknown target physical ID "
                    f"{physical_id}")
            owner_identity = target.get("owner_identity")
            if owner_identity not in identities:
                raise LedgerError(
                    f"{row['source_id']} physical target {physical_id} owner "
                    "is absent from semantic targets")
            represented_identities.add(str(owner_identity))
            ordinal = target.get("baseline_ordinal")
            if not isinstance(ordinal, int) or ordinal <= last_ordinal:
                raise LedgerError(
                    f"{row['source_id']} target physical IDs are not ordered")
            last_ordinal = ordinal
            joined_physical_ids.add(physical_id)
        if represented_identities != set(identities):
            raise LedgerError(
                f"{row['source_id']} semantic targets lack exact physical "
                "representation")
    if joined_physical_ids != set(expected_physical_ids):
        raise LedgerError(
            "statement ledger target physical IDs do not close over manifest")


def _routine_head(normalized: str) -> tuple[str, str, list[str]] | None:
    match = re.match(
        r"create(?: or replace)? (function|procedure)\s+([^\s(]+)\s*\(",
        normalized,
    )
    if match is None:
        return None
    routine_kind = match.group(1)
    name = match.group(2).replace('"', "")
    begin = match.end()
    depth = 1
    single = double = False
    cursor = begin
    while cursor < len(normalized):
        value = normalized[cursor]
        next_value = normalized[cursor + 1] if cursor + 1 < len(normalized) else ""
        if single:
            if value == "'" and next_value == "'":
                cursor += 2
                continue
            if value == "'":
                single = False
        elif double:
            if value == '"' and next_value == '"':
                cursor += 2
                continue
            if value == '"':
                double = False
        elif value == "'":
            single = True
        elif value == '"':
            double = True
        elif value == "(":
            depth += 1
        elif value == ")":
            depth -= 1
            if depth == 0:
                arguments_text = normalized[begin:cursor]
                return routine_kind, name, _argument_types(arguments_text)
        cursor += 1
    return None


def _split_commas(text: str) -> list[str]:
    parts: list[str] = []
    begin = 0
    depth = 0
    single = double = False
    for index, value in enumerate(text):
        if single:
            if value == "'" and (index + 1 >= len(text) or text[index + 1] != "'"):
                single = False
            continue
        if double:
            if value == '"':
                double = False
            continue
        if value == "'":
            single = True
        elif value == '"':
            double = True
        elif value in "([":
            depth += 1
        elif value in ")]":
            depth -= 1
        elif value == "," and depth == 0:
            parts.append(text[begin:index].strip())
            begin = index + 1
    tail = text[begin:].strip()
    if tail:
        parts.append(tail)
    return parts


_TYPE_ALIASES = {
    "int": "integer",
    "int4": "integer",
    "int8": "bigint",
    "int2": "smallint",
    "float8": "double precision",
    "float4": "real",
    "bool": "boolean",
    "varchar": "character varying",
    "bpchar": "character",
}


def _canonical_type(type_name: str) -> str:
    value = type_name.strip().replace('"', "")
    if value.startswith("$extension."):
        value = value.removeprefix("$extension.")
    aliases = {
        **_TYPE_ALIASES,
        "char": "character",
    }
    return aliases.get(value, value)


def _canonical_identity(value: str) -> tuple[str, str, tuple[str, ...]]:
    value = value.removeprefix("sql:").replace("sql_function:", "function:", 1)
    match = re.fullmatch(r"([^:]+):(.+)\((.*)\)", value)
    if match is None:
        raise LedgerError(f"cannot parse routine identity {value!r}")
    kind, name, arguments = match.groups()
    return (
        kind,
        name.replace('"', ""),
        tuple(_canonical_type(item) for item in _split_commas(arguments)),
    )


def _sql_name_leaf(value: str) -> str:
    return value.replace('"', "").split(".")[-1].rstrip(";")


def _routine_identity_key(
    value: str,
) -> tuple[str, str, tuple[str, ...]]:
    kind, name, arguments = _canonical_identity(value)
    if kind == "sql_function":
        kind = "function"
    return kind, name, arguments


def _routine_identity_matches(
    left: str,
    right: str,
    *,
    allow_leaf_name: bool,
) -> bool:
    left_kind, left_name, left_arguments = _routine_identity_key(left)
    right_kind, right_name, right_arguments = _routine_identity_key(right)
    if left_kind != right_kind or left_arguments != right_arguments:
        return False
    if left_name == right_name:
        return True
    return (
        allow_leaf_name
        and _sql_name_leaf(left_name) == _sql_name_leaf(right_name)
    )


def _legacy_routine_identity(
    normalized: str,
) -> str:
    head = _routine_head(normalized)
    if head is None:
        raise LedgerError("cannot derive legacy routine identity")
    routine_kind, name, arguments = head
    return f"{routine_kind}:{name}({','.join(arguments)})"


def _manifest_entity(
    index: Mapping[str, Any],
    identity: str,
    label: str,
) -> Mapping[str, Any]:
    by_identity = index.get("by_identity")
    if not isinstance(by_identity, dict):
        raise LedgerError("candidate statement manifest index is invalid")
    entity = by_identity.get(identity)
    if not isinstance(entity, dict):
        raise LedgerError(f"{label} has no manifest entity {identity}")
    return entity


def _unique_manifest_entity(
    index: Mapping[str, Any],
    candidates: Iterable[Mapping[str, Any]],
    label: str,
) -> Mapping[str, Any]:
    selected = list(candidates)
    if len(selected) != 1:
        identities = sorted(
            str(entity.get("identity")) for entity in selected)
        raise LedgerError(
            f"{label} resolves to manifest identities {identities}, "
            "expected exactly one")
    identity = selected[0].get("identity")
    if not isinstance(identity, str):
        raise LedgerError(f"{label} selected an entity without identity")
    return _manifest_entity(index, identity, label)


def _entities_of_kind(
    index: Mapping[str, Any],
    kinds: str | Iterable[str],
) -> list[Mapping[str, Any]]:
    wanted = {kinds} if isinstance(kinds, str) else set(kinds)
    entities = index.get("entities")
    if not isinstance(entities, list):
        raise LedgerError("candidate statement manifest index has no entities")
    return [
        entity for entity in entities
        if isinstance(entity, dict) and entity.get("kind") in wanted
    ]


def _entity_by_kind_and_leaf(
    index: Mapping[str, Any],
    kinds: str | Iterable[str],
    sql_name: str,
    label: str,
) -> Mapping[str, Any]:
    normalized_name = sql_name.replace('"', "").rstrip(";")
    leaf = _sql_name_leaf(sql_name)
    candidates = _entities_of_kind(index, kinds)
    exact = [
        entity for entity in candidates
        if entity.get("name") == normalized_name
    ]
    return _unique_manifest_entity(
        index,
        (
            entity for entity in candidates
            if entity.get("name") == leaf
        ) if not exact else exact,
        label,
    )


def _candidate_routine_entity(
    index: Mapping[str, Any],
    legacy_identity: str,
    delta: Mapping[str, Any],
    label: str,
) -> Mapping[str, Any]:
    renames = delta.get("identity_renames")
    if not isinstance(renames, list):
        raise LedgerError("approved delta has no routine identity renames")
    renamed_candidates: list[str] = []
    for row in renames:
        if not isinstance(row, dict):
            raise LedgerError("approved identity rename is not an object")
        legacy = row.get("legacy")
        candidate = row.get("candidate")
        if not isinstance(legacy, str) or not isinstance(candidate, str):
            raise LedgerError("approved identity rename is incomplete")
        if _routine_identity_matches(
                legacy_identity, legacy, allow_leaf_name=True):
            renamed_candidates.append(candidate)
    if renamed_candidates:
        if len(renamed_candidates) != 1:
            raise LedgerError(
                f"{label} matches duplicate approved identity renames")
        candidate_identity = renamed_candidates[0]
        return _unique_manifest_entity(
            index,
            (
                entity for entity in _entities_of_kind(
                    index, ("function", "sql_function", "procedure"))
                if isinstance(entity.get("identity"), str)
                and _routine_identity_matches(
                    candidate_identity,
                    str(entity["identity"]),
                    allow_leaf_name=False,
                )
            ),
            label,
        )

    legacy_kind, legacy_name, legacy_arguments = _routine_identity_key(
        legacy_identity)
    candidates: list[Mapping[str, Any]] = []
    exact: list[Mapping[str, Any]] = []
    for entity in _entities_of_kind(
            index, ("function", "sql_function", "procedure")):
        identity = entity.get("identity")
        if not isinstance(identity, str):
            continue
        try:
            kind, name, arguments = _routine_identity_key(identity)
        except LedgerError:
            continue
        if kind != legacy_kind or arguments != legacy_arguments:
            continue
        if name == legacy_name:
            exact.append(entity)
        elif _sql_name_leaf(name) == _sql_name_leaf(legacy_name):
            candidates.append(entity)
    return _unique_manifest_entity(
        index, exact or candidates, label)


def _physical_target(
    index: Mapping[str, Any],
    entity: Mapping[str, Any],
    facet: str,
    label: str,
) -> Mapping[str, Any]:
    identity = entity.get("identity")
    by_owner_facet = index.get("by_owner_facet")
    if not isinstance(identity, str) or not isinstance(by_owner_facet, dict):
        raise LedgerError(f"{label} cannot resolve a physical target")
    target = by_owner_facet.get((identity, facet))
    if not isinstance(target, dict):
        raise LedgerError(
            f"{label} has no physical {facet} target for {identity}")
    return target


def _balanced_signature(
    text: str,
    prefix_pattern: str,
    label: str,
) -> tuple[str, str, int]:
    match = re.match(prefix_pattern + r"\s+([^\s(]+)\s*\(", text)
    if match is None:
        raise LedgerError(f"{label} has no parseable signature")
    name = match.group(1).replace('"', "")
    begin = match.end()
    depth = 1
    single = double = False
    cursor = begin
    while cursor < len(text):
        value = text[cursor]
        next_value = text[cursor + 1] if cursor + 1 < len(text) else ""
        if single:
            if value == "'" and next_value == "'":
                cursor += 2
                continue
            if value == "'":
                single = False
        elif double:
            if value == '"' and next_value == '"':
                cursor += 2
                continue
            if value == '"':
                double = False
        elif value == "'":
            single = True
        elif value == '"':
            double = True
        elif value == "(":
            depth += 1
        elif value == ")":
            depth -= 1
            if depth == 0:
                return name, text[begin:cursor], cursor + 1
        cursor += 1
    raise LedgerError(f"{label} signature is unterminated")


def _manifest_relation_routine(
    entity: Mapping[str, Any],
) -> list[str]:
    relationships = entity.get("relationships")
    if not isinstance(relationships, list):
        return []
    return [
        value for value in relationships
        if isinstance(value, str)
        and (
            value.startswith("sql:function:")
            or value.startswith("sql:sql_function:")
            or value.startswith("sql:procedure:")
        )
    ]


def _entity_by_routine_relationship(
    index: Mapping[str, Any],
    entity_kind: str,
    legacy_routine_identity: str,
    delta: Mapping[str, Any],
    label: str,
    *,
    expected_name: str | None = None,
) -> Mapping[str, Any]:
    target_routine = _candidate_routine_entity(
        index, legacy_routine_identity, delta, label)
    target_identity = target_routine.get("identity")
    return _unique_manifest_entity(
        index,
        (
            entity for entity in _entities_of_kind(index, entity_kind)
            if (
                expected_name is None
                or entity.get("name") == expected_name
            )
            and target_identity in _manifest_relation_routine(entity)
        ),
        label,
    )


def _created_object_name(
    normalized: str,
    source_kind: str,
    label: str,
) -> str:
    patterns = {
        "create_schema": r"create\s+schema(?:\s+if\s+not\s+exists)?\s+([^\s;]+)",
        "create_sequence":
            r"create\s+sequence(?:\s+if\s+not\s+exists)?\s+([^\s;(]+)",
        "create_table":
            r"create(?:\s+unlogged|\s+temporary)?\s+table"
            r"(?:\s+if\s+not\s+exists)?\s+([^\s;(]+)",
        "create_index":
            r"create(?:\s+unique)?\s+index"
            r"(?:\s+if\s+not\s+exists)?\s+([^\s;(]+)",
        "create_view":
            r"create(?:\s+or\s+replace)?\s+view\s+([^\s;(]+)",
        "create_type": r"create\s+type\s+([^\s;(]+)",
        "create_operator_class":
            r"create\s+operator\s+class\s+([^\s;(]+)",
    }
    pattern = patterns.get(source_kind)
    if pattern is None:
        raise LedgerError(f"{label} has unsupported create kind {source_kind}")
    match = re.match(pattern, normalized)
    if match is None:
        raise LedgerError(f"{label} has no parseable object name")
    return match.group(1).replace('"', "")


def _aggregate_entity(
    index: Mapping[str, Any],
    normalized: str,
    label: str,
) -> Mapping[str, Any]:
    name, arguments_text, _ = _balanced_signature(
        normalized, r"create\s+aggregate", label)
    arguments = tuple(
        _canonical_type(value)
        for value in _split_commas(arguments_text)
    )
    if arguments_text.strip() == "*":
        arguments = ("*",)
    exact: list[Mapping[str, Any]] = []
    candidates: list[Mapping[str, Any]] = []
    for entity in _entities_of_kind(index, "aggregate"):
        identity = entity.get("identity")
        if not isinstance(identity, str):
            continue
        kind, candidate_name, candidate_arguments = _routine_identity_key(
            identity)
        if kind != "aggregate" or candidate_arguments != arguments:
            continue
        if candidate_name == name:
            exact.append(entity)
        elif _sql_name_leaf(candidate_name) == _sql_name_leaf(name):
            candidates.append(entity)
    return _unique_manifest_entity(
        index, exact or candidates, label)


def _operator_entity(
    index: Mapping[str, Any],
    normalized: str,
    delta: Mapping[str, Any],
    label: str,
) -> Mapping[str, Any]:
    operator_name, body, _ = _balanced_signature(
        normalized, r"create\s+operator", label)
    implementation = re.search(
        r"\b(?:function|procedure)\s*=\s*([^\s,)]+)", body)
    if implementation is None:
        raise LedgerError(f"{label} operator has no implementation routine")
    implementation_name = implementation.group(1).replace('"', "")
    arguments: list[str] = []
    for key in ("leftarg", "rightarg"):
        match = re.search(rf"\b{key}\s*=\s*([^\s,)]+)", body)
        if match is not None:
            arguments.append(_canonical_type(match.group(1)))
    legacy_routine_identity = (
        f"function:{implementation_name}({','.join(arguments)})")
    return _entity_by_routine_relationship(
        index,
        "operator",
        legacy_routine_identity,
        delta,
        label,
        expected_name=_sql_name_leaf(operator_name),
    )


def _cast_entity(
    index: Mapping[str, Any],
    normalized: str,
    delta: Mapping[str, Any],
    label: str,
) -> Mapping[str, Any]:
    conversion = re.search(
        r"\bwith\s+function\s+([^\s(]+)\s*\(([^)]*)\)", normalized)
    if conversion is None:
        raise LedgerError(f"{label} cast has no conversion function")
    name = conversion.group(1).replace('"', "")
    arguments = _argument_types(conversion.group(2))
    return _entity_by_routine_relationship(
        index,
        "cast",
        f"function:{name}({','.join(arguments)})",
        delta,
        label,
    )


def _comment_target_entity(
    index: Mapping[str, Any],
    normalized: str,
    delta: Mapping[str, Any],
    label: str,
) -> Mapping[str, Any]:
    prefix = re.match(r"comment\s+on\s+(\w+)\s+", normalized)
    if prefix is None:
        raise LedgerError(f"{label} comment target is invalid")
    target_kind = prefix.group(1)
    if target_kind == "function":
        name, arguments_text, _ = _balanced_signature(
            normalized, r"comment\s+on\s+function", label)
        arguments = _argument_types(arguments_text)
        return _candidate_routine_entity(
            index,
            f"function:{name}({','.join(arguments)})",
            delta,
            label,
        )
    object_match = re.match(
        rf"comment\s+on\s+{re.escape(target_kind)}\s+([^\s]+)\s+is\b",
        normalized,
    )
    if object_match is None:
        raise LedgerError(f"{label} comment object name is invalid")
    kind_map = {
        "table": "table",
        "view": "view",
        "sequence": "sequence",
        "type": "type",
    }
    manifest_kind = kind_map.get(target_kind)
    if manifest_kind is None:
        raise LedgerError(
            f"{label} comment kind {target_kind!r} is unsupported")
    return _entity_by_kind_and_leaf(
        index, manifest_kind, object_match.group(1), label)


def _acl_target_entity(
    index: Mapping[str, Any],
    normalized: str,
    delta: Mapping[str, Any],
    label: str,
) -> tuple[Mapping[str, Any], str, bool]:
    match = re.match(
        r"(grant|revoke)\s+(.+?)\s+on\s+(.+?)\s+(to|from)\s+public\s*;",
        normalized,
    )
    if match is None:
        raise LedgerError(f"{label} ACL statement is invalid")
    _, privilege_text, target_text, _ = match.groups()
    privilege = (
        "all"
        if privilege_text in {"all", "all privileges"}
        else privilege_text
    )
    if target_text.startswith("all tables in schema "):
        schema_name = target_text.removeprefix(
            "all tables in schema ").replace('"', "")
        entity = _entity_by_kind_and_leaf(
            index, "schema", schema_name, label)
        return entity, privilege, True
    routine_kind_match = re.match(r"(function|procedure)\s+", target_text)
    if routine_kind_match is not None:
        routine_kind = routine_kind_match.group(1)
        name, arguments_text, _ = _balanced_signature(
            target_text, routine_kind, label)
        arguments = _argument_types(arguments_text)
        entity = _candidate_routine_entity(
            index,
            f"{routine_kind}:{name}({','.join(arguments)})",
            delta,
            label,
        )
        return entity, privilege, False
    object_match = re.match(
        r"(?:(schema|sequence|table)\s+)?([^\s;]+)", target_text)
    if object_match is None:
        raise LedgerError(f"{label} ACL target is invalid")
    explicit_kind, sql_name = object_match.groups()
    if explicit_kind is not None:
        entity = _entity_by_kind_and_leaf(
            index, explicit_kind, sql_name, label)
        return entity, privilege, False

    leaf = _sql_name_leaf(sql_name)
    qualifier = (
        sql_name.replace('"', "").rsplit(".", 1)[0]
        if "." in sql_name.replace('"', "") else None
    )
    candidates = [
        entity for entity in _entities_of_kind(index, ("table", "view"))
        if entity.get("name") == leaf
    ]
    exact = [
        entity for entity in candidates
        if qualifier is not None and entity.get("schema") == qualifier
    ]
    return _unique_manifest_entity(
        index, exact or candidates, label), privilege, False


def _acl_entity(
    index: Mapping[str, Any],
    normalized: str,
    delta: Mapping[str, Any],
    label: str,
) -> Mapping[str, Any]:
    acl_kind = "grant" if normalized.startswith("grant ") else "revoke"
    target, privilege, all_tables = _acl_target_entity(
        index, normalized, delta, label)
    target_identity = target.get("identity")
    compatible_privileges = {privilege}
    if (
        privilege == "all"
        and target.get("kind") in {"function", "sql_function", "procedure"}
    ):
        compatible_privileges.add("execute")
    candidates: list[Mapping[str, Any]] = []
    for entity in _entities_of_kind(index, acl_kind):
        relationships = entity.get("relationships")
        options = entity.get("options")
        if (
            not isinstance(relationships, list)
            or target_identity not in relationships
        ):
            continue
        identity = str(entity.get("identity", ""))
        identity_parts = identity.rsplit(":", 3)
        candidate_privilege = (
            str(options.get("privileges", "")).split("::")[-1]
            if isinstance(options, dict) and options.get("privileges")
            else (
                identity_parts[-3]
                if len(identity_parts) == 4 else ""
            )
        )
        candidate_all_tables = (
            (
                isinstance(options, dict)
                and options.get("scope")
                    == "bioseq::sql::acl_scope::all_tables_in_schema"
            )
            or identity.startswith(
                f"sql:{acl_kind}:all tables in schema:")
        )
        if (
            candidate_privilege in compatible_privileges
            and candidate_all_tables == all_tables
        ):
            candidates.append(entity)
    return _unique_manifest_entity(index, candidates, label)


def _config_dump_entity(
    index: Mapping[str, Any],
    normalized: str,
    label: str,
) -> Mapping[str, Any]:
    match = re.match(
        r"select\s+(?:pg_catalog\.)?pg_extension_config_dump\s*\(\s*"
        r"'([^']+)'",
        normalized,
    )
    if match is None:
        raise LedgerError(f"{label} config-dump target is invalid")
    return _entity_by_kind_and_leaf(
        index, "config_dump", match.group(1), label)


def _natural_statement_entity_and_facet(
    index: Mapping[str, Any],
    statement: Mapping[str, Any],
    delta: Mapping[str, Any],
) -> tuple[Mapping[str, Any], str]:
    source_id = str(statement["source_id"])
    normalized = str(statement["normalized"])
    source_kind = str(statement["statement_kind"])
    label = f"legacy {source_id}"
    if source_kind in _ROUTINE_KINDS:
        return (
            _candidate_routine_entity(
                index, _legacy_routine_identity(normalized), delta, label),
            "primary",
        )
    if source_kind == "comment":
        return _comment_target_entity(
            index, normalized, delta, label), "catalog_comment"
    if source_kind in {"grant", "revoke"}:
        return _acl_entity(index, normalized, delta, label), "primary"
    if source_kind == "select":
        return _config_dump_entity(index, normalized, label), "primary"
    if source_kind == "set":
        name_match = re.match(r"set\s+([^\s=]+)", normalized)
        if name_match is None:
            raise LedgerError(f"{label} session setting is invalid")
        return _entity_by_kind_and_leaf(
            index, "session_setting", name_match.group(1), label), "primary"
    if source_kind == "alter":
        constraint = re.match(
            r"alter\s+table\s+[^\s]+\s+add\s+constraint\s+([^\s]+)\s+"
            r"(unique|primary\s+key)\b",
            normalized,
        )
        if constraint is None:
            raise LedgerError(f"{label} ALTER has no natural target")
        constraint_kind = (
            "unique" if constraint.group(2) == "unique" else "primary_key")
        return _entity_by_kind_and_leaf(
            index, constraint_kind, constraint.group(1), label), "primary"
    if source_kind == "create_aggregate":
        return _aggregate_entity(index, normalized, label), "primary"
    if source_kind == "create_operator":
        return _operator_entity(index, normalized, delta, label), "primary"
    if source_kind == "create_cast":
        return _cast_entity(index, normalized, delta, label), "primary"
    if source_kind == "create_type":
        sql_name = _created_object_name(normalized, source_kind, label)
        if re.match(r"create\s+type\s+[^\s;(]+\s*;", normalized):
            facet = "base_type_shell"
        elif re.match(r"create\s+type\s+[^\s;(]+\s*\(", normalized):
            facet = "base_type_definition"
        else:
            facet = "primary"
        return _entity_by_kind_and_leaf(
            index, "type", sql_name, label), facet
    kind_map = {
        "create_schema": "schema",
        "create_sequence": "sequence",
        "create_table": "table",
        "create_index": "index",
        "create_view": "view",
        "create_operator_class": "opclass",
    }
    manifest_kind = kind_map.get(source_kind)
    if manifest_kind is not None:
        sql_name = _created_object_name(normalized, source_kind, label)
        return _entity_by_kind_and_leaf(
            index, manifest_kind, sql_name, label), "primary"
    raise LedgerError(
        f"{label} kind {source_kind!r} has no semantic target derivation")


def _target_rows(
    index: Mapping[str, Any],
    identities_and_facets: Sequence[tuple[str, str]],
    label: str,
) -> tuple[list[str], list[str]]:
    physical: list[Mapping[str, Any]] = []
    for identity, facet in identities_and_facets:
        entity = _manifest_entity(index, identity, label)
        physical.append(_physical_target(index, entity, facet, label))
    physical.sort(key=lambda row: int(row["baseline_ordinal"]))
    identities: list[str] = []
    seen_identities: set[str] = set()
    physical_ids: list[str] = []
    for row in physical:
        identity = str(row["owner_identity"])
        if identity not in seen_identities:
            seen_identities.add(identity)
            identities.append(identity)
        physical_ids.append(str(row["baseline_physical_id"]))
    return identities, physical_ids


def _derive_statement_targets(
    statements: Sequence[Mapping[str, Any]],
    compiled_rules: Mapping[str, Mapping[str, Any]],
    manifest: Mapping[str, Any] | None,
    delta: Mapping[str, Any],
) -> tuple[dict[str, tuple[list[str], list[str]]], Mapping[str, Any]]:
    index = _statement_manifest_index(manifest)
    exact_fold = {"S002", "S008", "S024", "S038"}
    exact_replace = {"S019", "S025", "S027", "S075", "S077", "S083", "S085"}
    exact_remove = {
        "S003", "S020", "S021", "S022", "S023", "S061",
        "S062", "S063", "S084", "S377", "S378", "S396",
    }
    for disposition, expected in (
        ("fold", exact_fold),
        ("replace", exact_replace),
        ("remove", exact_remove),
    ):
        actual = {
            source_id for source_id, rule in compiled_rules.items()
            if source_id != "P000" and rule.get("disposition") == disposition
        }
        if actual != expected:
            raise LedgerError(
                f"closed {disposition} source set differs; "
                f"expected={sorted(expected)}, actual={sorted(actual)}")
    expected_absorbers = {
        "S008": ["S037"],
        "S019": ["S025", "S026", "S027", "S028"],
        "S024": ["S037"],
    }
    for source_id, rule in compiled_rules.items():
        actual_absorbers = rule.get(
            "replacement_or_absorbing_source_ids", [])
        if actual_absorbers != expected_absorbers.get(source_id, []):
            raise LedgerError(
                f"{source_id} replacement/absorbing mapping differs; "
                f"expected={expected_absorbers.get(source_id, [])}, "
                f"actual={actual_absorbers}")

    special: dict[str, Sequence[tuple[str, str]]] = {
        "S002": (("sql:schema:bioseq_internal", "primary"),),
        "S038": (("sql:schema:bioseq_internal", "primary"),),
        "S008": ((
            "sql:revoke:all tables in schema:bioseq_internal:"
            "all:public:ordinary",
            "primary",
        ),),
        "S024": ((
            "sql:revoke:all tables in schema:bioseq_internal:"
            "all:public:ordinary",
            "primary",
        ),),
        "S037": ((
            "sql:revoke:all tables in schema:bioseq_internal:"
            "all:public:ordinary",
            "primary",
        ),),
        "S019": (
            ("sql:function:bioseq.bioseq_handler(internal)", "primary"),
            ("sql:function:bioseq.bioseq_handler(internal)", "catalog_comment"),
            (
                "sql:access_method:"
                "$extension.bioseq::table::sql_routines.bioseq",
                "primary",
            ),
            (
                "sql:function:pg_catalog.alter_bioseq_table_set("
                "regclass,integer,integer,name,integer)",
                "primary",
            ),
            (
                "sql:function:pg_catalog.alter_bioseq_table_set("
                "regclass,integer,integer,name,integer)",
                "catalog_comment",
            ),
            (
                "sql:function:pg_catalog.alter_bioseq_table_reset("
                "regclass,boolean,boolean,boolean,boolean)",
                "primary",
            ),
            (
                "sql:function:pg_catalog.alter_bioseq_table_reset("
                "regclass,boolean,boolean,boolean,boolean)",
                "catalog_comment",
            ),
        ),
        "S083": ((
            "sql:catalog_dependencies:"
            "$extension.bioseq::table::sql_routines.bioseq_handler",
            "primary",
        ),),
        "S085": ((
            "sql:catalog_dependencies:"
            "$extension.bioseq::table::sql_routines.bioseq_handler",
            "primary",
        ),),
    }
    result: dict[str, tuple[list[str], list[str]]] = {}
    for statement in statements:
        source_id = str(statement["source_id"])
        rule = compiled_rules[source_id]
        if rule["disposition"] == "remove":
            result[source_id] = ([], [])
            continue
        explicit = special.get(source_id)
        if explicit is not None:
            result[source_id] = _target_rows(index, explicit, source_id)
            continue
        entity, facet = _natural_statement_entity_and_facet(
            index, statement, delta)
        result[source_id] = _target_rows(
            index, ((str(entity["identity"]), facet),), source_id)

    physical_union = {
        physical_id
        for _, physical_ids in result.values()
        for physical_id in physical_ids
    }
    expected_physical = set(index["physical_ids"])
    if physical_union != expected_physical:
        raise LedgerError(
            "statement source/target join does not close over the candidate "
            f"manifest; missing={sorted(expected_physical - physical_union)}, "
            f"extra={sorted(physical_union - expected_physical)}")
    coverage = {
        physical_id: sum(
            physical_id in physical_ids
            for _, physical_ids in result.values()
        )
        for physical_id in index["physical_ids"]
    }
    repeated_coverage = {
        physical_id: count
        for physical_id, count in coverage.items()
        if count != 1
    }
    expected_repeated_coverage = {
        "P002": 2,
        "P022": 2,
        "P023": 2,
        "P024": 2,
        "P025": 2,
        "P034": 3,
        "P074": 2,
    }
    if repeated_coverage != expected_repeated_coverage:
        raise LedgerError(
            "statement source/target join overlap closure differs; "
            f"expected={expected_repeated_coverage}, "
            f"actual={repeated_coverage}")
    if result["S002"] != result["S038"]:
        raise LedgerError("fixed-schema fold target is not many-to-one")
    if not (
        result["S008"] == result["S024"] == result["S037"]
    ):
        raise LedgerError("intermediate ACL fold target is not many-to-one")
    if result["S083"] != result["S085"]:
        raise LedgerError("AM dependency replacement is not many-to-one")
    absorbed = set()
    for source_id in ("S025", "S026", "S027", "S028"):
        absorbed.update(result[source_id][1])
    if not absorbed.issubset(set(result["S019"][1])):
        raise LedgerError(
            "S019 replacement does not include every absorbed alter target")
    for source_id, rule in compiled_rules.items():
        if source_id == "P000":
            continue
        current = set(result[source_id][1])
        for absorber in rule.get(
                "replacement_or_absorbing_source_ids", []):
            if not set(result[str(absorber)][1]).issubset(current):
                raise LedgerError(
                    f"{source_id} does not include absorbing target {absorber}")
    return result, index


def _argument_types(arguments_text: str) -> list[str]:
    result: list[str] = []
    modes = {"in", "out", "inout", "variadic"}
    for raw in _split_commas(arguments_text):
        declaration = re.split(r"\s+(?:default|=)\s*", raw, maxsplit=1)[0]
        tokens = declaration.split()
        if tokens and tokens[0] in modes:
            mode = tokens.pop(0)
            if mode == "out":
                continue
        if len(tokens) >= 2:
            first = tokens[0].strip('"')
            second = tokens[1].strip('"')
            known_types = {
                "bigint", "smallint", "integer", "real", "double",
                "boolean", "text", "name", "oid", "regclass", "record",
                "internal", "cstring", "bytea", "jsonb", "tid", "xid8",
                "anyelement", "anyarray", "void", "character",
            }
            first_type = (
                first in _TYPE_ALIASES
                or first in known_types
                or "." in first
                or first.endswith("[]")
            )
            second_type = (
                second in _TYPE_ALIASES
                or second in known_types
                or "." in second
                or second.endswith("[]")
            )
            compound_type = (
                (first, second) in {
                    ("double", "precision"),
                    ("character", "varying"),
                }
            )
            if (second_type and not compound_type) or not first_type:
                tokens.pop(0)
        type_name = " ".join(tokens)
        type_name = re.sub(r"\([^)]*\)", "", type_name).strip()
        if type_name.startswith("double precision"):
            type_name = "double precision"
        if type_name.startswith("character varying"):
            type_name = "character varying"
        array = type_name.endswith("[]")
        base = type_name[:-2] if array else type_name
        base = _TYPE_ALIASES.get(base, base)
        result.append(base + ("[]" if array else ""))
    return result


def _c_symbol(normalized: str) -> str | None:
    if "language c" not in normalized:
        return None
    two_literal = re.search(
        r"\bas\s+'module_pathname'\s*,\s*'([^']+)'",
        normalized,
        flags=re.IGNORECASE,
    )
    if two_literal is not None:
        return two_literal.group(1)
    dollar_literal = re.search(
        r"\bas\s+'module_pathname'\s*,\s*\$([A-Za-z0-9_]*)\$"
        r"([^$]+)\$\1\$",
        normalized,
        flags=re.IGNORECASE,
    )
    if dollar_literal is not None:
        return dollar_literal.group(2)
    head = _routine_head(normalized)
    return head[1].split(".")[-1] if head is not None else None


def _legacy_c_declarations(
    statements: Sequence[Mapping[str, Any]],
) -> list[dict[str, Any]]:
    declarations: list[dict[str, Any]] = []

    def append_declaration(
        source: Mapping[str, Any],
        normalized: str,
        nested: int | None,
        nested_span: tuple[int, int] | None = None,
    ) -> None:
        head = _routine_head(normalized)
        symbol = _c_symbol(normalized)
        if head is None or symbol is None:
            raise LedgerError(
                f"cannot parse legacy C declaration in {source['source_id']}")
        routine_kind, name, arguments = head
        if nested_span is None:
            source_span = {
                "byte_begin": source["byte_begin"],
                "byte_end": source["byte_end"],
                "line_begin": source["line_begin"],
                "line_end": source["line_end"],
            }
        else:
            exact = str(source["exact"])
            nested_begin, nested_end = nested_span
            source_span = {
                "byte_begin": (
                    int(source["byte_begin"])
                    + len(exact[:nested_begin].encode("utf-8"))
                ),
                "byte_end": (
                    int(source["byte_begin"])
                    + len(exact[:nested_end].encode("utf-8"))
                ),
                "line_begin": (
                    int(source["line_begin"])
                    + exact[:nested_begin].count("\n")
                ),
                "line_end": (
                    int(source["line_begin"])
                    + exact[:nested_end].count("\n")
                ),
            }
        declarations.append({
            "declaration_id": f"C{len(declarations) + 1:03d}",
            "source_id": source["source_id"],
            "nested_ordinal": nested,
            "source_span": source_span,
            "legacy_sql_sha256": _LEGACY_SHA256,
            "normalized_sha256": _sha256(normalized),
            "routine_identity": (
                f"{routine_kind}:{name}({','.join(arguments)})"),
            "legacy_c_symbol": symbol,
            "disposition": "effective",
        })

    for statement in statements:
        normalized = str(statement["normalized"])
        if statement["statement_kind"] in _ROUTINE_KINDS and "language c" in normalized:
            append_declaration(statement, normalized, None)
        if statement["source_id"] != "S019":
            continue
        exact = str(statement["exact"])
        matches = list(re.finditer(
            r"create(?:\s+or\s+replace)?\s+function\s+.*?"
            r"language\s+c\b.*?;",
            exact,
            flags=re.IGNORECASE | re.DOTALL,
        ))
        if len(matches) != 3:
            raise LedgerError(
                f"S019 contains {len(matches)} nested C declarations, expected 3")
        for nested, match in enumerate(matches, 1):
            append_declaration(
                statement,
                _normalize_sql(match.group(0)),
                nested,
                (match.start(), match.end()),
            )
    if len(declarations) != 268:
        raise LedgerError(
            f"legacy physical C declaration count is {len(declarations)}, "
            "expected 268")
    folded = 0
    for declaration in declarations:
        if declaration["source_id"] == "S019" and any(
            leaf in declaration["routine_identity"]
            for leaf in ("alter_bioseq_table_set", "alter_bioseq_table_reset")
        ):
            declaration["disposition"] = "folded_into_later_owner"
            folded += 1
    if folded != 2:
        raise LedgerError(
            f"S019 folded nested declaration count is {folded}, expected 2")
    return declarations


def _identity_entity(
    identity: str,
    by_identity: Mapping[str, Mapping[str, Any]],
) -> Mapping[str, Any]:
    entity = by_identity.get(identity)
    if entity is None:
        wanted = _canonical_identity(identity)
        matches: list[Mapping[str, Any]] = []
        for candidate_identity, candidate in by_identity.items():
            try:
                candidate_key = _canonical_identity(candidate_identity)
            except LedgerError:
                continue
            if candidate_key == wanted:
                matches.append(candidate)
        if len(matches) == 1:
            entity = matches[0]
    if entity is None:
        raise LedgerError(f"candidate manifest has no entity {identity}")
    return entity


def _source_id_for_identity(
    candidate_identity: str,
    statements: Sequence[Mapping[str, Any]],
    renames: Sequence[object],
) -> str:
    legacy_identity = candidate_identity
    candidate_key = _canonical_identity(candidate_identity)
    for rename in renames:
        if isinstance(rename, dict) and isinstance(rename.get("candidate"), str):
            if _canonical_identity(str(rename["candidate"])) == candidate_key:
                legacy_identity = str(rename.get("legacy"))
                break
    wanted_kind, wanted_name, wanted_arguments = _canonical_identity(legacy_identity)
    matches: list[tuple[str, str]] = []
    for statement in statements:
        head = _routine_head(str(statement["normalized"]))
        if head is None:
            continue
        kind, name, arguments = head
        canonical = (
            kind,
            name.replace('"', ""),
            tuple(_canonical_type(value) for value in arguments),
        )
        if canonical[0] != wanted_kind or canonical[2] != wanted_arguments:
            continue
        if canonical[1] == wanted_name:
            matches.append((str(statement["source_id"]), "exact"))
        elif canonical[1].split(".")[-1] == wanted_name.split(".")[-1]:
            matches.append((str(statement["source_id"]), "leaf"))
    exact = sorted({source_id for source_id, quality in matches if quality == "exact"})
    selected = exact or sorted({source_id for source_id, _ in matches})
    if not selected:
        for declaration in _legacy_c_declarations(statements):
            canonical = _canonical_identity(str(declaration["routine_identity"]))
            if (
                canonical[0] == wanted_kind
                and canonical[2] == wanted_arguments
                and canonical[1].split(".")[-1] == wanted_name.split(".")[-1]
            ):
                selected.append(str(declaration["source_id"]))
        selected = sorted(set(selected))
    if len(selected) != 1:
        raise LedgerError(
            f"{candidate_identity} maps to legacy source IDs {selected}, "
            "expected exactly one")
    return selected[0]


def _source_ids_by_symbol(
    statements: Sequence[Mapping[str, Any]],
) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    declarations = _legacy_c_declarations(statements)
    for row in declarations:
        result.setdefault(str(row["legacy_c_symbol"]), []).append(
            str(row["source_id"]))
    return {key: sorted(set(value)) for key, value in result.items()}


def _candidate_symbol(entity: Mapping[str, Any]) -> str:
    binding = entity.get("c_binding")
    if isinstance(binding, dict) and isinstance(binding.get("symbol"), str):
        return str(binding["symbol"])
    properties = entity.get("routine_properties")
    if isinstance(properties, dict) and isinstance(
            properties.get("c_symbol"), str):
        return str(properties["c_symbol"])
    raise LedgerError(f"manifest entity {entity.get('identity')} has no C symbol")


def _choose_source_id(
    candidates: Sequence[str],
    allowed: set[str],
    label: str,
    statements_by_id: Mapping[str, Mapping[str, Any]] | None = None,
    legacy_identity: str | None = None,
) -> str:
    selected = sorted(set(candidates).intersection(allowed))
    if len(selected) > 1 and statements_by_id is not None and legacy_identity:
        identity_head = legacy_identity.split(":", 1)[-1]
        wanted_name = identity_head.split("(", 1)[0].replace('"', "")
        wanted_leaf = wanted_name.split(".")[-1]
        narrowed: list[str] = []
        for source_id in selected:
            head = _routine_head(str(statements_by_id[source_id]["normalized"]))
            if head is None:
                continue
            actual_name = head[1].replace('"', "")
            if actual_name == wanted_name or actual_name.split(".")[-1] == wanted_leaf:
                narrowed.append(source_id)
        if len(narrowed) == 1:
            selected = narrowed
    if len(selected) != 1:
        raise LedgerError(
            f"{label} resolves to source IDs {selected}, expected exactly one")
    return selected[0]


def _catalog_join(
    delta: Mapping[str, Any],
    manifest: Mapping[str, Any] | None,
    statements: Sequence[Mapping[str, Any]],
    compiled_rules: Mapping[str, Mapping[str, Any]],
) -> dict[str, Any]:
    if manifest is None:
        raise LedgerError(
            "candidate manifest is required for the 118/six/309 join projection")
    by_identity, _ = _manifest_indexes(manifest)
    by_symbol = _source_ids_by_symbol(statements)
    statements_by_id = {
        str(statement["source_id"]): statement for statement in statements
    }
    symbol_rows = delta.get("routine_symbol_changes")
    input_rows = delta.get("input_name_changes")
    renames = delta.get("identity_renames")
    if not isinstance(symbol_rows, list) or len(symbol_rows) != 118:
        raise LedgerError("approved delta must contain exactly 118 symbol rows")
    if not isinstance(input_rows, list) or len(input_rows) != 309:
        raise LedgerError("approved delta must contain exactly 309 input rows")
    if not isinstance(renames, list) or len(renames) < 6:
        raise LedgerError("approved delta must contain the BPCHAR identity repairs")
    symbol_allowed = {
        source_id for source_id, rule in compiled_rules.items()
        if "derived-routine-symbol" in rule.get("approved_delta_categories", [])
    }
    input_allowed = {
        source_id for source_id, rule in compiled_rules.items()
        if "derived-input-name" in rule.get("approved_delta_categories", [])
    }
    bpchar_allowed = {f"S{value:03d}" for value in range(136, 142)}
    joined_symbols: list[dict[str, Any]] = []
    symbol_change_by_identity: dict[str, Mapping[str, Any]] = {}
    for row in symbol_rows:
        if not isinstance(row, dict):
            raise LedgerError("symbol change row is not an object")
        identity = str(row.get("identity"))
        symbol_change_by_identity[identity] = row
        entity = _identity_entity(identity, by_identity)
        if _candidate_symbol(entity) != row.get("candidate"):
            raise LedgerError(f"{identity} candidate symbol/manifest mismatch")
        source_id = _choose_source_id(
            by_symbol.get(str(row.get("legacy")), []),
            symbol_allowed,
            identity,
            statements_by_id,
            identity,
        )
        joined_symbols.append({
            "source_id": source_id,
            "routine_identity": identity,
            "legacy_symbol": row["legacy"],
            "target_cpp_identifier": row["candidate"],
            "target_sql_name": row["candidate"],
        })
    if len({row["source_id"] for row in joined_symbols}) != 118:
        raise LedgerError("118 symbol projection does not map one-to-one to sources")

    bpchar_rows: list[dict[str, Any]] = []
    for row in renames[:6]:
        if not isinstance(row, dict):
            raise LedgerError("BPCHAR rename row is not an object")
        legacy = str(row.get("legacy"))
        candidate = str(row.get("candidate"))
        entity = _identity_entity(candidate, by_identity)
        target = _candidate_symbol(entity)
        legacy_leaf = legacy.split(":", 1)[1].split("(", 1)[0].split(".")[-1]
        source_id = _choose_source_id(
            by_symbol.get(legacy_leaf, []),
            bpchar_allowed,
            candidate,
            statements_by_id,
            legacy,
        )
        bpchar_rows.append({
            "source_id": source_id,
            "legacy_identity": legacy,
            "target_identity": candidate,
            "target_cpp_identifier": target,
            "target_sql_name": target,
        })
    if [row["source_id"] for row in bpchar_rows] != [
            f"S{value:03d}" for value in range(136, 142)]:
        raise LedgerError("BPCHAR projection is not S136-S141 in order")

    joined_inputs: list[dict[str, Any]] = []
    for row in input_rows:
        if not isinstance(row, dict):
            raise LedgerError("input-name change row is not an object")
        identity = str(row.get("identity"))
        entity = _identity_entity(identity, by_identity)
        parameters = entity.get("parameters")
        ordinal = row.get("ordinal")
        if not isinstance(parameters, list) or not isinstance(ordinal, int):
            raise LedgerError(f"{identity} has no parameter projection")
        if ordinal < 1 or ordinal > len(parameters):
            raise LedgerError(f"{identity} input ordinal {ordinal} is invalid")
        parameter = parameters[ordinal - 1]
        if not isinstance(parameter, dict):
            raise LedgerError(f"{identity} parameter {ordinal} is invalid")
        target_name = parameter.get("name")
        if target_name != row.get("candidate"):
            raise LedgerError(
                f"{identity} input {ordinal} candidate/manifest name mismatch")
        expected_legacy_symbol = _candidate_symbol(entity)
        symbol_change = symbol_change_by_identity.get(identity)
        if symbol_change is not None:
            expected_legacy_symbol = str(symbol_change["legacy"])
        for rename in renames:
            if isinstance(rename, dict) and rename.get("candidate") == identity:
                legacy_identity = str(rename["legacy"])
                expected_legacy_symbol = (
                    legacy_identity.split(":", 1)[1]
                    .split("(", 1)[0].split(".")[-1]
                )
        legacy_identity = identity
        for rename in renames:
            if isinstance(rename, dict) and rename.get("candidate") == identity:
                legacy_identity = str(rename["legacy"])
                break
        source_id = _choose_source_id(
            by_symbol.get(expected_legacy_symbol, []),
            input_allowed,
            identity,
            statements_by_id,
            legacy_identity,
        )
        joined_inputs.append({
            "source_id": source_id,
            "routine_identity": identity,
            "input_ordinal": ordinal,
            "input_type": row.get("argument_type"),
            "target_cpp_identifier": target_name,
            "target_sql_name": target_name,
        })
    if len({row["routine_identity"] for row in joined_inputs}) != 169:
        raise LedgerError("309 input projection must cover exactly 169 identities")
    if len({row["source_id"] for row in joined_inputs}) != 169:
        raise LedgerError("309 input projection must cover exactly 169 source IDs")
    result = {
        "schema": _JOIN_SCHEMA,
        "inputs": {
            "approved_delta_semantic_sha256": _sha256(_json_bytes(delta)),
            "candidate_manifest_semantic_sha256": _sha256(_json_bytes(manifest)),
            "legacy_sql_sha256": _LEGACY_SHA256,
        },
        "counts": {
            "derived_routine_symbols": len(joined_symbols),
            "bpchar_identity_repairs": len(bpchar_rows),
            "derived_input_names": len(joined_inputs),
            "derived_input_name_identities":
                len({row["routine_identity"] for row in joined_inputs}),
        },
        "derived_routine_symbols": joined_symbols,
        "bpchar_identity_repairs": bpchar_rows,
        "derived_input_names": joined_inputs,
    }
    _validate_catalog_join(result)
    return result


def _validate_catalog_join(value: Mapping[str, Any]) -> None:
    if value.get("schema") != _JOIN_SCHEMA:
        raise LedgerError("unsupported catalog join schema")
    counts = value.get("counts")
    expected = {
        "derived_routine_symbols": 118,
        "bpchar_identity_repairs": 6,
        "derived_input_names": 309,
        "derived_input_name_identities": 169,
    }
    if counts != expected:
        raise LedgerError(f"catalog join counts differ: {counts!r}")
    for key, expected_count in (
        ("derived_routine_symbols", 118),
        ("bpchar_identity_repairs", 6),
        ("derived_input_names", 309),
    ):
        rows = value.get(key)
        if not isinstance(rows, list) or len(rows) != expected_count:
            raise LedgerError(
                f"catalog join {key} must contain {expected_count} rows")
    input_rows = value.get("derived_input_names")
    if not isinstance(input_rows, list):
        raise LedgerError("catalog join input rows are missing")
    keys: set[tuple[str, int]] = set()
    for row in input_rows:
        if not isinstance(row, dict):
            raise LedgerError("catalog join input row is not an object")
        if row.get("target_cpp_identifier") != row.get("target_sql_name"):
            raise LedgerError("derived SQL name does not equal C++ identifier")
        key = (str(row.get("routine_identity")), int(row.get("input_ordinal", 0)))
        if key in keys:
            raise LedgerError(f"duplicate catalog input join {key}")
        keys.add(key)


def _fmgr_inventory(
    statements: Sequence[Mapping[str, Any]],
    manifest: Mapping[str, Any] | None,
    delta: Mapping[str, Any],
) -> dict[str, Any]:
    if manifest is None:
        raise LedgerError("candidate manifest is required for the fmgr inventory")
    physical = _legacy_c_declarations(statements)
    effective = [
        {
            "legacy_owner_id": f"L{index + 1:03d}",
            "physical_declaration_id": row["declaration_id"],
            "source_id": row["source_id"],
            "routine_identity": row["routine_identity"],
            "legacy_c_symbol": row["legacy_c_symbol"],
        }
        for index, row in enumerate(
            item for item in physical if item["disposition"] == "effective")
    ]
    entities = manifest.get("entities")
    if not isinstance(entities, list):
        raise LedgerError("candidate manifest has no entities")
    target_entities = [
        entity for entity in entities
        if isinstance(entity, dict)
        and entity.get("kind") in {"function", "procedure"}
        and isinstance(entity.get("routine_properties"), dict)
        and entity["routine_properties"].get("language") == "c"
    ]
    target_entities.sort(key=lambda entity: str(entity.get("identity")))
    boundary_contracts = manifest.get("routine_boundary_contracts")
    boundary_count = manifest.get("routine_boundary_contract_count")
    if not isinstance(boundary_contracts, list) or boundary_count != 270:
        raise LedgerError(
            "candidate manifest must expose exactly 270 structured "
            "routine-boundary contracts")
    boundary_by_identity: dict[str, Mapping[str, Any]] = {}
    for contract in boundary_contracts:
        if not isinstance(contract, dict):
            raise LedgerError("routine-boundary contract is not an object")
        identity = contract.get("routine_identity")
        if not isinstance(identity, str) or not identity:
            raise LedgerError("routine-boundary contract has no identity")
        if identity in boundary_by_identity:
            raise LedgerError(
                f"duplicate routine-boundary contract {identity}")
        boundary_by_identity[identity] = contract
    target: list[dict[str, Any]] = []
    renames = delta.get("identity_renames")
    if not isinstance(renames, list):
        raise LedgerError("approved delta has no identity rename projection")
    for entity in target_entities:
        routine_identity = entity.get("routine_identity")
        if not isinstance(routine_identity, str):
            raise LedgerError(f"{entity.get('identity')} has no routine identity")
        boundary = boundary_by_identity.get(routine_identity)
        if boundary is None:
            raise LedgerError(
                f"{routine_identity} has no structured boundary contract")
        parameters = boundary.get("parameters")
        result_contract = boundary.get("result")
        if not isinstance(parameters, list) or not isinstance(
                result_contract, dict):
            raise LedgerError(
                f"{routine_identity} has an incomplete boundary contract")
        sql_parameters = [
            parameter for parameter in parameters
            if isinstance(parameter, dict)
            and parameter.get("injected_call_context") is False
        ]
        injected_parameters = [
            parameter for parameter in parameters
            if isinstance(parameter, dict)
            and parameter.get("injected_call_context") is True
        ]
        if len(sql_parameters) + len(injected_parameters) != len(parameters):
            raise LedgerError(
                f"{routine_identity} boundary parameter is malformed")
        sql_identity = str(entity.get("identity")).removeprefix("sql:")
        target.append({
            "sql_identity": sql_identity,
            "legacy_source_id": _source_id_for_identity(
                sql_identity, statements, renames),
            "cpp_declaration": entity.get("cpp_declaration"),
            "source": {
                "module": entity.get("source_module"),
                "path": entity.get("source_path"),
                "sha256": entity.get("source_sha256"),
                "span": entity.get("source_span"),
            },
            "target_symbol": _candidate_symbol(entity),
            "inputs": [
                {
                    "ordinal": int(parameter["sql_ordinal"]) + 1,
                    "sql_name": parameter.get("sql_name"),
                    "sql_type": parameter.get("sql_type"),
                    "cpp_identifier": parameter.get("cpp_name"),
                    "cpp_type": parameter.get("cpp_type"),
                    "nullable": parameter.get("nullable"),
                    "converter_key": parameter.get("converter_key"),
                }
                for parameter in sql_parameters
            ],
            "result": {
                "sql_type": result_contract.get("sql_type"),
                "cpp_type": result_contract.get("cpp_type"),
                "representation": result_contract.get("form"),
                "row_cpp_declaration": result_contract.get("row_cpp_name"),
                "nullable": result_contract.get("nullable"),
                "converter_key": result_contract.get("converter_key"),
            },
            "injected_context": [
                {
                    "cpp_ordinal": parameter.get("cpp_ordinal"),
                    "cpp_identifier": parameter.get("cpp_name"),
                    "cpp_type": parameter.get("cpp_type"),
                    "converter_key": parameter.get("converter_key"),
                }
                for parameter in injected_parameters
            ],
        })
    if set(boundary_by_identity) != {
            str(entity.get("routine_identity")) for entity in target_entities}:
        raise LedgerError(
            "structured routine-boundary contracts and target owners "
            "are not a one-to-one set")
    result = {
        "schema": _FMGR_SCHEMA,
        "complete": True,
        "data_gaps": [],
        "counts": {
            "legacy_physical_declarations": len(physical),
            "legacy_effective_owners": len(effective),
            "target_owners": len(target),
        },
        "legacy_physical_declarations": physical,
        "legacy_effective_owners": effective,
        "target_owners": target,
    }
    if result["counts"] != {
        "legacy_physical_declarations": 268,
        "legacy_effective_owners": 266,
        "target_owners": 270,
    }:
        raise LedgerError(f"fmgr inventory count mismatch: {result['counts']}")
    return result


def _parse_result_fields(sql_type: str) -> list[dict[str, Any]]:
    lower = sql_type.lower()
    if not lower.startswith("table (") or not sql_type.endswith(")"):
        return []
    rows: list[dict[str, Any]] = []
    for ordinal, field in enumerate(_split_commas(sql_type[7:-1]), 1):
        tokens = field.split(maxsplit=1)
        if len(tokens) != 2:
            raise LedgerError(f"cannot parse result field {field!r}")
        rows.append({
            "ordinal": ordinal,
            "sql_name": tokens[0],
            "sql_type": tokens[1],
            "cpp_member": None,
            "cpp_type": None,
            "nullable": None,
            "converter_key": None,
        })
    return rows


def _result_inventory(
    manifest: Mapping[str, Any] | None,
    statements: Sequence[Mapping[str, Any]],
    delta: Mapping[str, Any],
) -> dict[str, Any]:
    if manifest is None:
        raise LedgerError("candidate manifest is required for result inventory")
    entities = manifest.get("entities")
    if not isinstance(entities, list):
        raise LedgerError("candidate manifest has no entities")
    result_contracts = manifest.get("result_shape_contracts")
    result_contract_count = manifest.get("result_shape_contract_count")
    if not isinstance(result_contracts, list) or result_contract_count != 25:
        raise LedgerError(
            "candidate manifest must expose exactly 25 structured "
            "result-shape contracts")
    result_by_identity: dict[str, Mapping[str, Any]] = {}
    for contract in result_contracts:
        if not isinstance(contract, dict):
            raise LedgerError("result-shape contract is not an object")
        identity = contract.get("routine_identity")
        if not isinstance(identity, str) or not identity:
            raise LedgerError("result-shape contract has no identity")
        if identity in result_by_identity:
            raise LedgerError(f"duplicate result-shape contract {identity}")
        result_by_identity[identity] = contract
    rows: list[dict[str, Any]] = []
    renames = delta.get("identity_renames")
    if not isinstance(renames, list):
        raise LedgerError("approved delta has no identity rename projection")
    for entity in entities:
        if not isinstance(entity, dict) or entity.get("kind") not in {
                "function", "sql_function"}:
            continue
        sql_type = entity.get("result_sql_type")
        if not isinstance(sql_type, str):
            continue
        if sql_type.startswith("table ("):
            form = "returns_table"
        elif sql_type == "record":
            form = "named_out"
        else:
            continue
        sql_identity = (
            str(entity.get("identity"))
            .removeprefix("sql:")
            .replace("sql_function:", "function:", 1)
        )
        routine_identity = entity.get("routine_identity")
        if not isinstance(routine_identity, str):
            raise LedgerError(f"{sql_identity} has no routine identity")
        contract = result_by_identity.get(routine_identity)
        if contract is None:
            raise LedgerError(
                f"{sql_identity} has no structured result-shape contract")
        fields = contract.get("fields")
        if not isinstance(fields, list) or not fields:
            raise LedgerError(
                f"{sql_identity} result-shape contract has no fields")
        rows.append({
            "source_id": _source_id_for_identity(
                sql_identity, statements, renames),
            "sql_identity": sql_identity,
            "wrapper_kind": form,
            "cpp_function": entity.get("cpp_declaration"),
            "row_cpp_declaration": contract.get("row_cpp_name"),
            "source": {
                "module": entity.get("source_module"),
                "path": entity.get("source_path"),
                "sha256": entity.get("source_sha256"),
                "span": entity.get("source_span"),
            },
            "result_fields": [
                {
                    "ordinal": int(field["ordinal"]) + 1,
                    "sql_name": field.get("sql_name"),
                    "sql_type": field.get("sql_type"),
                    "cpp_member": field.get("cpp_name"),
                    "cpp_type": field.get("cpp_type"),
                    "nullable": field.get("nullable"),
                    "converter_key": field.get("converter_key"),
                }
                for field in fields if isinstance(field, dict)
            ],
        })
    rows.sort(key=lambda row: row["sql_identity"])
    counts = {
        "returns_table": sum(row["wrapper_kind"] == "returns_table" for row in rows),
        "named_out": sum(row["wrapper_kind"] == "named_out" for row in rows),
        "total": len(rows),
    }
    if counts != {"returns_table": 18, "named_out": 7, "total": 25}:
        raise LedgerError(
            "candidate manifest must contain the required 18 RETURNS TABLE "
            f"and seven named-OUT owners; observed {counts}")
    return {
        "schema": _RESULT_SCHEMA,
        "complete": True,
        "data_gaps": [],
        "counts": counts,
        "rows": rows,
    }


def _strip_cpp_comments(source: str) -> str:
    """Remove C++ comments while preserving byte positions and line numbers."""
    result = list(source)
    state = "code"
    escaped = False
    index = 0
    while index < len(source):
        value = source[index]
        next_value = source[index + 1] if index + 1 < len(source) else "\0"
        if state == "line_comment":
            if value == "\n":
                state = "code"
            else:
                result[index] = " "
            index += 1
            continue
        if state == "block_comment":
            if value == "*" and next_value == "/":
                result[index] = result[index + 1] = " "
                state = "code"
                index += 2
            else:
                if value != "\n":
                    result[index] = " "
                index += 1
            continue
        if state in {"string", "character"}:
            if escaped:
                escaped = False
            elif value == "\\":
                escaped = True
            elif (
                state == "string" and value == '"'
                or state == "character" and value == "'"
            ):
                state = "code"
            index += 1
            continue
        if value == "/" and next_value == "/":
            result[index] = result[index + 1] = " "
            state = "line_comment"
            index += 2
        elif value == "/" and next_value == "*":
            result[index] = result[index + 1] = " "
            state = "block_comment"
            index += 2
        elif value == '"':
            state = "string"
            index += 1
        elif value == "'":
            state = "character"
            index += 1
        else:
            index += 1
    return "".join(result)


def _test_indexes_guarded_lines(source: str) -> set[int]:
    """Return lines in the positive branch of BIOSEQ_TEST_INDEXES guards."""
    result: set[int] = set()
    conditions: list[str] = []
    exact_positive = re.compile(
        r"^#\s*if\s+defined\s*\(\s*BIOSEQ_TEST_INDEXES\s*\)\s*$"
    )
    exact_negative = re.compile(
        r"^#\s*if\s+!\s*defined\s*\(\s*BIOSEQ_TEST_INDEXES\s*\)\s*$"
    )
    for line_number, line in enumerate(source.splitlines(), 1):
        directive = line.strip()
        if re.match(r"^#\s*if(?:def|ndef)?\b", directive):
            if exact_positive.fullmatch(directive):
                conditions.append("positive")
            elif exact_negative.fullmatch(directive):
                conditions.append("negative")
            elif re.fullmatch(
                r"#\s*ifdef\s+BIOSEQ_TEST_INDEXES", directive
            ):
                conditions.append("positive")
            elif re.fullmatch(
                r"#\s*ifndef\s+BIOSEQ_TEST_INDEXES", directive
            ):
                conditions.append("negative")
            else:
                conditions.append("other")
            continue
        if re.match(r"^#\s*elif\b", directive):
            if not conditions:
                raise LedgerError("unmatched C++ #elif in source scan")
            conditions[-1] = (
                "positive" if exact_positive.fullmatch(
                    re.sub(r"^#\s*elif", "#if", directive)
                ) else "other"
            )
            continue
        if re.match(r"^#\s*else\b", directive):
            if not conditions:
                raise LedgerError("unmatched C++ #else in source scan")
            conditions[-1] = {
                "positive": "negative",
                "negative": "positive",
                "other": "other",
            }[conditions[-1]]
            continue
        if re.match(r"^#\s*endif\b", directive):
            if not conditions:
                raise LedgerError("unmatched C++ #endif in source scan")
            conditions.pop()
            continue
        if "positive" in conditions:
            result.add(line_number)
    if conditions:
        raise LedgerError("unterminated C++ conditional in source scan")
    return result


def _index_table_shard_mentions(
    relative: str,
    source: str,
) -> list[dict[str, Any]]:
    token = re.compile(r"\bindex_table_shard(?:_codec)?\b")
    commentless = _strip_cpp_comments(source)
    original_lines = source.splitlines()
    code_lines = commentless.splitlines()
    guarded_lines = _test_indexes_guarded_lines(source)
    rows: list[dict[str, Any]] = []
    for line_number, line in enumerate(original_lines, 1):
        code_line = code_lines[line_number - 1]
        for match in token.finditer(line):
            rendered = match.group(0)
            in_code = (
                code_line[match.start():match.end()] == rendered
            )
            if not in_code:
                classification = "comment"
            elif relative == _INDEX_TABLE_SHARD_OWNER and (
                re.search(
                    r"bioseq_sql::config_dump\]\]\s+index_table_shard\s*\{",
                    code_line,
                )
                or re.search(
                    r"std::is_(?:aggregate|standard_layout)_v"
                    r"<index_table_shard>",
                    code_line,
                )
            ):
                classification = "row_owner"
            elif (
                "/compile_contracts/" in relative
                or relative.endswith("sql_entity_handles_contract.cpp")
            ):
                classification = "compile_contract"
            elif (
                relative == _INDEX_TABLE_SHARD_RUNTIME_SUPPORT
                and line_number in guarded_lines
            ):
                classification = "runtime_test_consumer"
            else:
                classification = "production_consumer"
            rows.append({
                "source_path": relative,
                "line": line_number,
                "token": rendered,
                "classification": classification,
                "line_sha256": _sha256(line.strip()),
            })
    return rows


def _git_text(
    root: Path,
    revision: str,
    relative: str,
    git: str,
) -> str:
    command = [git, "show", f"{revision}:{relative}"]
    try:
        result = subprocess.run(
            command,
            cwd=root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as error:
        raise LedgerError(f"cannot execute {git}: {error}") from error
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise LedgerError(f"{' '.join(command)} failed: {detail}")
    try:
        return result.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        raise LedgerError(
            f"historical source {revision}:{relative} is not UTF-8"
        ) from error


def _historical_index_table_shard_mentions(
    root: Path,
    revision: str,
    git: str,
) -> list[dict[str, Any]]:
    command = [
        git,
        "grep",
        "-l",
        "-I",
        "index_table_shard",
        revision,
        "--",
        "main/bioseq/backend",
    ]
    try:
        result = subprocess.run(
            command,
            cwd=root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as error:
        raise LedgerError(f"cannot execute {git}: {error}") from error
    if result.returncode not in {0, 1}:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise LedgerError(f"{' '.join(command)} failed: {detail}")
    prefix = revision + ":"
    paths = []
    for line in result.stdout.decode("utf-8").splitlines():
        relative = line.removeprefix(prefix)
        if relative:
            paths.append(relative)
    rows: list[dict[str, Any]] = []
    for relative in sorted(set(paths)):
        rows.extend(_index_table_shard_mentions(
            relative,
            _git_text(root, revision, relative, git),
        ))
    production = [
        row for row in rows
        if row["classification"] == "production_consumer"
    ]
    if production:
        first = production[0]
        raise LedgerError(
            "index_table_shard had a historical production consumer at "
            f"{revision}:{first['source_path']}:{first['line']}"
        )
    return rows


def _index_table_shard_runtime_operations(source: str) -> list[str]:
    guarded_lines = _test_indexes_guarded_lines(source)

    def guarded_body(pattern: str) -> str | None:
        match = re.search(pattern, source, flags=re.DOTALL)
        if match is None:
            return None
        first_line = 1 + source.count("\n", 0, match.start())
        last_line = 1 + source.count("\n", 0, match.end())
        if (
            first_line not in guarded_lines
            or last_line not in guarded_lines
        ):
            return None
        return match.group("body")

    dedicated_match = _index_table_shard_runtime_body(source)
    if dedicated_match is None:
        return []
    dedicated_first_line = (
        1 + source.count("\n", 0, dedicated_match.start()))
    dedicated_last_line = (
        1 + source.count("\n", 0, dedicated_match.end()))
    if (
        dedicated_first_line not in guarded_lines
        or dedicated_last_line not in guarded_lines
    ):
        return []
    dedicated = dedicated_match.group("body")
    wrapper = guarded_body(
        r"\[\[nodiscard\]\]\s+runtime_receipt_entry\s+"
        r"exercise_codec\s*\([^)]*\)\s*\{"
        r"(?P<body>.*?)"
        r"\n\}\s*\n\s*template\s*<\s*class\s+Codec\s*,"
        r"\s*class\s+Exercise\s*>"
    )
    generic = guarded_body(
        r"\[\[nodiscard\]\]\s+runtime_receipt_entry\s+"
        r"exercise_codec_with_external\s*\([^)]*\)\s*\{"
        r"(?P<body>.*?)"
        r"\n\}\s*\n\s*template\s*<\s*class\s+Codec\s*,"
        r"\s*class\s+NullRowFactory"
    )
    descriptor_helper = guarded_body(
        r"\bvoid\s+validate_descriptor_mutations\s*"
        r"\(\s*TupleDesc\s+descriptor\s*\)\s*\{"
        r"(?P<body>.*?)"
        r"\n\}\s*\n\s*template\s*<\s*class\s+Codec\s*,"
        r"\s*auto\s+Member"
    )
    scan_helper = guarded_body(
        r"\[\[nodiscard\]\]\s+ScanKeyData\s+"
        r"verify_equal_scan_key\s*\([^)]*\)\s*\{"
        r"(?P<body>.*?)"
        r"\n\}\s*\n\s*template\s*<\s*class\s+Codec\s*>"
    )
    if any(
        body is None
        for body in (
            wrapper,
            generic,
            descriptor_helper,
            scan_helper,
        )
    ):
        return []
    assert wrapper is not None
    assert generic is not None
    assert descriptor_helper is not None
    assert scan_helper is not None

    dedicated_link = re.search(
        r"\bexercise_codec\s*<\s*codec\s*>\s*\("
        r"\s*descriptor\s*,\s*relation\s*,\s*\"sql:table\"\s*,",
        dedicated,
        flags=re.DOTALL,
    )
    wrapper_link = re.search(
        r"\bexercise_codec_with_external\s*<\s*Codec\s*>\s*\("
        r"\s*descriptor\s*,\s*live_relation\s*,"
        r"\s*nullptr\s*,\s*nullptr\s*,\s*entity_kind\s*,",
        wrapper,
        flags=re.DOTALL,
    )
    if dedicated_link is None or wrapper_link is None:
        return []

    observed: set[str] = set()
    patterns = {
        "encode": r"\bCodec::encode\s*\(",
        "deform": r"\bCodec::deform\s*\(",
        "update": r"\btypename\s+Codec::update\b",
        "form_tuple": r"\.form_tuple\s*\(\s*descriptor\s*\)",
        "modify": r"\.modify\s*\([^;]*?\bdescriptor\s*\)",
        "catalog_insert": (
            r"\bCatalogTupleInsert\s*\(\s*live_relation\s*,"
        ),
        "catalog_update": (
            r"\bCatalogTupleUpdate\s*\(\s*live_relation\s*,"
        ),
        "catalog_delete": (
            r"\bCatalogTupleDelete\s*\(\s*live_relation\s*,"
        ),
    }
    for operation, pattern in patterns.items():
        if re.search(pattern, generic, flags=re.DOTALL) is not None:
            observed.add(operation)

    descriptor_link = (
        re.search(
            r"\bvalidate_descriptor_mutations\s*<\s*Codec\s*>\s*"
            r"\(\s*descriptor\s*\)",
            generic,
        )
        is not None
        and re.search(
            r"\bCodec::validate_descriptor\s*\(\s*descriptor\s*\)",
            descriptor_helper,
        )
        is not None
    )
    if descriptor_link:
        observed.add("validate_descriptor")

    scan_link = (
        re.search(
            r"\bverify_equal_scan_key\s*<\s*codec\s*,",
            dedicated,
        )
        is not None
        and re.search(
            r"\bCodec::template\s+scan_key\s*<\s*Member\s*>\s*\(",
            scan_helper,
        )
        is not None
        and re.search(
            r"\bverify_scan\s*\(\s*\)",
            generic,
        )
        is not None
    )
    if scan_link:
        observed.add("scan_key")
    return sorted(observed)


def _index_table_shard_runtime_body(
    source: str,
) -> re.Match[str] | None:
    return re.search(
        r"\[\[nodiscard\]\]\s+runtime_receipt_entry\s+"
        r"exercise_index_table_shard\s*\(\s*\)\s*\{"
        r"(?P<body>.*?)"
        r"\n\}\s*\n\s*"
        r"\[\[nodiscard\]\]\s+std::vector<runtime_receipt_entry>",
        source,
        flags=re.DOTALL,
    )


def _index_table_shard_forwards_live_relation(source: str) -> bool:
    """Bind the dedicated shard owner to the generic live-CRUD harness."""
    dedicated = _index_table_shard_runtime_body(source)
    if dedicated is None:
        return False
    body = dedicated.group("body")
    forwarding = re.search(
        r"\bwith_relation_descriptor\s*<\s*"
        r"(?:indexing_codecs::)?index_table_shard_codec\s*>\s*\("
        r"\s*\"bioseq_internal\"\s*,\s*\"index_table_shard\"\s*,"
        r"\s*\[\]\s*\(\s*TupleDesc\s+descriptor\s*,"
        r"\s*Relation\s+relation\s*\)\s*\{"
        r".*?\breturn\s+exercise_codec\s*<\s*codec\s*>\s*\("
        r"\s*descriptor\s*,\s*relation\s*,\s*\"sql:table\"\s*,",
        body,
        flags=re.DOTALL,
    )
    return forwarding is not None


def _validated_index_table_shard_runtime_operations(
    source: str,
) -> list[str]:
    if not _index_table_shard_forwards_live_relation(source):
        raise LedgerError(
            "index_table_shard runtime consumer does not forward a live "
            "Relation into the generic codec harness")
    observed = _index_table_shard_runtime_operations(source)
    if tuple(observed) != _INDEX_TABLE_SHARD_RUNTIME_OPERATIONS:
        raise LedgerError(
            "index_table_shard runtime codec operations differ: "
            f"{observed!r}")
    return observed


def _evidence_seal(value: Mapping[str, Any]) -> str:
    return _sha256(_json_bytes(value))


def _summarize_mentions(
    mentions: Sequence[Mapping[str, Any]],
) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str], list[Mapping[str, Any]]] = {}
    for mention in mentions:
        key = (
            str(mention.get("source_path")),
            str(mention.get("classification")),
        )
        grouped.setdefault(key, []).append(mention)
    result: list[dict[str, Any]] = []
    for (source_path, classification), rows in sorted(grouped.items()):
        exact_rows = sorted(
            (dict(row) for row in rows),
            key=lambda row: (
                int(row["line"]),
                str(row["token"]),
                str(row["line_sha256"]),
            ),
        )
        result.append({
            "source_path": source_path,
            "classification": classification,
            "mention_count": len(exact_rows),
            "line_numbers": sorted({
                int(row["line"]) for row in exact_rows
            }),
            "tokens": sorted({
                str(row["token"]) for row in exact_rows
            }),
            "matching_mentions_sha256": _sha256(
                _json_bytes(exact_rows)),
        })
    return result


def _index_table_shard_consumer_evidence(
    root: Path,
    git: str,
    source_overrides: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    source_root = root / "main" / "bioseq" / "backend"
    sources = {
        path.relative_to(root).as_posix(): path.read_text(encoding="utf-8")
        for path in sorted(
            (*source_root.rglob("*.cppm"), *source_root.rglob("*.cpp")),
            key=lambda value: value.as_posix(),
        )
    }
    if source_overrides is not None:
        sources.update(source_overrides)
    mentions: list[dict[str, Any]] = []
    for relative, source in sorted(sources.items()):
        mentions.extend(_index_table_shard_mentions(relative, source))
    production = [
        row for row in mentions
        if row["classification"] == "production_consumer"
    ]
    if production:
        first = production[0]
        raise LedgerError(
            "index_table_shard gained a production consumer at "
            f"{first['source_path']}:{first['line']}"
        )
    runtime_source = sources.get(_INDEX_TABLE_SHARD_RUNTIME_SUPPORT)
    runtime_mentions = [
        row for row in mentions
        if row["classification"] == "runtime_test_consumer"
    ]
    if runtime_source is None or not runtime_mentions:
        raise LedgerError(
            "index_table_shard has no dedicated test-only runtime consumer")
    if (
        "#if defined(BIOSEQ_TEST_INDEXES)" not in runtime_source
        or "void exercise_all(std::string_view expected_receipt)"
            not in runtime_source
    ):
        raise LedgerError(
            "index_table_shard runtime consumer lost its exact guard or symbol")
    observed_operations = (
        _validated_index_table_shard_runtime_operations(runtime_source))

    current = {
        "scope": "main/bioseq/backend/**/*.{cppm,cpp}",
        "production_consumer_count": 0,
        "non_production_mention_groups": _summarize_mentions(mentions),
    }
    current["scan_sha256"] = _evidence_seal(current)
    historical: list[dict[str, Any]] = []
    for revision in _INDEX_TABLE_SHARD_HISTORY:
        rows = _historical_index_table_shard_mentions(root, revision, git)
        scan = {
            "source_revision": revision,
            "scope": "main/bioseq/backend",
            "production_consumer_count": 0,
            "non_production_mention_groups": _summarize_mentions(rows),
        }
        scan["scan_sha256"] = _evidence_seal(scan)
        historical.append(scan)
    runtime = {
        "required": True,
        "profile": "test_indexes",
        "source_path": _INDEX_TABLE_SHARD_RUNTIME_SUPPORT,
        "cpp_symbol": _INDEX_TABLE_SHARD_RUNTIME_SYMBOL,
        "preprocessor_guard": "#if defined(BIOSEQ_TEST_INDEXES)",
        "live_relation_forwarding_verified": True,
        "required_operations": list(
            _INDEX_TABLE_SHARD_RUNTIME_OPERATIONS),
        "observed_operations": observed_operations,
        "descriptor_field_count": 9,
        "nullable_field_count": 0,
        "primary_key_fields": [
            "database_oid",
            "tablespace_oid",
            "main_relfilenode",
            "column_attno",
            "index_name",
            "shard_idx",
        ],
    }
    runtime["contract_sha256"] = _evidence_seal(runtime)
    return {
        "current_source_scan": current,
        "historical_source_scans": historical,
        "runtime_codec_contract": runtime,
    }


def _validate_index_table_shard_disposition(
    row: Mapping[str, Any],
) -> None:
    if row.get("sql_identity") != _INDEX_TABLE_SHARD_IDENTITY:
        raise LedgerError(
            "index_table_shard disposition attached to the wrong identity")
    if row.get("consumer_disposition") != (
            "no_production_consumer_exists"):
        raise LedgerError(
            "index_table_shard must use no_production_consumer_exists")
    if row.get("migrated_consumers") != []:
        raise LedgerError(
            "index_table_shard no-consumer disposition has consumers")
    evidence = row.get("consumer_evidence")
    if not isinstance(evidence, dict):
        raise LedgerError(
            "index_table_shard no-consumer disposition has no evidence")
    _require_exact_fields(
        evidence,
        (
            "current_source_scan",
            "historical_source_scans",
            "runtime_codec_contract",
        ),
        "index_table_shard consumer evidence",
    )
    current = evidence.get("current_source_scan")
    history = evidence.get("historical_source_scans")
    runtime = evidence.get("runtime_codec_contract")
    if isinstance(current, dict):
        _require_exact_fields(
            current,
            (
                "scope",
                "production_consumer_count",
                "non_production_mention_groups",
                "scan_sha256",
            ),
            "index_table_shard current source scan",
        )
    if (
        not isinstance(current, dict)
        or current.get("scope")
            != "main/bioseq/backend/**/*.{cppm,cpp}"
        or current.get("production_consumer_count") != 0
        or current.get("scan_sha256") != _evidence_seal({
            key: value for key, value in current.items()
            if key != "scan_sha256"
        })
    ):
        raise LedgerError(
            "index_table_shard current no-consumer evidence is invalid")
    current_groups = current.get("non_production_mention_groups")
    if not isinstance(current_groups, list):
        raise LedgerError(
            "index_table_shard current mention groups are invalid")
    group_keys: set[tuple[str, str]] = set()
    for index, group in enumerate(current_groups):
        group = _require_exact_fields(
            group,
            (
                "source_path",
                "classification",
                "mention_count",
                "line_numbers",
                "tokens",
                "matching_mentions_sha256",
            ),
            f"index_table_shard current mention group {index}",
        )
        key = (
            str(group.get("source_path")),
            str(group.get("classification")),
        )
        if (
            key in group_keys
            or group.get("classification") not in {
                "comment",
                "row_owner",
                "compile_contract",
                "runtime_test_consumer",
            }
            or not isinstance(group.get("mention_count"), int)
            or group["mention_count"] < 1
            or not isinstance(group.get("line_numbers"), list)
            or not group["line_numbers"]
            or not all(
                isinstance(line, int) and line > 0
                for line in group["line_numbers"]
            )
            or group["line_numbers"] != sorted(set(group["line_numbers"]))
            or not isinstance(group.get("tokens"), list)
            or not group["tokens"]
            or not all(
                token in {"index_table_shard", "index_table_shard_codec"}
                for token in group["tokens"]
            )
            or not re.fullmatch(
                r"[0-9a-f]{64}",
                str(group.get("matching_mentions_sha256")),
            )
        ):
            raise LedgerError(
                "index_table_shard current mention group is invalid")
        group_keys.add(key)
    if (
        (_INDEX_TABLE_SHARD_RUNTIME_SUPPORT, "runtime_test_consumer")
            not in group_keys
        or (_INDEX_TABLE_SHARD_OWNER, "row_owner") not in group_keys
        or not any(
            classification == "compile_contract"
            for _, classification in group_keys
        )
    ):
        raise LedgerError(
            "index_table_shard current non-production evidence is incomplete")
    if not isinstance(history, list) or [
            item.get("source_revision")
            for item in history if isinstance(item, dict)
    ] != list(_INDEX_TABLE_SHARD_HISTORY):
        raise LedgerError(
            "index_table_shard historical evidence revisions differ")
    for index, item in enumerate(history):
        if isinstance(item, dict):
            _require_exact_fields(
                item,
                (
                    "source_revision",
                    "scope",
                    "production_consumer_count",
                    "non_production_mention_groups",
                    "scan_sha256",
                ),
                f"index_table_shard historical scan {index}",
            )
        if (
            not isinstance(item, dict)
            or item.get("scope") != "main/bioseq/backend"
            or item.get("production_consumer_count") != 0
            or not isinstance(
                item.get("non_production_mention_groups"), list)
            or item.get("scan_sha256") != _evidence_seal({
                key: value for key, value in item.items()
                if key != "scan_sha256"
            })
        ):
            raise LedgerError(
                "index_table_shard historical no-consumer evidence is invalid")
        historical_groups = item["non_production_mention_groups"]
        if not historical_groups:
            raise LedgerError(
                "index_table_shard historical mention evidence is empty")
        for group_index, group in enumerate(historical_groups):
            group = _require_exact_fields(
                group,
                (
                    "source_path",
                    "classification",
                    "mention_count",
                    "line_numbers",
                    "tokens",
                    "matching_mentions_sha256",
                ),
                "index_table_shard historical mention group "
                f"{index}:{group_index}",
            )
            if (
                group.get("classification") != "comment"
                or not isinstance(group.get("mention_count"), int)
                or group["mention_count"] < 1
                or not isinstance(group.get("line_numbers"), list)
                or not group["line_numbers"]
                or not isinstance(group.get("tokens"), list)
                or not group["tokens"]
                or not re.fullmatch(
                    r"[0-9a-f]{64}",
                    str(group.get("matching_mentions_sha256")),
                )
            ):
                raise LedgerError(
                    "index_table_shard historical mention group is invalid")
    if isinstance(runtime, dict):
        _require_exact_fields(
            runtime,
            (
                "required",
                "profile",
                "source_path",
                "cpp_symbol",
                "preprocessor_guard",
                "live_relation_forwarding_verified",
                "required_operations",
                "observed_operations",
                "descriptor_field_count",
                "nullable_field_count",
                "primary_key_fields",
                "contract_sha256",
            ),
            "index_table_shard runtime codec contract",
        )
    if (
        not isinstance(runtime, dict)
        or runtime.get("required") is not True
        or runtime.get("profile") != "test_indexes"
        or runtime.get("source_path")
            != _INDEX_TABLE_SHARD_RUNTIME_SUPPORT
        or runtime.get("cpp_symbol") != _INDEX_TABLE_SHARD_RUNTIME_SYMBOL
        or runtime.get("preprocessor_guard")
            != "#if defined(BIOSEQ_TEST_INDEXES)"
        or runtime.get("live_relation_forwarding_verified") is not True
        or tuple(runtime.get("required_operations", ()))
            != _INDEX_TABLE_SHARD_RUNTIME_OPERATIONS
        or runtime.get("observed_operations")
            != runtime.get("required_operations")
        or runtime.get("descriptor_field_count") != 9
        or runtime.get("nullable_field_count") != 0
        or runtime.get("primary_key_fields") != [
            "database_oid",
            "tablespace_oid",
            "main_relfilenode",
            "column_attno",
            "index_name",
            "shard_idx",
        ]
        or runtime.get("contract_sha256") != _evidence_seal({
            key: value for key, value in runtime.items()
            if key != "contract_sha256"
        })
    ):
        raise LedgerError(
            "index_table_shard runtime codec contract is invalid")


def _migrated_codec_consumers(
    root: Path,
    codec_type: str,
) -> list[dict[str, Any]]:
    source_root = root / "main" / "bioseq" / "backend"
    rows: list[dict[str, Any]] = []
    for path in sorted(
        (*source_root.rglob("*.cppm"), *source_root.rglob("*.cpp")),
        key=lambda value: value.as_posix(),
    ):
        relative = path.relative_to(root).as_posix()
        if (
            "/compile_contracts/" in relative
            or relative.endswith("sql_entity_handles_contract.cpp")
            or relative == _INDEX_TABLE_SHARD_RUNTIME_SUPPORT
            or path.name.endswith(("_test.cpp", "_test.cppm"))
            or "/test/" in relative
        ):
            continue
        text = path.read_text(encoding="utf-8")
        guarded_lines = _test_indexes_guarded_lines(text)
        if guarded_lines:
            text = "\n".join(
                "" if line_number in guarded_lines else line
                for line_number, line in enumerate(
                    text.splitlines(), 1)
            )
        aliases = {codec_type}
        for alias in _ROW_PRODUCTION_CODEC_ALIASES.get(
                codec_type, ()):
            if re.search(
                rf"\busing\s+{re.escape(alias)}\s*=\s*"
                rf"[^;]*\b{re.escape(codec_type)}\b[^;]*;",
                text,
                flags=re.DOTALL,
            ):
                aliases.add(alias)
        operation_pattern = re.compile(
            rf"\b(?:{'|'.join(re.escape(alias) for alias in sorted(aliases))})::"
            r"(attribute_number|deform|encode|expected_descriptor|field|"
            r"fields_count|scan_key|update|validate_descriptor)\b"
        )
        operations = sorted(set(operation_pattern.findall(text)))
        if operations:
            rows.append({"source_path": relative, "operations": operations})
    if not rows:
        raise LedgerError(
            f"generated row codec {codec_type} has no production consumer")
    return rows


def _row_authority(
    manifest: Mapping[str, Any] | None,
    root: Path,
    git: str,
) -> dict[str, Any]:
    if manifest is None:
        raise LedgerError("candidate manifest is required for row authority")
    modules = manifest.get("row_codec_modules")
    contracts = manifest.get("row_codec_contracts")
    entities = manifest.get("entities")
    if (
        not isinstance(modules, list)
        or not isinstance(contracts, list)
        or manifest.get("row_codec_contract_count") != 14
        or not isinstance(entities, list)
    ):
        raise LedgerError("candidate manifest has no row-codec projection")
    by_cpp = {
        str(entity.get("cpp_declaration")): entity
        for entity in entities if isinstance(entity, dict)
    }
    modules_by_row: dict[
        str,
        tuple[Mapping[str, Any], str],
    ] = {}
    for module in modules:
        if not isinstance(module, dict):
            continue
        cpp_names = module.get("row_cpp_names")
        identities = module.get("row_entity_identities")
        if not isinstance(cpp_names, list) or not isinstance(identities, list):
            raise LedgerError("row codec module has no row identity projection")
        if len(cpp_names) != len(identities):
            raise LedgerError("row codec module row arrays differ in length")
        for cpp_name, identity in zip(cpp_names, identities, strict=True):
            rendered_identity = str(identity)
            if rendered_identity in modules_by_row:
                raise LedgerError(
                    f"duplicate row codec module owner {rendered_identity}")
            modules_by_row[rendered_identity] = (module, str(cpp_name))
    rows: list[dict[str, Any]] = []
    for contract in contracts:
        if not isinstance(contract, dict):
            raise LedgerError("row-codec contract is not an object")
        identity = contract.get("row_identity")
        cpp_name = contract.get("row_cpp_name")
        fields = contract.get("fields")
        if (
            not isinstance(identity, str)
            or not isinstance(cpp_name, str)
            or not isinstance(fields, list)
            or not fields
        ):
            raise LedgerError("row-codec contract is incomplete")
        module_owner = modules_by_row.get(identity)
        entity = by_cpp.get(cpp_name)
        if module_owner is None or entity is None:
            raise LedgerError(
                f"row-codec contract {identity} has no module/entity owner")
        module, expected_cpp_name = module_owner
        if cpp_name != expected_cpp_name:
            raise LedgerError(
                f"row-codec contract {identity} C++ declaration "
                f"{cpp_name!r} differs from module owner "
                f"{expected_cpp_name!r}")
        sql_identity = identity.removeprefix("sql:")
        codec_type = cpp_name.split("::")[-1] + "_codec"
        row: dict[str, Any] = {
            "sql_identity": sql_identity,
            "cpp_row_declaration": cpp_name,
            "source": {
                "module": entity.get("source_module"),
                "path": entity.get("source_path"),
                "sha256": entity.get("source_sha256"),
                "span": entity.get("source_span"),
            },
            "generated_codec": {
                "module": module.get("module_name"),
                "namespace": module.get("cpp_namespace"),
                "type": codec_type,
            },
            "ordered_fields": [
                {
                    "ordinal": int(field["ordinal"]) + 1,
                    "cpp_member": field.get("cpp_name"),
                    "cpp_type": field.get("cpp_type"),
                    "sql_name": field.get("sql_name"),
                    "sql_type": field.get("sql_type"),
                    "nullable": field.get("nullable"),
                    "converter_key": field.get("converter_key"),
                }
                for field in fields if isinstance(field, dict)
            ],
            "legacy_authorities": [
                {
                    "baseline": "immutable_source_revision",
                    "status": "replaced",
                    "authority": authority,
                }
                for authority in _ROW_LEGACY_AUTHORITIES.get(
                    sql_identity, ())
            ],
            "migrated_consumers": (
                []
                if sql_identity == _INDEX_TABLE_SHARD_IDENTITY
                else _migrated_codec_consumers(root, codec_type)
            ),
        }
        if sql_identity == _INDEX_TABLE_SHARD_IDENTITY:
            row["consumer_disposition"] = (
                "no_production_consumer_exists")
            row["consumer_evidence"] = (
                _index_table_shard_consumer_evidence(root, git))
            _validate_index_table_shard_disposition(row)
        rows.append(row)
    rows.sort(key=lambda row: row["sql_identity"])
    contract_identities = {
        str(contract.get("row_identity"))
        for contract in contracts if isinstance(contract, dict)
    }
    if contract_identities != set(modules_by_row):
        raise LedgerError(
            "row-codec contracts and module owners are not an exact "
            "identity set")
    if len(rows) != 14 or len({row["sql_identity"] for row in rows}) != 14:
        raise LedgerError("row authority projection must contain exactly 14 rows")
    if set(_ROW_LEGACY_AUTHORITIES) != {
            row["sql_identity"] for row in rows}:
        raise LedgerError(
            "closed legacy row-authority inventory does not match the "
            "14 structured row-codec contracts")
    if any(
        not row["ordered_fields"]
        or not row["legacy_authorities"]
        or (
            row["sql_identity"] != _INDEX_TABLE_SHARD_IDENTITY
            and not row["migrated_consumers"]
        )
        for row in rows
    ):
        raise LedgerError(
            "row authority has an empty field, legacy, or consumer inventory")
    special = [
        row for row in rows
        if row["sql_identity"] == _INDEX_TABLE_SHARD_IDENTITY
    ]
    if len(special) != 1:
        raise LedgerError(
            "row authority must contain one index_table_shard owner")
    _validate_index_table_shard_disposition(special[0])
    return {
        "schema": _ROW_SCHEMA,
        "complete": True,
        "data_gaps": [],
        "counts": {"rows": 14},
        "rows": rows,
    }


def _validate_projection_shape(value: Mapping[str, Any], schema: str) -> None:
    if value.get("schema") != schema:
        raise LedgerError(f"unsupported projection schema, expected {schema}")
    if not isinstance(value.get("data_gaps"), list):
        raise LedgerError(f"{schema} must state its data gaps")
    if value.get("complete") is not True or value.get("data_gaps") != []:
        raise LedgerError(
            f"{schema} must be complete and contain no data gaps")
    expected_counts: dict[str, dict[str, int]] = {
        _FMGR_SCHEMA: {
            "legacy_physical_declarations": 268,
            "legacy_effective_owners": 266,
            "target_owners": 270,
        },
        _RESULT_SCHEMA: {
            "returns_table": 18,
            "named_out": 7,
            "total": 25,
        },
        _ROW_SCHEMA: {"rows": 14},
    }
    if value.get("counts") != expected_counts[schema]:
        raise LedgerError(
            f"{schema} exact counts differ: {value.get('counts')!r}")


def _default_paths(root: Path) -> dict[str, Path]:
    testdata = root / "main" / "bioseq" / "testdata"
    return {
        "schemas": testdata / "doc0388_ledger_schemas.json",
        "delta": testdata / "doc0388_catalog_delta.json",
        "rules": testdata / "doc0388_statement_disposition_rules.json",
        "statement": testdata / "doc0388_statement_disposition_ledger.json",
        "join": testdata / "doc0388_catalog_join_projection.json",
        "fmgr": testdata / "doc0388_fmgr_migration_inventory.json",
        "result": testdata / "doc0388_result_identity_ledger.json",
        "row": testdata / "doc0388_row_authority_migration_ledger.json",
    }


def _artifacts(
    root: Path,
    manifest_path: Path | None,
    git: str,
) -> tuple[
    bytes,
    dict[str, dict[str, Any]],
    dict[str, Any],
]:
    paths = _default_paths(root)
    blob = _git_blob(root, git)
    registry = _load_json(paths["schemas"], "ledger schema registry")
    _validate_schema_registry(registry)
    rules = _load_json(paths["rules"], "statement disposition rules")
    delta = _load_json(paths["delta"], "approved catalog delta")
    manifest = (
        _load_json(manifest_path, "candidate manifest")
        if manifest_path is not None else None
    )
    _, statements = _split_legacy(blob)
    compiled = _compile_rules(rules)
    artifacts = {
        "statement": _statement_ledger(blob, rules, manifest, delta),
        "join": _catalog_join(delta, manifest, statements, compiled),
        "fmgr": _fmgr_inventory(statements, manifest, delta),
        "result": _result_inventory(manifest, statements, delta),
        "row": _row_authority(manifest, root, git),
    }
    for artifact in artifacts.values():
        _validate_artifact_schema(artifact, registry)
    return blob, artifacts, registry


def _generate(
    root: Path,
    manifest_path: Path | None,
    git: str,
) -> None:
    _, artifacts, _ = _artifacts(root, manifest_path, git)
    paths = _default_paths(root)
    for name, value in artifacts.items():
        paths[name].write_bytes(_json_bytes(value))
        print(f"{paths[name]} {_sha256(_json_bytes(value))}")


def _check(root: Path, manifest_path: Path | None, git: str) -> None:
    blob, expected, registry = _artifacts(root, manifest_path, git)
    paths = _default_paths(root)
    manifest_index = _statement_manifest_index(
        _load_json(manifest_path, "candidate manifest")
        if manifest_path is not None else None
    )
    expected_statement_targets = {
        str(row["source_id"]): (
            list(row["semantic_target_identities"]),
            list(row["target_physical_ids"]),
        )
        for row in expected["statement"]["rows"]
        if isinstance(row, dict) and row.get("source_id") != "P000"
    }
    for name, generated in expected.items():
        checked = _load_json(paths[name], f"checked {name} ledger")
        if name == "statement":
            _validate_statement_ledger(
                checked,
                blob,
                manifest_index,
                expected_statement_targets,
            )
        elif name == "join":
            _validate_catalog_join(checked)
        elif name == "fmgr":
            _validate_projection_shape(checked, _FMGR_SCHEMA)
        elif name == "result":
            _validate_projection_shape(checked, _RESULT_SCHEMA)
        elif name == "row":
            _validate_projection_shape(checked, _ROW_SCHEMA)
            special = [
                row for row in checked.get("rows", [])
                if isinstance(row, dict)
                and row.get("sql_identity")
                    == _INDEX_TABLE_SHARD_IDENTITY
            ]
            if len(special) != 1:
                raise LedgerError(
                    "checked row authority lacks exact index_table_shard row")
            _validate_index_table_shard_disposition(special[0])
        _validate_artifact_schema(checked, registry)
        if _json_bytes(checked) != _json_bytes(generated):
            raise LedgerError(
                f"{paths[name]} is stale; regenerate DOC0388 ledgers")
        print(f"{name}: {len(_json_bytes(checked))} bytes "
              f"sha256={_sha256(_json_bytes(checked))}")


def _expect_failure(callback: Any, needle: str) -> None:
    try:
        callback()
    except LedgerError as error:
        if needle not in str(error):
            raise LedgerError(
                f"mutation failed for the wrong reason: {error}") from error
        return
    raise LedgerError(f"mutation unexpectedly passed; expected {needle!r}")


def _self_test(root: Path, manifest_path: Path | None, git: str) -> None:
    paths = _default_paths(root)
    registry_probe = _load_json(
        paths["schemas"], "ledger schema registry")
    _validate_schema_registry(registry_probe)
    mutation = copy.deepcopy(registry_probe)
    mutation["unknown_registry_field"] = True
    _expect_failure(
        lambda: _validate_schema_registry(mutation),
        "differs from the exact",
    )
    mutation = copy.deepcopy(registry_probe)
    mutation["artifacts"][_ROW_SCHEMA][
        "required_row_fields"
    ].remove("migrated_consumers")
    _expect_failure(
        lambda: _validate_schema_registry(mutation),
        "differs from the exact",
    )

    rules_probe = _load_json(
        paths["rules"], "statement disposition rules")
    _compile_rules(rules_probe)
    for provenance_field in (
        "git_revision",
        "repository_path",
        "sha256",
    ):
        mutation = copy.deepcopy(rules_probe)
        mutation["legacy_sql"][provenance_field] += ".mutated"
        _expect_failure(
            lambda mutation=mutation: _compile_rules(mutation),
            "pinned revision/path/hash",
        )
    for container, field in (
        (None, "unknown_top_level"),
        ("default", "unknown_default"),
        ("disposition_rules", "unknown_disposition_rule"),
        ("category_rules", "unknown_category_rule"),
    ):
        mutation = copy.deepcopy(rules_probe)
        if container is None:
            mutation[field] = True
        elif container == "default":
            mutation[container][field] = True
        else:
            mutation[container][0][field] = True
        _expect_failure(
            lambda mutation=mutation: _compile_rules(mutation),
            "unknown",
        )
    mutation = copy.deepcopy(rules_probe)
    mutation["default"]["disposition"] = "archive"
    _expect_failure(
        lambda: _compile_rules(mutation),
        "invalid disposition enum",
    )
    mutation = copy.deepcopy(rules_probe)
    mutation["category_rules"][0][
        "approved_delta_categories"
    ] = [7]
    _expect_failure(
        lambda: _compile_rules(mutation),
        "JSON string array",
    )

    evidence = _index_table_shard_consumer_evidence(root, git)
    production_consumer_probe = _migrated_codec_consumers(
        root, "metadata_age_log_codec")
    if (
        not production_consumer_probe
        or any(
            row.get("source_path")
                == _INDEX_TABLE_SHARD_RUNTIME_SUPPORT
            for row in production_consumer_probe
        )
    ):
        raise LedgerError(
            "test-only row-codec support contaminated the production "
            "consumer inventory")
    no_consumer_row: dict[str, Any] = {
        "sql_identity": _INDEX_TABLE_SHARD_IDENTITY,
        "migrated_consumers": [],
        "consumer_disposition": "no_production_consumer_exists",
        "consumer_evidence": evidence,
    }
    _validate_index_table_shard_disposition(no_consumer_row)
    mutation = copy.deepcopy(no_consumer_row)
    mutation["consumer_disposition"] = "migrated"
    _expect_failure(
        lambda: _validate_index_table_shard_disposition(mutation),
        "must use no_production_consumer_exists",
    )
    mutation = copy.deepcopy(no_consumer_row)
    mutation["consumer_evidence"]["current_source_scan"][
        "production_consumer_count"
    ] = 1
    _expect_failure(
        lambda: _validate_index_table_shard_disposition(mutation),
        "current no-consumer evidence is invalid",
    )
    mutation = copy.deepcopy(no_consumer_row)
    mutation["consumer_evidence"]["historical_source_scans"][0][
        "source_revision"
    ] = "0" * 40
    _expect_failure(
        lambda: _validate_index_table_shard_disposition(mutation),
        "historical evidence revisions differ",
    )
    mutation = copy.deepcopy(no_consumer_row)
    mutation["consumer_evidence"]["runtime_codec_contract"][
        "observed_operations"
    ].remove("catalog_delete")
    _expect_failure(
        lambda: _validate_index_table_shard_disposition(mutation),
        "runtime codec contract is invalid",
    )
    _expect_failure(
        lambda: _index_table_shard_consumer_evidence(
            root,
            git,
            {
                "main/bioseq/backend/pg/"
                "doc0388_index_table_shard_mutation.cppm": (
                    "export module doc0388.mutation;\n"
                    "constexpr auto production_table = "
                    "\"bioseq_internal.index_table_shard\";\n"
                ),
            },
        ),
        "gained a production consumer",
    )
    runtime_support_path = root / _INDEX_TABLE_SHARD_RUNTIME_SUPPORT
    runtime_support_source = runtime_support_path.read_text(
        encoding="utf-8")
    dedicated_runtime = _index_table_shard_runtime_body(
        runtime_support_source)
    generic_runtime = re.search(
        r"\[\[nodiscard\]\]\s+runtime_receipt_entry\s+"
        r"exercise_codec_with_external\s*\([^)]*\)\s*\{"
        r"(?P<body>.*?)"
        r"\n\}\s*\n\s*template\s*<\s*class\s+Codec\s*,"
        r"\s*class\s+NullRowFactory",
        runtime_support_source,
        flags=re.DOTALL,
    )
    if dedicated_runtime is None or generic_runtime is None:
        raise LedgerError(
            "index_table_shard runtime mutation setup did not find the "
            "dedicated and generic runtime functions")

    operation_mutations = (
        (
            "encode",
            generic_runtime,
            r"\bCodec::encode\s*\(",
            "Codec::mutated_encode(",
        ),
        (
            "deform",
            generic_runtime,
            r"\bCodec::deform\s*\(",
            "Codec::mutated_deform(",
        ),
        (
            "update",
            generic_runtime,
            r"\btypename\s+Codec::update\b",
            "typename Codec::mutated_update",
        ),
        (
            "form_tuple",
            generic_runtime,
            r"\.form_tuple\s*\(",
            ".mutated_form_tuple(",
        ),
        (
            "modify",
            generic_runtime,
            r"\.modify\s*\(",
            ".mutated_modify(",
        ),
        (
            "catalog_insert",
            generic_runtime,
            r"\bCatalogTupleInsert\s*\(\s*live_relation\s*,",
            "MutatedCatalogTupleInsert(live_relation,",
        ),
        (
            "catalog_update",
            generic_runtime,
            r"\bCatalogTupleUpdate\s*\(\s*live_relation\s*,",
            "MutatedCatalogTupleUpdate(live_relation,",
        ),
        (
            "catalog_delete",
            generic_runtime,
            r"\bCatalogTupleDelete\s*\(\s*live_relation\s*,",
            "MutatedCatalogTupleDelete(live_relation,",
        ),
        (
            "validate_descriptor",
            generic_runtime,
            (
                r"\bvalidate_descriptor_mutations\s*<\s*Codec\s*>\s*"
                r"\(\s*descriptor\s*\)"
            ),
            "mutated_validate_descriptor<Codec>(descriptor)",
        ),
        (
            "scan_key",
            dedicated_runtime,
            r"\bverify_equal_scan_key\s*<\s*codec\s*,",
            "mutated_verify_equal_scan_key<codec,",
        ),
    )
    for (
        operation,
        body_match,
        mutation_pattern,
        mutation_replacement,
    ) in operation_mutations:
        body = body_match.group("body")
        mutated_body, mutation_count = re.subn(
            mutation_pattern,
            mutation_replacement,
            body,
            flags=re.DOTALL,
        )
        if mutation_count == 0:
            raise LedgerError(
                "index_table_shard runtime mutation setup did not find "
                f"{operation!r}")
        mutated_source = (
            runtime_support_source[:body_match.start("body")]
            + mutated_body
            + runtime_support_source[body_match.end("body"):]
        )
        _expect_failure(
            lambda mutated_source=mutated_source:
                _validated_index_table_shard_runtime_operations(
                    mutated_source),
            "runtime codec operations differ",
        )

    dedicated_body = dedicated_runtime.group("body")
    mutated_body, replacement_count = re.subn(
        (
            r"(\bexercise_codec\s*<\s*codec\s*>\s*\("
            r"\s*descriptor\s*,\s*)relation"
            r"(\s*,\s*\"sql:table\"\s*,)"
        ),
        r"\1nullptr\2",
        dedicated_body,
        count=1,
        flags=re.DOTALL,
    )
    if replacement_count != 1:
        raise LedgerError(
            "index_table_shard live-Relation mutation setup did not "
            "find exactly one dedicated forwarding call")
    mutated_runtime_support = (
        runtime_support_source[:dedicated_runtime.start("body")]
        + mutated_body
        + runtime_support_source[dedicated_runtime.end("body"):]
    )
    _expect_failure(
        lambda: _validated_index_table_shard_runtime_operations(
            mutated_runtime_support),
        "does not forward a live Relation",
    )

    blob, artifacts, registry = _artifacts(root, manifest_path, git)
    if manifest_path is None:
        raise LedgerError(
            "statement target mutations require a candidate manifest")
    manifest = _load_json(manifest_path, "candidate manifest")
    _, legacy_statements = _split_legacy(blob)
    delta_probe = _load_json(paths["delta"], "approved catalog delta")
    expected_statement_targets, manifest_index = _derive_statement_targets(
        legacy_statements,
        _compile_rules(rules_probe),
        manifest,
        delta_probe,
    )
    statement = artifacts["statement"]
    mutation = copy.deepcopy(statement)
    mutation["rows"].pop()
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "exactly 502",
    )
    mutation = copy.deepcopy(statement)
    mutation["counts"]["statement_target_links"] = 494
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "exact counts differ",
    )
    mutation = copy.deepcopy(statement)
    mutation["rows"][2]["source_id"] = "S001"
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "source IDs",
    )
    mutation = copy.deepcopy(statement)
    mutation["rows"][50]["normalized_sha256"] = "0" * 64
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "normalized hash drift",
    )
    mutation = copy.deepcopy(statement)
    removed = next(row for row in mutation["rows"] if row["disposition"] == "remove")
    removed["evidence_refs"] = []
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "removal has no evidence",
    )
    mutation = copy.deepcopy(statement)
    mutation["rows"][8]["replacement_or_absorbing_source_ids"] = ["S009"]
    mutation["rows"][9]["replacement_or_absorbing_source_ids"] = ["S008"]
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "cycle",
    )
    mutation = copy.deepcopy(statement)
    source = next(
        row for row in mutation["rows"] if row["source_id"] == "S001")
    source["semantic_target_identities"] = ["sql:schema:bioseq"]
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "semantic target identity drift",
    )
    mutation = copy.deepcopy(statement)
    source = next(
        row for row in mutation["rows"] if row["source_id"] == "S001")
    source["semantic_target_identities"] = []
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "has no semantic target",
    )
    mutation = copy.deepcopy(statement)
    source = next(
        row for row in mutation["rows"] if row["source_id"] == "S001")
    source["target_physical_ids"] = ["P002"]
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "target physical ID drift",
    )
    mutation = copy.deepcopy(statement)
    source = next(
        row for row in mutation["rows"] if row["source_id"] == "S001")
    source["target_physical_ids"] = []
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "has no semantic target",
    )
    mutation = copy.deepcopy(statement)
    folded = next(
        row for row in mutation["rows"] if row["source_id"] == "S008")
    folded["semantic_target_identities"] = [
        "sql:revoke:schema:bioseq_internal:all:public:ordinary"]
    folded["target_physical_ids"] = ["P035"]
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "semantic target identity drift",
    )
    mutation = copy.deepcopy(statement)
    replaced = next(
        row for row in mutation["rows"] if row["source_id"] == "S019")
    replaced["target_physical_ids"].remove("P023")
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "target physical ID drift",
    )
    mutation = copy.deepcopy(statement)
    removed = next(
        row for row in mutation["rows"] if row["source_id"] == "S003")
    removed["semantic_target_identities"] = [
        "sql:session_setting:bioseq.logging"]
    removed["target_physical_ids"] = ["P001"]
    _expect_failure(
        lambda: _validate_statement_ledger(
            mutation, blob, manifest_index, expected_statement_targets),
        "removal has generated targets",
    )
    mutation = copy.deepcopy(rules_probe)
    acl_fold_rule = next(
        rule for rule in mutation["disposition_rules"]
        if "S008" in rule["source_ids"])
    acl_fold_rule["replacement_or_absorbing_source_ids"] = ["S039"]
    _expect_failure(
        lambda: _derive_statement_targets(
            legacy_statements,
            _compile_rules(mutation),
            manifest,
            delta_probe,
        ),
        "replacement/absorbing mapping differs",
    )
    mutation = copy.deepcopy(rules_probe)
    compatibility_rule = next(
        rule for rule in mutation["disposition_rules"]
        if rule["source_ids"] == ["S019"])
    compatibility_rule["replacement_or_absorbing_source_ids"].remove("S028")
    _expect_failure(
        lambda: _derive_statement_targets(
            legacy_statements,
            _compile_rules(mutation),
            manifest,
            delta_probe,
        ),
        "replacement/absorbing mapping differs",
    )
    mutation = copy.deepcopy(manifest)
    mutation["physical_statements"][0]["baseline_physical_id"] = "P900"
    _expect_failure(
        lambda: _statement_manifest_index(mutation),
        "ordering/identity drift",
    )
    mutation = copy.deepcopy(manifest)
    first_owner = mutation["physical_statements"][0]["owner_identity"]
    first_owner_entity = next(
        entity for entity in mutation["entities"]
        if entity["identity"] == first_owner)
    first_owner_entity["statement_ids"] = []
    _expect_failure(
        lambda: _statement_manifest_index(mutation),
        "physical closure differs",
    )
    join = artifacts["join"]
    mutation = copy.deepcopy(join)
    mutation["derived_input_names"][0]["target_sql_name"] = "mutated"
    _expect_failure(
        lambda: _validate_catalog_join(mutation),
        "does not equal",
    )
    mutation = copy.deepcopy(join)
    mutation["counts"]["derived_routine_symbols"] = 117
    _expect_failure(
        lambda: _validate_catalog_join(mutation),
        "counts differ",
    )
    mutation = copy.deepcopy(statement)
    del mutation["rows"][0]["target_physical_ids"]
    _expect_failure(
        lambda: _validate_artifact_schema(mutation, registry),
        "fields differ",
    )
    mutation = copy.deepcopy(artifacts["result"])
    mutation["rows"][0]["unknown_row_field"] = True
    _expect_failure(
        lambda: _validate_artifact_schema(mutation, registry),
        "fields differ",
    )
    mutation = copy.deepcopy(artifacts["result"])
    mutation["counts"]["returns_table"] = 17
    _expect_failure(
        lambda: _validate_projection_shape(mutation, _RESULT_SCHEMA),
        "exact counts differ",
    )
    mutation = copy.deepcopy(manifest)
    modules = mutation.get("row_codec_modules")
    if not isinstance(modules, list):
        raise LedgerError("candidate manifest has no row-codec modules")
    swappable = next(
        (
            module for module in modules
            if isinstance(module, dict)
            and isinstance(module.get("row_cpp_names"), list)
            and len(module["row_cpp_names"]) >= 2
        ),
        None,
    )
    if swappable is None:
        raise LedgerError(
            "candidate manifest has no multi-row codec module for mutation")
    swappable["row_cpp_names"][0], swappable["row_cpp_names"][1] = (
        swappable["row_cpp_names"][1],
        swappable["row_cpp_names"][0],
    )
    _expect_failure(
        lambda: _row_authority(mutation, root, git),
        "differs from module owner",
    )
    mutation = copy.deepcopy(rules_probe)
    mutation["disposition_rules"].append({
        "source_ids": ["S003"],
        "disposition": "remove",
        "reason": "overlap",
    })
    _expect_failure(lambda: _compile_rules(mutation), "overlapping")
    print("DOC0388 ledger mutation self-tests passed")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("generate", "check", "self-test"))
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
    )
    parser.add_argument(
        "--candidate-manifest",
        type=Path,
        help=(
            "production candidate manifest; required because the 118/six/309, "
            "270-owner, 25-result and 14-row projections are manifest joins"
        ),
    )
    parser.add_argument("--git", default="git")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    root = arguments.repository_root.resolve()
    if arguments.candidate_manifest is None:
        raise LedgerError(
            "--candidate-manifest is required; source-only generation would "
            "leave the 118/six/309 and target-authority projections unverifiable")
    manifest_path = arguments.candidate_manifest.resolve()
    if arguments.mode == "generate":
        _generate(root, manifest_path, arguments.git)
    elif arguments.mode == "check":
        _check(root, manifest_path, arguments.git)
    else:
        _self_test(root, manifest_path, arguments.git)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except LedgerError as error:
        print(f"DOC0388 ledger error: {error}", file=sys.stderr)
        raise SystemExit(2)
