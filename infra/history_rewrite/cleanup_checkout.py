#!/usr/bin/env python3
"""Plan and apply the requested cleanup to an already sanitized checkout only.

The public history must have been rewritten first; the private source checkout
is rejected by checking its HEAD against the rewrite map. Changes stay staged
for review and an ordinary new cleanup commit.
"""
import argparse
import fnmatch
import json
import os
from pathlib import Path
import shutil
import subprocess

REMOVE_DIRS = set('''_simd_minimizers .agents .claude .codex block-aligner-research
chartscii-cli-master cppgres-master cpptrace-main crc-fast-rust-main
easy_profiler-develop ftxui-main g3sa-main highway-master hwinfo-main
libassert-main memory-main minimap2-fast-contrib-v2.22 minimap2-master-clean
librapidarchive-main moltenvk-main onetbb-master paradedb-experiments
perf-cpp-dev pg_background-master pg-ast pgvector-master pg_extension-master
website_design_files_tmp CHAINING_BRANCHING_EXPERIMENT_FOLLOWUP_ARTIFACTS'''.split())
KEEP_FILES = {'vcpkg.json', 'vcpkg-configuration.json', 'LICENSE', '.gitignore', '.gitattributes'}


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), '-c', 'core.hooksPath=/dev/null', *args],
                                   env=dict(os.environ, GIT_OPTIONAL_LOCKS='0'))


def plan(paths):
    removed, moved = [], {}
    for path in paths:
        parts = path.split('/')
        top, name = parts[0], parts[-1]
        if fnmatch.fnmatchcase(name, 'ALL_RESULTS*.yaml'):
            target = 'research_benchmarks/paper/e2e/' + name
            if path != target:
                moved[path] = target
        elif fnmatch.fnmatchcase(name, 'CHAINING_BRANCHING*.yaml'):
            target = 'research_benchmarks/paper/chaining/' + name
            if path != target:
                moved[path] = target
        elif (len(parts) > 1 and (top in REMOVE_DIRS or top.endswith(('-main', '-master')))) or path.startswith('SEAL/seal-master/'):
            removed.append(path)
        elif len(parts) == 1 and not (top in KEEP_FILES or top.lower().startswith('cmake') or top.startswith('.clang')):
            removed.append(path)
    assert len(set(moved.values())) == len(moved), 'Result filenames collide'
    assert not set(moved.values()) & set(paths), 'Move destination already exists'
    return {'removed': sorted(removed), 'moved': dict(sorted(moved.items()))}


def cleanup(root, map_file, manifest, apply=False):
    if not __debug__:
        raise RuntimeError('Cleanup with disabled Python assertions is forbidden')
    root = Path(root).resolve()
    record = json.loads(Path(map_file).read_text())
    head = git(root, 'rev-parse', 'HEAD').decode().strip()
    assert head == record['rewritten_refs']['refs/heads/main'], 'Cleanup only accepts the rewritten main branch'
    assert not git(root, 'status', '--porcelain', '--untracked-files=no').strip(), 'Checkout has tracked changes'
    paths = [os.fsdecode(x) for x in git(root, 'ls-files', '-z').split(b'\0') if x]
    changes = dict(plan(paths), parent=head)
    Path(manifest).write_text(json.dumps(changes, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'removed_files':len(changes['removed']), 'moved_files':len(changes['moved']),
                      'removed_top_levels':sorted(set(x.split('/')[0] for x in changes['removed'])),
                      'moves':changes['moved']}, indent=2))
    if apply:
        for old, new in changes['moved'].items():
            (root/new).parent.mkdir(parents=True, exist_ok=True)
            git(root, 'mv', '--', old, new)
        for start in range(0, len(changes['removed']), 100):
            git(root, 'rm', '-q', '--', *changes['removed'][start:start+100])
        for directory, dirs, files in os.walk(root, topdown=True, followlinks=False):
            dirs[:] = [d for d in dirs if d != '.git']
            for name in list(dirs):
                if name.startswith('.bioseq-local'):
                    path = Path(directory)/name
                    relative = path.relative_to(root).as_posix()
                    assert not any(p == relative or p.startswith(relative+'/') for p in paths), 'Tracked local data would be deleted'
                    if path.is_symlink():
                        path.unlink()
                    else:
                        shutil.rmtree(path)
                    dirs.remove(name)
    return changes


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkout')
    parser.add_argument('map_file')
    parser.add_argument('manifest')
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    cleanup(args.checkout, args.map_file, args.manifest, args.apply)
