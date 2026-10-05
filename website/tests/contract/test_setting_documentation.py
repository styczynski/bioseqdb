#!/usr/bin/env python3
"""Check source-owned GUC metadata, examples, and configuration-table links."""
from __future__ import annotations

import argparse
import json
import re
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit


def walk(nodes):
    for node in nodes:
        yield node
        yield from walk(node.get("children", []))


def text(node):
    return node.get("text", "") + "".join(text(child) for child in node.get("children", []))


def expected_attributes(registration):
    authority = registration["generated_live_authority"]
    documentation = registration["documentation"]
    result = {
        "Type": authority["vartype"], "Default": authority["boot_value"],
        "Minimum": authority["minimum"] if authority.get("minimum") is not None else "Not applicable",
        "Maximum": authority["maximum"] if authority.get("maximum") is not None else "Not applicable",
        "Context": authority["context"],
        "Visible in SHOW ALL": "Yes" if authority["visible_in_show_all"] else "No",
    }
    unit = authority.get("unit") or documentation.get("display_unit")
    if unit:
        result["Unit"] = unit
    if authority["vartype"] == "bool":
        result["Possible values"] = "on, off"
    elif authority["visible_enum_values"]:
        result["Possible values"] = ", ".join(authority["visible_enum_values"])
    return result


class ConfigurationLinks(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack = []
        self.code = None
        self.links = []

    def handle_starttag(self, tag, attributes):
        attrs = dict(attributes)
        if tag == "code" and any(parent == "table" for parent, _ in self.stack):
            self.code = {"parts": [], "anchors": [a for parent, a in self.stack if parent == "a"]}
        elif self.code is not None and tag == "a":
            self.code["anchors"].append(attrs)
        if tag not in {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}:
            self.stack.append((tag, attrs))

    def handle_endtag(self, tag):
        if tag == "code" and self.code is not None:
            spelling = "".join(self.code["parts"]).strip()
            if re.fullmatch(r"bioseq\.[a-z_][a-z0-9_.]*", spelling):
                self.links.append((spelling, self.code["anchors"]))
            self.code = None
        for index in range(len(self.stack) - 1, -1, -1):
            if self.stack[index][0] == tag:
                del self.stack[index:]
                break

    def handle_data(self, value):
        if self.code is not None:
            self.code["parts"].append(value)


def validate(model_path: Path, manifests: list[Path], site: Path | None = None):
    model = json.loads(model_path.read_text())
    entities = {entity["canonical_id"]: entity for entity in model["sql_entities"]}
    registrations = {}
    profiles = {}
    for path in manifests:
        manifest = json.loads(path.read_text())
        generation = manifest.get("guc_generation", {})
        for registration in generation.get("registrations", []):
            name = registration["name"]
            if name not in registrations or generation["profile"] == "production":
                registrations[name] = registration
                profiles[name] = generation["profile"]
    assert registrations, "no generated GUC authority supplied"
    example_count = 0
    value_count = 0
    for name, registration in registrations.items():
        identity = "sql:guc:" + name
        assert identity in entities, f"missing setting reference: {name}"
        entity = entities[identity]
        if profiles[name] == "test_indexes":
            assert entity["attributes"].get("Availability") == "Test builds only", f"{name}: test-only setting is not labeled"
        expected = expected_attributes(registration)
        for key, value in expected.items():
            assert entity["attributes"].get(key) == value, f"{name}: incorrect {key}: {entity['attributes'].get(key)!r}, expected {value!r}"
        documentation = registration["documentation"]
        assert documentation["description"].strip(), f"{name}: empty source description"
        nodes = list(walk(entity["documentation"]))
        paragraphs = [text(node) for node in nodes if node["kind"] == "paragraph"]
        assert documentation["description"] in paragraphs, f"{name}: source description missing or rewritten"
        rows = [[text(cell) for cell in node.get("children", [])]
                for node in nodes if node["kind"] == "table_row"]
        for value in documentation["possible_values"]:
            assert value["description"].strip(), f"{name}: empty meaning for {value['value']}"
            meaning = value["description"]
            if not value.get("compiled", True):
                meaning += " Not available in this build."
            if value.get("hidden", False):
                meaning += " Hidden value."
            assert [value["value"], meaning] in rows, f"{name}: missing meaning/availability for {value['value']}"
            value_count += 1
        examples = documentation["examples"]
        assert examples, f"{name}: no explained usage example"
        sql_blocks = [node.get("text", "") for node in nodes if node["kind"] == "code_block"
                      and node.get("attributes", {}).get("language") == "sql"]
        for example in examples:
            assert example["description"].strip() and example["sql"].strip(), f"{name}: empty example"
            assert example["description"] in paragraphs, f"{name}: example explanation missing or rewritten"
            assert example["sql"] in sql_blocks, f"{name}: SQL example missing or rewritten"
            example_count += 1
    linked_occurrences = 0
    if site is not None:
        site_manifest = json.loads((site / "site-manifest.json").read_text())
        version = site_manifest["version"]
        base = site_manifest["base_path"].rstrip("/")
        page = site / version / "tutorials/project_configuration_reference/index.html"
        parser = ConfigurationLinks()
        parser.feed(page.read_text())
        assert parser.links, "configuration tables have no GUC code names"
        table_names = {name for name, _ in parser.links}
        assert set(registrations) <= table_names, f"settings missing from configuration tables: {sorted(set(registrations) - table_names)}"
        tooltip_cache = {}
        for name, anchors in parser.links:
            identity = "sql:guc:" + name
            assert name in registrations, f"configuration table setting lacks manifest documentation: {name}"
            candidates = [anchor for anchor in anchors if "entity-ref" in anchor.get("class", "").split()
                          and anchor.get("data-entity-id") == identity]
            assert len(candidates) == 1, f"configuration table GUC must have one reference link: {name}"
            anchor = candidates[0]
            url = anchor.get("data-entity-data-url", "")
            assert url, f"setting link lacks tooltip data: {name}"
            if url not in tooltip_cache:
                path = unquote(urlsplit(url).path)
                assert path.startswith(base + "/"), f"tooltip URL escapes deployment mount: {url}"
                relative = path[len(base):].lstrip("/")
                tooltip_cache[url] = json.loads((site / relative).read_text())["entities"]
            record = tooltip_cache[url][identity]
            assert record["kind"] == "setting", f"tooltip mislabels GUC: {name}"
            assert urlsplit(anchor["href"]).path == record["route"], f"setting link disagrees with tooltip route: {name}"
            for key, value in expected_attributes(registrations[name]).items():
                if key != "Visible in SHOW ALL":
                    assert record["setting"].get(key) == value, f"tooltip lost {name}/{key}"
            linked_occurrences += 1
    return {"settings": len(registrations), "explained_examples": example_count,
            "value_meanings": value_count, "linked_configuration_occurrences": linked_occurrences}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, action="append", required=True)
    parser.add_argument("--documentation-manifest", type=Path, action="append", default=[])
    parser.add_argument("--site", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.model, args.manifest + args.documentation_manifest, args.site), indent=2))


if __name__ == "__main__":
    main()
