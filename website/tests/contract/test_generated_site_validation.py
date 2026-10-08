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
from urllib.parse import unquote, urljoin, urlsplit

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
_EXPECTED_TUTORIAL_IDS = {
    "tutorial:getting_started",
    "tutorial:project_introduction",
    "tutorial:project_biology",
    "tutorial:project_alignment_foundations",
    "tutorial:project_minimizers",
    "tutorial:project_chaining",
    "tutorial:project_final_alignment_postprocessing_and_preprocessing",
    "tutorial:project_sequence_abstractions",
    "tutorial:project_postgres_custom_scan",
    "tutorial:project_postgres_storage",
    "tutorial:project_index_storage",
    "tutorial:project_benchmarking",
    "tutorial:project_conclusion",
    "tutorial:project_configuration_reference",
    "tutorial:bibliography",
}
_BENCHMARK_REMOTE_HOSTS = {"raw.githubusercontent.com"}

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


def _benchmark_data_inventory_problems(
    benchmark_manifest: Path,
    local_targets: set[Path],
    *,
    has_local_source: bool,
    remote_count: int,
) -> list[str]:
    """Validate every staged benchmark-data entry against the manifest mode."""

    problems: list[str] = []
    lexical_data_root = benchmark_manifest.parent / "benchmark-data"
    display_root = benchmark_manifest.parent.resolve()
    staged_files: set[Path] = set()
    if lexical_data_root.is_symlink():
        problems.append(_fail(
            lexical_data_root,
            "benchmark-data root must not be a symlink",
        ))
    elif lexical_data_root.exists() and not lexical_data_root.is_dir():
        problems.append(_fail(
            lexical_data_root,
            "benchmark-data root must be a directory",
        ))
    elif lexical_data_root.is_dir():
        for entry in sorted(lexical_data_root.rglob("*")):
            if entry.is_symlink():
                problems.append(_fail(
                    entry, "benchmark-data must not contain a symlink"))
            elif entry.is_dir():
                continue
            elif not entry.is_file():
                problems.append(_fail(
                    entry, "benchmark-data contains a special file"))
            else:
                staged_files.add(entry.resolve())
    if has_local_source and local_targets != staged_files:
        missing = sorted(
            path.relative_to(display_root).as_posix()
            for path in staged_files - local_targets
        )
        extra = sorted(
            path.relative_to(display_root).as_posix()
            for path in local_targets - staged_files
        )
        problems.append(_fail(
            benchmark_manifest,
            f"benchmark inventory mismatch missing={missing}, extra={extra}",
        ))
    elif not has_local_source and remote_count and staged_files:
        unexpected = sorted(
            path.relative_to(display_root).as_posix()
            for path in staged_files
        )
        problems.append(_fail(
            benchmark_manifest,
            f"remote benchmark override retains local data: {unexpected}",
        ))
    return problems


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
    analytics_tag = '<script async src="https://www.googletagmanager.com/gtag/js?id=G-989VYH5HT7"></script>'
    resource_text = text.replace(analytics_tag, "", 1)
    if 'src="http' in resource_text or "src='http" in resource_text:
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

    # The default benchmark manifest is a content-classified same-origin index.
    # An explicit override may contain only immutable public raw HTTPS URLs.
    benchmark_manifests = sorted(site.rglob("benchmarks-manifest.json"))
    if len(benchmark_manifests) != 1:
        problems.append(_fail(
            site, f"expected one benchmark manifest, got {benchmark_manifests}"))
    else:
        benchmark_manifest = benchmark_manifests[0]
        try:
            benchmark_document = json.loads(
                benchmark_manifest.read_text(encoding="utf-8"))
        except Exception:  # already reported by the general JSON pass
            benchmark_document = {}
        if isinstance(benchmark_document, list):
            sources = benchmark_document
        elif isinstance(benchmark_document, dict):
            sources = benchmark_document.get("sources", [])
        else:
            sources = []
        if not isinstance(sources, list) or not sources:
            problems.append(_fail(
                benchmark_manifest, "has no non-empty sources array"))
        else:
            local_records: list[tuple[str, str, int]] = []
            local_targets: set[Path] = set()
            remote_count = 0
            has_local_source = False
            source_values: set[str] = set()
            data_root = (benchmark_manifest.parent / "benchmark-data").resolve()
            for source in sources:
                if isinstance(source, str):
                    value = source
                elif isinstance(source, dict) and set(source).issubset(
                        {"url", "path"}) and len(source) == 1:
                    value = source.get("url", source.get("path", ""))
                else:
                    value = ""
                if not isinstance(value, str) or not value:
                    problems.append(_fail(
                        benchmark_manifest,
                        f"benchmark source is invalid: {source!r}",
                    ))
                    continue
                if value in source_values:
                    problems.append(_fail(
                        benchmark_manifest,
                        f"benchmark source is duplicated: {value!r}",
                    ))
                    continue
                source_values.add(value)
                try:
                    parsed = urlsplit(value)
                except ValueError as error:
                    problems.append(_fail(
                        benchmark_manifest,
                        f"benchmark source URL is invalid: {value!r}: {error}",
                    ))
                    continue
                if parsed.scheme or parsed.netloc:
                    remote_count += 1
                    decoded_path = unquote(parsed.path)
                    decoded_parts = tuple(
                        part for part in decoded_path.split("/") if part)
                    try:
                        port = parsed.port
                    except ValueError:
                        port = -1
                    immutable_revision = (
                        len(decoded_parts) >= 4
                        and re.fullmatch(
                            r"[0-9a-f]{40}", decoded_parts[2]) is not None
                    )
                    if (
                        parsed.scheme != "https"
                        or parsed.hostname not in _BENCHMARK_REMOTE_HOSTS
                        or port is not None
                        or parsed.username is not None
                        or parsed.password is not None
                        or parsed.query
                        or parsed.fragment
                        or ".." in decoded_parts
                        or "\\" in decoded_path
                        or "%" in decoded_path
                        or "/blob/" in parsed.path
                        or not immutable_revision
                    ):
                        problems.append(_fail(
                            benchmark_manifest,
                            f"unsupported benchmark override URL {value!r}",
                        ))
                    continue
                has_local_source = True
                decoded_path = unquote(parsed.path)
                relative_parts = Path(decoded_path).parts
                if (
                    decoded_path.startswith("/")
                    or parsed.query
                    or parsed.fragment
                    or "\\" in decoded_path
                    or "%" in decoded_path
                    or ".." in relative_parts
                ):
                    problems.append(_fail(
                        benchmark_manifest,
                        f"benchmark source is not a plain relative path: {value!r}",
                    ))
                    continue
                if len(relative_parts) < 2 or relative_parts[0] != "benchmark-data":
                    problems.append(_fail(
                        benchmark_manifest,
                        f"benchmark source is outside benchmark-data: {value!r}",
                    ))
                    continue
                lexical_target = benchmark_manifest.parent / decoded_path
                current = benchmark_manifest.parent
                traverses_symlink = False
                for part in relative_parts:
                    current = current / part
                    if current.is_symlink():
                        traverses_symlink = True
                        break
                if traverses_symlink:
                    problems.append(_fail(
                        benchmark_manifest,
                        f"benchmark source traverses a symlink: {value!r}",
                    ))
                    continue
                target = lexical_target.resolve()
                try:
                    target.relative_to(data_root)
                except ValueError:
                    problems.append(_fail(
                        benchmark_manifest,
                        f"benchmark source escapes benchmark-data: {value!r}",
                    ))
                    continue
                if target.suffix != ".json" or not target.is_file():
                    problems.append(_fail(
                        benchmark_manifest,
                        f"benchmark source does not resolve: {value!r}",
                    ))
                    continue
                if target in local_targets:
                    problems.append(_fail(
                        benchmark_manifest,
                        f"benchmark target is duplicated: {value!r}",
                    ))
                    continue
                local_targets.add(target)

                try:
                    document = json.loads(target.read_text(encoding="utf-8"))
                except Exception:
                    continue
                report_claim = (
                    isinstance(document, dict)
                    and isinstance(document.get("benchmarks"), dict)
                    and bool(document["benchmarks"])
                    and isinstance(document.get("meta"), dict)
                )
                config_blocks = []
                if isinstance(document, dict):
                    config_blocks = [
                        block for key, block in document.items()
                        if key not in {"params", "test_cases", "regression_rules"}
                        and isinstance(block, dict)
                        and isinstance(block.get("matrix"), dict)
                    ]
                config_claim = (
                    isinstance(document, dict)
                    and isinstance(document.get("params"), dict)
                    and bool(document["params"])
                    and isinstance(document.get("test_cases"), dict)
                    and bool(document["test_cases"])
                    and isinstance(document.get("regression_rules"), list)
                    and bool(config_blocks)
                )
                if report_claim == config_claim:
                    problems.append(_fail(
                        target,
                        "is not exactly one supported benchmark report/configuration",
                    ))
                    continue
                if config_claim:
                    local_records.append((decoded_path, "configuration", 0))
                    continue
                meta = document["meta"]
                report_shape_valid = (
                    type(meta.get("timestamp")) is int
                    and meta["timestamp"] >= 0
                    and isinstance(meta.get("created"), str)
                    and isinstance(meta.get("fast"), bool)
                    and isinstance(meta.get("history_eligible"), bool)
                    and all(
                        isinstance(benchmark, dict)
                        and isinstance(benchmark.get("execution_tuples"), list)
                        and bool(benchmark["execution_tuples"])
                        for benchmark in document["benchmarks"].values()
                    )
                )
                if not report_shape_valid:
                    problems.append(_fail(
                        target, "does not expose validated framework report metadata"))
                    continue
                local_records.append(
                    (decoded_path, "report", meta["timestamp"]))
            if has_local_source and remote_count:
                problems.append(_fail(
                    benchmark_manifest,
                    "must not mix same-origin data with remote overrides",
                ))
            problems.extend(_benchmark_data_inventory_problems(
                benchmark_manifest,
                local_targets,
                has_local_source=has_local_source,
                remote_count=remote_count,
            ))
            if has_local_source:
                if not any(kind == "configuration"
                           for _, kind, _ in local_records):
                    problems.append(_fail(
                        benchmark_manifest,
                        "same-origin benchmark inventory has no configuration",
                    ))
                expected_records = sorted(
                    (record for record in local_records
                     if record[1] == "configuration"),
                    key=lambda record: record[0],
                ) + sorted(
                    (record for record in local_records if record[1] == "report"),
                    key=lambda record: (-record[2], record[0]),
                )
                if local_records != expected_records:
                    problems.append(_fail(
                        benchmark_manifest,
                        "benchmark sources are not in deterministic config/newest-report order",
                    ))

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
                location = node.text.strip()
                if location in sitemap_urls:
                    problems.append(f"sitemap.xml: duplicate URL {location}")
                sitemap_urls.add(location)
    else:
        problems.append("sitemap.xml: missing generated sitemap")
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
        if path.name == "index.html" and canonical not in sitemap_urls:
            problems.append(_fail(path, "indexable page is missing from sitemap.xml"))
    for url in sorted(sitemap_urls):
        if url not in seen_canonical:
            problems.append(f"sitemap.xml: {url} is not the canonical URL of any page")
    robots_path = site / "robots.txt"
    if not robots_path.is_file():
        problems.append("robots.txt: missing generated crawler instructions")
    else:
        robots = robots_path.read_text(encoding="utf-8")
        home_canonical = next((url for url, path in seen_canonical.items()
                               if path == site / "index.html"), None)
        if home_canonical:
            expected_sitemap = home_canonical.rsplit("/", 1)[0] + "/sitemap.xml"
            if f"Sitemap: {expected_sitemap}" not in robots.splitlines():
                problems.append("robots.txt: must point to the generated absolute sitemap URL")
        if "User-agent: *" not in robots.splitlines() or "Allow: /" not in robots.splitlines():
            problems.append("robots.txt: public website pages must be crawlable")

    # (4) Every internal link/anchor/asset resolves to an emitted file.
    page_ids: dict[str, set[str]] = {
        "/" + path.relative_to(site).as_posix(): set(_ID_ATTR.findall(text))
        for path, text in pages.items()
    }
    for path, text in pages.items():
        page_route = "/" + path.relative_to(site).as_posix()
        page_url = "https://validation.invalid" + base_path.rstrip("/") + page_route
        for url in _ATTR_URL.findall(text):
            if url.startswith(_EXTERNAL_URL):
                continue
            resolved = urlsplit(urljoin(page_url, url))
            frag = unquote(resolved.fragment)
            rel = _route_to_relpath(resolved.path, base_path)
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
    for search_path in site.rglob("search-index.json"):
        records = json.loads(
            search_path.read_text(encoding="utf-8")
        ).get("records", [])
        tutorial_ids = {
            record.get("id")
            for record in records
            if record.get("group") == "Tutorials"
        }
        if tutorial_ids != _EXPECTED_TUTORIAL_IDS:
            problems.append(_fail(
                search_path,
                "tutorial search inventory is not the exact current tutorial set",
            ))
        for record in records:
            route = record.get("route")
            rel = _route_to_relpath(route, base_path) if route else None
            if rel is None or not (site / rel).is_file():
                problems.append(_fail(search_path,
                                      f"record {record.get('id')!r} route {route} does not resolve"))
        tutorial_root = search_path.parent / "tutorials"
        actual_slugs = {page.parent.name for page in tutorial_root.glob("*/index.html")}
        expected_slugs = {identifier.removeprefix("tutorial:")
                          for identifier in _EXPECTED_TUTORIAL_IDS}
        if actual_slugs != expected_slugs:
            problems.append(_fail(
                tutorial_root, "published chapters differ from the current source inventory"))
        bibliography = tutorial_root / "bibliography" / "index.html"
        bibliography_ids = set(_ID_ATTR.findall(
            bibliography.read_text(encoding="utf-8"))) if bibliography.is_file() else set()
        if len([identifier for identifier in bibliography_ids
                if identifier.startswith("ref-")]) < 100:
            problems.append(_fail(bibliography, "project bibliography lost its reference entries"))
        figure_manifest_routes = sorted(
            route for route in declared
            if re.fullmatch(r"/assets/[0-9a-f]{64}/figures/project/manifest\.json", route))
        expected_figures: set[str] = set()
        figure_manifest = (site / figure_manifest_routes[0].lstrip("/")
                           if len(figure_manifest_routes) == 1 else site / "assets")
        if len(figure_manifest_routes) != 1 or not figure_manifest.is_file():
            problems.append(_fail(figure_manifest,
                                  "expected one staged original-figure export manifest"))
        else:
            asset_root = figure_manifest.parents[2]
            figures = json.loads(figure_manifest.read_text(encoding="utf-8")).get("figures", [])
            if len(figures) != 74:
                problems.append(_fail(figure_manifest, "expected all 74 original diagrams and plots"))
            for figure in figures:
                svg = figure.get("svg", "")
                asset = asset_root / svg
                expected_figures.add(asset.relative_to(site).as_posix())
                if not asset.is_file():
                    problems.append(_fail(asset, "exported project figure is missing"))
                    continue
                try:
                    graphic = ET.parse(asset).getroot()
                    viewport = [float(value) for value in graphic.attrib["viewBox"].split()]
                    if (not graphic.tag.endswith("svg") or len(viewport) != 4
                            or viewport[2] <= 0 or viewport[3] <= 0):
                        raise ValueError("invalid SVG viewport")
                    for node in graphic.iter():
                        for name, target in node.attrib.items():
                            if name.endswith("href") and (
                                    target.startswith("file:") or ".dvi#" in target
                                    or "/Users/" in target or "/tmp/" in target):
                                raise ValueError(f"SVG leaks a build-local link: {target}")
                except (ET.ParseError, KeyError, ValueError) as error:
                    problems.append(_fail(asset, f"invalid exported SVG: {error}"))
        referenced_figures: set[str] = set()
        code_references = 0
        citation_count = 0
        for tutorial_page in tutorial_root.glob("*/index.html"):
            tutorial_text = tutorial_page.read_text(encoding="utf-8")
            page_url = ("https://validation.invalid" + base_path.rstrip("/") + "/"
                        + tutorial_page.relative_to(site).as_posix())
            for url in _ATTR_URL.findall(tutorial_text):
                resolved = urlsplit(urljoin(page_url, url))
                if "/figures/project/" in resolved.path:
                    rel = _route_to_relpath(resolved.path, base_path)
                    if rel:
                        referenced_figures.add(rel)
                if "#ref-" in url:
                    citation_count += 1
                    target = _route_to_relpath(resolved.path, base_path)
                    if (resolved.netloc != "validation.invalid"
                            or target != bibliography.relative_to(site).as_posix()):
                        problems.append(_fail(tutorial_page,
                                              f"citation must link to bibliography: {url}"))
                    if unquote(resolved.fragment) not in bibliography_ids:
                        problems.append(_fail(tutorial_page,
                                              f"citation has no bibliography entry: {url}"))
                if url.startswith("https://github.com/") and "/blob/" in url:
                    code_references += 1
            # Internal storage descriptions belong in the author's architecture
            # chapters; hidden executor test diagnostics still do not.
            for forbidden in ("DSM Test Fixture", "Fully Attested Alignment Jobs",
                              "Bioseq Batch ABI Version", "Bioseq Owned RestrictInfo Path Copies"):
                if forbidden in tutorial_text:
                    problems.append(_fail(tutorial_page,
                                          f"public chapter exposes test diagnostic {forbidden!r}"))
        if referenced_figures != expected_figures:
            problems.append(_fail(figure_manifest,
                                  "published chapter figures do not match the complete SVG inventory"))
        if citation_count < 100 or code_references < 10:
            problems.append(_fail(tutorial_root,
                                  "project citations or original source-code references were lost"))
        for slug, tokens in {
            "getting_started": ("readme_docker_demo", "mmsearch", "bioseq.cigar"),
            "project_biology": ("Biological context", "data-math"),
            "project_postgres_custom_scan": ("BioseqScan", "PlaceHolderVar"),
            "project_index_storage": ("minimizers_per_fragment", "symbol_counts"),
            "project_benchmarking": ("project-benchmarks", "7.68 cores", "120.8 GiB",
                                     "32,607 MiB", "595.84", "1.4.329", "500.28 GB",
                                     "6,000 IOPS / 500 MiB/s"),
        }.items():
            page = tutorial_root / slug / "index.html"
            text = page.read_text(encoding="utf-8") if page.is_file() else ""
            for token in tokens:
                if token not in text:
                    problems.append(_fail(page, f"project content token {token!r} is missing"))
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
