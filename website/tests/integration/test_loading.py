#!/usr/bin/env python3
"""Audit conditional website assets and their interaction ordering.

Use the pinned Playwright environment and a generated tree::

    python website/tests/integration/test_loading.py --site artifacts/website

Timing measurements are recorded as evidence, not brittle pass/fail budgets.
All external services are stubbed; no analytics events leave this test.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import time
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from support.static_fixture_server import StaticFixtureServer


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def wait_for(page, predicate, timeout=30):
    # Playwright's wait_for_function evaluates its predicate through eval in
    # the page realm, which our production CSP correctly forbids. Evaluate
    # directly through the browser protocol while retaining the real CSP.
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if page.evaluate(predicate):
            return
        page.wait_for_timeout(25)
    raise AssertionError(f"Condition did not become true: {predicate}")


def local_requests(route):
    host = urlsplit(route.request.url).hostname
    if host in ("127.0.0.1", "localhost"):
        route.continue_()
    elif host == "img.shields.io":
        route.fulfill(status=200, content_type="image/svg+xml", body='<svg xmlns="http://www.w3.org/2000/svg" width="110" height="20"><rect width="110" height="20" fill="#28425b"/></svg>')
    else:
        route.fulfill(status=204, body="")


def evidence(page):
    return page.evaluate("""() => ({
      navigation: performance.getEntriesByType('navigation')[0]?.toJSON(),
      paints: performance.getEntriesByType('paint').map(x => x.toJSON()),
      resources: performance.getEntriesByType('resource').map(x => ({url:x.name,type:x.initiatorType,start:x.startTime,duration:x.duration,bytes:x.decodedBodySize,blocking:x.renderBlockingStatus})),
      scripts: [...document.scripts].filter(x => x.src).map(x => ({src:x.src,type:x.type,defer:x.defer,async:x.async})),
      nodes: document.querySelectorAll('*').length,
      highlightedTokens: document.querySelectorAll('pre code .token').length,
      entityLinks: document.querySelectorAll('pre code .code-entity-ref').length
    })""")


def check_deferred_scripts(data):
    blocking = [s["src"] for s in data["scripts"] if s["type"] != "module" and not s["defer"] and not s["async"] and not s["src"].endswith("/js/theme.js")]
    check(not blocking, f"Unexpected parser-blocking scripts: {blocking}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", type=Path, required=True)
    parser.add_argument("--base-path", default="/")
    parser.add_argument("--version", default="v0.1")
    parser.add_argument("--output", type=Path, default=Path("build/loading-audit"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    from playwright.sync_api import sync_playwright

    report = {"site": str(args.site.resolve()), "conditions": "Local uncompressed server; cold contexts; external analytics/badges stubbed; timing is informational.", "pages": {}, "media": {}, "errors": []}
    prefix = args.base_path.rstrip("/")
    with StaticFixtureServer(args.site, args.base_path) as server, sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        contexts = []

        def context(motion="reduce", save_data=False):
            ctx = browser.new_context(viewport={"width": 1365, "height": 900}, reduced_motion=motion)
            contexts.append(ctx)
            ctx.route("**/*", local_requests)
            if save_data:
                ctx.add_init_script("Object.defineProperty(navigator,'connection',{value:{saveData:true,addEventListener(){}}});")
            page = ctx.new_page()
            errors, requests = [], []
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.on("request", lambda request: requests.append(request.url))
            return ctx, page, errors, requests

        def open_page(page, route):
            response = page.goto(server.origin + prefix + route, wait_until="networkidle")
            check(response and response.status == 200, f"Page failed: {route}")
            page.evaluate("document.fonts.ready")

        try:
            for name, route in (("landing", "/"), ("qa", "/qa/"), ("blog", "/blog/introducing-bioseqdb/")):
                ctx, page, errors, requests = context()
                if name == "landing":
                    pending_prism = []
                    page.route("**/vendor/prism/prism.min.js", lambda route: pending_prism.append(route))
                    page.goto(server.origin + prefix + route, wait_until="commit")
                    code = page.locator("[data-annotated-query]")
                    code.wait_for(state="attached")
                    check("SELECT" in code.inner_text(), "The SQL example is unreadable before syntax highlighting")
                    wait_for(page, "() => document.readyState === 'interactive'")
                    check(pending_prism, "Landing did not request the delayed syntax renderer")
                    for pending in pending_prism:
                        pending.continue_()
                    page.wait_for_load_state("networkidle")
                    page.evaluate("document.fonts.ready")
                else:
                    open_page(page, route)
                check(page.locator("[data-math]").count() == 0, f"The {name} fixture now contains math; choose a no-math fixture")
                unused = [url for url in requests if any(part in url for part in ("/vendor/katex/", "/js/project-math.js", "/js/project-benchmarks.mjs", "/js/benchmarks-design.mjs", "/js/project-benchmark-", "/css/project-benchmarks.css", "/project-benchmarks.json"))]
                check(not unused, f"{name} fetched unused math/benchmark dependencies: {unused}")
                data = evidence(page)
                check_deferred_scripts(data)
                if name in ("landing", "blog"):
                    check(data["highlightedTokens"] > 0, f"Syntax highlighting missing on {name}")
                    check(data["entityLinks"] > 0, f"SQL reference links missing on {name}")
                    entity = page.locator("pre code .code-entity-ref[data-entity-data-url]").first
                    entity.hover()
                    wait_for(page, "() => document.querySelector('#entity-pop')?.getAttribute('aria-hidden') === 'false'")
                    check(page.locator("#entity-pop").inner_text().strip(), f"SQL tooltip empty on {name}")
                    page.keyboard.press("Escape")
                if name == "landing":
                    for token, text in (("select", "SELECT"), ("where", "WHERE"), ("dna4", "'dna4'")):
                        hook = page.locator(f"#tok-{token}")
                        check(hook.count() == 1 and hook.inner_text() == text, f"Landing {token} annotation lost after Prism")
                        check(page.locator(f"#note-{token}").count() == 1, f"Landing {token} explanation missing")
                    page.locator('[data-example-tab="1"]').click()
                    panels = page.locator('[data-example-panel="1"]')
                    check(panels.count() > 0 and panels.evaluate_all("nodes => nodes.every(node => !node.hidden)"), "Landing example tabs failed after deferring syntax scripts")
                    check(page.locator('[data-example-panel="1"][data-sql-example-code]').is_visible(), "Selected SQL example is hidden")
                    check(not any(urlsplit(url).path.endswith(".mp4") for url in requests), "Reduced-motion landing fetched decorative video")
                    page.locator("[data-annotated-query]").scroll_into_view_if_needed()
                    page.screenshot(path=str(args.output / "landing-annotations.png"))
                check(not errors, f"{name} errors: {errors}")
                report["pages"][name] = data
                ctx.close()

            # Hold KaTeX until the fallback has been observed. Content must be
            # readable while the optional renderer is still in flight.
            ctx, page, errors, requests = context()
            pending_math = []
            page.route("**/vendor/katex/katex.min.js", lambda route: pending_math.append(route))
            page.goto(server.origin + prefix + f"/{args.version}/tutorials/project_alignment_foundations/", wait_until="commit")
            first_math = page.locator("[data-math]").first
            first_math.wait_for(state="attached")
            first_math.scroll_into_view_if_needed()
            wait_for(page, "() => document.querySelector('[data-math]')?.textContent.trim().length > 0")
            check(first_math.is_visible(), "Math fallback is hidden while KaTeX is unavailable")
            check(first_math.locator(".katex").count() == 0, "Delayed math fixture unexpectedly rendered")
            fallback = first_math.inner_text()
            page.screenshot(path=str(args.output / "math-fallback.png"))
            wait_for(page, "() => performance.getEntriesByType('resource').some(x=>x.name.includes('project-math')) || document.querySelector('script[src*=\"katex.min.js\"]')")
            check(pending_math, "The math page did not request KaTeX")
            for route in pending_math:
                route.continue_()
            first_math.locator(".katex math").wait_for(state="attached")
            page.wait_for_load_state("networkidle")
            page.screenshot(path=str(args.output / "math-rendered.png"))
            report["pages"]["math"] = {**evidence(page), "fallback": fallback, "rendered": first_math.locator(".katex").count()}
            check_deferred_scripts(report["pages"]["math"])
            check(not errors, f"Math errors: {errors}")
            ctx.close()

            ctx, page, errors, requests = context()
            open_page(page, "/benchmarks/")
            cell = page.locator("#benchmarks-app td.bd-s0072[tabindex='0']").first
            cell.wait_for(state="visible", timeout=30000)
            cell.hover()
            wait_for(page, "() => Number(getComputedStyle(document.querySelector('#benchmarks-app [data-benchmark-tip]')).opacity) > .9")
            tooltip = page.locator("#benchmarks-app [data-benchmark-tip]")
            check("vs baseline" in tooltip.inner_text(), "Benchmark comparison tooltip is missing its baseline comparison")
            report["pages"]["benchmarks"] = {**evidence(page), "tooltip": tooltip.inner_text(), "tableCells": page.locator("#benchmarks-app td").count()}
            page.screenshot(path=str(args.output / "benchmark-tooltip.png"))
            page.keyboard.press("Escape")
            wait_for(page, "() => Number(getComputedStyle(document.querySelector('#benchmarks-app [data-benchmark-tip]')).opacity) === 0")
            check_deferred_scripts(report["pages"]["benchmarks"])
            check(not errors, f"Benchmark errors: {errors}")
            ctx.close()

            # The hero may load only after its section is visible. The lower
            # decorative clip must remain unrequested until scrolling to it.
            for mode, motion, save_data in (("normal", "no-preference", False), ("reduced", "reduce", False), ("save-data", "no-preference", True)):
                ctx, page, errors, requests = context(motion, save_data)
                open_page(page, "/")
                videos = page.locator("video[data-autobg]")
                check(videos.count() == 2, "Landing media fixture changed")
                if mode == "normal":
                    wait_for(page, "() => document.querySelector('video[data-autobg] source')?.hasAttribute('src')")
                    check(any("/media/vials.mp4" in url for url in requests), "Visible hero video never requested")
                    check(not any("/media/small_vid.mp4" in url for url in requests), "Offscreen decorative video loaded eagerly")
                    videos.nth(1).scroll_into_view_if_needed()
                    wait_for(page, "() => [...document.querySelectorAll('video[data-autobg]')][1]?.querySelector('source')?.hasAttribute('src')")
                    page.wait_for_load_state("networkidle")
                    check(any("/media/small_vid.mp4" in url for url in requests), "Visible lower video never requested")
                else:
                    videos.nth(1).scroll_into_view_if_needed()
                    page.evaluate("() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))")
                    check(not any(urlsplit(url).path.endswith(".mp4") for url in requests), f"{mode} fetched decorative video")
                    check(videos.locator("source[src]").count() == 0, f"{mode} assigned media sources")
                check(not errors, f"{mode} media errors: {errors}")
                report["media"][mode] = {"requests": [url for url in requests if "/media/" in url], "sources": videos.locator("source").evaluate_all("nodes => nodes.map(x=>({src:x.getAttribute('src'),pending:x.dataset.src}))")}
                ctx.close()
        except Exception as error:
            report["errors"].append(str(error))
            raise
        finally:
            for ctx in contexts:
                ctx.close()
            browser.close()
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Loading audit passed: {len(report['pages'])} page cases, {len(report['media'])} media preferences; evidence in {args.output}")


if __name__ == "__main__":
    main()
