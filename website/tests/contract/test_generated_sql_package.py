#!/usr/bin/env python3
"""Verify the generated PostgreSQL install artifact and website SQL manifest."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import re
import stat
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

PRODUCTION_ARTIFACT_ORDER = ("bioseq_cache", "bioseq")
PRODUCTION_EXTENSION_SCHEMAS = {
    "bioseq_cache": "bioseq_cache",
    "bioseq": "public",
}


@dataclass(frozen=True)
class ArtifactInput:
    """One production extension's complete website qualification input."""

    name: str
    control: Path
    install_sql: Path
    manifest: Path
    dso: Path
    public_identities: Path


@dataclass(frozen=True)
class ArtifactReceipt:
    """Validated digests and identities used by the aggregate gate."""

    name: str
    control_sha256: str
    install_sql_sha256: str
    manifest_sha256: str
    dso_sha256: str
    stable_identities: tuple[str, ...]
    resolved_dso: Path


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"{path} must contain one JSON object")
    return value


def _resolve_regular_file(path: Path, label: str) -> Path:
    """Require an absolute, non-symlink regular file and return its real path."""

    if not path.is_absolute():
        raise RuntimeError(f"{label} must be an absolute path: {path}")
    if path.is_symlink():
        raise RuntimeError(f"{label} must not be a symbolic link: {path}")
    try:
        file_status = path.stat()
    except OSError as error:
        raise RuntimeError(f"cannot stat {label} {path}: {error}") from error
    if not stat.S_ISREG(file_status.st_mode):
        raise RuntimeError(f"{label} must be a regular file: {path}")
    return path.resolve(strict=True)


def _sha256_file(path: Path) -> str:
    """Hash a file without materializing a potentially large DSO in memory."""

    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise RuntimeError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def _decode_utf8(data: bytes, source: Path, label: str) -> str:
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise RuntimeError(f"{source}: {label} is not valid UTF-8") from error


def _single_quoted_control_value(
    assignments: dict[str, str], key: str, source: Path
) -> str:
    value = assignments.get(key)
    if value is None:
        raise RuntimeError(f"{source}: control file is missing {key}")
    match = re.fullmatch(r"'((?:''|[^'])*)'", value)
    if match is None:
        raise RuntimeError(f"{source}: control {key} must be single quoted")
    return match.group(1).replace("''", "'")


def _verify_control(
    artifact: ArtifactInput,
    control: Path,
    version: str,
    extension_schema: str,
) -> str:
    control_bytes = control.read_bytes()
    control_text = _decode_utf8(control_bytes, control, "control file")
    if not control_text.endswith("\n"):
        raise RuntimeError(f"{control}: control file must end with a newline")

    assignments: dict[str, str] = {}
    for line_number, raw_line in enumerate(control_text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"([a-z][a-z0-9_]*)\s*=\s*(.+)", line)
        if match is None:
            raise RuntimeError(
                f"{control}:{line_number}: invalid control assignment"
            )
        key, value = match.groups()
        if key in assignments:
            raise RuntimeError(f"{control}: duplicate control key {key}")
        assignments[key] = value

    default_version = _single_quoted_control_value(
        assignments, "default_version", control
    )
    if default_version != version:
        raise RuntimeError(
            f"{control}: default_version {default_version!r} does not match "
            f"install version {version!r}"
        )
    module_pathname = _single_quoted_control_value(
        assignments, "module_pathname", control
    )
    expected_module_pathname = f"$libdir/{artifact.name}"
    if module_pathname != expected_module_pathname:
        raise RuntimeError(
            f"{control}: module_pathname must be "
            f"{expected_module_pathname!r}"
        )

    relocatable = assignments.get("relocatable", "").lower()
    if artifact.name == "bioseq":
        if relocatable != "true" or "schema" in assignments:
            raise RuntimeError(
                f"{control}: bioseq must remain relocatable without a fixed schema"
            )
    else:
        if relocatable != "false":
            raise RuntimeError(
                f"{control}: {artifact.name} must remain fixed-schema"
            )
        control_schema = _single_quoted_control_value(
            assignments, "schema", control
        )
        if control_schema != extension_schema:
            raise RuntimeError(
                f"{control}: schema {control_schema!r} does not match "
                f"manifest schema {extension_schema!r}"
            )
    return hashlib.sha256(control_bytes).hexdigest()


def _require_current_manifest(
    manifest: dict[str, Any], source: Path
) -> None:
    unknown = sorted(set(manifest) - GENERATED_SQL_MANIFEST_FIELDS)
    if unknown:
        raise RuntimeError(
            f"{source}: generated SQL manifest has unknown fields {unknown}"
        )
    if (
        type(manifest.get("schema_revision")) is not int
        or manifest["schema_revision"] != 1
    ):
        raise RuntimeError(
            f"{source}: generated SQL manifest schema_revision must be 1"
        )


def _resolved_manifest_extension_name(manifest: dict[str, Any]) -> Any:
    if "extension_name" not in manifest:
        return "bioseq"
    return manifest["extension_name"]


def _string_list(value: Any, field: str, source: Path) -> list[str]:
    if (
        not isinstance(value, list)
        or any(not isinstance(item, str) or not item for item in value)
    ):
        raise RuntimeError(
            f"{source}: {field} must be an array of nonempty strings"
        )
    if len(value) != len(set(value)):
        raise RuntimeError(f"{source}: {field} contains duplicate identities")
    return value


def _public_projection(
    manifest: dict[str, Any], source: Path
) -> tuple[list[str], list[str]]:
    entities = manifest.get("entities")
    if not isinstance(entities, list):
        raise RuntimeError(f"{source}: entities must be an array")

    stable: list[str] = []
    installed: list[str] = []
    extension_schema = manifest.get("extension_schema")
    if not isinstance(extension_schema, str) or not extension_schema:
        raise RuntimeError(f"{source}: extension_schema must be a nonempty string")
    for index, raw_entity in enumerate(entities):
        if not isinstance(raw_entity, dict):
            raise RuntimeError(f"{source}: entities[{index}] must be an object")
        public = raw_entity.get("public_documentation")
        if not isinstance(public, bool):
            raise RuntimeError(
                f"{source}: entities[{index}].public_documentation must be boolean"
            )
        if not public:
            continue
        stable_identity = raw_entity.get(
            "stable_identity", raw_entity.get("identity")
        )
        installed_identity = raw_entity.get("installed_identity")
        if (
            not isinstance(stable_identity, str)
            or not stable_identity
            or not isinstance(installed_identity, str)
            or not installed_identity
        ):
            raise RuntimeError(
                f"{source}: entities[{index}] has invalid public identities"
            )
        resolved_identity = stable_identity.replace(
            "$extension", extension_schema
        )
        if resolved_identity != installed_identity:
            raise RuntimeError(
                f"{source}: entities[{index}] installed identity is not the "
                "declared extension-schema resolution"
            )
        stable.append(stable_identity)
        installed.append(installed_identity)

    if len(stable) != len(set(stable)) or len(installed) != len(set(installed)):
        raise RuntimeError(f"{source}: public identities are not unique")
    return sorted(stable), sorted(installed)


def _all_identity_projection(
    manifest: dict[str, Any], source: Path
) -> tuple[list[str], list[str]]:
    """Derive every stable/install identity, including non-public entities."""

    entities = manifest.get("entities")
    if not isinstance(entities, list):
        raise RuntimeError(f"{source}: entities must be an array")
    entity_count = manifest.get("entity_count")
    if type(entity_count) is not int or entity_count != len(entities):
        raise RuntimeError(f"{source}: entity_count does not match entities")

    extension_schema = manifest.get("extension_schema")
    if not isinstance(extension_schema, str) or not extension_schema:
        raise RuntimeError(f"{source}: extension_schema must be a nonempty string")

    stable: list[str] = []
    installed: list[str] = []
    for index, raw_entity in enumerate(entities):
        if not isinstance(raw_entity, dict):
            raise RuntimeError(f"{source}: entities[{index}] must be an object")
        stable_identity = raw_entity.get(
            "stable_identity", raw_entity.get("identity")
        )
        installed_identity = raw_entity.get("installed_identity")
        if (
            not isinstance(stable_identity, str)
            or not stable_identity
            or not isinstance(installed_identity, str)
            or not installed_identity
        ):
            raise RuntimeError(
                f"{source}: entities[{index}] has invalid identities"
            )
        resolved_identity = stable_identity.replace(
            "$extension", extension_schema
        )
        if resolved_identity != installed_identity:
            raise RuntimeError(
                f"{source}: entities[{index}] installed identity is not the "
                "declared extension-schema resolution"
            )
        stable.append(stable_identity)
        installed.append(installed_identity)

    if len(stable) != len(set(stable)):
        raise RuntimeError(f"{source}: stable identities are not unique")
    if len(installed) != len(set(installed)):
        raise RuntimeError(f"{source}: installed identities are not unique")
    return sorted(stable), sorted(installed)


def _verify_acl_companion_visibility(
    manifest: dict[str, Any], source: Path
) -> None:
    entities = manifest["entities"]
    acl_identities = {
        entity["stable_identity"]
        for entity in entities
        if entity["kind"] in {"grant", "revoke"}
    }
    public_acl_identities = {
        entity["stable_identity"]
        for entity in entities
        if entity["kind"] in {"grant", "revoke"}
        and entity["public_documentation"]
    }
    acl_summary_identities: set[str] = set()
    for index, entity in enumerate(entities):
        acl_summary = entity.get("acl_summary")
        if not isinstance(acl_summary, list) or any(
            not isinstance(identity, str) or not identity
            for identity in acl_summary
        ):
            raise RuntimeError(
                f"{source}: entities[{index}].acl_summary is malformed"
            )
        acl_summary_identities.update(acl_summary)
    if public_acl_identities:
        raise RuntimeError(
            f"{source}: grant/revoke companions must not be website-public"
        )
    if acl_summary_identities != acl_identities:
        raise RuntimeError(
            f"{source}: ACL companion entities and acl_summary ownership differ"
        )


def _verify_c_symbol_naming(manifest: dict[str, Any], source: Path) -> None:
    owners = manifest.get("c_symbol_owners")
    if not isinstance(owners, list):
        raise RuntimeError(f"{source}: c_symbol_owners must be an array")

    standardized_name = re.compile(r"[a-z_][a-z0-9_]*")
    for index, raw_owner in enumerate(owners):
        if not isinstance(raw_owner, dict):
            raise RuntimeError(
                f"{source}: c_symbol_owners[{index}] must be an object"
            )
        symbol = raw_owner.get("symbol")
        finfo_symbol = raw_owner.get("finfo_symbol")
        identities = _string_list(
            raw_owner.get("routine_identities"),
            f"c_symbol_owners[{index}].routine_identities",
            source,
        )
        if (
            not isinstance(symbol, str)
            or standardized_name.fullmatch(symbol) is None
        ):
            raise RuntimeError(
                f"{source}: C routine symbol {symbol!r} is not a "
                "lowercase snake-case identifier"
            )
        if finfo_symbol != f"pg_finfo_{symbol}":
            raise RuntimeError(
                f"{source}: C routine symbol {symbol!r} has a "
                "nonstandard fmgr-info symbol"
            )
        for identity in identities:
            qualified = identity.partition("(")[0]
            leaf = qualified.rsplit(".", 1)[-1]
            if leaf != symbol:
                raise RuntimeError(
                    f"{source}: SQL routine {identity!r} is bound to "
                    f"nonstandard C symbol {symbol!r}"
                )


def _verify_sql_text(name: str, source: Path, sql_bytes: bytes) -> str:
    sql_text = _decode_utf8(sql_bytes, source, "generated install SQL")
    if not sql_text.endswith("\n"):
        raise RuntimeError(
            f"{source}: generated install SQL must end with a newline"
        )
    if re.search(r"\bLANGUAGE\s+plpgsql\b", sql_text, re.IGNORECASE):
        raise RuntimeError(
            f"{source}: generated install SQL contains LANGUAGE plpgsql"
        )
    if re.search(r"\bDO\s+\$[A-Za-z_0-9]*\$", sql_text, re.IGNORECASE):
        raise RuntimeError(
            f"{source}: generated install SQL contains an anonymous DO block"
        )
    if re.search(
        r"^\s*(?:DROP|RESET)\b",
        sql_text,
        re.IGNORECASE | re.MULTILINE,
    ):
        raise RuntimeError(
            f"{source}: generated install SQL contains historical DROP/RESET cleanup"
        )
    if re.search(r"\bpublic\.", sql_text, re.IGNORECASE):
        raise RuntimeError(
            f"{source}: generated install SQL hardcodes the public schema"
        )
    if re.search(r"\bSTART\s+WITH\b", sql_text, re.IGNORECASE):
        raise RuntimeError(
            f"{source}: generated install SQL changed canonical sequence START syntax"
        )

    if name == "bioseq":
        if "SET bioseq.diagnostics.logging.enabled = off;" not in sql_text:
            raise RuntimeError(
                f"{source}: generated install SQL changed the canonical logging guard"
            )
        for required in (
            "CREATE FUNCTION vemptycount(",
            "CREATE OR REPLACE FUNCTION mmsearch(",
            "CREATE TYPE bioseq_aligner_options AS",
            "SFUNC = vemptycount",
        ):
            if required not in sql_text:
                raise RuntimeError(
                    f"{source}: generated install SQL is missing relocatable "
                    f"extension surface: {required}"
                )

    provenance_terms = re.compile(
        r"\b(?:integration tests?|project|stage|phase|milestone|prototype|"
        r"proof[- ]of[- ]concept|revision|design document|tech spec|"
        r"doc(?:ument)?\s*0*\d+)\b",
        re.IGNORECASE,
    )
    provenance_comment = re.compile(
        r"^\s*--.*\b(?:integration tests?|project|stage|phase|milestone|"
        r"prototype|proof[- ]of[- ]concept|revision|design document|"
        r"tech spec|doc(?:ument)?\s*0*\d+)\b",
        re.IGNORECASE | re.MULTILINE,
    )
    if provenance_comment.search(sql_text):
        raise RuntimeError(
            f"{source}: generated install SQL contains a project/test "
            "provenance comment"
        )
    comment_on_statement = re.compile(
        r"\bCOMMENT\s+ON\b.*?\bIS\s+'(?P<body>(?:''|[^'])*)'\s*;",
        re.IGNORECASE | re.DOTALL,
    )
    for match in comment_on_statement.finditer(sql_text):
        if provenance_terms.search(match.group("body").replace("''", "'")):
            raise RuntimeError(
                f"{source}: generated install SQL contains project/test "
                "provenance in a COMMENT ON description"
            )
    return hashlib.sha256(sql_bytes).hexdigest()


def _verify_public_golden(
    identity_golden_path: Path,
    derived_stable: list[str],
    derived_installed: list[str],
) -> None:
    expected = _read_json(identity_golden_path)
    if (
        set(expected)
        != {
            "schema",
            "schema_revision",
            "stable_identities",
            "installed_identities",
        }
        or expected.get("schema") != "bioseq.website.sql_public_identities"
        or type(expected.get("schema_revision")) is not int
        or expected["schema_revision"] != 1
    ):
        raise RuntimeError(
            f"{identity_golden_path}: unexpected public-identity fixture schema"
        )
    expected_stable = sorted(
        _string_list(
            expected.get("stable_identities"),
            "stable_identities",
            identity_golden_path,
        )
    )
    expected_installed = sorted(
        _string_list(
            expected.get("installed_identities"),
            "installed_identities",
            identity_golden_path,
        )
    )
    if derived_stable != expected_stable:
        raise RuntimeError(
            f"{identity_golden_path}: generated manifest public stable "
            "identities differ from the approved website set"
        )
    if derived_installed != expected_installed:
        raise RuntimeError(
            f"{identity_golden_path}: generated manifest public installed "
            "identities differ from the approved website set"
        )


def _verify_artifact(artifact: ArtifactInput) -> ArtifactReceipt:
    if artifact.name not in PRODUCTION_EXTENSION_SCHEMAS:
        raise RuntimeError(f"unsupported website extension artifact {artifact.name!r}")

    control = _resolve_regular_file(
        artifact.control, f"{artifact.name} control"
    )
    install_sql = _resolve_regular_file(
        artifact.install_sql, f"{artifact.name} install SQL"
    )
    manifest_path = _resolve_regular_file(
        artifact.manifest, f"{artifact.name} SQL manifest"
    )
    dso = _resolve_regular_file(artifact.dso, f"{artifact.name} DSO")
    identity_golden_path = _resolve_regular_file(
        artifact.public_identities,
        f"{artifact.name} public-identity fixture",
    )

    if control.name != f"{artifact.name}.control":
        raise RuntimeError(
            f"{artifact.name} control has unexpected name: {control.name}"
        )
    install_match = re.fullmatch(
        rf"{re.escape(artifact.name)}--([A-Za-z0-9][A-Za-z0-9._+-]*)\.sql",
        install_sql.name,
    )
    if install_match is None:
        raise RuntimeError(
            f"{artifact.name} install SQL has unexpected name: {install_sql.name}"
        )
    version = install_match.group(1)
    if version != "1.0":
        raise RuntimeError(
            f"{artifact.name} website artifact version must be 1.0, got {version}"
        )

    allowed_dso_names = {
        f"{artifact.name}.so",
        f"{artifact.name}.dylib",
        f"{artifact.name}.dll",
    }
    if dso.name not in allowed_dso_names:
        raise RuntimeError(
            f"{artifact.name} DSO has unexpected name: {dso.name}"
        )
    if dso.stat().st_size <= 0:
        raise RuntimeError(f"{artifact.name} DSO is empty: {dso}")

    sql_bytes = install_sql.read_bytes()
    install_sql_sha256 = _verify_sql_text(
        artifact.name, install_sql, sql_bytes
    )
    manifest = _read_json(manifest_path)
    _require_current_manifest(manifest, manifest_path)
    if manifest.get("authority_mode") != "annotated_cpp":
        raise RuntimeError(
            f"{manifest_path}: authority_mode must be annotated_cpp"
        )
    if manifest.get("compiler_profile") != "production":
        raise RuntimeError(
            f"{manifest_path}: website requires the production SQL manifest"
        )
    manifest_extension_name = _resolved_manifest_extension_name(manifest)
    if manifest_extension_name != artifact.name:
        raise RuntimeError(
            f"{manifest_path}: extension_name {manifest_extension_name!r} "
            f"does not match {artifact.name!r}"
        )
    extension_schema = manifest.get("extension_schema")
    expected_schema = PRODUCTION_EXTENSION_SCHEMAS[artifact.name]
    if extension_schema != expected_schema:
        raise RuntimeError(
            f"{manifest_path}: extension_schema must be {expected_schema!r}"
        )
    statements = manifest.get("statements")
    statement_count = manifest.get("statement_count")
    if (
        not isinstance(statements, list)
        or type(statement_count) is not int
        or statement_count != len(statements)
    ):
        raise RuntimeError(
            f"{manifest_path}: statement_count does not match statements"
        )
    if manifest.get("script_sha256") != install_sql_sha256:
        raise RuntimeError(
            f"{manifest_path}: script_sha256 does not match {install_sql}"
        )

    control_sha256 = _verify_control(
        artifact, control, version, extension_schema
    )
    _verify_c_symbol_naming(manifest, manifest_path)
    all_stable, _ = _all_identity_projection(manifest, manifest_path)
    _verify_acl_companion_visibility(manifest, manifest_path)
    derived_stable, derived_installed = _public_projection(
        manifest, manifest_path
    )
    public_count = manifest.get("public_entity_count")
    if type(public_count) is not int or public_count != len(derived_stable):
        raise RuntimeError(
            f"{manifest_path}: public_entity_count does not match entity projection"
        )
    top_level_stable = _string_list(
        manifest.get("public_stable_identities"),
        "public_stable_identities",
        manifest_path,
    )
    top_level_installed = _string_list(
        manifest.get("public_installed_identities"),
        "public_installed_identities",
        manifest_path,
    )
    if top_level_stable != sorted(top_level_stable):
        raise RuntimeError(
            f"{manifest_path}: public_stable_identities must be sorted"
        )
    if top_level_installed != sorted(top_level_installed):
        raise RuntimeError(
            f"{manifest_path}: public_installed_identities must be sorted"
        )
    if top_level_stable != derived_stable or top_level_installed != derived_installed:
        raise RuntimeError(
            f"{manifest_path}: top-level public identity projection is stale"
        )
    if artifact.name == "bioseq_cache" and any(
        "bioseq_cache_test" in identity for identity in all_stable
    ):
        raise RuntimeError(
            f"{manifest_path}: production cache manifest contains test-support SQL"
        )
    _verify_public_golden(
        identity_golden_path, derived_stable, derived_installed
    )

    return ArtifactReceipt(
        name=artifact.name,
        control_sha256=control_sha256,
        install_sql_sha256=install_sql_sha256,
        manifest_sha256=_sha256_file(manifest_path),
        dso_sha256=_sha256_file(dso),
        stable_identities=tuple(all_stable),
        resolved_dso=dso,
    )


def verify(args: argparse.Namespace) -> tuple[list[ArtifactReceipt], str]:
    artifacts = [
        ArtifactInput(
            name=raw[0],
            control=Path(raw[1]),
            install_sql=Path(raw[2]),
            manifest=Path(raw[3]),
            dso=Path(raw[4]),
            public_identities=Path(raw[5]),
        )
        for raw in args.artifact
    ]
    artifact_names = tuple(artifact.name for artifact in artifacts)
    if artifact_names != PRODUCTION_ARTIFACT_ORDER:
        raise RuntimeError(
            "website production artifacts must be ordered exactly "
            "bioseq_cache, bioseq"
        )

    receipts = [_verify_artifact(artifact) for artifact in artifacts]
    resolved_dsos = [receipt.resolved_dso for receipt in receipts]
    if len(set(resolved_dsos)) != len(resolved_dsos):
        raise RuntimeError("production extensions must retain separate DSO files")
    dso_names = [path.name for path in resolved_dsos]
    if len(set(dso_names)) != len(dso_names):
        raise RuntimeError("production extensions must retain separate DSO names")

    identity_owner: dict[str, str] = {}
    for receipt in receipts:
        for identity in receipt.stable_identities:
            previous = identity_owner.get(identity)
            if previous is not None:
                raise RuntimeError(
                    "cross-manifest stable identity collision: "
                    f"{identity!r} is owned by {previous} and {receipt.name}"
                )
            identity_owner[identity] = receipt.name

    aggregate_records: list[str] = []
    for receipt in receipts:
        aggregate_records.extend(
            (
                f"{receipt.name}:control:{receipt.control_sha256}",
                f"{receipt.name}:install_sql:{receipt.install_sql_sha256}",
                f"{receipt.name}:manifest:{receipt.manifest_sha256}",
                f"{receipt.name}:dso:{receipt.dso_sha256}",
            )
        )
    aggregate_input = "\n".join(aggregate_records) + "\n"
    aggregate_sha256 = hashlib.sha256(aggregate_input.encode("utf-8")).hexdigest()
    return receipts, aggregate_sha256


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--artifact",
        action="append",
        nargs=6,
        required=True,
        metavar=(
            "NAME",
            "CONTROL",
            "INSTALL_SQL",
            "SQL_MANIFEST",
            "DSO",
            "PUBLIC_IDENTITIES",
        ),
    )
    parser.add_argument("--stamp", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    receipts, aggregate_sha256 = verify(args)
    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp_lines = [
        "bioseq.generated_sql_packages",
        "schema_revision=1",
        "artifact_order=bioseq_cache,bioseq",
    ]
    for receipt in receipts:
        stamp_lines.extend(
            (
                f"{receipt.name}.control_sha256={receipt.control_sha256}",
                f"{receipt.name}.install_sql_sha256={receipt.install_sql_sha256}",
                f"{receipt.name}.manifest_sha256={receipt.manifest_sha256}",
                f"{receipt.name}.dso_sha256={receipt.dso_sha256}",
            )
        )
    stamp_lines.append(f"aggregate_sha256={aggregate_sha256}")
    args.stamp.write_text("\n".join(stamp_lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
