#!/usr/bin/env python3
"""Independent generated-site HTML validator (§14, §17.3 step 9/10, §20.7).

This is the *external* validation layer of the two-layer generated-site check:
the C++ generator (`site_builder.cppm validate_generated_html`) validates its own
typed model, and this script re-inspects the emitted bytes with an independent
implementation so a typed-model bug cannot self-certify. It runs against an
already-staged site directory, performs no network access, and writes nothing
into the staging tree except the success stamp requested on the command line.

Unlike the previous substring-only validator, this is a REAL HTML5 validator:

* Every ``*.html`` file under ``--site`` is parsed with
  ``html5lib.HTMLParser(strict=True)`` (html5lib 1.1). Strict mode raises
  ``html5lib.html5parser.ParseError`` on malformed or mis-nested markup, a
  duplicate attribute, a stray end tag, or an illegal table/section structure;
  the offending file and the parser message are reported.
* The parsed etree is walked to detect duplicate ``id`` attributes (§20.7
  "unique HTML IDs"), which the HTML5 parse itself does not reject.

On top of the real parse it PRESERVES every fail-closed rule the generator and
the previous validator already enforced, so the external stamp neither accepts
HTML the generator would reject nor rejects HTML it accepts. External
``<a href>`` links are allowed; only external ``src=`` / linked stylesheet
resources are forbidden. ``frame-ancestors`` is header-only (§15.5) and must
NOT appear in the meta CSP.

CLI:
  --site  <dir>    site root to validate; walks ``*.html`` recursively.
  --stamp <path>   success stamp, written only when every file passes. It is
                   bound to the SHA-256 of ``<site>/site-manifest.json`` when
                   that file exists (mirrors §17.3: the stamp is tied to the
                   staged manifest so publish depends on an independent check).

Exit codes:
  0  every HTML file valid; stamp written.
  2  usage / environment problem: bad arguments, missing site directory, no
     HTML files found, or html5lib not importable. No stamp written.
  3  one or more HTML validation failures; per-file diagnostics on stderr and
     no stamp written.

html5lib is a hard prerequisite of publication, so its absence is a fatal
environment error (exit 2), not a skip. Install the pinned environment from
``website/tests/requirements.lock`` with ``pip install --require-hashes``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

try:
    import html5lib
    from html5lib.html5parser import ParseError as Html5ParseError
except ImportError as exc:  # pragma: no cover - exercised only without the dep
    print(
        "test_generated_site_validation: html5lib is required but not importable "
        f"({exc}); install website/tests/requirements.lock with "
        "`pip install --require-hashes -r requirements.lock`",
        file=sys.stderr,
    )
    sys.exit(2)

# Substring rules below mirror the generator's fail-closed validator and the
# generated-site validator EXACTLY so the two stamps agree (external <a href>
# links are allowed; only external src=/stylesheet resources are forbidden).
_INLINE_HANDLERS = (
    " onclick=", " onload=", " onerror=", " onmouseover=", " onfocus=",
    " onsubmit=", " onchange=")
_H1 = re.compile(r"<h1[\s>]", re.IGNORECASE)

# HTML namespace used by html5lib's etree treebuilder; the ``id`` attribute key
# itself is not namespaced (verified empirically against html5lib 1.1).
_XHTML = "{http://www.w3.org/1999/xhtml}"

# --- §20.7 site-level relationship checks -------------------------------------
_CANON = re.compile(r'<link\s+rel="canonical"\s+href="([^"]+)"', re.IGNORECASE)
_ATTR_URL = re.compile(r'(?:href|src)="([^"]+)"', re.IGNORECASE)
_ID_ATTR = re.compile(r'\sid="([^"]+)"')
_HEADING = re.compile(r"<h([1-6])[\s>]", re.IGNORECASE)
_ENTITY_DATA_URL = re.compile(r'data-entity-data-url="([^"]+)"')
_EXTERNAL_URL = ("http://", "https://", "//", "mailto:", "tel:", "data:", "javascript:")

# §20.7 forbidden runtime/resource tokens: the generated site must ship none of the
# Design Component runtime, CDN loaders, external fonts, or eval-family entrypoints.
_FORBIDDEN_TOKENS = (
    "support.js", "unpkg", "fonts.googleapis", "fonts.gstatic",
    "ReactDOM", "babel/standalone", "new Function", "x-dc", "sc-if")
# §20.7 machine/socket/database path leakage: absolute workspace paths, PostgreSQL
# sockets, and connection strings must never reach a published byte.
_ABSOLUTE_PATH_TOKENS = ("/home/", "/root/", ".s.PGSQL", "dbname=", "postgresql://")


def _fail(path: Path, reason: str) -> str:
    return f"{path}: {reason}"


def _route_to_relpath(route: str, base_path: str) -> str | None:
    """Map a site-absolute route (already base_path-rooted) to a site-relative
    filesystem path, or None when it is not under the configured mount."""
    if not route.startswith(base_path):
        return None
    rel = route[len(base_path):]
    if rel.startswith("/"):
        rel = rel[1:]
    if rel == "" or rel.endswith("/"):
        rel += "index.html"
    return rel


def _parse_problems(path: Path, text: str) -> list[str]:
    """Real HTML5 parse + duplicate-id walk."""
    problems: list[str] = []

    # 1. Strict HTML5 parse: raises on the first parse error and stops.
    try:
        html5lib.HTMLParser(strict=True).parse(text)
    except Html5ParseError as error:
        problems.append(_fail(path, f"is not valid HTML5: {error}"))
    except Exception as error:  # defensive: never let a parser bug pass silently
        problems.append(_fail(path, f"could not be parsed as HTML5: {error}"))

    # 2. Lenient etree parse (never raises) to walk for duplicate ids even in a
    #    file the strict parse already rejected.
    try:
        document = html5lib.parse(text, treebuilder="etree")
    except Exception as error:  # pragma: no cover - lenient parse essentially never raises
        problems.append(_fail(path, f"could not build a document tree: {error}"))
        return problems

    counts: dict[str, int] = {}
    for element in document.iter():
        attrib = getattr(element, "attrib", None)
        if not attrib:
            continue
        value = attrib.get("id")
        if value is None:
            value = attrib.get(_XHTML + "id")
        if value is not None:
            counts[value] = counts.get(value, 0) + 1
    for identifier, count in sorted(counts.items()):
        if count > 1:
            problems.append(
                _fail(path, f"has duplicate id {identifier!r} ({count} occurrences)"))
    return problems


def validate_html(path: Path, text: str) -> list[str]:
    problems: list[str] = []

    # --- real HTML5 parse + unique-id check (new in this validator) ----------
    problems.extend(_parse_problems(path, text))

    # --- preserved substring rules (mirror the generator + prior validator) --
    if "{{" in text or "}}" in text:
        problems.append(_fail(path, "contains an unrendered template placeholder"))
    if "javascript:" in text:
        problems.append(_fail(path, "contains a javascript: URL"))
    for marker in ("bioseq:setup", "bioseq:visible", "bioseq:teardown"):
        if marker in text:
            problems.append(_fail(path, "leaks a hidden runnable-example SQL phase"))
    if any(handler in text for handler in _INLINE_HANDLERS):
        problems.append(_fail(path, "contains an inline event handler"))
    if " style=" in text:
        problems.append(_fail(path, "contains an inline style attribute"))
    if 'src="http' in text or "src='http" in text:
        problems.append(_fail(path, "references an external script/media resource"))
    if "<link" in text and 'stylesheet" href="http' in text:
        problems.append(_fail(path, "references an external stylesheet"))
    h1_count = len(_H1.findall(text))
    if h1_count != 1:
        problems.append(_fail(path, f"has {h1_count} <h1> elements, expected 1"))
    if "Content-Security-Policy" not in text:
        problems.append(_fail(path, "is missing the meta Content-Security-Policy"))
    if "frame-ancestors" in text:
        problems.append(_fail(path, "frame-ancestors must not appear in the meta CSP"))

    # --- §20.7 per-file additions -------------------------------------------
    # Non-skipped heading hierarchy: a level never jumps by more than one.
    previous_level = 0
    for level_text in _HEADING.findall(text):
        level = int(level_text)
        if previous_level and level > previous_level + 1:
            problems.append(_fail(path, f"heading h{level} skips a level after h{previous_level}"))
        previous_level = level
    for token in _FORBIDDEN_TOKENS:
        if token in text:
            problems.append(_fail(path, f"ships forbidden runtime/resource token {token!r}"))
    for token in _ABSOLUTE_PATH_TOKENS:
        if token in text:
            problems.append(_fail(path, f"leaks an absolute workspace/socket/database path {token!r}"))
    if 'aria-disabled="true"' in text:
        problems.append(_fail(path, "contains a disabled (aria-disabled) documentation link"))
    return problems


def site_level_checks(site: Path, pages: dict[Path, str],
                      asset_lock: Path | None) -> list[str]:
    """§20.7 whole-site relationship checks that no single page can self-certify:
    every artifact parses, the manifest matches the tree exactly, canonical/sitemap
    agree, and every internal link/anchor/asset/search-record/tooltip route exists."""
    problems: list[str] = []
    manifest = json.loads((site / "site-manifest.json").read_text(encoding="utf-8"))
    if not isinstance(manifest, dict):
        problems.append(
            _fail(site / "site-manifest.json", "manifest root must be an object")
        )
        return problems
    manifest_contract = (
        manifest.get("schema"),
        manifest.get("schema_revision"),
    )
    if (
        type(manifest_contract[1]) is not int
        or not (
            manifest_contract == ("bioseq.website.site_manifest", 1)
            or manifest_contract == ("bioseq.website.assembled_manifest", 1)
        )
    ):
        problems.append(
            _fail(
                site / "site-manifest.json",
                "does not declare a current website manifest schema",
            )
        )
    elif manifest_contract[0] == "bioseq.website.site_manifest" and (
        manifest.get("parser_identity") != "bioseq.website.parser"
        or type(manifest.get("parser_revision")) is not int
        or manifest["parser_revision"] != 1
        or manifest.get("binding_schema") != "bioseq.website.bindings"
        or type(manifest.get("binding_schema_revision")) is not int
        or manifest["binding_schema_revision"] != 1
    ):
        problems.append(
            _fail(
                site / "site-manifest.json",
                "does not declare the current parser and binding contracts",
            )
        )
    base_path = manifest.get("base_path", "/")

    # (1) Every emitted JSON/XML artifact parses with the standard-library parsers.
    for path in sorted(site.rglob("*.json")):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except Exception as error:  # noqa: BLE001
            problems.append(_fail(path, f"is not valid JSON: {error}"))
            continue
        expected_schema: tuple[str, int] | None = None
        if path.name == "search-index.json":
            expected_schema = ("bioseq.website.search", 2)
        elif path.name == "versions.json":
            expected_schema = ("bioseq.website.versions", 1)
        elif path.name == "documentation-coverage.json":
            expected_schema = ("bioseq.website.documentation_coverage", 1)
        elif path.name == "catalog-inventory.json":
            expected_schema = ("bioseq.website.sql_entities", 1)
        elif re.fullmatch(r"entities\.[0-9a-f]{64}\.json", path.name):
            expected_schema = ("bioseq.website.entity_tooltips", 1)
        if expected_schema is not None and (
            not isinstance(document, dict)
            or document.get("schema") != expected_schema[0]
            or type(document.get("schema_revision")) is not int
            or document["schema_revision"] != expected_schema[1]
        ):
            problems.append(_fail(path, "does not declare its current schema"))
    for path in sorted(site.rglob("*.xml")):
        try:
            ET.parse(path)
        except Exception as error:  # noqa: BLE001
            problems.append(_fail(path, f"is not valid XML: {error}"))

    # (2) site-manifest set-equality + per-file SHA-256 verification (§16). The
    #     external layer re-derives the tree so a typed-model manifest cannot
    #     self-certify; symlinks and special files are rejected outright.
    outputs = manifest.get("outputs", {})
    declared = set(outputs)
    present: set[str] = set()
    for path in sorted(site.rglob("*")):
        if path.is_symlink():
            problems.append(_fail(path, "site tree must not contain a symlink"))
            continue
        if path.is_dir():
            continue
        if not path.is_file():
            problems.append(_fail(path, "site tree contains a non-regular file"))
            continue
        route = "/" + path.relative_to(site).as_posix()
        if route == "/site-manifest.json":
            continue
        present.add(route)
        expected = outputs.get(route)
        if expected is not None:
            actual = hashlib.sha256(path.read_bytes()).hexdigest()
            if actual != expected:
                problems.append(_fail(path, f"manifest hash mismatch "
                                            f"(manifest {expected[:12]}, file {actual[:12]})"))
    for extra in sorted(present - declared):
        problems.append(f"{extra}: present in the site but absent from the manifest")
    for missing in sorted(declared - present):
        problems.append(f"{missing}: declared in the manifest but absent from the site")

    # (3) canonical <-> sitemap agreement and no duplicate canonical URL. Every
    #     sitemap URL must be some page's canonical; non-indexable pages (e.g. the
    #     404 page) may carry a canonical without a sitemap row.
    sitemap_urls: set[str] = set()
    sitemap_path = site / "sitemap.xml"
    if sitemap_path.is_file():
        for node in ET.parse(sitemap_path).iter():
            if node.tag.endswith("loc") and node.text:
                sitemap_urls.add(node.text.strip())
    seen_canonical: dict[str, Path] = {}
    for path, text in pages.items():
        found = _CANON.search(text)
        if not found:
            problems.append(_fail(path, "has no <link rel=canonical>"))
            continue
        canonical = found.group(1)
        if canonical in seen_canonical:
            problems.append(_fail(path, f"duplicate canonical URL {canonical} "
                                        f"(also on {seen_canonical[canonical]})"))
        else:
            seen_canonical[canonical] = path
    for url in sorted(sitemap_urls):
        if url not in seen_canonical:
            problems.append(f"sitemap.xml: {url} is not the canonical URL of any page")

    # (4) Every internal link/anchor/asset resolves to an emitted file.
    page_ids: dict[str, set[str]] = {
        "/" + path.relative_to(site).as_posix(): set(_ID_ATTR.findall(text))
        for path, text in pages.items()
    }
    for path, text in pages.items():
        page_route = "/" + path.relative_to(site).as_posix()
        page_dir = "/" + path.parent.relative_to(site).as_posix()
        for url in _ATTR_URL.findall(text):
            if url.startswith(_EXTERNAL_URL):
                continue
            target, _, frag = url.partition("#")
            target = target.split("?", 1)[0]
            if target == "":
                if frag and frag not in page_ids.get(page_route, set()):
                    problems.append(_fail(path, f"in-page anchor #{frag} has no target"))
                continue
            if target.startswith("/"):
                rel = _route_to_relpath(target, base_path)
            else:
                base = page_dir.rstrip("/")
                rel = _route_to_relpath(f"{base}/{target}", base_path)
            if rel is None:
                continue
            target_file = site / rel
            if not target_file.is_file():
                problems.append(_fail(path, f"internal link {url} -> {rel} does not exist"))
                continue
            if frag and target_file.suffix == ".html":
                target_route = "/" + target_file.relative_to(site).as_posix()
                ids = page_ids.get(target_route)
                if ids is None:
                    ids = set(_ID_ATTR.findall(target_file.read_text(encoding="utf-8")))
                if frag not in ids:
                    problems.append(_fail(path, f"link {url} anchor #{frag} not found in target"))

    # (5) Search records, entity references, and tooltip data URLs resolve (§12.4).
    expected_tutorial_ids = {
        "tutorial:sequence_column_type",
        "tutorial:similarity_search",
        "tutorial:query_cache",
        "tutorial:highway_runtime_dispatch",
    }
    for search_path in site.rglob("search-index.json"):
        records = json.loads(
            search_path.read_text(encoding="utf-8")
        ).get("records", [])
        tutorial_ids = {
            record.get("id")
            for record in records
            if record.get("group") == "Tutorials"
        }
        if tutorial_ids != expected_tutorial_ids:
            problems.append(_fail(
                search_path,
                "tutorial search inventory is not the exact current four-page set",
            ))
        for record in records:
            route = record.get("route")
            rel = _route_to_relpath(route, base_path) if route else None
            if rel is None or not (site / rel).is_file():
                problems.append(_fail(search_path,
                                      f"record {record.get('id')!r} route {route} does not resolve"))
        tutorial_root = search_path.parent / "tutorials"
        cache_page = tutorial_root / "query_cache" / "index.html"
        highway_page = tutorial_root / "highway_runtime_dispatch" / "index.html"
        if not cache_page.is_file():
            problems.append(_fail(cache_page, "current cache tutorial is missing"))
        else:
            cache_text = cache_page.read_text(encoding="utf-8")
            for token in (
                "exact_query", "reusable_fragments", "minimizers_per_sequence",
                "minimizers_per_fragment", "bioseq_cache.stats",
                "bioseq_cache.entries", "invalidate_relation", "invalidate_all",
                "SQLSTATE 42501",
            ):
                if token not in cache_text:
                    problems.append(_fail(
                        cache_page, f"cache qualification token {token!r} is missing"))
        if not highway_page.is_file():
            problems.append(_fail(highway_page, "current Highway tutorial is missing"))
        else:
            highway_text = highway_page.read_text(encoding="utf-8")
            for label in (
                "Highway Compiled Targets", "CPU-Supported Targets",
                "Runnable Targets", "Planned Architecture",
                "Executed Architecture",
            ):
                if label not in highway_text:
                    problems.append(_fail(
                        highway_page, f"Highway EXPLAIN label {label!r} is missing"))
    for entities_path in site.rglob("entities.*.json"):
        for entity_id, entity in json.loads(
                entities_path.read_text(encoding="utf-8")).get("entities", {}).items():
            route = entity.get("route")
            rel = _route_to_relpath(route, base_path) if route else None
            if route and (rel is None or not (site / rel).is_file()):
                problems.append(_fail(entities_path,
                                      f"entity {entity_id!r} route {route} does not resolve"))
    for path, text in pages.items():
        for data_url in _ENTITY_DATA_URL.findall(text):
            rel = _route_to_relpath(data_url, base_path)
            if rel is None or not (site / rel).is_file():
                problems.append(_fail(path, f"data-entity-data-url {data_url} does not resolve"))

    # (6) Every production font/vendor/media asset carries a license/inventory entry
    #     in the static-assets lock (§4.4). First-party css/js are project-owned.
    if asset_lock is not None and asset_lock.is_file():
        lock_text = asset_lock.read_text(encoding="utf-8")
        if not re.search(
            r"(?m)^schema: bioseq\.website\.static_assets\.lock\n"
            r"schema_revision: 1$",
            lock_text,
        ):
            problems.append(
                f"{asset_lock}: static-assets lock schema is not current"
            )
        for route in sorted(declared):
            match = re.match(r"^/assets/[0-9a-f]{64}/(.+)$", route)
            if not match:
                continue
            rel = match.group(1)
            if rel.split("/", 1)[0] in ("fonts", "vendor", "media") and rel not in lock_text:
                problems.append(f"{route}: static asset {rel} has no license/inventory entry")

    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Independent html5lib-based generated-site HTML validator.")
    parser.add_argument("--site", required=True, type=Path,
                        help="site root directory to validate (walked recursively)")
    parser.add_argument("--stamp", required=True, type=Path,
                        help="success stamp path (written only when all files pass)")
    parser.add_argument("--asset-lock", type=Path, default=None,
                        help="static-assets provenance lock (third_party.lock.yaml); when "
                             "given, every font/vendor/media asset must have an entry")
    args = parser.parse_args(argv)

    if not args.site.is_dir():
        print(
            f"test_generated_site_validation: site directory missing: {args.site}",
            file=sys.stderr,
        )
        return 2

    # Bind the stamp to the staged manifest hash when present (§17.3 step 9).
    manifest_path = args.site / "site-manifest.json"
    manifest_sha: str | None = None
    if manifest_path.is_file():
        manifest_bytes = manifest_path.read_bytes()
        manifest_sha = hashlib.sha256(manifest_bytes).hexdigest()
        try:
            json.loads(manifest_bytes)
        except json.JSONDecodeError as error:
            print(
                "test_generated_site_validation: site-manifest.json is not "
                f"valid JSON: {error}",
                file=sys.stderr,
            )
            return 3

    problems: list[str] = []
    pages: dict[Path, str] = {}
    for path in sorted(args.site.rglob("*.html")):
        text = path.read_text(encoding="utf-8", errors="strict")
        pages[path] = text
        problems.extend(validate_html(path, text))

    if not pages:
        print(
            f"test_generated_site_validation: no .html files found under {args.site}",
            file=sys.stderr,
        )
        return 2
    html_count = len(pages)

    # Whole-site relationship checks (§20.7): manifest set-equality + hashes,
    # canonical/sitemap agreement, internal link/anchor/asset/route existence.
    problems.extend(site_level_checks(args.site, pages, args.asset_lock))
    if problems:
        for problem in problems:
            print(f"test_generated_site_validation: {problem}", file=sys.stderr)
        print(
            "test_generated_site_validation: "
            f"{len(problems)} problem(s) across {html_count} HTML file(s)",
            file=sys.stderr,
        )
        return 3

    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    args.stamp.write_text(
        json.dumps(
            {
                "validator": "html5lib-strict",
                "validated_html": html_count,
                "manifest_sha256": manifest_sha,
            },
            indent=2,
        ) + "\n",
        encoding="utf-8",
    )
    manifest_note = manifest_sha[:12] if manifest_sha else "no manifest"
    print(
        "test_generated_site_validation: "
        f"{html_count} HTML files passed html5lib validation; "
        f"manifest {manifest_note}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
