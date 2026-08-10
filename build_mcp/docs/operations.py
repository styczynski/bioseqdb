"""Doc operations exposed as MCP tools; each returns a JSON string."""
from __future__ import annotations

import json

from . import registry, schemas
from .qmd_manager import get_manager, QmdUnavailable


def _err(msg, **x):
    return json.dumps({"error": msg, **x}, indent=2)


def doc_create(args: dict) -> str:
    try:
        schemas.validate("doc_create", args)
        path = registry.create_document(args["type"], args["title"], args["topics"],
            summary=args.get("summary", ""), origin=args.get("origin", "ai_output"),
            supersedes=args.get("supersedes"), allow_new_topic=args.get("allow_new_topic", False))
    except (schemas.ValidationError, registry.DocError) as e:
        return _err(str(e))
    try:
        indexed = get_manager().reindex_after_write()
    except QmdUnavailable as e:
        return json.dumps({"path": str(path), "indexed": False, "warning": str(e)}, indent=2)
    return json.dumps({"path": str(path), "indexed": indexed,
                       "note": "FTS-indexed now; embeddings refresh in background. Open the path "
                               "with Read and write the body."}, indent=2)


def doc_archive(args: dict) -> str:
    try:
        schemas.validate("doc_archive", args)
        old, new = registry.archive_document(args["doc"])
    except (schemas.ValidationError, registry.DocError) as e:
        return _err(str(e))
    try:
        reindexed = get_manager().reindex_after_write()
    except QmdUnavailable:
        reindexed = False
    return json.dumps({"archived_from": str(old), "archived_to": str(new),
                       "reindexed": reindexed}, indent=2)


def doc_search(args: dict) -> str:
    try:
        schemas.validate("doc_search", args)
    except schemas.ValidationError as e:
        return _err(str(e))
    try:
        out = get_manager().search(args["query"], limit=args.get("limit", 10),
            min_score=args.get("min_score", 0.0),
            include_archived=args.get("include_archived", False), intent=args.get("intent"))
    except QmdUnavailable as e:
        return _err(str(e), hint="qmd not installed/healthy; npm install in build_mcp")
    return json.dumps(out, indent=2)


def doc_read(args: dict) -> str:
    try:
        schemas.validate("doc_read", args)
        info = registry.find_document(args["doc"])
    except (schemas.ValidationError, registry.DocError) as e:
        return _err(str(e))
    return json.dumps({"path": str(info.path), "doc_id": info.doc_id, "type": info.type,
                       "archived": info.archived, "status": info.status, "topics": info.topics,
                       "note": "Open `path` with the Read tool to view the document."}, indent=2)
