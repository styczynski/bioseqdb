#!/usr/bin/env python3
"""Capture each rendered page with pinned Chromium for its SEO thumbnail.

The native renderer imports these JPEGs on its final pass, so its ordinary
manifest and validation gate cover the images as well as the HTML.
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import shutil
import threading
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path, PurePosixPath
from urllib.parse import quote, urlsplit

from playwright.async_api import async_playwright

WIDTH, HEIGHT = 1200, 630


class PreviewHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, base_path="/", **kwargs):
        self.mount = base_path
        super().__init__(*args, **kwargs)

    def do_GET(self):
        path = urlsplit(self.path).path
        if not path.startswith(self.mount):
            self.send_error(404)
            return
        self.path = "/" + path[len(self.mount):]
        super().do_GET()

    def log_message(self, *_args):
        pass


def thumbnail_path(route: str) -> Path:
    path = PurePosixPath(route.removeprefix("/"))
    if path.is_absolute() or ".." in path.parts or path.suffix != ".html":
        raise ValueError(f"Invalid HTML route: {route}")
    # Directory pages and the standalone error page both have stable names.
    return Path(path.parent / ("thumbnail.jpg" if path.name == "index.html"
                               else path.stem + "-thumbnail.jpg"))


async def capture(args, manifest, origin):
    routes = sorted(route for route in manifest["outputs"] if route.endswith(".html"))
    browser_signature = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    args.cache.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    queue = asyncio.Queue()
    for route in routes:
        queue.put_nowait(route)
    records = []
    async with async_playwright() as playwright:
        browser = await playwright.chromium.launch(headless=True)
        async def worker():
            context = await browser.new_context(
                viewport={"width": WIDTH, "height": HEIGHT}, device_scale_factor=1,
                color_scheme="light", reduced_motion="reduce", service_workers="block",
            )
            await context.add_init_script("localStorage.setItem('bioseq-theme', 'light')")
            async def handle(route):
                host = urlsplit(route.request.url).hostname
                if host in {"127.0.0.1", "img.shields.io"}:
                    await route.continue_()
                else:
                    # Capturing a build must not send analytics or submit forms.
                    await route.fulfill(status=204, body="")
            await context.route("**/*", handle)
            page = await context.new_page()
            while not queue.empty():
                route = queue.get_nowait()
                relative = thumbnail_path(route)
                destination = args.output / relative
                signature = hashlib.sha256((manifest["outputs"][route]
                    + manifest["asset_bundle_sha256"] + browser.version
                    + browser_signature).encode()).hexdigest()
                cached = args.cache / (signature + ".jpg")
                destination.parent.mkdir(parents=True, exist_ok=True)
                if not cached.exists():
                    mounted = manifest.get("base_path", "/") + route.lstrip("/")
                    response = await page.goto(origin + quote(mounted, safe="/"),
                                               wait_until="networkidle", timeout=45000)
                    if not response or response.status != 200:
                        raise RuntimeError(f"Thumbnail page failed: {route}")
                    await page.evaluate("document.fonts.ready")
                    await page.locator("h1").first.wait_for(state="visible")
                    # Readers see source TeX while the optional renderer loads.
                    # Sharing previews should capture the finished formula instead.
                    if await page.locator("[data-math]").count():
                        await page.locator("html[data-math-state='ready']").wait_for(
                            state="attached", timeout=45000)
                    await page.screenshot(path=str(cached), type="jpeg", quality=80,
                                          animations="disabled", caret="hide")
                shutil.copyfile(cached, destination)
                records.append({"page": route, "image": "/" + relative.as_posix(),
                                "width": WIDTH, "height": HEIGHT,
                                "sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
                                "source_sha256": manifest["outputs"][route]})
                queue.task_done()
                if len(records) % 50 == 0:
                    print(f"Website thumbnails: {len(records)}/{len(routes)}", flush=True)
            await context.close()
        await asyncio.gather(*(worker() for _ in range(args.workers)))
        await browser.close()
    expected = {args.output / thumbnail_path(route) for route in routes}
    for old in args.output.rglob("*.jpg"):
        if old not in expected:
            old.unlink()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({"schema": "bioseq.website.thumbnails.v1",
        "source_revision": manifest["source_revision"],
        "pages": sorted(records, key=lambda item: item["page"])}, indent=2) + "\n")
    print(f"Website thumbnails: {len(records)} rendered pages ready")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=4, choices=range(1, 9))
    args = parser.parse_args()
    args.site, args.output = args.site.resolve(), args.output.resolve()
    if args.site == args.output or args.site in args.output.parents:
        raise SystemExit("Thumbnails must be generated outside the preview site")
    manifest = json.loads((args.site / "site-manifest.json").read_text())
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(PreviewHandler,
        directory=str(args.site), base_path=manifest.get("base_path", "/")))
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        asyncio.run(capture(args, manifest, f"http://127.0.0.1:{server.server_port}"))
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
