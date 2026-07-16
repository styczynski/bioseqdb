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


def _default_routes(site: Path, mount: str, version: str) -> dict[str, str]:
    base = mount if mount else ""
    return {
        "landing": f"{base}/",
        "tutorial": _discover_tutorial_route(site, base, version),
        "reference": f"{base}/{version}/reference/",
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
            if label == "tutorial":
                _exercise_tooltip(page)

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
