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
    "getting_started",
    "project_introduction",
    "project_biology",
    "project_alignment_foundations",
    "project_minimizers",
    "project_chaining",
    "project_final_alignment_postprocessing_and_preprocessing",
    "project_sequence_abstractions",
    "project_postgres_custom_scan",
    "project_postgres_storage",
    "project_index_storage",
    "project_benchmarking",
    "project_conclusion",
    "project_configuration_reference",
    "bibliography",
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
_CODE_PALETTE = {
    "comment": "#8094a6",
    "keyword": "#65b9dd",
    "function": "#f39762",
    "string": "#ddb67d",
    "type": "#8bd0b3",
    "builtin": "#9bb3c7",
    "number": "#e2c478",
    "macro": "#c5a8d6",
    "operator": "#94aabf",
    "plain": "#cbd8e4",
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
        _check(re.search(r"\b(SELECT|SHOW|EXPLAIN|INSERT|UPDATE|DELETE|CREATE|ALTER|DROP|SET|RESET|CALL|DO)\b", visible,
                         re.IGNORECASE) is not None,
               f"{path}:{body_start}: visible phase has no user-facing SQL operation")

        created_tables = re.findall(
            r"\bCREATE\s+(?:TEMP(?:ORARY)?\s+)?TABLE\s+([a-z_][a-z0-9_]*)",
            setup + "\n" + visible, re.IGNORECASE)
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
    """The published tutorial and chapters are the current authored sources."""
    tutorial_root = _WEBSITE_ROOT / version / "tutorials"
    _check({path.stem for path in tutorial_root.glob("*.md")} == set(_EXPECTED_TUTORIALS),
           "authored tutorial inventory differs from the README/project chapter set")
    all_ids: list[str] = []
    sources: dict[str, str] = {}
    for slug in _EXPECTED_TUTORIALS:
        path = tutorial_root / f"{slug}.md"
        _check(path.is_file(), f"tutorial source is missing: {path}")
        all_ids.extend(_parse_authored_runnable_fences(path))
        source = path.read_text(encoding="utf-8")
        sources[slug] = source
        for body in re.findall(r"```sql runnable[^\n]*\n(.*?)\n```", source, re.S):
            _check("bioseq_internal." not in body
                   and "inspect_ready_minimizer_generation" not in body,
                   f"{slug} runnable SQL exposes a private or revoked interface")
        _check("-- bioseq:expected" not in source,
               f"{slug} contains authored result rows instead of captured PostgreSQL output")
    _check(len(all_ids) >= 30 and len(all_ids) == len(set(all_ids)),
           "README/project runnable examples are missing or repeat an id")
    _check("readme_docker_demo" in all_ids,
           "README Docker quick-start example is missing")
    for token in ("Docker quick start", "Packages and Docker images", "bioseq.load", "mmsearch"):
        _check(token in sources["getting_started"], f"README tutorial lost {token!r}")
    bibliography_ids = set(re.findall(r'<a id="(ref-[^"]+)"', sources["bibliography"]))
    _check(len(bibliography_ids) >= 100, "project bibliography lost its references")
    citations = re.findall(r'\.\./bibliography/#(ref-[a-zA-Z0-9_-]+)', "\n".join(sources.values()))
    _check(len(citations) >= 100 and set(citations) <= bibliography_ids,
           "project citation links do not all resolve to the bibliography")
    figure_manifest = json.loads(
        (_WEBSITE_ROOT / "static/figures/project/manifest.json").read_text())
    figures = figure_manifest["figures"]
    _check(len(figures) == 74, "original figure inventory is incomplete")
    actual_figures = set(re.findall(r'/static/(figures/project/[^)\s]+\.svg)',
                                    "\n".join(sources.values())))
    _check(actual_figures == {item["svg"] for item in figures},
           "authored chapters do not embed every exported original figure")
    _check(figure_manifest.get("theme", {}).get("background") == "transparent",
           "project SVG exports lost their transparent website theme")
    for item in figures:
        path = _WEBSITE_ROOT / "static" / item["svg"]
        _check(hashlib.sha256(path.read_bytes()).hexdigest() == item.get("svg_sha256"),
               f"figure palette manifest is stale for {path.name}")
    algorithms = [body for source in sources.values()
                  for body in re.findall(r'^```pseudocode\n(.*?)^```[ \t]*$',
                                         source, re.M | re.S)]
    _check(len(algorithms) == 19 and sum(len(body.splitlines()) for body in algorithms) == 605,
           "project algorithm inventory lost original listings or statements")
    provenance = json.loads((_WEBSITE_ROOT / "project_import_manifest.json").read_text())
    _check(provenance.get("entrypoint") == "papers/src/content/bioseqdb/thesis/main.tex",
           "chapter provenance lost the original main.tex entry point")
    source_root = _REPOSITORY_ROOT / "papers/src/content/bioseqdb/thesis"
    for item in provenance.get("inputs", []):
        path = source_root / item["path"]
        _check(path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == item["sha256"],
               f"chapter source manifest is stale for {path}")
    _check(len(provenance.get("inputs", [])) >= 60,
           "chapter source manifest lost recursive includes/partials")
    _parse_authored_runnable_fences(_WEBSITE_ROOT / version / "landing_examples.md")


def _load_benchmark_fixture_contract(site: Path, base_path: str) -> dict:
    """Bind browser comparisons to the same approved cells as the chapter."""
    paths = sorted(site.rglob("project-benchmarks.json"))
    _check(len(paths) == 1, f"expected one published benchmark dataset, got {paths}")
    path = paths[0]
    payload = path.read_bytes()
    source = _WEBSITE_ROOT / "static/project-benchmarks.json"
    _check(payload == source.read_bytes(), "published benchmark data differs from source")
    document = json.loads(payload)
    _check(document.get("schema") == "bioseq.project-benchmarks.v1",
           "unsupported project benchmark schema")
    tables = document.get("tables", {})
    _check(len(tables) == 10, "chapter benchmark source panels were omitted")
    cells = [cell for table in tables.values() for row in table["rows"]
             for cell in row["cells"].values()]
    _check(len(cells) == document["counts"]["cells"], "benchmark cell count drift")
    _check(sum(cell["a_ns"] is not None for cell in cells)
           == document["counts"]["measured"], "benchmark measurement count drift")
    for cell in cells:
        _check((cell["a_ns"] is None) == (cell["b_ns"] is None),
               "benchmark cell contains only one of its A/B measurements")
        if cell["a_ns"] is not None:
            _check(cell["a_ns"] > 0 and cell["b_ns"] > 0
                   and cell["samples"] >= 1, "invalid published benchmark timing")
    results_root = _REPOSITORY_ROOT / "research_benchmarks/paper/e2e"
    for name, expected in document["sources"].items():
        _check(Path(name).name == name, "benchmark source must be a filename")
        _check(hashlib.sha256((results_root / name).read_bytes()).hexdigest() == expected,
               f"published benchmark snapshot is stale: {name}")
    key = max(tables, key=lambda k: len(tables[k]["rows"]) * len(tables[k]["columns"]))
    route = _public_route(base_path, path.relative_to(site).as_posix())
    return {
        "key": key, "title": tables[key]["title"], "tables": tables,
        "benchmark_count": len(tables), "manifest_route": route,
        "local_source_routes": {route}, "remote_sources": set(),
        "local_source_sha256": {
            path.relative_to(site).as_posix(): hashlib.sha256(payload).hexdigest(),
        },
        "remote_artifacts": {}, "same_origin": True,
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
        authored_ids = _parse_authored_runnable_fences(
            _WEBSITE_ROOT / version / "tutorials" / f"{slug}.md")
        _check([example.get("local_id") for example in examples] == authored_ids,
               f"{slug}: resolved runnable examples differ from the authored fences")
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
                # Earlier statements may return rows before the expected error.
                errors = [node for node in _nodes_of_kind(result_nodes, "admonition")
                          if node.get("attributes", {}).get("class") == "warning"]
                _check(bool(sqlstate) and any(
                    f"SQLSTATE {sqlstate}:" in _node_text(node) for node in errors),
                       f"{canonical_id}: expected error output lost SQLSTATE {sqlstate}")
            else:
                # Preserve paragraph boundaries: a command-only result is
                # followed immediately by its separate provenance paragraph.
                output_text = "\n".join(_node_text(node) for node in result_nodes)
                _check(bool(tables) or re.search(
                    r"\b(?:CREATE (?:TABLE|EXTENSION)|ALTER TABLE|DROP TABLE|"
                    r"INSERT \d+ \d+|UPDATE \d+|DELETE \d+|SET|RESET|CALL|DO)\b", output_text),
                    f"{canonical_id}: successful SQL has neither rows nor a command status")
            for table in tables:
                _check(len(table) >= 1 and bool(table[0])
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

    # Captured rows and provenance are checked for every runnable block above;
    # theoretical SQL/EXPLAIN samples remain ordinary authored code blocks.
    readme_sql = "\n".join(example["visible_sql"]
                            for example in by_slug["getting_started"]["sql_examples"])
    for token in ("CREATE TABLE", "bioseq.load", "bioseq.update_index", "mmsearch"):
        _check(token in readme_sql, f"README runnable tutorial lost {token!r}")
    for slug in ("project_introduction", "project_sequence_abstractions",
                 "project_postgres_custom_scan", "project_index_storage"):
        _check(bool(by_slug[slug].get("sql_examples")),
               f"{slug}: practical project SQL lost its runnable embedding")

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

    # Landing slots use the original README SQL and the same isolated fixtures.
    filter_sql = landing["similarity_search"]["visible_sql"]
    for spelling in ("FROM samples AS t JOIN samples AS q", "mmsearch",
                     "bioseq.target_start", "bioseq.query_start", "bioseq.cigar"):
        _check(spelling in filter_sql,
               f"landing sequence-join example lost {spelling!r}")
    _check([column.get("name") for column in
            landing["similarity_search"]["result"].get("columns", [])]
           == ["target_id", "query_id", "target_start", "target_end",
               "query_start", "query_end", "score", "cigar"],
           "landing sequence join did not publish its final SELECT result")
    load_sql = landing["sequence_storage"]["visible_sql"]
    for spelling in ("CREATE TABLE imported", "bioseq.load", "readme_samples.fasta"):
        _check(spelling in load_sql,
               f"landing README load example lost {spelling!r}")
    _check([column.get("name") for column in
            landing["sequence_storage"]["result"].get("columns", [])] == ["load"],
           "landing load example did not publish the loader's actual result")

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
        **{f"tutorial:{slug}": f"{base}/{version}/tutorials/{slug}/"
           for slug in _EXPECTED_TUTORIALS},
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

    # Freeze the reveal frame so Escape deterministically lands after data has
    # loaded but before the animation runs. A stale frame must not reopen it.
    pending = ref.evaluate("""async anchor => {
        const original = window.requestAnimationFrame;
        const frames = [];
        window.requestAnimationFrame = callback => { frames.push(callback); return 0; };
        try {
            anchor.blur();
            anchor.focus();
            await Promise.resolve();
            await Promise.resolve();
            const tooltip = document.getElementById('entity-pop');
            const armed = tooltip.getAttribute('aria-hidden') === 'false'
                && anchor.getAttribute('aria-describedby') === tooltip.id;
            document.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape', bubbles: true}));
            frames.forEach(callback => callback(performance.now()));
            return {armed, frames: frames.length, opacity: tooltip.style.opacity,
                    hidden: tooltip.getAttribute('aria-hidden'),
                    describedBy: anchor.getAttribute('aria-describedby')};
        } finally {
            window.requestAnimationFrame = original;
        }
    }""")
    _check(pending["armed"] and pending["frames"] > 0,
           "entity tooltip regression did not intercept an active reveal frame")
    _check(pending["opacity"] == "0" and pending["hidden"] == "true"
           and not pending["describedBy"],
           "a queued entity tooltip animation reopened the tooltip after Escape")
    page.locator("#entity-pop").wait_for(state="hidden", timeout=3000)


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


def _exercise_code_entities(page, expected_ids: set[str]) -> None:
    """Every catalog range survives syntax highlighting as one exact hover link."""
    blocks = page.locator("code[data-entity-ranges]")
    _check(blocks.count() > 0, f"{page.url}: SQL samples have no entity ranges")
    ids = set()
    for index in range(blocks.count()):
        code = blocks.nth(index)
        metadata = json.loads(code.get_attribute("data-entity-ranges") or "[]")
        refs = code.locator("a.entity-ref[data-entity-id]")
        _check(refs.count() == len(metadata), "SQL entity range was lost or duplicated")
        text = code.text_content() or ""
        for item in metadata:
            # Generator offsets follow browser UTF-16 text ranges, not UTF-8 bytes.
            encoded = text.encode("utf-16-le")
            spelling = encoded[item["start"] * 2:(item["start"] + item["len"]) * 2].decode("utf-16-le")
            matches = refs.filter(has_text=spelling)
            _check(matches.count() > 0, f"SQL reference lost original spelling {spelling!r}")
            ids.add(item["id"])
        _check(refs.evaluate_all("refs => refs.every(ref => ref.href && ref.dataset.entityDataUrl)"),
               "SQL reference missing route or hover dataset")
    _check(expected_ids <= ids,
           f"required SQL references missing: {sorted(expected_ids - ids)}")
    for spelling in ("bioseq.seq", "bioseq.score", "mmsearch"):
        refs = page.locator("code a.entity-ref").filter(has_text=spelling)
        _check(refs.count() > 0, f"automatic SQL references missing {spelling}")
        refs.first.focus()
        page.wait_for_function("""spelling => {
            const pop = document.getElementById('entity-pop');
            return pop.getAttribute('aria-hidden') === 'false' &&
                pop.querySelector('.entity-pop-head pre')?.textContent.includes(spelling);
        }""", arg=spelling)
        _check(bool(page.locator("#entity-pop .entity-pop-body").inner_text().strip()),
               f"empty SQL tooltip for {spelling}")
        page.keyboard.press("Escape")
        page.wait_for_function("document.getElementById('entity-pop').getAttribute('aria-hidden') === 'true'")


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
    inp.fill("PostgreSQL API")
    results = dropdown.locator("a[href]")
    try:
        results.first.wait_for(state="visible", timeout=4000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"docs search produced no visible PostgreSQL API results: {error}")
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
    """The common header remains legible and reachable while the page scrolls."""
    nav = page.locator("header#dnav.site-header")
    _check(nav.count() == 1, "shared navigation header is missing")
    page.evaluate("""() => {
      document.documentElement.style.setProperty('scroll-behavior', 'auto', 'important');
      window.scrollTo(0, 120);
    }""")
    page.wait_for_timeout(40)
    state = nav.evaluate("""node => ({top: node.getBoundingClientRect().top,
      background: getComputedStyle(node).backgroundColor,
      position: getComputedStyle(node).position})""")
    _check(abs(state["top"]) <= 1 and state["position"] == "sticky",
           f"shared header did not stay at the viewport top: {state}")
    _check(state["background"] == "rgba(7, 19, 32, 0.86)",
           "shared header lost its readable design background")
    page.evaluate("""() => {
      window.scrollTo(0, 0);
      document.documentElement.style.removeProperty('scroll-behavior');
    }""")


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
        _check(len(expected_table) >= 1 and bool(expected_table[0]),
               f"{tutorial['slug']}: captured result table has no column header")
        expected_columns = len(expected_table[0]) if expected_table else 0
        type_labels = tables.nth(index).locator("thead th code")
        _check(type_labels.count() == expected_columns
               and all(bool(label.strip()) for label in type_labels.all_text_contents()),
               f"{tutorial['slug']}: result table omits PostgreSQL column type labels")
        _check(tables.nth(index).evaluate(
            "table => table.classList.contains('result-table-wide')") == (expected_columns > 4),
            f"{tutorial['slug']}: wide SQL result classification differs from its column count")
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
    _assert_content_scroll_layout(page, tutorial["slug"])


def _assert_content_scroll_layout(page, label: str) -> None:
    """Wide content keeps readable columns/lines inside a keyboard-scrollable region."""
    _check(page.locator("table.result-table, .algorithm-lines").evaluate_all(
        "nodes => nodes.every(node => node.parentElement.matches('.result-scroll, .algorithm-scroll'))"),
        f"{label}: results or algorithms lack their local scroll wrapper")
    regions = page.locator(".result-scroll, .algorithm-scroll")
    states = regions.evaluate_all("""nodes => nodes.map(node => {
        const oldLeft = node.scrollLeft;
        node.scrollLeft = node.scrollWidth;
        const canScroll = node.scrollLeft > 0;
        node.scrollLeft = oldLeft;
        return {overflow: getComputedStyle(node).overflowX,
                tabindex: node.tabIndex, role: node.getAttribute('role'),
                label: node.getAttribute('aria-label'),
                client: node.clientWidth, scroll: node.scrollWidth, canScroll};
    })""")
    for state in states:
        _check(state["overflow"] == "auto" and state["tabindex"] == 0
               and state["role"] == "region" and state["label"],
               f"{label}: wide content lost its accessible scroll container: {state}")
        if state["scroll"] > state["client"] + 1:
            _check(state["canScroll"], f"{label}: overflowing content cannot scroll locally")
    widths = page.locator(".result-table-wide th, .result-table-wide td").evaluate_all(
        """cells => cells.map(cell => ({width: cell.getBoundingClientRect().width,
            wrap: getComputedStyle(cell).overflowWrap,
            break: getComputedStyle(cell).wordBreak}))""")
    _check(all(cell["width"] >= 143 and cell["wrap"] == "normal"
               and cell["break"] == "normal" for cell in widths),
           f"{label}: wide SQL result columns collapse or break one character at a time")
    _check(page.locator(".algorithm-line-body").evaluate_all(
        "nodes => nodes.every(node => getComputedStyle(node).whiteSpace === 'pre')"),
        f"{label}: algorithm indentation or line spacing collapses")


def _exercise_algorithms(page, source: str, slug: str) -> None:
    expected = re.findall(r'^```pseudocode\n(.*?)^```[ \t]*$', source, re.M | re.S)
    panels = page.locator("main#doc-main .algorithm-panel")
    _check(panels.count() == len(expected),
           f"{slug}: original algorithms were omitted or rendered as ordinary code")
    for index, body in enumerate(expected):
        panel = panels.nth(index)
        lines = body.splitlines()
        rendered = panel.locator(".algorithm-line")
        _check(rendered.count() == len(lines),
               f"{slug}: algorithm {index + 1} lost source statements")
        state = panel.evaluate("""panel => ({
            radius: parseFloat(getComputedStyle(panel).borderRadius),
            lines: [...panel.querySelectorAll('.algorithm-line')].map(line => ({
                number: line.dataset.line,
                gutter: getComputedStyle(line, '::before').content,
                indent: line.querySelector('.algorithm-line-body').textContent.match(/^ */)[0].length,
            }))})""")
        _check(state["radius"] >= 10, f"{slug}: algorithm {index + 1} lost its rounded panel")
        for number, (original, actual) in enumerate(zip(lines, state["lines"]), 1):
            _check(actual["number"] == str(number) and actual["gutter"].strip('"') == str(number),
                   f"{slug}: algorithm {index + 1} line numbers are missing or out of order")
            indent = len(original) - len(original.lstrip(" "))
            _check(actual["indent"] == indent,
                   f"{slug}: algorithm {index + 1}, line {number} lost nesting indentation")
        original_math = [value.strip() for value in re.findall(r'\$([^$\n]+)\$', body)]
        rendered_math = [value.strip() for value in panel.locator(
            '.project-math annotation[encoding="application/x-tex"]').all_text_contents()]
        _check(rendered_math == original_math,
               f"{slug}: algorithm {index + 1} changed or omitted a mathematical expression")
        keywords = panel.locator(".algorithm-keyword")
        _check(keywords.count() > 0, f"{slug}: algorithm {index + 1} lacks control-flow highlighting")
        _check(keywords.evaluate_all(
            "nodes => nodes.every(node => getComputedStyle(node).color === 'rgb(244, 137, 79)')"),
            f"{slug}: algorithm {index + 1} lost its website keyword palette")
        _check(panel.locator(".algorithm-line-body code").evaluate_all("""nodes => nodes.every(node => {
            const style = getComputedStyle(node);
            const line = getComputedStyle(node.closest('.algorithm-line-body'));
            return style.color === 'rgb(143, 214, 186)' && style.fontSize === line.fontSize;
        })"""), f"{slug}: algorithm {index + 1} call names lost their palette or inherited size")


def _exercise_figure_viewer(page) -> None:
    """Transparent SVGs remain readable and zoomable on a dark modal surface."""
    original_viewport = page.viewport_size
    original_url = page.url
    link = page.locator("main#doc-main a.project-figure-link").first
    _check(link.count() == 1, "project diagram has no full-size viewer link")
    target = link.evaluate("node => node.href")
    alt = link.locator("img").get_attribute("alt")
    for width in (1280, 360):
        page.set_viewport_size({"width": width, "height": 900})
        link.focus()
        link.locator("img").click()
        dialog = page.locator("dialog.figure-viewer[open]")
        dialog.wait_for(state="visible", timeout=3000)
        image = dialog.locator(".figure-viewer-scroll img")
        image.evaluate("node => node.decode()")
        _check(page.url == original_url and image.evaluate("node => node.src") == target,
               "full-size viewer changed the SVG URL or navigated away from its chapter")
        _check(image.get_attribute("alt") == alt
               and dialog.get_attribute("aria-label") == "Full-size diagram",
               "full-size viewer lost the accessible diagram description")
        download = dialog.locator("a[download]")
        _check(download.count() == 1 and download.evaluate("node => node.href") == target,
               "full-size viewer does not offer the original SVG download")
        state = dialog.evaluate("""node => {
            const box = node.getBoundingClientRect();
            const scroll = node.querySelector('.figure-viewer-scroll');
            const image = scroll.querySelector('img');
            scroll.scrollLeft = scroll.scrollWidth;
            const moved = scroll.scrollLeft > 0;
            scroll.scrollLeft = 0;
            return {dark: getComputedStyle(node).backgroundColor === 'rgb(7, 19, 32)',
                    contained: box.left >= 0 && box.top >= 0
                        && box.right <= innerWidth + 1 && box.bottom <= innerHeight + 1,
                    imageWidth: image.getBoundingClientRect().width,
                    overflow: getComputedStyle(scroll).overflowX, moved};
        }""")
        _check(state["dark"] and state["contained"] and state["imageWidth"] >= 959,
               f"full-size viewer lost its dark, enlarged, viewport-contained presentation at {width}px")
        if width == 360:
            _check(state["overflow"] == "auto" and state["moved"],
                   "mobile full-size diagram cannot scroll horizontally")
        page.keyboard.press("Escape")
        dialog.wait_for(state="hidden", timeout=3000)
        page.wait_for_function(
            "link => document.activeElement === link", arg=link.element_handle(), timeout=3000)
        _check(page.locator("dialog.figure-viewer").count() == 0,
               "closing the diagram left an orphaned modal")
    if original_viewport:
        page.set_viewport_size(original_viewport)


def _exercise_project_content(page, tutorial: dict, base_path: str, version: str) -> None:
    """Original diagrams, mathematics, bibliography, and source links survive rendering."""
    slug = tutorial["slug"]
    header = page.locator(".design-page-header")
    _check(header.locator("h1").inner_text() == tutorial["title"],
           f"{slug}: authored chapter title changed")
    description = tutorial.get("description", "")
    if description == tutorial["title"]:
        _check(header.locator("p").count() == 0,
               f"{slug}: chapter heading is repeated as its description")
    else:
        _check(header.locator("p").inner_text() == description,
               f"{slug}: distinct authored chapter description was lost")
    prose_cells = page.locator(".tutorial-body > table th, .tutorial-body > table td")
    _check(prose_cells.evaluate_all(
        "cells => cells.every(cell => cell.getBoundingClientRect().width >= 128 "
        "&& getComputedStyle(cell).overflowWrap === 'normal')"),
        f"{slug}: general table columns became too narrow to read")
    source = (_WEBSITE_ROOT / version / "tutorials" / f"{slug}.md").read_text()
    expected_figures = re.findall(r'/static/figures/project/[^)\s]+\.svg', source)
    figures = page.locator('main#doc-main img[src*="/figures/project/"]')
    _check(figures.count() == len(expected_figures),
           f"{slug}: original figure count changed during rendering")
    loaded = figures.evaluate_all("""async images => {
        images.forEach(image => { image.loading = 'eager'; });
        return await Promise.all(images.map(async image => {
            try { await image.decode(); return true; }
            catch { return false; }
        }));
    }""")
    _check(all(loaded), f"{slug}: an original SVG failed to decode")
    _check(figures.evaluate_all("images => images.every(image => image.complete && image.naturalWidth > 0)"),
           f"{slug}: an original SVG failed to load")
    if expected_figures:
        _check(figures.evaluate_all(
            """images => images.every(image => {
                const style = getComputedStyle(image);
                return style.backgroundColor === 'rgba(0, 0, 0, 0)' && style.padding === '0px'
                    && Math.abs(image.getBoundingClientRect().width
                        - image.closest('p').getBoundingClientRect().width) <= 1;
            })"""),
            f"{slug}: themed figures lost their transparent full-column presentation")
    _exercise_algorithms(page, source, slug)
    bibliography = _public_route(base_path, f"{version}/tutorials/bibliography/")
    citations = page.locator('main#doc-main a[href*="#ref-"]')
    for index in range(citations.count()):
        href = citations.nth(index).get_attribute("href") or ""
        _check(urlsplit(urljoin(page.url, href)).path == bibliography,
               f"{slug}: citation no longer points to the bibliography: {href}")
    _check(page.locator("main#doc-main .katex-error").count() == 0,
           f"{slug}: mathematical content contains a KaTeX parse error")
    math = page.locator("main#doc-main [data-math]")
    _check(math.evaluate_all("nodes => nodes.every(node => node.querySelector('math'))"),
           f"{slug}: project mathematics was left as raw LaTeX")
    if slug == "project_biology":
        _check(math.count() > 0, "biology formulas were not rendered")
        _exercise_figure_viewer(page)
    if slug == "project_postgres_custom_scan":
        links = page.locator('main#doc-main a[href^="https://github.com/"][href*="/blob/"]')
        _check(links.count() >= 5, "original PostgreSQL source references were lost")
        _check(links.evaluate_all(
            "links => links.every(link => link.matches('.source-link') && link.querySelector('svg.github-mark'))"),
            "original PostgreSQL source references lost their GitHub icons")
    if slug == "getting_started":
        plans = page.locator("main#doc-main code.language-pgsql-plan")
        _check(plans.count() > 0 and plans.locator(".token.plan-node").count() > 0,
               "authored EXPLAIN examples lost plan-specific highlighting")
    if slug == "bibliography":
        _check(page.locator('main#doc-main [id^="ref-"]').count() >= 100,
               "bibliography reference targets were lost")
    if slug == "project_benchmarking":
        chapter_text = page.locator("main#doc-main").inner_text()
        for value in ("7.68 cores", "120.8 GiB", "32,607 MiB", "595.84",
                      "1.4.329", "500.28 GB", "6,000 IOPS / 500 MiB/s"):
            _check(value in chapter_text,
                   f"benchmark machine specification lost its original value: {value}")
    if slug == "project_configuration_reference":
        values = page.locator("main#doc-main td").all_inner_texts()
        _check("1: require_absent" in values
               and "2: require_compatible_persisted" in values,
               "index-read policy table lost its original numeric option codes")


def _assert_landing_routes(page, base_path: str, version: str) -> None:
    first = _public_route(
        base_path, f"{version}/tutorials/{_EXPECTED_TUTORIALS[0]}/")
    read_links = page.locator("a", has_text="Read the docs")
    _check(read_links.count() == 2,
           f"landing has two content Read the docs links below its common header, got {read_links.count()}")
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
            ("id:annotate", "#annotate"),
            ("id:workflow", "#workflow"), ("id:cta", "#cta"),
        )
    elif label == "benchmarks":
        source = design["benchmarks"]
        prefixes = ("bd-s", "bm-s", "bench-")
        pairs = (
            ("data-screen-label:BioseqDB benchmarks",
             '[data-screen-label="BioseqDB benchmarks"]'),
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
            ("data-bodygrid:first", "[data-bodygrid]"),
            ("id:sidebar", "#sidebar"),
            ("data-rightrail:first", "[data-rightrail]"),
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
    # The requested common header intentionally replaces the three prototypes;
    # all page-specific content retains its original design declarations.
    _compare_design_style(
        page, "#dnav", design["docs"]["id:dnav"],
        ("position", "top", "zIndex", "backgroundColor", "borderBottomWidth",
         "borderBottomColor", "backdropFilter"), "shared navigation")
    if label == "landing":
        records = design["landing"]
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
        code_reference = dict(records["data-code:first"])
        code_reference["style"] += (
            ";font-family:'IBM Plex Mono',monospace;font-size:13px;line-height:1.65;color:#cbd8e4")
        _compare_design_style(
            page, ".doc-code:not(:has(.anno-cards)) code", code_reference,
            ("fontFamily", "fontSize", "lineHeight", "color"),
            "Markdown code typography")


def _exercise_prism_pixel_contract(page, design: dict, artifact_dir: Path) -> None:
    """Pixel-compare the live Prism component with the same DOM restyled to the
    supplied terminal geometry and explicit design token palette."""
    page.set_viewport_size({"width": 1280, "height": 900})
    page.evaluate("() => document.fonts.ready")
    required_tokens = ("keyword", "function", "string", "operator", "punctuation")
    # The page can start with a short SET statement. Select a real SQL sample
    # containing the complete palette instead of depending on fence order.
    selector = ".doc-code:not(:has(.anno-cards)) pre:has(code.language-sql)"
    selector += "".join(f":has(.token.{kind})" for kind in required_tokens)
    pre = page.locator(selector).first
    _check(pre.count() == 1,
           "Prism pixel contract has no SQL component with the complete token palette")
    pre.scroll_into_view_if_needed()
    code = pre.locator("code.language-sql")
    _check(code.count() == 1, "Prism pixel contract requires a highlighted SQL fence")
    for token_class in required_tokens:
        _check(code.locator(f".token.{token_class}").count() > 0,
               f"Prism SQL sample has no {token_class} token to validate")

    artifact_dir.mkdir(parents=True, exist_ok=True)
    actual_path = artifact_dir / "prism-live.png"
    reference_path = artifact_dir / "prism-design-reference.png"
    actual = pre.screenshot(path=str(actual_path), animations="disabled")

    docs = design["docs"]
    pre.evaluate(
        """(pre, {termStyle, preStyle, palette}) => {
          const figure = pre.closest('.doc-code');
          const code = pre.querySelector('code.language-sql');
          figure.style.cssText = termStyle;
          pre.style.cssText = preStyle;
          pre.style.fontSize = '13px';
          pre.style.lineHeight = '1.65';
          code.style.fontFamily = "'IBM Plex Mono'";
          code.style.fontSize = '13px';
          code.style.lineHeight = '1.65';
          code.style.color = palette.plain;
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
          paint('.token.function,.token.function-name,.token.predicate', palette.function, '500');
          paint('.token.class-name,.token.type,.token.namespace', palette.type);
          paint('.token.builtin', palette.builtin);
          paint('.token.string,.token.char,.token.regex', palette.string);
          paint('.token.macro,.token.directive,.token.property', palette.macro);
          paint('.token.number,.token.boolean', palette.number);
          paint('.token.operator,.token.punctuation', palette.operator);
        }""",
        {"termStyle": docs["data-term:first"]["style"],
         "preStyle": docs["data-code:first"]["style"],
         "palette": _CODE_PALETTE},
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
    """Check same-column A/B ratios, controls and accessible design tooltips."""
    benchmark = contract["benchmarks"]
    panel = benchmark["tables"][benchmark["key"]]
    _check(page.locator("h1").inner_text().strip() == benchmark["title"],
           "benchmark heading differs from the published panel")
    _check(f"{benchmark['benchmark_count']} benchmarks" in
           page.locator("#bhead").inner_text(), "benchmark status count is stale")
    for label, expected in {
        "Docs": _public_route(base_path, f"{version}/tutorials/{_EXPECTED_TUTORIALS[0]}/"),
        "Benchmarks": _public_route(base_path, "benchmarks/"),
    }.items():
        link = page.locator("#dnav nav a", has_text=label)
        _check(link.count() == 1 and urlsplit(link.get_attribute("href")).path == expected,
               f"benchmark {label} navigation lost the configured mount")
    controls = page.locator("[data-ctrls] select")
    _check(controls.count() == 4, "benchmark comparison filters are missing")
    _check(controls.nth(1).locator("option").count() == 1
           and controls.nth(1).input_value() == "P50",
           "published medians must not invent percentile distributions")
    preferred = next(row for row in panel["rows"] if row["preferred"])
    baseline = next(row for row in panel["rows"] if row["id"] == "minimap2")
    column_index = next(i for i, column in enumerate(panel["columns"])
                        if preferred["cells"][column["id"]]["a_ns"] is not None
                        and baseline["cells"][column["id"]]["a_ns"] is not None)
    column = panel["columns"][column_index]["id"]
    row = page.locator("tbody tr").filter(
        has=page.get_by_text(preferred["label"], exact=True))
    cell = row.locator(".bd-s0072").nth(column_index)
    cell.hover()
    tooltip = page.locator("[data-benchmark-tip]")
    page.wait_for_function("() => Number(getComputedStyle(document.querySelector('[data-benchmark-tip]')).opacity) > 0")
    expected_ratio = baseline["cells"][column]["a_ns"] / preferred["cells"][column]["a_ns"]
    _check(f"{expected_ratio:.3f}×" in tooltip.inner_text(),
           "hover tooltip compares A against the wrong baseline/workload")
    _check(preferred["cells"][column]["source"] in tooltip.inner_text(),
           "benchmark tooltip lost source provenance")
    cell.focus()
    _check("baseline value" in tooltip.inner_text(),
           "keyboard focus did not reveal the comparison tooltip")
    page.keyboard.press("Escape")
    page.wait_for_function("() => getComputedStyle(document.querySelector('[data-benchmark-tip]')).opacity === '0'")
    controls.nth(3).select_option(preferred["id"])
    _check(row.locator(".bd-s0070").inner_text() == "BASE",
           "baseline selection did not move the baseline marker")
    minimap_row = page.locator("tbody tr").filter(
        has=page.get_by_text("minimap2", exact=True))
    minimap_row.locator(".bd-s0072").nth(column_index).hover()
    _check(f"{1 / expected_ratio:.3f}×" in tooltip.inner_text(),
           "baseline selection did not recompute the inverse A ratio")
    controls.nth(0).select_option("without_cigar_mapq_ns")
    minimap_row.locator(".bd-s0072").nth(column_index).focus()
    expected_b = preferred["cells"][column]["b_ns"] / baseline["cells"][column]["b_ns"]
    _check(f"{expected_b:.3f}×" in tooltip.inner_text(),
           "B metric compares the wrong observations")
    _check("B · without CIGAR/MAPQ" in tooltip.inner_text(),
           "comparison tooltip lost its B measurement")
    page.keyboard.press("Escape")
    controls.nth(0).select_option("wall_clock_ns")
    controls.nth(3).select_option("minimap2")
    page.locator("[data-bnav] a", has_text="Test cases").first.click()
    _check("1,024 queries" in page.locator("main").inner_text(),
           "test-case navigation did not expose the published query count")
    page.locator("[data-bnav] a", has_text="Comparison").first.click()
    _check(page.locator("[data-ctrls]").count() == 1,
           "comparison navigation failed to restore the table")
    page.evaluate("() => window.__closeBioseqBenchmarkFetchGate()")
    page.get_by_role("button", name="Reload").click()
    page.get_by_text("Fetching benchmark artifacts…", exact=True).wait_for(state="visible")
    _wait_for_benchmark_fetch_gate(page)
    _release_benchmark_fetches(page)
    requested = {urlsplit(url).path for url in request_urls}
    _check(benchmark["manifest_route"] in requested,
           "benchmark data was never fetched from the static site")
    _check(not [url for url in request_urls
                if urlsplit(url).hostname in _BENCHMARK_REMOTE_HOSTS],
           "published chapter benchmarks made an external data request")


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
        header = page.locator("header#dnav.site-header")
        _check(header.count() == 1, f"{label}: shared header missing at {width}px")
        for text in ("Docs", "Benchmarks"):
            _check(header.get_by_role("link", name=text, exact=True).is_visible(),
                   f"{label}: {text} navigation hidden at {width}px")
        _check(header.locator("#doc-search").is_visible(),
               f"{label}: common search hidden at {width}px")
        geometry = header.locator(".site-header-inner > *").evaluate_all("""nodes => {
          const boxes = nodes.map(node => ({name: node.className,
            rect: node.getBoundingClientRect()})).filter(x => x.rect.width && x.rect.height);
          const clipped = boxes.filter(x => x.rect.left < 0 || x.rect.right > innerWidth + 1);
          const overlapping = [];
          boxes.forEach((a, i) => boxes.slice(i + 1).forEach(b => {
            if (Math.min(a.rect.right, b.rect.right) - Math.max(a.rect.left, b.rect.left) > 1 &&
                Math.min(a.rect.bottom, b.rect.bottom) - Math.max(a.rect.top, b.rect.top) > 1)
              overlapping.push([a.name, b.name]);
          }));
          return {clipped: clipped.map(x => x.name), overlapping};
        }""")
        _check(not geometry["clipped"] and not geometry["overlapping"],
               f"{label}: header controls overlap/clip at {width}px: {geometry}")
        if label.startswith("tutorial:"):
            _assert_content_scroll_layout(page, label)
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
                  (url.pathname.endsWith('/project-benchmarks.json') && location.pathname.endsWith('/benchmarks/')) ||
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

            if label in ("landing", "benchmarks"):
                _exercise_docs_search(
                    page, request_urls, contract,
                    _public_route(base_path,
                                  f"{version}/tutorials/project_postgres_custom_scan/"))
                _exercise_version_selector(page, request_urls)
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
            if label.startswith("tutorial:"):
                slug = label.split(":", 1)[1]
                tutorial = contract["tutorials"][slug]
                _assert_literal_design_structure(page, label, contract["design"])
                _exercise_design_style_parity(page, label, contract["design"])
                _assert_tutorial_sidebar(page, base_path, version)
                if _nodes_of_kind(tutorial.get("content", []), "code_block"):
                    _exercise_copy(page)
                _exercise_tutorial_results(page, tutorial)
                _exercise_project_content(page, tutorial, base_path, version)
                _exercise_jsonld(page)
                _exercise_doc_frame(page, expect_toc=bool(
                    _nodes_of_kind(tutorial.get("content", []), "heading")))
                if slug == "getting_started":
                    _exercise_tooltip(page, request_urls, contract)
                    _exercise_code_entities(page, {
                        "sql:function:bioseq.cigar(anyelement)",
                    })
                    _check(page.locator(
                        'pre code a.entity-ref[data-entity-id="sql:type:bioseq.seq"]').count() >= 1,
                        "README sequence type sample lost its entity annotation")
                    _exercise_docs_search(
                        page, request_urls, contract,
                        _public_route(base_path,
                                      f"{version}/tutorials/project_postgres_custom_scan/"))
                    _exercise_version_selector(page, request_urls)
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
            if label == "tutorial:getting_started":
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
