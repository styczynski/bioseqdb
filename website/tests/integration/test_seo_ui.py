#!/usr/bin/env python3
"""Focused generated-site audit for Q&A, cited sources and chapter navigation.

Run with the pinned browser environment, for example::

    build/website/browser-venv/bin/python website/tests/integration/test_seo_ui.py \
        --site artifacts/website --output build/seo-ui-audit

Screenshots and measured geometry are written outside the publishable site.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from support.static_fixture_server import StaticFixtureServer

WIDTHS = (320, 420, 768, 1024, 1600)
THEMES = ("dark", "light")


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def page_geometry(page):
    return page.evaluate("""() => {
      const box = el => { const r=el.getBoundingClientRect(); return {x:r.x,y:r.y,width:r.width,height:r.height,right:r.right,bottom:r.bottom}; };
      const selectors=['.site-brand','.site-navigation','.site-search-wrap','.site-actions','.site-menu'];
      const header=selectors.map(s=>({selector:s,el:document.querySelector(s)})).filter(x=>x.el && x.el.getBoundingClientRect().width && getComputedStyle(x.el).display!=='none').map(x=>({selector:x.selector,...box(x.el)}));
      const overlaps=[];
      for(let a=0;a<header.length;a++) for(let b=a+1;b<header.length;b++) {
        const x=header[a],y=header[b];
        if(Math.min(x.right,y.right)-Math.max(x.x,y.x)>1 && Math.min(x.bottom,y.bottom)-Math.max(x.y,y.y)>1) overlaps.push([x.selector,y.selector]);
      }
      return {viewport:innerWidth,scrollWidth:document.documentElement.scrollWidth,header,overlaps,theme:document.documentElement.dataset.theme};
    }""")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", type=Path, required=True)
    parser.add_argument("--base-path", default="/")
    parser.add_argument("--version", default="v0.1")
    parser.add_argument("--output", type=Path, default=Path("build/seo-ui-audit"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    from playwright.sync_api import sync_playwright

    report = {"site": str(args.site.resolve()), "base_path": args.base_path, "layouts": [], "errors": []}
    with StaticFixtureServer(args.site, args.base_path) as server, sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        try:
            for theme in THEMES:
                for width in WIDTHS:
                    context = browser.new_context(viewport={"width": width, "height": 960}, color_scheme="light" if theme == "dark" else "dark", reduced_motion="reduce")
                    context.add_init_script(f"localStorage.setItem('bioseq-theme', {json.dumps(theme)});")
                    context.route("**/*", lambda route: route.continue_() if urlsplit(route.request.url).hostname in ("127.0.0.1", "localhost") else route.abort())
                    page = context.new_page()
                    errors = []
                    page.on("pageerror", lambda error: errors.append(str(error)))
                    prefix = args.base_path.rstrip("/")
                    def open_page(route):
                        response = page.goto(server.origin + prefix + route, wait_until="networkidle")
                        check(response and response.status == 200, f"Page failed: {route}")
                        page.evaluate("document.fonts.ready")
                        page.locator('.site-navigation a[href$="/qa/"]').wait_for(state="visible")
                        geometry = page_geometry(page)
                        check(geometry["theme"] == theme, "Saved theme was not respected")
                        check(geometry["scrollWidth"] <= width + 1, f"Horizontal page overflow: {route}: {geometry}")
                        check(not geometry["overlaps"], f"Header overlaps: {route}: {geometry['overlaps']}")
                        check(all(x["x"] >= -1 and x["right"] <= width + 1 for x in geometry["header"]), f"Header clipped: {route}")
                        return geometry

                    qa = open_page("/qa/")
                    questions = page.locator(".qa-question")
                    check(questions.count() >= 30, "Q&A must expose at least 30 questions")
                    check(page.locator(".qa-question h3").count() == questions.count(), "Question headings missing")
                    answers = page.locator(".qa-answer").all_text_contents()
                    check(all(len(x.strip()) >= 40 for x in answers), "Q&A contains an empty/placeholder answer")
                    qa["questions"] = questions.count()
                    page.screenshot(path=str(args.output / f"qa-{theme}-{width}.png"))
                    # Exact question text must be reachable through the shared search.
                    question = page.locator(".qa-question h3").first.inner_text().rstrip("#").strip()
                    page.locator("#doc-search").fill(question)
                    result = page.locator('[data-search-result][data-search-kind="question"]').first
                    result.wait_for(state="visible")
                    check(result.locator(".doc-search-badge").inner_text() == "?", "Question search badge must be ?")
                    check(result.locator(".doc-search-kind").inner_text() == "Question", "Question result label incorrect")
                    if width in (420, 1600):
                        page.screenshot(path=str(args.output / f"question-search-{theme}-{width}.png"))
                    result.click()
                    check("/qa/#" in page.url, "Question search did not open its answer")
                    check(page.locator(page.url[page.url.index("#"):]).count() == 1, "Question search anchor missing")

                    docs = open_page(f"/{args.version}/tutorials/project_alignment_foundations/")
                    sources = page.locator(".document-sources")
                    check(sources.count() == 1 and sources.is_visible(), "Sources section must be visible")
                    source_links = sources.locator("a").evaluate_all("nodes => nodes.map(x => ({href:x.getAttribute('href'),text:x.textContent}))")
                    hrefs = [x["href"] for x in source_links]
                    check(len(hrefs) >= 5, "Cited chapter sources were not resolved")
                    check(len(hrefs) == len(set(hrefs)), "Sources section contains duplicate URLs")
                    check(all(urlsplit(x).scheme in ("http", "https") and urlsplit(x).netloc != "bioseqdb.com" for x in hrefs), "Sources must link directly to external pages")
                    check(any("10.1093/bioinformatics/bty191" in x for x in hrefs), "The cited Minimap2 bibliography URL is absent")
                    check(all(x["text"].strip() for x in source_links), "Empty external link label")
                    sources.scroll_into_view_if_needed()
                    page.screenshot(path=str(args.output / f"sources-{theme}-{width}.png"))
                    pager = page.locator(".tutorial-pagination")
                    pager.scroll_into_view_if_needed()
                    cards = pager.locator(".tutorial-pagination-link")
                    check(cards.count() == 2, "Chapter must have previous and next links")
                    boxes = [cards.nth(n).bounding_box() for n in range(2)]
                    check(all(44 <= x["height"] <= 140 for x in boxes), f"Pager cards too large/small: {boxes}")
                    if width >= 666:
                        check(abs(boxes[0]["y"] - boxes[1]["y"]) <= 1, f"Pager should be two columns: {boxes}")
                        check(boxes[0]["x"] + boxes[0]["width"] < boxes[1]["x"], "Pager cards overlap")
                    else:
                        check(boxes[1]["y"] >= boxes[0]["y"] + boxes[0]["height"], "Mobile pager should stack")
                    page.screenshot(path=str(args.output / f"pager-{theme}-{width}.png"))
                    docs["source_links"] = source_links
                    docs["pager"] = boxes

                    first = open_page(f"/{args.version}/tutorials/getting_started/")
                    pager = page.locator(".tutorial-pagination")
                    check(pager.locator(".tutorial-pagination-link").count() == 1, "First chapter should only have Next")
                    next_card = pager.locator(".tutorial-pagination-next")
                    pager.scroll_into_view_if_needed()
                    parent_box, next_box = pager.bounding_box(), next_card.bounding_box()
                    if width >= 666:
                        check(next_box["x"] >= parent_box["x"] + parent_box["width"] / 2 - 1, "Next-only pager must stay on the right")
                    check(next_box["height"] <= 140, "Next-only card is oversized")
                    check(page_geometry(page)["scrollWidth"] <= width + 1, "Next-only page overflows")
                    page.screenshot(path=str(args.output / f"next-only-{theme}-{width}.png"))
                    first["pager"] = next_box
                    check(not errors, f"Browser errors: {errors}")
                    report["layouts"].append({"theme": theme, "width": width, "qa": qa, "chapter": docs, "first_chapter": first})
                    context.close()
        except Exception as error:
            report["errors"].append(str(error))
            raise
        finally:
            browser.close()
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"SEO UI audit passed: {len(report['layouts'])} theme/width combinations; screenshots in {args.output}")


if __name__ == "__main__":
    main()
