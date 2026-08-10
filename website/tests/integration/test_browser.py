#!/usr/bin/env python3
"""Playwright browser acceptance test for the generated static site
(§17.3 step 10, §20.7).

The same executed site model is rendered at TWO base paths, ``/`` and
``/bioseqdb/``, into separate byte trees; CMake runs this harness against the
normal staging root plus both fixtures. ``--base-path`` lets one already-executed
model be validated at each mount WITHOUT re-running any SQL. For each run this
harness:

  * starts support.static_fixture_server.StaticFixtureServer on 127.0.0.1,
    which reads the
    site's own security-headers.txt (§15.5) and applies every header to every
    response;
  * launches pinned headless Chromium and aborts any non-loopback request so a
    generated page cannot depend on the network;
  * loads the landing, a tutorial, and a reference page at the CONFIGURED mount
    (e.g. /bioseqdb/v0.1/...);
  * asserts the response carries the security-headers.txt contract (CSP incl.
    frame-ancestors, X-Content-Type-Options, Referrer-Policy, Permissions-Policy);
  * asserts the meta Content-Security-Policy is present and there is exactly one
    <h1>;
  * asserts no console errors and no uncaught page errors; and
  * exercises the tab widget if one is present (roving selection + panel show
    /hide), matching the progressive-enhancement JS.

On success it writes ``--stamp`` (bound to the staged manifest hash when
present), which §17.3 step 10 makes a prerequisite of publish.

Exit codes (documented so CMake can distinguish the cases):
  0  every browser assertion passed; stamp written.
  2  usage / environment error (bad arguments, missing site directory).
  3  a browser assertion FAILED; no stamp written.
  4  SKIP: the ``playwright`` package or its pinned Chromium build is not
     available in this environment. This is DISTINCT from an assertion failure
     (3): "browser tests requested but browser absent" must not read as "tests
     failed". No stamp written. The CMake acceptance wrapper treats every
     non-zero result, including this distinct diagnostic, as fatal because
     enabling browser acceptance makes the pinned browser mandatory.

Playwright's Chromium is not provisioned in every environment (it is installed
into <build>/website/playwright-browsers during infrastructure bootstrap). When
it is missing this harness prints a clear SKIP diagnostic and returns 4 instead
of crashing, so the mechanism is always exercisable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlsplit

_TESTS_ROOT = Path(__file__).resolve().parents[1]
_WEBSITE_ROOT = _TESTS_ROOT.parent
_REPOSITORY_ROOT = _WEBSITE_ROOT.parent
if str(_TESTS_ROOT) not in sys.path:
    sys.path.insert(0, str(_TESTS_ROOT))

from support.static_fixture_server import (  # noqa: E402
    StaticFixtureServer,
    parse_security_headers,
)

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_FAILED = 3
EXIT_SKIP = 4

_LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "[::1]", "::1"}
_REPOSITORY_URL = "https://github.com/styczynski/bioseqdb"
_EXPECTED_TUTORIALS = (
    "sequence_column_type",
    "similarity_search",
    "query_cache",
    "highway_runtime_dispatch",
)
_DESIGN_PALETTE = {
    "comment": "#6f8698",
    "keyword": "#4fb0d6",
    "function": "#f26522",
    "string": "#d8a15f",
    "type": "#74c8a8",
    "builtin": "#8aa0b5",
    "number": "#e0b04a",
    "macro": "#c99bd6",
    "operator": "#cbd8e4",
}


class AssertionFail(Exception):
    """A browser-level contract violation (maps to exit code 3)."""


def _check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionFail(message)


def _check_current_schema(
    document: object,
    schema: str,
    schema_revision: int,
    context: str,
) -> None:
    _check(isinstance(document, dict), f"{context} must be a JSON object")
    _check(
        document.get("schema") == schema
        and type(document.get("schema_revision")) is int
        and document["schema_revision"] == schema_revision,
        f"unexpected {context} schema contract",
    )


class _DesignParser(HTMLParser):
    """Collect semantic design nodes and their literal inline styles.

    The supplied design is an inline-style reference while the generated site
    translates those declarations to deterministic ``ld-*``/``dd-*`` classes.
    Recording semantic nodes lets the browser compare the two without publishing
    screenshots or a rewritten copy of the reference design.
    """

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.records: dict[str, dict[str, str]] = {}
        self._stack: list[dict[str, str]] = []
        self._query_card_div_children = 0
        self._hero_result_cells = 0

    def handle_starttag(self, tag: str, attrs) -> None:
        values = {key: (value or "") for key, value in attrs}
        record = {"tag": tag, "style": values.get("style", "")}
        if values.get("id"):
            self.records.setdefault(f"id:{values['id']}", record)
        for name in (
            "data-screen-label", "data-bodygrid", "data-search-wrap",
            "data-search-box", "data-rightrail", "data-term", "data-code",
        ):
            if name in values:
                suffix = values[name] if values[name] else "first"
                self.records.setdefault(f"{name}:{suffix}", record)

        parent = self._stack[-1] if self._stack else {}
        role = ""
        if parent.get("id") == "query-card" and tag == "div":
            self._query_card_div_children += 1
            if self._query_card_div_children == 2:
                role = "hero-result"
                self.records.setdefault("hero-result:first", record)

        in_hero_result = role == "hero-result" or any(
            item.get("role") == "hero-result" for item in self._stack)
        if in_hero_result and role != "hero-result":
            if tag == "div":
                self.records.setdefault("hero-result-label:first", record)
            elif tag == "table":
                self.records.setdefault("hero-result-table:first", record)
            elif tag == "th":
                self.records.setdefault("hero-result-header:first", record)
            elif tag == "td":
                self._hero_result_cells += 1
                key = (
                    "hero-result-accent-cell:first"
                    if self._hero_result_cells == 2
                    else "hero-result-cell:first"
                )
                self.records.setdefault(key, record)

        void_elements = {
            "area", "base", "br", "col", "embed", "hr", "img", "input",
            "link", "meta", "param", "source", "track", "wbr",
        }
        if tag not in void_elements:
            self._stack.append({"tag": tag, "id": values.get("id", ""), "role": role})

    def handle_endtag(self, tag: str) -> None:
        while self._stack:
            item = self._stack.pop()
            if item["tag"] == tag:
                break

    def handle_startendtag(self, tag: str, attrs) -> None:
        self.handle_starttag(tag, attrs)
        if self._stack and self._stack[-1]["tag"] == tag:
            self._stack.pop()


def _read_design_contract(design_source: Path) -> dict[str, dict[str, dict[str, str]]]:
    files = {"landing": design_source / "index.html", "docs": design_source / "docs.html"}
    contract: dict[str, dict[str, dict[str, str]]] = {}
    for label, path in files.items():
        _check(path.is_file(), f"supplied {label} design is missing: {path}")
        source = path.read_text(encoding="utf-8")
        parser = _DesignParser()
        parser.feed(source)
        contract[label] = parser.records

    required = {
        "landing": (
            "data-screen-label:BioseqDB landing", "id:nav", "id:annotate",
            "id:workflow", "id:cta", "hero-result:first",
            "hero-result-label:first", "hero-result-table:first",
            "hero-result-header:first", "hero-result-cell:first",
            "hero-result-accent-cell:first",
        ),
        "docs": (
            "data-screen-label:BioseqDB documentation", "id:dnav", "id:sidebar",
            "id:doc-search", "id:search-dd", "data-bodygrid:first",
            "data-search-wrap:first", "data-search-box:first", "data-rightrail:first",
            "data-term:first", "data-code:first",
        ),
    }
    for label, keys in required.items():
        for key in keys:
            _check(key in contract[label],
                   f"supplied {label} design is missing semantic node {key}")

    docs_text = files["docs"].read_text(encoding="utf-8").lower()
    for role, colour in _DESIGN_PALETTE.items():
        _check(colour in docs_text,
               f"supplied docs design does not contain {role} palette colour {colour}")
    return contract


def _translated_style_catalog(source: str, label: str, prefix: str,
                              page_class: str) -> list[str]:
    """Reproduce the translator's declaration numbering without rewriting files.

    Every authored declaration must survive byte-for-byte in the generated CSS;
    this catches a stale checked-in translation even when a few sampled elements
    happen to look acceptable.
    """
    start = source.index(f'<div data-screen-label="{label}"')
    end = source.index("</x-dc>", start)
    markup = source[start:end]
    if prefix == "ld-":
        for old, new in {
            "color:{{ n1Color }}": "color:#f26522",
            "color:{{ n2Color }}": "color:#64798b",
            "color:{{ n3Color }}": "color:#64798b",
            "color:{{ t1Color }}": "color:#f2f7fb",
            "color:{{ t2Color }}": "color:#a3b7c9",
            "color:{{ t3Color }}": "color:#a3b7c9",
        }.items():
            markup = markup.replace(old, new)
    styles: dict[str, str] = {}
    hovers: dict[str, str] = {}
    for match in re.finditer(r"<!--.*?-->|</?[^<>]+>", markup, flags=re.S):
        tag = match.group(0)
        if tag.startswith(("<!--", "</", "<!")):
            continue
        style = re.search(r'\sstyle="([^"]*)"', tag)
        if style and style.group(1) not in styles:
            styles[style.group(1)] = f"{prefix}s{len(styles) + 1:04d}"
        hover = re.search(r'\sstyle-hover="([^"]*)"', tag)
        if hover and hover.group(1) not in hovers:
            hovers[hover.group(1)] = f"{prefix}h{len(hovers) + 1:03d}"
    rules = [f".{page_class} .{name}{{{declaration}}}"
             for declaration, name in styles.items()]
    rules.extend(f".{page_class} .{name}:hover{{{declaration}}}"
                 for declaration, name in hovers.items())
    return rules


def _assert_translator_declaration_parity(site: Path, design_source: Path) -> None:
    configurations = (
        ("index.html", "BioseqDB landing", "ld-", "landing-design-page",
         "landing-design.css"),
        ("docs.html", "BioseqDB documentation", "dd-", "docs-design-page",
         "docs-design.css"),
    )
    for filename, label, prefix, page_class, css_name in configurations:
        source = (design_source / filename).read_text(encoding="utf-8")
        css_paths = sorted(site.glob(f"assets/*/css/{css_name}"))
        _check(len(css_paths) == 1,
               f"expected one published {css_name}, got {css_paths}")
        css = css_paths[0].read_text(encoding="utf-8")
        rules = _translated_style_catalog(source, label, prefix, page_class)
        missing = [rule for rule in rules if rule not in css]
        _check(not missing,
               f"{css_name} lost {len(missing)} literal design declaration(s); "
               f"first missing rule: {missing[0] if missing else ''}")

        global_match = re.search(
            r"<helmet>.*?<style>\s*(.*?)\s*</style>.*?</helmet>", source, re.S)
        _check(global_match is not None,
               f"supplied design {filename} has no global style block")
        global_css = global_match.group(1).strip()
        _check(global_css in css,
               f"{css_name} does not contain the literal supplied global CSS")


def _mount_prefix(base_path: str) -> str:
    normalized = "/" + base_path.strip("/") if base_path.strip("/") else ""
    return normalized


def _public_route(base_path: str, relative: str) -> str:
    return f"{_mount_prefix(base_path)}/{relative.lstrip('/')}"


def _node_text(node: dict) -> str:
    return str(node.get("text", "")) + "".join(
        _node_text(child) for child in node.get("children", []))


def _result_tables(nodes: list[dict]) -> list[list[list[str]]]:
    tables: list[list[list[str]]] = []
    for node in nodes:
        if (node.get("kind") == "table"
                and node.get("attributes", {}).get("class") == "result-table"):
            rows: list[list[str]] = []
            for row in node.get("children", []):
                if row.get("kind") != "table_row":
                    continue
                rows.append([_node_text(cell) for cell in row.get("children", [])])
            tables.append(rows)
        tables.extend(_result_tables(node.get("children", [])))
    return tables


def _nodes_of_kind(nodes: list[dict], kind: str) -> list[dict]:
    found: list[dict] = []
    for node in nodes:
        if node.get("kind") == kind:
            found.append(node)
        found.extend(_nodes_of_kind(node.get("children", []), kind))
    return found


def _assert_authored_content_contract(version: str) -> None:
    tutorial_path = (
        _WEBSITE_ROOT / version / "tutorials" / "similarity_search.md"
    )
    sequence_path = (
        _WEBSITE_ROOT / version / "tutorials" / "sequence_column_type.md"
    )
    cache_path = _WEBSITE_ROOT / version / "tutorials" / "query_cache.md"
    highway_path = (
        _WEBSITE_ROOT / version / "tutorials" / "highway_runtime_dispatch.md"
    )
    landing_path = _WEBSITE_ROOT / version / "landing_examples.md"
    common_path = (
        _REPOSITORY_ROOT
        / "bioseq"
        / "tests"
        / "integration"
        / "support"
        / "index.cppm"
    )
    for path in (
        tutorial_path, sequence_path, cache_path, highway_path,
        landing_path, common_path,
    ):
        _check(path.is_file(), f"acceptance source is missing: {path}")

    common = common_path.read_text(encoding="utf-8")
    fixture_match = re.search(
        r"export inline std::vector<seq_row> hoe_seed_rows\(\)\s*\{(.*?)\n\}",
        common, re.S)
    _check(fixture_match is not None, "cannot resolve canonical hoe_seed_rows source")
    fixture_pairs = re.findall(
        r'\.sequences\s*=\s*\{\s*"([ACGT]+)",\s*"([ACGT]+)"\s*\}',
        fixture_match.group(1), re.S)
    _check(len(fixture_pairs) == 5,
           f"canonical hoe_seed_rows source has {len(fixture_pairs)} rows, expected 5")
    probe_match = re.search(
        r'kHoeProbeSequence\s*=\s*\n?\s*"([ACGT]+)"', common)
    _check(probe_match is not None, "cannot resolve canonical kHoeProbeSequence source")

    tutorial = tutorial_path.read_text(encoding="utf-8")
    for target, _secondary_probe in fixture_pairs:
        _check(tutorial.count(target) == 1,
               "similarity tutorial does not copy each canonical hoe target exactly once")
    probe = probe_match.group(1)
    _check(tutorial.count(f"'{probe}'") == 2,
           "similarity tutorial must use exact kHoeProbeSequence in EXPLAIN and visible SQL")
    for assertion in (
        "EXPLAIN (FORMAT JSON)", "'{0,Plan,\"Node Type\"}'",
        "'{0,Plan,\"Custom Plan Provider\"}'", "'Custom Scan'", "'BioseqScan'",
    ):
        _check(assertion in tutorial,
               f"similarity tutorial setup is missing planner assertion {assertion}")
    _check("-- bioseq:expected" not in tutorial,
           "similarity tutorial must not author expected output rows")

    sequence = sequence_path.read_text(encoding="utf-8")
    _check("bioseq.seq('dna4', 64)" in sequence
           and "match=\"'dna4'\" title=\"Alphabet\"" in sequence
           and 'match="64" title="Storage-unit width"' in sequence,
           "sequence anatomy must be driven by the exact two Markdown annotations")
    _check(not re.search(r"temporary|placeholder|lorem|symbol_count", sequence, re.I),
           "sequence tutorial contains disabled or prototype content")
    cache = cache_path.read_text(encoding="utf-8")
    for spelling in (
        "order: 30", "bioseq_cache.query_cache.mode = 'exact_query'",
        "bioseq_cache.query_cache.mode = 'reusable_fragments'",
        "minimizers_per_sequence", "minimizers_per_fragment",
        "bioseq_cache.stats()", "bioseq_cache.entries()",
        "bioseq_cache.invalidate_relation", "bioseq_cache.invalidate_all()",
        "expect=error sqlstate=42501",
    ):
        _check(spelling in cache, f"cache tutorial is missing {spelling}")
    highway = highway_path.read_text(encoding="utf-8")
    for spelling in (
        "order: 40", "EXPLAIN (ANALYZE, VERBOSE, FORMAT JSON)",
        "Highway Compiled Targets", "CPU-Supported Targets",
        "Runnable Targets", "Planned Architecture",
        "Executed Architecture",
    ):
        _check(spelling in highway, f"Highway tutorial is missing {spelling}")
    landing = landing_path.read_text(encoding="utf-8")
    _check(not re.search(r"temporary|placeholder|lorem|symbol_count", landing, re.I),
           "landing examples contain disabled or prototype content")


def _load_static_contract(site: Path, base_path: str, version: str,
                          model_path: Path, design_source: Path) -> dict:
    """Fail before Chromium launch if generated artifacts already violate the
    routing/content/data-source contract."""
    _assert_authored_content_contract(version)
    _check(model_path.is_file(), f"resolved site model is missing: {model_path}")
    model = json.loads(model_path.read_text(encoding="utf-8"))
    _check_current_schema(model, "bioseq.website.site_model", 1, "site model")
    _check(
        model.get("parser_identity") == "bioseq.website.parser"
        and type(model.get("parser_revision")) is int
        and model["parser_revision"] == 1
        and model.get("binding_schema") == "bioseq.website.bindings"
        and type(model.get("binding_schema_revision")) is int
        and model["binding_schema_revision"] == 1,
        "site model does not declare current parser and binding contracts",
    )
    provenance = model.get("provenance", {})
    _check(bool(provenance.get("postgres_server_version"))
           and bool(provenance.get("extension_version")),
           "site model lacks live PostgreSQL/extension provenance")
    _check(re.fullmatch(r"[0-9a-f]{64}",
                        provenance.get("staged_extension_sha256", "")) is not None,
           "site model is not bound to the staged extension artifact-set hash")

    tutorials = model.get("tutorials", [])
    slugs = tuple(item.get("slug") for item in tutorials)
    _check(slugs == _EXPECTED_TUTORIALS,
           f"site model must contain the ordered tutorials "
           f"{_EXPECTED_TUTORIALS}, got {slugs}")
    by_slug = {item["slug"]: item for item in tutorials}
    _check(len(by_slug["sequence_column_type"].get("sql_examples", [])) == 2,
           "sequence_column_type must carry two runnable examples")
    _check(len(by_slug["similarity_search"].get("sql_examples", [])) == 1,
           "similarity_search must carry one runnable example")
    _check(len(by_slug["query_cache"].get("sql_examples", [])) == 3,
           "query_cache must carry three runnable examples")
    _check(len(by_slug["highway_runtime_dispatch"].get("sql_examples", [])) == 1,
           "highway_runtime_dispatch must carry one runnable example")

    for tutorial in tutorials:
        for example in tutorial.get("sql_examples", []):
            _check(not example.get("setup_sql") and not example.get("teardown_sql"),
                   f"resolved model leaked hidden SQL for {example.get('canonical_id')}")
            visible = example.get("visible_sql", "")
            _check(bool(visible.strip()),
                   f"runnable example {example.get('canonical_id')} has no visible SQL")
            _check("TEMPORARY" not in visible and "bioseq:setup" not in visible,
                   f"runnable example {example.get('canonical_id')} leaked disabled/setup text")
        tables = _result_tables(tutorial.get("content", []))
        expected_tables = 2 if tutorial["slug"] == "query_cache" else len(
            tutorial.get("sql_examples", []))
        _check(len(tables) == expected_tables,
               f"{tutorial['slug']}: {len(tables)} captured result table(s), "
               f"expected {expected_tables}")

    sequence_annotations = _nodes_of_kind(
        by_slug["sequence_column_type"].get("content", []), "code_annotation")
    _check(len(sequence_annotations) == 2,
           "sequence type anatomy must contain exactly two Markdown annotations")
    annotation_titles = [item.get("attributes", {}).get("title")
                         for item in sequence_annotations]
    _check(annotation_titles == ["Alphabet", "Storage-unit width"],
           f"unexpected anatomy annotation titles: {annotation_titles}")

    similarity_sql = by_slug["similarity_search"]["sql_examples"][0]["visible_sql"]
    required_sql = (
        "public.mmsearch", "bioseq.query_start", "bioseq.query_end",
        "bioseq.target_start", "bioseq.target_end", "bioseq.cigar",
        "CTAGTCACTGTAGCCTGATCGTAGCTAGT",
    )
    for spelling in required_sql:
        _check(spelling in similarity_sql,
               f"similarity tutorial visible SQL is missing {spelling}")

    cache_examples = by_slug["query_cache"]["sql_examples"]
    _check("bioseq_cache.stats()" in cache_examples[0]["visible_sql"],
           "exact-query cache example does not publish production stats")
    _check("bioseq_cache.entries()" in cache_examples[1]["visible_sql"],
           "reusable-fragment cache example does not inspect entries")
    _check(cache_examples[2].get("expected_sqlstate") == "42501"
           and "invalidate_relation" in cache_examples[2]["visible_sql"],
           "cache invalidation error example lost its exact SQLSTATE contract")
    highway_sql = by_slug["highway_runtime_dispatch"]["sql_examples"][0]
    _check("EXPLAIN (ANALYZE, VERBOSE, FORMAT JSON)" in highway_sql["visible_sql"],
           "Highway tutorial does not execute an analyzed JSON plan")
    highway_results = "\n".join(
        cell
        for table in _result_tables(
            by_slug["highway_runtime_dispatch"].get("content", []))
        for row in table
        for cell in row
    )
    for label in (
        "Highway Compiled Targets", "CPU-Supported Targets",
        "Runnable Targets", "Planned Architecture", "Executed Architecture",
    ):
        _check(label in highway_results,
               f"executed Highway plan is missing label {label}")

    landing = {item.get("slot"): item for item in model.get("landing_examples", [])}
    expected_slots = {"hero_query", "sequence_storage", "similarity_search"}
    _check(set(landing) == expected_slots,
           f"landing model slots are {sorted(landing)}, expected {sorted(expected_slots)}")
    for slot, item in landing.items():
        _check(item.get("has_result") is True and isinstance(item.get("result"), dict),
               f"landing slot {slot} has no captured PostgreSQL result")
        for column in item["result"].get("columns", []):
            _check("type_oid" not in column and bool(column.get("type_name")),
                   f"landing slot {slot} has an unresolved PostgreSQL column type")
        _check(bool(item.get("visible_sql", "").strip()),
               f"landing slot {slot} has no visible SQL")
        _check("TEMPORARY" not in item.get("visible_sql", ""),
               f"landing slot {slot} contains disabled SQL")

    filter_sql = landing["similarity_search"]["visible_sql"]
    for spelling in (
        "CREATE TABLE reference_reads", "organism = 'Homo sapiens'",
        "collected_on >= DATE '2025-01-01'", "bioseq.len(seq) >= 1000",
        "public.mmsearch", "ORDER BY quality_score DESC, accession", "LIMIT 25",
    ):
        _check(spelling in filter_sql,
               f"landing Filter and sort SQL is missing {spelling}")
    _check([column.get("name") for column in
            landing["similarity_search"]["result"].get("columns", [])]
           == ["id", "accession", "organism", "bases"],
           "landing Filter and sort did not publish its final SELECT result")

    load_sql = landing["sequence_storage"]["visible_sql"]
    for spelling in (
        "CREATE TABLE run_reads", "row_no", "bigserial PRIMARY KEY",
        "run_name      name NOT NULL DEFAULT 'run_042'",
        "SET bioseq.index.insert_maintenance.enabled = on",
        "bioseq.load", "landing_reads.fasta", "bioseq.column_mapping",
        "INSERT INTO run_reads", "public.mmsearch", "bioseq.target_start",
        "bioseq.target_end", "ORDER BY collected_on DESC, read_id",
    ):
        _check(spelling in load_sql,
               f"landing Load and index SQL is missing {spelling}")
    _check([column.get("name") for column in
            landing["sequence_storage"]["result"].get("columns", [])]
           == ["read_id", "sample_id", "match_start", "match_end"],
           "landing Load and index did not publish its final SELECT result")

    tutorials_dir = site / version / "tutorials"
    _check(not (tutorials_dir / "index.html").exists(),
           f"forbidden tutorial-list artifact exists: {tutorials_dir / 'index.html'}")
    for slug in _EXPECTED_TUTORIALS:
        _check((tutorials_dir / slug / "index.html").is_file(),
               f"direct tutorial route is missing: {slug}")

    search_path = site / version / "search-index.json"
    _check(search_path.is_file(), f"generated search index is missing: {search_path}")
    search = json.loads(search_path.read_text(encoding="utf-8"))
    _check_current_schema(search, "bioseq.website.search", 2, "search index")
    records = search.get("records", [])
    tutorial_records = [record for record in records if record.get("group") == "Tutorials"]
    _check({record.get("id") for record in tutorial_records}
           == {f"tutorial:{slug}" for slug in _EXPECTED_TUTORIALS},
           "search index must contain the four direct tutorial records")
    forbidden_route = _public_route(base_path, f"{version}/tutorials/")
    _check(not any(record.get("route") == forbidden_route for record in records),
           f"search index still contains forbidden tutorial-list route {forbidden_route}")
    for slug in _EXPECTED_TUTORIALS:
        expected = _public_route(base_path, f"{version}/tutorials/{slug}/")
        _check(any(record.get("route") == expected for record in tutorial_records),
               f"search index is missing direct route {expected}")

    tooltip_paths = sorted((site / version).glob("entities.*.json"))
    _check(len(tooltip_paths) == 1,
           f"expected one content-addressed tooltip dataset, got {tooltip_paths}")
    tooltip = json.loads(tooltip_paths[0].read_text(encoding="utf-8"))
    _check_current_schema(
        tooltip, "bioseq.website.entity_tooltips", 1, "entity tooltips"
    )
    tooltip_records = tooltip.get("entities", {})
    required_tooltips = {
        "sql:type:bioseq.seq",
        "sql:function:bioseq.len(bioseq.seq)",
        "sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)",
        "sql:function:bioseq.query_start(anyelement)",
        "sql:function:bioseq.query_end(anyelement)",
        "sql:function:bioseq.target_start(anyelement)",
        "sql:function:bioseq.target_end(anyelement)",
        "sql:function:bioseq.cigar(anyelement)",
        "sql:function:bioseq_cache.version()",
        "sql:function:bioseq_cache.stats()",
        "sql:function:bioseq_cache.entries()",
        "sql:function:bioseq_cache.invalidate_all()",
        "sql:function:bioseq_cache.invalidate_relation(regclass)",
        "sql:function:bioseq_cache.reset_stats()",
    }
    _check(required_tooltips <= set(tooltip_records),
           f"tooltip dataset is missing {sorted(required_tooltips - set(tooltip_records))}")

    forbidden_text = (
        "lorem ipsum", "temporary:", "late august 2027", "v0.9 alpha",
        "loading &amp; searching sequences",
    )
    for path in site.rglob("*.html"):
        lowered = path.read_text(encoding="utf-8").lower()
        for token in forbidden_text:
            _check(token not in lowered,
                   f"published HTML contains design placeholder {token!r}: {path}")

    design = _read_design_contract(design_source)
    _assert_translator_declaration_parity(site, design_source)
    return {
        "model": model,
        "tutorials": by_slug,
        "landing": landing,
        "search_route": _public_route(base_path, f"{version}/search-index.json"),
        "tooltip_route": _public_route(
            base_path, f"{version}/{tooltip_paths[0].name}"),
        "tooltip_records": tooltip_records,
        "design": design,
    }


def _load_playwright():
    """Return the sync_api module, or None if Playwright is unavailable."""
    try:
        from playwright.sync_api import sync_playwright  # noqa: F401
    except ImportError:
        return None
    import playwright.sync_api as api
    return api


def _discover_entity_route(site: Path, base: str, version: str) -> str:
    """Pick a real rendered entity page (SQL preferred) to exercise §12.3
    enrichment; fall back to the reference index."""
    reference = site / version / "reference"
    for domain in ("sql", "cpp"):
        domain_dir = reference / domain
        if not domain_dir.is_dir():
            continue
        for kind_dir in sorted(domain_dir.iterdir()):
            if not kind_dir.is_dir():
                continue
            for entity_dir in sorted(kind_dir.iterdir()):
                if entity_dir.is_dir() and (entity_dir / "index.html").is_file():
                    return (f"{base}/{version}/reference/{domain}/"
                            f"{kind_dir.name}/{entity_dir.name}/")
    return f"{base}/{version}/reference/"


def _default_routes(site: Path, mount: str, version: str) -> dict[str, str]:
    base = mount if mount else ""
    return {
        "landing": f"{base}/",
        "tutorial_sequence":
            f"{base}/{version}/tutorials/sequence_column_type/",
        "tutorial_similarity":
            f"{base}/{version}/tutorials/similarity_search/",
        "tutorial_cache":
            f"{base}/{version}/tutorials/query_cache/",
        "tutorial_highway":
            f"{base}/{version}/tutorials/highway_runtime_dispatch/",
        "reference": f"{base}/{version}/reference/",
        "entity": _discover_entity_route(site, base, version),
        "error": f"{base}/error.html",
    }


def _assert_headers(response, expected: list[tuple[str, str]]) -> None:
    got = {k.lower(): v for k, v in response.headers.items()}
    for name, value in expected:
        key = name.lower()
        _check(key in got, f"response is missing header {name!r} from security-headers.txt")
        # CSP/Permissions-Policy whitespace can be normalized by the stack; compare
        # on a whitespace-insensitive basis.
        norm_got = " ".join(got[key].split())
        norm_exp = " ".join(value.split())
        _check(norm_got == norm_exp,
               f"header {name!r} = {got[key]!r}, expected {value!r}")
    # frame-ancestors is header-only and must be present in the applied CSP.
    csp = got.get("content-security-policy", "")
    _check("frame-ancestors" in csp,
           "applied Content-Security-Policy header lacks frame-ancestors")


def _exercise_tabs(page) -> None:
    """Exercise the three literal landing example selectors and every synced panel."""
    tabs = page.locator("button[data-example-tab]")
    _check(tabs.count() == 3,
           f"landing example selector requires 3 buttons, got {tabs.count()}")
    _check(page.locator("button[data-example-tab][aria-pressed='true']").count() == 1,
           "landing examples must start with exactly one selected button")
    second = tabs.nth(1)
    second.click()
    _check(second.get_attribute("aria-pressed") == "true",
           "clicked example did not become aria-pressed")
    _check(tabs.nth(0).get_attribute("aria-pressed") == "false",
           "previously selected tab stayed selected after switching")
    for index in range(3):
        panels = page.locator(f'[data-example-panel="{index}"]')
        _check(panels.count() >= 3,
               f"landing example {index} lost one of its synchronized panels")
        states = panels.evaluate_all("nodes => nodes.map(node => node.hidden)")
        _check(all(not state for state in states) if index == 1 else all(states),
               f"landing example {index} panel visibility did not follow selection: {states}")
    second.focus()
    page.keyboard.press("ArrowRight")
    _check(tabs.nth(2).get_attribute("aria-pressed") == "true"
           and tabs.nth(2).evaluate("element => element === document.activeElement"),
           "ArrowRight did not select and focus the next landing example")
    tabs.first.click()
    install_copy = page.locator("button[data-copy-install]")
    _check(install_copy.count() == 1, "landing install copy button is missing")
    install_copy.click()
    done = install_copy.locator("[data-copy-done]")
    done.wait_for(state="visible", timeout=3000)
    _check(not done.evaluate("node => node.hidden")
           and install_copy.locator("[data-copy-idle]").evaluate("node => node.hidden"),
           "landing install copy interaction did not show its completion state")
    page.evaluate(
        """() => {
          Object.defineProperty(navigator, 'clipboard', {
            configurable: true,
            value: {writeText: () => Promise.reject(new Error('denied'))}
          });
          document.execCommand = () => false;
        }""")
    install_copy.click()
    failed = install_copy.locator("[data-copy-idle]", has_text="Copy failed")
    try:
        failed.wait_for(state="visible", timeout=3000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"landing copy failure was announced as success: {error}")
    _check(done.evaluate("node => node.hidden"),
           "landing copy failure incorrectly exposed the Copied state")


def _exercise_tooltip(page, request_urls: list[str], contract: dict) -> None:
    """A generated entity ref must fetch the content-addressed tooltip dataset;
    a hardcoded in-script entity map cannot satisfy this contract."""
    refs = page.locator("a.entity-ref[data-entity-id][data-entity-data-url]")
    _check(refs.count() > 0, "tutorial has no generated entity tooltip references")
    ref = refs.first
    entity_id = ref.get_attribute("data-entity-id") or ""
    data_url = ref.get_attribute("data-entity-data-url") or ""
    _check(entity_id in contract["tooltip_records"],
           f"entity ref {entity_id!r} is absent from generated tooltip JSON")
    _check(data_url == contract["tooltip_route"],
           f"entity ref dataset is {data_url!r}, expected {contract['tooltip_route']!r}")
    ref.scroll_into_view_if_needed()
    ref.focus()
    tip = page.locator("#entity-tooltip:visible, #entity-pop:visible").first
    try:
        tip.wait_for(state="visible", timeout=3000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"entity-ref focus did not show a tooltip: {error}")
    expected = contract["tooltip_records"][entity_id]
    tip_text = tip.inner_text().strip()
    _check(bool(tip_text), "tooltip shown with no content")
    expected_name = expected.get("qualified_name") or expected.get("title") or ""
    _check(expected_name in tip_text,
           f"tooltip does not contain generated entity name {expected_name!r}")
    view = tip.locator(".entity-pop-view")
    focusable = tip.locator(
        "a, button, input, select, textarea, [tabindex]:not([tabindex='-1'])")
    _check(view.count() <= 1 and focusable.count() == 0,
           "role=tooltip contains a focusable descendant")
    _check(tip.evaluate("node => getComputedStyle(node).pointerEvents") == "none",
           "role=tooltip unexpectedly accepts pointer interaction")
    if refs.count() > 1:
        second = refs.nth(1)
        second.focus()
        try:
            tip.wait_for(state="visible", timeout=3000)
        except Exception as error:  # noqa: BLE001
            raise AssertionFail(f"second entity ref did not show tooltip: {error}")
        _check(not ref.get_attribute("aria-describedby")
               and second.get_attribute("aria-describedby") == tip.get_attribute("id"),
               "switching entity refs leaked aria-describedby on the prior trigger")
    requested_paths = {urlsplit(url).path for url in request_urls}
    _check(contract["tooltip_route"] in requested_paths,
           f"tooltip interaction never fetched {contract['tooltip_route']}")
    page.keyboard.press("Escape")
    try:
        tip.wait_for(state="hidden", timeout=3000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"Escape did not dismiss the tooltip: {error}")


def _exercise_copy(page) -> None:
    """If a fenced code block is present (§10.5), verify every Copy button is
    keyboard-focusable and targets a real code element by id (§20.7)."""
    buttons = page.locator("button.copy-button[data-copy-target]")
    count = buttons.count()
    _check(count > 0, "tutorial has no live code-copy controls")
    for index in range(count):
        button = buttons.nth(index)
        target_id = button.get_attribute("data-copy-target")
        _check(bool(target_id), "copy button has an empty data-copy-target")
        target = page.locator(f"#{target_id}")
        _check(target.count() == 1,
               f"copy button target #{target_id} does not resolve to one element")
    # The first button must be reachable and operable by keyboard (no exception).
    first = buttons.first
    first.focus()
    _check(first.evaluate("el => el === document.activeElement"),
           "copy button did not accept keyboard focus")
    first.click()
    try:
        page.locator("button.copy-button[data-copy]", has_text="Copied").first.wait_for(
            state="visible", timeout=3000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"copy interaction did not acknowledge completion: {error}")
    page.evaluate(
        """() => {
          Object.defineProperty(navigator, 'clipboard', {
            configurable: true,
            value: {writeText: () => Promise.reject(new Error('denied'))}
          });
          document.execCommand = () => false;
        }""")
    failure_button = buttons.nth(1 if buttons.count() > 1 else 0)
    failure_button.click()
    try:
        failure_button.filter(has_text="Copy failed").wait_for(
            state="visible", timeout=3000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"docs copy failure was announced as success: {error}")


def _exercise_annotations(page) -> None:
    """The sequence-type anatomy is Markdown-owned and must reproduce the two-card
    supplied component exactly."""
    anatomy = page.locator("figure.annotated-type[data-annotated-type]")
    _check(anatomy.count() == 1, "Markdown anatomy did not render its stable component")
    expression = anatomy.locator(".annotated-type-expression code[data-code]")
    _check(expression.count() == 1
           and (expression.text_content() or "").strip() == "bioseq.seq('dna4', 64)",
           "annotated anatomy expression differs from the supplied component")
    marks = anatomy.locator("mark.anno.annotated-type-value[data-anno]")
    count = marks.count()
    _check(count == 2, f"sequence anatomy requires 2 annotation marks, got {count}")
    cards = anatomy.locator(
        "ol.annotated-type-cards[data-split] > li.annotated-type-card[data-anno-card]")
    _check(cards.count() == count,
           f"{count} annotation mark(s) but {cards.count()} callout card(s)")
    nums = cards.locator(".anno-card-num")
    _check(nums.count() == count, "each callout card must carry a number badge")
    _check([marks.nth(i).inner_text() for i in range(count)] == ["'dna4'", "64"],
           "anatomy marks must target exactly 'dna4' and 64")
    titles = cards.locator(".anno-card-title")
    _check([(titles.nth(i).text_content() or "").strip() for i in range(titles.count())]
           == ["Alphabet", "Storage-unit width"],
           "anatomy callout titles do not match the supplied two-card component")
    body = cards.all_inner_texts()
    _check("four-symbol DNA alphabet" in body[0]
           and "does not limit the sequence to 64 bases" in body[1],
           "anatomy callout explanations are not the authored Markdown text")


def _exercise_code_entities(page, expected_ids: set[str]) -> None:
    """Verify repeatable fence entity declarations became exact in-code hover
    ranges after Prism without changing source text."""
    code = page.locator("pre code[data-entity-ranges]").first
    _check(code.count() == 1, "required code block has no entity-range metadata")
    refs = code.locator("a.entity-ref[data-entity-id]")
    ids = {refs.nth(i).get_attribute("data-entity-id") or ""
           for i in range(refs.count())}
    _check(ids == expected_ids,
           f"decorated code entity IDs are {sorted(ids)}, expected {sorted(expected_ids)}")
    _check(bool(refs.first.get_attribute("href")), "code entity-ref missing href")
    text = code.text_content() or ""
    for ref_id in expected_ids:
        qualified = code.locator(f'a.entity-ref[data-entity-id="{ref_id}"]')
        _check(qualified.count() == 1,
               f"entity {ref_id} was split or decorated more than once")
        _check(qualified.first.inner_text() in text,
               f"entity decoration for {ref_id} altered the source spelling")


_GOLDEN_TOKENS = _TESTS_ROOT / "fixtures" / "search_tokens_golden.json"


def _load_golden_cases() -> list[dict[str, str]]:
    data = json.loads(_GOLDEN_TOKENS.read_text(encoding="utf-8"))
    _check_current_schema(
        data,
        "bioseq.website.search_tokens_golden",
        1,
        "search-token golden",
    )
    _check(
        set(data) == {"schema", "schema_revision", "comment", "cases"},
        "search-token golden has an unexpected member set",
    )
    return list(data.get("cases", []))


def _exercise_search_tokens(page) -> None:
    """Drive static/js/search_tokens.mjs against the shared golden corpus (§12.5),
    proving the JS normalizer agrees byte-for-byte with the C++ self-test."""
    cases = _load_golden_cases()
    _check(bool(cases), "shared search-token golden corpus is empty")
    module_url = page.evaluate(
        "() => { const s = document.querySelector('script[type=module]');"
        " return s ? new URL('./search_tokens.mjs', s.src).href : null; }")
    _check(bool(module_url), "cannot locate search_tokens.mjs from the module script")
    produced = page.evaluate(
        """async ({url, inputs}) => {
            const mod = await import(url);
            return inputs.map((value) => mod.normalizeSearchText(value));
        }""",
        {"url": module_url, "inputs": [case["input"] for case in cases]})
    for case, got in zip(cases, produced):
        _check(got == case["expected"],
               f"JS normalizer {case['input']!r} -> {got!r}, expected {case['expected']!r}")


def _exercise_docs_search(page, request_urls: list[str], contract: dict,
                          expected_tutorial_route: str) -> None:
    """Exercise the supplied docs dropdown against current generated search data,
    including keyboard focus and roving selection."""
    inp = page.locator("#doc-search")
    dropdown = page.locator("#search-dd")
    _check(inp.count() == 1 and dropdown.count() == 1,
           "docs design search input/dropdown is missing")
    page.locator("body").press("/")
    _check(inp.evaluate("el => el === document.activeElement"),
           "the docs '/' shortcut did not focus search")
    inp.fill("mmsearch")
    results = dropdown.locator("a[href]")
    try:
        results.first.wait_for(state="visible", timeout=4000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"docs search produced no visible mmsearch results: {error}")
    hrefs = [results.nth(index).get_attribute("href") or ""
             for index in range(results.count())]
    href_paths = [urlsplit(href).path for href in hrefs]
    _check(expected_tutorial_route in href_paths,
           f"docs search did not return second tutorial route {expected_tutorial_route}; "
           f"got {hrefs}")
    _check(dropdown.locator("mark, .doc-search-highlight").count() > 0,
           "docs search does not highlight the matched text")
    requested_paths = {urlsplit(url).path for url in request_urls}
    _check(contract["search_route"] in requested_paths,
           f"docs search never fetched generated index {contract['search_route']}")
    inp.press("ArrowDown")
    selected = dropdown.locator("a[aria-selected='true']")
    active_descendant = inp.get_attribute("aria-activedescendant") or ""
    focused_result = dropdown.locator("a:focus")
    _check(selected.count() == 1 or focused_result.count() == 1
           or (active_descendant and page.locator(f"#{active_descendant}").count() == 1),
           "ArrowDown did not establish a keyboard-visible search selection")
    page.keyboard.press("Escape")
    try:
        dropdown.wait_for(state="hidden", timeout=3000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"Escape did not dismiss docs search: {error}")
    inp.focus()
    inp.fill("bioseq")
    try:
        dropdown.wait_for(state="visible", timeout=3000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"docs search did not reopen for focusout test: {error}")
    page.locator("#ver-btn").focus()
    try:
        dropdown.wait_for(state="hidden", timeout=3000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"docs search remained expanded after focus left: {error}")
    _check(inp.get_attribute("aria-expanded") == "false",
           "docs search combobox remained aria-expanded after focus left")


def _exercise_nav(page) -> None:
    """The literal landing header changes to its legible glass state on scroll."""
    nav = page.locator("header#nav")
    _check(nav.count() == 1, "landing navigation header is missing")

    def wait_for_nav_state(predicate, description: str) -> dict:
        state = {"scrollY": -1, "background": ""}
        for _ in range(120):
            state = page.evaluate(
                """() => ({
                  scrollY: window.scrollY,
                  background: document.getElementById('nav')?.style.background || ''
                })""")
            if predicate(state):
                return state
            page.wait_for_timeout(25)
        raise AssertionFail(f"{description}: final state {state}")

    # The supplied design deliberately enables smooth scrolling.  Override that
    # presentation detail while exercising the nav state machine, then wait on
    # the state itself instead of a timing-sensitive animation interval.
    page.evaluate(
        """() => {
          document.documentElement.style.setProperty('scroll-behavior', 'auto', 'important');
          window.scrollTo(0, 120);
        }""")
    wait_for_nav_state(
        lambda state: "rgba" in state["background"],
        "landing navigation did not reach its scrolled state")
    _check("rgba" in (nav.evaluate("element => element.style.background") or ""),
           "landing navigation did not react to page scroll")
    page.evaluate("() => window.scrollTo(0, 0)")
    wait_for_nav_state(
        lambda state: state["scrollY"] == 0 and state["background"] == "transparent",
        "landing navigation did not reach its restored state")
    _check(nav.evaluate("element => element.style.background") == "transparent",
           "landing navigation did not restore its top-of-page state")
    page.evaluate(
        "() => document.documentElement.style.removeProperty('scroll-behavior')")


def _exercise_version_selector(page, request_urls: list[str]) -> None:
    """Exercise the literal docs version menu backed by generated versions.json."""
    button = page.locator("#ver-btn")
    menu = page.locator("#ver-menu")
    _check(button.count() == 1 and menu.count() == 1,
           "docs version button/menu is missing")
    source = menu.get_attribute("data-versions-url") or ""
    _check(bool(source), "docs version menu has no generated versions data URL")
    links = menu.locator("a[data-ver][href]")
    all_versions = menu.locator("a[data-ver-all][href]")
    try:
        links.first.wait_for(state="attached", timeout=3000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"docs version data produced no menu links: {error}")
    requested_paths = {urlsplit(url).path for url in request_urls}
    _check(urlsplit(source).path in requested_paths,
           f"docs version menu never fetched generated data {source}")
    _check(all_versions.count() == 1
           and urlsplit(all_versions.get_attribute("href") or "").path.endswith("/versions/")
           and all_versions.get_attribute("role") == "menuitem"
           and all_versions.evaluate("element => element === element.parentElement.lastElementChild"),
           "version manifest replacement removed the authored All versions link")
    button.click()
    _check(button.get_attribute("aria-expanded") == "true" and menu.is_visible(),
           "clicking the docs version button did not open its menu")
    button.press("ArrowDown")
    _check(links.first.evaluate("element => element === document.activeElement"),
           "ArrowDown did not focus the first docs version")
    page.keyboard.press("Escape")
    _check(button.get_attribute("aria-expanded") == "false"
           and button.evaluate("element => element === document.activeElement"),
           "Escape did not close the docs version menu and restore focus")


def _exercise_entity(page) -> None:
    """Entity page enrichment (§12.3): kind badge and a copyable signature."""
    _check(page.locator("article.design-entity-page").count() == 1,
           "entity route did not render the docs-design entity component")
    _check(page.locator(".design-kind-badge").count() >= 1,
           "entity page is missing its kind badge")
    signature = page.locator("#entity-signature-code")
    _check(signature.count() == 1, "entity page is missing its generated signature")
    button = page.locator("button.copy-button[data-copy-target='entity-signature-code']")
    _check(button.count() == 1, "entity signature has no copy button")


def _exercise_jsonld(page) -> None:
    """SEO JSON-LD (§15.5): the doc page's inert ld+json is valid, types as
    TechArticle, and carries a BreadcrumbList."""
    block = page.locator("script[type='application/ld+json']").first
    _check(block.count() == 1, "documentation page is missing generated JSON-LD")
    text = block.text_content() or "{}"
    try:
        data = json.loads(text)
    except json.JSONDecodeError as error:
        raise AssertionFail(f"JSON-LD is not valid JSON: {error}")
    nodes = data.get("@graph", [data]) if isinstance(data, dict) else []
    types = {node.get("@type") for node in nodes if isinstance(node, dict)}
    _check("TechArticle" in types, f"JSON-LD primary type is not TechArticle (got {types})")
    _check("BreadcrumbList" in types, "JSON-LD has no BreadcrumbList")


def _table_matrix(table) -> list[list[str]]:
    # Compare the DOM payload, not CSS-transformed innerText.  The supplied
    # landing design uppercases visible headers, while hidden tab panels expose
    # their raw text through innerText; textContent is stable in both states and
    # therefore proves that the rendered values came from the resolved model.
    header = [(cell.text_content() or "").strip()
              for cell in table.locator("thead th").all()]
    rows = []
    for row in table.locator("tbody tr").all():
        rows.append([(cell.text_content() or "").strip()
                     for cell in row.locator("td").all()])
    return [header, *rows]


def _landing_result_matrix(result: dict) -> list[list[str]]:
    header = [f"{column.get('name', '')} ({column.get('type_name', '')})"
              for column in result.get("columns", [])]
    rows = []
    for row in result.get("rows", []):
        rows.append(["NULL" if cell.get("is_null") else str(cell.get("text", ""))
                     for cell in row])
    return [header, *rows]


def _exercise_landing_results(page, contract: dict) -> None:
    wrappers = page.locator("[data-sql-example]")
    _check(wrappers.count() == 3,
           f"landing must render three model-bound SQL slots, got {wrappers.count()}")
    for slot, view in contract["landing"].items():
        wrapper = page.locator(f'[data-sql-example="{slot}"]')
        _check(wrapper.count() == 1, f"landing slot wrapper is missing or duplicated: {slot}")
        live_sql = page.locator(f'[data-sql-example-code="{slot}"] code')
        _check(live_sql.count() == 1,
               f"landing slot {slot} has no model-bound visible SQL source")
        _check((live_sql.text_content() or "").strip() == view["visible_sql"].strip(),
               f"landing slot {slot} rendered SQL differs from the resolved model")
        table = wrapper.locator("table.result-table")
        _check(table.count() == 1, f"landing slot {slot} has no executed result table")
        expected = _landing_result_matrix(view["result"])
        _check(_table_matrix(table) == expected,
               f"landing slot {slot} rendered fake/stale rows: "
               f"{_table_matrix(table)!r} != {expected!r}")
    _check(page.locator("[data-live-sql], .design-sr-only").count() == 0,
           "landing duplicates visible SQL in a screen-reader-only subtree")

    # The prominent annotated hero card is a second view of the same model slot.
    hero = page.locator(
        '[data-live-result="hero_query"] table.annotated-result-table')
    _check(hero.count() == 1, "annotated landing query is not bound to hero_query result")
    hero_expected = _landing_result_matrix(contract["landing"]["hero_query"]["result"])
    _check(_table_matrix(hero) == hero_expected,
           "annotated landing query contains prototype rows instead of backend rows")
    hero_rows = len(contract["landing"]["hero_query"]["result"].get("rows", []))
    hero_label = page.locator(
        '#query-card > [data-live-result="hero_query"] > div')
    _check(hero_label.count() == 1
           and (hero_label.text_content() or "").strip() == f"{hero_rows} rows",
           "annotated landing query row-count label is not model-derived")


def _exercise_tutorial_results(page, tutorial: dict) -> None:
    expected = _result_tables(tutorial.get("content", []))
    tables = page.locator("table.result-table")
    _check(tables.count() == len(expected),
           f"{tutorial['slug']}: rendered {tables.count()} result tables, "
           f"model contains {len(expected)}")
    actual = [_table_matrix(tables.nth(index)) for index in range(tables.count())]
    _check(actual == expected,
           f"{tutorial['slug']}: rendered SQL cells differ from real PostgreSQL model")
    for index, expected_table in enumerate(expected):
        expected_columns = len(expected_table[0]) if expected_table else 0
        type_labels = tables.nth(index).locator("thead th code")
        _check(type_labels.count() == expected_columns
               and all(bool(label.strip()) for label in type_labels.all_text_contents()),
               f"{tutorial['slug']}: result table omits PostgreSQL column type labels")
    body_text = page.locator("main#doc-main").inner_text()
    _check(body_text.count("Executed against Bioseq extension")
           == len(tutorial.get("sql_examples", [])),
           f"{tutorial['slug']}: every runnable result must carry backend provenance")


def _exercise_markdown_callouts(page, slug: str) -> None:
    if slug == "sequence_column_type":
        _check(page.locator(".admonition-note").count() == 1,
               "sequence tutorial Markdown NOTE callout is missing")
        _check(page.locator(".admonition-tip").count() == 1,
               "sequence tutorial Markdown TIP callout is missing")
    else:
        important = page.locator(".admonition-important")
        _check(important.count() == 1,
               "similarity tutorial Markdown IMPORTANT callout is missing")
        _check("BioseqScan" in important.inner_text(),
               "similarity tutorial callout lost its planner assertion explanation")


def _assert_landing_routes(page, base_path: str, version: str) -> None:
    first = _public_route(base_path, f"{version}/tutorials/sequence_column_type/")
    read_links = page.locator("a", has_text="Read the docs")
    _check(read_links.count() == 3,
           f"supplied landing has three Read the docs links, got {read_links.count()}")
    for index in range(read_links.count()):
        _check(read_links.nth(index).get_attribute("href") == first,
               f"landing Read the docs must target first tutorial {first}")
    github = page.locator("a", has_text="GitHub")
    _check(github.count() >= 3, "landing lost its GitHub links")
    for index in range(github.count()):
        _check(github.nth(index).get_attribute("href") == _REPOSITORY_URL,
               "landing GitHub link does not use the configured exact repository URL")


def _assert_tutorial_sidebar(page, base_path: str, version: str) -> None:
    sidebar = page.locator("#sidebar")
    _check(sidebar.count() == 1, "docs page has no supplied-design sidebar")
    chapter_lists = sidebar.locator("[data-chap-list][id]")
    chapter_ids = [chapter_lists.nth(index).get_attribute("id") or ""
                   for index in range(chapter_lists.count())]
    _check(len(chapter_ids) >= 2 and len(chapter_ids) == len(set(chapter_ids)),
           f"sidebar chapter lists must have unique live IDs, got {chapter_ids}")
    toggles = sidebar.locator("[data-chap-toggle][aria-controls]")
    controls = [toggles.nth(index).get_attribute("aria-controls") or ""
                for index in range(toggles.count())]
    _check(controls == chapter_ids,
           f"sidebar toggle controls {controls} do not match chapter IDs {chapter_ids}")
    first_toggle = toggles.first
    before = first_toggle.get_attribute("aria-expanded")
    first_toggle.click()
    _check(first_toggle.get_attribute("aria-expanded") != before,
           "sidebar chapter toggle did not change its expanded state")
    first_toggle.click()
    _check(first_toggle.get_attribute("aria-expanded") == before,
           "sidebar chapter toggle did not restore its expanded state")
    for slug in _EXPECTED_TUTORIALS:
        route = _public_route(base_path, f"{version}/tutorials/{slug}/")
        _check(sidebar.locator(f'a[href="{route}"]').count() == 1,
               f"sidebar must contain one direct link to {route}")
    forbidden = _public_route(base_path, f"{version}/tutorials/")
    _check(sidebar.locator(f'a[href="{forbidden}"]').count() == 0,
           f"sidebar links to forbidden tutorial-list route {forbidden}")


def _assert_literal_design_structure(page, label: str, design: dict) -> None:
    if label == "landing":
        source = design["landing"]
        prefix = "ld-s"
        pairs = (
            ("data-screen-label:BioseqDB landing",
             '[data-screen-label="BioseqDB landing"]'),
            ("id:nav", "#nav"), ("id:annotate", "#annotate"),
            ("id:workflow", "#workflow"), ("id:cta", "#cta"),
        )
    else:
        source = design["docs"]
        prefix = "dd-s"
        pairs = (
            ("data-screen-label:BioseqDB documentation",
             '[data-screen-label="BioseqDB documentation"]'),
            ("id:dnav", "#dnav"), ("data-bodygrid:first", "[data-bodygrid]"),
            ("id:sidebar", "#sidebar"), ("data-search-wrap:first", "[data-search-wrap]"),
            ("data-search-box:first", "[data-search-box]"),
            ("data-rightrail:first", "[data-rightrail]"),
            ("id:doc-search", "#doc-search"), ("id:search-dd", "#search-dd"),
        )
    for source_key, selector in pairs:
        node = page.locator(selector)
        _check(node.count() == 1, f"{label}: required literal design node {selector} missing")
        tag = node.evaluate("element => element.tagName.toLowerCase()")
        _check(tag == source[source_key]["tag"],
               f"{label}: {selector} tag {tag} differs from supplied design "
               f"{source[source_key]['tag']}")
        classes = node.get_attribute("class") or ""
        _check(any(name.startswith(prefix) for name in classes.split()),
               f"{label}: {selector} is not backed by translated design declarations")


def _compare_design_style(page, actual_selector: str, reference: dict,
                          properties: tuple[str, ...], description: str) -> None:
    values = page.evaluate(
        """({selector, tag, style, properties}) => {
          const actual = document.querySelector(selector);
          if (!actual) return null;
          const probe = document.createElement(tag);
          probe.style.cssText = style;
          probe.style.position = probe.style.position || 'fixed';
          probe.style.left = '-10000px';
          probe.style.visibility = 'hidden';
          document.body.appendChild(probe);
          const a = getComputedStyle(actual);
          const r = getComputedStyle(probe);
          const out = {};
          for (const property of properties) out[property] = [a[property], r[property]];
          probe.remove();
          return out;
        }""",
        {"selector": actual_selector, "tag": reference["tag"],
         "style": reference["style"], "properties": list(properties)},
    )
    _check(values is not None, f"{description}: actual selector is missing")
    mismatches = {key: pair for key, pair in values.items() if pair[0] != pair[1]}
    _check(not mismatches, f"{description}: supplied design style mismatch {mismatches}")


def _exercise_design_style_parity(page, label: str, design: dict) -> None:
    if label == "landing":
        records = design["landing"]
        _compare_design_style(
            page, "#nav", records["id:nav"],
            ("position", "top", "zIndex", "backgroundColor", "borderBottomWidth",
             "borderBottomColor", "backdropFilter"),
            "landing navigation")
        _compare_design_style(
            page, "#annotate", records["id:annotate"],
            ("display", "gap", "alignItems", "position"), "landing query anatomy")
        hero = '#query-card > [data-live-result="hero_query"]'
        _compare_design_style(
            page, hero, records["hero-result:first"],
            ("borderTopWidth", "borderTopColor", "borderRightWidth",
             "borderBottomWidth", "borderLeftWidth", "borderRadius",
             "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
             "backgroundColor"),
            "landing live hero result")
        _compare_design_style(
            page, f"{hero} > div", records["hero-result-label:first"],
            ("fontFamily", "fontSize", "letterSpacing", "textTransform",
             "color", "marginBottom"),
            "landing live hero result label")
        _compare_design_style(
            page, f"{hero} > table", records["hero-result-table:first"],
            ("borderCollapse", "fontFamily", "fontSize"),
            "landing live hero result table")
        _compare_design_style(
            page, f"{hero} thead th:first-child",
            records["hero-result-header:first"],
            ("textAlign", "fontWeight", "color", "fontSize", "letterSpacing",
             "textTransform", "paddingBottom"),
            "landing live hero result header")
        _compare_design_style(
            page, f"{hero} tbody td:first-child",
            records["hero-result-cell:first"],
            ("color", "paddingTop", "paddingBottom", "borderTopWidth",
             "borderTopColor"),
            "landing live hero result cell")
        _compare_design_style(
            page, f"{hero} tbody td:nth-child(2)",
            records["hero-result-accent-cell:first"],
            ("color", "paddingTop", "paddingBottom", "borderTopWidth",
             "borderTopColor"),
            "landing live hero result accent cell")
        return

    records = design["docs"]
    _compare_design_style(
        page, "#dnav", records["id:dnav"],
        ("position", "top", "zIndex", "backgroundColor", "borderBottomWidth",
         "borderBottomColor", "backdropFilter"), "docs navigation")
    _compare_design_style(
        page, "[data-bodygrid]", records["data-bodygrid:first"],
        ("display", "maxWidth", "alignItems"), "docs body grid")
    _compare_design_style(
        page, "#sidebar", records["id:sidebar"],
        ("position", "top", "height", "overflowY", "paddingTop", "paddingRight",
         "paddingBottom", "paddingLeft", "borderRightWidth", "borderRightColor"),
        "docs sidebar")
    _compare_design_style(
        page, "[data-search-box]", records["data-search-box:first"],
        ("display", "alignItems", "gap", "height", "borderRadius",
         "backgroundColor"), "docs search box")
    if page.locator(".doc-code:not(:has(.anno-cards))").count() > 0:
        _compare_design_style(
            page, ".doc-code:not(:has(.anno-cards))", records["data-term:first"],
            ("maxWidth", "borderTopWidth", "borderTopColor", "borderRadius",
             "backgroundImage", "boxShadow", "overflow"), "Markdown code card")
        _compare_design_style(
            page, ".doc-code:not(:has(.anno-cards)) pre", records["data-code:first"],
            ("marginTop", "marginRight", "marginBottom", "marginLeft", "paddingTop",
             "paddingRight", "paddingBottom", "paddingLeft", "overflowX"),
            "Markdown code geometry")
        _compare_design_style(
            page, ".doc-code:not(:has(.anno-cards)) code", records["data-code:first"],
            ("fontFamily", "fontSize", "lineHeight", "color"),
            "Markdown code typography")


def _exercise_prism_pixel_contract(page, design: dict, artifact_dir: Path) -> None:
    """Pixel-compare the live Prism component with the same DOM restyled to the
    supplied terminal geometry and explicit design token palette."""
    page.set_viewport_size({"width": 1280, "height": 900})
    page.evaluate("() => document.fonts.ready")
    pre = page.locator(".doc-code:not(:has(.anno-cards)) pre").first
    _check(pre.count() == 1, "Prism pixel contract has no unannotated SQL component")
    pre.scroll_into_view_if_needed()
    code = pre.locator("code.language-sql")
    _check(code.count() == 1, "Prism pixel contract requires a highlighted SQL fence")
    for token_class in ("keyword", "function", "string", "operator", "punctuation"):
        _check(code.locator(f".token.{token_class}").count() > 0,
               f"Prism SQL sample has no {token_class} token to validate")

    artifact_dir.mkdir(parents=True, exist_ok=True)
    actual_path = artifact_dir / "prism-live.png"
    reference_path = artifact_dir / "prism-design-reference.png"
    actual = pre.screenshot(path=str(actual_path), animations="disabled")

    docs = design["docs"]
    page.evaluate(
        """({termStyle, preStyle, palette}) => {
          const pre = document.querySelector('.doc-code:not(:has(.anno-cards)) pre');
          const figure = pre.closest('.doc-code');
          const code = pre.querySelector('code.language-sql');
          figure.style.cssText = termStyle;
          pre.style.cssText = preStyle;
          code.style.fontFamily = "'IBM Plex Mono'";
          code.style.fontSize = '14px';
          code.style.lineHeight = '1.85';
          code.style.color = palette.operator;
          code.style.background = 'transparent';
          code.style.textShadow = 'none';
          const paint = (selector, colour, weight = '') => {
            code.querySelectorAll(selector).forEach((node) => {
              node.style.color = colour;
              if (weight) node.style.fontWeight = weight;
            });
          };
          paint('.token.comment,.token.prolog,.token.doctype,.token.cdata', palette.comment);
          paint('.token.keyword,.token.atrule,.token.selector', palette.keyword);
          paint('.token.function,.token.function-name,.token.predicate', palette.function, '600');
          paint('.token.class-name,.token.type,.token.namespace', palette.type);
          paint('.token.builtin', palette.builtin);
          paint('.token.string,.token.char,.token.regex', palette.string);
          paint('.token.macro,.token.directive,.token.property', palette.macro);
          paint('.token.number,.token.boolean', palette.number);
          paint('.token.operator,.token.punctuation', palette.operator);
        }""",
        {"termStyle": docs["data-term:first"]["style"],
         "preStyle": docs["data-code:first"]["style"],
         "palette": _DESIGN_PALETTE},
    )
    reference = pre.screenshot(path=str(reference_path), animations="disabled")
    _check(actual == reference,
           "Prism adaptation is not pixel-identical to the supplied design geometry/palette; "
           f"live={hashlib.sha256(actual).hexdigest()}, "
           f"reference={hashlib.sha256(reference).hexdigest()} (artifacts: {artifact_dir})")


def _exercise_doc_frame(page, *, expect_toc: bool) -> None:
    """Three-region doc frame (§15.1): a visual breadcrumb trail, a left
    documentation-navigation rail, and — on article pages — an on-this-page
    table of contents whose links target in-page anchors that exist."""
    crumbs = page.locator("nav.breadcrumb ol li")
    canonical_crumbs = page.locator("[data-breadcrumb] > *")
    design_crumbs = page.locator("nav.design-breadcrumb a[href]")
    _check(crumbs.count() >= 2 or canonical_crumbs.count() >= 3
           or design_crumbs.count() >= 2,
           "doc page has no breadcrumb trail")
    canonical_nav = page.locator("aside#sidebar")
    nav = canonical_nav if canonical_nav.count() else page.locator("aside.doc-nav")
    _check(nav.count() == 1, "doc page is missing its documentation-navigation rail")
    _check(nav.locator("a[href]").count() >= 1, "doc-nav rail has no links")
    canonical_toc = page.locator("aside[data-rightrail]")
    toc = canonical_toc if canonical_toc.count() else page.locator("aside.doc-toc")
    if expect_toc:
        _check(toc.count() == 1, "doc article is missing the supplied right rail")
        anchors = toc.locator("a[href^='#']")
        count = anchors.count()
        _check(count >= 1, "on-this-page TOC is present but empty")
        for index in range(count):
            target = (anchors.nth(index).get_attribute("href") or "")[1:]
            if not target:
                continue
            found = page.evaluate("(id) => document.getElementById(id) !== null", target)
            _check(found, f"on-this-page anchor #{target} has no matching element")
        first_target = (anchors.first.get_attribute("href") or "")[1:]
        anchors.first.click()
        page.wait_for_timeout(150)
        _check(page.evaluate("() => location.hash") == f"#{first_target}",
               f"TOC interaction did not update the live hash to #{first_target}")


def _exercise_capability_tracer(page) -> None:
    """Exercise the supplied architecture cards, diagram trace, and roving keys."""
    tracer = page.locator("#capabilities")
    _check(tracer.count() == 1, "landing capability section is missing")
    options = tracer.locator("[data-cap-select][data-cap]")
    total = options.count()
    _check(total == 8, f"capability tracer requires 8 cards, got {total}")
    _check(tracer.locator("svg[data-architecture-click]").count() == 1,
           "capability tracer is missing the reference architecture diagram")
    _check(all(options.nth(index).get_attribute("role") == "button"
               for index in range(total)),
           "capability cards are not exposed as keyboard buttons")
    _check(tracer.locator("[data-cap-select][aria-pressed='true']").count() == 1,
           "capability tracer must start with exactly one selection")
    second = options.nth(1)
    capability = second.get_attribute("data-cap") or ""
    trace = {"mmsearch": "sql", "seqmeta": "meta", "load": "pg", "index": "index",
             "columnar": "shard", "simd": "cpu", "vulkan": "gpu",
             "parallel": "engine"}[capability]
    second.click()
    _check(second.get_attribute("aria-pressed") == "true",
           "clicked capability did not become pressed")
    _check(tracer.locator("[data-cap-select][aria-pressed='true']").count() == 1,
           "selecting a capability left more than one pressed")
    node = tracer.locator(f'[data-diag="{trace}"]')
    _check(node.count() == 1 and "dgGrad" in node.evaluate(
        "element => [...element.querySelectorAll('.f')].map(node => node.style.fill).join(' ')") ,
           f"capability trace '{trace}' did not highlight its architecture node")
    second.focus()
    page.keyboard.press("ArrowRight")
    third = options.nth(2)
    _check(third.get_attribute("aria-pressed") == "true"
           and third.evaluate("element => element === document.activeElement")
           and tracer.locator("[data-cap-select][aria-pressed='true']").count() == 1,
           "ArrowRight did not move the single capability selection and focus")
    tracer.locator('[data-architecture-click] [data-click="simd"]').first.click(
        force=True)
    _check(tracer.locator('[data-cap="simd"][aria-pressed="true"]').count() == 1,
           "clicking the architecture diagram did not select its capability card")


def _exercise_connectors(page) -> None:
    """The live query anatomy must draw all three supplied SVG connectors."""
    grid = page.locator("#annotate")
    _check(grid.count() == 1, "landing query anatomy is missing")
    svg = grid.locator("#annot-svg")
    _check(svg.count() == 1, "hero query is missing its connector layer")
    _check(svg.first.get_attribute("aria-hidden") == "true",
           "connector layer must be aria-hidden")
    grid.scroll_into_view_if_needed()
    page.wait_for_timeout(250)
    _check(not svg.evaluate("element => element.hidden"),
           "connector layer stayed hidden at desktop width")
    for selector in ("path.cx-rect", "path.cx-lead", "circle.cx-dot"):
        _check(svg.locator(selector).count() == 3,
               f"connector layer expected 3 {selector} elements")


def _exercise_workflow(page) -> None:
    """Workflow progress (§15.2): a progress fill plus per-step markers."""
    workflow = page.locator("#workflow")
    _check(workflow.count() == 1, "landing workflow section is missing")
    fill = workflow.locator("#wf-progress-fill")
    _check(fill.count() == 1,
           "workflow has no progress fill")
    cards = workflow.locator("[data-wf-card]")
    _check(cards.count() == 5, f"workflow requires 5 steps, got {cards.count()}")
    page.evaluate(
        """() => {
          document.documentElement.style.setProperty('scroll-behavior', 'auto', 'important');
          document.getElementById('wf-grid').scrollIntoView({block: 'center'});
        }""")
    state = {"width": 0, "active": 0}
    for _ in range(120):
        state = page.evaluate(
            """() => {
              const fill = document.getElementById('wf-progress-fill');
              const cards = [...document.querySelectorAll('#workflow [data-wf-card]')];
              return {
                width: Number.parseFloat(fill?.style.width || '0'),
                active: cards.filter(
                  card => card.style.boxShadow && card.style.boxShadow !== 'none').length
              };
            }""")
        if 0 < state["width"] <= 100 and state["active"] == 1:
            break
        page.wait_for_timeout(25)
    width = state["width"]
    _check(0 < width <= 100, f"workflow progress did not respond to scroll: {width}")
    active = state["active"]
    _check(active == 1, f"workflow must visually select one active step, got {active}")
    page.evaluate(
        "() => document.documentElement.style.removeProperty('scroll-behavior')")


def _exercise_media_band(page) -> None:
    """Media band (§15.2): a muted self-hosted decorative background loop with
    no native player chrome and the reference mp4 source."""
    videos = page.locator("video[data-autobg]")
    _check(videos.count() == 2,
           f"landing requires the two supplied background videos, got {videos.count()}")
    sources = []
    for index in range(videos.count()):
        video = videos.nth(index)
        _check(video.get_attribute("controls") is None,
               "decorative background video exposes native player controls")
        _check(video.get_attribute("autoplay") is not None
               and video.get_attribute("muted") is not None
               and video.get_attribute("loop") is not None,
               "decorative background video lost autoplay/muted/loop semantics")
        source = video.locator("source[type='video/mp4']")
        _check(source.count() == 1, "background video has no single MP4 source")
        sources.append(source.get_attribute("src") or "")
    _check({urlsplit(source).path.rsplit("/", 1)[-1] for source in sources}
           == {"vials.mp4", "small_vid.mp4"},
           f"landing videos are not the supplied assets: {sources}")


def _exercise_cta_grid(page) -> None:
    """Final CTA reproduces the moving reference DNA tile field."""
    grid = page.locator("#cta-grid")
    _check(grid.count() == 1, "landing CTA DNA tile field is missing")
    page.locator("#cta").scroll_into_view_if_needed()
    page.wait_for_timeout(150)
    _check(grid.locator(".cta-grid-tile").count() >= 12,
           "CTA DNA tile field was not constructed")


def _exercise_reduced_motion(page) -> None:
    """Reduced-motion branch (§15.3): under prefers-reduced-motion the reveal
    effects are disabled (content shown immediately, never armed-hidden) and the
    decorative video does not autoplay. Reloads to re-run the JS under emulation."""
    _check(page.locator('[data-screen-label="BioseqDB landing"]').count() == 1,
           "reduced-motion check is not running on the landing page")
    page.emulate_media(reduced_motion="reduce")
    page.reload(wait_until="networkidle")
    reveal = page.locator("[data-reveal]")
    total = reveal.count()
    _check(total > 0 and page.locator("[data-reveal].rv-in").count() == total,
           "reduced motion did not reveal every landing section immediately")
    page.wait_for_timeout(150)
    paused = page.locator("video[data-autobg]").evaluate_all(
        "videos => videos.length === 2 && videos.every(video => video.paused)")
    _check(paused, "decorative background video played under reduced motion")
    page.emulate_media(reduced_motion="no-preference")
    page.reload(wait_until="networkidle")


def _exercise_responsive(page, label: str) -> None:
    """Responsive layout (§15.3/§20): no horizontal overflow and the key landmarks
    are present at each required visual width."""
    for width in (360, 768, 1024, 1440):
        page.set_viewport_size({"width": width, "height": 900})
        page.wait_for_timeout(120)
        if width == 360 and label != "landing":
            menu = page.locator("[data-menu]")
            backdrop = page.locator("[data-backdrop]")
            _check(menu.count() == 1 and menu.is_visible()
                   and menu.get_attribute("aria-expanded") == "false",
                   f"{label}: docs drawer button is not ready at mobile width")
            menu.click()
            _check(menu.get_attribute("aria-expanded") == "true"
                   and backdrop.is_visible(),
                   f"{label}: docs drawer did not open")
            page.keyboard.press("Escape")
            _check(menu.get_attribute("aria-expanded") == "false"
                   and not backdrop.is_visible(),
                   f"{label}: Escape did not close the docs drawer")
        overflow = page.evaluate(
            "() => document.documentElement.scrollWidth - document.documentElement.clientWidth")
        _check(overflow <= 1, f"{label}: {overflow}px horizontal overflow at width {width}")
    _check(page.locator("header.site-header, header#nav, header#dnav").count() == 1,
           f"{label}: missing header landmark")
    _check(page.locator("main#main, main#doc-main").count() == 1,
           f"{label}: missing main landmark")
    _check(page.locator(
        "footer.site-footer, main#main footer, "
        "footer[data-screen-label='Docs · Footer']").count() == 1,
           f"{label}: missing footer landmark")
    page.set_viewport_size({"width": 1280, "height": 900})


def _run_one(api, server: StaticFixtureServer, routes: dict[str, str],
             verbose: bool, contract: dict, artifact_dir: Path,
             base_path: str, version: str) -> None:
    expected_headers = server.extra_headers
    _check(bool(expected_headers),
           "security-headers.txt is missing or empty at the site root")

    with api.sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        context = browser.new_context()

        # Enforce offline: abort any non-loopback request.
        def _guard(route):
            host = urlsplit(route.request.url).hostname or ""
            if host in _LOOPBACK_HOSTS or host == server._host:
                route.continue_()
            else:
                route.abort()

        context.route("**/*", _guard)
        page = context.new_page()
        request_urls: list[str] = []
        page.on("requestfinished", lambda request: request_urls.append(request.url))

        console_errors: list[str] = []
        page.on("console", lambda msg: console_errors.append(f"{msg.type}: {msg.text}")
                if msg.type == "error" else None)
        page.on("pageerror", lambda exc: console_errors.append(f"pageerror: {exc}"))

        for label, route in routes.items():
            url = server.url_for(route)
            if verbose:
                print(f"  navigating {label}: {url}")
            response = page.goto(url, wait_until="networkidle")
            _check(response is not None, f"{label}: no response from {url}")
            _check(response.ok, f"{label}: HTTP {response.status} for {url}")
            _assert_headers(response, expected_headers)

            meta = page.locator('meta[http-equiv="Content-Security-Policy"]')
            _check(meta.count() == 1, f"{label}: expected exactly one meta CSP")

            h1 = page.locator("h1")
            _check(h1.count() == 1, f"{label}: expected exactly one <h1>, got {h1.count()}")

            if label == "landing":
                _assert_literal_design_structure(page, label, contract["design"])
                _exercise_design_style_parity(page, label, contract["design"])
                _assert_landing_routes(page, base_path, version)
                _exercise_landing_results(page, contract)
                _exercise_tabs(page)
                _exercise_nav(page)
                _exercise_search_tokens(page)
                _exercise_capability_tracer(page)
                _exercise_connectors(page)
                _exercise_workflow(page)
                _exercise_media_band(page)
                _exercise_cta_grid(page)
                _exercise_reduced_motion(page)
            if label == "tutorial_sequence":
                _assert_literal_design_structure(page, label, contract["design"])
                _exercise_design_style_parity(page, label, contract["design"])
                _assert_tutorial_sidebar(page, base_path, version)
                _exercise_tooltip(page, request_urls, contract)
                _exercise_copy(page)
                _exercise_annotations(page)
                _exercise_code_entities(
                    page, {"sql:function:bioseq.len(bioseq.seq)"})
                _exercise_markdown_callouts(page, "sequence_column_type")
                _exercise_tutorial_results(page, contract["tutorials"]["sequence_column_type"])
                _exercise_docs_search(
                    page, request_urls, contract,
                    _public_route(base_path, f"{version}/tutorials/similarity_search/"))
                _exercise_version_selector(page, request_urls)
                _exercise_jsonld(page)
                _exercise_doc_frame(page, expect_toc=True)
            if label == "tutorial_similarity":
                _assert_literal_design_structure(page, label, contract["design"])
                _exercise_design_style_parity(page, label, contract["design"])
                _assert_tutorial_sidebar(page, base_path, version)
                _exercise_copy(page)
                _exercise_code_entities(page, {
                    "sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)",
                    "sql:function:bioseq.query_start(anyelement)",
                    "sql:function:bioseq.query_end(anyelement)",
                    "sql:function:bioseq.target_start(anyelement)",
                    "sql:function:bioseq.target_end(anyelement)",
                    "sql:function:bioseq.cigar(anyelement)",
                })
                _exercise_markdown_callouts(page, "similarity_search")
                _exercise_tutorial_results(page, contract["tutorials"]["similarity_search"])
                _exercise_jsonld(page)
                _exercise_doc_frame(page, expect_toc=True)
            if label == "tutorial_cache":
                _assert_literal_design_structure(page, label, contract["design"])
                _exercise_design_style_parity(page, label, contract["design"])
                _assert_tutorial_sidebar(page, base_path, version)
                _exercise_copy(page)
                _exercise_code_entities(page, {
                    "sql:function:bioseq_cache.stats()",
                })
                _exercise_tutorial_results(page, contract["tutorials"]["query_cache"])
                _exercise_jsonld(page)
                _exercise_doc_frame(page, expect_toc=True)
            if label == "tutorial_highway":
                _assert_literal_design_structure(page, label, contract["design"])
                _exercise_design_style_parity(page, label, contract["design"])
                _assert_tutorial_sidebar(page, base_path, version)
                _exercise_copy(page)
                _exercise_tutorial_results(
                    page, contract["tutorials"]["highway_runtime_dispatch"])
                body = page.locator("main#doc-main").inner_text()
                for highway_label in (
                    "Highway Compiled Targets", "CPU-Supported Targets",
                    "Runnable Targets", "Planned Architecture",
                    "Executed Architecture",
                ):
                    _check(highway_label in body,
                           f"Highway page is missing {highway_label}")
                _exercise_jsonld(page)
                _exercise_doc_frame(page, expect_toc=True)
            if label == "reference":
                _assert_literal_design_structure(page, label, contract["design"])
                _exercise_design_style_parity(page, label, contract["design"])
            if label == "entity":
                _assert_literal_design_structure(page, label, contract["design"])
                _exercise_design_style_parity(page, label, contract["design"])
                _exercise_entity(page)
                _exercise_jsonld(page)
                _exercise_doc_frame(page, expect_toc=False)
            _exercise_responsive(page, label)
            if label == "tutorial_similarity":
                _exercise_prism_pixel_contract(page, contract["design"], artifact_dir)

        if console_errors:
            raise AssertionFail("console/page errors: " + "; ".join(console_errors))

        context.close()
        browser.close()


def _manifest_sha(site: Path) -> str | None:
    manifest = site / "site-manifest.json"
    if manifest.is_file():
        return hashlib.sha256(manifest.read_bytes()).hexdigest()
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Playwright acceptance test for one rendered site at one base path.")
    parser.add_argument("--site", required=True, type=Path,
                        help="staging/fixture site root to serve and test")
    parser.add_argument("--base-path", default="/",
                        help="mount / URL base path this tree was rendered for "
                             "(e.g. / or /bioseqdb/)")
    parser.add_argument("--version", default="v0.1",
                        help="version path segment used to derive default routes")
    parser.add_argument("--model", required=True, type=Path,
                        help="resolved site-model.json used for model-to-DOM checks")
    parser.add_argument("--design-source", required=True, type=Path,
                        help="supplied website_design directory used as parity reference")
    parser.add_argument("--artifact-dir", type=Path,
                        help="build/test-only directory for screenshot comparison artifacts")
    parser.add_argument("--stamp", type=Path,
                        help="success stamp written only when all assertions pass")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if not args.site.is_dir():
        print(f"test_browser: site directory missing: {args.site}", file=sys.stderr)
        return EXIT_USAGE

    # Validate the invocation and its inputs BEFORE probing the browser so a
    # usage error (2) is always distinct from browser-absence SKIP (4).
    try:
        server = StaticFixtureServer(args.site, args.base_path)
    except (NotADirectoryError, ValueError) as error:
        print(f"test_browser: {error}", file=sys.stderr)
        return EXIT_USAGE

    if not parse_security_headers(args.site):
        print(f"test_browser: no security-headers.txt at {args.site}; the header "
              "contract cannot be verified", file=sys.stderr)
        return EXIT_USAGE

    try:
        contract = _load_static_contract(
            args.site, args.base_path, args.version, args.model, args.design_source)
    except AssertionFail as error:
        print(f"test_browser: FAIL [base={args.base_path}] {error}", file=sys.stderr)
        return EXIT_FAILED
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"test_browser: FAIL [base={args.base_path}] invalid acceptance input: {error}",
              file=sys.stderr)
        return EXIT_FAILED

    api = _load_playwright()
    if api is None:
        print("test_browser: SKIP - the 'playwright' package is not installed; "
              "install website/tests/requirements.lock into <build>/website/venv. "
              "(exit 4)", file=sys.stderr)
        return EXIT_SKIP

    routes = _default_routes(args.site, server.mount, args.version)
    artifact_dir = args.artifact_dir or (args.site.parent / "browser-artifacts")
    server.start()
    try:
        _run_one(api, server, routes, args.verbose, contract, artifact_dir,
                 args.base_path, args.version)
    except AssertionFail as error:
        print(f"test_browser: FAIL [base={args.base_path}] {error}", file=sys.stderr)
        return EXIT_FAILED
    except Exception as error:  # noqa: BLE001
        # A Chromium/browser launch failure surfaces here (e.g. executable not
        # found because `playwright install` never ran). Treat it as SKIP, not a
        # site failure, so "browser absent" stays distinct from "tests failed".
        name = type(error).__name__
        text = str(error)
        launch_markers = (
            "Executable doesn't exist", "playwright install",
            "Failed to launch", "BrowserType.launch", "Host system is missing",
            "download", "chromium",
        )
        if name == "Error" and any(m.lower() in text.lower() for m in launch_markers):
            print(f"test_browser: SKIP - pinned Chromium is not available "
                  f"({text.splitlines()[0] if text else name}); run "
                  "`playwright install chromium` into <build>/website/playwright-browsers. "
                  "(exit 4)", file=sys.stderr)
            return EXIT_SKIP
        print(f"test_browser: ERROR [base={args.base_path}] {name}: {text}",
              file=sys.stderr)
        return EXIT_FAILED
    finally:
        server.stop()

    if args.stamp is not None:
        args.stamp.parent.mkdir(parents=True, exist_ok=True)
        args.stamp.write_text(
            json.dumps(
                {
                    "validator": "playwright-chromium",
                    "base_path": args.base_path,
                    "routes": list(routes.values()),
                    "manifest_sha256": _manifest_sha(args.site),
                    "model_sha256": hashlib.sha256(args.model.read_bytes()).hexdigest(),
                    "design_sha256": {
                        name: hashlib.sha256((args.design_source / name).read_bytes()).hexdigest()
                        for name in ("index.html", "docs.html")
                    },
                },
                indent=2,
            ) + "\n",
            encoding="utf-8",
        )
    print(f"test_browser: PASS [base={args.base_path}] {len(routes)} pages, "
          "design parity + dynamic data + pixel contract verified")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
