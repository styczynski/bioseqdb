#!/usr/bin/env python3
"""Verify the generated PostgreSQL install artifact and website SQL manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"{path} must contain one JSON object")
    return value


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
        transitional = raw_entity.get("typed_install_owner")
        if not isinstance(public, bool) or not isinstance(transitional, bool):
            raise RuntimeError(
                f"{source}: entities[{index}] visibility/owner fields must be booleans"
            )
        if not public:
            continue
        if transitional:
            raise RuntimeError(
                f"{source}: transitional install owner at entities[{index}] "
                "cannot be website-public"
            )
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


def verify(args: argparse.Namespace) -> str:
    install_sql = args.install_sql.resolve(strict=True)
    manifest_path = args.manifest.resolve(strict=True)
    source_golden = args.source_golden.resolve(strict=True)
    identity_golden_path = args.public_identities.resolve(strict=True)

    if install_sql == source_golden:
        raise RuntimeError(
            "packaged install SQL resolves to the checked source golden; "
            "the generated build artifact must be package authority"
        )
    if install_sql.name != "bioseq--1.0.sql":
        raise RuntimeError(
            f"generated install artifact has unexpected name: {install_sql.name}"
        )

    sql_bytes = install_sql.read_bytes()
    try:
        sql_text = sql_bytes.decode("utf-8")
    except UnicodeDecodeError as error:
        raise RuntimeError("generated install SQL is not valid UTF-8") from error
    if not sql_text.endswith("\n"):
        raise RuntimeError("generated install SQL must end with a newline")
    if re.search(r"\bLANGUAGE\s+plpgsql\b", sql_text, re.IGNORECASE):
        raise RuntimeError("generated install SQL contains LANGUAGE plpgsql")
    if re.search(r"\bDO\s+\$[A-Za-z_0-9]*\$", sql_text, re.IGNORECASE):
        raise RuntimeError("generated install SQL contains an anonymous DO block")
    if re.search(
        r"^\s*(?:DROP|RESET)\b",
        sql_text,
        re.IGNORECASE | re.MULTILINE,
    ):
        raise RuntimeError(
            "generated install SQL contains historical DROP/RESET cleanup"
        )
    if re.search(r"\bpublic\.", sql_text, re.IGNORECASE):
        raise RuntimeError(
            "generated install SQL hardcodes public for a relocatable extension"
        )
    if "SET bioseq.logging = off;" not in sql_text:
        raise RuntimeError(
            "generated install SQL changed the canonical logging guard"
        )
    if re.search(r"\bSTART\s+WITH\b", sql_text, re.IGNORECASE):
        raise RuntimeError(
            "generated install SQL changed canonical sequence START syntax"
        )
    for required in (
        "CREATE FUNCTION vemptycount(",
        "CREATE OR REPLACE FUNCTION mmsearch(",
        "CREATE TYPE bioseq_aligner_options AS",
        "SFUNC = vemptycount",
    ):
        if required not in sql_text:
            raise RuntimeError(
                "generated install SQL is missing relocatable extension "
                f"surface: {required}"
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
            "generated install SQL contains a project/test provenance comment"
        )
    comment_on_statement = re.compile(
        r"\bCOMMENT\s+ON\b.*?\bIS\s+'(?P<body>(?:''|[^'])*)'\s*;",
        re.IGNORECASE | re.DOTALL,
    )
    for match in comment_on_statement.finditer(sql_text):
        if provenance_terms.search(match.group("body").replace("''", "'")):
            raise RuntimeError(
                "generated install SQL contains project/test provenance in "
                "a COMMENT ON description"
            )

    manifest = _read_json(manifest_path)
    if manifest.get("manifest_version") != 1:
        raise RuntimeError(f"{manifest_path}: manifest_version must be 1")
    if manifest.get("authority_mode") != "annotated_cpp":
        raise RuntimeError(
            f"{manifest_path}: authority_mode must be annotated_cpp"
        )
    digest = hashlib.sha256(sql_bytes).hexdigest()
    if manifest.get("script_sha256") != digest:
        raise RuntimeError(
            f"{manifest_path}: script_sha256 does not match {install_sql}"
        )
    _verify_c_symbol_naming(manifest, manifest_path)
    if source_golden.read_bytes() != sql_bytes:
        raise RuntimeError(
            "generated install SQL differs from the checked stale-golden oracle"
        )

    derived_stable, derived_installed = _public_projection(
        manifest, manifest_path
    )
    public_count = manifest.get("public_entity_count")
    if public_count != len(derived_stable):
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

    expected = _read_json(identity_golden_path)
    if expected.get("schema") != "bioseq.website.sql_public_identities/v1":
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
            "generated manifest public stable identities differ from the "
            "approved website set"
        )
    if derived_installed != expected_installed:
        raise RuntimeError(
            "generated manifest public installed identities differ from the "
            "approved public-schema website set"
        )
    return digest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--install-sql", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--source-golden", type=Path, required=True)
    parser.add_argument("--public-identities", type=Path, required=True)
    parser.add_argument("--stamp", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    digest = verify(args)
    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    args.stamp.write_text(
        f"bioseq.generated_sql_package/v1\nsha256={digest}\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
