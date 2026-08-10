#!/usr/bin/env python3
"""One-shot Goal A+B migration for DOCS/.

A: dedupe DOC ids; rename TITLE__DOCXXXX.md -> XXXX_TITLE.md; regen INDEX.md + .doc_counter.
B: split into CURRENT/<TYPE>/ and ARCHIVE/<TYPE>/; set `archived:` in frontmatter.

archive := (orig number < 290) OR (status == superseded)
dedupe  := git-earliest twin keeps id; later twins -> next free ids (asc git order)
refs    := rewrite bare `old`->`new`, `TYPE/old`->`BUCKET/TYPE/new`, and `](../TYPE/old)` ->
           the correct relative path (os.path.relpath) per referencing file.

  python3 migrate_cleanup.py            # dry-run
  python3 migrate_cleanup.py --execute  # mutate (aborts if DOCS is git-dirty)
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from docs import frontmatter as fmlib   # noqa: E402  shared parser

REPO_ROOT = Path(__file__).resolve().parents[1]
DOCS_ROOT = REPO_ROOT / "DOCS"
COUNTER_FILE = DOCS_ROOT / ".doc_counter"
INDEX_FILE = DOCS_ROOT / "INDEX.md"
RENAME_MAP_FILE = DOCS_ROOT / ".cleanup_rename_map.txt"
STALE = [DOCS_ROOT / ".migration_map.txt", DOCS_ROOT / ".migration_summary.md"]
CURRENT, ARCHIVE = "CURRENT", "ARCHIVE"
ARCHIVE_THRESHOLD = 290
SUF = re.compile(r"__DOC(\d{4})\.md$")
DOC_ID_RE = re.compile(r"^DOC(\d{4})$")
MD_LINK = re.compile(r"\]\(([^)]+\.md)\)")


def doc_num(d: str) -> int:
    m = DOC_ID_RE.match(d or "")
    return int(m.group(1)) if m else -1


@dataclass
class DocRec:
    path: Path
    type_dir: str
    canonical: str
    orig_id: str
    orig_num: int
    status: str
    slug: str
    new_id: str = ""
    renumbered: bool = False
    archived: bool = False
    new_name: str = ""
    new_path: Optional[Path] = field(default=None)
    new_text: str = ""


def git_added(path: Path) -> str:
    try:
        o = subprocess.run(["git", "log", "--diff-filter=A", "--follow", "--format=%aI",
                            "--", str(path)], cwd=REPO_ROOT, text=True,
                           capture_output=True).stdout.strip().splitlines()
        return o[-1] if o else ""
    except Exception:
        return ""


def git_mv(old: Path, new: Path) -> None:
    new.parent.mkdir(parents=True, exist_ok=True)
    if subprocess.run(["git", "mv", "-f", str(old), str(new)], cwd=REPO_ROOT,
                      capture_output=True).returncode != 0:
        old.rename(new)


def scan() -> List[DocRec]:
    recs, problems = [], []
    for td in sorted(p for p in DOCS_ROOT.iterdir() if p.is_dir()):
        if td.name in (CURRENT, ARCHIVE):
            continue
        for f in sorted(td.glob("*.md")):
            m = SUF.search(f.name)
            if not m:
                problems.append(f"non-conforming name: {f}")
                continue
            try:
                canonical, repaired = fmlib.repair(f.read_text(encoding="utf-8", errors="replace"))
            except ValueError as e:
                problems.append(f"unrepairable frontmatter in {f}: {e}")
                continue
            fm, _ = fmlib.parse(canonical)
            did = fm.get("doc_id") or f"DOC{m.group(1)}"
            if not DOC_ID_RE.match(did):
                problems.append(f"bad doc_id in {f}: {did!r}")
            recs.append(DocRec(path=f, type_dir=td.name, canonical=canonical, orig_id=did,
                               orig_num=doc_num(did), status=fm.get("status", "active"),
                               slug=SUF.sub("", f.name)))
            if repaired:
                print(f"REPAIR: normalised frontmatter of {f.name}", file=sys.stderr)
    if problems:
        raise SystemExit("PREFLIGHT FAILED:\n  " + "\n  ".join(problems))
    return recs


def build_plan(recs: List[DocRec]) -> int:
    used = {r.orig_num for r in recs}
    try:
        used.add(int(COUNTER_FILE.read_text().strip() or "0"))
    except Exception:
        pass
    nxt = max(used) + 1
    groups: Dict[str, List[DocRec]] = defaultdict(list)
    for r in recs:
        groups[r.orig_id].append(r)
    renum: List[DocRec] = []
    for ms in groups.values():
        if len(ms) == 1:
            ms[0].new_id = ms[0].orig_id
            continue
        ms.sort(key=lambda r: (git_added(r.path) or "9999", str(r.path)))
        ms[0].new_id = ms[0].orig_id
        renum += ms[1:]
    renum.sort(key=lambda r: (git_added(r.path) or "9999", str(r.path)))
    for r in renum:
        r.new_id, r.renumbered, nxt = f"DOC{nxt:04d}", True, nxt + 1
    for r in recs:
        r.archived = (r.orig_num < ARCHIVE_THRESHOLD) or (r.status == "superseded")
        r.new_name = f"{doc_num(r.new_id):04d}_{r.slug}.md"
        r.new_path = DOCS_ROOT / (ARCHIVE if r.archived else CURRENT) / r.type_dir / r.new_name
    return nxt - 1


def global_repl(recs: List[DocRec]) -> List[Tuple[str, str]]:
    out: List[Tuple[str, str]] = []
    for t in recs:
        bucket = ARCHIVE if t.archived else CURRENT
        out.append((f"{t.type_dir}/{t.path.name}", f"{bucket}/{t.type_dir}/{t.new_name}"))
        out.append((t.path.name, t.new_name))
    out.sort(key=lambda kv: len(kv[0]), reverse=True)
    return out


def rewrite_text(rec: DocRec, recs: List[DocRec], gl: List[Tuple[str, str]]) -> Tuple[str, int]:
    text, n = rec.canonical, 0
    if "../" in text and "](" in text:
        rels = []
        here = rec.new_path.parent
        for t in recs:
            old = f"../{t.type_dir}/{t.path.name}"
            if old in text:
                rels.append((old, os.path.relpath(t.new_path, here)))
        for old, new in sorted(rels, key=lambda kv: len(kv[0]), reverse=True):
            n += text.count(old)
            text = text.replace(old, new)
    for old, new in gl:
        if old != new and old in text:
            n += text.count(old)
            text = text.replace(old, new)
    return text, n


def regen_index(recs: List[DocRec]) -> str:
    by: Dict[Tuple[str, str], List[DocRec]] = defaultdict(list)
    for r in recs:
        by[(ARCHIVE if r.archived else CURRENT, r.type_dir)].append(r)
    out = ["# DOCS Index", "", "Auto-generated by `migrate_cleanup.py` (Goal A).",
           "CURRENT/<TYPE>/ = active; ARCHIVE/<TYPE>/ = archived. Files are NNNN_TITLE.md.", ""]
    for bucket in (CURRENT, ARCHIVE):
        out.append(f"# {bucket}\n")
        for (b, d) in sorted(k for k in by if k[0] == bucket):
            ms = sorted(by[(b, d)], key=lambda r: doc_num(r.new_id), reverse=True)
            out += [f"## {bucket}/{d} ({len(ms)} files)\n", "| DOC | filename | status |",
                    "|-----|----------|--------|"]
            out += [f"| {r.new_id} | {r.new_name} | {r.status} |" for r in ms]
            out.append("")
    return "\n".join(out)


def execute(recs: List[DocRec], new_counter: int) -> None:
    gl = global_repl(recs)
    total = 0
    # Phase 1: compute all new texts + frontmatter edits IN MEMORY; validate before mutating.
    for r in recs:
        text, n = rewrite_text(r, recs, gl)
        total += n
        block, body = fmlib.split_frontmatter(text)
        if block is None:
            raise SystemExit(f"unexpected: {r.path} unparseable after repair")
        if r.renumbered:
            block = fmlib.fm_set_line(block, "doc_id", r.new_id)
        block = fmlib.fm_set_line(block, "archived", "true" if r.archived else "false")
        if r.status != "superseded":
            block = fmlib.fm_set_line(block, "status", "archived" if r.archived else "active")
        r.new_text = fmlib.reassemble(block, body)
    bad = []
    for r in recs:
        fm, _ = fmlib.parse(r.new_text)
        if not DOC_ID_RE.match(fm.get("doc_id", "")):
            bad.append(f"{r.new_path}: missing/invalid doc_id")
        if r.archived and str(fm.get("archived")).lower() != "true":
            bad.append(f"{r.new_path}: ARCHIVE but archived!=true")
    if bad:
        raise SystemExit("POSTFLIGHT (pre-mutation) FAILED:\n  " + "\n  ".join(bad))
    # Phase 2: mutate.
    for r in recs:
        git_mv(r.path, r.new_path)
        r.new_path.write_text(r.new_text, encoding="utf-8")
    # Phase 3: link-resolution check (warn-only; some links target non-docs/externals).
    dangling = []
    for r in recs:
        for link in MD_LINK.findall(r.new_text):
            if link.startswith(("http://", "https://", "#")):
                continue
            tgt = (r.new_path.parent / link).resolve()
            if not tgt.exists():
                dangling.append(f"{r.new_path.name} -> {link}")
    if dangling:
        print(f"WARNING: {len(dangling)} md link(s) may not resolve (non-doc/external?):",
              file=sys.stderr)
        for d in dangling[:15]:
            print("   " + d, file=sys.stderr)
    INDEX_FILE.write_text(regen_index(recs), encoding="utf-8")
    COUNTER_FILE.write_text(f"{new_counter}\n", encoding="utf-8")
    RENAME_MAP_FILE.write_text("\n".join(
        f"{r.path.relative_to(DOCS_ROOT)}\t->\t{r.new_path.relative_to(DOCS_ROOT)}"
        for r in recs) + "\n", encoding="utf-8")
    for art in STALE:
        if art.exists():
            subprocess.run(["git", "rm", "-f", "--quiet", str(art)], cwd=REPO_ROOT,
                           capture_output=True)
    print(f"OK: moved {len(recs)}, rewrote {total} refs, counter={new_counter}.", file=sys.stderr)


def print_plan(recs: List[DocRec], new_counter: int) -> None:
    gl = global_repl(recs)
    ren = [r for r in recs if r.renumbered]
    arc = [r for r in recs if r.archived]
    total = sum(rewrite_text(r, recs, gl)[1] for r in recs)
    print(f"# Plan: {len(recs)} docs | {len(ren)} renumbered | {len(arc)} ARCHIVE | "
          f"{len(recs)-len(arc)} CURRENT | refs rewritten={total} | counter={new_counter}\n")
    print("## Renumbered")
    for r in ren:
        print(f"  {r.orig_id} -> {r.new_id}   {r.path.name}")
    print("\n## Sample moves")
    for r in recs[:20]:
        print(f"  {r.path.relative_to(DOCS_ROOT)} -> {r.new_path.relative_to(DOCS_ROOT)}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--execute", action="store_true")
    args = ap.parse_args()
    if (DOCS_ROOT / CURRENT).exists() or (DOCS_ROOT / ARCHIVE).exists():
        print("ERROR: CURRENT/ or ARCHIVE/ exists — already migrated.", file=sys.stderr)
        return 1
    if args.execute:
        dirty = subprocess.run(["git", "status", "--porcelain", "DOCS"], cwd=REPO_ROOT,
                               capture_output=True, text=True).stdout.strip()
        if dirty:
            print("ERROR: DOCS is git-dirty; commit or stash its changes before --execute.",
                  file=sys.stderr)
            return 1
    recs = scan()
    new_counter = build_plan(recs)
    if not args.execute:
        print_plan(recs, new_counter)
        print("\n(dry-run; re-run with --execute)")
        return 0
    execute(recs, new_counter)
    return 0


if __name__ == "__main__":
    sys.exit(main())
