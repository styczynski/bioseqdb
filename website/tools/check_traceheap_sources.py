#!/usr/bin/env python3
"""Keep the C++ table-AM tutorial excerpts and reproducible download in sync."""
from __future__ import annotations

import argparse
import gzip
import io
from pathlib import Path
import re
import shlex
import tarfile

WEBSITE = Path(__file__).resolve().parents[1]
POST = WEBSITE / "blog/writing-a-postgres-table-access-method-in-cpp.md"
SOURCE = WEBSITE / "static/examples/traceheap"
ARCHIVE = WEBSITE / "static/examples/traceheap.tar.gz"
FENCES = re.compile(r"^```([^\n]*)\n(.*?)^```[ \t]*$", re.M | re.S)
EXPECTED = {
    "traceheap_makefile": ("makefile", "Makefile", True),
    "traceheap_registration": ("sql", "traceheap--1.0.sql", False),
    "traceheap_headers": ("cpp", "traceheap.cpp", False),
    "traceheap_handler": ("cpp", "traceheap.cpp", False),
    "traceheap_begin": ("cpp", "traceheap.cpp", False),
    "traceheap_next": ("cpp", "traceheap.cpp", False),
    "traceheap_insert": ("cpp", "traceheap.cpp", False),
    "traceheap_toast": ("cpp", "traceheap.cpp", False),
}
FILES = ("Makefile", "README.md", "traceheap.control", "traceheap--1.0.sql",
         "traceheap.cpp", "test.sql")


def normalized(text, language):
    if language == "cpp":
        text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"\s+", "", text)


def validate_excerpts():
    seen = set()
    for info, code in FENCES.findall(POST.read_text()):
        parts = shlex.split(info)
        ids = [item[3:] for item in parts if item.startswith("id=traceheap_")]
        if not ids:
            continue
        assert len(ids) == 1 and ids[0] in EXPECTED, f"unknown excerpt: {info}"
        identifier = ids[0]
        assert identifier not in seen, f"duplicate excerpt: {identifier}"
        seen.add(identifier)
        language, file, whole = EXPECTED[identifier]
        assert parts[0] == language
        excerpt = normalized(code, language)
        source = normalized((SOURCE / file).read_text(), language)
        assert excerpt == source if whole else excerpt in source, f"source drift: {identifier}"
    assert seen == set(EXPECTED), f"missing excerpts: {set(EXPECTED) - seen}"


def archive_bytes():
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for file in FILES:
            data = (SOURCE / file).read_bytes()
            info = tarfile.TarInfo("traceheap/" + file)
            info.size = len(data)
            info.mode = 0o644
            info.mtime = 0
            archive.addfile(info, io.BytesIO(data))
    packed = io.BytesIO()
    with gzip.GzipFile(filename="", fileobj=packed, mode="wb", mtime=0) as compressed:
        compressed.write(raw.getvalue())
    return packed.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Check without rewriting the archive")
    args = parser.parse_args()
    validate_excerpts()
    archive = archive_bytes()
    if args.check:
        assert ARCHIVE.is_file() and ARCHIVE.read_bytes() == archive, (
            "traceheap archive has drifted; run website/tools/check_traceheap_sources.py")
    else:
        ARCHIVE.write_bytes(archive)
    print(f"traceheap tutorial: PASS ({len(EXPECTED)} source excerpts, {len(FILES)} download files)")


if __name__ == "__main__":
    main()
