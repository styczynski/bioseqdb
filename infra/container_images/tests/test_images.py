"""Focused renderer contracts; execution belongs to the container MCP worker."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
import build_container_images as images


class ImageContracts(unittest.TestCase):
    def test_diagnosed_repair_stays_pending_until_qualified_replacement(self):
        cell=dict(postgres_major=17,distro='ubuntu_26.04',architecture='arm64')
        smoke={k:True for k in ('passed','default_insert_search','default_persisted_search',
                               'column_defaults_and_vacuum')}
        package=dict(cell,sha256='old',validation={'passed':True},installed_sql_smoke=smoke)
        inventory={'packages':[package],'missing_cells':[]}
        key=images.fleet.cell_key(cell)
        state={'package_rebuilds':{'owner':[cell]},'jobs':{},'history':[]}
        self.assertEqual(images.fleet.pending_package_repairs(state,inventory),{key})
        self.assertEqual(images.fleet.package_readiness_counts(state,inventory)['release_ready_packages'],0)
        state['package_rebuilds']['owner'].clear()
        for status in ('running','failed','completed'):
            state['jobs']['owner']=dict(cell=cell,replaces_package_sha256='old',observed={'status':status})
            self.assertEqual(images.fleet.pending_package_repairs(state,inventory),{key})
        state['jobs']['owner']['observed']['status']='failed'
        self.assertEqual(len(images.fleet.pending_package_failures(state,inventory)),1)
        state['history'].append({'previous_job':state['jobs'].pop('owner')})
        self.assertEqual(images.fleet.pending_package_repairs(state,inventory),{key})
        inventory['packages'][0]=dict(package,sha256='new',installed_sql_smoke={})
        self.assertEqual(images.fleet.pending_package_repairs(state,inventory),{key})
        inventory['packages'][0]=dict(package,sha256='new')
        self.assertEqual(images.fleet.pending_package_repairs(state,inventory),set())
        self.assertEqual(images.fleet.package_readiness_counts(state,inventory)['release_ready_packages'],1)

    def test_matrix_has_matching_bases_versions_functions_and_distinct_tags(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            archive=root/'example.deb'
            archive.write_bytes(b'package fixture')
            item=dict(path=str(archive),bytes=archive.stat().st_size,sha256=images.digest(archive))
            tags=set()
            for distro in images.DISTROS:
                for major in (17,18,19,20):
                    for arch in ('arm64','x64'):
                        with self.subTest(distro=distro,major=major,architecture=arch):
                            package=dict(distro=distro,postgres_major=major,architecture=arch,sha256=item['sha256'],
                                validation=dict(binaries=[dict(path=f'/tmp/inspect/usr/lib/postgresql/{major}/lib/bioseq.so')]))
                            context=root/f'{distro}-{major}-{arch}'
                            values=images.render_context(dict(package=package,files=[item]),context)
                            self.assertEqual(values['extension_libdir'],f'/usr/lib/postgresql/{major}/lib')
                            self.assertIn('bioseq.cigar(seq)',(context/'smoke.sql').read_text())
                            self.assertIn('EXPLAIN (ANALYZE',(context/'smoke.sql').read_text())
                            self.assertIn('bioseq.target_start(seq)',(context/'smoke.sql').read_text())
                            self.assertNotIn('POSTGRES_HOST_AUTH_METHOD',(context/'Dockerfile').read_text())
                            self.assertNotIn('26.04-slim',values['base_image'])
                            if distro=='debian_13':self.assertEqual(values['base_image'],'debian:trixie-slim')
                            tag=images.image_tag(package)
                            self.assertNotIn(tag,tags);tags.add(tag)
            self.assertEqual(len(tags),24)

    def test_corrupt_package_is_rejected_before_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);archive=root/'example.deb';archive.write_bytes(b'bad')
            request=dict(package=dict(distro='ubuntu_26.04',postgres_major=17,architecture='x64'),
                         files=[dict(path=str(archive),bytes=3,sha256=hashlib.sha256(b'old').hexdigest())])
            with self.assertRaisesRegex(ValueError,'SHA-256'):
                images.render_context(request,root/'context')

    def test_worker_requires_mcp_admission(self):
        from unittest.mock import patch
        with patch.dict(images.os.environ,{},clear=True):
            with self.assertRaisesRegex(RuntimeError,'bioseq-build'):
                images.worker(Path('/not-read'))
