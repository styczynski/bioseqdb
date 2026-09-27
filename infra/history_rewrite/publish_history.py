#!/usr/bin/env python3
"""Publish audited refs only to the empty repository whose numeric ID is pinned.

Use only after verify_history.py has passed. The clean Bioseq MCP build must
also pass unless the user explicitly permits publishing while it is running.
An explicit manifest enumerates refs; push --mirror and force-push are forbidden.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import uuid

DESTINATION = 'https://github.com/styczynski/bioseqdb.git'
DESTINATION_ID = 1390666747


def run(args, **kwargs):
    return subprocess.check_output(args, **kwargs)


def require_ready(record, allow_running_build=False):
    if not record.get('final_audit_passed'):
        raise RuntimeError('Final history audit must pass')
    attempt = record.get('clean_build_attempt', {})
    pending_allowed = (allow_running_build
                       and record.get('publish_while_build_running_authorized') is True
                       and attempt.get('status') == 'running'
                       and str(attempt.get('receipt', '')).startswith('br_'))
    if not record.get('clean_build_passed') and not pending_allowed:
        raise RuntimeError('Clean MCP build must pass or publication while running must be explicitly authorized')


def verify_destination_url(prefix):
    # Some Git versions omit remotes supplied only by -c from remote get-url.
    # Register a temporary remote in this isolated checkout, then remove it.
    name = 'history-publication-' + uuid.uuid4().hex
    run(prefix + ['remote', 'add', name, DESTINATION])
    try:
        urls = run(prefix + ['remote', 'get-url', '--push', '--all', name]).decode().splitlines()
        if urls != [DESTINATION]:
            raise RuntimeError('Git configuration redirects the publication destination')
    finally:
        run(prefix + ['remote', 'remove', name])


def publish(repository, manifest, allow_running_build=False):
    repository = str(Path(repository).resolve())
    record = json.loads(Path(manifest).read_text())
    refs = record['publication_refs']
    require_ready(record, allow_running_build)
    metadata = json.loads(run(['gh', 'api', 'repos/styczynski/bioseqdb']))
    if metadata['id'] != DESTINATION_ID or metadata['full_name'] != 'styczynski/bioseqdb' or metadata['fork']:
        raise RuntimeError('Destination identity mismatch')
    if run(['git', 'ls-remote', DESTINATION]).strip():
        raise RuntimeError('Destination must still be empty')
    current = dict(line.split(' ',1) for line in run(['git','-C',repository,'for-each-ref','--format=%(refname) %(objectname)']).decode().splitlines())
    if current != refs:
        raise RuntimeError('Repository refs changed since audit')
    if any(not ref.startswith(('refs/heads/', 'refs/tags/')) for ref in refs):
        raise RuntimeError('Only explicit branches and tags may be published')
    if run(['git', '-C', repository, 'status', '--porcelain', '--untracked-files=no']).strip():
        raise RuntimeError('Tracked checkout changes are not committed')
    # URL is a constant, not a caller-supplied remote name or a redirect alias.
    prefix = ['git','-C',repository,'-c','http.followRedirects=false']
    verify_destination_url(prefix)
    actions = json.loads(run(['gh','api','repos/styczynski/bioseqdb/actions/permissions']))
    try:
        if actions['enabled']:
            run(['gh','api','--method','PUT','repos/styczynski/bioseqdb/actions/permissions','-F','enabled=false'])
        subprocess.check_call(prefix + ['-c','lfs.url='+DESTINATION+'/info/lfs',
                                       '-c','lfs.pushurl='+DESTINATION+'/info/lfs',
                                       'lfs','push','--all',DESTINATION])
        # Do not run checkout-provided hooks during this security migration.
        # LFS was explicitly uploaded and verified in the preceding command.
        subprocess.check_call(prefix + ['-c','core.hooksPath=/dev/null',
                                       'push','--atomic','--no-follow-tags',DESTINATION,
                                       *[f'{oid}:{ref}' for ref,oid in sorted(refs.items())]])
    finally:
        if actions['enabled']:
            run(['gh','api','--method','PUT','repos/styczynski/bioseqdb/actions/permissions','-F','enabled=true'])
    actual = dict((ref,oid) for oid,ref in (line.split('\t') for line in run(['git','ls-remote','--refs',DESTINATION]).decode().splitlines()) if not ref.endswith('^{}'))
    if actual != refs:
        raise RuntimeError('Published refs do not match the audited manifest')
    print(f'Published and verified {len(refs)} refs to {DESTINATION}')


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('repository');p.add_argument('manifest')
    p.add_argument('--allow-running-build', action='store_true')
    a=p.parse_args();publish(a.repository,a.manifest,a.allow_running_build)
