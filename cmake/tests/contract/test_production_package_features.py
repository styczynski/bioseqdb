"""Keep the installed smoke producer and all release acceptance layers joined."""
from __future__ import annotations

import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
from types import SimpleNamespace
import unittest

# Packaging's supported Python range is newer than the build MCP's interpreter.
# Re-exec this isolated contract without changing the interpreter of other tests.
if not (3, 12) <= sys.version_info[:2] < (3, 14):
    for name in ('python3.13', 'python3.12'):
        candidate = shutil.which(name)
        if candidate is None:
            continue
        probe = subprocess.run(
            [candidate, '-c', 'import sys; sys.exit(not (3, 12) <= sys.version_info[:2] < (3, 14))'],
            capture_output=True, timeout=10)
        if probe.returncode == 0:
            os.execv(candidate, [candidate, str(Path(__file__).resolve()), *sys.argv[1:]])
    raise RuntimeError('Production packaging contracts require Python 3.12 or 3.13')

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'infra/packaging'))
from bioseq_packaging import acceptance, manifest, model

SPEC = importlib.util.spec_from_file_location('production_feature_package_builder', ROOT / 'infra/package_builder.py')
assert SPEC is not None and SPEC.loader is not None
controller = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = controller
SPEC.loader.exec_module(controller)
smoke_driver = runpy.run_path(str(ROOT / 'infra/packaging/scripts/smoke_test_package.py'))


class ProductionPackageFeatures(unittest.TestCase):
    def setUp(self) -> None:
        script = (ROOT / 'infra/packaging/scripts/smoke_installed_package.sh').read_text()
        producer = re.search(r"printf '(\{\"passed\":true,.*\})\\n'", script)
        self.assertIsNotNone(producer)
        self.raw = json.loads(producer.group(1) % ('debian_13', 'x64', 17))
        self.attested = dict(self.raw, fresh_container=True, container_image='debian:13')
        self.cell = SimpleNamespace(
            distro=SimpleNamespace(distro_id=model.DistroId.DEBIAN_13,
                                   image_for=lambda _: 'debian:13'),
            request=SimpleNamespace(architecture=model.Architecture.X64),
            postgres_major=17)

    def validate(self, layer: str, raw: dict, attested: dict) -> None:
        identity = dict(architecture='x64', postgres_major=17, distro='debian_13')
        if layer == 'container':
            smoke_driver['parse_result'](json.dumps(raw), image='debian:13', **identity)
        elif layer == 'worker':
            acceptance._parse_smoke(json.dumps(attested), cell=self.cell)
        elif layer == 'manifest':
            manifest._validate_smoke(attested, label='production feature fixture', **identity)
        elif layer == 'controller':
            controller.require_smoke_attestation(attested, label='production feature fixture', **identity)
        else:
            self.fail(layer)

    def test_actual_installed_smoke_passes_every_release_layer(self) -> None:
        for layer in ('container', 'worker', 'manifest', 'controller'):
            with self.subTest(layer=layer):
                self.validate(layer, copy.deepcopy(self.raw), copy.deepcopy(self.attested))

    def test_cache_and_api_must_be_explicitly_true_at_every_layer(self) -> None:
        for field in ('bioseq_cache_extension_created', 'external_api_enabled'):
            for value in ('missing', False, None, 1):
                raw, attested = copy.deepcopy(self.raw), copy.deepcopy(self.attested)
                if value == 'missing':
                    raw.pop(field)
                    attested.pop(field)
                else:
                    raw[field] = attested[field] = value
                for layer in ('container', 'worker', 'manifest', 'controller'):
                    with self.subTest(field=field, value=value, layer=layer):
                        with self.assertRaises((smoke_driver['SmokeError'], model.PackageBuildError,
                                                manifest.ManifestError, controller.PackageBuilderError)):
                            self.validate(layer, raw, attested)

    def test_controller_still_rejects_unknown_smoke_claims(self) -> None:
        self.attested['unrecognized_claim'] = True
        with self.assertRaisesRegex(controller.PackageBuilderError, 'unexpected keys'):
            self.validate('controller', self.raw, self.attested)


if __name__ == '__main__':
    unittest.main()
