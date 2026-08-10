"""Durable filesystem-publication primitives for the build MCP."""

from __future__ import annotations

import contextlib
import os
import tempfile
from collections.abc import Callable
from pathlib import Path


_COPY_CHUNK_SIZE = 1024 * 1024


def copy_descriptor_exact(source_descriptor: int, destination_descriptor: int) -> None:
    """Copy a descriptor from offset zero without changing its cursor."""
    offset = 0
    while True:
        chunk = os.pread(source_descriptor, _COPY_CHUNK_SIZE, offset)
        if not chunk:
            return
        view = memoryview(chunk)
        while view:
            count = os.write(destination_descriptor, view)
            if count <= 0:
                raise OSError("short write while publishing streamed content")
            view = view[count:]
        offset += len(chunk)


def fsync_directory(path: Path) -> None:
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    descriptor = os.open(str(path), flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def durable_replace_stream(
    destination: Path,
    write: Callable[[int], None],
    *,
    mode: int = 0o600,
) -> None:
    """Write and fsync a sibling temporary, replace, then fsync the parent."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.",
        suffix=".tmp",
        dir=destination.parent,
    )
    temporary: Path | None = Path(temporary_name)
    try:
        os.fchmod(descriptor, mode)
        write(descriptor)
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        os.replace(temporary, destination)
        temporary = None
        fsync_directory(destination.parent)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary is not None:
            with contextlib.suppress(FileNotFoundError):
                temporary.unlink()


def durable_replace_bytes(
    destination: Path,
    payload: bytes,
    *,
    mode: int = 0o600,
) -> None:
    def write(descriptor: int) -> None:
        view = memoryview(payload)
        while view:
            count = os.write(descriptor, view)
            if count <= 0:
                raise OSError("short write while publishing durable content")
            view = view[count:]

    durable_replace_stream(destination, write, mode=mode)


def publish_private_exclusive(
    destination: Path,
    write: Callable[[int], None],
    *,
    mode: int,
) -> None:
    """Exclusively publish a private file without following a final symlink."""
    flags = os.O_RDWR | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(destination, flags, mode)
    published = False
    try:
        write(descriptor)
        os.fchmod(descriptor, mode)
        os.fsync(descriptor)
        fsync_directory(destination.parent)
        published = True
    finally:
        os.close(descriptor)
        if not published:
            with contextlib.suppress(FileNotFoundError):
                destination.unlink()
