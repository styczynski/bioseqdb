"""Manage local qmd: collections, indexing (off the request thread), HTTP server, search.

Hits are resolved to REAL on-disk paths/ids via the registry (never qmd's handelized `file`
or hash `docid`). Indexing runs in a single coalesced background worker; `reindex_after_write`
does a quick non-blocking `qmd update` so a new doc is immediately searchable."""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import List, Optional

from . import registry
from .qmd_client import QmdHttpClient, QmdHttpError

HERE = Path(__file__).resolve().parents[1]
QMD_BIN = HERE / "node_modules" / ".bin" / "qmd"
PORT = int(os.environ.get("BIOSEQ_QMD_PORT", "8181"))
COL_CURRENT, COL_ARCHIVE = "bioseq-docs-current", "bioseq-docs-archive"
SYNC_INTERVAL = 600.0
EMBED_MAX_MB = os.environ.get("BIOSEQ_QMD_EMBED_MAX_MB")
DOCNUM = re.compile(r"DOC(\d{4})")
ANYNUM = re.compile(r"(\d{4})")


def _log(m: str) -> None:
    print(f"[bioseq-build-mcp:qmd] {m}", file=sys.stderr, flush=True)


class QmdUnavailable(Exception):
    pass


class QmdManager:
    def __init__(self):
        self.client = QmdHttpClient(f"http://localhost:{PORT}")
        self._ready_lock = threading.Lock()
        self._embed_lock = threading.Lock()     # held only during qmd embed
        self._dirty = threading.Event()
        self._ready = False
        self._worker = None

    def _cli(self, args: List[str], timeout: float = 1800.0):
        if not QMD_BIN.exists():
            raise QmdUnavailable(f"qmd not found at {QMD_BIN}; run npm install in {HERE}")
        return subprocess.run([str(QMD_BIN), *args], cwd=HERE, env={**os.environ},
                              capture_output=True, text=True, timeout=timeout)

    # ---- fast readiness (collections + HTTP server only) ----
    def ensure_ready(self) -> None:
        with self._ready_lock:
            if self._ready:
                return
            self._ensure_collections()
            self._ensure_server()
            self._ready = True
            self._start_worker()
            self._dirty.set()           # request an initial index in the background

    def _has_collection(self, name: str) -> bool:
        try:
            return name in self._cli(["collection", "list"], timeout=60).stdout
        except Exception:
            return False

    def _ensure_collections(self) -> None:
        registry.CURRENT_ROOT.mkdir(parents=True, exist_ok=True)
        registry.ARCHIVE_ROOT.mkdir(parents=True, exist_ok=True)
        if not self._has_collection(COL_CURRENT):
            self._cli(["collection", "add", str(registry.CURRENT_ROOT), "--name", COL_CURRENT,
                       "--mask", "**/*.md"], timeout=120)
            self._cli(["context", "add", f"qmd://{COL_CURRENT}",
                       "Active bioseqdb engineering docs (specs, plans, reports, prompts, "
                       "research). Files are NNNN_TITLE.md; higher number = newer."], timeout=60)
        if not self._has_collection(COL_ARCHIVE):
            self._cli(["collection", "add", str(registry.ARCHIVE_ROOT), "--name", COL_ARCHIVE,
                       "--mask", "**/*.md"], timeout=120)
            self._cli(["context", "add", f"qmd://{COL_ARCHIVE}",
                       "Archived / superseded bioseqdb documents (historical)."], timeout=60)
            self._cli(["collection", "exclude", COL_ARCHIVE], timeout=60)

    def _ensure_server(self) -> None:
        if self.client.health():
            return
        # No --daemon (avoids clobbering the global ~/.cache/qmd/mcp.pid of a user's own daemon).
        _log(f"starting qmd HTTP server on :{PORT}")
        subprocess.Popen([str(QMD_BIN), "mcp", "--http", "--port", str(PORT)], cwd=HERE,
                         env={**os.environ}, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
        for _ in range(90):
            if self.client.health():
                return
            time.sleep(1.0)
        raise QmdUnavailable(f"qmd HTTP server not healthy on :{PORT} (port busy? set BIOSEQ_QMD_PORT)")

    # ---- background indexer: single worker, coalesced via _dirty ----
    def _start_worker(self) -> None:
        if self._worker:
            return

        def loop():
            while True:
                self._dirty.wait()
                self._dirty.clear()
                with self._embed_lock:
                    try:
                        self._cli(["update"], timeout=1800)
                        ea = ["embed"]
                        if EMBED_MAX_MB:
                            ea += ["--max-batch-mb", str(EMBED_MAX_MB)]
                        self._cli(ea, timeout=7200)
                    except Exception as e:
                        _log(f"index error: {e}")

        self._worker = threading.Thread(target=loop, daemon=True)
        self._worker.start()
        threading.Thread(target=self._periodic, daemon=True).start()

    def _periodic(self) -> None:
        while True:
            time.sleep(SYNC_INTERVAL)
            self._dirty.set()

    def reindex_after_write(self) -> bool:
        """Quick FTS `qmd update` now (non-blocking on embed) so the doc is immediately
        searchable; schedule a background embed. Returns True if FTS-indexed synchronously."""
        self.ensure_ready()
        if self._embed_lock.acquire(blocking=False):
            try:
                self._cli(["update"], timeout=600)
            finally:
                self._embed_lock.release()
            self._dirty.set()
            return True
        self._dirty.set()           # embed in progress; picked up next cycle
        return False

    # ---- search ----
    def search(self, query: str, *, limit=10, min_score=0.0, include_archived=False,
               intent=None) -> dict:
        self.ensure_ready()
        cols = [COL_CURRENT] + ([COL_ARCHIVE] if include_archived else [])
        searches = [{"type": "lex", "query": query}, {"type": "vec", "query": query}]
        results = None
        # Forward intent only when the caller supplies one (passing the raw query as intent
        # would duplicate it in qmd's rerankQuery and muddy the rerank signal).
        attempts = [
            lambda: self.client.query_rest(searches, limit=limit, min_score=min_score,
                                           collections=cols, intent=intent),
            lambda: self.client.query_mcp(searches, limit=limit, min_score=min_score,
                                          collections=cols, intent=intent),
        ]
        for attempt in attempts:
            try:
                results = attempt()
                break
            except QmdHttpError as e:
                _log(f"HTTP search path failed: {e}")
        if results is None:
            results = self._cli_query(query, limit, min_score, cols)
        return self._shape(results)

    def _cli_query(self, query, limit, min_score, cols) -> list:
        args = ["query", query, "--format", "json", "--full-path", "-n", str(limit)]
        if min_score:
            args += ["--min-score", str(min_score)]
        for c in cols:
            args += ["-c", c]
        try:
            r = self._cli(args, timeout=600)
            return json.loads(r.stdout) if r.stdout.lstrip().startswith(("[", "{")) else []
        except Exception as e:
            _log(f"CLI query failed: {e}")
            return []

    def _shape(self, results: list) -> dict:
        norm = [n for n in (self._norm(r) for r in results if r) if n]
        if not norm:
            return {"best": None, "others": [], "count": 0}
        b = norm[0]
        return {"best": {"path": b["path"], "fragment": b["snippet"], "doc_id": b["doc_id"],
                         "title": b["title"], "score": b["score"], "context": b["context"]},
                "others": [{"path": n["path"], "doc_id": n["doc_id"], "title": n["title"],
                            "score": n["score"], "context": n["context"]} for n in norm[1:]],
                "count": len(norm)}

    def _extract_num(self, file_field: str) -> Optional[int]:
        if not file_field:
            return None
        seg = file_field.rstrip("/").split("/")[-1]
        m = re.match(r"(\d{4})", seg) or DOCNUM.search(seg) or ANYNUM.search(seg)
        return int(m.group(1)) if m else None

    def _norm(self, r: dict) -> Optional[dict]:
        ff = r.get("file") or r.get("path") or r.get("displayPath") or ""
        info = None
        num = self._extract_num(ff)
        if num is not None:
            info = registry.by_number(num)
        path, doc_id, title = "", "", r.get("title") or ""
        if info:
            path, doc_id, title = str(info.path), info.doc_id, (title or info.title)
        elif ff.startswith("/") and Path(ff).exists():     # CLI --full-path real path
            path = ff
            m = registry.NAME_RE.match(Path(ff).name)
            doc_id = f"DOC{m.group(1)}" if m else ""
        if not path:
            return None
        # `snippet` is the HTTP /query + MCP field; the CLI `query --format json` tier emits
        # `body` instead (and no score), so fall back to it for a non-empty fragment.
        return {"path": path, "doc_id": doc_id, "title": title,
                "snippet": (r.get("snippet") or r.get("text") or r.get("excerpt")
                            or r.get("body") or ""),
                "score": round(float(r.get("score", 0.0)), 4),
                "context": r.get("context") or "", "qmd_docid": r.get("docid", "")}


_MANAGER = None


def get_manager() -> QmdManager:
    global _MANAGER
    if _MANAGER is None:
        _MANAGER = QmdManager()
    return _MANAGER
