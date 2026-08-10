"""HTTP client for a running `qmd mcp --http`.

Primary: session-free POST /query. Secondary: POST /mcp tools/call with the
initialize -> Mcp-Session-Id -> notifications/initialized handshake. Both normalised to a
list of result dicts."""
from __future__ import annotations

import json
import urllib.error
import urllib.request


class QmdHttpError(Exception):
    pass


class QmdHttpClient:
    def __init__(self, base_url: str, timeout: float = 300.0):
        self.base = base_url.rstrip("/")
        self.timeout = timeout
        self._sid = None
        self._id = 0

    def health(self) -> bool:
        try:
            with urllib.request.urlopen(self.base + "/health", timeout=5) as r:
                return r.status == 200
        except Exception:
            return False

    def _http(self, path: str, body: dict, extra_headers=None):
        h = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if extra_headers:
            h.update(extra_headers)
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode(),
                                     headers=h, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.headers, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.headers, e.read().decode("utf-8", "replace")
        except urllib.error.URLError as e:
            raise QmdHttpError(f"qmd HTTP failed: {e}") from e

    # ---- primary: REST /query (session-free) ----
    def query_rest(self, searches, *, limit=10, min_score=0.0, collections=None,
                   intent=None, rerank=True) -> list:
        body = {"searches": searches, "limit": limit, "minScore": min_score, "rerank": rerank}
        if collections:
            body["collections"] = collections
        if intent:
            body["intent"] = intent
        _, raw = self._http("/query", body)
        obj = _parse(raw)
        if isinstance(obj, dict) and "error" in obj:
            raise QmdHttpError(f"/query error: {obj['error']}")
        return _results(obj)

    # ---- secondary: /mcp tools/call (handshake) ----
    def _ensure_session(self):
        if self._sid:
            return
        self._id += 1
        headers, _raw = self._http("/mcp", {"jsonrpc": "2.0", "id": self._id,
            "method": "initialize", "params": {"protocolVersion": "2024-11-05",
            "capabilities": {}, "clientInfo": {"name": "bioseq-build-mcp", "version": "2.0.0"}}})
        sid = headers.get("Mcp-Session-Id") or headers.get("mcp-session-id")
        if not sid:
            raise QmdHttpError("qmd /mcp did not return Mcp-Session-Id")
        self._sid = sid
        self._http("/mcp", {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}},
                   {"Mcp-Session-Id": sid})

    def query_mcp(self, searches, *, limit=10, min_score=0.0, collections=None,
                  intent=None, rerank=True) -> list:
        self._ensure_session()
        self._id += 1
        args = {"searches": searches, "limit": limit, "minScore": min_score, "rerank": rerank}
        if collections:
            args["collections"] = collections
        if intent:
            args["intent"] = intent
        _, raw = self._http("/mcp", {"jsonrpc": "2.0", "id": self._id, "method": "tools/call",
            "params": {"name": "query", "arguments": args}}, {"Mcp-Session-Id": self._sid})
        obj = _parse(raw)
        if isinstance(obj, dict) and "error" in obj:
            raise QmdHttpError(f"/mcp error: {obj['error']}")
        return _results((obj or {}).get("result", obj))


def _parse(raw: str):
    raw = (raw or "").strip()
    if not raw:
        raise QmdHttpError("empty qmd response")
    if raw[0] in "{[":
        return json.loads(raw)
    for ln in reversed(raw.splitlines()):       # SSE
        ln = ln.strip()
        if ln.startswith("data:"):
            return json.loads(ln[5:].strip())
    raise QmdHttpError(f"unparseable: {raw[:200]}")


def _results(obj):
    if isinstance(obj, list):
        return obj
    if isinstance(obj, dict):
        if isinstance(obj.get("results"), list):
            return obj["results"]
        sc = obj.get("structuredContent")
        if isinstance(sc, dict) and isinstance(sc.get("results"), list):
            return sc["results"]
        for it in obj.get("content", []) or []:
            if it.get("type") == "text":
                try:
                    return _results(json.loads(it["text"]))
                except Exception:
                    pass
    return []
