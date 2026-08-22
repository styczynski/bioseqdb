#!/usr/bin/env python3
"""Static contracts for guarded external authored-source inventories."""

from __future__ import annotations

from pathlib import Path
import re
import unittest


class ExternalSourceInventoryContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.repository_root = Path(__file__).resolve().parents[3]
        cls.external_root = cls.repository_root / "bioseq" / "external"
        cls.cmake_source = (
            cls.external_root / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        cls.helper_source = (
            cls.repository_root / "cmake" / "bioseq_source_inventory.cmake"
        ).read_text(encoding="utf-8")

    def _inventory_call(self, variable: str) -> str:
        match = re.search(
            rf"bioseq_collect_authored_sources\(\s*{re.escape(variable)}"
            r"(?P<body>.*?)\)",
            self.cmake_source,
            re.DOTALL,
        )
        self.assertIsNotNone(match, f"missing guarded inventory for {variable}")
        return match.group("body")

    def test_external_cmake_uses_only_three_guarded_inventories(self) -> None:
        self.assertIn(
            'include("${BIOSEQ_REPOSITORY_ROOT}/cmake/'
            'bioseq_source_inventory.cmake")',
            self.cmake_source,
        )
        self.assertEqual(
            self.cmake_source.count("bioseq_collect_authored_sources("),
            3,
        )

        cases = (
            (
                "_bioseq_provider_core_modules",
                '${CMAKE_CURRENT_SOURCE_DIR}/provider',
                "*.cppm",
                14,
                False,
            ),
            (
                "_bioseq_provider_adapter_modules",
                '${CMAKE_CURRENT_SOURCE_DIR}/providers',
                "*.cppm",
                6,
                False,
            ),
            (
                "_bioseq_external_integration_test_sources",
                '${CMAKE_CURRENT_SOURCE_DIR}/tests',
                "*_test.cpp",
                11,
                True,
            ),
        )
        for variable, directory, pattern, count, recurse in cases:
            with self.subTest(variable=variable):
                body = self._inventory_call(variable)
                self.assertIn(f'BASE_DIRECTORY "{directory}"', body)
                self.assertIn(f'INCLUDE_PATTERNS "{pattern}"', body)
                self.assertRegex(body, rf"EXPECTED_COUNT\s+{count}(?:\s|$)")
                self.assertEqual("RECURSE" in body, recurse)

        self.assertNotRegex(
            self.cmake_source,
            r"file\(GLOB\s+_bioseq_provider_(?:core|adapter)_modules",
        )

    def test_guarded_inventory_counts_match_authored_tree(self) -> None:
        cases = (
            (self.external_root / "provider", "*.cppm", False, 14),
            (self.external_root / "providers", "*.cppm", False, 6),
            (self.external_root / "tests", "*_test.cpp", True, 11),
        )
        for directory, pattern, recurse, expected in cases:
            with self.subTest(directory=directory.relative_to(self.external_root)):
                iterator = (
                    directory.rglob(pattern)
                    if recurse else directory.glob(pattern)
                )
                sources = sorted(path for path in iterator if path.is_file())
                self.assertEqual(len(sources), expected)

    def test_integration_test_inventory_has_one_clear_owner_name(self) -> None:
        self.assertNotIn("_bioseq_provider_test_sources", self.cmake_source)
        self.assertEqual(
            self.cmake_source.count(
                "_bioseq_external_integration_test_sources"),
            2,
        )
        self.assertRegex(
            self.cmake_source,
            r"target_sources\(bioseq_external_api_integration_tests PRIVATE\s*"
            r"\$\{_bioseq_external_integration_test_sources\}\)",
        )

    def test_annotation_discovery_remains_independent_and_recursive(self) -> None:
        self.assertIn(
            "file(GLOB_RECURSE _bioseq_external_scan_inputs "
            "CONFIGURE_DEPENDS\n"
            '  "${_bioseq_external_source_root}/*.cppm")',
            self.cmake_source,
        )
        self.assertIn(
            "${_bioseq_external_scan_inputs}",
            self.cmake_source,
        )

    def test_shared_helper_remains_fail_closed(self) -> None:
        for contract in (
            "CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE",
            "list(REMOVE_DUPLICATES _bioseq_inventory_sources)",
            "list(SORT _bioseq_inventory_sources)",
            "exclusion did not match an included",
            "EXPECTED_COUNT",
        ):
            with self.subTest(contract=contract):
                self.assertIn(contract, self.helper_source)


if __name__ == "__main__":
    unittest.main()
