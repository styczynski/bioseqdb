#!/usr/bin/env python3
"""newdoc — thin CLI over the build MCP's docs package (create/search/archive/read).

Document tooling lives in build_mcp/docs and is primarily driven through the build
MCP tools (doc_create/doc_search/doc_archive/doc_read). This CLI is for humans who prefer the
terminal. There is no INDEX.md or .doc_counter anymore — DOC ids are max+1 over the filesystem
and discovery is via QMD search.

  newdoc.py new <type> "<title>" --topics tag1,tag2 [--summary ...] [--origin ...]
  newdoc.py search "<query>" [--include-archived]
  newdoc.py read <DOCxxxx|number|filename>
  newdoc.py archive <DOCxxxx|number|filename>
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from docs import registry                                  # noqa: E402
from docs.qmd_manager import get_manager, QmdUnavailable   # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(prog="newdoc")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("new")
    p.add_argument("type")
    p.add_argument("title")
    p.add_argument("--topics", default="")
    p.add_argument("--summary", default="")
    p.add_argument("--origin", default="ai_output")
    p.add_argument("--supersedes", default="")
    p.add_argument("--allow-new-topic", action="store_true")
    a_ = sub.add_parser("archive")
    a_.add_argument("doc")
    r_ = sub.add_parser("read")
    r_.add_argument("doc")
    q_ = sub.add_parser("search")
    q_.add_argument("query")
    q_.add_argument("--include-archived", action="store_true")
    a = ap.parse_args()
    try:
        if a.cmd == "new":
            path = registry.create_document(
                a.type, a.title, [t for t in a.topics.split(",") if t.strip()],
                summary=a.summary, origin=a.origin,
                supersedes=[s for s in a.supersedes.split(",") if s.strip()],
                allow_new_topic=a.allow_new_topic)
            print(path)
            try:
                get_manager().reindex_after_write()
            except QmdUnavailable as e:
                print(f"(not indexed: {e})", file=sys.stderr)
        elif a.cmd == "archive":
            _, new = registry.archive_document(a.doc)
            print(new)
            try:
                get_manager().reindex_after_write()
            except QmdUnavailable:
                pass
        elif a.cmd == "read":
            print(registry.find_document(a.doc).path)
        elif a.cmd == "search":
            print(json.dumps(get_manager().search(a.query, include_archived=a.include_archived),
                             indent=2))
    except registry.DocError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
