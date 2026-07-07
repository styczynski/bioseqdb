"""Registry over main/DOCS/{CURRENT,ARCHIVE}/<TYPE>/NNNN_TITLE.md.

Filesystem is the source of truth (no .doc_counter): next id = max existing + 1.
Knows routing, slugging, create, archive (targeted frontmatter edits), and resolution."""
from __future__ import annotations

import datetime
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

from . import frontmatter as fmlib

DOCS_ROOT = Path(__file__).resolve().parents[2] / "DOCS"
CURRENT_ROOT = DOCS_ROOT / "CURRENT"
ARCHIVE_ROOT = DOCS_ROOT / "ARCHIVE"
TOPICS_FILE = DOCS_ROOT / ".topics"

VALID_TYPES = {"guidelines", "human_notes", "tech_spec", "research", "report",
               "impl_plan", "prompt", "external_doc", "bugfix"}
VALID_ORIGINS = {"ai_output", "human_prompt", "external_dump", "human_notes", "mixed"}
TYPE_TO_DIR = {"guidelines": "GUIDELINES", "human_notes": "HUMAN_NOTES", "tech_spec": "TECH_SPECS",
               "research": "RESEARCH", "report": "REPORTS", "impl_plan": "IMPL_PLANS",
               "prompt": "PROMPTS", "external_doc": "EXTERNAL_DOCS", "bugfix": "BUGFIX"}
BENCH_TOPICS = {"bench-framework", "benchmarking-speedup"}
BUGFIX_KIND_TAGS = {"bugfix-prompt", "bugfix-report", "bugfix-plan"}
NAME_RE = re.compile(r"^(\d{4})_(.+)\.md$")
DOC_ID_RE = re.compile(r"^DOC(\d{4})$")


class DocError(Exception):
    pass


@dataclass
class DocInfo:
    path: Path
    doc_id: str
    num: int
    type: str
    title: str
    archived: bool
    status: str
    topics: list


def doc_num(d: str) -> int:
    m = DOC_ID_RE.match(d or "")
    return int(m.group(1)) if m else -1


def slugify(t: str) -> str:
    return re.sub(r"_+", "_", re.sub(r"[^A-Za-z0-9]+", "_", t).strip("_").upper())


def load_topics() -> set:
    return ({t.strip() for t in TOPICS_FILE.read_text().splitlines() if t.strip()}
            if TOPICS_FILE.exists() else set())


def save_topics(s: set) -> None:
    TOPICS_FILE.write_text("\n".join(sorted(s)) + "\n")


def all_docs(include_archived: bool = True) -> List[DocInfo]:
    out = []
    for root in [CURRENT_ROOT] + ([ARCHIVE_ROOT] if include_archived else []):
        if not root.exists():
            continue
        for f in sorted(root.glob("*/*.md")):
            m = NAME_RE.match(f.name)
            if not m:
                continue
            fm, _ = fmlib.parse(f.read_text(encoding="utf-8", errors="replace"))
            out.append(DocInfo(path=f, doc_id=fm.get("doc_id", f"DOC{m.group(1)}"),
                               num=int(m.group(1)), type=fm.get("type", "?"), title=m.group(2),
                               archived=(root is ARCHIVE_ROOT), status=fm.get("status", "active"),
                               topics=fmlib.as_list(fm.get("topics"))))
    return out


def by_number(n: int) -> Optional[DocInfo]:
    for d in all_docs(True):
        if d.num == n:
            return d
    return None


def next_doc_id() -> str:
    nums = [d.num for d in all_docs(True)]
    return f"DOC{(max(nums) + 1) if nums else 1:04d}"


def resolve_directory(t: str, topics: list) -> str:
    if any(x in BENCH_TOPICS for x in topics):
        return "BENCHMARK_FRAMEWORK"
    if t == "bugfix" or any(x in BUGFIX_KIND_TAGS for x in topics):
        return "BUGFIX"
    return TYPE_TO_DIR[t]


def create_document(doc_type, title, topics, *, summary="", origin="ai_output",
                    supersedes=None, allow_new_topic=False) -> Path:
    if doc_type not in VALID_TYPES:
        raise DocError(f"unknown type '{doc_type}'; valid: {sorted(VALID_TYPES)}")
    if origin not in VALID_ORIGINS:
        raise DocError(f"unknown origin '{origin}'; valid: {sorted(VALID_ORIGINS)}")
    topics = [t.strip() for t in topics if t and t.strip()]
    if not topics:
        raise DocError("at least one topic is required")
    known = load_topics()
    unknown = [t for t in topics if t not in known]
    if unknown:
        if not allow_new_topic:
            raise DocError(f"unknown topic(s) {unknown}; pass allow_new_topic=true")
        known.update(unknown)
        save_topics(known)
    did = next_doc_id()
    target = CURRENT_ROOT / resolve_directory(doc_type, topics) / f"{doc_num(did):04d}_{slugify(title)}.md"
    if target.exists():
        raise DocError(f"file exists: {target}")
    today = datetime.date.today()
    fm = {"type": doc_type, "date": f"{today.day:02d}-{today.month:02d}", "origin": origin,
          "doc_id": did, "topics": topics, "summary": summary or title,
          "archived": "false", "status": "active"}
    if supersedes:
        fm["supersedes"] = [s.strip() for s in supersedes if s.strip()]
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(fmlib.render(fm) + f"\n# {title}\n\n<!-- content below -->\n", encoding="utf-8")
    return target


def find_document(selector: str) -> DocInfo:
    sel = selector.strip()
    docs = all_docs(True)
    want = doc_num(sel) if DOC_ID_RE.match(sel) else (int(sel) if sel.isdigit() else None)
    if want is not None:
        hit = [d for d in docs if d.num == want]
        if hit:
            return hit[0]
        raise DocError(f"no document with id/number {sel}")
    base = Path(sel).name
    exact = [d for d in docs if d.path.name == base]
    if exact:
        if len(exact) == 1:
            return exact[0]
        raise DocError(f"ambiguous filename '{base}' ({len(exact)})")
    partial = [d for d in docs if sel in str(d.path)]
    if len(partial) == 1:
        return partial[0]
    raise DocError(f"no unique match for '{selector}' ({len(partial)} matches)")


def archive_document(selector: str) -> Tuple[Path, Path]:
    info = find_document(selector)
    if info.archived:
        raise DocError(f"{info.doc_id} is already archived")
    new_path = ARCHIVE_ROOT / info.path.relative_to(CURRENT_ROOT)
    block, body = fmlib.split_frontmatter(info.path.read_text(encoding="utf-8", errors="replace"))
    if block is None:
        raise DocError(f"{info.path} malformed frontmatter")
    block = fmlib.fm_set_line(block, "archived", "true")
    if fmlib.fm_get(block, "status") != "superseded":
        block = fmlib.fm_set_line(block, "status", "archived")
    new_path.parent.mkdir(parents=True, exist_ok=True)
    _git_mv(info.path, new_path)
    new_path.write_text(fmlib.reassemble(block, body), encoding="utf-8")
    return info.path, new_path


def _git_mv(old: Path, new: Path) -> None:
    if subprocess.run(["git", "mv", "-f", str(old), str(new)], cwd=DOCS_ROOT.parent.parent,
                      capture_output=True).returncode != 0:
        old.rename(new)
