#!/usr/bin/env python3
"""Check generated SQL reference coverage against source manifests and catalog.

The manifests determine declarations and authored prose. The catalog determines
installed routine/type metadata. Neither inventory is an execution allowlist.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

DOCUMENTED_KINDS = {
    "type", "function", "sql_function", "procedure", "aggregate", "operator",
    "cast", "schema", "table", "view", "sequence", "opclass", "access_method", "guc", "index",
}


def walk(nodes):
    for node in nodes:
        yield node
        yield from walk(node.get("children", []))


def node_text(node):
    return node.get("text", "") + "".join(node_text(child) for child in node.get("children", []))


def validate(model_path: Path, manifests: list[Path], catalog_path: Path) -> dict:
    model = json.loads(model_path.read_text())
    entities = model["sql_entities"]
    by_id = {entity["canonical_id"]: entity for entity in entities}
    assert len(by_id) == len(entities), "duplicate SQL documentation identities"
    assert not model.get("omitted_sql_entities"), "extension catalog entities were omitted"
    declared = 0
    authored = 0
    public = 0
    for path in manifests:
        manifest = json.loads(path.read_text())
        for source in manifest["entities"]:
            if source["kind"] not in DOCUMENTED_KINDS:
                continue
            declared += 1
            identity = source["identity"]
            assert identity in by_id, f"missing source declaration: {identity}"
            rendered = by_id[identity]
            assert rendered["summary"], f"empty SQL summary: {identity}"
            assert rendered["attributes"]["installed_identity"] == source["installed_identity"]
            authored_summary = source["brief"] or source["details"].split("\n\n", 1)[0]
            if source["kind"] in {"function", "sql_function", "procedure"}:
                assert authored_summary, f"SQL routine lacks an in-code description: {identity}"
            if authored_summary:
                authored += 1
                assert rendered["summary"] == authored_summary, f"rewritten authored summary: {identity}"
            if source["public_documentation"]:
                public += 1
            if source.get("source_path"):
                assert rendered["declaration"]["path"] == source["source_path"]
                assert rendered["declaration"]["line"] == source["source_line"]
            if source["kind"] == "type" and source["columns"]:
                assert len(rendered["fields"]) == len(source["columns"]), f"missing type fields: {identity}"
                assert all(field["type"] for field in rendered["fields"]), f"missing field SQL types: {identity}"
                assert all(field["description"] for field in rendered["fields"]), f"missing field descriptions: {identity}"
            nodes = list(walk(rendered["documentation"]))
            for example in source.get("documentation_examples", []) if source["kind"] != "guc" else []:
                assert any(node["kind"] == "paragraph" and node_text(node) == example["description"]
                           for node in nodes), f"lost source example description: {identity}"
                assert any(node["kind"] == "code_block" and node.get("text") == example["sql"]
                           for node in nodes), f"lost source SQL example: {identity}"
            bindings = ([source["operator_binding"]] if source.get("operator_binding") else []) + source.get("operator_aliases", [])
            for binding in bindings:
                target = binding["function_identity"] if source["kind"] == "operator" else binding["operator_identity"]
                assert target in by_id, f"operator alias target missing: {identity}/{target}"
                assert any(node["kind"] == "entity_link" and node.get("attributes", {}).get("entity_id") == target
                           for node in nodes), f"operator/function cross-link missing: {identity}/{target}"
                syntax = f"{binding['left_type']} {binding['symbol']} {binding['right_type']}"
                assert any(node["kind"] == "code_block" and node.get("text") == syntax
                           and node.get("attributes", {}).get("language") == "sql" for node in nodes), f"operator syntax missing: {identity}"
    catalog = json.loads(catalog_path.read_text())
    catalog_entities = catalog["entities"]
    installed = {entity.get("attributes", {}).get("installed_identity", entity["canonical_id"])
                 for entity in entities}
    # SQL routine aliases (varchar/character varying, sql_function/function) and
    # aggregate transition bindings may use different manifest identity strings.
    by_signature = {(entity["qualified_name"], entity["normalized_signature"]): entity
                    for entity in entities}
    for source in catalog_entities:
        identity = source["canonical_id"]
        key = (source["qualified_name"], source["normalized_signature"])
        assert identity in installed or key in by_signature, f"missing live extension member: {identity}"
        current = by_signature.get(key)
        if current:
            for name, definition in source.get("attributes", {}).items():
                if name.startswith("constraint "):
                    assert current["attributes"].get(name) == definition, f"lost table constraint: {identity}/{name}"
        if source.get("fields"):
            assert current is not None and current["fields"], f"lost live fields: {identity}"
            assert [(field["name"], field["type"]) for field in current["fields"]] == [
                (field["name"], field["type"]) for field in source["fields"]
            ], f"catalog field names/types changed: {identity}"
    for entity in entities:
        nodes = list(walk(entity["documentation"]))
        assert any(node["kind"] == "heading" and node_text(node) == "Examples" for node in nodes), f"SQL entity lacks examples section: {entity['canonical_id']}"
        assert any(node["kind"] == "code_block" and node.get("attributes", {}).get("language") == "sql"
                   and node.get("text", "").strip() for node in nodes), f"SQL entity lacks SQL example: {entity['canonical_id']}"
        for field in entity.get("fields", []):
            assert field["description"], f"SQL field lacks source description: {entity['canonical_id']}/{field['name']}"
    return {"declared_entities": declared, "authored_summaries": authored,
            "runnable_public_entities": public, "live_catalog_entities": len(catalog_entities),
            "documented_entities_with_overload_groups": len(entities)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, action="append", required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(validate(args.model, args.manifest, args.catalog), indent=2))


if __name__ == "__main__":
    main()
