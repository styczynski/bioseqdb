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
