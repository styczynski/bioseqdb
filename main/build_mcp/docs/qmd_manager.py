"""Manage local qmd: collections, indexing (off the request thread), HTTP server, search.

Hits are resolved to REAL on-disk paths/ids via the registry (never qmd's handelized `file`
or hash `docid`). Indexing runs in a single coalesced background worker; `reindex_after_write`
does a quick non-blocking `qmd update` so a new doc is immediately searchable."""
from __future__ import annotations

import fcntl
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
ACTIVITY_REGISTRY = Path(
    os.environ.get(
        "BIOSEQ_QMD_ACTIVITY_REGISTRY",
        "/tmp/bioseq-build-qmd-activity.json",
    )
)
ACTIVITY_REGISTRY_LOCK = Path(
    os.environ.get(
        "BIOSEQ_QMD_ACTIVITY_REGISTRY_LOCK",
        "/tmp/bioseq-build-qmd-activity.lock",
    )
)
QMD_OPERATION_LOCK = Path(
    os.environ.get(
        "BIOSEQ_QMD_OPERATION_LOCK",
        "/tmp/bioseq-build-qmd-operation.lock",
    )
)
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
        self._activity_lock = threading.Lock()
        self._update_running = 0
        self._embed_running = 0
        self._external_update_running = 0
        self._dirty_pending = False
        self._dirty = threading.Event()
        self._ready = False
        self._worker = None

    @staticmethod
    def _lock_file(path: Path, *, blocking: bool = True):
        path.parent.mkdir(parents=True, exist_ok=True)
        handle = path.open("a+", encoding="utf-8")
        flags = fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB)
        try:
            fcntl.flock(handle.fileno(), flags)
        except BlockingIOError:
            handle.close()
            return None
        return handle

    @staticmethod
    def _unlock_file(handle) -> None:
        fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        handle.close()

    @staticmethod
    def _read_activity_registry_locked() -> dict | None:
        try:
            raw = ACTIVITY_REGISTRY.read_text(encoding="utf-8")
        except FileNotFoundError:
            return {}
        except (OSError, UnicodeError):
            return None
        if not raw.strip():
            return {}
        try:
            value = json.loads(raw)
        except json.JSONDecodeError:
            return None
        return value if isinstance(value, dict) else None

    @staticmethod
    def _write_activity_registry_locked(value: dict) -> None:
        ACTIVITY_REGISTRY.parent.mkdir(parents=True, exist_ok=True)
        with ACTIVITY_REGISTRY.open("w", encoding="utf-8") as handle:
            json.dump(value, handle, sort_keys=True, separators=(",", ":"))
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())

    def _publish_process_activity_locked(self) -> None:
        """Publish this process's active phases under the cross-process lock.

        The caller holds ``_activity_lock``. An invalid pre-existing registry is
        preserved as an invalid sentinel so status remains fail closed.
        """
        registry_lock = self._lock_file(ACTIVITY_REGISTRY_LOCK)
        if registry_lock is None:  # pragma: no cover - blocking acquisition
            raise RuntimeError("could not acquire QMD activity registry lock")
        try:
            value = self._read_activity_registry_locked()
            if value is None:
                value = {"invalid": None}
            key = str(os.getpid())
            update_running = (
                self._update_running + self._external_update_running
            )
            initialized = self._ready or self._worker is not None
            if (
                initialized
                or self._dirty_pending
                or update_running
                or self._embed_running
            ):
                value[key] = {
                    "pid": os.getpid(),
                    "initialized": initialized,
                    "update_queued": int(self._dirty_pending),
                    "update_running": update_running,
                    "embed_queued": int(
                        self._dirty_pending or self._update_running
                    ),
                    "embed_running": self._embed_running,
                }
            else:
                value.pop(key, None)
            self._write_activity_registry_locked(value)
        finally:
            self._unlock_file(registry_lock)

    def begin_external_update(self) -> None:
        """Publish document-environment/readiness work before it starts."""
        with self._activity_lock:
            self._external_update_running += 1
            self._publish_process_activity_locked()

    def end_external_update(self) -> None:
        """Finish a previously published document-environment operation."""
        with self._activity_lock:
            if self._external_update_running <= 0:
                raise RuntimeError("QMD external update activity is unbalanced")
            self._external_update_running -= 1
            self._publish_process_activity_locked()

    def _queue_index(self) -> None:
        """Atomically publish queued work before waking the local worker."""
        with self._activity_lock:
            self._dirty_pending = True
            self._publish_process_activity_locked()
            self._dirty.set()

    def unregister_idle_process(self) -> None:
        """Remove a cleanly exiting idle MCP process from shared activity."""
        with self._activity_lock:
            if (
                self._dirty_pending
                or self._update_running
                or self._embed_running
                or self._external_update_running
            ):
                return
            self._ready = False
            self._worker = None
            self._publish_process_activity_locked()

    @staticmethod
    def _pid_alive(pid: object) -> bool:
        if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
            return False
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True

    @classmethod
    def _shared_activity(cls) -> tuple[dict, dict, bool]:
        """Aggregate all live MCP document workers without exposing identity."""
        registry_lock = cls._lock_file(ACTIVITY_REGISTRY_LOCK)
        if registry_lock is None:  # pragma: no cover - blocking acquisition
            return ({"queued": 0, "running": 0, "state": "unknown"},) * 2 + (True,)
        try:
            value = cls._read_activity_registry_locked()
            if isinstance(value, dict):
                stale_idle_keys = []
                expected_keys = {
                    "pid", "initialized", "update_queued", "update_running",
                    "embed_queued", "embed_running",
                }
                for key, raw in value.items():
                    if not isinstance(raw, dict) or set(raw) != expected_keys:
                        continue
                    counters = (
                        raw["update_queued"], raw["update_running"],
                        raw["embed_queued"], raw["embed_running"],
                    )
                    if (
                        isinstance(raw["pid"], int)
                        and not isinstance(raw["pid"], bool)
                        and raw["pid"] > 0
                        and all(
                            isinstance(counter, int)
                            and not isinstance(counter, bool)
                            and counter == 0
                            for counter in counters
                        )
                        and not cls._pid_alive(raw["pid"])
                    ):
                        stale_idle_keys.append(key)
                if stale_idle_keys:
                    for key in stale_idle_keys:
                        value.pop(key, None)
                    cls._write_activity_registry_locked(value)
        finally:
            cls._unlock_file(registry_lock)
        unknown = value is None
        update_queued = update_running = 0
        embed_queued = embed_running = 0
        recognized = False
        if value is not None:
            for raw in value.values():
                if not isinstance(raw, dict) or set(raw) != {
                    "pid", "initialized", "update_queued", "update_running",
                    "embed_queued", "embed_running",
                }:
                    unknown = True
                    continue
                counters = (
                    raw["update_queued"], raw["update_running"],
                    raw["embed_queued"], raw["embed_running"],
                )
                if (
                    any(
                        not isinstance(counter, int)
                        or isinstance(counter, bool)
                        or counter < 0
                        for counter in counters
                    )
                    or not isinstance(raw["initialized"], bool)
                    or not cls._pid_alive(raw["pid"])
                ):
                    unknown = True
                    continue
                recognized = True
                update_queued += raw["update_queued"]
                update_running += raw["update_running"]
                embed_queued += raw["embed_queued"]
                embed_running += raw["embed_running"]

        def transient(queued: int, running: int) -> dict:
            if unknown:
                state = "unknown"
            elif running:
                state = "running"
            elif queued:
                state = "queued"
            elif recognized:
                state = "idle"
            else:
                state = "absent"
            return {"queued": queued, "running": running, "state": state}

        return (
            transient(update_queued, update_running),
            transient(embed_queued, embed_running),
            unknown,
        )

    def activity_snapshot(self) -> dict:
        """Return the path-free status payload consumed by builder ``status``."""
        update, embed, registry_unknown = self._shared_activity()
        persistent = self.client.health()
        if registry_unknown:
            qmd_state = "unknown"
        elif persistent:
            qmd_state = (
                "busy"
                if update["queued"] or embed["queued"]
                or update["running"] or embed["running"]
                else "idle"
            )
        elif update["state"] != "absent" or embed["state"] != "absent":
            qmd_state = "unknown"
        else:
            qmd_state = "absent"
        return {
            "document_update": update,
            "document_embed": embed,
            "qmd_server": {
                "persistent": persistent,
                "state": qmd_state,
            },
        }

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
            self.begin_external_update()
            operation_lock = None
            try:
                operation_lock = self._lock_file(QMD_OPERATION_LOCK)
                if operation_lock is None:  # pragma: no cover
                    raise QmdUnavailable("could not acquire QMD operation lock")
                self._ensure_collections()
                self._ensure_server()
                self._ready = True
                self._start_worker()
                self._queue_index()
            finally:
                try:
                    if operation_lock is not None:
                        self._unlock_file(operation_lock)
                finally:
                    self.end_external_update()

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
                with self._embed_lock:
                    operation_lock = self._lock_file(QMD_OPERATION_LOCK)
                    if operation_lock is None:  # pragma: no cover
                        continue
                    try:
                        with self._activity_lock:
                            if not self._dirty_pending:
                                self._dirty.clear()
                                continue
                            self._dirty_pending = False
                            self._update_running = 1
                            self._publish_process_activity_locked()
                            self._dirty.clear()
                        try:
                            self._cli(["update"], timeout=1800)
                        except Exception:
                            with self._activity_lock:
                                self._update_running = 0
                                self._publish_process_activity_locked()
                            raise
                        ea = ["embed"]
                        if EMBED_MAX_MB:
                            ea += ["--max-batch-mb", str(EMBED_MAX_MB)]
                        with self._activity_lock:
                            self._update_running = 0
                            self._embed_running = 1
                            self._publish_process_activity_locked()
                        try:
                            self._cli(ea, timeout=7200)
                        finally:
                            with self._activity_lock:
                                self._embed_running = 0
                                self._publish_process_activity_locked()
                    except Exception as e:
                        _log(f"index error: {e}")
                    finally:
                        self._unlock_file(operation_lock)

        self._worker = threading.Thread(target=loop, daemon=True)
        self._worker.start()
        threading.Thread(target=self._periodic, daemon=True).start()

    def _periodic(self) -> None:
        while True:
            time.sleep(SYNC_INTERVAL)
            self._queue_index()

    def reindex_after_write(self) -> bool:
        """Quick FTS `qmd update` now (non-blocking on embed) so the doc is immediately
        searchable; schedule a background embed. Returns True if FTS-indexed synchronously."""
        self.ensure_ready()
        if self._embed_lock.acquire(blocking=False):
            operation_lock = self._lock_file(QMD_OPERATION_LOCK, blocking=False)
            if operation_lock is None:
                self._embed_lock.release()
                self._queue_index()
                return False
            try:
                with self._activity_lock:
                    self._update_running = 1
                    self._publish_process_activity_locked()
                try:
                    self._cli(["update"], timeout=600)
                finally:
                    with self._activity_lock:
                        self._update_running = 0
                        self._dirty_pending = True
                        self._publish_process_activity_locked()
                        self._dirty.set()
            finally:
                self._unlock_file(operation_lock)
                self._embed_lock.release()
            return True
        self._queue_index()         # embed in progress; picked up next cycle
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
_MANAGER_LOCK = threading.Lock()


def get_manager() -> QmdManager:
    global _MANAGER
    with _MANAGER_LOCK:
        if _MANAGER is None:
            _MANAGER = QmdManager()
    return _MANAGER
