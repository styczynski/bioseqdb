#!/usr/bin/env python3
"""
newdoc — create a new document under main/DOCS/ with consistent frontmatter.

Usage:
  newdoc.py <type> "<title>" [options]

Examples:
  newdoc.py impl_plan "Bench V19 instrumentation overhaul" --topics bench-framework
  newdoc.py tech_spec "SneakySnake v4 implementation"   --topics sneaky-snake
  newdoc.py prompt    "Refactor aligner pipeline stage" --topics aligner

Subcommands:
  index                                Regenerate main/DOCS/INDEX.md
  lint [--fix]                         Validate every doc; --fix back-fills supersession links
  list [--type T] [--topic Y]          List docs (DOC id descending)
  renumber HEAD                        Re-id docs introduced by HEAD (merge-conflict resolution)

Behaviour:
  * Validates type and topics against closed vocabularies.
  * Refuses unknown topics unless --allow-new-topic is set.
  * Allocates the next DOC id from main/DOCS/.doc_counter (atomic w/ the file write).
  * Prints the absolute path of the new file on stdout. Nothing else (so it can be piped).
  * Refreshes main/DOCS/INDEX.md after creating the file.

Exit codes:
  0 success
  2 invalid type
  3 unknown topic without --allow-new-topic
  4 file already exists (won't overwrite)
  5 lint failure (used by `lint` subcommand only)
"""
from __future__ import annotations

import argparse
import datetime
import os
import re
import subprocess
import sys
import shutil
from pathlib import Path
from collections import defaultdict

ROOT = Path(__file__).resolve().parent
DOCS_ROOT = ROOT / "main" / "DOCS"
COUNTER = DOCS_ROOT / ".doc_counter"
TOPICS_FILE = DOCS_ROOT / ".topics"
INDEX_FILE = DOCS_ROOT / "INDEX.md"

VALID_TYPES = {
    "guidelines", "human_notes", "tech_spec", "research", "report",
    "impl_plan", "prompt", "external_doc", "bugfix",
}
VALID_ORIGINS = {"ai_output", "human_prompt", "external_dump", "human_notes", "mixed"}

TYPE_TO_DIR = {
    "guidelines":   "GUIDELINES",
    "human_notes":  "HUMAN_NOTES",
    "tech_spec":    "TECH_SPECS",
    "research":     "RESEARCH",
    "report":       "REPORTS",
    "impl_plan":    "IMPL_PLANS",
    "prompt":       "PROMPTS",
    "external_doc": "EXTERNAL_DOCS",
    "bugfix":       "BUGFIX",
}

BENCH_TOPICS = {"bench-framework", "benchmarking-speedup"}
BUGFIX_KIND_TAGS = {"bugfix-prompt", "bugfix-report", "bugfix-plan"}

# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

def load_topics() -> set[str]:
    if not TOPICS_FILE.exists():
        return set()
    return {t.strip() for t in TOPICS_FILE.read_text().splitlines() if t.strip()}

def save_topics(topics: set[str]) -> None:
    TOPICS_FILE.write_text("\n".join(sorted(topics)) + "\n")

def slugify_title(title: str) -> str:
    s = re.sub(r"[^A-Za-z0-9]+", "_", title).strip("_").upper()
    s = re.sub(r"_+", "_", s)
    return s

def read_counter() -> int:
    if not COUNTER.exists():
        return 0
    return int(COUNTER.read_text().strip() or "0")

def write_counter(n: int) -> None:
    COUNTER.parent.mkdir(parents=True, exist_ok=True)
    COUNTER.write_text(str(n) + "\n")

def today_dd_mm() -> str:
    d = datetime.date.today()
    return f"{d.day:02d}-{d.month:02d}"

def yaml_quote(s: str) -> str:
    s = s.replace('"', '\\"').replace("\n", " ").strip()
    return f'"{s}"'

def yaml_list(xs: list[str]) -> str:
    return "[" + ", ".join(xs) + "]"

def parse_frontmatter(text: str) -> tuple[dict, str]:
    """Returns (frontmatter_dict, body). frontmatter_dict is {} if not present."""
    if not text.startswith("---\n"):
        return {}, text
    end = text.find("\n---\n", 4)
    if end == -1:
        return {}, text
    fm_block = text[4:end]
    body = text[end + 5:]
    fm = {}
    for line in fm_block.splitlines():
        if not line.strip(): continue
        if ":" not in line: continue
        key, _, value = line.partition(":")
        key = key.strip()
        value = value.strip()
        # Strip surrounding quotes
        if value.startswith('"') and value.endswith('"'):
            value = value[1:-1]
        # Parse list
        if value.startswith("[") and value.endswith("]"):
            inner = value[1:-1].strip()
            if not inner:
                value = []
            else:
                value = [v.strip() for v in inner.split(",")]
        fm[key] = value
    return fm, body

def render_frontmatter(fm: dict) -> str:
    """Serialise a frontmatter dict back to text. Order of keys preserved by inserting in canonical order."""
    order = ["type", "date", "first_commit", "origin", "doc_id", "topics",
             "summary", "supersedes", "superseded_by", "status"]
    lines = ["---"]
    for k in order:
        if k not in fm: continue
        v = fm[k]
        if isinstance(v, list):
            lines.append(f"{k}: {yaml_list(v)}")
        elif k == "summary":
            lines.append(f"{k}: {yaml_quote(v)}")
        else:
            lines.append(f"{k}: {v}")
    # Any extra keys we don't know about go at the end
    for k, v in fm.items():
        if k in order: continue
        if isinstance(v, list):
            lines.append(f"{k}: {yaml_list(v)}")
        else:
            lines.append(f"{k}: {v}")
    lines.append("---")
    return "\n".join(lines) + "\n"

# --------------------------------------------------------------------------
# Routing
# --------------------------------------------------------------------------

def resolve_directory(doc_type: str, topics: list[str]) -> str:
    """Apply routing rule: importance-class override for bench/bugfix."""
    if any(t in BENCH_TOPICS for t in topics):
        return "BENCHMARK_FRAMEWORK"
    if doc_type == "bugfix" or any(t in BUGFIX_KIND_TAGS for t in topics):
        return "BUGFIX"
    return TYPE_TO_DIR[doc_type]

# --------------------------------------------------------------------------
# new-doc command (default)
# --------------------------------------------------------------------------

def cmd_new(args: argparse.Namespace) -> int:
    doc_type = args.type
    title = args.title

    if doc_type not in VALID_TYPES:
        print(f"error: unknown type '{doc_type}'. Valid: {sorted(VALID_TYPES)}", file=sys.stderr)
        return 2

    topics = []
    if args.topics:
        topics = [t.strip() for t in args.topics.split(",") if t.strip()]
    if not topics:
        print("error: at least one --topics tag is required.", file=sys.stderr)
        return 2

    known_topics = load_topics()
    unknown = [t for t in topics if t not in known_topics]
    if unknown:
        if not args.allow_new_topic:
            print(
                f"error: unknown topic(s) {unknown}. "
                f"Rerun with --allow-new-topic to add them to {TOPICS_FILE.relative_to(ROOT)}.",
                file=sys.stderr,
            )
            return 3
        known_topics.update(unknown)
        save_topics(known_topics)

    origin = args.origin
    if origin not in VALID_ORIGINS:
        print(f"error: unknown origin '{origin}'. Valid: {sorted(VALID_ORIGINS)}", file=sys.stderr)
        return 2

    # Determine target dir and filename
    directory = resolve_directory(doc_type, topics)
    slug = slugify_title(title)
    counter = read_counter() + 1
    doc_id = f"DOC{counter:04d}"
    target_dir = DOCS_ROOT / directory
    target_dir.mkdir(parents=True, exist_ok=True)
    target_path = target_dir / f"{slug}__{doc_id}.md"

    if target_path.exists():
        print(f"error: file already exists: {target_path}", file=sys.stderr)
        return 4

    summary = args.summary or title

    fm = {
        "type": doc_type,
        "date": today_dd_mm(),
        "origin": origin,
        "doc_id": doc_id,
        "topics": topics,
        "summary": summary,
        "status": "active",
    }
    if args.supersedes:
        fm["supersedes"] = [s.strip() for s in args.supersedes.split(",") if s.strip()]

    content = render_frontmatter(fm) + f"\n# {title}\n\n<!-- Write your content below. -->\n"

    if args.dry_run:
        print(f"[dry-run] would create {target_path}")
        return 0

    target_path.write_text(content)
    write_counter(counter)

    # Refresh the index
    cmd_index(argparse.Namespace())

    # Print path on stdout (and nothing else)
    print(str(target_path))

    # Optionally exec editor
    if args.editor:
        editor = os.environ.get("EDITOR", "vi")
        os.execvp(editor, [editor, str(target_path)])

    return 0

# --------------------------------------------------------------------------
# index command
# --------------------------------------------------------------------------

def iter_doc_files() -> list[Path]:
    if not DOCS_ROOT.exists(): return []
    out = []
    for d in sorted(DOCS_ROOT.iterdir()):
        if not d.is_dir(): continue
        for f in sorted(d.iterdir()):
            if f.suffix == ".md":
                out.append(f)
    return out

def cmd_index(args: argparse.Namespace) -> int:
    by_dir = defaultdict(list)
    for f in iter_doc_files():
        text = f.read_text(encoding="utf-8", errors="replace")
        fm, _ = parse_frontmatter(text)
        rec = {
            "path": f,
            "doc_id": fm.get("doc_id", "????"),
            "type": fm.get("type", "?"),
            "topics": fm.get("topics", []) if isinstance(fm.get("topics"), list) else [fm.get("topics", "")],
            "summary": fm.get("summary", ""),
            "status": fm.get("status", "active"),
            "supersedes": fm.get("supersedes", []),
        }
        d = f.parent.name
        by_dir[d].append(rec)

    lines = [
        "# DOCS Index",
        "",
        "Auto-generated by `newdoc.py index`.",
        "",
        "All documents in `main/DOCS/`, grouped by directory.",
        "Within a directory, the file with the **highest DOC id** is the most recent.",
        "",
    ]
    for d in sorted(by_dir.keys()):
        files = by_dir[d]
        lines.append(f"## {d} ({len(files)} files)")
        lines.append("")
        lines.append("| DOC | filename | type | topics | summary |")
        lines.append("|-----|----------|------|--------|---------|")
        for rec in sorted(files, key=lambda r: r["doc_id"], reverse=True):
            topics = ", ".join(rec["topics"]) if isinstance(rec["topics"], list) else rec["topics"]
            summary = (rec["summary"] or "").replace("|", "\\|")[:140]
            fname = rec["path"].name
            status_marker = "" if rec["status"] == "active" else f" *({rec['status']})*"
            lines.append(f"| {rec['doc_id']} | {fname}{status_marker} | {rec['type']} | {topics} | {summary} |")
        lines.append("")

    DOCS_ROOT.mkdir(parents=True, exist_ok=True)
    INDEX_FILE.write_text("\n".join(lines))
    return 0

# --------------------------------------------------------------------------
# lint command
# --------------------------------------------------------------------------

def cmd_lint(args: argparse.Namespace) -> int:
    errors = 0
    fixes = 0
    known_topics = load_topics()

    # Pass 1: gather (doc_id -> path, fm)
    docs = {}
    for f in iter_doc_files():
        text = f.read_text(encoding="utf-8", errors="replace")
        fm, body = parse_frontmatter(text)
        if not fm:
            print(f"LINT: {f.relative_to(ROOT)}: missing frontmatter", file=sys.stderr)
            errors += 1
            continue
        # Required fields
        for req in ("type", "date", "origin", "doc_id", "topics", "summary"):
            if req not in fm:
                print(f"LINT: {f.relative_to(ROOT)}: missing field '{req}'", file=sys.stderr)
                errors += 1
        # Type vocabulary
        if fm.get("type") not in VALID_TYPES:
            print(f"LINT: {f.relative_to(ROOT)}: invalid type '{fm.get('type')}'", file=sys.stderr)
            errors += 1
        # Origin vocabulary
        if fm.get("origin") and fm["origin"] not in VALID_ORIGINS:
            print(f"LINT: {f.relative_to(ROOT)}: invalid origin '{fm['origin']}'", file=sys.stderr)
            errors += 1
        # Topics
        topics = fm.get("topics", [])
        if not isinstance(topics, list): topics = [topics]
        if not topics:
            print(f"LINT: {f.relative_to(ROOT)}: empty topics", file=sys.stderr)
            errors += 1
        for t in topics:
            if t and t not in known_topics:
                print(f"LINT: {f.relative_to(ROOT)}: unknown topic '{t}'", file=sys.stderr)
                errors += 1
        # doc_id ↔ filename
        doc_id = fm.get("doc_id", "")
        if not re.match(r"^DOC\d{4}$", doc_id):
            print(f"LINT: {f.relative_to(ROOT)}: doc_id '{doc_id}' not in DOCNNNN form", file=sys.stderr)
            errors += 1
        elif f"__{doc_id}.md" not in f.name:
            print(f"LINT: {f.relative_to(ROOT)}: doc_id {doc_id} doesn't match filename {f.name}", file=sys.stderr)
            errors += 1
        # Summary length
        s = fm.get("summary", "")
        if isinstance(s, str) and len(s) > 200:
            print(f"LINT: {f.relative_to(ROOT)}: summary length {len(s)} > 200", file=sys.stderr)
            errors += 1
        docs[doc_id] = (f, fm, body)

    # Pass 2: supersedes / superseded_by integrity
    for doc_id, (f, fm, body) in docs.items():
        sup = fm.get("supersedes", [])
        if not isinstance(sup, list): sup = [sup] if sup else []
        for x in sup:
            if x not in docs:
                print(f"LINT: {f.relative_to(ROOT)}: supersedes references unknown {x}", file=sys.stderr)
                errors += 1
                continue
            tf, tfm, tbody = docs[x]
            existing_sb = tfm.get("superseded_by", [])
            if not isinstance(existing_sb, list): existing_sb = [existing_sb] if existing_sb else []
            if doc_id not in existing_sb:
                if args.fix:
                    new_sb = existing_sb + [doc_id]
                    tfm["superseded_by"] = sorted(set(new_sb))
                    tfm["status"] = "superseded"
                    tf.write_text(render_frontmatter(tfm) + tbody)
                    fixes += 1
                    print(f"FIX: {tf.relative_to(ROOT)}: added superseded_by {doc_id}", file=sys.stderr)
                else:
                    print(f"LINT: {tf.relative_to(ROOT)}: missing superseded_by {doc_id}", file=sys.stderr)
                    errors += 1

    if args.fix and fixes:
        cmd_index(argparse.Namespace())
    if errors:
        return 5
    return 0

# --------------------------------------------------------------------------
# list command
# --------------------------------------------------------------------------

def cmd_list(args: argparse.Namespace) -> int:
    records = []
    for f in iter_doc_files():
        text = f.read_text(encoding="utf-8", errors="replace")
        fm, _ = parse_frontmatter(text)
        if not fm: continue
        if args.type and fm.get("type") != args.type: continue
        topics = fm.get("topics", [])
        if not isinstance(topics, list): topics = [topics]
        if args.topic and args.topic not in topics: continue
        records.append((fm.get("doc_id", ""), f, fm.get("summary", "")))
    for doc_id, f, summary in sorted(records, reverse=True):
        print(f"{doc_id}  {f.relative_to(ROOT)}  —  {summary}")
    return 0

# --------------------------------------------------------------------------
# renumber HEAD command (merge-conflict resolution)
# --------------------------------------------------------------------------

def cmd_renumber(args: argparse.Namespace) -> int:
    """Re-id the docs introduced by the HEAD commit to the next free counter values."""
    # Find files added by HEAD
    out = subprocess.check_output(
        ["git", "diff-tree", "--no-commit-id", "--name-status", "-r", "HEAD"],
        cwd=ROOT, text=True
    )
    new_files = []
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and parts[0] == "A" and parts[1].startswith("main/DOCS/"):
            p = ROOT / parts[1]
            if p.exists() and p.suffix == ".md":
                new_files.append(p)

    if not new_files:
        print("No new docs in HEAD.", file=sys.stderr)
        return 0

    counter = read_counter()
    for f in new_files:
        text = f.read_text(encoding="utf-8", errors="replace")
        fm, body = parse_frontmatter(text)
        if not fm or "doc_id" not in fm: continue
        old_id = fm["doc_id"]
        counter += 1
        new_id = f"DOC{counter:04d}"
        # Rename file
        old_name = f.name
        new_name = re.sub(r"__DOC\d{4}\.md$", f"__{new_id}.md", old_name)
        new_path = f.parent / new_name
        # Update frontmatter
        fm["doc_id"] = new_id
        new_text = render_frontmatter(fm) + body
        f.rename(new_path)
        new_path.write_text(new_text)
        print(f"Renumbered {old_id} -> {new_id} ({old_name} -> {new_name})", file=sys.stderr)
    write_counter(counter)
    cmd_index(argparse.Namespace())
    return 0

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(prog="newdoc")
    sub = parser.add_subparsers(dest="cmd")

    # The default behaviour is creating a new doc; the first positional is the type.
    # We support both `newdoc.py impl_plan "title"` and `newdoc.py new impl_plan "title"`.
    p_new = sub.add_parser("new", help="Create a new doc (default)")
    p_new.add_argument("type", help="One of: " + ", ".join(sorted(VALID_TYPES)))
    p_new.add_argument("title", help="Document title")
    p_new.add_argument("--topics", default="", help="Comma-separated topic tags")
    p_new.add_argument("--origin", default="ai_output", help="Default: ai_output")
    p_new.add_argument("--summary", default="", help="One-line summary (defaults to title)")
    p_new.add_argument("--supersedes", default="", help="Comma-separated DOC ids replaced")
    p_new.add_argument("--allow-new-topic", action="store_true", help="Allow unknown topics")
    p_new.add_argument("--editor", action="store_true", help="Open $EDITOR after creating")
    p_new.add_argument("--dry-run", action="store_true", help="Print what would happen")
    p_new.set_defaults(func=cmd_new)

    p_idx = sub.add_parser("index", help="Regenerate INDEX.md")
    p_idx.set_defaults(func=cmd_index)

    p_lint = sub.add_parser("lint", help="Validate every doc")
    p_lint.add_argument("--fix", action="store_true", help="Auto-fix where possible")
    p_lint.set_defaults(func=cmd_lint)

    p_list = sub.add_parser("list", help="List docs")
    p_list.add_argument("--type")
    p_list.add_argument("--topic")
    p_list.set_defaults(func=cmd_list)

    p_renum = sub.add_parser("renumber", help="Re-id HEAD-introduced docs (merge conflicts)")
    p_renum.add_argument("rev", default="HEAD", nargs="?")
    p_renum.set_defaults(func=cmd_renumber)

    # If first arg is a type, treat as implicit `new`.
    argv = sys.argv[1:]
    if argv and argv[0] in VALID_TYPES:
        argv = ["new", *argv]

    args = parser.parse_args(argv)
    if not args.cmd:
        parser.print_help()
        return 1
    return args.func(args)

if __name__ == "__main__":
    sys.exit(main())
