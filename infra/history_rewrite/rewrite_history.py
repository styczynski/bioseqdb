#!/usr/bin/env python3
"""Rewrite into a NEW object database; never modify or push the input repository.

Usage: rewrite_history.py SOURCE NEW_BARE_REPO COMMIT_MAP_JSON
Changed private Markdown becomes its original commit ID plus a newline;
unchanged documents inherit their placeholder from a matching parent.
Only refs/heads and refs/tags are exported. No alternates, reflogs or source packs
are copied. Commit identities, timestamps, encoding and parent order are retained.
Cryptographic signatures cannot survive changed objects and are removed.
"""
import argparse
import functools
import hashlib
import json
import os
from pathlib import Path
import subprocess
import zlib

MARKDOWN = (b'.md', b'.markdown', b'.mdown', b'.mkd', b'.mkdn', b'.mdx', b'.mdtxt', b'.mdtext')
DROP_HEADERS = {b'gpgsig', b'gpgsig-sha256', b'mergetag'}


def git(repo, *args, data=None):
    """Run Git against an explicit repository, ignoring replace refs and hooks."""
    return subprocess.check_output(
        ['git', '--no-replace-objects', '-C', str(Path(repo).resolve()),
         '-c', 'core.hooksPath=/dev/null', *args], input=data,
        env=dict(os.environ, GIT_OPTIONAL_LOCKS='0', GIT_NO_LAZY_FETCH='1'))


def headers(raw):
    """Parse headers without decoding identities or timestamps."""
    head, message = raw.split(b'\n\n', 1)
    fields = []
    for line in head.split(b'\n'):
        if line.startswith(b' '):
            fields[-1] += b'\n' + line
        else:
            fields.append(line)
    return fields, message


def subject(message):
    """Use precisely the first physical line, normalizing the line ending."""
    return message.split(b'\n', 1)[0].split(b'\r', 1)[0] + b'\n'


class Reader:
    """Read exact Git object bytes through Git's persistent batch interface."""
    def __init__(self, repo):
        self.process = subprocess.Popen(
            ['git', '--no-replace-objects', '-C', str(Path(repo).resolve()), 'cat-file', '--batch'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            env=dict(os.environ, GIT_OPTIONAL_LOCKS='0', GIT_NO_LAZY_FETCH='1'))

    def get(self, oid):
        self.process.stdin.write(oid.encode('ascii') + b'\n')
        self.process.stdin.flush()
        line = self.process.stdout.readline().split()
        if len(line) != 3 or line[0].decode() != oid:
            raise ValueError('Missing or invalid object: ' + oid)
        data = self.process.stdout.read(int(line[2]))
        if len(data) != int(line[2]) or self.process.stdout.read(1) != b'\n':
            raise ValueError('Truncated object: ' + oid)
        return line[1], data

    def close(self):
        self.process.stdin.close()
        if self.process.wait() != 0:
            raise RuntimeError('cat-file failed')
        self.process.stdout.close()


class Writer:
    """Write only explicitly selected objects to an isolated SHA-1 repository."""
    def __init__(self, destination):
        self.root = Path(destination).resolve()
        self.written = set()

    def put(self, kind, data):
        raw = kind + b' ' + str(len(data)).encode() + b'\0' + data
        oid = hashlib.sha1(raw).hexdigest()
        if oid not in self.written:
            target = self.root / 'objects' / oid[:2] / oid[2:]
            target.parent.mkdir(exist_ok=True)
            if not target.exists():
                with target.open('xb') as stream:
                    stream.write(zlib.compress(raw))
            self.written.add(oid)
        return oid


class Rewriter:
    def __init__(self, source, destination, extra_private_paths=()):
        self.reader = Reader(source)
        self.writer = Writer(destination)
        self.mapping = {}
        self.commit_trees = {}
        self.output_trees = {}
        self.extra_private_paths = frozenset(os.fsencode(p) for p in extra_private_paths)

    @functools.lru_cache(maxsize=None)
    def tree(self, oid):
        kind, data = self.reader.get(oid)
        if kind != b'tree':
            raise ValueError('Expected tree: ' + oid)
        entries, offset = [], 0
        while offset < len(data):
            space = data.index(b' ', offset)
            nul = data.index(b'\0', space)
            entries.append((data[offset:space], data[space + 1:nul], data[nul + 1:nul + 21].hex()))
            offset = nul + 21
        if offset != len(data):
            raise ValueError('Invalid tree')
        return tuple(entries)

    @functools.lru_cache(maxsize=None)
    def has_markdown(self, oid, prefix=b''):
        return any((self.has_markdown(child, prefix + name + b'/') if mode == b'40000' and name != b'website'
                    else False if mode == b'40000'
                    else name.lower().endswith(MARKDOWN) or prefix + name in self.extra_private_paths)
                   for mode, name, child in self.tree(oid))

    def copy(self, oid, expected):
        if oid in self.writer.written:
            return oid
        kind, data = self.reader.get(oid)
        if kind != expected:
            raise ValueError('Unexpected object type: ' + oid)
        if kind == b'tree':
            for mode, name, child in self.tree(oid):
                if mode != b'160000':
                    self.copy(child, b'tree' if mode == b'40000' else b'blob')
        if self.writer.put(kind, data) != oid:
            raise ValueError('Object hash mismatch')
        return oid

    def output_entries(self, oid):
        return self.output_trees[oid] if oid in self.output_trees else self.tree(oid)

    def rewrite_tree(self, oid, commit_oid, parents=(), prefix=b''):
        if parents and oid == parents[0][0]:
            return parents[0][1]
        if not self.has_markdown(oid, prefix):
            return self.copy(oid, b'tree')
        parent_entries = [(dict((n, (m, o)) for m, n, o in self.tree(old)),
                           dict((n, (m, o)) for m, n, o in self.output_entries(new)))
                          for old, new in parents]
        result, entries = [], []
        for mode, name, child in self.tree(oid):
            if mode == b'40000':
                child_parents = [(old[name][1], new[name][1]) for old, new in parent_entries
                                 if name in old and old[name][0] == b'40000']
                rewritten = self.copy(child, b'tree') if name == b'website' else self.rewrite_tree(child, commit_oid, child_parents, prefix + name + b'/')
            elif name.lower().endswith(MARKDOWN) or prefix + name in self.extra_private_paths:
                if mode == b'160000':
                    raise ValueError('Markdown gitlink needs an explicit policy')
                inherited = [new[name][1] for old, new in parent_entries if old.get(name) == (mode, child)]
                rewritten = inherited[0] if inherited else self.writer.put(b'blob', commit_oid.encode() + b'\n')
                if mode == b'120000':
                    mode = b'100644'
            elif mode == b'160000':
                rewritten = child
            else:
                rewritten = self.copy(child, b'blob')
            result.append(mode + b' ' + name + b'\0' + bytes.fromhex(rewritten))
            entries.append((mode, name, rewritten))
        result_oid = self.writer.put(b'tree', b''.join(result))
        self.output_trees[result_oid] = tuple(entries)
        return result_oid

    def commit(self, oid):
        kind, raw = self.reader.get(oid)
        if kind != b'commit':
            raise ValueError('Expected commit: ' + oid)
        fields, message = headers(raw)
        parents = [self.commit_trees[f[7:].decode()] for f in fields if f.startswith(b'parent ')]
        new_fields = []
        for field in fields:
            key, value = field.split(b' ', 1)
            if key == b'tree':
                tree_oid = self.rewrite_tree(value.decode(), oid, parents)
                self.commit_trees[oid] = (value.decode(), tree_oid)
                field = b'tree ' + tree_oid.encode()
            elif key == b'parent':
                field = b'parent ' + self.mapping[value.decode()].encode()
            elif key in DROP_HEADERS:
                continue
            elif key not in (b'author', b'committer', b'encoding'):
                raise ValueError('Unrecognized commit header: ' + repr(key))
            new_fields.append(field)
        rewritten = self.writer.put(b'commit', b'\n'.join(new_fields) + b'\n\n' + subject(message))
        self.mapping[oid] = rewritten
        return rewritten

    def tag(self, oid):
        kind, raw = self.reader.get(oid)
        if kind == b'commit':
            return self.mapping[oid]
        if kind != b'tag':
            raise ValueError('Tags to standalone trees/blobs are refused: ' + oid)
        fields, message = headers(raw)
        new_fields = []
        for field in fields:
            key, value = field.split(b' ', 1)
            if key == b'object':
                field = b'object ' + self.tag(value.decode()).encode()
            elif key not in (b'type', b'tag', b'tagger'):
                raise ValueError('Unrecognized tag header')
            new_fields.append(field)
        return self.writer.put(b'tag', b'\n'.join(new_fields) + b'\n\n' + subject(message))


def rewrite(source, destination, map_path, extra_private_paths=()):
    source, destination, map_path = map(lambda p: Path(p).resolve(), (source, destination, map_path))
    if destination.exists() or map_path.exists():
        raise ValueError('Destination and map must not already exist')
    if source == destination or source in destination.parents or source in map_path.parents:
        raise ValueError('Outputs must be outside the source repository')
    if git(source, 'rev-parse', '--show-object-format').strip() != b'sha1':
        raise ValueError('Only SHA-1 input repositories are supported')
    if git(source, 'rev-parse', '--is-shallow-repository').strip() != b'false':
        raise ValueError('Shallow input is refused')
    if git(source, 'for-each-ref', '--format=%(refname)', 'refs/replace').strip():
        raise ValueError('Replace refs are refused')
    refs = {}
    for line in git(source, 'for-each-ref', '--format=%(refname) %(objectname)', 'refs/heads', 'refs/tags').splitlines():
        ref, oid = line.decode().split(' ')
        refs[ref] = oid
        git(source, 'rev-parse', '--verify', ref + '^{commit}')
    if not refs:
        raise ValueError('No branches or tags')
    commits = git(source, 'rev-list', '--reverse', '--topo-order', '--stdin', data=('\n'.join(refs) + '\n').encode()).decode().splitlines()
    git(source.parent, 'init', '--bare', '--initial-branch=main', str(destination))
    git(destination, 'config', 'core.logAllRefUpdates', 'false')
    git(destination, 'config', 'push.default', 'nothing')
    worker = Rewriter(source, destination, extra_private_paths)
    try:
        for i, oid in enumerate(commits, 1):
            worker.commit(oid)
            if i % 100 == 0:
                print(f'Rewritten {i}/{len(commits)} commits', flush=True)
        new_refs = {ref: worker.tag(oid) for ref, oid in refs.items()}
        commands = ''.join(f'create {ref} {oid}\n' for ref, oid in new_refs.items())
        git(destination, 'update-ref', '--stdin', data=commands.encode())
        original_head = git(source, 'symbolic-ref', 'HEAD').decode().strip()
        if original_head in new_refs:
            git(destination, 'symbolic-ref', 'HEAD', original_head)
        payload = {'version': 1, 'commits': worker.mapping, 'source_refs': refs, 'rewritten_refs': new_refs,
                   'extra_private_paths': list(extra_private_paths)}
        with map_path.open('x') as stream:
            json.dump(payload, stream, indent=2, sort_keys=True)
            stream.write('\n')
        print(f'Rewritten {len(commits)} commits and {len(new_refs)} refs', flush=True)
        return payload
    finally:
        worker.reader.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source')
    parser.add_argument('destination')
    parser.add_argument('map_path')
    parser.add_argument('--private-path', action='append', default=[], help='Also redact a verified historical document alias')
    args = parser.parse_args()
    rewrite(args.source, args.destination, args.map_path, args.private_path)
