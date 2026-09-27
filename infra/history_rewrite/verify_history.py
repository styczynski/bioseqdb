#!/usr/bin/env python3
"""Independent full-history audit of Git objects, separate from the writer.

Usage: verify_history.py SOURCE REWRITTEN COMMIT_MAP_JSON
The map records the exact refs admitted for publication. Additional refs fail.
"""
import argparse
import functools
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def git(repo, *args, data=None):
    return subprocess.check_output(['git', '--no-replace-objects', '-C', str(Path(repo).resolve()), *args],
                                   input=data, env=dict(os.environ, GIT_OPTIONAL_LOCKS='0', GIT_NO_LAZY_FETCH='1'))


@functools.lru_cache(maxsize=None)
def private_document(path, extra_paths=()):
    return (bool(re.search(rb'\.(md|markdown|mdown|mkd|mkdn|mdx|mdtxt|mdtext)$', path, re.I)) or path in extra_paths) and not re.search(rb'(^|/)website/', path)


READERS = {}


def object_bytes(repo, oid, expected):
    if repo not in READERS:
        READERS[repo] = subprocess.Popen(['git', '--no-replace-objects', '-C', repo, 'cat-file', '--batch'],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    reader = READERS[repo]
    reader.stdin.write(oid.encode()+b'\n');reader.stdin.flush()
    head = reader.stdout.readline().split()
    assert len(head) == 3 and head[0].decode() == oid and head[1] == expected
    data = reader.stdout.read(int(head[2]))
    assert len(data) == int(head[2]) and reader.stdout.read(1) == b'\n'
    return data


@functools.lru_cache(maxsize=32768)
def tree_entries(repo, oid):
    data = object_bytes(repo, oid, b'tree')
    entries = tuple((m.group(1), m.group(2), m.group(3).hex())
                    for m in re.finditer(rb'([0-7]+) ([^\x00]+)\x00(.{20})', data, re.S))
    assert b''.join(mode+b' '+name+b'\0'+bytes.fromhex(child) for mode,name,child in entries) == data
    return entries


def walk_tree(repo, oid, prefix=b''):
    for mode, name, child in tree_entries(repo, oid):
        path = prefix+name
        if mode == b'40000':
            yield from walk_tree(repo, child, path+b'/')
        else:
            yield path, (mode, b'commit' if mode == b'160000' else b'blob', child)


@functools.lru_cache(maxsize=12)
def tree(repo, revision):
    fields, _ = metadata(repo, revision)
    root = next(f[5:].decode() for f in fields if f.startswith(b'tree '))
    return dict(walk_tree(repo, root))


@functools.lru_cache(maxsize=256)
def metadata(repo, oid):
    raw = object_bytes(repo, oid, b'commit')
    head, msg = raw.split(b'\n\n', 1)
    fields = []
    for line in head.split(b'\n'):
        if line.startswith(b' '):
            fields[-1] += b'\n' + line
        else:
            fields.append(line)
    return fields, msg


def _verify(source, destination, map_path):
    source, destination = str(Path(source).resolve()), str(Path(destination).resolve())
    record = json.loads(Path(map_path).read_text())
    mapping = record['commits']
    extra_paths = tuple(os.fsencode(p) for p in record.get('extra_private_paths', []))
    source_refs = dict(line.split(' ', 1) for line in git(source, 'for-each-ref', '--format=%(refname) %(objectname)', 'refs/heads', 'refs/tags').decode().splitlines())
    assert source_refs == record['source_refs'], 'Source refs changed'
    destination_refs = dict(line.split(' ', 1) for line in git(destination, 'for-each-ref', '--format=%(refname) %(objectname)').decode().splitlines())
    expected_refs = record.get('publication_refs', record['rewritten_refs'])
    assert destination_refs == expected_refs, 'Unexpected, missing or changed publication refs'
    source_commits = git(source, 'rev-list', '--reverse', '--topo-order', '--stdin', data=('\n'.join(source_refs)+'\n').encode()).decode().splitlines()
    assert set(source_commits) == set(mapping), 'Incomplete original commit coverage'
    reachable = set(git(destination, 'rev-list', '--all').decode().splitlines())
    additions = record.get('cleanup_commits', {})
    assert reachable == set(mapping.values()) | set(additions), 'Unexpected or missing destination commits'
    assert len(mapping) == len(set(mapping.values())), 'Commits were collapsed'
    private_blobs, preserved_blobs, website_blobs = set(), set(), set()
    pointer_checks = unchanged_checks = website_checks = 0
    for index, old in enumerate(source_commits, 1):
        new = mapping[old]
        old_headers, old_message = metadata(source, old)
        new_headers, new_message = metadata(destination, new)
        old_parents = [h[7:].decode() for h in old_headers if h.startswith(b'parent ')]
        new_parents = [h[7:].decode() for h in new_headers if h.startswith(b'parent ')]
        assert new_parents == [mapping[x] for x in old_parents], ('Parent topology', old)
        excluded = (b'tree', b'parent', b'gpgsig', b'gpgsig-sha256', b'mergetag')
        old_kept = [h for h in old_headers if h.split(b' ', 1)[0] not in excluded]
        new_kept = [h for h in new_headers if h.split(b' ', 1)[0] not in (b'tree', b'parent')]
        assert old_kept == new_kept, ('Metadata', old)
        assert new_message == old_message.split(b'\n', 1)[0].split(b'\r', 1)[0] + b'\n', ('Message', old)
        before, after = tree(source, old), tree(destination, new)
        assert before.keys() == after.keys(), ('Paths changed', old)
        parent_trees = [(tree(source, p), tree(destination, mapping[p])) for p in old_parents]
        for path, entry in before.items():
            if private_document(path, extra_paths):
                private_blobs.add(entry[2])
                matches = [n[path][2] for o, n in parent_trees if o.get(path) == entry]
                expected_data = old.encode() + b'\n'
                expected_blob = matches[0] if matches else hashlib.sha1(b'blob 41\0' + expected_data).hexdigest()
                expected_mode = b'100644' if entry[0] == b'120000' else entry[0]
                assert after[path] == (expected_mode, b'blob', expected_blob), ('Invalid private document placeholder', old, path)
                pointer_checks += 1
            else:
                assert entry == after[path], ('Preserved file changed', old, path)
                unchanged_checks += 1
                if entry[1] == b'blob':
                    preserved_blobs.add(entry[2])
                    if re.search(rb'(^|/)website/', path):
                        website_blobs.add(entry[2])
                        website_checks += 1
        if index % 100 == 0:
            print(f'Audited {index}/{len(source_commits)} original commits', flush=True)
    # Scan every final commit, including cleanup commits, with a separate content check.
    batch = subprocess.Popen(['git', '-C', destination, 'cat-file', '--batch'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    checked_blobs = set()
    try:
        for scan_index, revision in enumerate(sorted(reachable), 1):
            for path, (mode, kind, oid) in tree(destination, revision).items():
                if private_document(path, extra_paths) and oid not in checked_blobs:
                    batch.stdin.write(oid.encode()+b'\n'); batch.stdin.flush()
                    header = batch.stdout.readline().split()
                    assert len(header) == 3 and header[1] == b'blob' and int(header[2]) == 41, ('Private content survived', revision, path)
                    content = batch.stdout.read(41)
                    assert batch.stdout.read(1) == b'\n'
                    assert re.fullmatch(rb'[0-9a-f]{40}\n', content) and content[:-1].decode() in mapping, ('Bad provenance pointer', revision, path)
                    assert mode in (b'100644', b'100755'), ('Unsafe document mode', revision, path)
                    checked_blobs.add(oid)
            if scan_index % 500 == 0:
                print(f'Scanned Markdown in {scan_index}/{len(reachable)} final commits', flush=True)
        for revision, info in additions.items():
            parent = info['parent']
            new_headers, msg = metadata(destination, revision)
            assert [h[7:].decode() for h in new_headers if h.startswith(b'parent ')] == [parent]
            assert b'\n' not in msg.rstrip(b'\n') and b'\r' not in msg
            expected = dict(tree(destination, parent))
            for path in info['removed']:
                del expected[os.fsencode(path)]
            for old, new in info['moved'].items():
                entry = expected.pop(os.fsencode(old))
                assert os.fsencode(new) not in expected, 'Move collision'
                expected[os.fsencode(new)] = entry
            for path, entry in info.get('added', {}).items():
                assert os.fsencode(path) not in expected
                expected[os.fsencode(path)] = (entry[0].encode(), entry[1].encode(), entry[2])
            for path, entry in info.get('modified', {}).items():
                assert os.fsencode(path) in expected
                expected[os.fsencode(path)] = (entry[0].encode(), entry[1].encode(), entry[2])
            assert tree(destination, revision) == expected, 'Unplanned cleanup changes'
    finally:
        batch.stdin.close()
        assert batch.wait() == 0
        batch.stdout.close()
    fsck = git(destination, 'fsck', '--full', '--strict', '--no-reflogs')
    assert not fsck.strip(), ('Unexpected unreachable objects', fsck[:1000])
    # All actual objects, not merely branch tips, are examined for old private blobs.
    objects = git(destination, 'cat-file', '--batch-all-objects', '--batch-check=%(objectname) %(objecttype)').decode().splitlines()
    present = {line.split()[0] for line in objects}
    aliases = (private_blobs & present) - website_blobs
    assert not aliases, f'{len(aliases)} original Markdown blobs survive under other paths'
    print(json.dumps({'status': 'passed', 'original_commits': len(mapping), 'total_commits': len(reachable),
                      'refs': len(destination_refs), 'private_document_snapshots': pointer_checks,
                      'unchanged_file_snapshots': unchanged_checks, 'website_file_snapshots': website_checks,
                      'verified_placeholder_blobs': len(checked_blobs), 'all_objects': len(objects)}, sort_keys=True), flush=True)


def verify(source, destination, map_path):
    if not __debug__:
        raise RuntimeError('Auditing with disabled Python assertions is forbidden')
    try:
        return _verify(source, destination, map_path)
    finally:
        for reader in READERS.values():
            reader.stdin.close();reader.wait();reader.stdout.close()
        READERS.clear()
        tree.cache_clear();tree_entries.cache_clear();metadata.cache_clear()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source')
    parser.add_argument('destination')
    parser.add_argument('map_path')
    args = parser.parse_args()
    verify(args.source, args.destination, args.map_path)
