#!/usr/bin/env python3
"""Produce and validate DOC0388 dual-cluster PostgreSQL catalog evidence."""

from __future__ import annotations

import argparse
import collections
import csv
import dataclasses
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid
from typing import Iterable, Mapping, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

from private_postgres import (  # noqa: E402
    ClusterInputs,
    UniquePrivateCluster,
)


class CatalogParityError(RuntimeError):
    """The catalog evidence is incomplete, ambiguous, or not approved."""


@dataclasses.dataclass(frozen=True, order=True)
class SnapshotKey:
    catalog: str
    identity: str
    ordinal: int


@dataclasses.dataclass(frozen=True)
class Snapshot:
    rows: Mapping[SnapshotKey, object]
    counts: Mapping[str, int]


_BUNDLE_SCHEMA = "bioseq_doc0388_catalog_evidence/v1"
_DELTA_SCHEMA = "bioseq_doc0388_catalog_delta/v1"
_EXPECTED_LEGACY_SHA256 = (
    "0eb3aa6da75d80fc07c92dc1f6213ef27ddf1da7f4eaed33e6c69e6c8a7dfa71"
)
_EXPECTED_LEGACY_REVISION = (
    "e5e8a939a9e007c5640fc29da4302a9c92701dcf"
)
_EXPECTED_DELTA_SEMANTIC_SHA256 = (
    "716e0672dcaa01b5f1e6c5d885c4374c951fc2d6b8388f7da41b9cfb860c7380"
)
_EXPECTED_GUC_LEDGER_SHA256 = (
    "684823cdde26f2a8647cdaa1e8332ec74ea56c2f11a4e0970254218ab8bb88df"
)
_UNUSUAL_SCHEMA = "doc0388_relocated_7f3"
_RELOCATION_PROBE_SCHEMA = "doc0388_relocation_probe_4c9"
_RELOCATION_TARGET_SEQUENCE = (
    "ACGTTGCATGTCGCATGATGCATGAGAGCTTAGCGTACGATGCTAGCATCGTACGGTACCTAG"
)
_RELOCATION_NONMATCH_SEQUENCE = "T" * len(_RELOCATION_TARGET_SEQUENCE)
_BPCHAR_LEGACY_SYMBOLS = frozenset({
    "vchareq",
    "vcharne",
    "vchargt",
    "vcharlt",
    "vcharle",
    "vcharge",
})
_IGNORED_BUNDLE_FILES = frozenset({"evidence.json", "bundle.sha256"})
_REQUIRED_SNAPSHOT_CATALOGS = frozenset({
    "pg_namespace",
    "pg_class",
    "pg_attribute",
    "pg_constraint",
    "pg_index",
    "pg_sequence",
    "pg_proc",
    "pg_type",
    "pg_rewrite",
    "pg_cast",
    "pg_operator",
    "pg_opclass",
    "pg_am",
    "pg_aggregate",
    "pg_extension",
    "pg_depend",
    "pg_description",
    "pg_shdescription",
    "acl",
    "effective_privilege",
    "extension_membership",
    "guc",
})
_REQUIRED_SNAPSHOT_FIELDS: dict[str, tuple[str, ...]] = {
    "pg_namespace": ("owner", "acl_is_null"),
    "pg_class": (
        "relkind", "persistence", "owner", "access_method",
        "replica_identity", "row_security", "force_row_security",
        "options", "view_definition", "acl_is_null",
    ),
    "pg_attribute": (
        "ordinal", "type", "not_null", "has_default", "default",
        "identity", "generated", "storage", "compression",
        "acl_is_null", "collation",
    ),
    "pg_constraint": (
        "type", "deferrable", "deferred", "validated", "definition",
    ),
    "pg_index": (
        "table", "unique", "primary", "exclusion", "immediate",
        "valid", "ready", "live", "replica_identity", "definition",
    ),
    "pg_sequence": (
        "type", "start", "increment", "max", "min", "cache", "cycle",
    ),
    "pg_proc": (
        "kind", "language", "owner", "result", "proargnames",
        "proargmodes", "number_arguments", "number_defaults",
        "argument_types", "default_expressions", "variadic_type",
        "volatility", "strict", "security_definer", "leakproof",
        "parallel", "cost", "rows", "support", "configuration",
        "probin", "prosrc", "acl_is_null",
    ),
    "pg_type": (
        "owner", "kind", "category", "preferred", "defined",
        "delimiter", "length", "by_value", "alignment", "storage",
        "not_null", "input", "output", "receive", "send", "typmod_in",
        "typmod_out", "analyze", "subscript", "element", "array",
        "base", "collation", "default", "default_binary", "acl_is_null",
    ),
    "pg_rewrite": ("event", "instead", "definition"),
    "pg_cast": ("context", "method", "function"),
    "pg_operator": (
        "kind", "can_merge", "can_hash", "result", "function",
        "commutator", "negator", "restriction", "join",
    ),
    "pg_opclass": (
        "family", "input_type", "default", "key_type",
        "operators", "functions",
    ),
    "pg_am": ("type", "handler"),
    "pg_aggregate": (
        "kind", "arguments", "transition", "final", "combine", "serial",
        "deserial", "moving_transition", "moving_inverse",
        "moving_final", "transition_type", "transition_space",
        "initial_condition", "moving_transition_type",
        "moving_transition_space", "moving_initial_condition",
        "final_extra", "moving_final_extra", "final_modify",
        "moving_final_modify", "sort_operator",
    ),
    "pg_extension": (
        "name", "owner", "namespace", "relocatable", "version",
        "config", "conditions",
    ),
    "pg_depend": (
        "object_type", "object_identity", "reference_type",
        "reference_identity", "dependency_type",
    ),
    "pg_description": ("object_type", "description"),
    "pg_shdescription": ("description",),
    "acl": ("grantor", "grantable"),
    "effective_privilege": ("probe_role", "granted"),
    "extension_membership": ("object_type", "dependency_type"),
    "guc": (
        "setting", "unit", "category", "short_description",
        "extra_description", "context", "type", "source", "minimum",
        "maximum", "enum_values", "boot_value", "reset_value",
        "pending_restart",
    ),
}
_KNOWN_CATALOG_CHANGE_CATEGORIES = frozenset({
    "bpchar_vector_repair",
    "dead_routine_removal",
    "derived_input_name",
    "derived_routine_symbol",
    "doxygen_catalog_comment_projection",
    "native_routine_replacement",
    "standardized_routine_identity",
})
_GUC_LEDGER_COLUMNS = (
    "registration_ordinal",
    "source_owner",
    "name",
    "group",
    "member",
    "kind",
    "storage_kind",
    "storage",
    "default_expression",
    "minimum_expression",
    "maximum_expression",
    "context",
    "flags",
    "unit",
    "hooks",
    "feature_predicate",
    "profiles",
    "baseline_legacy_description_sha256",
    "default_semantic",
    "enum_rows_name_wire_hidden_feature_alias_of",
    "enum_omissions_name_wire",
)
_GUC_MANIFEST_AUTHORITY_COLUMNS = tuple(
    column
    for column in _GUC_LEDGER_COLUMNS
    if column != "baseline_legacy_description_sha256"
)
_GUC_INT32_MIN = -(1 << 31)
_GUC_INT32_MAX = (1 << 31) - 1
# Closed numeric authority for the legacy spellings retained by the sealed
# ledger. Deriving these values from candidate source would make the live
# pg_settings comparison self-referential.
_GUC_REVIEWED_INTEGER_CONSTANTS = {
    "CHUNK_ROW_COUNT_MAXIMUM": 100_000_000,
    "CHUNK_ROW_COUNT_MINIMUM": 1_000,
    "COMPRESSION_LEVEL_MAX": 19,
    "COMPRESSION_LEVEL_MIN": 1,
    "DEFAULT_CHUNK_ROW_COUNT": 10_000,
    "DEFAULT_STRIPE_ROW_COUNT": 150_000,
    "INT_MAX": _GUC_INT32_MAX,
    "STRIPE_ROW_COUNT_MAXIMUM": 100_000_000,
    "STRIPE_ROW_COUNT_MINIMUM": 1_000,
}
_GUC_DECIMAL_INTEGER = re.compile(
    r"(?P<sign>[+-]?)"
    r"(?P<digits>0|[1-9](?:'?[0-9])*)"
    r"(?P<suffix>"
    r"(?:[uU](?:ll|LL|[lL])?|(?:ll|LL|[lL])[uU]?)?"
    r")"
)
_GUC_NAMED_INTEGER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_GUC_DECIMAL_REAL = re.compile(
    r"(?P<sign>[+-]?)"
    r"(?:"
    r"(?P<integer>[0-9]+)(?:\.(?P<fraction>[0-9]*))?"
    r"|"
    r"\.(?P<leading_fraction>[0-9]+)"
    r")"
)


def _regular_file(value: str | Path, label: str) -> Path:
    path = Path(value).expanduser()
    try:
        metadata = path.lstat()
    except OSError as error:
        raise CatalogParityError(f"{label} is missing: {path}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise CatalogParityError(
            f"{label} must be a regular non-symlink file: {path}")
    return path.resolve(strict=True)


def _executable(value: str | Path, label: str) -> Path:
    path = Path(value).expanduser()
    try:
        resolved = path.resolve(strict=True)
        metadata = resolved.stat()
    except OSError as error:
        raise CatalogParityError(f"{label} is missing: {path}") from error
    if not stat.S_ISREG(metadata.st_mode):
        raise CatalogParityError(
            f"{label} must resolve to a regular file: {path}")
    if not os.access(resolved, os.X_OK):
        raise CatalogParityError(f"{label} is not executable: {resolved}")
    return resolved


def _real_directory(
    value: str | Path,
    label: str,
    *,
    create: bool,
) -> Path:
    path = Path(value).expanduser()
    if create:
        path.mkdir(parents=True, exist_ok=True)
    try:
        metadata = path.lstat()
    except OSError as error:
        raise CatalogParityError(f"{label} is missing: {path}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise CatalogParityError(
            f"{label} must be a real non-symlink directory: {path}")
    return path.resolve(strict=True)


def _canonical_repository_relative_path(
    value: str,
    label: str,
) -> Path:
    relative = Path(value)
    if (
        not value
        or "\x00" in value
        or relative.is_absolute()
        or value != relative.as_posix()
        or "\\" in value
        or relative == Path(".")
        or ".." in relative.parts
    ):
        raise CatalogParityError(
            f"{label} is not a canonical repository-relative path: "
            f"{value!r}")
    return relative


def _canonical_repository_source(
    repository_root: Path,
    relative: Path,
    label: str,
) -> Path:
    candidate = (repository_root / relative).absolute()
    try:
        metadata = candidate.lstat()
    except OSError as error:
        raise CatalogParityError(
            f"{label} is missing: {candidate}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise CatalogParityError(
            f"{label} must be a regular non-symlink file: {candidate}")
    if metadata.st_nlink != 1:
        raise CatalogParityError(f"{label} is hard-linked: {candidate}")
    canonical = candidate.resolve(strict=True)
    if canonical != candidate:
        raise CatalogParityError(
            f"{label} path is not canonical: {candidate}")
    try:
        canonical_relative = canonical.relative_to(repository_root)
    except ValueError as error:
        raise CatalogParityError(
            f"{label} escapes the repository: {canonical}") from error
    if canonical_relative != relative:
        raise CatalogParityError(
            f"{label} differs from its canonical repository path")
    return canonical


def _fresh_bundle_destination(
    output_root: Path,
    bundle_id: str,
) -> Path:
    if re.fullmatch(r"[0-9a-f]{64}", bundle_id) is None:
        raise CatalogParityError(
            "catalog evidence bundle id is not lowercase SHA-256")
    destination = output_root / bundle_id
    if destination.exists() or destination.is_symlink():
        raise CatalogParityError(
            "fresh catalog run nonce collided with existing evidence: "
            f"{destination}")
    return destination


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _canonical_json(document: object) -> bytes:
    return (
        json.dumps(
            document,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n"
    ).encode("utf-8")


def _is_canonical_uuid4_hex(value: object) -> bool:
    if (
        not isinstance(value, str)
        or re.fullmatch(r"[0-9a-f]{32}", value) is None
    ):
        return False
    try:
        parsed = uuid.UUID(hex=value)
    except ValueError:
        return False
    return (
        parsed.hex == value
        and parsed.version == 4
        and parsed.variant == uuid.RFC_4122
    )


def _write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    metadata = path.parent.lstat()
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise CatalogParityError(
            f"output parent is not a real directory: {path.parent}")
    if path.exists() or path.is_symlink():
        raise CatalogParityError(f"refusing to overwrite evidence file: {path}")
    path.write_bytes(data)


def _write_json(path: Path, document: object) -> None:
    _write_bytes(path, _canonical_json(document))


def _copy_file(source: Path, destination: Path) -> None:
    _write_bytes(destination, source.read_bytes())


def _load_json(path: Path, label: str) -> dict[str, object]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CatalogParityError(f"cannot read {label}: {path}: {error}") from error
    if not isinstance(document, dict):
        raise CatalogParityError(f"{label} must be a JSON object: {path}")
    return document


def _run(
    command: Sequence[str],
    *,
    timeout: int,
    env: Mapping[str, str] | None = None,
    input_bytes: bytes | None = None,
    log: Path | None = None,
) -> bytes:
    try:
        result = subprocess.run(
            list(command),
            input=input_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=None if env is None else dict(env),
            timeout=timeout,
            check=False,
            start_new_session=True,
        )
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b""
        if log is not None:
            _write_bytes(log, output)
        raise CatalogParityError(
            f"command timed out after {timeout}s: {' '.join(command)}") from error
    if log is not None:
        _write_bytes(log, result.stdout)
    if result.returncode != 0:
        raise CatalogParityError(
            f"command failed with exit code {result.returncode}: "
            f"{' '.join(command)}\n"
            f"{result.stdout.decode('utf-8', errors='replace')}")
    return result.stdout


def _git_blob(
    git: Path,
    repository_root: Path,
    revision: str,
    repository_path: str,
    *,
    timeout: int,
) -> bytes:
    if (
        not repository_path
        or repository_path.startswith("/")
        or ".." in Path(repository_path).parts
    ):
        raise CatalogParityError(
            f"legacy repository path is unsafe: {repository_path!r}")
    return _run(
        [
            str(git),
            "-C",
            str(repository_root),
            "show",
            f"{revision}:{repository_path}",
        ],
        timeout=timeout,
    )


def _validate_delta(
    delta: dict[str, object],
    legacy_bytes: bytes,
) -> dict[str, object]:
    if (
        _sha256_bytes(_canonical_json(delta))
        != _EXPECTED_DELTA_SEMANTIC_SHA256
    ):
        raise CatalogParityError(
            "approved catalog delta differs from the closed semantic set")
    if delta.get("schema") != _DELTA_SCHEMA:
        raise CatalogParityError("approved catalog delta has the wrong schema")
    expected_delta_keys = {
        "schema",
        "legacy_sql",
        "catalog_contract",
        "identity_renames",
        "removed_dead_routines",
        "native_routine_replacements",
        "catalog_changes",
        "routine_symbol_changes",
        "input_name_changes",
        "catalog_comment_changes",
    }
    if set(delta) != expected_delta_keys:
        raise CatalogParityError(
            "approved catalog delta has unexpected top-level fields")
    legacy = delta.get("legacy_sql")
    contract = delta.get("catalog_contract")
    if not isinstance(legacy, dict) or not isinstance(contract, dict):
        raise CatalogParityError("approved catalog delta is incomplete")
    expected_contract_keys = {
        "postgresql_major",
        "candidate_c_routine_identity_count",
        "candidate_c_link_symbol_count",
        "candidate_c_finfo_symbol_count",
        "legacy_to_candidate_prosrc_change_count",
        "derived_routine_symbol_change_count",
        "derived_input_name_routine_count",
        "derived_input_name_position_count",
        "candidate_catalog_comment_count",
        "candidate_am_dependency_count",
        "candidate_public_manifest_identity_count",
        "identity_rename_count",
        "approved_catalog_change_count",
        "approved_catalog_change_counts_by_category",
    }
    if set(contract) != expected_contract_keys:
        raise CatalogParityError(
            "approved catalog contract has unexpected fields")
    if (
        legacy.get("git_revision") != _EXPECTED_LEGACY_REVISION
        or legacy.get("repository_path")
        != "main/bioseq/sql/bioseq--1.0.sql"
    ):
        raise CatalogParityError(
            "approved catalog delta changed the immutable legacy selector")
    expected_hash = legacy.get("sha256")
    actual_hash = _sha256_bytes(legacy_bytes)
    if (
        expected_hash != _EXPECTED_LEGACY_SHA256
        or actual_hash != _EXPECTED_LEGACY_SHA256
    ):
        raise CatalogParityError(
            "immutable legacy SQL SHA-256 drifted: "
            f"expected={_EXPECTED_LEGACY_SHA256} actual={actual_hash}")
    _exact_delta_rows(
        delta,
        "routine_symbol_changes",
        ("identity", "legacy", "candidate"),
        expected_count=118,
    )
    _exact_delta_rows(
        delta,
        "input_name_changes",
        (
            "identity",
            "ordinal",
            "argument_type",
            "legacy",
            "candidate",
        ),
        expected_count=309,
    )
    _exact_delta_rows(
        delta,
        "catalog_comment_changes",
        ("identity", "legacy", "candidate"),
        expected_count=48,
    )
    _catalog_change_rows(delta, contract)
    return contract


def _validate_presence_envelope(
    value: object,
    *,
    label: str,
) -> dict[str, object]:
    if not isinstance(value, dict) or not isinstance(
        value.get("present"), bool
    ):
        raise CatalogParityError(f"{label} is not a presence envelope")
    if value["present"]:
        if set(value) != {"present", "value"}:
            raise CatalogParityError(
                f"{label} present envelope has the wrong fields")
    elif set(value) != {"present"}:
        raise CatalogParityError(
            f"{label} absent envelope has the wrong fields")
    return dict(value)


def _catalog_change_rows(
    delta: Mapping[str, object],
    contract: Mapping[str, object],
) -> tuple[dict[str, object], ...]:
    """Validate the closed, exact all-catalog-difference authority."""
    raw = delta.get("catalog_changes")
    if not isinstance(raw, list):
        raise CatalogParityError(
            "approved catalog delta has no exact catalog_changes ledger")
    required = {
        "category",
        "catalog",
        "identity",
        "ordinal",
        "property",
        "legacy",
        "candidate",
    }
    rows: list[dict[str, object]] = []
    locators: list[tuple[str, str, int, str]] = []
    counts: collections.Counter[str] = collections.Counter()
    for row_number, value in enumerate(raw, start=1):
        if not isinstance(value, dict) or set(value) != required:
            raise CatalogParityError(
                f"approved catalog_changes row {row_number} "
                "has the wrong columns")
        category = value.get("category")
        catalog = value.get("catalog")
        identity = value.get("identity")
        ordinal = value.get("ordinal")
        property_name = value.get("property")
        if (
            category not in _KNOWN_CATALOG_CHANGE_CATEGORIES
            or catalog not in _REQUIRED_SNAPSHOT_CATALOGS
            or not isinstance(identity, str)
            or not identity
            or not isinstance(ordinal, int)
            or isinstance(ordinal, bool)
            or ordinal < 1
            or not isinstance(property_name, str)
            or not property_name
        ):
            raise CatalogParityError(
                f"approved catalog_changes row {row_number} is malformed")
        legacy = _validate_presence_envelope(
            value.get("legacy"),
            label=f"catalog_changes row {row_number} legacy",
        )
        candidate = _validate_presence_envelope(
            value.get("candidate"),
            label=f"catalog_changes row {row_number} candidate",
        )
        if legacy == candidate:
            raise CatalogParityError(
                f"approved catalog_changes row {row_number} "
                "does not describe a change")
        locator = (str(catalog), identity, ordinal, property_name)
        if locator in locators:
            raise CatalogParityError(
                "approved catalog_changes ledger contains a duplicate "
                "catalog/identity/ordinal/property locator")
        locators.append(locator)
        counts[str(category)] += 1
        rows.append({
            "category": category,
            "catalog": catalog,
            "identity": identity,
            "ordinal": ordinal,
            "property": property_name,
            "legacy": legacy,
            "candidate": candidate,
        })
    if locators != sorted(locators):
        raise CatalogParityError(
            "approved catalog_changes ledger is not sorted by its "
            "catalog/identity/ordinal/property locator")
    expected_total = contract.get("approved_catalog_change_count")
    expected_counts = contract.get(
        "approved_catalog_change_counts_by_category")
    if (
        not isinstance(expected_total, int)
        or isinstance(expected_total, bool)
        or expected_total != len(rows)
        or not isinstance(expected_counts, dict)
        or expected_counts != dict(sorted(counts.items()))
    ):
        raise CatalogParityError(
            "approved catalog change count contract differs from the "
            "exact ledger")
    return tuple(rows)


def _match_catalog_change_authority(
    *,
    authority: Mapping[
        tuple[str, str, int, str],
        Mapping[str, object],
    ],
    consumed: set[tuple[str, str, int, str]],
    key: SnapshotKey,
    property_name: str,
    legacy_value: object,
    candidate_value: object,
    observed_category: str | None,
) -> str | None:
    """Consume one exact presence-aware authority row, or reject the match."""
    locator = (
        key.catalog,
        key.identity,
        key.ordinal,
        property_name,
    )
    legacy_envelope = (
        {"present": False}
        if legacy_value is dataclasses.MISSING
        else {"present": True, "value": legacy_value}
    )
    candidate_envelope = (
        {"present": False}
        if candidate_value is dataclasses.MISSING
        else {"present": True, "value": candidate_value}
    )
    row = authority.get(locator)
    if (
        row is None
        or row.get("legacy") != legacy_envelope
        or row.get("candidate") != candidate_envelope
        or (
            observed_category is not None
            and row.get("category") != observed_category
        )
        or locator in consumed
    ):
        return None
    consumed.add(locator)
    category = row.get("category")
    if not isinstance(category, str):
        raise CatalogParityError(
            "matched catalog change authority has no exact category")
    return category


def _untrusted_catalog_change_candidates(
    rows: Iterable[Mapping[str, object]],
) -> tuple[dict[str, object], ...]:
    """Render observed, non-summary differences for manual authority review."""
    candidates: list[dict[str, object]] = []
    seen: set[tuple[str, str, int, str]] = set()
    for row in rows:
        catalog = row.get("catalog")
        identity = row.get("identity")
        ordinal = row.get("ordinal")
        property_name = row.get("property")
        legacy = row.get("legacy")
        candidate = row.get("candidate")
        if catalog not in _REQUIRED_SNAPSHOT_CATALOGS:
            continue
        if (
            not isinstance(identity, str)
            or not identity
            or not isinstance(ordinal, int)
            or isinstance(ordinal, bool)
            or ordinal < 1
            or not isinstance(property_name, str)
            or not property_name
            or not isinstance(legacy, dict)
            or not isinstance(candidate, dict)
        ):
            raise CatalogParityError(
                "untrusted catalog diagnostic received a malformed "
                "observed difference")
        locator = (str(catalog), identity, ordinal, property_name)
        if locator in seen:
            raise CatalogParityError(
                "untrusted catalog diagnostic received a duplicate "
                "difference locator")
        seen.add(locator)
        candidates.append({
            "observed_category": row.get("observed_category"),
            "catalog": catalog,
            "identity": identity,
            "ordinal": ordinal,
            "property": property_name,
            "legacy": _validate_presence_envelope(
                legacy,
                label="untrusted diagnostic legacy",
            ),
            "candidate": _validate_presence_envelope(
                candidate,
                label="untrusted diagnostic candidate",
            ),
        })
    candidates.sort(key=lambda row: (
        str(row["catalog"]),
        str(row["identity"]),
        int(row["ordinal"]),
        str(row["property"]),
    ))
    return tuple(candidates)


def _exact_delta_rows(
    delta: Mapping[str, object],
    field: str,
    columns: Sequence[str],
    *,
    expected_count: int | None = None,
) -> tuple[dict[str, object], ...]:
    """Validate and return one duplicate-free exact approved-delta ledger."""
    raw = delta.get(field)
    if not isinstance(raw, list):
        raise CatalogParityError(
            f"approved catalog delta has no exact {field} ledger")
    if expected_count is not None and len(raw) != expected_count:
        raise CatalogParityError(
            f"approved {field} ledger has {len(raw)} rows, "
            f"expected {expected_count}")
    rows: list[dict[str, object]] = []
    seen: set[tuple[object, ...]] = set()
    authority_keys: set[tuple[object, ...]] = set()
    required = frozenset(columns)
    for ordinal, value in enumerate(raw, start=1):
        if not isinstance(value, dict) or frozenset(value) != required:
            raise CatalogParityError(
                f"approved {field} row {ordinal} has the wrong columns")
        if not isinstance(value.get("identity"), str):
            raise CatalogParityError(
                f"approved {field} row {ordinal} has no identity")
        if field == "input_name_changes":
            if (
                not isinstance(value.get("ordinal"), int)
                or value["ordinal"] < 1
                or not isinstance(value.get("argument_type"), str)
                or value.get("candidate") in (None, "")
            ):
                raise CatalogParityError(
                    f"approved {field} row {ordinal} is malformed")
        elif not isinstance(value.get("candidate"), str):
            raise CatalogParityError(
                f"approved {field} row {ordinal} is malformed")
        key = tuple(value[column] for column in columns)
        if key in seen:
            raise CatalogParityError(
                f"approved {field} ledger contains a duplicate row")
        seen.add(key)
        authority_key = (
            (value["identity"], value["ordinal"])
            if field == "input_name_changes"
            else (value["identity"],)
        )
        if authority_key in authority_keys:
            raise CatalogParityError(
                f"approved {field} ledger has competing authority rows")
        authority_keys.add(authority_key)
        rows.append(dict(value))
    return tuple(rows)


def _validate_manifest(
    manifest: dict[str, object],
    candidate_sql: Path,
    contract: Mapping[str, object],
) -> None:
    expected_fields = {
        "c_routine_identity_count": "candidate_c_routine_identity_count",
        "c_link_symbol_inventory_count": "candidate_c_link_symbol_count",
        "c_finfo_symbol_inventory_count": "candidate_c_finfo_symbol_count",
        "public_entity_count": "candidate_public_manifest_identity_count",
    }
    for manifest_field, contract_field in expected_fields.items():
        if manifest.get(manifest_field) != contract.get(contract_field):
            raise CatalogParityError(
                f"candidate manifest {manifest_field}="
                f"{manifest.get(manifest_field)!r}, expected "
                f"{contract.get(contract_field)!r}")
    if manifest.get("script_sha256") != _sha256(candidate_sql):
        raise CatalogParityError(
            "candidate SQL bytes do not match the candidate manifest")
    public_stable = manifest.get("public_stable_identities")
    public_installed = manifest.get("public_installed_identities")
    expected_public = contract.get("candidate_public_manifest_identity_count")
    if (
        not isinstance(public_stable, list)
        or not isinstance(public_installed, list)
        or len(public_stable) != expected_public
        or len(public_installed) != expected_public
        or len(set(public_stable)) != len(public_stable)
    ):
        raise CatalogParityError(
            "candidate public manifest identity inventory is incomplete")
    if manifest.get("natural_authority_complete") is not True:
        raise CatalogParityError(
            "candidate manifest does not have complete natural authority")


def _guc_ledger_records(
    ledger: Mapping[str, object],
    active_profile: str,
) -> tuple[dict[str, object], ...]:
    """Return the independently reviewed GUC rows active in one profile."""
    columns = ledger.get("columns")
    raw_rows = ledger.get("rows")
    profiles = ledger.get("profiles")
    expected_root_keys = {
        "schema_version",
        "description_authority",
        "baseline_legacy_description_hash_is_non_authoritative",
        "ordinal_base",
        "source_owner_order",
        "profiles",
        "columns",
        "rows",
    }
    if (
        set(ledger) != expected_root_keys
        or ledger.get("schema_version") != 1
        or ledger.get("description_authority") != "cleaned_doxygen_only"
        or ledger.get(
            "baseline_legacy_description_hash_is_non_authoritative"
        ) is not True
        or ledger.get("ordinal_base") != 0
        or ledger.get("source_owner_order")
        != ["column", "custom_scan", "batch_sort_scan"]
        or columns != list(_GUC_LEDGER_COLUMNS)
        or not isinstance(raw_rows, list)
        or not isinstance(profiles, dict)
        or set(profiles) != {"production", "test_indexes"}
        or active_profile not in {"production", "test_indexes"}
    ):
        raise CatalogParityError("independent GUC parity ledger is malformed")
    assert isinstance(columns, list)
    assert isinstance(profiles, dict)
    for profile_name, descriptor in profiles.items():
        if (
            not isinstance(descriptor, dict)
            or set(descriptor) != {"row_count"}
            or not isinstance(descriptor.get("row_count"), int)
            or isinstance(descriptor.get("row_count"), bool)
        ):
            raise CatalogParityError(
                f"GUC parity ledger profile {profile_name} is malformed")
    records: list[dict[str, object]] = []
    all_ordinals: list[int] = []
    all_names: set[str] = set()
    all_storages: set[str] = set()
    observed_profile_counts: collections.Counter[str] = collections.Counter()
    storage_by_kind = {
        "bool": "bool",
        "integer": "int32",
        "real": "double",
        "string": "string",
        "enum": "int32_enum",
    }
    for ledger_ordinal, raw in enumerate(raw_rows, start=1):
        if not isinstance(raw, list) or len(raw) != len(columns):
            raise CatalogParityError(
                f"GUC parity ledger row {ledger_ordinal} has wrong width")
        record = dict(zip(columns, raw))
        row_profiles = record.get("profiles")
        hooks = record.get("hooks")
        flags = record.get("flags")
        enum_rows = record.get(
            "enum_rows_name_wire_hidden_feature_alias_of")
        omissions = record.get("enum_omissions_name_wire")
        ordinal = record.get("registration_ordinal")
        name = record.get("name")
        group = record.get("group")
        member = record.get("member")
        kind = record.get("kind")
        storage_kind = record.get("storage_kind")
        legacy_storage = record.get("storage")
        baseline_hash = record.get(
            "baseline_legacy_description_sha256")
        if (
            not isinstance(ordinal, int)
            or isinstance(ordinal, bool)
            or ordinal < 0
            or not isinstance(name, str)
            or not isinstance(group, str)
            or not isinstance(member, str)
            or name != f"{group}.{member}"
            or kind not in storage_by_kind
            or storage_kind != storage_by_kind.get(kind)
            or not isinstance(legacy_storage, str)
            or not legacy_storage
            or record.get("source_owner")
            not in {"column", "custom_scan", "batch_sort_scan"}
            or not isinstance(record.get("default_expression"), str)
            or (
                record.get("minimum_expression") is not None
                and not isinstance(
                    record.get("minimum_expression"), str)
            )
            or (
                record.get("maximum_expression") is not None
                and not isinstance(
                    record.get("maximum_expression"), str)
            )
            or not isinstance(record.get("context"), str)
            or not isinstance(flags, list)
            or not all(isinstance(value, str) for value in flags)
            or (
                record.get("unit") is not None
                and not isinstance(record.get("unit"), str)
            )
            or not isinstance(hooks, list)
            or hooks != [None, None, None]
            or not isinstance(record.get("feature_predicate"), str)
            or not isinstance(row_profiles, list)
            or not all(isinstance(value, str) for value in row_profiles)
            or len(row_profiles) != len(set(row_profiles))
            or any(
                value not in {"production", "test_indexes"}
                for value in row_profiles
            )
            or not isinstance(baseline_hash, str)
            or re.fullmatch(r"[0-9a-f]{64}", baseline_hash) is None
            or (
                record.get("default_semantic") is not None
                and not isinstance(record.get("default_semantic"), str)
            )
            or not isinstance(enum_rows, list)
            or not isinstance(omissions, list)
        ):
            raise CatalogParityError(
                f"GUC parity ledger row {ledger_ordinal} is malformed")
        if any(
            not isinstance(value, list)
            or len(value) != 5
            or not isinstance(value[0], str)
            or not isinstance(value[1], str)
            or value[2] not in {0, 1}
            or not isinstance(value[3], str)
            or (value[4] is not None and not isinstance(value[4], str))
            for value in enum_rows
        ) or any(
            not isinstance(value, list)
            or len(value) != 2
            or not all(isinstance(item, str) for item in value)
            for value in omissions
        ):
            raise CatalogParityError(
                f"GUC parity ledger row {ledger_ordinal} "
                "has malformed enum authority")
        if (
            kind == "enum"
            and (
                not enum_rows
                or not isinstance(record.get("default_semantic"), str)
                or (
                    record.get("default_semantic")
                    not in {
                        value[0]
                        for value in enum_rows
                        if isinstance(value, list) and len(value) == 5
                    }
                    and not (
                        name == "bioseq.compression"
                        and record.get("default_semantic")
                        == "best_compiled_algorithm"
                    )
                )
            )
        ) or (
            kind != "enum"
            and (
                enum_rows
                or omissions
                or record.get("default_semantic") is not None
            )
        ):
            raise CatalogParityError(
                f"GUC parity ledger row {ledger_ordinal} has enum "
                "authority inconsistent with its kind")
        enum_names = {
            str(value[0])
            for value in enum_rows
            if isinstance(value, list) and len(value) == 5
        }
        if any(
            value[4] is not None and value[4] not in enum_names
            for value in enum_rows
        ):
            raise CatalogParityError(
                f"GUC parity ledger row {ledger_ordinal} has an "
                "unresolved enum alias")
        assert isinstance(ordinal, int)
        assert isinstance(name, str)
        assert isinstance(legacy_storage, str)
        expected_profiles = (
            ["test_indexes"]
            if name == "bioseq.test_force_paper_index_failure"
            else ["production", "test_indexes"]
        )
        if row_profiles != expected_profiles:
            raise CatalogParityError(
                f"GUC parity ledger row {ledger_ordinal} has an "
                "unexpected profile selection")
        all_ordinals.append(ordinal)
        if name in all_names or legacy_storage in all_storages:
            raise CatalogParityError(
                "GUC parity ledger repeats a name or legacy backing cell")
        all_names.add(name)
        all_storages.add(legacy_storage)
        for profile_name in row_profiles:
            observed_profile_counts[profile_name] += 1
        if active_profile in row_profiles:
            records.append(record)
    if all_ordinals != list(range(len(raw_rows))):
        raise CatalogParityError(
            "GUC parity ledger ordinals are not dense and ordered")
    for profile_name, descriptor in profiles.items():
        assert isinstance(descriptor, dict)
        if descriptor["row_count"] != observed_profile_counts[profile_name]:
            raise CatalogParityError(
                f"GUC parity ledger {profile_name} row count is inconsistent")
    records.sort(key=lambda row: int(row["registration_ordinal"]))
    profile_contract = profiles.get(active_profile)
    if (
        not isinstance(profile_contract, dict)
        or profile_contract.get("row_count") != len(records)
    ):
        raise CatalogParityError(
            f"GUC parity ledger {active_profile} row count is inconsistent")
    if len({str(row["name"]) for row in records}) != len(records):
        raise CatalogParityError(
            "active GUC ledger has duplicate setting names")
    return tuple(records)


def _guc_integer_atom(expression: str, label: str) -> int:
    literal = _GUC_DECIMAL_INTEGER.fullmatch(expression)
    if literal is not None:
        suffix = literal.group("suffix")
        if literal.group("sign") == "-" and "u" in suffix.lower():
            raise CatalogParityError(
                f"{label} uses an ambiguous negative unsigned literal: "
                f"{expression!r}")
        value = int(
            literal.group("sign")
            + literal.group("digits").replace("'", ""),
            10,
        )
    elif _GUC_NAMED_INTEGER.fullmatch(expression) is not None:
        if expression not in _GUC_REVIEWED_INTEGER_CONSTANTS:
            raise CatalogParityError(
                f"{label} uses an unreviewed named integer constant: "
                f"{expression}")
        value = _GUC_REVIEWED_INTEGER_CONSTANTS[expression]
    else:
        raise CatalogParityError(
            f"{label} is not a supported integer constant atom: "
            f"{expression!r}")
    if value < _GUC_INT32_MIN or value > _GUC_INT32_MAX:
        raise CatalogParityError(
            f"{label} integer atom is outside int32: {expression!r}")
    return value


def _guc_integer_value(expression: str, label: str) -> int:
    """Resolve the closed integer-expression subset used by GUC authority."""
    compact = expression.strip()
    shift_parts = compact.split("<<")
    if len(shift_parts) == 1:
        return _guc_integer_atom(compact, label)
    if len(shift_parts) != 2:
        raise CatalogParityError(
            f"{label} contains more than one shift: {expression!r}")
    left_text, shift_text = (part.strip() for part in shift_parts)
    if not left_text or not shift_text:
        raise CatalogParityError(
            f"{label} has an incomplete shift expression: {expression!r}")
    left = _guc_integer_atom(left_text, label)
    shift = _guc_integer_atom(shift_text, label)
    if left < 0 or shift < 0 or shift >= 31:
        raise CatalogParityError(
            f"{label} has an unsafe int32 shift: {expression!r}")
    if left > (_GUC_INT32_MAX >> shift):
        raise CatalogParityError(
            f"{label} shift result is outside int32: {expression!r}")
    return left << shift


def _guc_real_value(expression: str, label: str) -> str:
    """Render the accepted decimal subset as PostgreSQL pg_settings does."""
    literal = _GUC_DECIMAL_REAL.fullmatch(expression)
    if literal is None:
        raise CatalogParityError(
            f"{label} is not a supported real constant expression: "
            f"{expression!r}")
    if "." not in expression:
        integer_literal = _GUC_DECIMAL_INTEGER.fullmatch(expression)
        if (
            integer_literal is None
            or integer_literal.group("suffix")
        ):
            raise CatalogParityError(
                f"{label} is not an unambiguous decimal integer-to-real "
                f"constant: {expression!r}")
        try:
            value = float(int(expression, 10))
        except (OverflowError, ValueError) as error:
            raise CatalogParityError(
                f"{label} real constant cannot be represented: "
                f"{expression!r}") from error
    else:
        value = float(expression)
    if not math.isfinite(value):
        raise CatalogParityError(
            f"{label} real constant is not finite: {expression!r}")
    return format(value, ".6g")


def _literal_guc_value(
    expression: object,
    kind: object,
    *,
    label: str,
    required: bool,
) -> str | None:
    """Translate the closed scalar C++ expression subset to pg_settings text."""
    if not isinstance(kind, str):
        raise CatalogParityError(f"{label} has malformed kind metadata")
    if expression is None:
        if required:
            raise CatalogParityError(f"{label} has no expression")
        return None
    if not isinstance(expression, str):
        raise CatalogParityError(f"{label} has malformed expression metadata")
    compact = expression.strip()
    if kind == "bool" and compact in {"true", "false"}:
        return "on" if compact == "true" else "off"
    if kind == "integer":
        return str(_guc_integer_value(compact, label))
    if kind == "real":
        return _guc_real_value(compact, label)
    raise CatalogParityError(
        f"{label} is not a supported {kind} constant expression: "
        f"{expression!r}")


def _expected_guc_registration_rows(
    records: Sequence[Mapping[str, object]],
) -> tuple[dict[str, object], ...]:
    expected: list[dict[str, object]] = []
    for row in records:
        group = str(row["group"])
        member = str(row["member"])
        expected.append({
            "authority": {
                column: row[column]
                for column in _GUC_MANIFEST_AUTHORITY_COLUMNS
            },
            "candidate": {
                "owner": group,
                "cpp_storage": (
                    group.replace(".", "::")
                    + "::settings::"
                    + member
                ),
                "storage_kind": row["storage_kind"],
                "source_module": f"{group}.sql_guc_settings",
            },
        })
    return tuple(expected)


def _manifest_guc_source_provenance(
    manifest: Mapping[str, object],
    repository_root: Path | None,
) -> dict[str, dict[str, str]]:
    entities = manifest.get("entities")
    if not isinstance(entities, list) or not entities:
        raise CatalogParityError(
            "candidate manifest has no source-provenance entity inventory")
    result: dict[str, dict[str, str]] = {}
    normalized_repository_root = (
        None
        if repository_root is None
        else _real_directory(
            repository_root, "repository root", create=False)
    )
    for index, entity in enumerate(entities):
        if not isinstance(entity, dict):
            raise CatalogParityError(
                f"candidate manifest entity {index} is malformed")
        if entity.get("kind") != "guc":
            continue
        stable_identity = entity.get("stable_identity")
        source_module = entity.get("source_module")
        source_path = entity.get("source_path")
        source_sha256 = entity.get("source_sha256")
        if (
            not isinstance(stable_identity, str)
            or not stable_identity.startswith("sql:guc:")
            or stable_identity == "sql:guc:"
            or not isinstance(source_module, str)
            or not source_module
            or not isinstance(source_path, str)
            or not source_path
            or not isinstance(source_sha256, str)
            or re.fullmatch(r"[0-9a-f]{64}", source_sha256) is None
        ):
            raise CatalogParityError(
                f"candidate manifest GUC entity {index} has malformed "
                "source provenance or identity")
        name = stable_identity.removeprefix("sql:guc:")
        if entity.get("identity") != stable_identity:
            raise CatalogParityError(
                f"candidate manifest GUC entity {name} has competing "
                "identity forms")
        relative = _canonical_repository_relative_path(
            source_path,
            f"candidate manifest GUC entity {name} source path",
        )
        if normalized_repository_root is not None:
            source_file = _canonical_repository_source(
                normalized_repository_root,
                relative,
                f"GUC source for {name}",
            )
            if _sha256(source_file) != source_sha256:
                raise CatalogParityError(
                    f"candidate manifest GUC source hash differs from "
                    f"repository bytes: {name}")
        if name in result:
            raise CatalogParityError(
                f"candidate manifest repeats GUC entity {name}")
        result[name] = {
            "source_module": source_module,
            "source_path": source_path,
            "source_sha256": source_sha256,
        }
    if not result:
        raise CatalogParityError(
            "candidate manifest has no exact GUC entity provenance")
    return dict(sorted(result.items()))


def _validate_guc_registration_rows(
    records: Sequence[Mapping[str, object]],
    registrations: object,
    source_provenance_by_guc: Mapping[str, Mapping[str, str]],
) -> tuple[dict[str, object], ...]:
    if not isinstance(registrations, list):
        raise CatalogParityError(
            "candidate manifest has no exact GUC registrations")
    observed: list[dict[str, object]] = []
    for index, registration in enumerate(registrations):
        if (
            not isinstance(registration, dict)
            or list(registration) != [
                "authority",
                "generated_live_authority",
                "candidate",
            ]
        ):
            raise CatalogParityError(
                "candidate manifest has a malformed GUC registration")
        authority = registration.get("authority")
        generated_live = registration.get("generated_live_authority")
        candidate_registration = registration.get("candidate")
        if (
            not isinstance(authority, dict)
            or list(authority) != list(_GUC_MANIFEST_AUTHORITY_COLUMNS)
            or not isinstance(generated_live, dict)
            or list(generated_live) != [
                "vartype",
                "context",
                "unit",
                "boot_value",
                "reset_value",
                "current_value",
                "minimum",
                "maximum",
                "visible_enum_values",
                "short_description",
                "extra_description",
                "raw_flags",
                "visible_in_show_all",
                "included_in_sample",
                "hooks",
            ]
            or not isinstance(generated_live.get("hooks"), dict)
            or list(generated_live["hooks"]) != [
                "check",
                "assign",
                "show",
            ]
            or any(
                not isinstance(generated_live["hooks"].get(name), dict)
                or list(generated_live["hooks"][name])
                != ["symbol", "present", "behavior"]
                for name in ("check", "assign", "show")
            )
            or not isinstance(candidate_registration, dict)
            or list(candidate_registration) != [
                "owner",
                "cpp_storage",
                "storage_kind",
                "source_module",
                "source_sha256",
            ]
            or re.fullmatch(
                r"[0-9a-f]{64}",
                str(candidate_registration.get("source_sha256")),
            ) is None
        ):
            raise CatalogParityError(
                f"candidate manifest GUC registration {index} "
                "has malformed authority or provenance")
        registration_name = str(authority["name"])
        provenance = source_provenance_by_guc.get(registration_name)
        if (
            provenance is None
            or provenance.get("source_module")
            != candidate_registration["source_module"]
            or provenance.get("source_sha256")
            != candidate_registration["source_sha256"]
        ):
            raise CatalogParityError(
                f"candidate manifest GUC registration {index} source "
                "provenance is not bound to its exact GUC entity")
        observed.append({
            "authority": dict(authority),
            "candidate": {
                key: candidate_registration[key]
                for key in (
                    "owner",
                    "cpp_storage",
                    "storage_kind",
                    "source_module",
                )
            },
        })
    expected = _expected_guc_registration_rows(records)
    expected_names = {
        str(row["authority"]["name"])
        for row in expected
    }
    if set(source_provenance_by_guc) != expected_names:
        raise CatalogParityError(
            "candidate manifest exact GUC entity identities differ from "
            "the independent ledger")
    if tuple(observed) != expected:
        raise CatalogParityError(
            "candidate manifest GUC registration rows differ from the "
            "independent ledger")
    return tuple(observed)


def _validate_guc_authority(
    *,
    manifest: Mapping[str, object],
    ledger: Mapping[str, object],
    ledger_path: Path,
    active_profile: str,
    snapshot: Snapshot | None,
    repository_root: Path | None = None,
) -> dict[str, object]:
    """Join the sealed GUC ledger to manifest generation and live settings."""
    ledger_sha256 = _sha256(ledger_path)
    if ledger_sha256 != _EXPECTED_GUC_LEDGER_SHA256:
        raise CatalogParityError(
            "independent GUC parity ledger differs from the reviewed "
            "closed authority")
    records = _guc_ledger_records(ledger, active_profile)
    generation = manifest.get("guc_generation")
    if not isinstance(generation, dict):
        raise CatalogParityError("candidate manifest has no GUC generation")
    parity = generation.get("parity_ledger")
    registrations = generation.get("registrations")
    if (
        generation.get("profile") != active_profile
        or generation.get("registration_count") != len(records)
        or not isinstance(parity, dict)
        or parity.get("sha256") != ledger_sha256
    ):
        raise CatalogParityError(
            "candidate manifest is not sealed to the independent GUC ledger")
    observed_registration_rows = _validate_guc_registration_rows(
        records,
        registrations,
        _manifest_guc_source_provenance(manifest, repository_root),
    )

    live_checked = 0
    if snapshot is not None:
        live = {
            key.identity.removeprefix("guc:"): value
            for key, value in snapshot.rows.items()
            if key.catalog == "guc"
        }
        expected_names = {str(row["name"]) for row in records}
        if set(live) != expected_names:
            raise CatalogParityError(
                "live candidate GUC identities differ from the independent "
                f"ledger; missing={sorted(expected_names - set(live))}, "
                f"unexpected={sorted(set(live) - expected_names)}")
        context_map = {
            "internal": "internal",
            "postmaster": "postmaster",
            "sighup": "sighup",
            "superuser_backend": "superuser-backend",
            "backend": "backend",
            "superuser_set": "superuser",
            "user_set": "user",
        }
        for record in records:
            name = str(record["name"])
            value = live[name]
            if not isinstance(value, dict):
                raise CatalogParityError(
                    f"live GUC row is malformed: {name}")
            expected_context = context_map.get(str(record["context"]))
            expected_unit = record.get("unit")
            observed_unit = value.get("unit")
            expected_kind = str(record["kind"])
            expected_live_kind = {
                "bool": "bool",
                "int32": "integer",
                "double": "real",
                "string": "string",
                "int32_enum": "enum",
            }.get(str(record["storage_kind"]))
            failures: dict[str, object] = {}
            if (
                expected_live_kind != expected_kind
                or value.get("type") != expected_live_kind
            ):
                failures["type"] = {
                    "ledger_kind": expected_kind,
                    "ledger_storage_kind": record["storage_kind"],
                    "expected_live": expected_live_kind,
                    "live": value.get("type"),
                }
            if value.get("context") != expected_context:
                failures["context"] = {
                    "ledger": expected_context,
                    "live": value.get("context"),
                }
            if (
                (None if expected_unit is None else str(expected_unit).lower())
                != (None if observed_unit is None
                    else str(observed_unit).lower())
            ):
                failures["unit"] = {
                    "ledger": expected_unit,
                    "live": observed_unit,
                }
            literal_default = (
                None
                if expected_kind == "enum"
                else _literal_guc_value(
                    record.get("default_expression"),
                    expected_kind,
                    label=f"live GUC {name} default",
                    required=True,
                )
            )
            if (
                literal_default is not None
                and str(value.get("boot_value")).lower() != literal_default
            ):
                failures["boot_value"] = {
                    "ledger": literal_default,
                    "live": value.get("boot_value"),
                }
            if literal_default is not None:
                for live_field in ("setting", "reset_value"):
                    if (
                        str(value.get(live_field)).lower()
                        != literal_default
                    ):
                        failures[live_field] = {
                            "ledger": literal_default,
                            "live": value.get(live_field),
                        }
            for expression_field, live_field in (
                ("minimum_expression", "minimum"),
                ("maximum_expression", "maximum"),
            ):
                literal_bound = _literal_guc_value(
                    record.get(expression_field),
                    expected_kind,
                    label=f"live GUC {name} {expression_field}",
                    required=expected_kind in {"integer", "real"},
                )
                if (
                    literal_bound is not None
                    and str(value.get(live_field)).lower()
                    != literal_bound
                ):
                    failures[live_field] = {
                        "ledger": literal_bound,
                        "live": value.get(live_field),
                    }
            default_semantic = record.get("default_semantic")
            enum_rows = record.get(
                "enum_rows_name_wire_hidden_feature_alias_of")
            if expected_kind == "enum":
                if not isinstance(enum_rows, list):
                    failures["enum_rows"] = "malformed ledger rows"
                else:
                    allowed = {
                        str(row[0])
                        for row in enum_rows
                        if isinstance(row, list) and len(row) == 5
                    }
                    required = {
                        str(row[0])
                        for row in enum_rows
                        if (
                            isinstance(row, list)
                            and len(row) == 5
                            and row[2] == 0
                            and row[3] == "true"
                        )
                    }
                    observed = set(value.get("enum_values") or [])
                    expected_enum_default: str | None = None
                    if (
                        name == "bioseq.compression"
                        and default_semantic == "best_compiled_algorithm"
                    ):
                        expected_enum_default = next(
                            (
                                candidate
                                for candidate in ("zstd", "lz4", "pglz")
                                if candidate in observed
                            ),
                            None,
                        )
                        if expected_enum_default is None:
                            failures["enum_default"] = {
                                "ledger": "best_compiled_algorithm",
                                "live": sorted(observed),
                            }
                    elif (
                        isinstance(default_semantic, str)
                        and default_semantic in allowed
                    ):
                        expected_enum_default = default_semantic
                    if (
                        expected_enum_default is not None
                        and value.get("boot_value")
                        != expected_enum_default
                    ):
                        failures["enum_default"] = {
                            "ledger": expected_enum_default,
                            "live": value.get("boot_value"),
                        }
                    if (
                        expected_enum_default is not None
                    ):
                        for live_field in ("setting", "reset_value"):
                            if (
                                value.get(live_field)
                                != expected_enum_default
                            ):
                                failures[
                                    f"enum_{live_field}"
                                ] = {
                                    "ledger": expected_enum_default,
                                    "live": value.get(live_field),
                                }
                    if not required.issubset(observed) or not observed.issubset(
                        allowed
                    ):
                        failures["enum_values"] = {
                            "required": sorted(required),
                            "allowed": sorted(allowed),
                            "live": sorted(observed),
                        }
            if failures:
                raise CatalogParityError(
                    f"live GUC {name} differs from the independent ledger: "
                    + json.dumps(failures, sort_keys=True))
            live_checked += 1
    return {
        "profile": active_profile,
        "ledger_sha256": ledger_sha256,
        "registration_count": len(records),
        "manifest_rows_checked": len(observed_registration_rows),
        "live_rows_checked": live_checked,
    }


def _stage_member(
    stage_root: Path,
    manifest: Mapping[str, object],
    member: str,
) -> Path:
    staged = manifest.get("staged")
    if not isinstance(staged, dict) or not isinstance(staged.get(member), str):
        raise CatalogParityError(f"stage manifest is missing staged.{member}")
    relative = Path(staged[member])
    if relative.is_absolute() or ".." in relative.parts:
        raise CatalogParityError(
            f"stage manifest has unsafe staged.{member}: {relative}")
    path = stage_root / relative
    return _real_directory(path, f"staged {member}", create=False)


def _stage(
    *,
    label: str,
    cmake: Path,
    pg_config: Path,
    stage_script: Path,
    control_file: Path,
    sql_file: Path,
    dso: Path,
    stage_parent: Path,
    retained_root: Path,
    timeout: int,
) -> tuple[Path, dict[str, object]]:
    stage_root = stage_parent / label
    manifest_path = stage_root / "stage-manifest.json"
    aggregate_path = stage_root / "stage-artifacts.sha256"
    _run(
        [
            str(cmake),
            f"-DPG_CONFIG={pg_config}",
            f"-DSTAGE_ROOT={stage_root}",
            f"-DSTAGE_PARENT={stage_parent}",
            f"-DCONTROL_FILE={control_file}",
            f"-DINSTALL_SQL={sql_file}",
            f"-DLIBRARY_FILE={dso}",
            "-DEXTENSION_VERSION=1.0",
            f"-DMANIFEST_OUT={manifest_path}",
            f"-DHASH_OUT={aggregate_path}",
            "-P",
            str(stage_script),
        ],
        timeout=timeout,
        log=retained_root / "logs" / f"stage-{label}.log",
    )
    manifest_path = _regular_file(manifest_path, f"{label} stage manifest")
    aggregate_path = _regular_file(aggregate_path, f"{label} stage hash")
    manifest = _load_json(manifest_path, f"{label} stage manifest")
    if manifest.get("schema") != "stage_manifest/v1":
        raise CatalogParityError(f"{label} stage manifest schema is invalid")
    artifacts = manifest.get("bioseq_artifacts")
    if not isinstance(artifacts, dict):
        raise CatalogParityError(f"{label} stage manifest has no artifacts")
    expected = {
        "control": _sha256(control_file),
        "install_sql": _sha256(sql_file),
        "library": _sha256(dso),
    }
    for artifact, expected_hash in expected.items():
        record = artifacts.get(artifact)
        if not isinstance(record, dict) or record.get("sha256") != expected_hash:
            raise CatalogParityError(
                f"{label} staged {artifact} hash does not match its input")
    retained_stage = retained_root / "stages"
    _copy_file(manifest_path, retained_stage / f"{label}-manifest.json")
    _copy_file(aggregate_path, retained_stage / f"{label}-artifacts.sha256")
    return stage_root.resolve(strict=True), manifest


def _psql_command(
    cluster: UniquePrivateCluster,
    bindir: Path,
    *arguments: str,
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
        *arguments,
    ]


def _install_extension(
    *,
    label: str,
    cluster: UniquePrivateCluster,
    bindir: Path,
    legacy_catalog_only: bool,
    retained_root: Path,
    timeout: int,
) -> None:
    body_check = "off" if legacy_catalog_only else "on"
    sql = f"""
CREATE ROLE doc0388_catalog_probe NOLOGIN;
LOAD '$libdir/bioseq';
DO $assertion$
BEGIN
  IF current_setting('check_function_bodies') <> 'on' THEN
    RAISE EXCEPTION 'unexpected initial check_function_bodies setting';
  END IF;
END
$assertion$;
SET check_function_bodies = {body_check};
DO $assertion$
BEGIN
  IF current_setting('check_function_bodies') <> '{body_check}' THEN
    RAISE EXCEPTION 'failed to select catalog body-check policy';
  END IF;
END
$assertion$;
CREATE EXTENSION bioseq;
"""
    query_path = retained_root / "queries" / f"install-{label}.sql"
    _write_bytes(query_path, sql.encode("utf-8"))
    _run(
        _psql_command(cluster, bindir, "--file", str(query_path)),
        timeout=timeout,
        env=cluster.child_environment(),
        log=retained_root / "logs" / f"install-{label}.log",
    )


def _snapshot_cluster(
    *,
    label: str,
    cluster: UniquePrivateCluster,
    bindir: Path,
    query: Path,
    retained_root: Path,
    timeout: int,
) -> tuple[Path, dict[str, object]]:
    result_path = retained_root / "results" / f"{label}.csv"
    result = _run(
        _psql_command(cluster, bindir, "--file", str(query)),
        timeout=timeout,
        env=cluster.child_environment(),
    )
    _write_bytes(result_path, result)
    metadata = _capture_server_metadata(
        label=label,
        cluster=cluster,
        bindir=bindir,
        retained_root=retained_root,
        timeout=timeout,
    )
    return result_path, metadata


def _parse_server_metadata(data: bytes, label: str) -> dict[str, str]:
    records = list(csv.DictReader(
        io.StringIO(data.decode("utf-8", errors="strict"))))
    if len(records) != 1:
        raise CatalogParityError(
            f"{label} server metadata returned {len(records)} rows")
    metadata = records[0]
    if not str(metadata.get("server_version_num", "")).startswith("17"):
        raise CatalogParityError(
            f"{label} did not run on PostgreSQL 17: {metadata}")
    return metadata


def _capture_server_metadata(
    *,
    label: str,
    cluster: UniquePrivateCluster,
    bindir: Path,
    retained_root: Path,
    timeout: int,
) -> dict[str, str]:
    metadata_sql = """
COPY (
  SELECT
    current_setting('server_version') AS server_version,
    current_setting('server_version_num') AS server_version_num,
    version() AS version_string,
    (SELECT system_identifier::text
       FROM pg_catalog.pg_control_system()) AS system_identifier,
    (SELECT n.nspname
       FROM pg_catalog.pg_extension AS e
       JOIN pg_catalog.pg_namespace AS n ON n.oid = e.extnamespace
      WHERE e.extname = 'bioseq') AS extension_schema
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
"""
    metadata_query_path = (
        retained_root / "queries" / f"{label}-server-metadata.sql")
    _write_bytes(metadata_query_path, metadata_sql.encode("utf-8"))
    metadata_bytes = _run(
        _psql_command(
            cluster, bindir, "--file", str(metadata_query_path)),
        timeout=timeout,
        env=cluster.child_environment(),
    )
    metadata_path = retained_root / "results" / f"{label}-server.csv"
    _write_bytes(metadata_path, metadata_bytes)
    return _parse_server_metadata(metadata_bytes, label)


def _parse_snapshot(path: Path) -> Snapshot:
    rows: dict[SnapshotKey, object] = {}
    counts: collections.Counter[str] = collections.Counter()
    with path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ["catalog", "identity", "ordinal", "value"]:
            raise CatalogParityError(
                f"snapshot has an invalid CSV header: {path}: "
                f"{reader.fieldnames!r}")
        for line_number, record in enumerate(reader, start=2):
            try:
                key = SnapshotKey(
                    catalog=record["catalog"],
                    identity=record["identity"],
                    ordinal=int(record["ordinal"]),
                )
                value = json.loads(record["value"])
            except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
                raise CatalogParityError(
                    f"malformed snapshot row {path}:{line_number}") from error
            if key in rows:
                raise CatalogParityError(f"duplicate snapshot key: {key}")
            if key.catalog != "snapshot_scope":
                required_fields = _REQUIRED_SNAPSHOT_FIELDS.get(key.catalog)
                if (
                    required_fields is None
                    or not isinstance(value, dict)
                    or set(value) != set(required_fields)
                ):
                    raise CatalogParityError(
                        f"snapshot row {path}:{line_number} has an "
                        f"invalid {key.catalog} field set")
                if key.catalog == "pg_opclass":
                    nested_fields = {
                        "operators": {
                            "strategy", "purpose", "left", "right",
                            "operator", "sort_family",
                        },
                        "functions": {
                            "number", "left", "right", "function",
                        },
                    }
                    for nested_name, expected_fields in nested_fields.items():
                        nested = value[nested_name]
                        if nested is None:
                            continue
                        if (
                            not isinstance(nested, list)
                            or any(
                                not isinstance(item, dict)
                                or set(item) != expected_fields
                                for item in nested
                            )
                        ):
                            raise CatalogParityError(
                                f"snapshot row {path}:{line_number} "
                                f"has malformed pg_opclass.{nested_name}")
            rows[key] = value
            counts[key.catalog] += 1
    if not rows:
        raise CatalogParityError(f"snapshot is empty: {path}")
    scope: dict[str, object] = {}
    for key, value in rows.items():
        if key.catalog != "snapshot_scope":
            continue
        if (
            key.ordinal != 1
            or not key.identity.startswith("catalog:")
        ):
            raise CatalogParityError(
                f"snapshot has a malformed scope marker: {key}")
        scope[key.identity.removeprefix("catalog:")] = value
    expected_scope = {
        catalog: {
            "queried": True,
            "fields": list(_REQUIRED_SNAPSHOT_FIELDS[catalog]),
        }
        for catalog in sorted(_REQUIRED_SNAPSHOT_CATALOGS)
    }
    if scope != expected_scope:
        raise CatalogParityError(
            "snapshot query-section coverage drifted: "
            f"observed={scope!r} expected={expected_scope!r}")
    return Snapshot(rows=rows, counts=dict(sorted(counts.items())))


def _rename_pairs(delta: Mapping[str, object]) -> tuple[tuple[str, str], ...]:
    raw = delta.get("identity_renames")
    if not isinstance(raw, list) or len(raw) != 19:
        raise CatalogParityError(
            "approved exact routine-rename inventory is invalid")
    pairs: list[tuple[str, str]] = []
    seen_legacy: set[str] = set()
    seen_candidate: set[str] = set()
    for item in raw:
        if (
            not isinstance(item, dict)
            or not isinstance(item.get("legacy"), str)
            or not isinstance(item.get("candidate"), str)
            or item["legacy"] == item["candidate"]
            or item["legacy"] in seen_legacy
            or item["candidate"] in seen_candidate
        ):
            raise CatalogParityError(
                "approved exact routine-rename row is invalid")
        seen_legacy.add(item["legacy"])
        seen_candidate.add(item["candidate"])
        pairs.append((item["legacy"], item["candidate"]))
    return tuple(pairs)


def _rename_identity(
    value: str,
    pairs: Sequence[tuple[str, str]],
) -> str:
    result = value
    for legacy, candidate in pairs:
        legacy_core = legacy.split(":", 1)[-1]
        candidate_core = candidate.split(":", 1)[-1]
        result = result.replace(legacy, candidate)
        result = result.replace(legacy_core, candidate_core)
    return result


def _rename_nested_identity_values(
    value: object,
    pairs: Sequence[tuple[str, str]],
) -> object:
    if isinstance(value, str):
        return _rename_identity(value, pairs)
    if isinstance(value, list):
        return [
            _rename_nested_identity_values(item, pairs)
            for item in value
        ]
    if isinstance(value, dict):
        return {
            key: _rename_nested_identity_values(item, pairs)
            for key, item in value.items()
        }
    return value


def _mapped_legacy_snapshot(
    snapshot: Snapshot,
    pairs: Sequence[tuple[str, str]],
) -> Snapshot:
    rows: dict[SnapshotKey, object] = {}
    for key, value in snapshot.rows.items():
        mapped_key = SnapshotKey(
            key.catalog,
            _rename_identity(key.identity, pairs),
            key.ordinal,
        )
        mapped_value = value
        if key.catalog != "pg_proc":
            mapped_value = _rename_nested_identity_values(value, pairs)
        if mapped_key in rows:
            raise CatalogParityError(
                f"approved identity mapping collided at {mapped_key}")
        rows[mapped_key] = mapped_value
    return Snapshot(rows=rows, counts=snapshot.counts)


def _flatten(value: object, path: str = "") -> dict[str, object]:
    if isinstance(value, dict):
        flattened: dict[str, object] = {}
        for key in sorted(value):
            child = f"{path}.{key}" if path else key
            flattened.update(_flatten(value[key], child))
        return flattened
    return {path: value}


def _routine_leaf(identity: str) -> str:
    before_args = identity.rsplit("(", 1)[0]
    return before_args.rsplit(".", 1)[-1]


def _dead_tokens(delta: Mapping[str, object]) -> tuple[str, ...]:
    raw = delta.get("removed_dead_routines")
    if not isinstance(raw, list) or len(raw) != 2:
        raise CatalogParityError("dead-routine delta inventory is invalid")
    if not all(
        isinstance(value, str) and value.startswith("function:")
        for value in raw
    ):
        raise CatalogParityError("dead-routine identities are malformed")
    return tuple(value.removeprefix("function:") for value in raw)


def _native_identities(delta: Mapping[str, object]) -> frozenset[str]:
    raw = delta.get("native_routine_replacements")
    if not isinstance(raw, list) or len(raw) != 4:
        raise CatalogParityError("native-routine delta inventory is invalid")
    return frozenset(value for value in raw if isinstance(value, str))


def _is_native_reference(
    identity: str,
    native_identities: Iterable[str],
) -> bool:
    return any(
        routine.split(":", 1)[-1] in identity
        for routine in native_identities
    )


def _unconsumed_exact_ledger_diagnostics(
    ledgers: Mapping[str, Sequence[object]],
) -> list[dict[str, object]]:
    diagnostics: list[dict[str, object]] = []
    for ledger_name, missing_rows in sorted(ledgers.items()):
        if missing_rows:
            diagnostics.append({
                "catalog": "summary",
                "identity": ledger_name,
                "ordinal": 1,
                "property": "unconsumed_exact_rows",
                "legacy": list(missing_rows),
                "candidate": [],
                "approved_category": None,
            })
    return diagnostics


def _compare_snapshots(
    legacy: Snapshot,
    candidate: Snapshot,
    delta: Mapping[str, object],
    contract: Mapping[str, object],
) -> tuple[list[dict[str, object]], list[dict[str, object]], dict[str, object]]:
    pairs = _rename_pairs(delta)
    original_legacy = legacy
    legacy = _mapped_legacy_snapshot(legacy, pairs)
    dead = _dead_tokens(delta)
    native = _native_identities(delta)
    symbol_rows = _exact_delta_rows(
        delta,
        "routine_symbol_changes",
        ("identity", "legacy", "candidate"),
        expected_count=118,
    )
    input_name_rows = _exact_delta_rows(
        delta,
        "input_name_changes",
        (
            "identity",
            "ordinal",
            "argument_type",
            "legacy",
            "candidate",
        ),
        expected_count=309,
    )
    comment_rows = _exact_delta_rows(
        delta,
        "catalog_comment_changes",
        ("identity", "legacy", "candidate"),
    )
    symbol_authority = {
        (
            str(row["identity"]),
            str(row["legacy"]),
            str(row["candidate"]),
        )
        for row in symbol_rows
    }
    symbol_authority.update(
        (
            candidate_identity,
            _routine_leaf(legacy_identity),
            _routine_leaf(candidate_identity),
        )
        for legacy_identity, candidate_identity in pairs
        if ".vchar" in legacy_identity and ".vbpchar" in candidate_identity
    )
    input_name_authority = {
        (
            str(row["identity"]),
            int(row["ordinal"]),
            str(row["argument_type"]),
            row["legacy"],
            row["candidate"],
        )
        for row in input_name_rows
    }
    comment_authority = {
        (str(row["identity"]), row["legacy"], row["candidate"])
        for row in comment_rows
    }
    consumed_symbols: set[tuple[str, str, str]] = set()
    consumed_input_names: set[
        tuple[str, int, str, object, object]
    ] = set()
    consumed_comments: set[tuple[str, object, object]] = set()
    catalog_change_rows = _catalog_change_rows(delta, contract)
    catalog_change_authority = {
        (
            str(row["catalog"]),
            str(row["identity"]),
            int(row["ordinal"]),
            str(row["property"]),
        ): row
        for row in catalog_change_rows
    }
    consumed_catalog_changes: set[tuple[str, str, int, str]] = set()
    all_keys = sorted(set(legacy.rows) | set(candidate.rows))
    approved: list[dict[str, object]] = []
    unapproved: list[dict[str, object]] = []
    c_prosrc_changes: list[tuple[str, str, str]] = []

    def record(
        *,
        key: SnapshotKey,
        property_name: str,
        legacy_value: object,
        candidate_value: object,
        category: str | None,
        exact: bool = False,
    ) -> None:
        del exact
        legacy_envelope = (
            {"present": False}
            if legacy_value is dataclasses.MISSING
            else {"present": True, "value": legacy_value}
        )
        candidate_envelope = (
            {"present": False}
            if candidate_value is dataclasses.MISSING
            else {"present": True, "value": candidate_value}
        )
        approved_category = _match_catalog_change_authority(
            authority=catalog_change_authority,
            consumed=consumed_catalog_changes,
            key=key,
            property_name=property_name,
            legacy_value=legacy_value,
            candidate_value=candidate_value,
            observed_category=category,
        )
        row = {
            "catalog": key.catalog,
            "identity": key.identity,
            "ordinal": key.ordinal,
            "property": property_name,
            "legacy": legacy_envelope,
            "candidate": candidate_envelope,
            "observed_category": category,
            "approved_category": approved_category,
        }
        (
            approved
            if approved_category is not None
            else unapproved
        ).append(row)

    for legacy_identity, candidate_identity in pairs:
        legacy_key = SnapshotKey("pg_proc", legacy_identity, 1)
        candidate_key = SnapshotKey("pg_proc", candidate_identity, 1)
        if (
            legacy_key not in original_legacy.rows
            or candidate_key not in candidate.rows
        ):
            record(
                key=candidate_key,
                property_name="identity",
                legacy_value=(
                    legacy_identity
                    if legacy_key in original_legacy.rows
                    else dataclasses.MISSING
                ),
                candidate_value=(
                    candidate_identity
                    if candidate_key in candidate.rows
                    else dataclasses.MISSING
                ),
                category=(
                    "bpchar_vector_repair"
                    if ".vbpchar" in candidate_identity
                    else "standardized_routine_identity"
                ),
            )
        else:
            category = (
                "bpchar_vector_repair"
                if ".vbpchar" in candidate_identity
                else "standardized_routine_identity"
            )
            record(
                key=candidate_key,
                property_name="identity",
                legacy_value=legacy_identity,
                candidate_value=candidate_identity,
                category=category,
            )

    for key in all_keys:
        before = legacy.rows.get(key, dataclasses.MISSING)
        after = candidate.rows.get(key, dataclasses.MISSING)
        if before is dataclasses.MISSING or after is dataclasses.MISSING:
            category: str | None = None
            exact = False
            if any(token in key.identity for token in dead):
                if before is not dataclasses.MISSING and after is dataclasses.MISSING:
                    category = "dead_routine_removal"
            elif key.catalog == "pg_description":
                old_description = (
                    before.get("description")
                    if isinstance(before, dict)
                    else None
                )
                new_description = (
                    after.get("description")
                    if isinstance(after, dict)
                    else None
                )
                authority_row = (
                    key.identity, old_description, new_description)
                if authority_row in comment_authority:
                    consumed_comments.add(authority_row)
                    category = "doxygen_catalog_comment_projection"
                    exact = True
            elif key.catalog in {
                "pg_depend",
                "pg_description",
                "acl",
                "effective_privilege",
                "extension_membership",
            } and _is_native_reference(key.identity, native):
                category = "native_routine_replacement"
            record(
                key=key,
                property_name="<record>",
                legacy_value=before,
                candidate_value=after,
                category=category,
                exact=exact,
            )
            continue
        before_flat = _flatten(before)
        after_flat = _flatten(after)
        for property_name in sorted(set(before_flat) | set(after_flat)):
            old = before_flat.get(property_name, dataclasses.MISSING)
            new = after_flat.get(property_name, dataclasses.MISSING)
            if old == new:
                continue
            category = None
            exact = False
            if key.catalog == "pg_proc":
                canonical_identity = key.identity
                if canonical_identity in native:
                    if property_name in {
                        "language",
                        "probin",
                        "prosrc",
                        "configuration",
                    }:
                        category = "native_routine_replacement"
                elif property_name == "prosrc":
                    authority_row = (
                        key.identity,
                        old,
                        new,
                    )
                    if authority_row in symbol_authority:
                        assert isinstance(old, str)
                        assert isinstance(new, str)
                        consumed_symbols.add(authority_row)
                        category = (
                            "bpchar_vector_repair"
                            if new.startswith("vbpchar")
                            else "derived_routine_symbol"
                        )
                        c_prosrc_changes.append((key.identity, old, new))
                elif property_name == "proargnames":
                    old_value = before if isinstance(before, dict) else {}
                    new_value = after if isinstance(after, dict) else {}
                    old_names = old_value.get("proargnames")
                    new_names = new_value.get("proargnames")
                    argument_types = new_value.get("argument_types")
                    old_list = (
                        list(old_names) if isinstance(old_names, list) else [])
                    new_list = (
                        list(new_names) if isinstance(new_names, list) else [])
                    width = max(len(old_list), len(new_list))
                    old_list.extend([None] * (width - len(old_list)))
                    new_list.extend([None] * (width - len(new_list)))
                    observed_rows: set[
                        tuple[str, int, str, object, object]
                    ] = set()
                    shape_is_valid = (
                        isinstance(argument_types, list)
                        and len(argument_types) >= width
                    )
                    if shape_is_valid:
                        for position, (old_name, new_name) in enumerate(
                            zip(old_list, new_list), start=1
                        ):
                            if old_name == new_name:
                                continue
                            argument_type = argument_types[position - 1]
                            if not isinstance(argument_type, str):
                                shape_is_valid = False
                                break
                            observed_rows.add((
                                key.identity,
                                position,
                                argument_type,
                                old_name,
                                new_name,
                            ))
                    if (
                        shape_is_valid
                        and observed_rows
                        and observed_rows.issubset(input_name_authority)
                    ):
                        consumed_input_names.update(observed_rows)
                        category = "derived_input_name"
                        exact = True
            elif (
                key.catalog == "pg_description"
                and property_name == "description"
            ):
                authority_row = (key.identity, old, new)
                if authority_row in comment_authority:
                    consumed_comments.add(authority_row)
                    category = "doxygen_catalog_comment_projection"
                    exact = True
            elif key.catalog == "pg_depend" and _is_native_reference(
                key.identity, native
            ):
                category = "native_routine_replacement"
            record(
                key=key,
                property_name=property_name,
                legacy_value=old,
                candidate_value=new,
                category=category,
                exact=exact,
            )

    expected_prosrc = contract.get("legacy_to_candidate_prosrc_change_count")
    expected_derived_prosrc = contract.get(
        "derived_routine_symbol_change_count")
    expected_name_routines = contract.get("derived_input_name_routine_count")
    expected_name_positions = contract.get("derived_input_name_position_count")
    raw_dead = delta.get("removed_dead_routines")
    assert isinstance(raw_dead, list)
    dead_keys = {
        SnapshotKey("pg_proc", identity, 1)
        for identity in raw_dead
        if isinstance(identity, str)
    }
    if (
        len(dead_keys) != 2
        or not dead_keys.issubset(original_legacy.rows)
        or any(key in candidate.rows for key in dead_keys)
    ):
        unapproved.append({
            "catalog": "summary",
            "identity": "dead_routine_removal",
            "ordinal": 1,
            "property": "exact_identity_closure",
            "legacy": sorted(
                key.identity
                for key in dead_keys
                if key in original_legacy.rows
            ),
            "candidate": sorted(
                key.identity for key in dead_keys if key in candidate.rows
            ),
            "approved_category": None,
        })
    if len(c_prosrc_changes) != expected_prosrc:
        unapproved.append({
            "catalog": "summary",
            "identity": "derived_routine_symbol",
            "ordinal": 1,
            "property": "count",
            "legacy": len(c_prosrc_changes),
            "candidate": expected_prosrc,
            "approved_category": None,
        })
    derived_prosrc_count = sum(
        not candidate_symbol.startswith("vbpchar")
        for identity, legacy_symbol, candidate_symbol in c_prosrc_changes
    )
    if derived_prosrc_count != expected_derived_prosrc:
        unapproved.append({
            "catalog": "summary",
            "identity": "derived_routine_symbol",
            "ordinal": 2,
            "property": "exact_category_count",
            "legacy": derived_prosrc_count,
            "candidate": expected_derived_prosrc,
            "approved_category": None,
        })
    if (
        len({row[0] for row in consumed_input_names})
        != expected_name_routines
        or len(consumed_input_names) != expected_name_positions
    ):
        unapproved.append({
            "catalog": "summary",
            "identity": "derived_input_name",
            "ordinal": 1,
            "property": "counts",
            "legacy": {
                "routines": len({
                    row[0] for row in consumed_input_names}),
                "positions": len(consumed_input_names),
            },
            "candidate": {
                "routines": expected_name_routines,
                "positions": expected_name_positions,
            },
            "approved_category": None,
        })
    unconsumed_ledgers = {
        "routine_symbol_changes": sorted(
            symbol_authority - consumed_symbols),
        "input_name_changes": sorted(
            input_name_authority - consumed_input_names,
            key=lambda row: (
                row[0], row[1], row[2], str(row[3]), str(row[4])),
        ),
        "catalog_comment_changes": sorted(
            comment_authority - consumed_comments,
            key=lambda row: (row[0], str(row[1]), str(row[2])),
        ),
        "catalog_changes": [
            catalog_change_authority[locator]
            for locator in sorted(
                set(catalog_change_authority)
                - consumed_catalog_changes
            )
        ],
    }
    unapproved.extend(
        _unconsumed_exact_ledger_diagnostics(unconsumed_ledgers))
    summary = {
        "prosrc_changes": [
            {"identity": identity, "legacy": old, "candidate": new}
            for identity, old, new in sorted(c_prosrc_changes)
        ],
        "prosrc_change_count": len(c_prosrc_changes),
        "derived_routine_symbol_change_count": derived_prosrc_count,
        "bpchar_prosrc_change_count": (
            len(c_prosrc_changes) - derived_prosrc_count),
        "derived_input_name_routine_count": len({
            row[0] for row in consumed_input_names}),
        "derived_input_name_position_count": len(consumed_input_names),
        "catalog_comment_change_count": len(consumed_comments),
        "approved_catalog_change_count": len(consumed_catalog_changes),
        "exact_ledgers_fully_consumed": not any(
            unconsumed_ledgers.values()),
    }
    return approved, unapproved, summary


def _nm_exports(dso: Path, *, timeout: int) -> frozenset[str]:
    nm = shutil.which("nm")
    if nm is None:
        raise CatalogParityError("cannot find nm for DSO symbol closure")
    if platform.system() == "Darwin":
        command = [nm, "-gU", str(dso)]
    else:
        command = [nm, "-D", "--defined-only", str(dso)]
    output = _run(command, timeout=timeout).decode(
        "utf-8", errors="replace")
    symbols: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if not fields:
            continue
        symbol = fields[-1]
        if platform.system() == "Darwin" and symbol.startswith("_"):
            symbol = symbol[1:]
        symbols.add(symbol)
    return frozenset(symbols)


def _exact_string_inventory(
    value: object,
    *,
    label: str,
    expected_count: int | None = None,
) -> frozenset[str]:
    if (
        not isinstance(value, list)
        or not all(isinstance(item, str) and item for item in value)
        or len(value) != len(set(value))
        or (
            expected_count is not None
            and len(value) != expected_count
        )
    ):
        expected = (
            ""
            if expected_count is None
            else f" with exactly {expected_count} rows"
        )
        raise CatalogParityError(
            f"{label} must be a duplicate-free string inventory{expected}")
    return frozenset(value)


def _validate_symbol_edge_closure(
    *,
    manifest_c_symbols: frozenset[str],
    manifest_finfo_symbols: frozenset[str],
    manifest_owner_symbols: frozenset[str],
    manifest_identity_to_symbol: Mapping[str, str],
    catalog_identity_to_symbol: Mapping[str, str],
    exported_symbols: frozenset[str],
    external_legacy_symbols: frozenset[str],
    expected_identity_count: int,
    expected_c_symbol_count: int,
    expected_finfo_symbol_count: int,
) -> dict[str, object]:
    """Validate the pure catalog↔manifest↔DSO↔finfo edge closure."""
    expected_finfo = frozenset(
        f"pg_finfo_{symbol}" for symbol in manifest_c_symbols)
    expected_exported_finfo = expected_finfo | frozenset(
        f"pg_finfo_{symbol}" for symbol in external_legacy_symbols)
    actual_exported_finfo = frozenset(
        symbol
        for symbol in exported_symbols
        if symbol.startswith("pg_finfo_")
    )
    manifest_bound_symbols = frozenset(
        manifest_identity_to_symbol.values())
    failures: dict[str, object] = {
        "manifest_identity_count": (
            []
            if len(manifest_identity_to_symbol) == expected_identity_count
            else [
                len(manifest_identity_to_symbol),
                expected_identity_count,
            ]
        ),
        "catalog_identity_count": (
            []
            if len(catalog_identity_to_symbol) == expected_identity_count
            else [
                len(catalog_identity_to_symbol),
                expected_identity_count,
            ]
        ),
        "manifest_c_symbol_count": (
            []
            if len(manifest_c_symbols) == expected_c_symbol_count
            else [
                len(manifest_c_symbols),
                expected_c_symbol_count,
            ]
        ),
        "manifest_finfo_symbol_count": (
            []
            if len(manifest_finfo_symbols) == expected_finfo_symbol_count
            else [
                len(manifest_finfo_symbols),
                expected_finfo_symbol_count,
            ]
        ),
        "manifest_finfo_missing": sorted(
            expected_finfo - manifest_finfo_symbols),
        "manifest_finfo_extra": sorted(
            manifest_finfo_symbols - expected_finfo),
        "manifest_owner_symbols_missing": sorted(
            manifest_c_symbols - manifest_owner_symbols),
        "manifest_owner_symbols_extra": sorted(
            manifest_owner_symbols - manifest_c_symbols),
        "manifest_unbound_symbols": sorted(
            manifest_c_symbols - manifest_bound_symbols),
        "manifest_unknown_bound_symbols": sorted(
            manifest_bound_symbols - manifest_c_symbols),
        "catalog_identities_missing": sorted(
            set(manifest_identity_to_symbol)
            - set(catalog_identity_to_symbol)),
        "catalog_identities_extra": sorted(
            set(catalog_identity_to_symbol)
            - set(manifest_identity_to_symbol)),
        "catalog_cross_name_bindings": sorted(
            identity
            for identity in (
                set(manifest_identity_to_symbol)
                & set(catalog_identity_to_symbol)
            )
            if (
                manifest_identity_to_symbol[identity]
                != catalog_identity_to_symbol[identity]
            )
        ),
        "dso_c_symbols_missing": sorted(
            manifest_c_symbols - exported_symbols),
        "dso_finfo_symbols_missing": sorted(
            expected_exported_finfo - actual_exported_finfo),
        "dso_finfo_symbols_extra": sorted(
            actual_exported_finfo - expected_exported_finfo),
    }
    if any(failures.values()):
        raise CatalogParityError(
            "catalog/manifest/DSO/finfo edge closure failed: "
            + json.dumps(failures, sort_keys=True))
    return {
        "manifest_identity_count": len(manifest_identity_to_symbol),
        "catalog_identity_count": len(catalog_identity_to_symbol),
        "c_symbol_count": len(manifest_c_symbols),
        "finfo_symbol_count": len(manifest_finfo_symbols),
        "owner_symbol_count": len(manifest_owner_symbols),
        "external_legacy_symbol_count": len(external_legacy_symbols),
        "bijection": True,
    }


def _manifest_snapshot_identity(stable_identity: str) -> str:
    """Normalize a manifest stable routine identity to snapshot spelling."""
    if not stable_identity.startswith(
        ("sql:function:", "sql:procedure:", "sql:aggregate:")
    ):
        raise CatalogParityError(
            f"manifest C binding has non-routine identity: {stable_identity}")
    identity = stable_identity.removeprefix("sql:")
    open_paren = identity.rfind("(")
    if open_paren < 0 or not identity.endswith(")"):
        raise CatalogParityError(
            f"manifest routine identity is malformed: {stable_identity}")
    prefix = identity[:open_paren]
    raw_arguments = identity[open_paren + 1:-1]
    aliases = {
        "bool": "boolean",
        "char": "character",
        "float4": "real",
        "float8": "double precision",
        "int": "integer",
        "int2": "smallint",
        "int4": "integer",
        "int8": "bigint",
        "time": "time without time zone",
        "timestamp": "timestamp without time zone",
        "timestamptz": "timestamp with time zone",
        "varchar": "character varying",
    }
    arguments: list[str] = []
    for raw in raw_arguments.split(",") if raw_arguments else ():
        argument = raw.strip()
        suffix = ""
        while argument.endswith("[]"):
            suffix += "[]"
            argument = argument[:-2]
        argument = aliases.get(argument, argument)
        if argument == "bioseq_aligner_options":
            argument = "$extension.bioseq_aligner_options"
        arguments.append(argument + suffix)
    return prefix + "(" + ",".join(arguments) + ")"


def _manifest_runtime_identity(stable_identity: str) -> str:
    prefixes = (
        "sql:function:",
        "sql:procedure:",
        "sql:aggregate:",
    )
    prefix = next(
        (value for value in prefixes if stable_identity.startswith(value)),
        None,
    )
    if prefix is None:
        raise CatalogParityError(
            f"manifest C binding has non-routine identity: "
            f"{stable_identity}")
    identity = stable_identity.removeprefix(prefix)
    if "(" not in identity or not identity.endswith(")"):
        raise CatalogParityError(
            f"manifest routine identity is malformed: {stable_identity}")
    return identity.removeprefix("$extension.")


def _validate_symbol_closure(
    *,
    manifest: Mapping[str, object],
    dso: Path,
    candidate_snapshot: Snapshot,
    prosrc_summary: Mapping[str, object],
    timeout: int,
) -> dict[str, object]:
    c_symbols_raw = manifest.get("c_link_symbols")
    finfo_raw = manifest.get("c_finfo_symbols")
    retired_raw = manifest.get("retired_c_symbols")
    c_symbols = _exact_string_inventory(
        c_symbols_raw,
        label="candidate manifest C link symbols",
        expected_count=270,
    )
    finfo_symbols = _exact_string_inventory(
        finfo_raw,
        label="candidate manifest finfo symbols",
        expected_count=270,
    )
    retired = _exact_string_inventory(
        retired_raw,
        label="candidate manifest retired C symbols",
        expected_count=117,
    )
    if finfo_symbols != frozenset(
        f"pg_finfo_{symbol}" for symbol in c_symbols
    ):
        raise CatalogParityError(
            "candidate manifest finfo inventory is not derived exactly from "
            "its C link-symbol inventory")

    entities = manifest.get("entities")
    if not isinstance(entities, list):
        raise CatalogParityError(
            "candidate manifest has no exact entity inventory")
    expected_identity_to_symbol: dict[str, str] = {}
    expected_symbol_to_identities: dict[str, set[str]] = (
        collections.defaultdict(set))
    expected_symbol_to_runtime_identities: dict[str, set[str]] = (
        collections.defaultdict(set))
    expected_identity_to_finfo: dict[str, str] = {}
    for entity_ordinal, entity in enumerate(entities, start=1):
        if not isinstance(entity, dict):
            raise CatalogParityError(
                f"candidate manifest entity {entity_ordinal} is malformed")
        binding = entity.get("c_binding")
        if binding is None:
            continue
        stable = entity.get("stable_identity")
        if (
            not isinstance(binding, dict)
            or not isinstance(stable, str)
            or not isinstance(binding.get("symbol"), str)
            or not isinstance(binding.get("finfo_symbol"), str)
        ):
            raise CatalogParityError(
                f"candidate manifest C entity {entity_ordinal} is malformed")
        identity = _manifest_snapshot_identity(stable)
        runtime_identity = _manifest_runtime_identity(stable)
        symbol = binding["symbol"]
        finfo_symbol = binding["finfo_symbol"]
        if (
            symbol not in c_symbols
            or finfo_symbol != f"pg_finfo_{symbol}"
            or binding.get("routine_identity") != runtime_identity
            or identity in expected_identity_to_symbol
        ):
            raise CatalogParityError(
                "candidate manifest C entity mapping is not one-to-one")
        expected_identity_to_symbol[identity] = symbol
        expected_identity_to_finfo[identity] = finfo_symbol
        expected_symbol_to_identities[symbol].add(identity)
        expected_symbol_to_runtime_identities[symbol].add(
            runtime_identity)
        shared_routines = binding.get(
            "same_symbol_routine_identities")
        if (
            not isinstance(shared_routines, list)
            or not all(
                isinstance(value, str) for value in shared_routines)
            or len(shared_routines) != len(set(shared_routines))
        ):
            raise CatalogParityError(
                "candidate manifest C entity has malformed shared-symbol "
                "routine identities")
    if (
        len(expected_identity_to_symbol)
        != manifest.get("c_routine_identity_count")
        or set(expected_symbol_to_identities) != set(c_symbols)
    ):
        raise CatalogParityError(
            "candidate manifest entity bindings do not close over its "
            "routine/symbol inventories")

    actual_identity_to_symbol: dict[str, str] = {}
    for key, value in candidate_snapshot.rows.items():
        if (
            key.catalog != "pg_proc"
            or not isinstance(value, dict)
            or value.get("language") != "c"
            or value.get("kind") not in {"f", "p"}
        ):
            continue
        symbol = value.get("prosrc")
        if not isinstance(symbol, str) or key.identity in actual_identity_to_symbol:
            raise CatalogParityError(
                f"candidate catalog C binding is malformed: {key.identity}")
        actual_identity_to_symbol[key.identity] = symbol
    if actual_identity_to_symbol != expected_identity_to_symbol:
        missing = sorted(
            set(expected_identity_to_symbol) - set(actual_identity_to_symbol))
        unexpected = sorted(
            set(actual_identity_to_symbol) - set(expected_identity_to_symbol))
        mismatched = sorted(
            identity
            for identity in (
                set(actual_identity_to_symbol)
                & set(expected_identity_to_symbol)
            )
            if (
                actual_identity_to_symbol[identity]
                != expected_identity_to_symbol[identity]
            )
        )
        raise CatalogParityError(
            "live catalog ↔ manifest C binding bijection failed: "
            + json.dumps({
                "missing": missing,
                "unexpected": unexpected,
                "cross_name": mismatched,
            }, sort_keys=True))

    owners = manifest.get("c_symbol_owners")
    if not isinstance(owners, list):
        raise CatalogParityError(
            "candidate manifest has no C symbol-owner inventory")
    owner_symbols: set[str] = set()
    for owner in owners:
        owner_routines = (
            owner.get("routine_identities")
            if isinstance(owner, dict)
            else None
        )
        if (
            not isinstance(owner, dict)
            or not isinstance(owner.get("symbol"), str)
            or owner.get("finfo_symbol")
            != f"pg_finfo_{owner.get('symbol')}"
            or not isinstance(owner_routines, list)
            or not all(
                isinstance(value, str) for value in owner_routines)
            or owner_routines
            != sorted(set(owner_routines))
            or set(owner_routines)
            != expected_symbol_to_runtime_identities.get(
                str(owner.get("symbol")), set())
            or owner["symbol"] in owner_symbols
        ):
            raise CatalogParityError(
                "candidate manifest C symbol-owner row is malformed")
        owner_symbols.add(owner["symbol"])
    if owner_symbols != set(c_symbols):
        raise CatalogParityError(
            "candidate manifest symbol-owner rows differ from C symbols")
    for entity in entities:
        if not isinstance(entity, dict):
            continue
        binding = entity.get("c_binding")
        stable = entity.get("stable_identity")
        if not isinstance(binding, dict) or not isinstance(stable, str):
            continue
        symbol = binding.get("symbol")
        shared = binding.get("same_symbol_routine_identities")
        if (
            not isinstance(symbol, str)
            or not isinstance(shared, list)
            or shared != sorted(set(shared))
            or set(shared)
            != expected_symbol_to_runtime_identities.get(symbol, set())
        ):
            raise CatalogParityError(
                "candidate manifest entity shared-symbol group differs "
                "from its exact C symbol owner")

    exports = _nm_exports(dso, timeout=timeout)
    missing_c = sorted(c_symbols - exports)
    missing_finfo = sorted(finfo_symbols - exports)
    external_raw = manifest.get("external_legacy_macro_symbols")
    external_symbols = _exact_string_inventory(
        external_raw,
        label="candidate manifest external C symbols",
    )
    edge_closure = _validate_symbol_edge_closure(
        manifest_c_symbols=c_symbols,
        manifest_finfo_symbols=finfo_symbols,
        manifest_owner_symbols=frozenset(owner_symbols),
        manifest_identity_to_symbol=expected_identity_to_symbol,
        catalog_identity_to_symbol=actual_identity_to_symbol,
        exported_symbols=exports,
        external_legacy_symbols=external_symbols,
        expected_identity_count=272,
        expected_c_symbol_count=270,
        expected_finfo_symbol_count=270,
    )
    expected_exported_finfo = finfo_symbols | frozenset(
        f"pg_finfo_{symbol}" for symbol in external_symbols)
    actual_exported_finfo = frozenset(
        symbol for symbol in exports if symbol.startswith("pg_finfo_"))
    unexpected_finfo = sorted(
        actual_exported_finfo - expected_exported_finfo)
    missing_external_finfo = sorted(
        expected_exported_finfo - actual_exported_finfo)
    retired_exports = retired | frozenset(
        f"pg_finfo_{symbol}" for symbol in retired)
    still_exported_retired = sorted(retired_exports & exports)
    prosrc_changes = prosrc_summary.get("prosrc_changes")
    if not isinstance(prosrc_changes, list) or len(prosrc_changes) != 124:
        raise CatalogParityError(
            "cannot reconcile retired symbols without 124 prosrc changes")
    old_symbols = frozenset(
        row["legacy"]
        for row in prosrc_changes
        if isinstance(row, dict) and isinstance(row.get("legacy"), str)
    )
    expected_old = retired | _BPCHAR_LEGACY_SYMBOLS
    unexpected_old = sorted(old_symbols - expected_old)
    missing_old = sorted(expected_old - old_symbols)
    old_symbol_exports = old_symbols | frozenset(
        f"pg_finfo_{symbol}" for symbol in old_symbols)
    still_exported_old = sorted(old_symbol_exports & exports)
    new_symbols = frozenset(
        row["candidate"]
        for row in prosrc_changes
        if isinstance(row, dict) and isinstance(row.get("candidate"), str)
    )
    missing_new = sorted(new_symbols - exports)
    if len(old_symbols) != 123 or len(expected_old) != 123:
        raise CatalogParityError(
            "legacy retired-symbol closure is not exactly 123 symbols")
    failures = {
        "missing_candidate_c_symbols": missing_c,
        "missing_candidate_finfo_symbols": missing_finfo,
        "unexpected_finfo_symbols": unexpected_finfo,
        "missing_external_finfo_symbols": missing_external_finfo,
        "still_exported_retired_symbols": still_exported_retired,
        "unexpected_legacy_changed_symbols": unexpected_old,
        "missing_legacy_changed_symbols": missing_old,
        "still_exported_legacy_changed_symbols": still_exported_old,
        "missing_candidate_changed_symbols": missing_new,
    }
    if any(failures.values()):
        raise CatalogParityError(
            "DSO symbol closure does not match the reconciled catalog: "
            + json.dumps(failures, sort_keys=True))
    return {
        "dso_sha256": _sha256(dso),
        "candidate_c_symbol_count": len(c_symbols),
        "candidate_finfo_symbol_count": len(finfo_symbols),
        "retired_manifest_symbol_count": len(retired),
        "legacy_changed_identity_count": len(prosrc_changes),
        "legacy_changed_unique_symbol_count": len(old_symbols),
        "legacy_missing_symbols": sorted(old_symbols),
        "candidate_changed_symbols": sorted(new_symbols),
        "catalog_manifest_binding_count": len(actual_identity_to_symbol),
        "catalog_manifest_bijection": True,
        "manifest_finfo_derivation_exact": True,
        "edge_closure": edge_closure,
        "nm_export_count": len(exports),
        **failures,
    }


def _validate_candidate_catalog(
    snapshot: Snapshot,
    manifest: Mapping[str, object],
    contract: Mapping[str, object],
) -> dict[str, object]:
    routine_rows = [
        (key, value)
        for key, value in snapshot.rows.items()
        if key.catalog == "pg_proc" and isinstance(value, dict)
    ]
    c_routines = [
        (key, value)
        for key, value in routine_rows
        if value.get("language") == "c"
        and value.get("kind") in {"f", "p"}
    ]
    bad_leaf_symbols = [
        {
            "identity": key.identity,
            "prosrc": value.get("prosrc"),
            "expected": _routine_leaf(key.identity),
        }
        for key, value in c_routines
        if value.get("prosrc") != _routine_leaf(key.identity)
    ]
    symbol_groups: dict[str, list[str]] = collections.defaultdict(list)
    for key, value in c_routines:
        symbol = value.get("prosrc")
        if isinstance(symbol, str):
            symbol_groups[symbol].append(key.identity)
    shared = {
        symbol: sorted(identities)
        for symbol, identities in symbol_groups.items()
        if len(identities) > 1
    }
    expected_shared = {"from_array": 2, "mmsearch": 2}
    observed_shared = {
        symbol: len(identities) for symbol, identities in shared.items()
    }
    comments = snapshot.counts.get("pg_description", 0)
    am_edges = [
        key
        for key, value in snapshot.rows.items()
        if key.catalog == "pg_depend"
        and isinstance(value, dict)
        and value.get("dependency_type") == "n"
        and (
            (
                value.get("object_type") == "access method"
                and value.get("object_identity") == "bioseq"
                and isinstance(value.get("reference_identity"), str)
                and (
                    "bioseq_internal." in value["reference_identity"]
                    or "$extension." in value["reference_identity"]
                )
            )
            or (
                value.get("reference_type") == "access method"
                and value.get("reference_identity") == "bioseq"
                and isinstance(value.get("object_identity"), str)
                and (
                    "bioseq_internal." in value["object_identity"]
                    or "$extension." in value["object_identity"]
                )
            )
        )
    ]
    bpchar_targets = {
        key.identity
        for key, _ in routine_rows
        if any(
            f".{leaf}(" in key.identity
            for leaf in (
                "vbpchareq",
                "vbpcharne",
                "vbpchargt",
                "vbpcharlt",
                "vbpcharle",
                "vbpcharge",
            )
        )
    }
    bpchar_legacy = {
        key.identity
        for key, _ in routine_rows
        if any(f".{leaf}(" in key.identity for leaf in _BPCHAR_LEGACY_SYMBOLS)
    }
    public_stable = manifest.get("public_stable_identities")
    guc_generation = manifest.get("guc_generation")
    expected_guc_count = (
        guc_generation.get("registration_count")
        if isinstance(guc_generation, dict)
        else None
    )
    guc_count = snapshot.counts.get("guc", 0)
    failures: dict[str, object] = {}
    if len(c_routines) != contract.get("candidate_c_routine_identity_count"):
        failures["c_routine_identity_count"] = len(c_routines)
    if bad_leaf_symbols:
        failures["non_leaf_c_symbols"] = bad_leaf_symbols
    if observed_shared != expected_shared:
        failures["symbol_sharing_groups"] = observed_shared
    if comments != contract.get("candidate_catalog_comment_count"):
        failures["catalog_comment_count"] = comments
    if len(am_edges) != contract.get("candidate_am_dependency_count"):
        failures["am_dependency_count"] = len(am_edges)
    if len(bpchar_targets) != 6 or bpchar_legacy:
        failures["bpchar_identity_closure"] = {
            "targets": sorted(bpchar_targets),
            "legacy": sorted(bpchar_legacy),
        }
    if (
        not isinstance(public_stable, list)
        or len(public_stable)
        != contract.get("candidate_public_manifest_identity_count")
    ):
        failures["public_manifest_identity_count"] = (
            len(public_stable) if isinstance(public_stable, list) else None)
    if not isinstance(expected_guc_count, int) or guc_count != expected_guc_count:
        failures["guc_registration_count"] = {
            "catalog": guc_count,
            "manifest": expected_guc_count,
        }
    if failures:
        raise CatalogParityError(
            "candidate catalog invariants failed: "
            + json.dumps(failures, sort_keys=True))
    return {
        "c_routine_identity_count": len(c_routines),
        "c_distinct_link_symbol_count": len(symbol_groups),
        "symbol_sharing_groups": shared,
        "catalog_comment_count": comments,
        "am_dependency_count": len(am_edges),
        "bpchar_target_identities": sorted(bpchar_targets),
        "bpchar_legacy_identity_count": len(bpchar_legacy),
        "public_manifest_identity_count": len(public_stable),
        "guc_registration_count": guc_count,
    }


def _sql_literal(value: str) -> str:
    if "\x00" in value:
        raise CatalogParityError("SQL literal contains NUL")
    return "'" + value.replace("'", "''") + "'"


def _sql_identifier(value: str) -> str:
    if not value or "\x00" in value:
        raise CatalogParityError("SQL identifier is empty or contains NUL")
    return '"' + value.replace('"', '""') + '"'


def _manifest_sql_reference(
    manifest: Mapping[str, object],
    stable_identity: str,
    schema: str,
    *,
    require_public: bool,
) -> str:
    """Resolve one manifest identity without trusting default install schema."""
    entities = manifest.get("entities")
    if not isinstance(entities, list):
        raise CatalogParityError(
            "candidate manifest has no relocation entity inventory")
    matches = [
        entity
        for entity in entities
        if (
            isinstance(entity, dict)
            and entity.get("stable_identity") == stable_identity
        )
    ]
    if len(matches) != 1:
        raise CatalogParityError(
            "candidate manifest does not contain exactly one relocation "
            f"identity {stable_identity!r}")
    if require_public:
        public = manifest.get("public_stable_identities")
        if (
            not isinstance(public, list)
            or public.count(stable_identity) != 1
        ):
            raise CatalogParityError(
                "candidate public manifest does not contain exactly one "
                f"relocation identity {stable_identity!r}")

    prefixes = ("sql:function:", "sql:sql_function:", "sql:type:")
    prefix = next(
        (candidate for candidate in prefixes
         if stable_identity.startswith(candidate)),
        None,
    )
    if prefix is None:
        raise CatalogParityError(
            f"unsupported relocation SQL identity {stable_identity!r}")
    spelling = stable_identity.removeprefix(prefix)
    if prefix != "sql:type:":
        spelling = spelling.split("(", 1)[0]
    spelling = spelling.replace("$extension", schema)
    components = spelling.split(".")
    if (
        len(components) != 2
        or any(not component or "$extension" in component
               for component in components)
    ):
        raise CatalogParityError(
            f"malformed relocation SQL reference {stable_identity!r}")
    return ".".join(_sql_identifier(component) for component in components)


def _manifest_named_sql_function_reference(
    manifest: Mapping[str, object],
    schema: str,
    name: str,
) -> str:
    entities = manifest.get("entities")
    if not isinstance(entities, list):
        raise CatalogParityError(
            "candidate manifest has no relocation entity inventory")
    matches = [
        entity
        for entity in entities
        if (
            isinstance(entity, dict)
            and entity.get("kind") == "sql_function"
            and entity.get("schema") == "$extension"
            and entity.get("schema_mode") == "extension"
            and entity.get("name") == name
            and isinstance(entity.get("stable_identity"), str)
        )
    ]
    if len(matches) != 1:
        raise CatalogParityError(
            "candidate manifest does not contain exactly one extension SQL "
            f"function named {name!r}")
    stable_identity = matches[0]["stable_identity"]
    assert isinstance(stable_identity, str)
    return _manifest_sql_reference(
        manifest,
        stable_identity,
        schema,
        require_public=False,
    )


def _relocated_identity(identity: str, schema: str) -> tuple[str, str]:
    if identity.startswith("sql:type:"):
        return "type", identity.removeprefix("sql:type:").replace(
            "$extension", schema)
    if identity.startswith("sql:function:"):
        return "function", identity.removeprefix("sql:function:").replace(
            "$extension", schema)
    raise CatalogParityError(
        "public relocation probe only accepts function/type identities, "
        f"got {identity!r}")


def _relocation_install_query(schema: str) -> bytes:
    quoted_schema = _sql_identifier(schema)
    return f"""
SET search_path = pg_catalog;
CREATE ROLE doc0388_catalog_probe NOLOGIN;
LOAD '$libdir/bioseq';
DO $assertion$
BEGIN
  IF current_setting('check_function_bodies') <> 'on' THEN
    RAISE EXCEPTION 'relocation candidate disabled function-body checks';
  END IF;
END
$assertion$;
CREATE SCHEMA {quoted_schema};
CREATE EXTENSION bioseq SCHEMA {quoted_schema};
""".encode("utf-8")


def _relocation_queries(
    manifest: Mapping[str, object],
    schema: str,
) -> tuple[bytes, bytes, bytes, bytes, bytes]:
    public = manifest.get("public_stable_identities")
    if not isinstance(public, list) or not all(
        isinstance(value, str) for value in public
    ):
        raise CatalogParityError("public manifest identities are invalid")
    checks: list[str] = []
    for stable in public:
        kind, installed = _relocated_identity(stable, schema)
        literal = _sql_literal(installed)
        stable_literal = _sql_literal(stable)
        if kind == "type":
            resolved = f"pg_catalog.to_regtype({literal})::oid"
            classid = "'pg_catalog.pg_type'::regclass"
        else:
            resolved = f"pg_catalog.to_regprocedure({literal})::oid"
            classid = "'pg_catalog.pg_proc'::regclass"
        checks.append(
            "SELECT "
            f"{stable_literal}::text AS stable_identity, "
            f"({resolved} IS NOT NULL) AS resolved, "
            "EXISTS ("
            " SELECT 1 FROM pg_catalog.pg_depend AS d"
            " JOIN pg_catalog.pg_extension AS e"
            "   ON d.refclassid = 'pg_catalog.pg_extension'::regclass"
            "  AND d.refobjid = e.oid"
            "  AND d.deptype = 'e'"
            " WHERE e.extname = 'bioseq'"
            f"   AND d.classid = {classid}"
            f"   AND d.objid = {resolved}) AS extension_member"
        )
    manifest_query = (
        "SET search_path = pg_catalog;\n"
        "COPY (\n"
        + "\nUNION ALL\n".join(checks)
        + "\nORDER BY stable_identity\n"
        ") TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');\n"
    ).encode("utf-8")
    quoted_schema = _sql_identifier(schema)
    probe_schema = _sql_identifier(_RELOCATION_PROBE_SCHEMA)
    probe_table = f"{probe_schema}.{_sql_identifier('targets')}"
    seq_type = _manifest_sql_reference(
        manifest,
        "sql:type:bioseq.seq",
        schema,
        require_public=True,
    )
    letter_type = _manifest_sql_reference(
        manifest,
        "sql:type:bioseq.letter",
        schema,
        require_public=False,
    )
    aligner_options_type = _manifest_sql_reference(
        manifest,
        "sql:type:$extension.bioseq_aligner_options",
        schema,
        require_public=True,
    )
    sequence_length = _manifest_sql_reference(
        manifest,
        "sql:function:bioseq.len(bioseq.seq)",
        schema,
        require_public=True,
    )
    from_array_two = _manifest_sql_reference(
        manifest,
        "sql:function:bioseq.from_array(bioseq.letter[],integer)",
        schema,
        require_public=False,
    )
    from_array_three = _manifest_sql_reference(
        manifest,
        "sql:function:bioseq.from_array(bioseq.letter[],text,integer)",
        schema,
        require_public=False,
    )
    mmsearch_two = _manifest_sql_reference(
        manifest,
        "sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)",
        schema,
        require_public=True,
    )
    mmsearch_three = _manifest_sql_reference(
        manifest,
        "sql:function:$extension.mmsearch("
        "bioseq.seq,bioseq.seq,$extension.bioseq_aligner_options)",
        schema,
        require_public=True,
    )
    aligner_options = _manifest_named_sql_function_reference(
        manifest, schema, "aligner_options")
    target_literal = (
        f"{_sql_literal(_RELOCATION_TARGET_SEQUENCE)}::"
        f"{seq_type}('dna4', 32)"
    )
    nonmatch_literal = (
        f"{_sql_literal(_RELOCATION_NONMATCH_SEQUENCE)}::"
        f"{seq_type}('dna4', 32)"
    )
    letter_array = (
        "ARRAY["
        f"'A'::{letter_type}('dna4'),"
        f"'C'::{letter_type}('dna4'),"
        f"'G'::{letter_type}('dna4'),"
        f"'T'::{letter_type}('dna4')]"
    )
    aligner_options_regclass = _sql_literal(
        f"{quoted_schema}.{_sql_identifier('bioseq_aligner_options')}")
    smoke_query = f"""
SET search_path = pg_catalog;
SET max_parallel_workers_per_gather = 0;
SET bioseq.update_index_on_insert = on;
SET bioseq.enable_vectorization = on;
CREATE SCHEMA {probe_schema};
CREATE TABLE {probe_table} (
  id integer PRIMARY KEY,
  target {seq_type}('dna4', 32)
) USING bioseq;
INSERT INTO {probe_table} (id, target) VALUES
  (101, {target_literal}),
  (202, {nonmatch_literal}),
  (303, NULL);
COPY (
  SELECT
    (SELECT n.nspname
       FROM pg_catalog.pg_extension AS e
       JOIN pg_catalog.pg_namespace AS n ON n.oid = e.extnamespace
      WHERE e.extname = 'bioseq') AS extension_schema,
    ({_sql_literal(schema)} <> ALL (
       pg_catalog.current_schemas(false))) AS schema_absent_from_search_path,
    {sequence_length}('ACGT'::{seq_type}('dna4', 32))
      AS sequence_length,
    ({aligner_options}(kmer_size => 17)).kmer_size
      AS explicit_kmer_size,
    (SELECT count(*)
       FROM pg_catalog.pg_attribute AS a
      WHERE a.attrelid =
              {aligner_options_regclass}::pg_catalog.regclass
        AND a.attnum > 0
        AND NOT a.attisdropped) AS aligner_option_column_count,
    {from_array_two}({letter_array}, 32)::pg_catalog.text
      AS from_array_arity_2,
    {from_array_three}({letter_array}, 'dna4', 32)::pg_catalog.text
      AS from_array_arity_3,
    (SELECT COALESCE(
              pg_catalog.string_agg(
                p.id::pg_catalog.text, ',' ORDER BY p.id), '')
       FROM {probe_table} AS p
      WHERE {mmsearch_two}(p.target, {target_literal}))
      AS mmsearch_arity_2_ids,
    (SELECT COALESCE(
              pg_catalog.string_agg(
                p.id::pg_catalog.text, ',' ORDER BY p.id), '')
       FROM {probe_table} AS p
      WHERE {mmsearch_three}(
        p.target, {target_literal},
        {aligner_options}(kmer_size => 17)))
      AS mmsearch_arity_3_ids,
    (NULL::{aligner_options_type} IS NULL)
      AS aligner_options_type_resolved
) TO STDOUT WITH (FORMAT csv, HEADER true, ENCODING 'UTF8');
"""
    explain_two_query = f"""
SET search_path = pg_catalog;
SET max_parallel_workers_per_gather = 0;
EXPLAIN (VERBOSE, COSTS off, FORMAT json)
SELECT p.id
  FROM {probe_table} AS p
 WHERE {mmsearch_two}(p.target, {target_literal})
 ORDER BY p.id;
"""
    explain_three_query = f"""
SET search_path = pg_catalog;
SET max_parallel_workers_per_gather = 0;
EXPLAIN (VERBOSE, COSTS off, FORMAT json)
SELECT p.id
  FROM {probe_table} AS p
 WHERE {mmsearch_three}(
   p.target, {target_literal},
   {aligner_options}(kmer_size => 17))
 ORDER BY p.id;
"""
    cleanup_query = f"""
SET search_path = pg_catalog;
DROP SCHEMA {probe_schema} CASCADE;
"""
    return (
        manifest_query,
        smoke_query.encode("utf-8"),
        explain_two_query.encode("utf-8"),
        explain_three_query.encode("utf-8"),
        cleanup_query.encode("utf-8"),
    )


def _relocation_query_artifacts(
    manifest: Mapping[str, object],
    schema: str,
) -> dict[str, bytes]:
    names = (
        "relocation-manifest.sql",
        "relocation-smoke.sql",
        "relocation-explain-arity-2.sql",
        "relocation-explain-arity-3.sql",
        "relocation-cleanup.sql",
    )
    queries = _relocation_queries(manifest, schema)
    if len(names) != len(queries):
        raise CatalogParityError(
            "relocation query artifact inventory is inconsistent")
    return {
        "install-relocated-candidate.sql":
            _relocation_install_query(schema),
        **dict(zip(names, queries)),
    }


def _validate_relocation_query_replay(
    query_root: Path,
    manifest: Mapping[str, object],
    schema: str,
) -> None:
    for name, expected in _relocation_query_artifacts(
        manifest, schema
    ).items():
        _require_exact_bytes(
            query_root / name,
            expected,
            f"retained generated relocation query {name}",
        )


def _run_relocation_probe(
    *,
    cluster: UniquePrivateCluster,
    bindir: Path,
    manifest: Mapping[str, object],
    snapshot_query: Path,
    candidate_snapshot: Snapshot,
    retained_root: Path,
    timeout: int,
) -> dict[str, object]:
    queries = _relocation_query_artifacts(manifest, _UNUSUAL_SCHEMA)
    query_root = retained_root / "queries"
    for name, query in queries.items():
        _write_bytes(query_root / name, query)
    install_query_path = query_root / "install-relocated-candidate.sql"
    _run(
        _psql_command(
            cluster, bindir, "--file", str(install_query_path)),
        timeout=timeout,
        env=cluster.child_environment(),
        log=retained_root / "logs" / "install-relocated-candidate.log",
    )
    manifest_query_path = query_root / "relocation-manifest.sql"
    smoke_query_path = query_root / "relocation-smoke.sql"
    explain_two_query_path = (
        query_root / "relocation-explain-arity-2.sql")
    explain_three_query_path = (
        query_root / "relocation-explain-arity-3.sql")
    cleanup_query_path = query_root / "relocation-cleanup.sql"
    manifest_result = _run(
        _psql_command(
            cluster, bindir, "--file", str(manifest_query_path)),
        timeout=timeout,
        env=cluster.child_environment(),
    )
    smoke_result = _run(
        _psql_command(cluster, bindir, "--file", str(smoke_query_path)),
        timeout=timeout,
        env=cluster.child_environment(),
    )
    explain_two_result = _run(
        _psql_command(
            cluster,
            bindir,
            "--tuples-only",
            "--no-align",
            "--file",
            str(explain_two_query_path),
        ),
        timeout=timeout,
        env=cluster.child_environment(),
    )
    explain_three_result = _run(
        _psql_command(
            cluster,
            bindir,
            "--tuples-only",
            "--no-align",
            "--file",
            str(explain_three_query_path),
        ),
        timeout=timeout,
        env=cluster.child_environment(),
    )
    _run(
        _psql_command(cluster, bindir, "--file", str(cleanup_query_path)),
        timeout=timeout,
        env=cluster.child_environment(),
    )
    manifest_result_path = retained_root / "results" / "relocation-manifest.csv"
    smoke_result_path = retained_root / "results" / "relocation-smoke.csv"
    explain_two_result_path = (
        retained_root / "results" / "relocation-explain-arity-2.json")
    explain_three_result_path = (
        retained_root / "results" / "relocation-explain-arity-3.json")
    _write_bytes(manifest_result_path, manifest_result)
    _write_bytes(smoke_result_path, smoke_result)
    _write_bytes(explain_two_result_path, explain_two_result)
    _write_bytes(explain_three_result_path, explain_three_result)
    relocated_result, server = _snapshot_cluster(
        label="relocated",
        cluster=cluster,
        bindir=bindir,
        query=snapshot_query,
        retained_root=retained_root,
        timeout=timeout,
    )
    return _validate_relocation_results(
        manifest=manifest,
        manifest_result=manifest_result,
        smoke_result=smoke_result,
        explain_two_result=explain_two_result,
        explain_three_result=explain_three_result,
        server=server,
        candidate_snapshot=candidate_snapshot,
        relocated_snapshot=_parse_snapshot(relocated_result),
    )


def _relocation_snapshot_projection(
    snapshot: Snapshot,
) -> dict[SnapshotKey, object]:
    """Remove only containing-schema defaults that CREATE EXTENSION does not own."""
    projected: dict[SnapshotKey, object] = {}
    for key, value in snapshot.rows.items():
        if (
            (key.catalog == "pg_namespace"
             and key.identity == "schema:$extension")
            or (
                key.catalog in {"acl", "effective_privilege"}
                and key.identity.startswith("schema:$extension:")
            )
            or key.catalog == "snapshot_scope"
        ):
            continue
        projected[key] = value
    return projected


def _validate_relocated_catalog_equality(
    candidate_snapshot: Snapshot,
    relocated_snapshot: Snapshot,
) -> int:
    expected_catalog = _relocation_snapshot_projection(candidate_snapshot)
    relocated_catalog = _relocation_snapshot_projection(relocated_snapshot)
    if relocated_catalog != expected_catalog:
        missing = sorted(
            f"{key.catalog}:{key.identity}:{key.ordinal}"
            for key in set(expected_catalog) - set(relocated_catalog)
        )
        unexpected = sorted(
            f"{key.catalog}:{key.identity}:{key.ordinal}"
            for key in set(relocated_catalog) - set(expected_catalog)
        )
        changed = sorted(
            f"{key.catalog}:{key.identity}:{key.ordinal}"
            for key in set(expected_catalog) & set(relocated_catalog)
            if expected_catalog[key] != relocated_catalog[key]
        )
        raise CatalogParityError(
            "relocated full catalog differs from the ordinary extension "
            "catalog: "
            + json.dumps({
                "missing": missing,
                "unexpected": unexpected,
                "changed": changed,
            }, sort_keys=True))
    return len(expected_catalog)


def _validate_relocation_manifest_result(
    manifest: Mapping[str, object],
    manifest_result: bytes,
) -> int:
    try:
        decoded = manifest_result.decode("utf-8", errors="strict")
    except UnicodeError as error:
        raise CatalogParityError(
            "relocated public manifest result is not valid UTF-8") from error
    reader = csv.DictReader(io.StringIO(decoded))
    expected_header = [
        "stable_identity",
        "resolved",
        "extension_member",
    ]
    if reader.fieldnames != expected_header:
        raise CatalogParityError(
            "relocated public manifest result has an invalid CSV header")
    manifest_rows = list(reader)
    public = manifest.get("public_stable_identities")
    if not isinstance(public, list) or not all(
        isinstance(identity, str) for identity in public
    ):
        raise CatalogParityError(
            "candidate public manifest identities are invalid")
    observed_identities = [
        row["stable_identity"] for row in manifest_rows]
    if (
        len(manifest_rows) != len(public)
        or len(set(observed_identities)) != len(observed_identities)
        or observed_identities != sorted(public)
        or any(
            row["resolved"] != "t"
            or row["extension_member"] != "t"
            for row in manifest_rows
        )
    ):
        raise CatalogParityError(
            "relocated public manifest does not close exactly over the "
            "live catalog")
    return len(manifest_rows)


def _validate_relocation_explain(
    explain_result: bytes,
    arity: int,
) -> dict[str, object]:
    if arity not in {2, 3}:
        raise CatalogParityError(
            f"relocated functional EXPLAIN has unsupported arity {arity}")
    try:
        explain_text = explain_result.decode("utf-8", errors="strict")
        explain = json.loads(explain_text)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise CatalogParityError(
            f"relocated {arity}-argument functional EXPLAIN is not valid "
            "UTF-8 JSON") from error
    if (
        not isinstance(explain, list)
        or len(explain) != 1
        or not isinstance(explain[0], dict)
        or not isinstance(explain[0].get("Plan"), dict)
    ):
        raise CatalogParityError(
            f"relocated {arity}-argument functional EXPLAIN has an "
            "invalid document shape")

    plan_nodes: list[Mapping[str, object]] = []

    def collect_plan_nodes(node: Mapping[str, object]) -> None:
        if not isinstance(node.get("Node Type"), str):
            raise CatalogParityError(
                f"relocated {arity}-argument functional EXPLAIN contains "
                "a plan node without a node type")
        plan_nodes.append(node)
        child_plans = node.get("Plans", [])
        if not isinstance(child_plans, list) or not all(
            isinstance(child, dict) for child in child_plans
        ):
            raise CatalogParityError(
                f"relocated {arity}-argument functional EXPLAIN contains "
                "a malformed child plan inventory")
        for child in child_plans:
            collect_plan_nodes(child)

    collect_plan_nodes(explain[0]["Plan"])
    provider_nodes = [
        node
        for node in plan_nodes
        if (
            node.get("Node Type") == "Custom Scan"
            and node.get("Custom Plan Provider") == "BioseqScan"
        )
    ]
    if (
        len(provider_nodes) != 1
        or provider_nodes[0].get("Schema")
        != _RELOCATION_PROBE_SCHEMA
        or provider_nodes[0].get("Relation Name") != "targets"
    ):
        raise CatalogParityError(
            f"relocated {arity}-argument functional EXPLAIN does not prove "
            "exactly one BioseqScan on the qualified probe relation")
    return {
        "sha256": _sha256_bytes(explain_result),
        "format": "json",
        "arity": arity,
        "custom_scan_provider": "BioseqScan",
        "probe_relation": (
            f"{_RELOCATION_PROBE_SCHEMA}.targets"
        ),
        "nonempty": True,
    }


def _validate_relocation_results(
    *,
    manifest: Mapping[str, object],
    manifest_result: bytes,
    smoke_result: bytes,
    explain_two_result: bytes,
    explain_three_result: bytes,
    server: Mapping[str, str],
    candidate_snapshot: Snapshot,
    relocated_snapshot: Snapshot,
) -> dict[str, object]:
    manifest_row_count = _validate_relocation_manifest_result(
        manifest, manifest_result)
    expected_smoke = {
        "extension_schema": _UNUSUAL_SCHEMA,
        "schema_absent_from_search_path": "t",
        "sequence_length": "4",
        "explicit_kmer_size": "17",
        "aligner_option_column_count": "36",
        "from_array_arity_2": "ACGT",
        "from_array_arity_3": "ACGT",
        "mmsearch_arity_2_ids": "101",
        "mmsearch_arity_3_ids": "101",
        "aligner_options_type_resolved": "t",
    }
    smoke_reader = csv.DictReader(
        io.StringIO(smoke_result.decode("utf-8", errors="strict")))
    if smoke_reader.fieldnames != list(expected_smoke):
        raise CatalogParityError(
            "relocated package smoke query returned the wrong CSV header")
    smoke_rows = list(smoke_reader)
    if len(smoke_rows) != 1:
        raise CatalogParityError(
            "relocated package smoke query returned the wrong row count")
    smoke = smoke_rows[0]
    if smoke != expected_smoke:
        raise CatalogParityError(
            f"relocated package smoke mismatch: {smoke!r}")
    explain_two = _validate_relocation_explain(explain_two_result, 2)
    explain_three = _validate_relocation_explain(explain_three_result, 3)
    full_catalog_row_count = _validate_relocated_catalog_equality(
        candidate_snapshot,
        relocated_snapshot,
    )
    entities = manifest.get("entities")
    if not isinstance(entities, list):
        raise CatalogParityError(
            "candidate manifest has no full relocation entity inventory")
    extension_entities = [
        entity
        for entity in entities
        if (
            isinstance(entity, dict)
            and isinstance(entity.get("stable_identity"), str)
            and "$extension" in entity["stable_identity"]
        )
    ]
    if not extension_entities:
        raise CatalogParityError(
            "candidate manifest has no $extension relocation closure")
    unexpected_public_members = sorted(
        key.identity
        for key in relocated_snapshot.rows
        if (
            key.catalog == "extension_membership"
            and (
                " public." in key.identity
                or key.identity.startswith("public.")
                or ":public." in key.identity
            )
        )
    )
    if unexpected_public_members:
        raise CatalogParityError(
            "relocated extension left extension-owned objects in public: "
            + ", ".join(unexpected_public_members))
    return {
        "schema": _UNUSUAL_SCHEMA,
        "public_manifest_rows": manifest_row_count,
        "resolved_rows": manifest_row_count,
        "extension_member_rows": manifest_row_count,
        "manifest_extension_entity_count": len(extension_entities),
        "full_catalog_row_count": full_catalog_row_count,
        "full_catalog_byte_equal_after_schema_normalization": True,
        "unexpected_public_extension_members": [],
        "smoke": smoke,
        "explain": {
            "arity_2": explain_two,
            "arity_3": explain_three,
        },
        "server": dict(server),
    }


def _jsonl_bytes(rows: Iterable[Mapping[str, object]]) -> bytes:
    return b"".join(_canonical_json(dict(row)) for row in rows)


def _write_jsonl(path: Path, rows: Iterable[Mapping[str, object]]) -> None:
    _write_bytes(path, _jsonl_bytes(rows))


def _row_count(path: Path) -> int:
    with path.open("r", encoding="utf-8", newline="") as stream:
        return sum(1 for _ in csv.DictReader(stream))


def _payload_records(bundle: Path) -> dict[str, dict[str, object]]:
    records: dict[str, dict[str, object]] = {}
    for path in sorted(bundle.rglob("*")):
        relative = path.relative_to(bundle).as_posix()
        metadata = path.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            raise CatalogParityError(
                f"evidence bundle contains a symlink: {relative}")
        if stat.S_ISDIR(metadata.st_mode):
            continue
        if not stat.S_ISREG(metadata.st_mode):
            raise CatalogParityError(
                f"evidence bundle contains a non-regular file: {relative}")
        if relative in _IGNORED_BUNDLE_FILES:
            continue
        records[relative] = {
            "sha256": _sha256(path),
            "size": metadata.st_size,
        }
    return records


def _seal_bundle(
    bundle: Path,
    evidence: dict[str, object],
) -> None:
    if "payloads" in evidence:
        raise CatalogParityError("evidence report was already sealed")
    evidence["payloads"] = _payload_records(bundle)
    report_path = bundle / "evidence.json"
    _write_json(report_path, evidence)
    _write_bytes(
        bundle / "bundle.sha256",
        (_sha256(report_path) + "  evidence.json\n").encode("ascii"),
    )


def _require_exact_bytes(path: Path, expected: bytes, label: str) -> None:
    retained = _regular_file(path, label).read_bytes()
    if retained != expected:
        raise CatalogParityError(
            f"{label} does not equal the independently regenerated bytes")


def _validate_live_bundle(
    *,
    bundle: Path,
    evidence: Mapping[str, object],
    input_provenance: Mapping[str, object],
    expected_legacy_sha256: str,
    timeout: int,
) -> None:
    legacy_path = _regular_file(
        bundle / "inputs" / "legacy.sql", "retained legacy SQL")
    candidate_sql = _regular_file(
        bundle / "inputs" / "candidate.sql", "retained candidate SQL")
    manifest_path = _regular_file(
        bundle / "inputs" / "candidate-manifest.json",
        "retained candidate manifest",
    )
    delta_path = _regular_file(
        bundle / "inputs" / "approved-delta.json",
        "retained approved delta",
    )
    guc_ledger_path = _regular_file(
        bundle / "inputs" / "guc-parity-ledger.json",
        "retained independent GUC parity ledger",
    )
    candidate_dso = _regular_file(
        bundle / "inputs" / "candidate-dso", "retained candidate DSO")
    dso_provenance = input_provenance.get("candidate_dso")
    if (
        not isinstance(dso_provenance, dict)
        or dso_provenance.get("sha256") != _sha256(candidate_dso)
        or dso_provenance.get("size") != candidate_dso.stat().st_size
    ):
        raise CatalogParityError(
            "retained candidate DSO differs from input provenance")

    legacy_bytes = legacy_path.read_bytes()
    if _sha256_bytes(legacy_bytes) != expected_legacy_sha256:
        raise CatalogParityError("retained live legacy SQL hash drifted")
    delta = _load_json(delta_path, "retained approved delta")
    contract = _validate_delta(delta, legacy_bytes)
    manifest = _load_json(manifest_path, "retained candidate manifest")
    _validate_manifest(manifest, candidate_sql, contract)
    _validate_relocation_query_replay(
        bundle / "queries",
        manifest,
        _UNUSUAL_SCHEMA,
    )

    legacy_snapshot = _parse_snapshot(
        _regular_file(
            bundle / "results" / "legacy.csv",
            "retained legacy normalized catalog",
        ))
    candidate_snapshot = _parse_snapshot(
        _regular_file(
            bundle / "results" / "candidate.csv",
            "retained candidate normalized catalog",
        ))
    active_profile = input_provenance.get("active_profile")
    if active_profile not in {"production", "test_indexes"}:
        raise CatalogParityError(
            "retained evidence has an invalid active build profile")
    guc_authority = _validate_guc_authority(
        manifest=manifest,
        ledger=_load_json(
            guc_ledger_path, "retained independent GUC parity ledger"),
        ledger_path=guc_ledger_path,
        active_profile=active_profile,
        snapshot=candidate_snapshot,
    )
    approved, unapproved, prosrc_summary = _compare_snapshots(
        legacy_snapshot, candidate_snapshot, delta, contract)
    full_diff = sorted(
        [*approved, *unapproved],
        key=lambda row: (
            str(row["catalog"]),
            str(row["identity"]),
            int(row["ordinal"]),
            str(row["property"]),
        ),
    )
    for name, rows in (
        ("full", full_diff),
        ("approved", approved),
        ("unapproved", unapproved),
    ):
        _require_exact_bytes(
            bundle / "diffs" / f"{name}.jsonl",
            _jsonl_bytes(rows),
            f"retained {name} catalog diff",
        )
    _require_exact_bytes(
        bundle / "diffs" / "untrusted-catalog-changes-candidate.jsonl",
        _jsonl_bytes(_untrusted_catalog_change_candidates(unapproved)),
        "retained untrusted catalog-change diagnostic",
    )

    expected_counts = {
        "legacy": {
            "total": len(legacy_snapshot.rows),
            "by_catalog": legacy_snapshot.counts,
        },
        "candidate": {
            "total": len(candidate_snapshot.rows),
            "by_catalog": candidate_snapshot.counts,
        },
    }
    for label in ("legacy", "candidate"):
        _require_exact_bytes(
            bundle / "results" / f"{label}-counts.json",
            _canonical_json(expected_counts[label]),
            f"retained {label} catalog counts",
        )
    if evidence.get("row_counts") != expected_counts:
        raise CatalogParityError(
            "reported catalog row counts were not independently derived")

    candidate_invariants = _validate_candidate_catalog(
        candidate_snapshot, manifest, contract)
    symbol_closure = _validate_symbol_closure(
        manifest=manifest,
        dso=candidate_dso,
        candidate_snapshot=candidate_snapshot,
        prosrc_summary=prosrc_summary,
        timeout=timeout,
    )
    legacy_server = _parse_server_metadata(
        _regular_file(
            bundle / "results" / "legacy-server.csv",
            "retained legacy server metadata",
        ).read_bytes(),
        "retained legacy",
    )
    candidate_server = _parse_server_metadata(
        _regular_file(
            bundle / "results" / "candidate-server.csv",
            "retained candidate server metadata",
        ).read_bytes(),
        "retained candidate",
    )
    relocated_server = _parse_server_metadata(
        _regular_file(
            bundle / "results" / "relocated-server.csv",
            "retained relocated server metadata",
        ).read_bytes(),
        "retained relocated",
    )
    system_identifiers = {
        legacy_server["system_identifier"],
        candidate_server["system_identifier"],
        relocated_server["system_identifier"],
    }
    if len(system_identifiers) != 3:
        raise CatalogParityError(
            "retained server metadata does not prove three clean nodes")
    relocation = _validate_relocation_results(
        manifest=manifest,
        manifest_result=_regular_file(
            bundle / "results" / "relocation-manifest.csv",
            "retained relocation manifest result",
        ).read_bytes(),
        smoke_result=_regular_file(
            bundle / "results" / "relocation-smoke.csv",
            "retained relocation smoke result",
        ).read_bytes(),
        explain_two_result=_regular_file(
            bundle / "results" / "relocation-explain-arity-2.json",
            "retained two-argument relocation EXPLAIN result",
        ).read_bytes(),
        explain_three_result=_regular_file(
            bundle / "results" / "relocation-explain-arity-3.json",
            "retained three-argument relocation EXPLAIN result",
        ).read_bytes(),
        server=relocated_server,
        candidate_snapshot=candidate_snapshot,
        relocated_snapshot=_parse_snapshot(_regular_file(
            bundle / "results" / "relocated.csv",
            "retained relocated normalized catalog",
        )),
    )

    regenerated_results: dict[str, object] = {
        "prosrc-reconciliation.json": prosrc_summary,
        "candidate-invariants.json": candidate_invariants,
        "guc-authority.json": guc_authority,
        "symbol-closure.json": symbol_closure,
        "relocation.json": relocation,
    }
    for name, document in regenerated_results.items():
        _require_exact_bytes(
            bundle / "results" / name,
            _canonical_json(document),
            f"retained {name}",
        )

    candidate_stage = _load_json(
        _regular_file(
            bundle / "stages" / "candidate-manifest.json",
            "retained candidate stage manifest",
        ),
        "retained candidate stage manifest",
    )
    legacy_stage = _load_json(
        _regular_file(
            bundle / "stages" / "legacy-manifest.json",
            "retained legacy stage manifest",
        ),
        "retained legacy stage manifest",
    )
    if (
        candidate_stage.get("schema") != "stage_manifest/v1"
        or legacy_stage.get("schema") != "stage_manifest/v1"
        or candidate_stage.get("postgresql_version")
        != legacy_stage.get("postgresql_version")
        or not str(candidate_stage.get("postgresql_version", "")).startswith(
            "PostgreSQL 17"
        )
    ):
        raise CatalogParityError(
            "retained stages do not prove the same PostgreSQL 17 toolchain")
    for label, stage, expected_sql_hash in (
        ("candidate", candidate_stage, _sha256(candidate_sql)),
        ("legacy", legacy_stage, _sha256(legacy_path)),
    ):
        artifacts = stage.get("bioseq_artifacts")
        install_record = (
            artifacts.get("install_sql")
            if isinstance(artifacts, dict)
            else None
        )
        library_record = (
            artifacts.get("library")
            if isinstance(artifacts, dict)
            else None
        )
        if (
            not isinstance(install_record, dict)
            or install_record.get("sha256") != expected_sql_hash
            or not isinstance(library_record, dict)
            or library_record.get("sha256") != _sha256(candidate_dso)
        ):
            raise CatalogParityError(
                f"retained {label} stage artifact hashes drifted")
    expected_postgresql = {
        "stage_version": candidate_stage.get("postgresql_version"),
        "legacy_server": legacy_server,
        "candidate_server": candidate_server,
        "relocated_server": relocated_server,
        "distinct_system_identifiers": True,
    }
    if evidence.get("postgresql") != expected_postgresql:
        raise CatalogParityError(
            "reported PostgreSQL provenance was not independently derived")

    expected_query_hashes = {
        path.relative_to(bundle).as_posix(): _sha256(path)
        for path in sorted((bundle / "queries").glob("*.sql"))
        if path.is_file()
    }
    if evidence.get("queries") != expected_query_hashes:
        raise CatalogParityError(
            "reported query inventory was not independently derived")
    expected_result_hashes = {
        path.relative_to(bundle).as_posix(): _sha256(path)
        for path in sorted((bundle / "results").glob("*"))
        if path.is_file()
    }
    if evidence.get("result_hashes") != expected_result_hashes:
        raise CatalogParityError(
            "reported result hashes were not independently derived")

    expected_diff = {
        "total_difference_count": len(full_diff),
        "approved_difference_count": len(approved),
        "unapproved_difference_count": len(unapproved),
        "approved_categories": _category_counts(approved),
    }
    if evidence.get("diff") != expected_diff:
        raise CatalogParityError(
            "reported diff counts/categories were not independently derived")
    expected_application = {
        "delta_sha256": _sha256(delta_path),
        "prosrc_reconciliation": prosrc_summary,
        "candidate_invariants": candidate_invariants,
        "guc_authority": guc_authority,
        "symbol_closure": symbol_closure,
        "relocation": relocation,
    }
    if evidence.get("approved_delta_application") != expected_application:
        raise CatalogParityError(
            "reported approved-delta application was not independently derived")
    expected_verdict = {
        "unapproved_difference_count": len(unapproved),
        "zero_unapproved_differences": not unapproved,
        "passed": not unapproved,
    }
    if evidence.get("verdict") != expected_verdict or unapproved:
        raise CatalogParityError(
            "catalog verdict does not equal the recomputed closed-delta result")

    if (
        active_profile not in {"production", "test_indexes"}
        or evidence.get("active_build_profile") != active_profile
    ):
        raise CatalogParityError(
            "active build profile differs from sealed input provenance")
    legacy_install = evidence.get("legacy_catalog_install")
    candidate_install = evidence.get("candidate_install")
    if (
        not isinstance(legacy_install, dict)
        or legacy_install.get("catalog_only") is not True
        or legacy_install.get("check_function_bodies") != "off"
        or legacy_install.get("current_dso_sha256") != _sha256(candidate_dso)
        or legacy_install.get("legacy_sql_sha256")
        != _sha256_bytes(legacy_bytes)
        or not isinstance(candidate_install, dict)
        or candidate_install.get("check_function_bodies") != "on"
        or candidate_install.get("candidate_sql_sha256")
        != _sha256(candidate_sql)
        or candidate_install.get("candidate_manifest_sha256")
        != _sha256(manifest_path)
        or candidate_install.get("candidate_dso_sha256")
        != _sha256(candidate_dso)
    ):
        raise CatalogParityError(
            "reported install policy/hashes differ from retained inputs")


def _validate_bundle(
    bundle: Path,
    *,
    expected_legacy_sha256: str = _EXPECTED_LEGACY_SHA256,
    require_live: bool = False,
    timeout: int = 120,
) -> dict[str, object]:
    bundle = _real_directory(bundle, "catalog evidence bundle", create=False)
    report_path = _regular_file(bundle / "evidence.json", "evidence report")
    seal_path = _regular_file(bundle / "bundle.sha256", "evidence seal")
    seal_fields = seal_path.read_text(encoding="ascii").strip().split()
    if (
        len(seal_fields) != 2
        or seal_fields[1] != "evidence.json"
        or seal_fields[0] != _sha256(report_path)
    ):
        raise CatalogParityError("evidence report seal is invalid")
    evidence = _load_json(report_path, "evidence report")
    if evidence.get("schema") != _BUNDLE_SCHEMA:
        raise CatalogParityError("evidence report schema is invalid")
    input_provenance = evidence.get("input_provenance")
    if not isinstance(input_provenance, dict):
        raise CatalogParityError("evidence report has no input provenance")
    recomputed_bundle_id = _sha256_bytes(
        _canonical_json(input_provenance))
    if evidence.get("bundle_id") != recomputed_bundle_id:
        raise CatalogParityError(
            "evidence bundle ID does not seal its input provenance")
    payloads = evidence.get("payloads")
    if not isinstance(payloads, dict):
        raise CatalogParityError("evidence report has no payload inventory")
    actual_payloads = _payload_records(bundle)
    if payloads != actual_payloads:
        raise CatalogParityError("evidence payload count/hash inventory drifted")
    legacy = _regular_file(bundle / "inputs" / "legacy.sql", "legacy SQL")
    if _sha256(legacy) != expected_legacy_sha256:
        raise CatalogParityError("retained legacy SQL SHA-256 drifted")
    retained_inputs = {
        "candidate_sql": bundle / "inputs" / "candidate.sql",
        "candidate_manifest": bundle / "inputs" / "candidate-manifest.json",
        "snapshot_query": bundle / "queries" / "catalog-snapshot.sql",
        "approved_delta": bundle / "inputs" / "approved-delta.json",
        "guc_parity_ledger":
            bundle / "inputs" / "guc-parity-ledger.json",
        "orchestrator":
            bundle / "inputs" / "run_private_catalog_parity.py",
        "private_postgres_helper":
            bundle / "inputs" / "private_postgres.py",
    }
    for provenance_key, retained_path in retained_inputs.items():
        provenance = input_provenance.get(provenance_key)
        if (
            not isinstance(provenance, dict)
            or provenance.get("sha256")
            != _sha256(_regular_file(
                retained_path, f"retained {provenance_key}"))
        ):
            raise CatalogParityError(
                f"retained {provenance_key} differs from input provenance")
    row_counts = evidence.get("row_counts")
    if not isinstance(row_counts, dict):
        raise CatalogParityError("evidence report has no row counts")
    for label in ("legacy", "candidate"):
        result = _regular_file(
            bundle / "results" / f"{label}.csv",
            f"{label} normalized catalog result",
        )
        record = row_counts.get(label)
        if not isinstance(record, dict) or record.get("total") != _row_count(result):
            raise CatalogParityError(
                f"{label} normalized result row count was tampered")
    unapproved_path = _regular_file(
        bundle / "diffs" / "unapproved.jsonl",
        "unapproved catalog diff",
    )
    verdict = evidence.get("verdict")
    if (
        unapproved_path.stat().st_size != 0
        or not isinstance(verdict, dict)
        or verdict.get("unapproved_difference_count") != 0
        or verdict.get("zero_unapproved_differences") is not True
        or verdict.get("passed") is not True
    ):
        raise CatalogParityError(
            "catalog evidence does not have a zero-unapproved-diff verdict")
    fresh_run_nonce = input_provenance.get("fresh_run_nonce")
    if not _is_canonical_uuid4_hex(fresh_run_nonce):
        raise CatalogParityError(
            "catalog evidence has no sealed canonical UUIDv4 fresh-run nonce")
    is_live = evidence.get("evidence_kind") == "live_catalog_parity"
    if require_live and not is_live:
        raise CatalogParityError(
            "catalog evidence is not a live parity bundle")
    if is_live:
        _validate_live_bundle(
            bundle=bundle,
            evidence=evidence,
            input_provenance=input_provenance,
            expected_legacy_sha256=expected_legacy_sha256,
            timeout=timeout,
        )
    return evidence


def _self_test_bundle(root: Path, *, unapproved: bool = False) -> tuple[Path, str]:
    bundle = root / ("unapproved" if unapproved else "valid")
    bundle.mkdir()
    legacy_bytes = b"legacy-catalog-fixture\n"
    _write_bytes(bundle / "inputs" / "legacy.sql", legacy_bytes)
    retained_fixture_inputs = {
        "candidate_sql": (
            bundle / "inputs" / "candidate.sql",
            b"candidate\n",
        ),
        "candidate_manifest": (
            bundle / "inputs" / "candidate-manifest.json",
            b"{}\n",
        ),
        "snapshot_query": (
            bundle / "queries" / "catalog-snapshot.sql",
            b"SELECT 1;\n",
        ),
        "approved_delta": (
            bundle / "inputs" / "approved-delta.json",
            b"{}\n",
        ),
        "guc_parity_ledger": (
            bundle / "inputs" / "guc-parity-ledger.json",
            b"{}\n",
        ),
        "orchestrator": (
            bundle / "inputs" / "run_private_catalog_parity.py",
            b"fixture orchestrator\n",
        ),
        "private_postgres_helper": (
            bundle / "inputs" / "private_postgres.py",
            b"fixture lifecycle helper\n",
        ),
    }
    input_provenance: dict[str, object] = {
        "legacy_sql": {
            "sha256": _sha256_bytes(legacy_bytes),
            "size": len(legacy_bytes),
        },
        "fresh_run_nonce": "00000000000040008000000000000000",
    }
    for key, (path, data) in retained_fixture_inputs.items():
        _write_bytes(path, data)
        input_provenance[key] = {
            "path": f"/fixture/{path.name}",
            "sha256": _sha256_bytes(data),
            "size": len(data),
        }
    csv_bytes = (
        b"catalog,identity,ordinal,value\r\n"
        b"pg_proc,function:fixture(),1,\"{\"\"prosrc\"\":\"\"fixture\"\"}\"\r\n"
    )
    _write_bytes(bundle / "results" / "legacy.csv", csv_bytes)
    _write_bytes(bundle / "results" / "candidate.csv", csv_bytes)
    _write_bytes(bundle / "diffs" / "approved.jsonl", b"")
    _write_bytes(
        bundle / "diffs" / "unapproved.jsonl",
        b"{}\n" if unapproved else b"",
    )
    _seal_bundle(
        bundle,
        {
            "schema": _BUNDLE_SCHEMA,
            "bundle_id": _sha256_bytes(
                _canonical_json(input_provenance)),
            "input_provenance": input_provenance,
            "row_counts": {
                "legacy": {"total": 1},
                "candidate": {"total": 1},
            },
            "verdict": {
                "unapproved_difference_count": 1 if unapproved else 0,
                "zero_unapproved_differences": not unapproved,
                "passed": not unapproved,
            },
        },
    )
    return bundle, _sha256_bytes(legacy_bytes)


def _rewrite_report_and_seal(
    bundle: Path,
    mutator,
) -> None:
    report_path = bundle / "evidence.json"
    report = _load_json(report_path, "self-test evidence")
    mutator(report)
    report_path.write_bytes(_canonical_json(report))
    (bundle / "bundle.sha256").write_text(
        _sha256(report_path) + "  evidence.json\n",
        encoding="ascii",
    )


def run_self_tests() -> None:
    if (
        _manifest_snapshot_identity(
            "sql:function:$extension.fixture(time,timestamp,timestamptz)")
        != (
            "function:$extension.fixture("
            "time without time zone,"
            "timestamp without time zone,"
            "timestamp with time zone)"
        )
        or _manifest_runtime_identity(
            "sql:function:$extension.fixture(int4)")
        != "fixture(int4)"
    ):
        raise CatalogParityError(
            "catalog parity self-test failed routine identity normalization")
    with tempfile.TemporaryDirectory(prefix="doc0388_catalog_selftest_") as raw:
        root = Path(raw)
        valid, expected_legacy = _self_test_bundle(root)
        _validate_bundle(valid, expected_legacy_sha256=expected_legacy)

        def expect_failure(label: str, action) -> None:
            try:
                action()
            except CatalogParityError:
                return
            raise CatalogParityError(
                f"catalog parity self-test did not reject {label}")

        change_rows = [
            {
                "category": "derived_routine_symbol",
                "catalog": "pg_proc",
                "identity": "function:$extension.fixture()",
                "ordinal": 1,
                "property": "prosrc",
                "legacy": {"present": False},
                "candidate": {"present": True, "value": None},
            },
            {
                "category": "standardized_routine_identity",
                "catalog": "pg_type",
                "identity": "type:$extension.fixture",
                "ordinal": 1,
                "property": "input",
                "legacy": {"present": True, "value": "legacy_input"},
                "candidate": {
                    "present": True,
                    "value": "candidate_input",
                },
            },
        ]
        change_contract = {
            "approved_catalog_change_count": len(change_rows),
            "approved_catalog_change_counts_by_category": {
                "derived_routine_symbol": 1,
                "standardized_routine_identity": 1,
            },
        }
        validated_change_rows = _catalog_change_rows(
            {"catalog_changes": change_rows},
            change_contract,
        )
        change_authority = {
            (
                str(row["catalog"]),
                str(row["identity"]),
                int(row["ordinal"]),
                str(row["property"]),
            ): row
            for row in validated_change_rows
        }
        consumed_changes: set[tuple[str, str, int, str]] = set()
        matched_category = _match_catalog_change_authority(
            authority=change_authority,
            consumed=consumed_changes,
            key=SnapshotKey(
                "pg_proc",
                "function:$extension.fixture()",
                1,
            ),
            property_name="prosrc",
            legacy_value=dataclasses.MISSING,
            candidate_value=None,
            observed_category="derived_routine_symbol",
        )
        if (
            matched_category != "derived_routine_symbol"
            or len(consumed_changes) != 1
            or len(set(change_authority) - consumed_changes) != 1
        ):
            raise CatalogParityError(
                "catalog parity self-test failed exact authority consumption")
        if _match_catalog_change_authority(
            authority=change_authority,
            consumed=set(),
            key=SnapshotKey(
                "pg_proc",
                "function:$extension.fixture()",
                1,
            ),
            property_name="prosrc",
            legacy_value=None,
            candidate_value=None,
            observed_category="derived_routine_symbol",
        ) is not None:
            raise CatalogParityError(
                "catalog parity self-test conflated absent and explicit null")
        if _match_catalog_change_authority(
            authority=change_authority,
            consumed=set(),
            key=SnapshotKey(
                "pg_proc",
                "function:$extension.fixture()",
                1,
            ),
            property_name="prosrc",
            legacy_value=dataclasses.MISSING,
            candidate_value=None,
            observed_category="native_routine_replacement",
        ) is not None:
            raise CatalogParityError(
                "catalog parity self-test accepted a category-wide allowance")
        for ledger_name in (
            "routine_symbol_changes",
            "input_name_changes",
            "catalog_comment_changes",
            "catalog_changes",
        ):
            unconsumed_diagnostics = (
                _unconsumed_exact_ledger_diagnostics({
                    ledger_name: [{"fixture": ledger_name}],
                })
            )
            if (
                len(unconsumed_diagnostics) != 1
                or unconsumed_diagnostics[0]["identity"] != ledger_name
                or unconsumed_diagnostics[0]["property"]
                != "unconsumed_exact_rows"
            ):
                raise CatalogParityError(
                    "catalog parity self-test failed to reject unconsumed "
                    f"{ledger_name} authority")

        duplicate_changes = [dict(row) for row in change_rows]
        duplicate_changes.append(dict(change_rows[0]))
        expect_failure(
            "a duplicate exact catalog-change locator",
            lambda: _catalog_change_rows(
                {"catalog_changes": duplicate_changes},
                {
                    "approved_catalog_change_count": 3,
                    "approved_catalog_change_counts_by_category": {
                        "derived_routine_symbol": 2,
                        "standardized_routine_identity": 1,
                    },
                },
            ),
        )
        expect_failure(
            "order-dependent catalog-change authority",
            lambda: _catalog_change_rows(
                {"catalog_changes": list(reversed(change_rows))},
                change_contract,
            ),
        )
        bad_category = [dict(row) for row in change_rows]
        bad_category[0]["category"] = "broad_property_allowance"
        expect_failure(
            "an unknown catalog-change category",
            lambda: _catalog_change_rows(
                {"catalog_changes": bad_category},
                change_contract,
            ),
        )
        bad_catalog = [dict(row) for row in change_rows]
        bad_catalog[0]["catalog"] = "pg_unreviewed"
        expect_failure(
            "an unqueried catalog-change authority",
            lambda: _catalog_change_rows(
                {"catalog_changes": bad_catalog},
                change_contract,
            ),
        )
        expect_failure(
            "a catalog-change count mismatch",
            lambda: _catalog_change_rows(
                {"catalog_changes": change_rows},
                {
                    **change_contract,
                    "approved_catalog_change_count": 1,
                },
            ),
        )

        observed_difference = {
            "catalog": "pg_proc",
            "identity": "function:$extension.fixture()",
            "ordinal": 1,
            "property": "prosrc",
            "legacy": {"present": False},
            "candidate": {"present": True, "value": None},
            "observed_category": "derived_routine_symbol",
            "approved_category": None,
        }
        untrusted = _untrusted_catalog_change_candidates([
            observed_difference,
            {
                "catalog": "summary",
                "identity": "fixture",
                "ordinal": 1,
                "property": "count",
                "legacy": 1,
                "candidate": 2,
            },
        ])
        if (
            len(untrusted) != 1
            or set(untrusted[0]) != {
                "observed_category",
                "catalog",
                "identity",
                "ordinal",
                "property",
                "legacy",
                "candidate",
            }
            or "category" in untrusted[0]
        ):
            raise CatalogParityError(
                "untrusted catalog diagnostic became promotable authority")
        expect_failure(
            "duplicate untrusted catalog diagnostics",
            lambda: _untrusted_catalog_change_candidates([
                observed_difference,
                observed_difference,
            ]),
        )

        if (
            _literal_guc_value(
                "DEFAULT_CHUNK_ROW_COUNT",
                "integer",
                label="self-test named GUC default",
                required=True,
            )
            != "10000"
            or _literal_guc_value(
                "1 << 20",
                "integer",
                label="self-test shifted GUC bound",
                required=True,
            )
            != "1048576"
            or _literal_guc_value(
                "200U",
                "integer",
                label="self-test suffixed GUC literal",
                required=True,
            )
            != "200"
            or _literal_guc_value(
                "100000000000000000000",
                "real",
                label="self-test exponent-form GUC real",
                required=True,
            )
            != "1e+20"
            or _literal_guc_value(
                "-0.0",
                "real",
                label="self-test negative-zero GUC real",
                required=True,
            )
            != "-0"
            or _literal_guc_value(
                "-0",
                "real",
                label="self-test integer-zero GUC real",
                required=True,
            )
            != "0"
            or _literal_guc_value(
                None,
                "integer",
                label="self-test optional GUC bound",
                required=False,
            )
            is not None
        ):
            raise CatalogParityError(
                "catalog parity self-test failed GUC constant resolution")
        expect_failure(
            "a negative unsigned GUC integer literal",
            lambda: _literal_guc_value(
                "-1U",
                "integer",
                label="self-test negative unsigned GUC literal",
                required=True,
            ),
        )
        expect_failure(
            "an octal-looking integer-to-real GUC literal",
            lambda: _literal_guc_value(
                "010",
                "real",
                label="self-test ambiguous integer-to-real GUC literal",
                required=True,
            ),
        )
        expect_failure(
            "an unreviewed named GUC integer constant",
            lambda: _literal_guc_value(
                "UNREVIEWED_LIMIT",
                "integer",
                label="self-test unreviewed GUC constant",
                required=True,
            ),
        )
        expect_failure(
            "a missing required GUC scalar expression",
            lambda: _literal_guc_value(
                None,
                "real",
                label="self-test missing GUC scalar",
                required=True,
            ),
        )
        expect_failure(
            "an unsupported non-null optional GUC expression",
            lambda: _literal_guc_value(
                "fixture_mode::off",
                "enum",
                label="self-test optional GUC expression",
                required=False,
            ),
        )
        expect_failure(
            "a chained GUC integer shift",
            lambda: _literal_guc_value(
                "1 << 2 << 3",
                "integer",
                label="self-test chained GUC shift",
                required=True,
            ),
        )
        expect_failure(
            "an overflowing GUC integer shift",
            lambda: _literal_guc_value(
                "1 << 31",
                "integer",
                label="self-test overflowing GUC shift",
                required=True,
            ),
        )

        primitive_guc = {
            "registration_ordinal": 0,
            "source_owner": "column",
            "name": "bioseq.fixture",
            "group": "bioseq",
            "member": "fixture",
            "kind": "bool",
            "storage_kind": "bool",
            "storage": "legacy_fixture",
            "default_expression": "true",
            "minimum_expression": None,
            "maximum_expression": None,
            "context": "user_set",
            "flags": [],
            "unit": None,
            "hooks": [None, None, None],
            "feature_predicate": "true",
            "profiles": ["production", "test_indexes"],
            "baseline_legacy_description_sha256": "a" * 64,
            "default_semantic": None,
            "enum_rows_name_wire_hidden_feature_alias_of": [],
            "enum_omissions_name_wire": [],
        }
        enum_guc = {
            **primitive_guc,
            "registration_ordinal": 1,
            "name": "bioseq.test_force_paper_index_failure",
            "member": "test_force_paper_index_failure",
            "kind": "enum",
            "storage_kind": "int32_enum",
            "storage": "legacy_test_force_paper_index_failure",
            "default_expression": "fixture_mode::off",
            "feature_predicate": "defined(BIOSEQ_TEST_INDEXES)",
            "profiles": ["test_indexes"],
            "baseline_legacy_description_sha256": "b" * 64,
            "default_semantic": "off",
            "enum_rows_name_wire_hidden_feature_alias_of": [
                ["off", "0", 0, "true", None],
                ["on", "1", 0, "true", None],
            ],
            "enum_omissions_name_wire": [["count", "2"]],
        }
        trailing_guc = {
            **primitive_guc,
            "registration_ordinal": 2,
            "name": "bioseq.trailing_fixture",
            "member": "trailing_fixture",
            "storage": "legacy_trailing_fixture",
            "baseline_legacy_description_sha256": "c" * 64,
        }

        def guc_ledger_fixture(
            records: Sequence[Mapping[str, object]],
            production_count: int,
            test_count: int,
        ) -> dict[str, object]:
            return {
                "schema_version": 1,
                "description_authority": "cleaned_doxygen_only",
                "baseline_legacy_description_hash_is_non_authoritative": True,
                "ordinal_base": 0,
                "source_owner_order": [
                    "column",
                    "custom_scan",
                    "batch_sort_scan",
                ],
                "profiles": {
                    "production": {"row_count": production_count},
                    "test_indexes": {"row_count": test_count},
                },
                "columns": list(_GUC_LEDGER_COLUMNS),
                "rows": [
                    [record[column] for column in _GUC_LEDGER_COLUMNS]
                    for record in records
                ],
            }

        guc_ledger = guc_ledger_fixture(
            [primitive_guc, enum_guc, trailing_guc],
            2,
            3,
        )
        production_gucs = _guc_ledger_records(
            guc_ledger,
            "production",
        )
        test_gucs = _guc_ledger_records(
            guc_ledger,
            "test_indexes",
        )
        compression_guc = {
            **enum_guc,
            "registration_ordinal": 0,
            "name": "bioseq.compression",
            "member": "compression",
            "storage": "legacy_compression",
            "feature_predicate": "true",
            "profiles": ["production", "test_indexes"],
            "default_semantic": "best_compiled_algorithm",
        }
        _guc_ledger_records(
            guc_ledger_fixture([compression_guc], 1, 1),
            "production",
        )
        invalid_semantic_default = {
            **compression_guc,
            "name": "bioseq.not_compression",
            "member": "not_compression",
        }
        expect_failure(
            "an unscoped feature-selected enum default",
            lambda: _guc_ledger_records(
                guc_ledger_fixture(
                    [invalid_semantic_default], 1, 1),
                "production",
            ),
        )

        def manifest_registrations(
            records: Sequence[Mapping[str, object]],
        ) -> list[dict[str, object]]:
            registrations: list[dict[str, object]] = []
            for expected in _expected_guc_registration_rows(records):
                authority = dict(expected["authority"])
                candidate = dict(expected["candidate"])
                candidate["source_sha256"] = "c" * 64
                hooks = authority["hooks"]
                generated_live_authority = {
                    "vartype": authority["kind"],
                    "context": "user",
                    "unit": None,
                    "boot_value": (
                        "off"
                        if authority["kind"] == "bool"
                        else "fixture"
                    ),
                    "reset_value": (
                        "off"
                        if authority["kind"] == "bool"
                        else "fixture"
                    ),
                    "current_value": (
                        "off"
                        if authority["kind"] == "bool"
                        else "fixture"
                    ),
                    "minimum": None,
                    "maximum": None,
                    "visible_enum_values": [],
                    "short_description": "Fixture setting.",
                    "extra_description": None,
                    "raw_flags": 0,
                    "visible_in_show_all": True,
                    "included_in_sample": True,
                    "hooks": {
                        name: {
                            "symbol": hooks[index],
                            "present": hooks[index] is not None,
                            "behavior": (
                                f"custom_{name}"
                                if hooks[index] is not None
                                else "none"
                            ),
                        }
                        for index, name in enumerate(
                            ("check", "assign", "show")
                        )
                    },
                }
                registrations.append({
                    "authority": authority,
                    "generated_live_authority": (
                        generated_live_authority
                    ),
                    "candidate": candidate,
                })
            return registrations

        production_registrations = manifest_registrations(production_gucs)
        test_registrations = manifest_registrations(test_gucs)

        def fixture_source_provenance(
            registrations: Sequence[Mapping[str, object]],
        ) -> dict[str, dict[str, str]]:
            return {
                str(registration["authority"]["name"]): {
                    "source_module": str(
                        registration["candidate"]["source_module"]),
                    "source_path": (
                        "fixture/"
                        + str(registration["authority"]["name"])
                        + ".cppm"
                    ),
                    "source_sha256": str(
                        registration["candidate"]["source_sha256"]),
                }
                for registration in registrations
            }
        production_source_provenance = fixture_source_provenance(
            production_registrations)
        test_source_provenance = fixture_source_provenance(
            test_registrations)
        _validate_guc_registration_rows(
            production_gucs,
            production_registrations,
            production_source_provenance,
        )
        _validate_guc_registration_rows(
            test_gucs,
            test_registrations,
            test_source_provenance,
        )
        fixture_entity_manifest = {
            "entities": [
                {
                    "kind": "guc",
                    "identity": f"sql:guc:{name}",
                    "stable_identity": f"sql:guc:{name}",
                    **provenance,
                }
                for name, provenance in test_source_provenance.items()
            ],
        }
        if (
            _manifest_guc_source_provenance(
                fixture_entity_manifest, None)
            != test_source_provenance
        ):
            raise CatalogParityError(
                "catalog parity self-test failed exact GUC entity "
                "provenance extraction")
        entity_source_path_mutation = json.loads(json.dumps(
            fixture_entity_manifest))
        entity_source_path_mutation["entities"][0]["source_path"] = (
            "./"
            + entity_source_path_mutation["entities"][0]["source_path"]
        )
        expect_failure(
            "a noncanonical GUC entity source path",
            lambda: _manifest_guc_source_provenance(
                entity_source_path_mutation, None),
        )
        entity_identity_mutation = json.loads(json.dumps(
            fixture_entity_manifest))
        entity_identity_mutation["entities"][0]["identity"] = (
            "sql:guc:bioseq.competing_fixture")
        expect_failure(
            "a competing GUC entity/name provenance identity",
            lambda: _manifest_guc_source_provenance(
                entity_identity_mutation, None),
        )
        entity_name_mutation = json.loads(json.dumps(
            fixture_entity_manifest))
        entity_name_mutation["entities"][0]["identity"] = (
            "sql:guc:bioseq.renamed_fixture")
        entity_name_mutation["entities"][0]["stable_identity"] = (
            "sql:guc:bioseq.renamed_fixture")
        expect_failure(
            "a GUC registration bound to a differently named entity",
            lambda: _validate_guc_registration_rows(
                test_gucs,
                test_registrations,
                _manifest_guc_source_provenance(
                    entity_name_mutation, None),
            ),
        )

        backing_cell_mutation = json.loads(json.dumps(
            production_registrations))
        backing_cell_mutation[0]["candidate"]["cpp_storage"] = (
            "bioseq::settings::different_cell")
        expect_failure(
            "a generated GUC backing-cell mutation",
            lambda: _validate_guc_registration_rows(
                production_gucs,
                backing_cell_mutation,
                production_source_provenance,
            ),
        )
        storage_abi_mutation = json.loads(json.dumps(
            production_registrations))
        storage_abi_mutation[0]["candidate"]["storage_kind"] = "int32"
        expect_failure(
            "a generated GUC storage-ABI mutation",
            lambda: _validate_guc_registration_rows(
                production_gucs,
                storage_abi_mutation,
                production_source_provenance,
            ),
        )
        hook_mutation = json.loads(json.dumps(guc_ledger))
        hook_mutation["rows"][0][
            _GUC_LEDGER_COLUMNS.index("hooks")
        ] = ["check_fixture", None, None]
        expect_failure(
            "a schema-v1 GUC hook mutation",
            lambda: _guc_ledger_records(
                hook_mutation,
                "production",
            ),
        )
        omission_mutation = json.loads(json.dumps(test_registrations))
        omission_mutation[1]["authority"][
            "enum_omissions_name_wire"
        ] = []
        expect_failure(
            "a generated enum-omission mutation",
            lambda: _validate_guc_registration_rows(
                test_gucs,
                omission_mutation,
                test_source_provenance,
            ),
        )
        swapped_profiles = json.loads(json.dumps(guc_ledger))
        profile_column = _GUC_LEDGER_COLUMNS.index("profiles")
        swapped_profiles["rows"][0][profile_column] = ["test_indexes"]
        swapped_profiles["rows"][1][profile_column] = [
            "production",
            "test_indexes",
        ]
        expect_failure(
            "equal-count GUC profile identity drift",
            lambda: _guc_ledger_records(
                swapped_profiles,
                "production",
            ),
        )
        expect_failure(
            "an unconsumed generated GUC registration",
            lambda: _validate_guc_registration_rows(
                test_gucs,
                test_registrations[:-1],
                test_source_provenance,
            ),
        )
        extra_registration = json.loads(json.dumps(test_registrations))
        extra_registration.append(extra_registration[-1])
        expect_failure(
            "an extra generated GUC registration",
            lambda: _validate_guc_registration_rows(
                test_gucs,
                extra_registration,
                test_source_provenance,
            ),
        )
        source_hash_mutation = json.loads(json.dumps(test_registrations))
        source_hash_mutation[0]["candidate"]["source_sha256"] = "d" * 64
        expect_failure(
            "a generated GUC source-module hash mutation",
            lambda: _validate_guc_registration_rows(
                test_gucs,
                source_hash_mutation,
                test_source_provenance,
            ),
        )

        def write_snapshot_fixture(
            path: Path,
            *,
            omit_proc_field: str | None = None,
            omit_scope_field: str | None = None,
            duplicate_proc: bool = False,
        ) -> None:
            stream = io.StringIO(newline="")
            writer = csv.writer(stream)
            writer.writerow(["catalog", "identity", "ordinal", "value"])
            proc_value = {
                field: None
                for field in _REQUIRED_SNAPSHOT_FIELDS["pg_proc"]
                if field != omit_proc_field
            }
            proc_row = [
                "pg_proc",
                "function:$extension.fixture()",
                1,
                json.dumps(proc_value, sort_keys=True),
            ]
            writer.writerow(proc_row)
            if duplicate_proc:
                writer.writerow(proc_row)
            for catalog in sorted(_REQUIRED_SNAPSHOT_CATALOGS):
                fields = list(_REQUIRED_SNAPSHOT_FIELDS[catalog])
                if catalog == "pg_proc" and omit_scope_field is not None:
                    fields.remove(omit_scope_field)
                writer.writerow([
                    "snapshot_scope",
                    f"catalog:{catalog}",
                    1,
                    json.dumps(
                        {"queried": True, "fields": fields},
                        sort_keys=True,
                    ),
                ])
            path.write_text(stream.getvalue(), encoding="utf-8")

        valid_snapshot_path = root / "snapshot-valid.csv"
        write_snapshot_fixture(valid_snapshot_path)
        _parse_snapshot(valid_snapshot_path)
        missing_proc_field = root / "snapshot-missing-prosrc.csv"
        write_snapshot_fixture(
            missing_proc_field,
            omit_proc_field="prosrc",
        )
        expect_failure(
            "a snapshot row missing pg_proc.prosrc",
            lambda: _parse_snapshot(missing_proc_field),
        )
        missing_scope_field = root / "snapshot-scope-missing-prosrc.csv"
        write_snapshot_fixture(
            missing_scope_field,
            omit_scope_field="prosrc",
        )
        expect_failure(
            "a snapshot scope marker missing pg_proc.prosrc",
            lambda: _parse_snapshot(missing_scope_field),
        )
        duplicate_snapshot = root / "snapshot-duplicate.csv"
        write_snapshot_fixture(
            duplicate_snapshot,
            duplicate_proc=True,
        )
        expect_failure(
            "a duplicate snapshot identity",
            lambda: _parse_snapshot(duplicate_snapshot),
        )

        manifest_symbols = frozenset({"fixture_a", "fixture_b"})
        finfo_symbols = frozenset({
            "pg_finfo_fixture_a",
            "pg_finfo_fixture_b",
        })
        identity_symbols = {
            "function:$extension.fixture_a()": "fixture_a",
            "function:$extension.fixture_b()": "fixture_b",
        }
        exported_symbols = manifest_symbols | finfo_symbols
        _validate_symbol_edge_closure(
            manifest_c_symbols=manifest_symbols,
            manifest_finfo_symbols=finfo_symbols,
            manifest_owner_symbols=manifest_symbols,
            manifest_identity_to_symbol=identity_symbols,
            catalog_identity_to_symbol=identity_symbols,
            exported_symbols=exported_symbols,
            external_legacy_symbols=frozenset(),
            expected_identity_count=2,
            expected_c_symbol_count=2,
            expected_finfo_symbol_count=2,
        )
        expect_failure(
            "a duplicate raw C-symbol inventory",
            lambda: _exact_string_inventory(
                ["fixture_a", "fixture_a"],
                label="fixture C symbols",
                expected_count=2,
            ),
        )
        expect_failure(
            "a non-string raw C-symbol inventory",
            lambda: _exact_string_inventory(
                ["fixture_a", 7],
                label="fixture C symbols",
                expected_count=2,
            ),
        )
        expect_failure(
            "a consistently undersized symbol closure",
            lambda: _validate_symbol_edge_closure(
                manifest_c_symbols=frozenset({"fixture_a"}),
                manifest_finfo_symbols=frozenset({
                    "pg_finfo_fixture_a",
                }),
                manifest_owner_symbols=frozenset({"fixture_a"}),
                manifest_identity_to_symbol={
                    "function:$extension.fixture_a()": "fixture_a",
                },
                catalog_identity_to_symbol={
                    "function:$extension.fixture_a()": "fixture_a",
                },
                exported_symbols=frozenset({
                    "fixture_a",
                    "pg_finfo_fixture_a",
                }),
                external_legacy_symbols=frozenset(),
                expected_identity_count=2,
                expected_c_symbol_count=2,
                expected_finfo_symbol_count=2,
            ),
        )
        expect_failure(
            "a consistently oversized symbol closure",
            lambda: _validate_symbol_edge_closure(
                manifest_c_symbols=frozenset({
                    "fixture_a",
                    "fixture_b",
                    "fixture_c",
                }),
                manifest_finfo_symbols=frozenset({
                    "pg_finfo_fixture_a",
                    "pg_finfo_fixture_b",
                    "pg_finfo_fixture_c",
                }),
                manifest_owner_symbols=frozenset({
                    "fixture_a",
                    "fixture_b",
                    "fixture_c",
                }),
                manifest_identity_to_symbol={
                    **identity_symbols,
                    "function:$extension.fixture_c()": "fixture_c",
                },
                catalog_identity_to_symbol={
                    **identity_symbols,
                    "function:$extension.fixture_c()": "fixture_c",
                },
                exported_symbols=exported_symbols | {
                    "fixture_c",
                    "pg_finfo_fixture_c",
                },
                external_legacy_symbols=frozenset(),
                expected_identity_count=2,
                expected_c_symbol_count=2,
                expected_finfo_symbol_count=2,
            ),
        )
        expect_failure(
            "an undersized live catalog symbol edge set",
            lambda: _validate_symbol_edge_closure(
                manifest_c_symbols=manifest_symbols,
                manifest_finfo_symbols=finfo_symbols,
                manifest_owner_symbols=manifest_symbols,
                manifest_identity_to_symbol=identity_symbols,
                catalog_identity_to_symbol={
                    next(iter(identity_symbols.items()))[0]:
                    next(iter(identity_symbols.items()))[1],
                },
                exported_symbols=exported_symbols,
                external_legacy_symbols=frozenset(),
                expected_identity_count=2,
                expected_c_symbol_count=2,
                expected_finfo_symbol_count=2,
            ),
        )
        expect_failure(
            "an oversized live catalog symbol edge set",
            lambda: _validate_symbol_edge_closure(
                manifest_c_symbols=manifest_symbols,
                manifest_finfo_symbols=finfo_symbols,
                manifest_owner_symbols=manifest_symbols,
                manifest_identity_to_symbol=identity_symbols,
                catalog_identity_to_symbol={
                    **identity_symbols,
                    "function:$extension.extra()": "fixture_a",
                },
                exported_symbols=exported_symbols,
                external_legacy_symbols=frozenset(),
                expected_identity_count=2,
                expected_c_symbol_count=2,
                expected_finfo_symbol_count=2,
            ),
        )
        expect_failure(
            "a missing exported pg_finfo edge",
            lambda: _validate_symbol_edge_closure(
                manifest_c_symbols=manifest_symbols,
                manifest_finfo_symbols=finfo_symbols,
                manifest_owner_symbols=manifest_symbols,
                manifest_identity_to_symbol=identity_symbols,
                catalog_identity_to_symbol=identity_symbols,
                exported_symbols=(
                    exported_symbols - {"pg_finfo_fixture_b"}
                ),
                external_legacy_symbols=frozenset(),
                expected_identity_count=2,
                expected_c_symbol_count=2,
                expected_finfo_symbol_count=2,
            ),
        )
        expect_failure(
            "an extra exported pg_finfo edge",
            lambda: _validate_symbol_edge_closure(
                manifest_c_symbols=manifest_symbols,
                manifest_finfo_symbols=finfo_symbols,
                manifest_owner_symbols=manifest_symbols,
                manifest_identity_to_symbol=identity_symbols,
                catalog_identity_to_symbol=identity_symbols,
                exported_symbols=(
                    exported_symbols | {"pg_finfo_unreviewed"}
                ),
                external_legacy_symbols=frozenset(),
                expected_identity_count=2,
                expected_c_symbol_count=2,
                expected_finfo_symbol_count=2,
            ),
        )

        relocation_fixture_identity = "sql:type:$extension.fixture"
        relocation_manifest = {
            "public_stable_identities": [relocation_fixture_identity],
            "entities": [
                {"stable_identity": relocation_fixture_identity},
            ],
        }
        relocation_manifest_csv = (
            "stable_identity,resolved,extension_member\r\n"
            f"{relocation_fixture_identity},t,t\r\n"
        ).encode("utf-8")
        relocation_smoke_csv = (
            "extension_schema,schema_absent_from_search_path,"
            "sequence_length,explicit_kmer_size,"
            "aligner_option_column_count,from_array_arity_2,"
            "from_array_arity_3,mmsearch_arity_2_ids,"
            "mmsearch_arity_3_ids,aligner_options_type_resolved\r\n"
            f"{_UNUSUAL_SCHEMA},t,4,17,36,ACGT,ACGT,101,101,t\r\n"
        ).encode("utf-8")
        relocation_explain = _canonical_json([{
            "Plan": {
                "Node Type": "Sort",
                "Plans": [{
                    "Node Type": "Custom Scan",
                    "Custom Plan Provider": "BioseqScan",
                    "Schema": _RELOCATION_PROBE_SCHEMA,
                    "Relation Name": "targets",
                    "Alias": "p",
                }],
            },
        }])
        relocation_key = SnapshotKey(
            "pg_class",
            "table:$extension.fixture",
            1,
        )
        relocation_value = {"relkind": "r"}
        ordinary_relocation_snapshot = Snapshot(
            rows={relocation_key: relocation_value},
            counts={"pg_class": 1},
        )
        _validate_relocation_results(
            manifest=relocation_manifest,
            manifest_result=relocation_manifest_csv,
            smoke_result=relocation_smoke_csv,
            explain_two_result=relocation_explain,
            explain_three_result=relocation_explain,
            server={"system_identifier": "fixture"},
            candidate_snapshot=ordinary_relocation_snapshot,
            relocated_snapshot=ordinary_relocation_snapshot,
        )
        duplicate_smoke_header = relocation_smoke_csv.replace(
            b"extension_schema,schema_absent_from_search_path,",
            b"extension_schema,extension_schema,"
            b"schema_absent_from_search_path,",
        ).replace(
            f"{_UNUSUAL_SCHEMA},t,4".encode("utf-8"),
            f"{_UNUSUAL_SCHEMA},{_UNUSUAL_SCHEMA},t,4".encode("utf-8"),
        )
        expect_failure(
            "a duplicated relocated smoke-result CSV header",
            lambda: _validate_relocation_results(
                manifest=relocation_manifest,
                manifest_result=relocation_manifest_csv,
                smoke_result=duplicate_smoke_header,
                explain_two_result=relocation_explain,
                explain_three_result=relocation_explain,
                server={"system_identifier": "fixture"},
                candidate_snapshot=ordinary_relocation_snapshot,
                relocated_snapshot=ordinary_relocation_snapshot,
            ),
        )
        expect_failure(
            "a relocated manifest result with the wrong header",
            lambda: _validate_relocation_manifest_result(
                relocation_manifest,
                relocation_manifest_csv.replace(
                    b"stable_identity,resolved,extension_member",
                    b"identity,resolved,extension_member",
                ),
            ),
        )
        expect_failure(
            "a relocated manifest result with the wrong identity",
            lambda: _validate_relocation_manifest_result(
                relocation_manifest,
                relocation_manifest_csv.replace(
                    relocation_fixture_identity.encode("utf-8"),
                    b"sql:type:$extension.other",
                ),
            ),
        )
        duplicate_relocation_manifest = {
            "public_stable_identities": [
                relocation_fixture_identity,
                "sql:type:$extension.other",
            ],
        }
        duplicate_relocation_manifest_csv = (
            "stable_identity,resolved,extension_member\r\n"
            f"{relocation_fixture_identity},t,t\r\n"
            f"{relocation_fixture_identity},t,t\r\n"
        ).encode("utf-8")
        expect_failure(
            "duplicate relocated manifest result identities",
            lambda: _validate_relocation_manifest_result(
                duplicate_relocation_manifest,
                duplicate_relocation_manifest_csv,
            ),
        )
        expect_failure(
            "an empty relocated mmsearch result",
            lambda: _validate_relocation_results(
                manifest=relocation_manifest,
                manifest_result=relocation_manifest_csv,
                smoke_result=relocation_smoke_csv.replace(
                    b",ACGT,101,101,t\r\n",
                    b",ACGT,,101,t\r\n",
                ),
                explain_two_result=relocation_explain,
                explain_three_result=relocation_explain,
                server={"system_identifier": "fixture"},
                candidate_snapshot=ordinary_relocation_snapshot,
                relocated_snapshot=ordinary_relocation_snapshot,
            ),
        )
        expect_failure(
            "an empty relocated three-argument mmsearch result",
            lambda: _validate_relocation_results(
                manifest=relocation_manifest,
                manifest_result=relocation_manifest_csv,
                smoke_result=relocation_smoke_csv.replace(
                    b",101,101,t\r\n",
                    b",101,,t\r\n",
                ),
                explain_two_result=relocation_explain,
                explain_three_result=relocation_explain,
                server={"system_identifier": "fixture"},
                candidate_snapshot=ordinary_relocation_snapshot,
                relocated_snapshot=ordinary_relocation_snapshot,
            ),
        )
        expect_failure(
            "an incorrect relocated from_array result",
            lambda: _validate_relocation_results(
                manifest=relocation_manifest,
                manifest_result=relocation_manifest_csv,
                smoke_result=relocation_smoke_csv.replace(
                    b",ACGT,ACGT,101,101,t\r\n",
                    b",ACGT,AGGT,101,101,t\r\n",
                ),
                explain_two_result=relocation_explain,
                explain_three_result=relocation_explain,
                server={"system_identifier": "fixture"},
                candidate_snapshot=ordinary_relocation_snapshot,
                relocated_snapshot=ordinary_relocation_snapshot,
            ),
        )
        expect_failure(
            "an empty relocated two-argument functional EXPLAIN",
            lambda: _validate_relocation_results(
                manifest=relocation_manifest,
                manifest_result=relocation_manifest_csv,
                smoke_result=relocation_smoke_csv,
                explain_two_result=b"",
                explain_three_result=relocation_explain,
                server={"system_identifier": "fixture"},
                candidate_snapshot=ordinary_relocation_snapshot,
                relocated_snapshot=ordinary_relocation_snapshot,
            ),
        )
        expect_failure(
            "an empty relocated three-argument functional EXPLAIN",
            lambda: _validate_relocation_results(
                manifest=relocation_manifest,
                manifest_result=relocation_manifest_csv,
                smoke_result=relocation_smoke_csv,
                explain_two_result=relocation_explain,
                explain_three_result=b"",
                server={"system_identifier": "fixture"},
                candidate_snapshot=ordinary_relocation_snapshot,
                relocated_snapshot=ordinary_relocation_snapshot,
            ),
        )
        sequential_relocation_explain = _canonical_json([{
            "Plan": {
                "Node Type": "Seq Scan",
                "Schema": _RELOCATION_PROBE_SCHEMA,
                "Relation Name": "targets",
                "Alias": "p",
            },
        }])
        expect_failure(
            "a relocated sequential-scan EXPLAIN",
            lambda: _validate_relocation_results(
                manifest=relocation_manifest,
                manifest_result=relocation_manifest_csv,
                smoke_result=relocation_smoke_csv,
                explain_two_result=sequential_relocation_explain,
                explain_three_result=relocation_explain,
                server={"system_identifier": "fixture"},
                candidate_snapshot=ordinary_relocation_snapshot,
                relocated_snapshot=ordinary_relocation_snapshot,
            ),
        )
        wrong_relation_relocation_explain = _canonical_json([{
            "Plan": {
                "Node Type": "Custom Scan",
                "Custom Plan Provider": "BioseqScan",
                "Schema": _RELOCATION_PROBE_SCHEMA,
                "Relation Name": "other_targets",
                "Alias": "p",
            },
        }])
        expect_failure(
            "a BioseqScan EXPLAIN on the wrong relocated relation",
            lambda: _validate_relocation_results(
                manifest=relocation_manifest,
                manifest_result=relocation_manifest_csv,
                smoke_result=relocation_smoke_csv,
                explain_two_result=relocation_explain,
                explain_three_result=wrong_relation_relocation_explain,
                server={"system_identifier": "fixture"},
                candidate_snapshot=ordinary_relocation_snapshot,
                relocated_snapshot=ordinary_relocation_snapshot,
            ),
        )
        duplicate_provider_relocation_explain = _canonical_json([{
            "Plan": {
                "Node Type": "Append",
                "Plans": [
                    {
                        "Node Type": "Custom Scan",
                        "Custom Plan Provider": "BioseqScan",
                        "Schema": _RELOCATION_PROBE_SCHEMA,
                        "Relation Name": "targets",
                    },
                    {
                        "Node Type": "Custom Scan",
                        "Custom Plan Provider": "BioseqScan",
                        "Schema": _RELOCATION_PROBE_SCHEMA,
                        "Relation Name": "targets",
                    },
                ],
            },
        }])
        expect_failure(
            "duplicate BioseqScan nodes in a relocated EXPLAIN",
            lambda: _validate_relocation_results(
                manifest=relocation_manifest,
                manifest_result=relocation_manifest_csv,
                smoke_result=relocation_smoke_csv,
                explain_two_result=duplicate_provider_relocation_explain,
                explain_three_result=relocation_explain,
                server={"system_identifier": "fixture"},
                candidate_snapshot=ordinary_relocation_snapshot,
                relocated_snapshot=ordinary_relocation_snapshot,
            ),
        )
        relocation_mmsearch_two = (
            "sql:function:$extension.mmsearch("
            "bioseq.seq,bioseq.seq)"
        )
        relocation_mmsearch_three = (
            "sql:function:$extension.mmsearch("
            "bioseq.seq,bioseq.seq,$extension.bioseq_aligner_options)"
        )
        relocation_query_public = [
            relocation_mmsearch_two,
            relocation_mmsearch_three,
            "sql:function:bioseq.len(bioseq.seq)",
            "sql:type:$extension.bioseq_aligner_options",
            "sql:type:bioseq.seq",
        ]
        relocation_query_entities = [
            {"stable_identity": identity}
            for identity in [
                *relocation_query_public,
                "sql:function:bioseq.from_array("
                "bioseq.letter[],integer)",
                "sql:function:bioseq.from_array("
                "bioseq.letter[],text,integer)",
                "sql:type:bioseq.letter",
            ]
        ]
        relocation_query_entities.append({
            "stable_identity": (
                "sql:sql_function:$extension.aligner_options(integer)"
            ),
            "kind": "sql_function",
            "schema": "$extension",
            "schema_mode": "extension",
            "name": "aligner_options",
        })
        relocation_query_manifest = {
            "public_stable_identities": relocation_query_public,
            "entities": relocation_query_entities,
        }
        (
            relocation_manifest_query,
            relocation_functional_query,
            relocation_explain_two_query,
            relocation_explain_three_query,
            relocation_cleanup_query,
        ) = _relocation_queries(
            relocation_query_manifest,
            _UNUSUAL_SCHEMA,
        )
        relocated_mmsearch_reference = (
            f"{_sql_identifier(_UNUSUAL_SCHEMA)}."
            f"{_sql_identifier('mmsearch')}"
        ).encode("utf-8")
        relocated_options_reference = (
            f"{_sql_identifier(_UNUSUAL_SCHEMA)}."
            f"{_sql_identifier('aligner_options')}"
        ).encode("utf-8")
        fixed_from_array_reference = (
            f"{_sql_identifier('bioseq')}."
            f"{_sql_identifier('from_array')}"
        ).encode("utf-8")
        qualified_probe_table = (
            f"{_sql_identifier(_RELOCATION_PROBE_SCHEMA)}."
            f"{_sql_identifier('targets')}"
        ).encode("utf-8")
        qualified_probe_drop = (
            f"DROP SCHEMA {_sql_identifier(_RELOCATION_PROBE_SCHEMA)} "
            "CASCADE;"
        ).encode("utf-8")
        relocation_install_query = _relocation_install_query(
            _UNUSUAL_SCHEMA)
        expected_relocated_install = (
            f"CREATE EXTENSION bioseq SCHEMA "
            f"{_sql_identifier(_UNUSUAL_SCHEMA)};"
        ).encode("utf-8")
        explicit_options = b"(kmer_size => 17)"
        if (
            relocated_mmsearch_reference not in relocation_functional_query
            or relocated_mmsearch_reference not in relocation_explain_two_query
            or relocated_mmsearch_reference
            not in relocation_explain_three_query
            or relocated_options_reference not in relocation_functional_query
            or relocated_options_reference
            not in relocation_explain_three_query
            or relocated_options_reference in relocation_explain_two_query
            or explicit_options not in relocation_functional_query
            or explicit_options not in relocation_explain_three_query
            or explicit_options in relocation_explain_two_query
            or fixed_from_array_reference not in relocation_functional_query
            or b"public.mmsearch" in relocation_functional_query
            or b"bioseq.mmsearch" in relocation_functional_query
            or b'"public"."mmsearch"' in relocation_functional_query
            or b'"bioseq"."mmsearch"' in relocation_functional_query
            or b"from_array_arity_2" not in relocation_functional_query
            or b"from_array_arity_3" not in relocation_functional_query
            or b"mmsearch_arity_2_ids" not in relocation_functional_query
            or b"mmsearch_arity_3_ids" not in relocation_functional_query
            or (
                b"CREATE TABLE " + qualified_probe_table
                not in relocation_functional_query
            )
            or b") USING bioseq;" not in relocation_functional_query
            or qualified_probe_drop not in relocation_cleanup_query
            or any(
                b"SET search_path = pg_catalog;" not in query
                for query in (
                    relocation_install_query,
                    relocation_manifest_query,
                    relocation_functional_query,
                    relocation_explain_two_query,
                    relocation_explain_three_query,
                    relocation_cleanup_query,
                )
            )
            or expected_relocated_install not in relocation_install_query
            or not relocation_manifest_query
        ):
            raise CatalogParityError(
                "catalog parity self-test failed relocation query closure")
        relocation_query_replay_root = root / "relocation-query-replay"
        relocation_query_replay_root.mkdir()
        relocation_query_artifacts = _relocation_query_artifacts(
            relocation_query_manifest,
            _UNUSUAL_SCHEMA,
        )
        for query_name, query_bytes in relocation_query_artifacts.items():
            _write_bytes(
                relocation_query_replay_root / query_name,
                query_bytes,
            )
        _validate_relocation_query_replay(
            relocation_query_replay_root,
            relocation_query_manifest,
            _UNUSUAL_SCHEMA,
        )
        for query_name, query_bytes in relocation_query_artifacts.items():
            query_path = relocation_query_replay_root / query_name
            query_path.unlink()
            _write_bytes(query_path, query_bytes + b"-- tampered\n")
            expect_failure(
                f"tampered replay query {query_name}",
                lambda: _validate_relocation_query_replay(
                    relocation_query_replay_root,
                    relocation_query_manifest,
                    _UNUSUAL_SCHEMA,
                ),
            )
            query_path.unlink()
            _write_bytes(query_path, query_bytes)
        expect_failure(
            "a relocation query missing a public mmsearch identity",
            lambda: _relocation_queries(
                {
                    **relocation_query_manifest,
                    "public_stable_identities": [
                        identity
                        for identity in relocation_query_public
                        if identity != relocation_mmsearch_three
                    ],
                },
                _UNUSUAL_SCHEMA,
            ),
        )
        expect_failure(
            "a changed relocated catalog value",
            lambda: _validate_relocation_results(
                manifest=relocation_manifest,
                manifest_result=relocation_manifest_csv,
                smoke_result=relocation_smoke_csv,
                explain_two_result=relocation_explain,
                explain_three_result=relocation_explain,
                server={"system_identifier": "fixture"},
                candidate_snapshot=ordinary_relocation_snapshot,
                relocated_snapshot=Snapshot(
                    rows={relocation_key: {"relkind": "v"}},
                    counts={"pg_class": 1},
                ),
            ),
        )
        expect_failure(
            "an undersized relocated catalog",
            lambda: _validate_relocated_catalog_equality(
                ordinary_relocation_snapshot,
                Snapshot(rows={}, counts={}),
            ),
        )
        expect_failure(
            "an oversized relocated catalog",
            lambda: _validate_relocation_results(
                manifest=relocation_manifest,
                manifest_result=relocation_manifest_csv,
                smoke_result=relocation_smoke_csv,
                explain_two_result=relocation_explain,
                explain_three_result=relocation_explain,
                server={"system_identifier": "fixture"},
                candidate_snapshot=ordinary_relocation_snapshot,
                relocated_snapshot=Snapshot(
                    rows={
                        relocation_key: relocation_value,
                        SnapshotKey(
                            "pg_class",
                            "table:$extension.extra",
                            1,
                        ): relocation_value,
                    },
                    counts={"pg_class": 2},
                ),
            ),
        )

        fresh_destination_root = root / "fresh-destination"
        fresh_destination_root.mkdir()
        fresh_bundle_id = "a" * 64
        fresh_destination = _fresh_bundle_destination(
            fresh_destination_root,
            fresh_bundle_id,
        )
        fresh_destination.mkdir()
        expect_failure(
            "a fresh evidence destination collision",
            lambda: _fresh_bundle_destination(
                fresh_destination_root,
                fresh_bundle_id,
            ),
        )

        expect_failure(
            "legacy SHA drift",
            lambda: _validate_bundle(
                valid,
                expected_legacy_sha256="0" * 64,
            ),
        )

        invalid_nonce = root / "invalid_nonce"
        shutil.copytree(valid, invalid_nonce)

        def mutate_nonce(report: dict[str, object]) -> None:
            provenance = report["input_provenance"]
            assert isinstance(provenance, dict)
            provenance["fresh_run_nonce"] = "0" * 32
            report["bundle_id"] = _sha256_bytes(
                _canonical_json(provenance))

        _rewrite_report_and_seal(invalid_nonce, mutate_nonce)
        expect_failure(
            "a non-UUIDv4 fresh-run nonce",
            lambda: _validate_bundle(
                invalid_nonce,
                expected_legacy_sha256=expected_legacy,
            ),
        )

        missing = root / "missing"
        shutil.copytree(valid, missing)
        (missing / "results" / "legacy.csv").unlink()
        expect_failure(
            "a missing normalized result",
            lambda: _validate_bundle(
                missing,
                expected_legacy_sha256=expected_legacy,
            ),
        )

        tampered = root / "tampered"
        shutil.copytree(valid, tampered)
        with (tampered / "results" / "candidate.csv").open("ab") as stream:
            stream.write(b"tampered\n")
        expect_failure(
            "normalized result hash tampering",
            lambda: _validate_bundle(
                tampered,
                expected_legacy_sha256=expected_legacy,
            ),
        )

        helper_tampered = root / "helper_tampered"
        shutil.copytree(valid, helper_tampered)
        with (
            helper_tampered / "inputs" / "private_postgres.py"
        ).open("ab") as stream:
            stream.write(b"tampered lifecycle helper\n")
        expect_failure(
            "private PostgreSQL helper hash tampering",
            lambda: _validate_bundle(
                helper_tampered,
                expected_legacy_sha256=expected_legacy,
            ),
        )

        count_tampered = root / "count_tampered"
        shutil.copytree(valid, count_tampered)

        def mutate_count(report: dict[str, object]) -> None:
            row_counts = report["row_counts"]
            assert isinstance(row_counts, dict)
            legacy_count = row_counts["legacy"]
            assert isinstance(legacy_count, dict)
            legacy_count["total"] = 2

        _rewrite_report_and_seal(count_tampered, mutate_count)
        expect_failure(
            "normalized result row-count tampering",
            lambda: _validate_bundle(
                count_tampered,
                expected_legacy_sha256=expected_legacy,
            ),
        )

        forged_live = root / "forged_live"
        shutil.copytree(valid, forged_live)

        def claim_live_verdict(report: dict[str, object]) -> None:
            report["evidence_kind"] = "live_catalog_parity"

        _rewrite_report_and_seal(forged_live, claim_live_verdict)
        expect_failure(
            "a coherently re-sealed shallow live verdict",
            lambda: _validate_bundle(
                forged_live,
                expected_legacy_sha256=expected_legacy,
            ),
        )

        unapproved, unapproved_legacy = _self_test_bundle(
            root, unapproved=True)
        expect_failure(
            "an unapproved difference",
            lambda: _validate_bundle(
                unapproved,
                expected_legacy_sha256=unapproved_legacy,
            ),
        )


def _cluster_inputs(
    *,
    label: str,
    stage_root: Path,
    stage_manifest: Mapping[str, object],
    config_template: Path,
    retained_root: Path,
    timeout: int,
) -> ClusterInputs:
    return ClusterInputs(
        bindir=_stage_member(stage_root, stage_manifest, "bindir"),
        database="doc0388_catalog",
        config_template=config_template,
        output_root=retained_root / "runs" / label,
        run_label=f"doc0388_catalog_{label}",
        library_dirs=(
            _stage_member(stage_root, stage_manifest, "pkglibdir"),
        ),
        init_command_timeout=timeout,
        bootstrap_timeout=timeout,
        startup_timeout=timeout,
        shutdown_timeout=timeout,
    )


def _stop_clusters(clusters: Iterable[UniquePrivateCluster]) -> None:
    failures: list[str] = []
    for cluster in reversed(list(clusters)):
        try:
            cluster.stop()
        except Exception as error:  # preserve cleanup of every owned node
            failures.append(str(error))
    if failures:
        raise CatalogParityError(
            "private PostgreSQL teardown failed: " + "; ".join(failures))


def _input_seal(
    *,
    paths: Mapping[str, Path],
    legacy_bytes: bytes,
    active_profile: str,
    postgresql_provenance: Mapping[str, object],
    fresh_run_nonce: str,
) -> tuple[str, dict[str, object]]:
    inputs: dict[str, object] = {
        name: {
            "path": str(path),
            "sha256": _sha256(path),
            "size": path.stat().st_size,
        }
        for name, path in sorted(paths.items())
    }
    inputs["legacy_sql"] = {
        "sha256": _sha256_bytes(legacy_bytes),
        "size": len(legacy_bytes),
    }
    inputs["active_profile"] = active_profile
    inputs["postgresql"] = dict(postgresql_provenance)
    inputs["fresh_run_nonce"] = fresh_run_nonce
    digest = _sha256_bytes(_canonical_json(inputs))
    return digest, inputs


def _postgresql_input_provenance(
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
        output = _run(
            [str(pg_config), flag],
            timeout=timeout,
        ).decode("utf-8", errors="strict").strip()
        if not output:
            raise CatalogParityError(f"pg_config {flag} returned no value")
        values[flag.removeprefix("--")] = output
    if not values["version"].startswith("PostgreSQL 17"):
        raise CatalogParityError(
            f"pg_config did not select PostgreSQL 17: {values['version']}")
    bindir = _real_directory(
        values["bindir"], "PostgreSQL bindir", create=False)
    tools: dict[str, object] = {}
    for name in (
        "postgres",
        "initdb",
        "pg_ctl",
        "createdb",
        "dropdb",
        "psql",
    ):
        candidate = (bindir / name).resolve(strict=True)
        if not candidate.is_file() or not os.access(candidate, os.X_OK):
            raise CatalogParityError(
                f"PostgreSQL tool is not executable: {candidate}")
        tools[name] = {
            "path": str(candidate),
            "sha256": _sha256(candidate),
            "size": candidate.stat().st_size,
        }
    return {
        "pg_config": values,
        "tools": tools,
    }


def _category_counts(rows: Iterable[Mapping[str, object]]) -> dict[str, int]:
    counts: collections.Counter[str] = collections.Counter()
    for row in rows:
        category = row.get("approved_category")
        counts[str(category)] += 1
    return dict(sorted(counts.items()))


def _run_live(arguments: argparse.Namespace) -> Path:
    run_self_tests()
    timeout = int(arguments.timeout_seconds)
    if timeout < 1 or timeout > 600:
        raise CatalogParityError("timeout-seconds must be in the range 1..600")
    repository_root = _real_directory(
        arguments.repository_root, "repository root", create=False)
    output_root = _real_directory(
        arguments.output_root, "catalog evidence root", create=True)
    paths = {
        "python": _executable(sys.executable, "Python interpreter"),
        "cmake": _executable(arguments.cmake, "CMake"),
        "git": _executable(arguments.git, "Git"),
        "pg_config": _executable(arguments.pg_config, "pg_config"),
        "stage_script": _regular_file(
            arguments.stage_script, "PostgreSQL stage script"),
        "config_template": _regular_file(
            arguments.config_template, "private PostgreSQL config"),
        "control_file": _regular_file(
            arguments.control_file, "extension control file"),
        "candidate_sql": _regular_file(
            arguments.candidate_sql, "generated candidate SQL"),
        "candidate_manifest": _regular_file(
            arguments.candidate_manifest, "generated candidate manifest"),
        "candidate_dso": _regular_file(
            arguments.candidate_dso, "candidate DSO"),
        "snapshot_query": _regular_file(
            arguments.snapshot_query, "catalog snapshot query"),
        "approved_delta": _regular_file(
            arguments.approved_delta, "approved catalog delta"),
        "guc_parity_ledger": _regular_file(
            arguments.guc_parity_ledger, "independent GUC parity ledger"),
        "orchestrator": _regular_file(__file__, "catalog parity orchestrator"),
        "private_postgres_helper": _regular_file(
            Path(__file__).resolve().parent / "private_postgres.py",
            "private PostgreSQL lifecycle helper",
        ),
    }
    delta = _load_json(paths["approved_delta"], "approved catalog delta")
    legacy_contract = delta.get("legacy_sql")
    if not isinstance(legacy_contract, dict):
        raise CatalogParityError("approved delta has no legacy SQL contract")
    revision = legacy_contract.get("git_revision")
    repository_path = legacy_contract.get("repository_path")
    if not isinstance(revision, str) or not isinstance(repository_path, str):
        raise CatalogParityError("legacy Git selector is invalid")
    legacy_bytes = _git_blob(
        paths["git"],
        repository_root,
        revision,
        repository_path,
        timeout=timeout,
    )
    contract = _validate_delta(delta, legacy_bytes)
    manifest = _load_json(paths["candidate_manifest"], "candidate manifest")
    _validate_manifest(manifest, paths["candidate_sql"], contract)
    guc_ledger = _load_json(
        paths["guc_parity_ledger"], "independent GUC parity ledger")
    _validate_guc_authority(
        manifest=manifest,
        ledger=guc_ledger,
        ledger_path=paths["guc_parity_ledger"],
        active_profile=arguments.active_profile,
        snapshot=None,
        repository_root=repository_root,
    )
    postgresql_provenance = _postgresql_input_provenance(
        paths["pg_config"], timeout=timeout)
    bundle_id, input_provenance = _input_seal(
        paths=paths,
        legacy_bytes=legacy_bytes,
        active_profile=arguments.active_profile,
        postgresql_provenance=postgresql_provenance,
        fresh_run_nonce=uuid.uuid4().hex,
    )
    final_bundle = _fresh_bundle_destination(output_root, bundle_id)

    partial = output_root / (
        f".partial-{bundle_id}-{os.getpid()}-{uuid.uuid4().hex[:8]}")
    partial.mkdir()
    failed_destination: Path | None = None
    try:
        _write_bytes(partial / "inputs" / "legacy.sql", legacy_bytes)
        _copy_file(
            paths["candidate_sql"], partial / "inputs" / "candidate.sql")
        _copy_file(
            paths["candidate_manifest"],
            partial / "inputs" / "candidate-manifest.json",
        )
        _copy_file(
            paths["candidate_dso"],
            partial / "inputs" / "candidate-dso",
        )
        _copy_file(
            paths["snapshot_query"],
            partial / "queries" / "catalog-snapshot.sql",
        )
        _copy_file(
            paths["approved_delta"],
            partial / "inputs" / "approved-delta.json",
        )
        _copy_file(
            paths["guc_parity_ledger"],
            partial / "inputs" / "guc-parity-ledger.json",
        )
        _copy_file(
            paths["orchestrator"],
            partial / "inputs" / "run_private_catalog_parity.py",
        )
        _copy_file(
            paths["private_postgres_helper"],
            partial / "inputs" / "private_postgres.py",
        )

        stage_workspace = _real_directory(
            output_root / ".stage-work", "catalog stage workspace", create=True)
        with tempfile.TemporaryDirectory(
            prefix=f"{bundle_id[:12]}-", dir=stage_workspace
        ) as raw_stage_workspace:
            workspace = Path(raw_stage_workspace)
            stage_parent = workspace / "stages"
            stage_parent.mkdir()
            candidate_input = workspace / "candidate-input"
            legacy_input = workspace / "legacy-input"
            candidate_input.mkdir()
            legacy_input.mkdir()
            candidate_stage_sql = candidate_input / "bioseq--1.0.sql"
            legacy_stage_sql = legacy_input / "bioseq--1.0.sql"
            candidate_stage_sql.write_bytes(paths["candidate_sql"].read_bytes())
            legacy_stage_sql.write_bytes(legacy_bytes)
            candidate_stage, candidate_stage_manifest = _stage(
                label="candidate",
                cmake=paths["cmake"],
                pg_config=paths["pg_config"],
                stage_script=paths["stage_script"],
                control_file=paths["control_file"],
                sql_file=candidate_stage_sql,
                dso=paths["candidate_dso"],
                stage_parent=stage_parent,
                retained_root=partial,
                timeout=timeout,
            )
            legacy_stage, legacy_stage_manifest = _stage(
                label="legacy",
                cmake=paths["cmake"],
                pg_config=paths["pg_config"],
                stage_script=paths["stage_script"],
                control_file=paths["control_file"],
                sql_file=legacy_stage_sql,
                dso=paths["candidate_dso"],
                stage_parent=stage_parent,
                retained_root=partial,
                timeout=timeout,
            )
            if (
                candidate_stage_manifest.get("postgresql_version")
                != legacy_stage_manifest.get("postgresql_version")
            ):
                raise CatalogParityError(
                    "legacy and candidate stages selected different servers")

            legacy_cluster = UniquePrivateCluster(_cluster_inputs(
                label="legacy",
                stage_root=legacy_stage,
                stage_manifest=legacy_stage_manifest,
                config_template=paths["config_template"],
                retained_root=partial,
                timeout=timeout,
            ))
            candidate_cluster = UniquePrivateCluster(_cluster_inputs(
                label="candidate",
                stage_root=candidate_stage,
                stage_manifest=candidate_stage_manifest,
                config_template=paths["config_template"],
                retained_root=partial,
                timeout=timeout,
            ))
            started: list[UniquePrivateCluster] = []
            try:
                legacy_cluster.start()
                started.append(legacy_cluster)
                candidate_cluster.start()
                started.append(candidate_cluster)
                _install_extension(
                    label="legacy",
                    cluster=legacy_cluster,
                    bindir=_stage_member(
                        legacy_stage, legacy_stage_manifest, "bindir"),
                    legacy_catalog_only=True,
                    retained_root=partial,
                    timeout=timeout,
                )
                _install_extension(
                    label="candidate",
                    cluster=candidate_cluster,
                    bindir=_stage_member(
                        candidate_stage,
                        candidate_stage_manifest,
                        "bindir",
                    ),
                    legacy_catalog_only=False,
                    retained_root=partial,
                    timeout=timeout,
                )
                legacy_result, legacy_server = _snapshot_cluster(
                    label="legacy",
                    cluster=legacy_cluster,
                    bindir=_stage_member(
                        legacy_stage, legacy_stage_manifest, "bindir"),
                    query=partial / "queries" / "catalog-snapshot.sql",
                    retained_root=partial,
                    timeout=timeout,
                )
                candidate_result, candidate_server = _snapshot_cluster(
                    label="candidate",
                    cluster=candidate_cluster,
                    bindir=_stage_member(
                        candidate_stage,
                        candidate_stage_manifest,
                        "bindir",
                    ),
                    query=partial / "queries" / "catalog-snapshot.sql",
                    retained_root=partial,
                    timeout=timeout,
                )
                if (
                    legacy_server["system_identifier"]
                    == candidate_server["system_identifier"]
                    or legacy_cluster.port == candidate_cluster.port
                    or legacy_cluster.data_dir.resolve(strict=True)
                    == candidate_cluster.data_dir.resolve(strict=True)
                ):
                    raise CatalogParityError(
                        "legacy and candidate did not use distinct clean "
                        "PostgreSQL nodes")
            finally:
                _stop_clusters(started)

            legacy_snapshot = _parse_snapshot(legacy_result)
            candidate_snapshot = _parse_snapshot(candidate_result)
            approved, unapproved, prosrc_summary = _compare_snapshots(
                legacy_snapshot,
                candidate_snapshot,
                delta,
                contract,
            )
            full_diff = sorted(
                [*approved, *unapproved],
                key=lambda row: (
                    str(row["catalog"]),
                    str(row["identity"]),
                    int(row["ordinal"]),
                    str(row["property"]),
                ),
            )
            _write_jsonl(partial / "diffs" / "full.jsonl", full_diff)
            _write_jsonl(
                partial / "diffs" / "approved.jsonl",
                approved,
            )
            _write_jsonl(
                partial / "diffs" / "unapproved.jsonl",
                unapproved,
            )
            _write_jsonl(
                partial
                / "diffs"
                / "untrusted-catalog-changes-candidate.jsonl",
                _untrusted_catalog_change_candidates(unapproved),
            )
            if unapproved:
                raise CatalogParityError(
                    f"catalog parity has {len(unapproved)} unapproved "
                    "differences; manually review the non-authoritative "
                    "candidate at "
                    f"{partial / 'diffs' / 'untrusted-catalog-changes-candidate.jsonl'}")

            relocation_cluster = UniquePrivateCluster(_cluster_inputs(
                label="relocated",
                stage_root=candidate_stage,
                stage_manifest=candidate_stage_manifest,
                config_template=paths["config_template"],
                retained_root=partial,
                timeout=timeout,
            ))
            relocation_cluster.start()
            try:
                relocation = _run_relocation_probe(
                    cluster=relocation_cluster,
                    bindir=_stage_member(
                        candidate_stage,
                        candidate_stage_manifest,
                        "bindir",
                    ),
                    manifest=manifest,
                    snapshot_query=partial / "queries" / "catalog-snapshot.sql",
                    candidate_snapshot=candidate_snapshot,
                    retained_root=partial,
                    timeout=timeout,
                )
                relocated_server = relocation.get("server")
                if (
                    not isinstance(relocated_server, dict)
                    or relocated_server.get("system_identifier")
                    in {
                        legacy_server["system_identifier"],
                        candidate_server["system_identifier"],
                    }
                ):
                    raise CatalogParityError(
                        "relocation did not use a third clean PostgreSQL node")
            finally:
                _stop_clusters([relocation_cluster])

        candidate_invariants = _validate_candidate_catalog(
            candidate_snapshot, manifest, contract)
        guc_authority = _validate_guc_authority(
            manifest=manifest,
            ledger=guc_ledger,
            ledger_path=paths["guc_parity_ledger"],
            active_profile=arguments.active_profile,
            snapshot=candidate_snapshot,
            repository_root=repository_root,
        )
        symbol_closure = _validate_symbol_closure(
            manifest=manifest,
            dso=paths["candidate_dso"],
            candidate_snapshot=candidate_snapshot,
            prosrc_summary=prosrc_summary,
            timeout=timeout,
        )
        _write_json(
            partial / "results" / "legacy-counts.json",
            {
                "total": len(legacy_snapshot.rows),
                "by_catalog": legacy_snapshot.counts,
            },
        )
        _write_json(
            partial / "results" / "candidate-counts.json",
            {
                "total": len(candidate_snapshot.rows),
                "by_catalog": candidate_snapshot.counts,
            },
        )
        _write_json(
            partial / "results" / "prosrc-reconciliation.json",
            prosrc_summary,
        )
        _write_json(
            partial / "results" / "symbol-closure.json",
            symbol_closure,
        )
        _write_json(
            partial / "results" / "candidate-invariants.json",
            candidate_invariants,
        )
        _write_json(
            partial / "results" / "guc-authority.json",
            guc_authority,
        )
        _write_json(
            partial / "results" / "relocation.json",
            relocation,
        )
        head_revision = _run(
            [
                str(paths["git"]),
                "-C",
                str(repository_root),
                "rev-parse",
                "HEAD",
            ],
            timeout=timeout,
        ).decode("ascii", errors="strict").strip()
        retained_query_hashes = {
            path.relative_to(partial).as_posix(): _sha256(path)
            for path in sorted((partial / "queries").glob("*.sql"))
        }
        retained_result_hashes = {
            path.relative_to(partial).as_posix(): _sha256(path)
            for path in sorted((partial / "results").glob("*"))
            if path.is_file()
        }
        relocation_server = relocation["server"]
        assert isinstance(relocation_server, dict)
        system_identifiers = {
            legacy_server["system_identifier"],
            candidate_server["system_identifier"],
            relocation_server["system_identifier"],
        }
        evidence: dict[str, object] = {
            "schema": _BUNDLE_SCHEMA,
            "evidence_kind": "live_catalog_parity",
            "bundle_id": bundle_id,
            "input_provenance": input_provenance,
            "git_head": head_revision,
            "active_build_profile": arguments.active_profile,
            "legacy_catalog_install": {
                "catalog_only": True,
                "check_function_bodies": "off",
                "reason": (
                    "The immutable legacy SQL names retired C symbols. "
                    "The exact 124 identity-level changes and missing alias "
                    "closure are checked against the candidate manifest and "
                    "the current DSO; no compatibility exports are added."
                ),
                "current_dso_sha256": _sha256(paths["candidate_dso"]),
                "legacy_sql_sha256": _sha256_bytes(legacy_bytes),
            },
            "candidate_install": {
                "check_function_bodies": "on",
                "candidate_sql_sha256": _sha256(paths["candidate_sql"]),
                "candidate_manifest_sha256": _sha256(
                    paths["candidate_manifest"]),
                "candidate_dso_sha256": _sha256(paths["candidate_dso"]),
            },
            "postgresql": {
                "stage_version": candidate_stage_manifest.get(
                    "postgresql_version"),
                "legacy_server": legacy_server,
                "candidate_server": candidate_server,
                "relocated_server": relocation_server,
                "distinct_system_identifiers": len(system_identifiers) == 3,
            },
            "queries": retained_query_hashes,
            "row_counts": {
                "legacy": {
                    "total": len(legacy_snapshot.rows),
                    "by_catalog": legacy_snapshot.counts,
                },
                "candidate": {
                    "total": len(candidate_snapshot.rows),
                    "by_catalog": candidate_snapshot.counts,
                },
            },
            "result_hashes": retained_result_hashes,
            "diff": {
                "total_difference_count": len(full_diff),
                "approved_difference_count": len(approved),
                "unapproved_difference_count": len(unapproved),
                "approved_categories": _category_counts(approved),
            },
            "approved_delta_application": {
                "delta_sha256": _sha256(paths["approved_delta"]),
                "prosrc_reconciliation": prosrc_summary,
                "candidate_invariants": candidate_invariants,
                "guc_authority": guc_authority,
                "symbol_closure": symbol_closure,
                "relocation": relocation,
            },
            "verdict": {
                "unapproved_difference_count": len(unapproved),
                "zero_unapproved_differences": not unapproved,
                "passed": not unapproved,
            },
        }
        _seal_bundle(partial, evidence)
        _validate_bundle(partial, require_live=True, timeout=timeout)
        partial.replace(final_bundle)
        _validate_bundle(final_bundle, require_live=True, timeout=timeout)
        return final_bundle
    except BaseException:
        if partial.exists():
            failed_destination = output_root / (
                f"failed-{bundle_id}-{os.getpid()}-{uuid.uuid4().hex[:8]}")
            partial.replace(failed_destination)
            print(
                f"retained failed catalog evidence at {failed_destination}",
                file=sys.stderr,
            )
        raise


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("self-test")
    run = subparsers.add_parser("run")
    run.add_argument("--repository-root", required=True)
    run.add_argument("--output-root", required=True)
    run.add_argument("--cmake", required=True)
    run.add_argument("--git", required=True)
    run.add_argument("--pg-config", required=True)
    run.add_argument("--stage-script", required=True)
    run.add_argument("--config-template", required=True)
    run.add_argument("--control-file", required=True)
    run.add_argument("--candidate-sql", required=True)
    run.add_argument("--candidate-manifest", required=True)
    run.add_argument("--candidate-dso", required=True)
    run.add_argument("--snapshot-query", required=True)
    run.add_argument("--approved-delta", required=True)
    run.add_argument("--guc-parity-ledger", required=True)
    run.add_argument(
        "--active-profile",
        choices=("production", "test_indexes"),
        required=True,
    )
    run.add_argument("--timeout-seconds", type=int, default=120)
    return parser


def main() -> None:
    arguments = _parser().parse_args()
    try:
        if arguments.command == "self-test":
            run_self_tests()
            print("DOC0388 catalog parity self-tests passed")
            return
        result = _run_live(arguments)
        print(f"DOC0388 catalog parity evidence: {result}")
    except CatalogParityError as error:
        print(f"run_private_catalog_parity: {error}", file=sys.stderr)
        raise SystemExit(1) from error


if __name__ == "__main__":
    main()
