"""Markdown YAML-frontmatter: parse/render (symmetric), block-level edits, repair().

No external YAML dependency. Mirrors the hand-rolled format used across DOCS
(single-line scalars, `[a, b]` lists, double-quoted summary). `repair()` normalises the
known-malformed docs (stray `## ` on the `type:` line, missing fences)."""
from __future__ import annotations

import re

_ORDER = ["type", "date", "first_commit", "origin", "doc_id", "topics", "summary",
          "supersedes", "superseded_by", "archived", "status"]
_KEY = re.compile(r"^#{0,6}\s*([A-Za-z_][A-Za-z0-9_]*):(.*)$")


def split_frontmatter(text: str):
    """Return (block_without_fences, body) or (None, text) if no well-formed frontmatter."""
    if not text.startswith("---\n"):
        return None, text
    end = text.find("\n---\n", 4)
    if end == -1:
        return None, text
    return text[4:end], text[end + 5:]


def fm_get(block: str, key: str):
    for ln in block.splitlines():
        k, sep, v = ln.partition(":")
        if sep and k.strip() == key:
            return v.strip()
    return None


def fm_set_line(block: str, key: str, value: str) -> str:
    out, found = [], False
    for ln in block.splitlines():
        k, sep, _ = ln.partition(":")
        if sep and k.strip() == key:
            out.append(f"{key}: {value}")
            found = True
        else:
            out.append(ln)
    if not found:
        out.append(f"{key}: {value}")
    return "\n".join(out)


def reassemble(block: str, body: str) -> str:
    return "---\n" + block + "\n---\n" + body


def repair(text: str):
    """Normalise malformed frontmatter. Returns (canonical, repaired_bool).
    Raises ValueError if no doc_id can be recovered."""
    if split_frontmatter(text)[0] is not None:
        return text, False
    lines = text.split("\n")
    i = 0
    while i < len(lines) and lines[i].strip() in ("", "---"):
        i += 1
    fields = []
    while i < len(lines) and lines[i].strip() not in ("", "---"):
        m = _KEY.match(lines[i])
        if not m:
            break
        fields.append(f"{m.group(1)}:{m.group(2)}")
        i += 1
    block = "\n".join(fields)
    if fm_get(block, "doc_id") is None:
        raise ValueError("repair: no doc_id recoverable")
    return reassemble(block, "\n".join(lines[i:]).lstrip("\n")), True


def parse(text: str):
    block, body = split_frontmatter(text)
    if block is None:
        try:
            block, body = split_frontmatter(repair(text)[0])
        except ValueError:
            return {}, text
    fm = {}
    for ln in (block or "").splitlines():
        if not ln.strip() or ":" not in ln:
            continue
        k, _, v = ln.partition(":")
        k, v = k.strip(), v.strip()
        if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
            v = v[1:-1].replace('\\"', '"').replace("\\\\", "\\")
        elif v.startswith("[") and v.endswith("]"):
            inner = v[1:-1].strip()
            v = [x.strip() for x in inner.split(",")] if inner else []
        fm[k] = v
    return fm, (body or "")


def _q(s) -> str:
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ").strip() + '"'


def render(fm: dict) -> str:
    lines, seen = ["---"], set()
    for k in _ORDER:
        if k not in fm:
            continue
        seen.add(k)
        v = fm[k]
        if isinstance(v, list):
            lines.append(f"{k}: [" + ", ".join(map(str, v)) + "]")
        elif k == "summary":
            lines.append(f"{k}: {_q(v)}")
        else:
            lines.append(f"{k}: {v}")
    for k, v in fm.items():
        if k in seen:
            continue
        lines.append((f"{k}: [" + ", ".join(map(str, v)) + "]") if isinstance(v, list)
                     else f"{k}: {v}")
    lines.append("---")
    return "\n".join(lines) + "\n"


def as_list(v):
    return [] if v is None else (v if isinstance(v, list) else [v])
