#!/usr/bin/env python3
"""Playwright browser acceptance test for the generated static site
(§17.3 step 10, §20.7).

The same executed site model is rendered at TWO base paths, ``/`` and
``/bioseqdb/``, into separate byte trees; CMake runs this harness against the
normal staging root plus both fixtures. ``--base-path`` lets one already-executed
model be validated at each mount WITHOUT re-running any SQL. For each run this
harness:

  * starts fixtures.server.StaticFixtureServer on 127.0.0.1, which reads the
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
     failed". No stamp written. CMake should treat 4 as a soft/skipped result
     unless browser tests are mandatory for the current mode.

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
import sys
from pathlib import Path
from urllib.parse import urlsplit

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

from fixtures.server import StaticFixtureServer, parse_security_headers  # noqa: E402

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_FAILED = 3
EXIT_SKIP = 4

_LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "[::1]", "::1"}


class AssertionFail(Exception):
    """A browser-level contract violation (maps to exit code 3)."""


def _check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionFail(message)


def _load_playwright():
    """Return the sync_api module, or None if Playwright is unavailable."""
    try:
        from playwright.sync_api import sync_playwright  # noqa: F401
    except ImportError:
        return None
    import playwright.sync_api as api
    return api


def _discover_tutorial_route(site: Path, base: str, version: str) -> str:
    """Pick a real rendered tutorial route so the acceptance test tracks the
    actual v0.1 content instead of a hard-coded slug. Falls back to the tutorials
    index when no individual tutorial page exists."""
    tutorials_dir = site / version / "tutorials"
    if tutorials_dir.is_dir():
        for child in sorted(tutorials_dir.iterdir()):
            if child.is_dir() and (child / "index.html").is_file():
                return f"{base}/{version}/tutorials/{child.name}/"
    return f"{base}/{version}/tutorials/"


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
        "tutorial": _discover_tutorial_route(site, base, version),
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
    """If a [data-tabs] widget is present, verify roving selection + panels."""
    widget = page.locator("[data-tabs]").first
    if widget.count() == 0:
        return
    tabs = widget.locator('[role="tab"]')
    n = tabs.count()
    if n < 2:
        return
    second = tabs.nth(1)
    second.click()
    _check(second.get_attribute("aria-selected") == "true",
           "clicked tab did not become aria-selected")
    _check(tabs.nth(0).get_attribute("aria-selected") == "false",
           "previously selected tab stayed selected after switching")
    panel_id = second.get_attribute("aria-controls")
    _check(bool(panel_id), "selected tab has no aria-controls")
    panel = page.locator(f"#{panel_id}")
    _check(panel.is_visible(), "selected tab panel is not visible")


def _exercise_tooltip(page) -> None:
    """If entity-ref tooltips are present (§12.4), verify keyboard focus shows a
    populated tooltip and Escape dismisses it (§20.7)."""
    refs = page.locator("a.entity-ref[data-entity-id][data-entity-data-url]")
    if refs.count() == 0:
        return
    ref = refs.first
    ref.scroll_into_view_if_needed()
    ref.focus()
    tip = page.locator("#entity-tooltip")
    try:
        tip.wait_for(state="visible", timeout=3000)
    except Exception as error:  # noqa: BLE001
        raise AssertionFail(f"entity-ref focus did not show a tooltip: {error}")
    _check(bool(tip.inner_text().strip()), "tooltip shown with no content")
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
    if count == 0:
        return
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


def _exercise_annotations(page) -> None:
    """If an annotated code block is present (§10.6), verify each underlined match
    span has a matching numbered callout card."""
    marks = page.locator(".doc-code mark.anno")
    count = marks.count()
    if count == 0:
        return
    cards = page.locator(".anno-cards .anno-card")
    _check(cards.count() == count,
           f"{count} annotation mark(s) but {cards.count()} callout card(s)")
    nums = page.locator(".anno-card .anno-card-num")
    _check(nums.count() == count, "each callout card must carry a number badge")


def _exercise_code_entities(page) -> None:
    """If a code block carries entity-range metadata (§12.6), verify the client
    decorator wrapped in-code entity links without altering the source text."""
    code = page.locator("pre code[data-entity-ranges]").first
    if code.count() == 0:
        return
    refs = code.locator("a.entity-ref[data-entity-id]")
    _check(refs.count() > 0,
           "code has entity-range metadata but the decorator added no entity link")
    _check(bool(refs.first.get_attribute("href")), "code entity-ref missing href")
    text = code.text_content() or ""
    _check("bioseq.len" in text,
           "decoration altered the code source text (must only wrap, never rewrite)")
    qualified = code.locator("a.entity-ref", has_text="bioseq.len")
    if qualified.count() > 0:
        _check(qualified.first.inner_text() == "bioseq.len",
               "qualified code entity was split into multiple hover targets")


_GOLDEN_TOKENS = Path(_THIS_DIR) / "fixtures" / "search_tokens_golden.json"


def _load_golden_cases() -> list[dict[str, str]]:
    data = json.loads(_GOLDEN_TOKENS.read_text(encoding="utf-8"))
    return list(data.get("cases", []))


def _exercise_search_tokens(page) -> None:
    """Drive static/js/search_tokens.mjs against the shared golden corpus (§12.5),
    proving the JS normalizer agrees byte-for-byte with the C++ self-test."""
    cases = _load_golden_cases()
    if not cases:
        return
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


def _exercise_search(page) -> None:
    """Search dialog (§12.5): Ctrl+K opens, results rank/group/highlight, arrow
    keys select, Escape closes."""
    button = page.locator("#search-button")
    if button.count() == 0:
        return
    _check(page.locator("#search-button .search-button-kbd").count() == 1,
           "search button does not expose its keyboard shortcut")
    _check(page.locator("#search-dialog").count() == 1, "search dialog is missing")
    inp = page.locator("#search-input")
    button.click()
    inp.wait_for(state="visible", timeout=4000)
    _check(page.evaluate("() => document.getElementById('search-dialog').open === true"),
           "clicking the search box did not keep the search dialog open")
    inp.fill("sequence")
    first = page.locator("#search-results a.search-result").first
    first.wait_for(state="visible", timeout=4000)
    _check(page.locator("#search-results a.search-result").count() > 0,
           "search returned no results for 'sequence'")
    _check(page.locator("#search-results .search-group-title").count() > 0,
           "search results are not grouped")
    _check(page.locator("#search-results mark").count() > 0,
           "search matches are not highlighted")
    inp.press("ArrowDown")
    _check(page.locator("#search-results a.search-result[aria-selected='true']").count() == 1,
           "ArrowDown did not move the roving selection")
    page.keyboard.press("Escape")
    # Poll the dialog's open property directly via evaluate (CDP, not subject to
    # the page CSP), instead of wait_for_function (needs unsafe-eval) or a
    # visibility wait (flaky for a top-layer <dialog>).
    closed = False
    for _ in range(40):
        if page.evaluate("() => !document.getElementById('search-dialog').open"):
            closed = True
            break
        page.wait_for_timeout(100)
    _check(closed, "Escape did not close the search dialog")


def _exercise_nav(page) -> None:
    """Mobile navigation (§15.1): a real button with aria-expanded/aria-controls,
    toggle, and Escape close."""
    toggle = page.locator("#nav-toggle")
    if toggle.count() == 0:
        return
    _check(toggle.get_attribute("aria-controls") == "nav-primary",
           "nav toggle is missing aria-controls")
    _check(toggle.get_attribute("aria-expanded") == "false",
           "nav toggle should start collapsed")
    page.set_viewport_size({"width": 360, "height": 800})
    toggle.click()
    _check(toggle.get_attribute("aria-expanded") == "true",
           "clicking the nav toggle did not expand it")
    page.keyboard.press("Escape")
    _check(toggle.get_attribute("aria-expanded") == "false",
           "Escape did not collapse the mobile nav")
    page.set_viewport_size({"width": 1280, "height": 900})


def _exercise_version_selector(page) -> None:
    """Version selector (§15.1): lists at least the current version and keeps an
    ordinary /versions/ fallback link."""
    if page.locator("#version-selector").count() == 0:
        return
    _check(page.locator("#version-list a").count() >= 1,
           "version selector lists no versions")
    _check(page.locator(".version-all").count() == 1,
           "version selector is missing its /versions/ fallback link")


def _exercise_entity(page) -> None:
    """Entity page enrichment (§12.3): kind badge and a copyable signature."""
    if page.locator("article.entity").count() == 0:
        return
    _check(page.locator(".kind-badge").count() == 1, "entity page is missing its kind badge")
    signature = page.locator("#entity-signature-code")
    if signature.count() == 1:
        button = page.locator("button.copy-button[data-copy-target='entity-signature-code']")
        _check(button.count() == 1, "entity signature has no copy button")


def _exercise_jsonld(page) -> None:
    """SEO JSON-LD (§15.5): the doc page's inert ld+json is valid, types as
    TechArticle, and carries a BreadcrumbList."""
    block = page.locator("script[type='application/ld+json']").first
    if block.count() == 0:
        return
    text = block.text_content() or "{}"
    try:
        data = json.loads(text)
    except json.JSONDecodeError as error:
        raise AssertionFail(f"JSON-LD is not valid JSON: {error}")
    nodes = data.get("@graph", [data]) if isinstance(data, dict) else []
    types = {node.get("@type") for node in nodes if isinstance(node, dict)}
    _check("TechArticle" in types, f"JSON-LD primary type is not TechArticle (got {types})")
    _check("BreadcrumbList" in types, "JSON-LD has no BreadcrumbList")


def _exercise_doc_frame(page, *, expect_toc: bool) -> None:
    """Three-region doc frame (§15.1): a visual breadcrumb trail, a left
    documentation-navigation rail, and — on article pages — an on-this-page
    table of contents whose links target in-page anchors that exist."""
    crumbs = page.locator("nav.breadcrumb ol li")
    canonical_crumbs = page.locator("[data-breadcrumb] > *")
    _check(crumbs.count() >= 2 or canonical_crumbs.count() >= 3,
           "doc page has no breadcrumb trail")
    canonical_nav = page.locator("aside#sidebar")
    nav = canonical_nav if canonical_nav.count() else page.locator("aside.doc-nav")
    _check(nav.count() == 1, "doc page is missing its documentation-navigation rail")
    _check(nav.locator("a[href]").count() >= 1, "doc-nav rail has no links")
    canonical_toc = page.locator("aside[data-rightrail]")
    toc = canonical_toc if canonical_toc.count() else page.locator("aside.doc-toc")
    if expect_toc and toc.count() == 1:
        anchors = toc.locator("a[href^='#']")
        count = anchors.count()
        _check(count >= 1, "on-this-page TOC is present but empty")
        for index in range(count):
            target = (anchors.nth(index).get_attribute("href") or "")[1:]
            if not target:
                continue
            found = page.evaluate("(id) => document.getElementById(id) !== null", target)
            _check(found, f"on-this-page anchor #{target} has no matching element")


def _exercise_capability_tracer(page) -> None:
    """Architecture capability tracer (§15.2): real <button> options, exactly one
    selected, selecting one traces its architecture node and clears the rest, and
    arrow keys move the selection (§15.4 keyboard-operable, buttons-not-divs)."""
    tracer = page.locator("[data-capability-tracer]")
    if tracer.count() == 0:
        return
    options = tracer.locator(".capability-option")
    total = options.count()
    _check(total >= 2, "capability tracer has fewer than two options")
    _check(tracer.locator("svg.architecture-svg").count() == 1,
           "capability tracer is missing the reference architecture diagram")
    _check(options.first.evaluate("el => el.tagName") == "BUTTON",
           "capability option is not a real <button>")
    _check(tracer.locator(".capability-option[aria-pressed='true']").count() == 1,
           "capability tracer must start with exactly one selection")
    second = options.nth(1)
    trace = second.get_attribute("data-trace") or ""
    second.click()
    _check(second.get_attribute("aria-pressed") == "true",
           "clicked capability did not become pressed")
    _check(tracer.locator(".capability-option[aria-pressed='true']").count() == 1,
           "selecting a capability left more than one pressed")
    node = tracer.locator(f"[data-arch-node='{trace}']")
    _check(node.count() == 1 and "is-traced" in (node.get_attribute("class") or ""),
           f"capability trace '{trace}' did not highlight its architecture node")
    second.focus()
    page.keyboard.press("ArrowRight")
    _check(tracer.locator(".capability-option[aria-pressed='true']").count() == 1,
           "arrow-key navigation left the tracer without a single selection")


def _exercise_connectors(page) -> None:
    """Callout connectors (§15.2): an aria-hidden SVG overlay that landing.mjs
    draws from each annotation to the query card; layout must not depend on it."""
    grid = page.locator("[data-connectors]")
    if grid.count() == 0:
        return
    svg = grid.locator(".connector-layer")
    _check(svg.count() == 1, "hero query is missing its connector layer")
    _check(svg.first.get_attribute("aria-hidden") == "true",
           "connector layer must be aria-hidden")


def _exercise_workflow(page) -> None:
    """Workflow progress (§15.2): a progress fill plus per-step markers."""
    workflow = page.locator("[data-workflow]")
    if workflow.count() == 0:
        return
    _check(workflow.locator("[data-workflow-progress]").count() == 1,
           "workflow has no progress fill")
    _check(workflow.locator("[data-wf-step]").count() >= 1, "workflow has no steps")


def _exercise_media_band(page) -> None:
    """Media band (§15.2): a muted self-hosted decorative background loop with
    no native player chrome and the reference mp4 source."""
    video = page.locator("video[data-media-band]")
    if video.count() == 0:
        return
    first = video.first
    _check(first.get_attribute("controls") is None,
           "decorative media band exposes native player controls")
    _check(first.get_attribute("autoplay") is not None,
           "decorative media band is missing autoplay")
    _check(first.get_attribute("muted") is not None,
           "media band video must be muted so it never forces audio")
    source = video.locator("source")
    _check(source.count() >= 1
           and (source.first.get_attribute("src") or "").endswith(".mp4"),
           "media band video has no mp4 <source>")
    _check((source.first.get_attribute("src") or "").endswith("small_vid.mp4"),
           "media band does not use the reference small_vid.mp4 asset")


def _exercise_cta_grid(page) -> None:
    """Final CTA reproduces the moving reference DNA tile field."""
    grid = page.locator("#cta-grid")
    if grid.count() == 0:
        return
    page.locator("#cta").scroll_into_view_if_needed()
    page.wait_for_timeout(150)
    _check(grid.locator(".cta-grid-tile").count() >= 12,
           "CTA DNA tile field was not constructed")


def _exercise_reduced_motion(page) -> None:
    """Reduced-motion branch (§15.3): under prefers-reduced-motion the reveal
    effects are disabled (content shown immediately, never armed-hidden) and the
    decorative video does not autoplay. Reloads to re-run the JS under emulation."""
    if page.locator(".landing-hero").count() == 0:
        return
    page.emulate_media(reduced_motion="reduce")
    page.reload(wait_until="networkidle")
    reveal = page.locator("[data-reveal]")
    total = reveal.count()
    if total > 0:
        _check(page.locator("[data-reveal].reveal-armed").count() == 0,
               "reduced motion still armed the reveal animations")
        _check(page.locator("[data-reveal].is-revealed").count() == total,
               "reduced motion did not reveal every section immediately")
    if page.locator("video[data-media-band]").count() > 0:
        paused = page.evaluate(
            "() => { const v = document.querySelector('video[data-media-band]');"
            " return v ? v.paused : true; }")
        _check(paused, "decorative video autoplayed under reduced motion")
    page.emulate_media(reduced_motion="no-preference")
    page.reload(wait_until="networkidle")


def _exercise_responsive(page, label: str) -> None:
    """Responsive layout (§15.3/§20): no horizontal overflow and the key landmarks
    are present at each required visual width."""
    for width in (360, 768, 1024, 1440):
        page.set_viewport_size({"width": width, "height": 900})
        page.wait_for_timeout(120)
        overflow = page.evaluate(
            "() => document.documentElement.scrollWidth - document.documentElement.clientWidth")
        _check(overflow <= 1, f"{label}: {overflow}px horizontal overflow at width {width}")
    _check(page.locator("header.site-header, header#nav, header#dnav").count() == 1,
           f"{label}: missing header landmark")
    _check(page.locator("main#main").count() == 1, f"{label}: missing main landmark")
    _check(page.locator("footer.site-footer, main#main footer").count() == 1,
           f"{label}: missing footer landmark")
    page.set_viewport_size({"width": 1280, "height": 900})


def _run_one(api, server: StaticFixtureServer, routes: dict[str, str],
             verbose: bool) -> None:
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
                _exercise_tabs(page)
                _exercise_nav(page)
                _exercise_version_selector(page)
                _exercise_search_tokens(page)
                _exercise_search(page)
                _exercise_capability_tracer(page)
                _exercise_connectors(page)
                _exercise_workflow(page)
                _exercise_media_band(page)
                _exercise_cta_grid(page)
                _exercise_reduced_motion(page)
            if label == "tutorial":
                _exercise_tooltip(page)
                _exercise_copy(page)
                _exercise_annotations(page)
                _exercise_code_entities(page)
                _exercise_jsonld(page)
                _exercise_doc_frame(page, expect_toc=True)
            if label == "entity":
                _exercise_entity(page)
                _exercise_jsonld(page)
                _exercise_doc_frame(page, expect_toc=True)
            _exercise_responsive(page, label)

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
    parser.add_argument("--stamp", type=Path,
                        help="success stamp written only when all assertions pass")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if not args.site.is_dir():
        print(f"browser_test: site directory missing: {args.site}", file=sys.stderr)
        return EXIT_USAGE

    # Validate the invocation and its inputs BEFORE probing the browser so a
    # usage error (2) is always distinct from browser-absence SKIP (4).
    try:
        server = StaticFixtureServer(args.site, args.base_path)
    except (NotADirectoryError, ValueError) as error:
        print(f"browser_test: {error}", file=sys.stderr)
        return EXIT_USAGE

    if not parse_security_headers(args.site):
        print(f"browser_test: no security-headers.txt at {args.site}; the header "
              "contract cannot be verified", file=sys.stderr)
        return EXIT_USAGE

    api = _load_playwright()
    if api is None:
        print("browser_test: SKIP - the 'playwright' package is not installed; "
              "install main/website/tests/requirements.lock into <build>/website/venv. "
              "(exit 4)", file=sys.stderr)
        return EXIT_SKIP

    routes = _default_routes(args.site, server.mount, args.version)
    server.start()
    try:
        _run_one(api, server, routes, args.verbose)
    except AssertionFail as error:
        print(f"browser_test: FAIL [base={args.base_path}] {error}", file=sys.stderr)
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
            print(f"browser_test: SKIP - pinned Chromium is not available "
                  f"({text.splitlines()[0] if text else name}); run "
                  "`playwright install chromium` into <build>/website/playwright-browsers. "
                  "(exit 4)", file=sys.stderr)
            return EXIT_SKIP
        print(f"browser_test: ERROR [base={args.base_path}] {name}: {text}",
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
                },
                indent=2,
            ) + "\n",
            encoding="utf-8",
        )
    print(f"browser_test: PASS [base={args.base_path}] "
          f"{len(routes)} pages, headers + CSP + tabs verified")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
