#!/usr/bin/env python3
"""Loopback static fixture server for the generated-site browser tests (§15.5,
§20.7).

Two responsibilities the Playwright harness relies on:

1. Header contract. The generated site ships ``security-headers.txt`` (§15.5): a
   host-agnostic sample of the headers a real deployment must add, including the
   header-only ``frame-ancestors`` directive plus ``X-Content-Type-Options``,
   ``Referrer-Policy`` and ``Permissions-Policy``. Static files cannot enforce
   these, so this fixture server reads that file from the site root and APPLIES
   every listed header to every response. ``test_browser.py`` then
   asserts the real response headers match the artifact.

2. Base-path mount. A site rendered for base path ``/bioseqdb/`` already carries
   that prefix inside every internal URL, so it must be *served* at that mount:
   ``GET /bioseqdb/index.html`` maps to ``<root>/index.html`` and anything
   outside the prefix is a 404. Mounting one byte tree at two prefixes is NOT a
   base-path test (§20.7); each base path is a separately rendered tree served
   here at its own mount.

The server binds to 127.0.0.1 on an ephemeral port, serves only regular files
under the (resolved) root, and never touches the network. It can also be run
standalone for manual debugging::

    python website/tests/support/static_fixture_server.py \
        --site <root> --base-path /bioseqdb/ --port 0
"""

from __future__ import annotations

import argparse
import http.server
import mimetypes
import threading
from pathlib import Path

# Extension -> Content-Type overrides so ES modules and JSON are served with the
# types the CSP (script-src 'self') and browsers expect.
_CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".json": "application/json",
    ".svg": "image/svg+xml",
    ".xml": "application/xml",
    ".txt": "text/plain; charset=utf-8",
    ".md": "text/plain; charset=utf-8",
    ".woff2": "font/woff2",
    ".png": "image/png",
    ".webp": "image/webp",
    ".mp4": "video/mp4",
    ".map": "application/json",
}


def parse_security_headers(root: Path) -> list[tuple[str, str]]:
    """Parse ``<root>/security-headers.txt`` into an ordered header list.

    Format: one ``Header-Name: value`` per line; blank lines and lines starting
    with ``#`` are ignored. A missing file yields an empty list (the caller can
    decide whether that is acceptable).
    """
    path = root / "security-headers.txt"
    headers: list[tuple[str, str]] = []
    if not path.is_file():
        return headers
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if ":" not in line:
            raise ValueError(f"security-headers.txt: malformed line: {raw!r}")
        name, value = line.split(":", 1)
        headers.append((name.strip(), value.strip()))
    return headers


def content_type_for(path: Path) -> str:
    suffix = path.suffix.lower()
    if suffix in _CONTENT_TYPES:
        return _CONTENT_TYPES[suffix]
    guessed, _ = mimetypes.guess_type(path.name)
    return guessed or "application/octet-stream"


class StaticFixtureServer:
    """A threaded loopback HTTP server for one rendered site at one base path."""

    def __init__(self, root: Path, base_path: str = "/", host: str = "127.0.0.1",
                 port: int = 0) -> None:
        self.root = Path(root).resolve()
        if not self.root.is_dir():
            raise NotADirectoryError(f"site root is not a directory: {self.root}")
        # mount is "" for "/" and "/bioseqdb" for "/bioseqdb/".
        self.base_path = base_path if base_path.startswith("/") else "/" + base_path
        self.mount = self.base_path.rstrip("/")
        self.extra_headers = parse_security_headers(self.root)
        self._host = host
        self._port = port
        self._httpd: http.server.ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None

    # -- request handler -----------------------------------------------------
    def _make_handler(self):
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):  # keep the test output clean
                return

            def _resolve(self):
                # Strip query/fragment; the path is already percent-decoded by
                # the base handler's ``self.path``? It is not, so decode here.
                from urllib.parse import unquote, urlsplit

                url_path = unquote(urlsplit(self.path).path)
                if server.mount:
                    if url_path != server.mount and not url_path.startswith(
                            server.mount + "/"):
                        return None
                    sub = url_path[len(server.mount):]
                else:
                    sub = url_path
                if sub in ("", "/"):
                    sub = "/index.html"
                if sub.endswith("/"):
                    sub += "index.html"
                rel = sub.lstrip("/")
                candidate = (server.root / rel).resolve()
                # Confinement: never serve outside the resolved root.
                if server.root != candidate and server.root not in candidate.parents:
                    return None
                if candidate.is_dir():
                    candidate = candidate / "index.html"
                if not candidate.is_file():
                    return None
                return candidate

            def _emit(self, status: int, body: bytes, content_type: str):
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                for name, value in server.extra_headers:
                    self.send_header(name, value)
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(body)

            def do_GET(self):
                target = self._resolve()
                if target is None:
                    self._emit(404, b"not found", "text/plain; charset=utf-8")
                    return
                self._emit(200, target.read_bytes(), content_type_for(target))

            do_HEAD = do_GET

        return Handler

    # -- lifecycle -----------------------------------------------------------
    def start(self) -> "StaticFixtureServer":
        self._httpd = http.server.ThreadingHTTPServer(
            (self._host, self._port), self._make_handler())
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        if self._httpd is not None:
            self._httpd.shutdown()
            self._httpd.server_close()
            self._httpd = None
        if self._thread is not None:
            self._thread.join(timeout=5)
            self._thread = None

    @property
    def port(self) -> int:
        assert self._httpd is not None, "server not started"
        return self._httpd.server_address[1]

    @property
    def origin(self) -> str:
        return f"http://{self._host}:{self.port}"

    def url_for(self, route: str) -> str:
        if not route.startswith("/"):
            route = "/" + route
        return self.origin + route

    def __enter__(self) -> "StaticFixtureServer":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Static fixture server (debug).")
    parser.add_argument("--site", required=True, type=Path)
    parser.add_argument("--base-path", default="/")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args(argv)
    server = StaticFixtureServer(args.site, args.base_path, args.host, args.port).start()
    print(f"serving {server.root} at {server.origin}{server.mount}/ "
          f"(headers: {len(server.extra_headers)})")
    try:
        while True:
            import time
            time.sleep(3600)
    except KeyboardInterrupt:
        server.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
