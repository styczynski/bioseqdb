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
  * loads the landing, benchmark explorer, tutorials, and reference pages at the
    CONFIGURED mount (e.g. /bioseqdb/v0.1/...);
  * serves the benchmark explorer's build-copied JSON artifacts over loopback
    HTTP, then exercises loading, rendered, reload, responsive, tooltip,
    filtering/navigation, and failure states;
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
import shlex
import subprocess
import sys
from datetime import datetime
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import quote, unquote, urljoin, urlsplit

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
    "quick_read_search_write",
    "sequence_column_type",
    "columnar_storage_and_vectorized_aggregates",
    "sequence_generation",
    "fasta_fastq_gzip_and_io",
    "index_lifecycle_and_metadata",
    "similarity_search",
    "slice_concat_reverse",
    "multiple_sequence_columns_and_joins",
    "highway_runtime_dispatch",
    "reading_bioseq_explain",
    "query_cache",
    "remote_provider_to_search",
)
_EXPECTED_RUNNABLE_COUNTS = {
    "quick_read_search_write": 1,
    "sequence_column_type": 2,
    "columnar_storage_and_vectorized_aggregates": 1,
    "sequence_generation": 1,
    "fasta_fastq_gzip_and_io": 1,
    "index_lifecycle_and_metadata": 1,
    "slice_concat_reverse": 1,
    "similarity_search": 1,
    "multiple_sequence_columns_and_joins": 1,
    "remote_provider_to_search": 1,
    "query_cache": 3,
    "highway_runtime_dispatch": 2,
    "reading_bioseq_explain": 1,
}
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
_BENCHMARK_REMOTE_HOSTS = {"raw.githubusercontent.com"}


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
            "data-bgrid", "data-bnav", "data-ctrls", "data-footcols",
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
    files = {
        "landing": design_source / "index.html",
        "docs": design_source / "docs.html",
        "benchmarks": design_source / "benchmarks.html",
    }
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
        "benchmarks": (
            "data-screen-label:BioseqDB benchmarks", "id:bhead",
            "data-bgrid:first", "data-bnav:first", "data-ctrls:first",
            "data-footcols:first",
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


def _parse_authored_runnable_fences(path: Path) -> list[str]:
    """Parse runnable fences and enforce the authored phase/cleanup contract."""
    lines = path.read_text(encoding="utf-8").splitlines()
    runnable_ids: list[str] = []
    line_index = 0
    while line_index < len(lines):
        opening = lines[line_index]
        if not opening.startswith("```sql runnable"):
            line_index += 1
            continue
        try:
            tokens = shlex.split(opening[3:])
        except ValueError as error:
            raise AssertionFail(f"{path}:{line_index + 1}: invalid fence: {error}") from error
        _check(tokens[:2] == ["sql", "runnable"],
               f"{path}:{line_index + 1}: runnable fence has invalid grammar")
        identifiers = [token[3:] for token in tokens[2:] if token.startswith("id=")]
        _check(len(identifiers) == 1 and bool(identifiers[0]),
               f"{path}:{line_index + 1}: runnable fence needs one nonempty id")

        body_start = line_index + 1
        line_index = body_start
        while line_index < len(lines) and lines[line_index] != "```":
            line_index += 1
        _check(line_index < len(lines),
               f"{path}:{body_start}: runnable fence is not closed")
        body = lines[body_start:line_index]
        markers = ("-- bioseq:setup", "-- bioseq:visible", "-- bioseq:teardown")
        positions = [
            [index for index, line in enumerate(body)
             if line.rstrip(" \t\r") == marker]
            for marker in markers
        ]
        _check(all(len(found) == 1 for found in positions)
               and positions[0][0] < positions[1][0] < positions[2][0],
               f"{path}:{body_start}: runnable phases must occur exactly once in order")
        setup = "\n".join(body[positions[0][0] + 1:positions[1][0]]).strip()
        visible = "\n".join(body[positions[1][0] + 1:positions[2][0]]).strip()
        teardown = "\n".join(body[positions[2][0] + 1:]).strip()
        _check(bool(setup) and bool(visible) and bool(teardown),
               f"{path}:{body_start}: setup, visible, and teardown must be nonempty")
        _check(re.search(r"\b(SELECT|SHOW|EXPLAIN|INSERT|UPDATE|DELETE)\b", visible,
                         re.IGNORECASE) is not None,
               f"{path}:{body_start}: visible phase has no user-facing SQL operation")

        created_tables = re.findall(
            r"\bCREATE\s+(?:TEMP(?:ORARY)?\s+)?TABLE\s+([a-z_][a-z0-9_]*)",
            setup, re.IGNORECASE)
        for table in created_tables:
            _check(re.search(rf"\bDROP\s+TABLE(?:\s+IF\s+EXISTS)?\s+{re.escape(table)}\b",
                             teardown, re.IGNORECASE) is not None,
                   f"{path}:{body_start}: teardown does not drop setup table {table}")
        prepared = re.findall(
            r"\bPREPARE\s+([a-z_][a-z0-9_]*)", setup, re.IGNORECASE)
        for statement in prepared:
            _check(re.search(rf"\bDEALLOCATE\s+{re.escape(statement)}\b",
                             teardown, re.IGNORECASE) is not None,
                   f"{path}:{body_start}: teardown does not deallocate {statement}")
        settings_sql = f"{setup};\n{visible}"
        settings = sorted(set(re.findall(
            r"(?im)(?:\A|;)(?:(?:[ \t]*--[^\n]*\n)|\s)*"
            r"SET\s+(?:LOCAL\s+)?([a-z_][a-z0-9_.]*)\s*(?:=|\bTO\b)",
            settings_sql,
        )))
        for setting in settings:
            _check(re.search(rf"(?im)^\s*RESET\s+{re.escape(setting)}\s*;", teardown)
                   is not None,
                   f"{path}:{body_start}: teardown does not reset {setting}")
        runnable_ids.append(identifiers[0])
        line_index += 1
    return runnable_ids


def _runnable_output_groups(nodes: list[dict]) -> dict[str, list[dict]]:
    """Associate each runnable code node with its injected result subtree."""
    groups: dict[str, list[dict]] = {}
    for index, node in enumerate(nodes):
        info = node.get("attributes", {}).get("info", "")
        if node.get("kind") == "code_block" and " runnable " in f" {info} ":
            tokens = shlex.split(info)
            identifiers = [token[3:] for token in tokens if token.startswith("id=")]
            _check(len(identifiers) == 1,
                   f"resolved runnable block has invalid info string: {info}")
            result_nodes: list[dict] = []
            for following in nodes[index + 1:]:
                if following.get("kind") in {"code_block", "heading"}:
                    break
                result_nodes.append(following)
                if "Executed against Bioseq extension" in _node_text(following):
                    break
            _check(identifiers[0] not in groups,
                   f"resolved model repeats runnable id {identifiers[0]}")
            groups[identifiers[0]] = result_nodes
        nested = _runnable_output_groups(node.get("children", []))
        for identifier, result_nodes in nested.items():
            _check(identifier not in groups,
                   f"resolved model repeats nested runnable id {identifier}")
            groups[identifier] = result_nodes
    return groups


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
    columnar_path = (
        _WEBSITE_ROOT / version / "tutorials"
        / "columnar_storage_and_vectorized_aggregates.md"
    )
    generation_path = (
        _WEBSITE_ROOT / version / "tutorials" / "sequence_generation.md"
    )
    io_path = (
        _WEBSITE_ROOT / version / "tutorials" / "fasta_fastq_gzip_and_io.md"
    )
    index_path = (
        _WEBSITE_ROOT / version / "tutorials" / "index_lifecycle_and_metadata.md"
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
        columnar_path, generation_path, io_path, index_path,
        landing_path, common_path,
    ):
        _check(path.is_file(), f"acceptance source is missing: {path}")

    all_runnable_ids: list[str] = []
    for slug in _EXPECTED_TUTORIALS:
        path = _WEBSITE_ROOT / version / "tutorials" / f"{slug}.md"
        _check(path.is_file(), f"tutorial source is missing: {path}")
        runnable_ids = _parse_authored_runnable_fences(path)
        _check(len(runnable_ids) == _EXPECTED_RUNNABLE_COUNTS[slug],
               f"{slug} has {len(runnable_ids)} runnable examples, expected "
               f"{_EXPECTED_RUNNABLE_COUNTS[slug]}")
        source = path.read_text(encoding="utf-8")
        links = re.findall(r"\[\[(sql:[^\]|]+)\]\]", source)
        _check(bool(links), f"{slug} has no SQL reference links")
        _check("[[sql:" not in re.sub(r"\[\[sql:[^\]|]+\]\]", "", source),
               f"{slug} has a malformed or labelled SQL reference link")
        runnable_bodies = re.findall(
            r"```sql runnable[^\n]*\n(.*?)\n```", source, re.DOTALL)
        for body in runnable_bodies:
            _check("bioseq_internal." not in body
                   and "inspect_ready_minimizer_generation" not in body,
                   f"{slug} runnable SQL exposes a private or revoked interface")
        all_runnable_ids.extend(runnable_ids)
    _check(len(all_runnable_ids) == len(set(all_runnable_ids)),
           "runnable ids must be unique across every tutorial")
    authored_tutorials = "\n".join(
        (_WEBSITE_ROOT / version / "tutorials" / f"{slug}.md").read_text(
            encoding="utf-8")
        for slug in _EXPECTED_TUTORIALS
    )
    for forbidden in (
        "Test-build-only", "DSM Test Fixture", "Fully Attested Alignment Jobs",
        "Bioseq Batch ABI Version", "Bioseq Owned RestrictInfo Path Copies",
        "Bioseq Match Placeholder Sites", "Bioseq CPU Prepared Target",
        "Bioseq Requirement Executable", "bioseq_internal.",
        "inspect_ready_minimizer_generation",
        "inspect_ready_hash_major_minimizer_generation",
    ):
        _check(forbidden not in authored_tutorials,
               f"public tutorial corpus exposes non-user field {forbidden}")
    _check(re.search(r"\breport_[0-9]+\.json\b", authored_tutorials) is None,
           "tutorials must not hard-code numbered benchmark report filenames")

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
        r'k_hoe_probe_sequence\s*=\s*\n?\s*"([ACGT]+)"', common)
    _check(probe_match is not None,
           "cannot resolve canonical k_hoe_probe_sequence source")

    tutorial = tutorial_path.read_text(encoding="utf-8")
    runnable_match = re.search(
        r"```sql runnable id=canonical_mmsearch[^\n]*\n(.*?)\n```",
        tutorial, re.S)
    _check(runnable_match is not None,
           "similarity tutorial has no canonical runnable search")
    runnable_source = runnable_match.group(1)
    fixture_insert = re.search(
        r"INSERT INTO tutorial_mmsearch\b.*?;", runnable_source, re.S)
    _check(fixture_insert is not None,
           "similarity tutorial has no canonical target INSERT")
    for target, _secondary_probe in fixture_pairs:
        _check(fixture_insert.group(0).count(f"'{target}'") == 1,
               "similarity tutorial does not copy each canonical hoe target exactly once")
    probe = probe_match.group(1)
    _check(tutorial.count(f"'{probe}'") >= 2,
           "similarity tutorial must retain the canonical short-motif examples")
    visible_query = runnable_source.split("-- bioseq:visible", 1)[-1].split(
        "-- bioseq:teardown", 1)[0]
    _check(visible_query.count(f"'{fixture_pairs[0][0]}'") == 2,
           "similarity tutorial must explain and execute the same full-read query")
    for assertion in (
        "EXPLAIN (VERBOSE, COSTS OFF)", "public.mmsearch(",
        "The first result shows whether the planner selected `BioseqScan`",
    ):
        _check(assertion in tutorial,
               f"similarity tutorial is missing visible plan contract {assertion}")
    _check("-- bioseq:expected" not in tutorial,
           "similarity tutorial must not author expected output rows")

    sequence = sequence_path.read_text(encoding="utf-8")
    _check("bioseq.seq('dna4', 64)" in sequence
           and "match=\"'dna4'\" title=\"Alphabet\"" in sequence
           and 'match="64" title="Storage-unit width"' in sequence,
           "sequence anatomy must be driven by the exact two Markdown annotations")
    _check(not re.search(r"temporary|placeholder|lorem", sequence, re.I),
           "sequence tutorial contains disabled or prototype content")
    cache = cache_path.read_text(encoding="utf-8")
    for spelling in (
        "order: 110", "bioseq_cache.query_cache.mode = 'exact_query'",
        "bioseq_cache.query_cache.mode = 'reusable_fragments'",
        "minimizers_per_sequence", "minimizers_per_fragment",
        "bioseq_cache.stats()", "bioseq_cache.entries()",
        "bioseq_cache.invalidate_relation", "bioseq_cache.invalidate_all()",
        "expect=error sqlstate=42501",
    ):
        _check(spelling in cache, f"cache tutorial is missing {spelling}")
    highway = highway_path.read_text(encoding="utf-8")
    for spelling in (
        "order: 90", "EXPLAIN (ANALYZE, VERBOSE, FORMAT JSON)",
        "Highway Compiled Targets", "CPU-Supported Targets",
        "Runnable Targets", "Planned Architecture",
        "Executed Architecture",
    ):
        _check(spelling in highway, f"Highway tutorial is missing {spelling}")
    explain_path = (
        _WEBSITE_ROOT / version / "tutorials" / "reading_bioseq_explain.md"
    )
    explain_source = explain_path.read_text(encoding="utf-8")
    for forbidden in (
        "Test-build-only DSM diagnostics", "DSM Test Fixture",
        "Fully Attested Alignment Jobs", "Bioseq Batch ABI Version",
        "Bioseq Owned RestrictInfo Path Copies",
    ):
        _check(forbidden not in explain_source,
               f"public EXPLAIN tutorial exposes non-user field {forbidden}")
    columnar = columnar_path.read_text(encoding="utf-8")
    for spelling in (
        "order: 20", "id=columnar_vector_aggregate", "VectorAggNode",
        "count(*)", "sum", "avg", "min", "max",
        "bioseq.list_seq_storage_shards", "alter_bioseq_table_set",
        "alter_bioseq_table_reset", "bioseq.column_cache.capacity_mb",
        "bioseq.vacuum_seq_storage", "NULL and empty inputs",
        "Administrator-only shard inspection",
    ):
        _check(spelling in columnar,
               f"columnar/vector tutorial is missing {spelling}")
    generation = generation_path.read_text(encoding="utf-8")
    for spelling in (
        "order: 30", "id=deterministic_sequence_generation",
        "bioseq.create_sample_table", "bioseq.create_query_target_sample_table",
        "same_seed_same_sequence", "rejects any other alphabet",
    ):
        _check(spelling in generation,
               f"sequence-generation tutorial is missing {spelling}")
    io_tutorial = io_path.read_text(encoding="utf-8")
    for spelling in (
        "order: 40", "id=local_fasta_load_index_search", "bioseq.load",
        ".fastq", ".bgzf", "regular zlib-backed", "parallel_gzip",
        "does not provide random range access", "no current SQL function",
    ):
        _check(spelling in io_tutorial,
               f"FASTA/FASTQ tutorial is missing {spelling}")
    index_tutorial = index_path.read_text(encoding="utf-8")
    for spelling in (
        "order: 50", "id=index_lifecycle_metadata",
        "minimizers_per_fragment", "minimizers_per_sequence", "symbol_counts",
        "UPDATE_ON_INSERT", "UPDATE_MANUALLY", "bioseq.list_metadata()",
        "Bioseq Fetched From Meta", "index_read_requirement => 2",
        "actual_index_source=persisted",
        "bioseq.last_alignment_execution_stats()",
        "bioseq.set_active_read_index", "administrator/policy-manager",
    ):
        _check(spelling in index_tutorial,
               f"index-lifecycle tutorial is missing {spelling}")
    landing = landing_path.read_text(encoding="utf-8")
    _check(not re.search(r"temporary|placeholder|lorem|symbol_count", landing, re.I),
           "landing examples contain disabled or prototype content")


def _load_benchmark_fixture_contract(site: Path, base_path: str) -> dict:
    """Bind browser checks to the generated content-classified benchmark bundle."""
    manifest_paths = sorted(site.rglob("benchmarks-manifest.json"))
    _check(len(manifest_paths) == 1,
           f"expected one published benchmarks-manifest.json, got {manifest_paths}")
    manifest_path = manifest_paths[0]
    _check(not manifest_path.is_symlink() and manifest_path.is_file(),
           f"published benchmark manifest is not a regular file: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if isinstance(manifest, list):
        sources = manifest
    elif isinstance(manifest, dict):
        sources = manifest.get("sources", [])
    else:
        sources = []
    _check(isinstance(sources, list) and bool(sources),
           "published benchmark manifest has no JSON sources")

    manifest_route = _public_route(
        base_path, manifest_path.relative_to(site).as_posix())
    manifest_url = f"https://website.invalid{manifest_route}"
    site_root = site.resolve()
    repository_root = _REPOSITORY_ROOT.resolve()
    repository_identity = tuple(
        part for part in urlsplit(_REPOSITORY_URL).path.split("/") if part)
    _check(len(repository_identity) == 2,
           f"repository URL has no owner/repository identity: {_REPOSITORY_URL}")

    def repository_source(relative: Path, context: str) -> Path:
        _check(not relative.is_absolute() and bool(relative.parts)
               and all(part not in {"", ".", ".."} for part in relative.parts),
               f"{context} has an invalid repository path: {relative}")
        lexical = repository_root
        for part in relative.parts:
            lexical = lexical / part
            _check(not lexical.is_symlink(),
                   f"{context} traverses a repository symlink: {relative}")
        try:
            resolved = lexical.resolve(strict=True)
            resolved.relative_to(repository_root)
        except (OSError, ValueError) as error:
            raise AssertionFail(
                f"{context} escapes or is absent from the repository: {relative}: "
                f"{error}") from None
        _check(resolved.is_file(),
               f"{context} is not a regular repository file: {relative}")
        return resolved

    records: list[dict] = []
    local_source_routes: set[str] = set()
    local_generated_paths: set[Path] = set()
    local_source_sha256: dict[str, str] = {}
    remote_sources: set[str] = set()
    remote_artifacts: dict[str, bytes] = {}
    source_values: set[str] = set()
    reserved = {"params", "test_cases", "regression_rules", "meta", "benchmarks"}

    for source_index, source in enumerate(sources):
        if isinstance(source, str):
            value = source
        elif (isinstance(source, dict) and len(source) == 1
              and set(source).issubset({"url", "path"})):
            value = source.get("url", source.get("path", ""))
        else:
            value = ""
        _check(isinstance(value, str) and bool(value),
               f"benchmark manifest contains an invalid source: {source!r}")
        _check(value not in source_values,
               f"benchmark manifest repeats source {value!r}")
        source_values.add(value)
        parsed = urlsplit(value)
        repository_relative: Path
        generated_path: Path | None = None

        if parsed.scheme or parsed.netloc:
            decoded_path = unquote(parsed.path)
            decoded_parts = tuple(
                part for part in decoded_path.split("/") if part)
            try:
                port = parsed.port
            except ValueError:
                port = -1
            _check(
                parsed.scheme == "https"
                and parsed.hostname in _BENCHMARK_REMOTE_HOSTS
                and port is None
                and parsed.username is None
                and parsed.password is None
                and not parsed.query
                and not parsed.fragment
                and len(decoded_parts) >= 4
                and tuple(decoded_parts[:2]) == repository_identity
                and re.fullmatch(r"[0-9a-f]{40}", decoded_parts[2]) is not None
                and decoded_path == "/" + "/".join(decoded_parts)
                and quote(decoded_path, safe="/-._~") == parsed.path
                and "%" not in decoded_path
                and "\\" not in decoded_path
                and ".." not in decoded_parts
                and "/blob/" not in parsed.path,
                f"benchmark override is not a safe immutable public raw URL: {value}",
            )
            revision = decoded_parts[2]
            repository_relative = Path(*decoded_parts[3:])
            try:
                remote_blob = subprocess.run(
                    ["git", "show", f"{revision}:{repository_relative.as_posix()}"],
                    cwd=repository_root,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=15,
                    check=False,
                )
            except subprocess.TimeoutExpired as error:
                raise AssertionFail(
                    f"benchmark override Git lookup timed out: {value}") from error
            _check(remote_blob.returncode == 0,
                   f"benchmark override revision/path is unavailable locally: {value}")
            body = remote_blob.stdout
            remote_sources.add(value)
            remote_artifacts[value] = body
        else:
            decoded_path = unquote(parsed.path)
            decoded_parts = Path(decoded_path).parts
            _check(
                not parsed.path.startswith("/")
                and not parsed.query
                and not parsed.fragment
                and len(decoded_parts) >= 2
                and decoded_parts[0] == "benchmark-data"
                and decoded_path == "/".join(decoded_parts)
                and quote(decoded_path, safe="/-._~") == parsed.path
                and "%" not in decoded_path
                and "\\" not in decoded_path
                and ".." not in decoded_parts,
                f"same-origin benchmark source is not a canonical contained path: {value}",
            )
            lexical_generated_path = manifest_path.parent.joinpath(*decoded_parts)
            current = manifest_path.parent
            for part in decoded_parts:
                current = current / part
                _check(not current.is_symlink(),
                       f"same-origin benchmark source traverses a symlink: {value}")
            try:
                generated_path = lexical_generated_path.resolve(strict=True)
                generated_path.relative_to(site_root)
            except (OSError, ValueError):
                raise AssertionFail(
                    f"benchmark manifest source escapes or is absent: {value}") from None
            _check(generated_path.is_file(),
                   f"same-origin benchmark artifact is missing: {generated_path}")
            body = generated_path.read_bytes()
            repository_relative = Path(*decoded_parts[1:])
            authoritative = repository_source(
                repository_relative, f"same-origin benchmark source {value}")
            _check(body == authoritative.read_bytes(),
                   f"published benchmark artifact rewrote source bytes: "
                   f"{repository_relative.as_posix()}")
            source_route = urlsplit(urljoin(manifest_url, value)).path
            local_source_routes.add(source_route)
            local_generated_paths.add(generated_path)
            generated_relative = generated_path.relative_to(site_root).as_posix()
            local_source_sha256[generated_relative] = hashlib.sha256(body).hexdigest()

        source_name = Path(unquote(parsed.path)).name
        try:
            document = json.loads(body)
        except json.JSONDecodeError as error:
            raise AssertionFail(
                f"benchmark artifact {value} is invalid JSON: {error}") from error
        _check(isinstance(document, dict),
               f"benchmark artifact {value} must be a JSON object")

        configuration_keys = tuple(
            key for key, block in document.items()
            if key not in reserved
            and isinstance(block, dict)
            and isinstance(block.get("matrix"), dict)
        )
        configuration_claim = (
            isinstance(document.get("params"), dict)
            and bool(document["params"])
            and isinstance(document.get("test_cases"), dict)
            and bool(document["test_cases"])
            and isinstance(document.get("regression_rules"), list)
            and bool(configuration_keys)
        )
        report_claim = (
            isinstance(document.get("benchmarks"), dict)
            and bool(document["benchmarks"])
            and isinstance(document.get("meta"), dict)
        )
        _check(configuration_claim != report_claim,
               f"benchmark artifact {value} does not claim exactly one supported shape")

        timestamp = 0
        created = ""
        if report_claim:
            meta = document["meta"]
            timestamp = meta.get("timestamp")
            created = meta.get("created")
            _check(type(timestamp) is int and timestamp >= 0
                   and isinstance(created, str) and created.endswith("Z")
                   and isinstance(meta.get("dirty"), bool)
                   and meta.get("fast") is False
                   and isinstance(meta.get("history_eligible"), bool),
                   f"benchmark report {value} lacks current public writer metadata")
            try:
                parsed_created = datetime.fromisoformat(
                    created.replace("Z", "+00:00"))
                created_timestamp = int(parsed_created.timestamp())
            except (OSError, OverflowError, ValueError) as error:
                raise AssertionFail(
                    f"benchmark report {value} has invalid created metadata: "
                    f"{error}") from error
            _check(parsed_created.tzinfo is not None
                   and abs(created_timestamp - timestamp) <= 1,
                   f"benchmark report {value} has inconsistent created/timestamp metadata")
            for key, benchmark in document["benchmarks"].items():
                _check(isinstance(key, str) and bool(key)
                       and isinstance(benchmark, dict)
                       and isinstance(benchmark.get("execution_tuples"), list)
                       and bool(benchmark["execution_tuples"]),
                       f"benchmark report {value} has an invalid benchmark block")

        records.append({
            "manifest_index": source_index,
            "value": value,
            "name": source_name,
            "kind": "report" if report_claim else "configuration",
            "document": document,
            "repository_relative": repository_relative.as_posix(),
            "configuration_keys": configuration_keys,
            "timestamp": timestamp,
            "created": created,
        })

    has_local_sources = bool(local_generated_paths)
    _check(not (has_local_sources and remote_sources),
           "benchmark manifest mixes same-origin data with remote overrides")
    data_root = manifest_path.parent / "benchmark-data"
    _check(not data_root.is_symlink(),
           "published benchmark-data root must not be a symlink")
    staged_files: set[Path] = set()
    if data_root.exists():
        _check(data_root.is_dir(),
               "published benchmark-data root must be a directory")
        for entry in sorted(data_root.rglob("*")):
            _check(not entry.is_symlink(),
                   f"published benchmark-data contains a symlink: {entry}")
            if entry.is_dir():
                continue
            _check(entry.is_file(),
                   f"published benchmark-data contains a special file: {entry}")
            staged_files.add(entry.resolve())
    if has_local_sources:
        _check(staged_files == local_generated_paths,
               "published benchmark-data inventory differs from manifest sources: "
               f"staged={sorted(str(path) for path in staged_files)}, "
               f"manifest={sorted(str(path) for path in local_generated_paths)}")
    else:
        _check(not staged_files,
               "remote benchmark override retained same-origin benchmark data")

    if has_local_sources:
        expected_records = sorted(
            (record for record in records if record["kind"] == "configuration"),
            key=lambda record: record["repository_relative"],
        ) + sorted(
            (record for record in records if record["kind"] == "report"),
            key=lambda record: (
                -record["timestamp"], record["repository_relative"]),
        )
        _check(records == expected_records,
               "same-origin benchmark manifest is not in deterministic "
               "configuration/metadata-newest order")

    benchmarks: dict[str, dict] = {}

    def ensure_benchmark(key: str) -> dict:
        if key not in benchmarks:
            benchmarks[key] = {
                "key": key,
                "title": key,
                "has_results": False,
                "runs": [],
            }
        return benchmarks[key]

    for record in records:
        if record["kind"] != "configuration":
            continue
        for key in record["configuration_keys"]:
            ensure_benchmark(key)
    for record in records:
        if record["kind"] != "report":
            continue
        for key, benchmark in record["document"]["benchmarks"].items():
            model_benchmark = ensure_benchmark(key)
            model_benchmark["has_results"] = True
            model_benchmark["title"] = benchmark.get("title") or model_benchmark["title"]
            tuples = benchmark["execution_tuples"]
            case_set = {
                case
                for execution_tuple in tuples
                if isinstance(execution_tuple, dict)
                for case in (execution_tuple.get("test_cases") or {})
            }
            model_benchmark["runs"].append({
                "manifest_index": record["manifest_index"],
                "name": record["name"],
                "created": record["created"],
                "timestamp": record["timestamp"],
                "score": len(tuples) * max(1, len(case_set)),
            })

    _check(bool(benchmarks), "benchmark sources expose no configured benchmark keys")
    model_order = sorted(
        benchmarks,
        key=lambda key: (
            0 if benchmarks[key]["has_results"] else 1,
            key,
        ),
    )
    for benchmark in benchmarks.values():
        benchmark["runs"].sort(key=lambda run: run["manifest_index"])
        benchmark["runs"].sort(key=lambda run: run["created"], reverse=True)
        benchmark["runs"].sort(key=lambda run: run["score"], reverse=True)
    benchmark_key = sorted(
        model_order,
        key=lambda key: -(
            benchmarks[key]["runs"][0]["score"]
            if benchmarks[key]["runs"] else 0
        ),
    )[0]
    benchmark = benchmarks[benchmark_key]
    default_report = benchmark["runs"][0] if benchmark["runs"] else None
    if default_report is not None:
        equally_rich = [
            run for run in benchmark["runs"]
            if run["score"] == default_report["score"]
        ]
        _check(default_report["timestamp"] == max(
                   run["timestamp"] for run in equally_rich),
               "default report is not the metadata-newest equally rich run")

    return {
        "key": benchmark_key,
        "title": benchmark["title"],
        "benchmark_count": len(benchmarks),
        "default_report": default_report,
        "sources": sources,
        "manifest_route": manifest_route,
        "local_source_routes": local_source_routes,
        "local_source_sha256": dict(sorted(local_source_sha256.items())),
        "remote_sources": remote_sources,
        "remote_artifacts": remote_artifacts,
        "same_origin": has_local_sources,
    }


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
    model_runnable_ids: list[str] = []
    for tutorial in tutorials:
        slug = tutorial["slug"]
        examples = tutorial.get("sql_examples", [])
        _check(len(examples) == _EXPECTED_RUNNABLE_COUNTS[slug],
               f"{slug} must carry {_EXPECTED_RUNNABLE_COUNTS[slug]} "
               f"runnable example(s), got {len(examples)}")
        groups = _runnable_output_groups(tutorial.get("content", []))
        local_ids = [example.get("local_id", "") for example in examples]
        _check(all(bool(identifier) for identifier in local_ids),
               f"{slug}: resolved runnable example has no local id")
        _check(set(groups) == set(local_ids),
               f"{slug}: result groups {sorted(groups)} do not match examples "
               f"{sorted(local_ids)}")

        for example in examples:
            _check(not example.get("setup_sql") and not example.get("teardown_sql"),
                   f"resolved model leaked hidden SQL for {example.get('canonical_id')}")
            visible = example.get("visible_sql", "")
            _check(bool(visible.strip()),
                   f"runnable example {example.get('canonical_id')} has no visible SQL")
            _check("TEMPORARY" not in visible and "bioseq:setup" not in visible,
                   f"runnable example {example.get('canonical_id')} leaked disabled/setup text")
            canonical_id = example.get("canonical_id", "")
            _check(canonical_id == f"{version}/tutorial/{slug}#{example['local_id']}",
                   f"{slug}: malformed canonical runnable id {canonical_id!r}")
            model_runnable_ids.append(canonical_id)

            result_nodes = groups[example["local_id"]]
            provenance_count = _node_text({"children": result_nodes}).count(
                "Executed against Bioseq extension")
            _check(provenance_count == 1,
                   f"{canonical_id}: captured output needs one backend provenance line")
            tables = _result_tables(result_nodes)
            if example.get("expectation") == "error":
                sqlstate = example.get("expected_sqlstate", "")
                _check(not tables and bool(sqlstate)
                       and f"SQLSTATE {sqlstate}:" in _node_text({"children": result_nodes}),
                       f"{canonical_id}: expected error output lost SQLSTATE {sqlstate}")
            else:
                _check(bool(tables),
                       f"{canonical_id}: successful runnable SQL has no result table")
                for table in tables:
                    _check(len(table) >= 2 and bool(table[0])
                           and all(bool(cell.strip()) for cell in table[0]),
                           f"{canonical_id}: result table lacks readable columns or rows")

        source = (_WEBSITE_ROOT / version / "tutorials" / f"{slug}.md").read_text(
            encoding="utf-8")
        authored_links = set(re.findall(r"\[\[(sql:[^\]|]+)\]\]", source))
        resolved_links = {
            node.get("target", ""): node.get("attributes", {}).get("entity_id", "")
            for node in _nodes_of_kind(tutorial.get("content", []), "entity_link")
            if node.get("target", "").startswith("sql:")
        }
        _check(authored_links <= set(resolved_links),
               f"{slug}: authored SQL links did not survive resolution: "
               f"{sorted(authored_links - set(resolved_links))}")
        _check(all(resolved_links[target].startswith("sql:")
                   for target in authored_links),
               f"{slug}: one or more SQL links resolved outside the SQL catalog")

    _check(len(model_runnable_ids) == len(set(model_runnable_ids)),
           "resolved runnable ids must be globally unique")

    sequence_annotations = _nodes_of_kind(
        by_slug["sequence_column_type"].get("content", []), "code_annotation")
    _check(len(sequence_annotations) == 2,
           "sequence type anatomy must contain exactly two Markdown annotations")
    annotation_titles = [item.get("attributes", {}).get("title")
                         for item in sequence_annotations]
    _check(annotation_titles == ["Alphabet", "Storage-unit width"],
           f"unexpected anatomy annotation titles: {annotation_titles}")

    columnar_example = by_slug[
        "columnar_storage_and_vectorized_aggregates"
    ]["sql_examples"][0]
    _check("EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)"
           in columnar_example["visible_sql"]
           and "bioseq.list_seq_storage_shards" not in columnar_example["visible_sql"],
           "columnar tutorial must execute its analyzed aggregate without "
           "administrator-only shard inspection")
    columnar_results = "\n".join(
        cell
        for table in _result_tables(by_slug[
            "columnar_storage_and_vectorized_aggregates"].get("content", []))
        for row in table
        for cell in row
    )
    _check("Custom Scan (VectorAggNode)" in columnar_results
           and "Custom Scan (BioseqScan)" in columnar_results,
           "columnar aggregate example did not capture VectorAggNode over BioseqScan")

    generation_example = by_slug["sequence_generation"]["sql_examples"][0]
    for spelling in (
        "bioseq.create_sample_table", "bioseq.create_query_target_sample_table",
        "same_seed_same_sequence", "truth_target_start", "effective_error_rate",
    ):
        _check(spelling in generation_example["visible_sql"],
               f"generation example visible SQL is missing {spelling}")

    io_example = by_slug["fasta_fastq_gzip_and_io"]["sql_examples"][0]
    for spelling in (
        "bioseq.load", "landing_reads.fasta",
        "bioseq.update_index", "minimizers_per_fragment",
        "symbol_counts", "EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)",
    ):
        _check(spelling in io_example["visible_sql"],
               f"local-I/O example visible SQL is missing {spelling}")

    index_example = by_slug[
        "index_lifecycle_and_metadata"
    ]["sql_examples"][0]
    for spelling in (
        "bioseq.list_metadata()", "bioseq.update_index",
        "minimizers_per_fragment", "minimizers_per_sequence", "symbol_counts",
        "index_read_requirement => 2",
        "bioseq.last_alignment_execution_stats()", "stage = 'query_sketch'",
    ):
        _check(spelling in index_example["visible_sql"],
               f"index-lifecycle example visible SQL is missing {spelling}")
    index_results = "\n".join(
        cell
        for table in _result_tables(by_slug[
            "index_lifecycle_and_metadata"].get("content", []))
        for row in table
        for cell in row
    )
    _check("Custom Scan (BioseqBatchJoin)" in index_results
           and "require_compatible_persisted" in index_results
           and "Bioseq Fetched From Meta" in index_results
           and "actual_index_source=persisted" in index_results,
           "index lifecycle did not capture public persisted-route evidence")
    index_tables = _result_tables(
        by_slug["index_lifecycle_and_metadata"].get("content", []))
    stats_table = next((table for table in index_tables
                        if table and table[0][0].startswith(
                            "requested_target_index_source")), None)
    _check(stats_table is not None and len(stats_table) == 2,
           "index lifecycle needs one captured query-sketch statistics row")
    stats = dict(zip(stats_table[0], stats_table[1]))
    _check(stats.get("requested_target_index_sourcetext") == "persisted"
           and stats.get("actual_target_index_sourcetext") == "persisted"
           and int(stats.get("persisted_index_rowsbigint", "0")) > 0
           and stats.get("missing_persisted_index_rowsbigint") == "0"
           and stats.get("recomputed_target_index_rowsbigint") == "0"
           and int(stats.get("persisted_generation_read_sessionsbigint", "0")) > 0,
           "index lifecycle statistics do not prove persisted reads without recomputation")
    batch_rows = re.search(
        r"Custom Scan \(BioseqBatchJoin\).*\brows=([0-9]+)", index_results)
    _check(batch_rows is not None and int(batch_rows.group(1)) > 0,
           "index lifecycle must find matches through its persisted batch join")

    similarity_sql = by_slug["similarity_search"]["sql_examples"][0]["visible_sql"]
    required_sql = (
        "public.mmsearch", "bioseq.query_start", "bioseq.query_end",
        "bioseq.target_start", "bioseq.target_end", "bioseq.cigar",
        "CTAGTCACTGTAGCCTGATCGTAGCTAGT",
    )
    for spelling in required_sql:
        _check(spelling in similarity_sql,
               f"similarity tutorial visible SQL is missing {spelling}")
    similarity_tables = _result_tables(
        by_slug["similarity_search"].get("content", []))
    _check(len(similarity_tables) == 2
           and similarity_tables[0][0][0].startswith("QUERY PLAN")
           and len(similarity_tables[0]) > 1
           and len(similarity_tables[1]) > 1
           and "Custom Scan (BioseqScan)" in "\n".join(
               row[0] for row in similarity_tables[0][1:]),
           "similarity tutorial must capture its BioseqScan plan and result table")

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

    explain_example = by_slug["reading_bioseq_explain"]["sql_examples"][0]
    _check("EXPLAIN (ANALYZE, VERBOSE, BUFFERS, COSTS OFF)"
           in explain_example["visible_sql"],
           "EXPLAIN tutorial does not publish its analyzed text plan")
    explain_tables = _result_tables(
        by_slug["reading_bioseq_explain"].get("content", []))
    _check(len(explain_tables) == 1
           and explain_tables[0][0][0].startswith("QUERY PLAN")
           and len(explain_tables[0]) > 2,
           "EXPLAIN tutorial did not capture a formatted PostgreSQL plan table")
    explain_output = "\n".join(row[0] for row in explain_tables[0][1:])
    for label in (
        "Custom Scan (BioseqScan)", "One-to-many alignment:",
        "Highway Compiled Targets", "Executed Architecture",
        "| QuerySketchStage:", "| chain_stage:", "| align_stage:",
        "* Execution statistics [", "* Index outcomes [",
    ):
        _check(label in explain_output,
               f"executed text plan is missing Bioseq section {label}")
    scan_rows = re.search(
        r"Custom Scan \(BioseqScan\).*\brows=([0-9]+)", explain_output)
    _check(scan_rows is not None and int(scan_rows.group(1)) > 0,
           "EXPLAIN tutorial must demonstrate a search that finds matches")

    remote_content = by_slug["remote_provider_to_search"].get("content", [])
    remote_table_nodes = [
        node for node in _nodes_of_kind(remote_content, "table")
        if node.get("attributes", {}).get("class") == "result-table"
    ]
    _check(bool(remote_table_nodes)
           and bool(remote_table_nodes[0].get("children")),
           "remote-provider tutorial has no captured capability table model")
    remote_header_contract: list[tuple[str, str]] = []
    for cell in remote_table_nodes[0]["children"][0].get("children", []):
        children = cell.get("children", [])
        type_nodes = [child for child in children
                      if child.get("kind") == "code_span"]
        name = "".join(_node_text(child) for child in children
                       if child.get("kind") not in {"code_span", "line_break"})
        _check(len(type_nodes) == 1 and bool(name),
               "remote-provider capability header lost its name/type model")
        remote_header_contract.append((name, _node_text(type_nodes[0])))
    _check(remote_header_contract == [
               ("schema_ready", "boolean"),
               ("table_am_ready", "boolean"),
               ("relation_count", "bigint"),
           ],
           "remote-provider capability columns lost PostgreSQL format_type labels")
    remote_tables = _result_tables(remote_content)
    _check(len(remote_tables) == 2
           and remote_tables[0][0] == [
               name + type_name for name, type_name in remote_header_contract]
           and len(remote_tables[0]) == 2
           and len(remote_tables[1]) >= 2,
           "remote-provider tutorial must show honest capability and search results")

    landing = {item.get("slot"): item for item in model.get("landing_examples", [])}
    expected_slots = {"hero_query", "sequence_storage", "similarity_search"}
    _check(set(landing) == expected_slots,
           f"landing model slots are {sorted(landing)}, expected {sorted(expected_slots)}")
    for slot, item in landing.items():
        _check(item.get("has_result") is True and isinstance(item.get("result"), dict),
               f"landing slot {slot} has no captured PostgreSQL result")
        _check(bool(item["result"].get("rows")),
               f"landing slot {slot} must demonstrate a nonempty search result")
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
        "SET bioseq.index.insert_maintenance.enabled = off",
        "bioseq.update_index", "minimizers_per_fragment",
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
           "search index must contain every direct tutorial record")
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
    benchmarks = _load_benchmark_fixture_contract(site, base_path)
    return {
        "model": model,
        "tutorials": by_slug,
        "landing": landing,
        "search_route": _public_route(base_path, f"{version}/search-index.json"),
        "tooltip_route": _public_route(
            base_path, f"{version}/{tooltip_paths[0].name}"),
        "tooltip_records": tooltip_records,
        "design": design,
        "benchmarks": benchmarks,
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
        "tutorial_quick":
            f"{base}/{version}/tutorials/quick_read_search_write/",
        "tutorial_sequence":
            f"{base}/{version}/tutorials/sequence_column_type/",
        "tutorial_columnar":
            f"{base}/{version}/tutorials/columnar_storage_and_vectorized_aggregates/",
        "tutorial_generation":
            f"{base}/{version}/tutorials/sequence_generation/",
        "tutorial_io":
            f"{base}/{version}/tutorials/fasta_fastq_gzip_and_io/",
        "tutorial_index":
            f"{base}/{version}/tutorials/index_lifecycle_and_metadata/",
        "tutorial_slice":
            f"{base}/{version}/tutorials/slice_concat_reverse/",
        "tutorial_similarity":
            f"{base}/{version}/tutorials/similarity_search/",
        "tutorial_columns":
            f"{base}/{version}/tutorials/multiple_sequence_columns_and_joins/",
        "tutorial_remote":
            f"{base}/{version}/tutorials/remote_provider_to_search/",
        "tutorial_cache":
            f"{base}/{version}/tutorials/query_cache/",
        "tutorial_highway":
            f"{base}/{version}/tutorials/highway_runtime_dispatch/",
        "tutorial_explain":
            f"{base}/{version}/tutorials/reading_bioseq_explain/",
        "reference": f"{base}/{version}/reference/",
        "entity": _discover_entity_route(site, base, version),
        "benchmarks": f"{base}/benchmarks/",
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
    comparison_expected = [
        [[cell.strip() for cell in row] for row in table]
        for table in expected
    ]
    _check(actual == comparison_expected,
           f"{tutorial['slug']}: rendered SQL cells differ from real PostgreSQL model")
    for index, expected_table in enumerate(expected):
        _check(len(expected_table) >= 2 and bool(expected_table[0]),
               f"{tutorial['slug']}: captured result table has no data row")
        expected_columns = len(expected_table[0]) if expected_table else 0
        type_labels = tables.nth(index).locator("thead th code")
        _check(type_labels.count() == expected_columns
               and all(bool(label.strip()) for label in type_labels.all_text_contents()),
               f"{tutorial['slug']}: result table omits PostgreSQL column type labels")
    body_text = page.locator("main#doc-main").inner_text()
    _check(body_text.count("Executed against Bioseq extension")
           == len(tutorial.get("sql_examples", [])),
           f"{tutorial['slug']}: every runnable result must carry backend provenance")
    plan_table_indexes = [
        index for index, table in enumerate(expected)
        if table and table[0] and table[0][0].startswith("QUERY PLAN")
    ]
    for index in plan_table_indexes:
        plan_cells = tables.nth(index).locator("tbody td")
        expected_plan_cells = [row[0] for row in expected[index][1:]]
        _check(plan_cells.count() == len(expected_plan_cells)
               and plan_cells.all_text_contents() == expected_plan_cells,
               f"{tutorial['slug']}: EXPLAIN cells did not preserve model whitespace")
        if len(expected_plan_cells) > 1:
            _check(any(cell != cell.lstrip() for cell in expected_plan_cells),
                   f"{tutorial['slug']}: text EXPLAIN model has no indentation to preserve")
        _check(plan_cells.evaluate_all(
                   "cells => cells.every((cell) => "
                   "getComputedStyle(cell).whiteSpace === 'pre-wrap')"),
               f"{tutorial['slug']}: EXPLAIN rows lost pre-wrap rendering")


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
    first = _public_route(
        base_path, f"{version}/tutorials/{_EXPECTED_TUTORIALS[0]}/")
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
        prefixes = ("ld-s",)
        pairs = (
            ("data-screen-label:BioseqDB landing",
             '[data-screen-label="BioseqDB landing"]'),
            ("id:nav", "#nav"), ("id:annotate", "#annotate"),
            ("id:workflow", "#workflow"), ("id:cta", "#cta"),
        )
    elif label == "benchmarks":
        source = design["benchmarks"]
        prefixes = ("bd-s", "bm-s", "bench-")
        pairs = (
            ("data-screen-label:BioseqDB benchmarks",
             '[data-screen-label="BioseqDB benchmarks"]'),
            ("id:bhead", "#bhead"),
            ("data-bgrid:first", "[data-bgrid]"),
            ("data-bnav:first", "[data-bnav]"),
            ("data-ctrls:first", "[data-ctrls]"),
            ("data-footcols:first", "[data-footcols]"),
        )
    else:
        source = design["docs"]
        prefixes = ("dd-s",)
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
        _check(any(name.startswith(prefixes) for name in classes.split()),
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

    if label == "benchmarks":
        records = design["benchmarks"]
        _compare_design_style(
            page, "#bhead", records["id:bhead"],
            ("position", "top", "zIndex", "backgroundColor",
             "borderBottomWidth", "borderBottomColor", "backdropFilter"),
            "benchmark header")
        _compare_design_style(
            page, "[data-bgrid]", records["data-bgrid:first"],
            ("display", "alignItems"), "benchmark body grid")
        _compare_design_style(
            page, "[data-bnav]", records["data-bnav:first"],
            ("position", "top", "height", "overflowY", "paddingTop",
             "paddingRight", "paddingBottom", "paddingLeft",
             "borderRightWidth", "borderRightColor"),
            "benchmark navigation rail")
        _compare_design_style(
            page, "[data-ctrls]", records["data-ctrls:first"],
            ("display", "gap", "marginTop", "paddingTop", "paddingRight",
             "paddingBottom", "paddingLeft", "borderTopWidth", "borderRadius"),
            "benchmark filter grid")
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


def _wait_for_benchmark_fetch_gate(page) -> None:
    pending = 0
    for _ in range(120):
        pending = page.evaluate(
            "() => window.__bioseqBenchmarkFetchGate?.pending.length || 0")
        if pending:
            return
        page.wait_for_timeout(25)
    raise AssertionFail(
        "benchmark loader never requested its HTTP JSON manifest")


def _release_benchmark_fetches(page) -> None:
    page.evaluate("() => window.__releaseBioseqBenchmarkFetches()")
    try:
        page.locator('[data-screen-label="BioseqDB benchmarks"] h1').wait_for(
            state="visible", timeout=5000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"benchmark real-data model did not render: {error}")


def _exercise_benchmarks(page, request_urls: list[str], contract: dict,
                         base_path: str, version: str) -> None:
    """Exercise the copied shell against every dynamically discovered artifact."""
    benchmark = contract["benchmarks"]
    _check((page.locator("h1").inner_text()).strip() == benchmark["title"],
           "benchmark heading is not derived from discovered benchmark content")
    _check(benchmark["key"] in page.locator("main").inner_text(),
           f"rendered benchmark model is missing real key {benchmark['key']}")
    _check(f"{benchmark['benchmark_count']} benchmarks" in
           page.locator("#bhead").inner_text(),
           "benchmark status does not reflect the complete supplied artifact model")
    _check(page.locator("[data-bnav]").count() == 1,
           "benchmark model has no copied navigation rail")
    _check(page.locator("[data-bnav]", has_text=benchmark["key"]).count() == 1,
           "benchmark navigation was not populated from the real artifacts")
    expected_header_routes = {
        "Docs": _public_route(base_path, f"{version}/"),
        "Product": _public_route(base_path, ""),
        "Benchmarks": _public_route(base_path, "benchmarks/"),
    }
    for text_value, expected_route in expected_header_routes.items():
        link = page.locator("#bhead nav a", has_text=text_value)
        _check(link.count() == 1
               and urlsplit(link.get_attribute("href") or "").path == expected_route,
               f"benchmark {text_value} navigation does not preserve mount "
               f"{expected_route}")

    controls = page.locator("[data-ctrls] select")
    _check(controls.count() == 4,
           f"benchmark comparison requires four filters, got {controls.count()}")
    for index in range(controls.count()):
        _check(controls.nth(index).locator("option").count() >= 1,
               f"benchmark filter {index} has no model-derived options")
    statistic = controls.nth(1)
    _check(statistic.locator("option[value='P01']").count() == 1,
           "benchmark statistic filter lost the P01 option")
    default_report = benchmark["default_report"]
    if default_report is not None:
        report_chip = page.locator("#benchmark-main").get_by_text(
            default_report["name"], exact=True)
        _check(report_chip.count() == 1,
               "benchmark page did not select the metadata-newest default report")
        report_chip.hover()
        report_tooltips: list[str] = []
        for _ in range(120):
            report_tooltips = page.evaluate(
                """() => [...document.body.querySelectorAll('*')]
                  .filter((node) => {
                    const style = getComputedStyle(node);
                    return style.position === 'fixed' &&
                      style.pointerEvents === 'none' && Number(style.opacity) > 0;
                  })
                  .map((node) => (node.textContent || '').trim())
                  .filter(Boolean)""")
            if report_tooltips:
                break
            page.wait_for_timeout(25)
        _check(any(default_report["created"] in text
                   for text in report_tooltips),
               "selected report tooltip does not expose its newest metadata")

        metric = controls.nth(0)
        metric_index = min(1, metric.locator("option").count() - 1)
        metric_value = (
            metric.locator("option").nth(metric_index).get_attribute("value") or "")
        metric.select_option(metric_value)
        page.wait_for_function(
            "metric => document.body.textContent.includes(metric)", metric_value)
        _check(metric.input_value() == metric_value,
               "benchmark metric filter did not retain its selected value")
        statistic.select_option("P01")
        page.wait_for_function(
            "() => document.body.textContent.includes('P01 of')")
        _check("P01 of" in page.locator("main").inner_text(),
               "benchmark statistic filter did not update the rendered table note")
        baseline = controls.nth(3)
        if baseline.locator("option").count() > 1:
            baseline_value = (
                baseline.locator("option").nth(1).get_attribute("value") or "")
            baseline.select_option(baseline_value)
            page.wait_for_function(
                "value => document.body.textContent.includes('baseline ' + value)",
                baseline_value)
            _check(baseline.input_value() == baseline_value,
                   "benchmark baseline filter did not retain its selected value")
        direction = page.get_by_role("button", name="↑ higher is better")
        _check(direction.count() == 1, "benchmark direction filter is missing")
        direction.click()
    else:
        statistic.select_option("P01")
        _check(statistic.input_value() == "P01",
               "configuration-only statistic filter did not retain its value")
        _check("configuration only — no report published" in
               page.locator("#benchmark-main").inner_text(),
               "configuration-only benchmark state is not identifiable")
        _check(page.get_by_role("button", name="↑ higher is better").count() == 0,
               "configuration-only benchmark exposed report direction controls")

    test_cases = page.locator("[data-bnav] a", has_text="Test cases").first
    _check(test_cases.count() == 1, "benchmark Test cases navigation is missing")
    test_cases.click()
    _check("Test cases" in page.locator("main").inner_text(),
           "benchmark sidebar navigation did not render the Test cases view")
    comparison = page.locator("[data-bnav] a", has_text="Comparison").first
    comparison.click()
    _check(page.locator("[data-ctrls]").count() == 1,
           "benchmark sidebar navigation did not restore the Comparison view")

    if default_report is not None:
        # The first tbody cell can be the optional group-heading colspan cell.
        # A measured result cell carries the supplied distribution tooltip.
        hover_target = page.locator("main table tbody tr td:nth-child(2)").first
        _check(hover_target.count() == 1,
               "benchmark comparison table has no model-derived cell to inspect")
        hover_target.hover()
        tooltip_texts: list[str] = []
        for _ in range(120):
            tooltip_texts = page.evaluate(
                """() => [...document.body.querySelectorAll('*')]
                  .filter((node) => {
                    const style = getComputedStyle(node);
                    return style.position === 'fixed' &&
                      style.pointerEvents === 'none' && Number(style.opacity) > 0;
                  })
                  .map((node) => (node.textContent || '').trim())
                  .filter(Boolean)""")
            if tooltip_texts:
                break
            page.wait_for_timeout(25)
        _check(bool(tooltip_texts),
               "hovering a real benchmark cell did not expose its distribution tooltip")
    else:
        _check("No report published for this benchmark yet." in
               page.locator("#benchmark-main").inner_text(),
               "configuration-only comparison did not retain its empty-report note")

    page.evaluate("() => window.__closeBioseqBenchmarkFetchGate()")
    page.get_by_role("button", name="Reload").click()
    loading = page.get_by_text("Fetching benchmark artifacts…", exact=True)
    try:
        loading.wait_for(state="visible", timeout=3000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"benchmark Reload did not restore loading state: {error}")
    _wait_for_benchmark_fetch_gate(page)
    _release_benchmark_fetches(page)

    manifest_requests = {
        url for url in request_urls
        if urlsplit(url).path == benchmark["manifest_route"]
    }
    _check(bool(manifest_requests),
           "benchmark browser run did not fetch its manifest over HTTP")
    page_origin = urlsplit(page.url)
    _check(all(
               urlsplit(url).scheme in {"http", "https"}
               and (urlsplit(url).scheme, urlsplit(url).netloc)
               == (page_origin.scheme, page_origin.netloc)
               for url in manifest_requests),
           "benchmark manifest was not fetched from the page origin over HTTP")

    requested_local_routes = {
        urlsplit(url).path for url in request_urls
        if urlsplit(url).path in benchmark["local_source_routes"]
    }
    _check(requested_local_routes == benchmark["local_source_routes"],
           f"benchmark loader requested local artifacts "
           f"{sorted(requested_local_routes)}, expected "
           f"{sorted(benchmark['local_source_routes'])}")
    _check(all(
               (urlsplit(url).scheme, urlsplit(url).netloc)
               == (page_origin.scheme, page_origin.netloc)
               for url in request_urls
               if urlsplit(url).path in benchmark["local_source_routes"]),
           "benchmark artifact request escaped the generated site's origin")
    requested_remote_sources = {
        url for url in request_urls if url in benchmark["remote_sources"]
    }
    _check(requested_remote_sources == benchmark["remote_sources"],
           f"benchmark loader requested remote overrides "
           f"{sorted(requested_remote_sources)}, expected "
           f"{sorted(benchmark['remote_sources'])}")
    _check(all(urlsplit(url).scheme in {"http", "https"}
               for url in requested_remote_sources),
           "benchmark remote overrides were not fetched over HTTP")
    if benchmark["same_origin"]:
        unexpected_remote = [
            url for url in request_urls
            if urlsplit(url).hostname in _BENCHMARK_REMOTE_HOSTS
        ]
        _check(not unexpected_remote,
               f"default benchmark bundle made remote requests: {unexpected_remote}")

    blob_requests = [url for url in request_urls
                     if urlsplit(url).hostname == "github.com"
                     and "/blob/" in urlsplit(url).path]
    _check(not blob_requests,
           f"benchmark loader requested GitHub HTML blob pages: {blob_requests}")
    for url in requested_remote_sources:
        parsed = urlsplit(url)
        _check(parsed.hostname in _BENCHMARK_REMOTE_HOSTS,
               f"benchmark override used an unexpected host: {url}")
        _check(parsed.hostname != "github.com" and "/blob/" not in parsed.path,
               f"benchmark override used a GitHub HTML blob page: {url}")


def _exercise_benchmark_failure(page, remote_state: dict[str, bool]) -> None:
    """Reload with valid HTTP JSON that has no recognised artifact shape."""
    remote_state["failure"] = True
    page.evaluate("() => window.__closeBioseqBenchmarkFetchGate()")
    page.get_by_role("button", name="Reload").click()
    _wait_for_benchmark_fetch_gate(page)
    page.evaluate("() => window.__releaseBioseqBenchmarkFetches()")
    failure = page.get_by_text("No benchmark data reachable", exact=True)
    try:
        failure.wait_for(state="visible", timeout=5000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"benchmark failure state did not render: {error}")
    _check(page.locator('[data-screen-label="BioseqDB benchmarks"] h1').count() == 0,
           "benchmark failure state retained a stale rendered model")
    remote_state["failure"] = False


def _exercise_responsive(page, label: str) -> None:
    """Responsive layout (§15.3/§20): no horizontal overflow and the key landmarks
    are present at each required visual width."""
    for width in (360, 768, 1024, 1440):
        page.set_viewport_size({"width": width, "height": 900})
        page.wait_for_timeout(120)
        if width == 360 and label not in ("landing", "benchmarks"):
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
        if label == "benchmarks":
            columns = page.locator("[data-bgrid]").evaluate(
                "node => getComputedStyle(node).gridTemplateColumns")
            nav_position = page.locator("[data-bnav]").evaluate(
                "node => getComputedStyle(node).position")
            control_columns = page.locator("[data-ctrls]").evaluate(
                "node => getComputedStyle(node).gridTemplateColumns")
            if width <= 1080:
                _check(nav_position == "static" and " " not in columns.strip(),
                       f"benchmarks: shell did not collapse at width {width}: "
                       f"nav={nav_position}, columns={columns}")
            else:
                _check(nav_position == "sticky" and " " in columns.strip(),
                       f"benchmarks: desktop shell lost its navigation rail at {width}")
            expected_control_columns = 1 if width <= 640 else (2 if width <= 760 else 4)
            _check(len(control_columns.split()) == expected_control_columns,
                   f"benchmarks: expected {expected_control_columns} filter columns at "
                   f"{width}, got {control_columns}")
        overflow = page.evaluate(
            "() => document.documentElement.scrollWidth - document.documentElement.clientWidth")
        _check(overflow <= 1, f"{label}: {overflow}px horizontal overflow at width {width}")
    _check(page.locator("header.site-header, header#nav, header#dnav, header#bhead").count() == 1,
           f"{label}: missing header landmark")
    _check(page.locator(
        "main#main, main#doc-main, "
        "[data-screen-label='BioseqDB benchmarks'] main").count() == 1,
           f"{label}: missing main landmark")
    _check(page.locator(
        "footer.site-footer, main#main footer, "
        "footer[data-screen-label='Docs · Footer'], "
        "[data-screen-label='BioseqDB benchmarks'] > footer").count() == 1,
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
        remote_state = {"failure": False}

        # The default benchmark bundle is served by the loopback fixture server,
        # exactly as a deployed same-origin static host serves it. Optional
        # public-raw override URLs remain offline-deterministic by receiving the
        # corresponding source-owned bytes here.
        def _guard(route):
            parsed = urlsplit(route.request.url)
            host = parsed.hostname or ""
            if host in _LOOPBACK_HOSTS or host == server._host:
                if (remote_state["failure"]
                        and parsed.path in
                        contract["benchmarks"]["local_source_routes"]):
                    route.fulfill(
                        status=200,
                        body=b"{}",
                        headers={
                            "cache-control": "no-store",
                            "content-type": "application/json; charset=utf-8",
                        },
                    )
                else:
                    route.continue_()
            elif route.request.url in contract["benchmarks"]["remote_sources"]:
                if remote_state["failure"]:
                    body = b"{}"
                else:
                    body = contract["benchmarks"]["remote_artifacts"][
                        route.request.url]
                route.fulfill(
                    status=200,
                    body=body,
                    headers={
                        "access-control-allow-origin": "*",
                        "cache-control": "no-store",
                        "content-type": "application/json; charset=utf-8",
                    },
                )
            else:
                route.abort()

        context.route("**/*", _guard)
        context.add_init_script(
            """(() => {
              const nativeFetch = window.fetch.bind(window);
              const gate = {open: false, pending: []};
              window.__bioseqBenchmarkFetchGate = gate;
              window.fetch = (input, init) => {
                const raw = input instanceof Request ? input.url : String(input);
                let url;
                try { url = new URL(raw, location.href); } catch (_) {
                  return nativeFetch(input, init);
                }
                const benchmarkJson =
                  url.pathname.endsWith('/benchmarks-manifest.json') ||
                  url.pathname.includes('/benchmark-data/') ||
                  url.hostname === 'raw.githubusercontent.com';
                if (!benchmarkJson || gate.open) return nativeFetch(input, init);
                return new Promise((resolve, reject) => {
                  gate.pending.push(() => nativeFetch(input, init).then(resolve, reject));
                });
              };
              window.__releaseBioseqBenchmarkFetches = () => {
                gate.open = true;
                const queued = gate.pending.splice(0);
                queued.forEach((start) => start());
              };
              window.__closeBioseqBenchmarkFetchGate = () => { gate.open = false; };
            })();""")
        page = context.new_page()
        request_urls: list[str] = []
        page.on("request", lambda request: request_urls.append(request.url))

        console_errors: list[str] = []
        page.on("console", lambda msg: console_errors.append(f"{msg.type}: {msg.text}")
                if msg.type == "error" else None)
        page.on("pageerror", lambda exc: console_errors.append(f"pageerror: {exc}"))

        for label, route in routes.items():
            url = server.url_for(route)
            if verbose:
                print(f"  navigating {label}: {url}")
            wait_until = "domcontentloaded" if label == "benchmarks" else "networkidle"
            response = page.goto(url, wait_until=wait_until)
            _check(response is not None, f"{label}: no response from {url}")
            _check(response.ok, f"{label}: HTTP {response.status} for {url}")
            _assert_headers(response, expected_headers)

            if label == "benchmarks":
                loading = page.get_by_text("Fetching benchmark artifacts…", exact=True)
                try:
                    loading.wait_for(state="visible", timeout=3000)
                except Exception as error:  # noqa: BLE001
                    raise AssertionFail(
                        f"benchmark loading state was never observable: {error}")
                _wait_for_benchmark_fetch_gate(page)
                _release_benchmark_fetches(page)
                page.wait_for_load_state("networkidle")

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
            if label in {
                "tutorial_quick", "tutorial_columnar", "tutorial_generation",
                "tutorial_io", "tutorial_index", "tutorial_slice",
                "tutorial_columns", "tutorial_remote", "tutorial_explain",
            }:
                _assert_literal_design_structure(page, label, contract["design"])
                _exercise_design_style_parity(page, label, contract["design"])
                _assert_tutorial_sidebar(page, base_path, version)
                tutorial_slug = {
                    "tutorial_quick": "quick_read_search_write",
                    "tutorial_columnar": "columnar_storage_and_vectorized_aggregates",
                    "tutorial_generation": "sequence_generation",
                    "tutorial_io": "fasta_fastq_gzip_and_io",
                    "tutorial_index": "index_lifecycle_and_metadata",
                    "tutorial_slice": "slice_concat_reverse",
                    "tutorial_columns": "multiple_sequence_columns_and_joins",
                    "tutorial_remote": "remote_provider_to_search",
                    "tutorial_explain": "reading_bioseq_explain",
                }[label]
                expected_code_entities = {
                    "tutorial_columnar": {
                        "sql:type:bioseq.seq",
                        "sql:function:bioseq.len(bioseq.seq)",
                    },
                    "tutorial_generation": {
                        "sql:type:bioseq.seq",
                        "sql:function:bioseq.len(bioseq.seq)",
                        "sql:function:bioseq.symbol_count(bioseq.seq,text)",
                    },
                    "tutorial_io": {
                        "sql:type:bioseq.seq",
                        "sql:function:bioseq.len(bioseq.seq)",
                        "sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)",
                        "sql:function:bioseq.target_start(anyelement)",
                        "sql:function:bioseq.target_end(anyelement)",
                    },
                    "tutorial_index": {
                        "sql:type:bioseq.seq",
                        "sql:function:$extension.mmsearch("
                        "bioseq.seq,bioseq.seq,$extension.bioseq_aligner_options)",
                        "sql:function:bioseq.target_start(anyelement)",
                        "sql:function:bioseq.target_end(anyelement)",
                    },
                }.get(label)
                if expected_code_entities is not None:
                    _exercise_code_entities(page, expected_code_entities)
                _exercise_tutorial_results(page, contract["tutorials"][tutorial_slug])
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
            if label == "benchmarks":
                _assert_literal_design_structure(page, label, contract["design"])
                _exercise_design_style_parity(page, label, contract["design"])
                _exercise_benchmarks(
                    page, request_urls, contract, base_path, version)
            _exercise_responsive(page, label)
            if label == "tutorial_similarity":
                _exercise_prism_pixel_contract(page, contract["design"], artifact_dir)
            if label == "benchmarks":
                _exercise_benchmark_failure(page, remote_state)

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
                        for name in (
                            "index.html", "docs.html", "benchmarks.html",
                            "bench-loader.js", "benchmarks-manifest.json",
                            "support.js",
                        )
                    },
                    "benchmark_source_sha256":
                        contract["benchmarks"]["local_source_sha256"],
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
