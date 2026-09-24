#!/usr/bin/env python3
"""Reclaim disk with transparent macOS compression; preserve report paths/bytes.

Only retired benchmark JSON evidence is eligible. Each staged copy must have
the same SHA-256 and save allocated blocks before it atomically replaces the
unchanged original. No result rows or raw evidence are deleted or rewritten.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / 'reporting/bioseq_benchmark_e2e'


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.digest()


def identity(stat):
    return stat.st_ino, stat.st_size, stat.st_mtime_ns


def compress(path):
    original = path.stat()
    if path.is_symlink() or original.st_size < 128 * 1024:
        return 0
    # Already compressed/sparse files need no second pass.
    if original.st_blocks * 512 < original.st_size // 2:
        return 0
    before = sha256(path)
    with tempfile.TemporaryDirectory(prefix='.compressed-report-', dir=path.parent) as temporary:
        staged = Path(temporary) / path.name
        subprocess.run(['/usr/bin/ditto', '--hfsCompression', str(path), str(staged)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=60)
        saved = (original.st_blocks - staged.stat().st_blocks) * 512
        if saved <= 0:
            return 0
        if staged.stat().st_size != original.st_size or sha256(staged) != before:
            raise RuntimeError(f'Compressed copy changed evidence bytes: {path}')
        if identity(path.stat()) != identity(original):
            raise RuntimeError(f'Evidence changed during compression: {path}')
        os.replace(staged, path)
        return saved


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--limit', type=int, default=0)
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('Apple ditto/APFS compression requires macOS')
    files = sorted((p for p in EVIDENCE.rglob('*')
                    if p.suffix in ('.json', '.jsonl') and p.is_file() and not p.is_symlink()
                    and p.stat().st_size >= 128 * 1024
                    and p.stat().st_blocks * 512 >= p.stat().st_size // 2),
                   key=lambda p: p.stat().st_size, reverse=True)
    saved = compressed = examined = 0
    for path in files:
        if args.limit and examined >= args.limit:
            break
        examined += 1
        if args.apply:
            difference = compress(path)
            saved += difference
            compressed += bool(difference)
        if examined % 100 == 0:
            print(json.dumps(dict(examined=examined, compressed=compressed,
                                  saved_allocated_bytes=saved)), flush=True)
    print(json.dumps(dict(timestamp_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        examined=examined, compressed=compressed, saved_allocated_bytes=saved,
        applied=args.apply, evidence_paths_and_sha256_preserved=True)), flush=True)


if __name__ == '__main__':
    main()
