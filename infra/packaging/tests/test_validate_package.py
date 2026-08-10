from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


PACKAGING_ROOT = Path(__file__).resolve().parents[1]
VALIDATOR_PATH = PACKAGING_ROOT / "scripts" / "validate_package.py"


def load_validator():
    spec = importlib.util.spec_from_file_location(
        "bioseq_packaging_test_validate_package",
        VALIDATOR_PATH,
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load package validator: {VALIDATOR_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


validate_package = load_validator()


class RenderedPackageExactnessTests(unittest.TestCase):
    _CANONICAL_PATHS = (
        "usr/lib/postgresql/17/lib/bioseq.so",
        "usr/lib/postgresql/17/lib/bioseq_cache.so",
        "usr/share/postgresql/17/extension/bioseq--1.0.sql",
        "usr/share/postgresql/17/extension/bioseq_cache--1.0.sql",
        "usr/share/postgresql/17/extension/bioseq.control",
        "usr/share/postgresql/17/extension/bioseq_cache.control",
    )

    def _validate(self, listed_paths: list[str]) -> dict[str, object]:
        def extract_fixture(_path: Path, _fmt: str, destination: Path) -> None:
            for relative in listed_paths:
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(relative.encode("utf-8"))

        def binary_fixture(
            path: Path, _architecture: str
        ) -> validate_package.BinaryValidation:
            return validate_package.BinaryValidation(
                path=str(path),
                elf_architecture="Advanced Micro Devices X86-64",
                rpath_absent=True,
                compiler_runtime_safe=True,
            )

        with tempfile.TemporaryDirectory() as raw:
            artifact = Path(raw) / "bioseq-postgresql17_4.2.3-1_amd64.deb"
            artifact.write_bytes(b"test-package")
            with (
                mock.patch.object(
                    validate_package,
                    "package_paths",
                    return_value=listed_paths,
                ),
                mock.patch.object(
                    validate_package,
                    "package_metadata",
                    return_value=(
                        "bioseq-postgresql17",
                        "4.2.3-1",
                        "amd64",
                        "postgresql-17, libvulkan1",
                    ),
                ),
                mock.patch.object(
                    validate_package,
                    "extract_package",
                    side_effect=extract_fixture,
                ),
                mock.patch.object(
                    validate_package,
                    "validate_dso",
                    side_effect=binary_fixture,
                ),
            ):
                return validate_package.validate_artifact(
                    artifact,
                    "x64",
                    17,
                    "postgresql-17",
                    "libvulkan1",
                )

    def test_exact_six_artifact_package_is_accepted(self) -> None:
        validation = self._validate(list(self._CANONICAL_PATHS))

        self.assertTrue(validation["passed"])
        self.assertTrue(validation["six_artifacts"])

    def test_extra_payload_is_rejected(self) -> None:
        listed_paths = [
            *self._CANONICAL_PATHS,
            "usr/share/doc/bioseq/README.md",
        ]

        with self.assertRaises(validate_package.ValidationError):
            self._validate(listed_paths)

    def test_duplicate_required_basename_is_rejected(self) -> None:
        listed_paths = [
            *self._CANONICAL_PATHS,
            "opt/bioseq/lib/bioseq.so",
        ]

        with self.assertRaises(validate_package.ValidationError):
            self._validate(listed_paths)

    def test_misplaced_required_artifact_is_rejected(self) -> None:
        listed_paths = [
            *(
                path
                for path in self._CANONICAL_PATHS
                if not path.endswith("/bioseq--1.0.sql")
            ),
            "opt/bioseq/bioseq--1.0.sql",
        ]

        with self.assertRaises(validate_package.ValidationError):
            self._validate(listed_paths)


if __name__ == "__main__":
    unittest.main()
