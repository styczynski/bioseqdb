"""Offline Shields fixtures and responsive landing badge acceptance."""
from __future__ import annotations

import json
import time
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

FIXTURES = Path(__file__).resolve().parents[1] / "fixtures/shields"
WIDTHS = (320, 420, 568, 666, 768, 1024, 1300, 1600, 1920, 2560)
ENDPOINTS = {
    "/github/stars/styczynski/bioseqdb": "github",
    "/sourceforge/dt/bioseqdb": "sourceforge",
    "/docker/pulls/styczynski/bioseqdb": "docker",
    "/badge/version-0.1.0--alpha.1-a89ce6": "version",
    "/badge/license-Apache%202.0-d8a15f": "license",
}
DESTINATIONS = {
    "github": "https://github.com/styczynski/bioseqdb/stargazers",
    "sourceforge": "https://sourceforge.net/projects/bioseqdb/files/",
    "docker": "https://hub.docker.com/r/styczynski/bioseqdb",
    "version": "https://sourceforge.net/projects/bioseqdb/files/",
    "license": "https://github.com/styczynski/bioseqdb/blob/main/LICENSE",
}


def _wait(page, expression):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if page.evaluate(expression):
            return
        page.wait_for_timeout(40)
    raise AssertionError("badge images did not settle")


def serve_badge(route):
    parsed = urlsplit(route.request.url)
    if parsed.hostname != "img.shields.io":
        return False
    name = ENDPOINTS.get(parsed.path)
    assert name, f"unexpected Shields endpoint: {parsed.path}"
    params = parse_qs(parsed.query)
    theme = "light" if params.get("labelColor") == ["e2eaf2"] else "dark"
    style = params.get("style", [""])[0]
    assert style in ("flat-square", "for-the-badge"), "unexpected badge style"
    route.fulfill(status=200, content_type="image/svg+xml", body=(
        FIXTURES / f"{name}-{theme}-{style}.svg").read_bytes())
    return True


def exercise_badges(browser, server, base_path, artifact_dir):
    output = Path(artifact_dir) / "badges"
    output.mkdir(parents=True, exist_ok=True)
    # The OS says light; the website must still default to its chosen dark theme.
    context = browser.new_context(color_scheme="light")
    failure = {"enabled": False}

    def offline(route):
        url = urlsplit(route.request.url)
        if url.hostname in ("localhost", "127.0.0.1", server._host):
            route.continue_()
        elif url.hostname == "img.shields.io":
            if failure["enabled"]:
                route.abort()
            else:
                serve_badge(route)
        elif url.hostname == "www.googletagmanager.com":
            route.fulfill(status=200, content_type="application/javascript", body="/* offline */")
        else:
            route.abort()

    context.route("**/*", offline)
    page = context.new_page()
    errors, reports = [], []
    page.on("pageerror", lambda error: errors.append(str(error)))
    page.goto(server.url_for(base_path), wait_until="networkidle")
    assert page.locator("html").get_attribute("data-theme") == "dark"
    for theme in ("dark", "light"):
        if theme == "light":
            page.locator("[data-theme-toggle]").click()
        for width in WIDTHS:
            page.set_viewport_size({"width": width, "height": 1000})
            _wait(page, """() => [...document.querySelectorAll('[data-badge-image]')].every(img =>
              img.complete && img.naturalWidth > 0 && new URL(img.src).searchParams.get('style') ===
                (innerWidth >= 1600 ? 'for-the-badge' : 'flat-square'))""")
            page.evaluate("() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)))")
            assert page.locator("[data-project-badge]").count() == 5
            for key, destination in DESTINATIONS.items():
                link = page.locator(f'[data-project-badge="{key}"]')
                assert link.get_attribute("href") == destination
                assert link.get_attribute("aria-label") and link.get_attribute("title")
                params = parse_qs(urlsplit(link.locator("img").get_attribute("src")).query)
                assert params["labelColor"] == ["e2eaf2" if theme == "light" else "142b3c"]
            boxes = page.evaluate("""() => {
              const box = node => { const b = node.getBoundingClientRect(); return {x:b.x,y:b.y,right:b.right,bottom:b.bottom}; };
              return {header:box(document.querySelector('[data-site-header]')),
                label:box(document.querySelector('.ld-s0020')),
                badges:[...document.querySelectorAll('[data-project-badge]')].map(box),
                overflow:document.documentElement.scrollWidth-innerWidth};
            }""")
            assert boxes["overflow"] <= 1, f"{theme}/{width}: horizontal overflow"
            for index, badge in enumerate(boxes["badges"]):
                assert badge["x"] >= 0 and badge["right"] <= width, f"{theme}/{width}: badge clips"
                assert badge["y"] >= boxes["header"]["bottom"], f"{theme}/{width}: badge behind header"
                assert badge["bottom"] <= boxes["label"]["y"], f"{theme}/{width}: badge overlaps hero"
                for other in boxes["badges"][index+1:]:
                    assert (min(badge["right"], other["right"]) <= max(badge["x"], other["x"])
                            or min(badge["bottom"], other["bottom"]) <= max(badge["y"], other["y"])), "badges overlap"
            page.screenshot(path=str(output / f"{theme}-{width}.png"), animations="disabled")
            reports.append({"theme": theme, "width": width, "geometry": boxes})
    page.reload(wait_until="networkidle")
    assert page.locator("html").get_attribute("data-theme") == "light"
    assert "labelColor=e2eaf2" in page.locator("[data-badge-image]").first.get_attribute("src")
    failure["enabled"] = True
    page.reload(wait_until="networkidle")
    assert page.locator("[data-project-badge][data-unavailable]").count() == 5
    for link in page.locator("[data-project-badge]").all():
        assert link.locator(".landing-badge-fallback").is_visible()
    page.screenshot(path=str(output / "unavailable.png"), animations="disabled")
    failure["enabled"] = False
    page.locator("[data-theme-toggle]").click()
    _wait(page, "() => document.querySelectorAll('[data-project-badge][data-unavailable]').length === 0")
    assert not errors, errors
    context.close()
    nojs = browser.new_context(java_script_enabled=False)
    nojs.route("**/*", offline)
    page = nojs.new_page()
    page.goto(server.url_for(base_path), wait_until="networkidle")
    assert page.locator("[data-project-badge]").count() == 5
    assert page.locator("[data-badge-image]").evaluate_all("nodes => nodes.every(n => n.complete && n.naturalWidth > 0)")
    nojs.close()
    (output / "audit.json").write_text(json.dumps(reports, indent=2))
