#!/usr/bin/env python3
"""Losslessly compress old, closed macOS E2E reports without changing their paths.

APFS transparent compression preserves ordinary JSON reads. Never touches build
trees, receipt registries, current reports, or benchmark target/index files.
Called only between local benchmark runs, with a bounded maintenance window.
"""
from __future__ import annotations

import filecmp
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time


def compress_idle_evidence(root: Path, *, minimum_free_gib: int = 48,
                           max_seconds: int = 45) -> dict:
    result = {'files': 0, 'allocated_bytes_reclaimed': 0}
    if sys.platform != 'darwin':
        return result
    started = time.monotonic()
    cutoff = time.time() - 86400
    candidates = []
    scopes = [root / 'reporting/bioseq_benchmark_e2e',
              *(p for p in root.glob('.bioseq*') if p.is_dir() and not p.is_symlink())]
    for scope in scopes:
        for directory, dirs, files in os.walk(scope, followlinks=False):
            dirs[:] = [name for name in dirs if not name.startswith('.compression-')]
            for name in files:
                if not re.fullmatch(r'report_\d+(?:\.raw\.jsonl|\.json)', name):
                    continue
                path = Path(directory) / name
                info = path.lstat()
                if (stat.S_ISREG(info.st_mode) and info.st_nlink == 1
                        and info.st_size >= 10 * 1024**2 and info.st_mtime < cutoff
                        and not info.st_flags & stat.UF_COMPRESSED):
                    candidates.append((info.st_size, path))
    for _, path in sorted(candidates, reverse=True):
        if (time.monotonic() - started >= max_seconds
                or shutil.disk_usage(root).free >= minimum_free_gib * 1024**3):
            break
        before = path.stat()
        opened = subprocess.run(['lsof', str(path)], capture_output=True, timeout=15)
        if opened.returncode != 1 or opened.stdout or opened.stderr:
            continue
        with tempfile.TemporaryDirectory(prefix='.compression-', dir=path.parent) as staging:
            compressed = Path(staging) / path.name
            subprocess.run(['/usr/bin/ditto', '--hfsCompression', '--nocache',
                            str(path), str(compressed)], check=True, timeout=60)
            after = path.stat()
            packed = compressed.stat()
            if ((before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
                    != (after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
                    or not filecmp.cmp(path, compressed, shallow=False)
                    or packed.st_blocks >= before.st_blocks):
                continue
            # Same-filesystem atomic replacement; all file contents and paths
            # remain intact, including references from historical authorities.
            os.replace(compressed, path)
            result['files'] += 1
            result['allocated_bytes_reclaimed'] += (before.st_blocks - packed.st_blocks) * 512
    return result
