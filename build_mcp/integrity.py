"""Deterministic content-integrity primitives for the build MCP."""

from __future__ import annotations

import hashlib
import json
import os
from collections.abc import Mapping
from pathlib import Path
from typing import Any


_HASH_CHUNK_SIZE = 1024 * 1024


def sha256_bytes(value: bytes | bytearray | memoryview) -> str:
    """Return the lowercase SHA-256 digest for an in-memory byte sequence."""
    return hashlib.sha256(value).hexdigest()


def sha256_file(path_or_descriptor: str | os.PathLike[str] | int) -> str:
    """Stream a path or descriptor from offset zero without changing its cursor."""
    digest = hashlib.sha256()
    if isinstance(path_or_descriptor, int):
        offset = 0
        while True:
            chunk = os.pread(path_or_descriptor, _HASH_CHUNK_SIZE, offset)
            if not chunk:
                break
            digest.update(chunk)
            offset += len(chunk)
        return digest.hexdigest()

    with Path(path_or_descriptor).open("rb") as handle:
        while chunk := handle.read(_HASH_CHUNK_SIZE):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json_bytes(value: Any) -> bytes:
    """Serialize any JSON value using the repository's canonical byte form."""
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
        allow_nan=False,
    ).encode("utf-8")


def canonical_json_sha256(value: Any) -> str:
    return sha256_bytes(canonical_json_bytes(value))


def content_sha256(value: Mapping[str, Any]) -> str:
    """Hash a mapping without mutating it or trusting its existing content seal."""
    unsealed = {
        key: member
        for key, member in value.items()
        if key != "content_sha256"
    }
    return canonical_json_sha256(unsealed)
