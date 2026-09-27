#!/usr/bin/env python3
"""Regression tests for the standalone migration utility (no Bioseq binaries)."""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from rewrite_history import git, rewrite
from verify_history import verify
from cleanup_checkout import plan


class HistoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root/'source'
        subprocess.check_call(['git', 'init', '-q', '-b', 'main', str(self.source)])
        git(self.source, 'config', 'user.name', 'Original Åuthor')
        git(self.source, 'config', 'user.email', 'author@example.invalid')
        self.destination, self.map = self.root/'public.git', self.root/'map.json'

    def tearDown(self):
        self.temp.cleanup()

    def write(self, path, content):
        target = self.source/path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)

    def commit(self, message='Subject\n\nConfidential multiline body'):
        git(self.source, 'add', '-A')
        env = dict(os.environ, GIT_AUTHOR_DATE='2003-03-03T01:02:03-0700',
                   GIT_COMMITTER_DATE='2011-04-05T06:07:08+0530')
        subprocess.check_call(['git','-C',str(self.source),'commit','-q','--allow-empty','-m',message],env=env)
        return git(self.source, 'rev-parse', 'HEAD').decode().strip()

    def run_rewrite(self):
        with contextlib.redirect_stdout(io.StringIO()):
            result = rewrite(self.source, self.destination, self.map)
            verify(self.source, self.destination, self.map)
        return result['commits']

    def test_unchanged_changed_and_deleted_documents(self):
        self.write('README.md', b'secret one')
        self.write('src/code.cpp', b'int main(){}\n')
        x = self.commit()
        self.write('src/code.cpp', b'int main(){return 0;}\n')
        y = self.commit()
        self.write('README.md', b'secret two')
        z = self.commit()
        git(self.source, 'rm', '-q', 'README.md')
        self.commit()
        mapping = self.run_rewrite()
        for commit, expected in [(x,x),(y,x),(z,z)]:
            self.assertEqual(git(self.destination,'show',mapping[commit]+':README.md'),expected.encode()+b'\n')
        self.assertFalse((self.destination/'objects/info/alternates').exists())
        self.assertFalse(git(self.destination,'remote').strip())

    def test_path_rules_modes_and_weird_names(self):
        self.write('website/guide.MD', b'public tutorial')
        self.write('nested/website/a.md', b'public nested tutorial')
        self.write('websiteish/a.MD', b'private')
        self.write('a.MARKDOWN', b'private2')
        self.write('a.mdx', b'private3')
        self.write('space and\nnewline.md', b'private4')
        self.write('DOCS/no_extension', b'allowed non markdown')
        self.write('executable.md', b'private5')
        (self.source/'executable.md').chmod(0o755)
        (self.source/'link.md').symlink_to('a.MARKDOWN')
        x = self.commit()
        mapping = self.run_rewrite()
        self.assertEqual(git(self.destination,'show',mapping[x]+':website/guide.MD'),b'public tutorial')
        self.assertEqual(git(self.destination,'show',mapping[x]+':link.md'),x.encode()+b'\n')

    def test_merge_inherits_matching_parent_and_preserves_topology(self):
        self.write('a.md', b'base secret'); base = self.commit()
        git(self.source,'checkout','-q','-b','side')
        self.write('a.md', b'branch secret'); side = self.commit()
        git(self.source,'checkout','-q','main')
        self.write('src/code.c', b'code'); main = self.commit()
        git(self.source,'merge','--no-ff','-m','Merge\n\nprivate body','side')
        merge = git(self.source,'rev-parse','HEAD').decode().strip()
        mapping = self.run_rewrite()
        self.assertEqual(git(self.destination,'show',mapping[merge]+':a.md'),side.encode()+b'\n')
        self.assertEqual(git(self.destination,'rev-list','--parents','-n','1',mapping[merge]).decode().split()[1:],[mapping[main],mapping[side]])

    def test_rename_website_to_private_and_back(self):
        self.write('website/a.md',b'public original'); self.commit()
        git(self.source,'mv','website/a.md','private.md'); moved = self.commit()
        (self.source/'website').mkdir(exist_ok=True)
        git(self.source,'mv','private.md','website/a.md'); back = self.commit()
        mapping = self.run_rewrite()
        self.assertEqual(git(self.destination,'show',mapping[moved]+':private.md'),moved.encode()+b'\n')
        self.assertEqual(git(self.destination,'show',mapping[back]+':website/a.md'),b'public original')

    def test_shared_website_blob_and_tags(self):
        self.write('website/tutorial.md',b'shared content')
        self.write('DOCS/secret.md',b'shared content')
        original = self.commit()
        git(self.source,'tag','light')
        git(self.source,'tag','-a','annotated','-m','Release\n\nPrivate tag body')
        mapping = self.run_rewrite()
        self.assertEqual(git(self.destination,'rev-parse','annotated^{}').strip().decode(),mapping[original])
        self.assertEqual(git(self.destination,'cat-file','tag','annotated').split(b'\n\n',1)[1],b'Release\n')

    def test_refuse_source_or_existing_destination(self):
        self.write('a.md',b'secret'); self.commit()
        with self.assertRaises(ValueError): rewrite(self.source,self.source,self.map)
        with self.assertRaises(ValueError): rewrite(self.source,self.source/'child.git',self.map)
        self.assertFalse((self.source/'child.git').exists())

    def test_audit_rejects_resurrected_secret_and_extra_ref(self):
        self.write('a.md',b'private secret'); original = self.commit()
        mapping = self.run_rewrite()
        git(self.destination,'update-ref','refs/heads/unexpected',mapping[original])
        with self.assertRaises(AssertionError): verify(self.source,self.destination,self.map)
        git(self.destination,'update-ref','-d','refs/heads/unexpected')
        git(self.destination,'hash-object','-w','--stdin',data=b'private secret')
        with self.assertRaises(AssertionError): verify(self.source,self.destination,self.map)

    def test_reject_tags_to_raw_blobs(self):
        self.write('a.md',b'secret'); self.commit()
        blob=git(self.source,'rev-parse','HEAD:a.md').decode().strip()
        git(self.source,'update-ref','refs/tags/raw-document',blob)
        with self.assertRaises(subprocess.CalledProcessError): rewrite(self.source,self.destination,self.map)
        self.assertFalse(self.destination.exists())

    def test_cleanup_preserves_build_sources_and_moves_all_results(self):
        paths = ['CMakeLists.txt', 'CMakePresets.json', '.clang-format', '.clang-tidy',
                 'vcpkg.json', 'vcpkg-configuration.json', 'LICENSE', '.gitignore', '.gitattributes',
                 'build_mcp/server.py', 'bioseq/index.cppm', 'vendor/fmt/CMakeLists.txt',
                 'website/tutorial.md', 'README.md', '.mcp.json', '.mcp.jsonU', '.codex/config.toml',
                 'other-master/code.cpp', 'another-main/code.c', 'SEAL/seal-master/a.h',
                 'CHAINING_BRANCHING_EXPERIMENT_FOLLOWUP_ARTIFACTS/a.cpp',
                 'ALL_RESULTS.yaml', 'somewhere/ALL_RESULTS_OTHER.yaml', 'CHAINING_BRANCHING_TEST.yaml']
        result = plan(paths)
        self.assertEqual(len(result['removed']), 8)
        self.assertEqual(result['moved']['somewhere/ALL_RESULTS_OTHER.yaml'],
                         'research_benchmarks/paper/e2e/ALL_RESULTS_OTHER.yaml')
        self.assertEqual(result['moved']['CHAINING_BRANCHING_TEST.yaml'],
                         'research_benchmarks/paper/chaining/CHAINING_BRANCHING_TEST.yaml')
        self.assertTrue(all(x not in result['removed'] for x in paths[:13]))

    def test_cleanup_refuses_filename_collisions(self):
        with self.assertRaises(AssertionError):
            plan(['ALL_RESULTS.yaml', 'old/ALL_RESULTS.yaml'])
        with self.assertRaises(AssertionError):
            plan(['ALL_RESULTS.yaml', 'research_benchmarks/paper/e2e/ALL_RESULTS.yaml'])

    def test_merge_prefers_first_matching_file_parent(self):
        self.write('a.md', b'base'); root = self.commit()
        self.write('a.md', b'other'); self.commit()
        self.write('a.md', b'base'); first = self.commit()
        # The complete second-parent tree matches, but a.md also matches the
        # first parent and must retain that parent's more recent provenance.
        git(self.source,'checkout','-q','-b','second',root)
        self.write('code.c', b'other code'); second = self.commit()
        tree_id = git(self.source,'rev-parse',second+'^{tree}').strip().decode()
        merge = git(self.source,'commit-tree',tree_id,'-p',first,'-p',second,data=b'Merge\n').strip().decode()
        git(self.source,'update-ref','refs/heads/merge',merge)
        mapping = self.run_rewrite()
        self.assertEqual(git(self.destination,'show',mapping[merge]+':a.md'),first.encode()+b'\n')

    def test_document_alias_is_redacted_without_changing_other_text(self):
        self.write('old/prompt.txt',b'same private content')
        self.write('safe.txt',b'keep this non-markdown file')
        original=self.commit()
        git(self.source,'mv','old/prompt.txt','prompt.md');self.commit()
        with contextlib.redirect_stdout(io.StringIO()):
            mapping=rewrite(self.source,self.destination,self.map,['old/prompt.txt'])['commits']
            verify(self.source,self.destination,self.map)
        self.assertEqual(git(self.destination,'show',mapping[original]+':old/prompt.txt'),original.encode()+b'\n')
        self.assertEqual(git(self.destination,'show',mapping[original]+':safe.txt'),b'keep this non-markdown file')


if __name__ == '__main__':
    unittest.main(verbosity=2)
