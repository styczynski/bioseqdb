#!/usr/bin/env python3
"""Contracts for the guarded Bioseq IO module inventory."""

from __future__ import annotations

from pathlib import Path
import re
import unittest


class BioseqIoSourceInventoryContractTest(unittest.TestCase):
    """Keep IO production and benchmark module ownership exact."""

    benchmark_modules = (
        "benchmark/common.cppm",
        "benchmark/index.cppm",
        "benchmark/lazy_expression.cppm",
        "benchmark/main.cppm",
        "benchmark/readers.cppm",
        "benchmark/writers.cppm",
    )
    production_modules = tuple(sorted((
        "buffer.cppm",
        "compression/aligned_byte_buffer.cppm",
        "compression/bit_reader.cppm",
        "compression/block_finder.cppm",
        "compression/chunk.cppm",
        "compression/chunk_fetcher.cppm",
        "compression/crc32.cppm",
        "compression/deflate.cppm",
        "compression/dynamic_huffman_skip_table/high_windows.cppm",
        "compression/dynamic_huffman_skip_table/index.cppm",
        "compression/dynamic_huffman_skip_table/low_windows.cppm",
        "compression/gzip_format.cppm",
        "compression/huffman.cppm",
        "compression/parallel_gzip_decoder.cppm",
        "compression/parallel_gzip_source.cppm",
        "compression/precode_validation.cppm",
        "compression/window_map.cppm",
        "errors.cppm",
        "fasta.cppm",
        "fastq.cppm",
        "file_kind.cppm",
        "identifier_parser/formats.cppm",
        "identifier_parser/index.cppm",
        "options.cppm",
        "pipeline.cppm",
        "reader.cppm",
        "reader/batch_pipeline.cppm",
        "reader/stream_pipeline.cppm",
        "reader/types.cppm",
        "simd.cppm",
        "sinks.cppm",
        "source.cppm",
        "sources.cppm",
        "writer.cppm",
    )))

    @classmethod
    def setUpClass(cls) -> None:
        """Load the authored CMake and repository inventories once."""
        cls.repository_root = Path(__file__).resolve().parents[3]
        cls.bioseq_cmake = (
            cls.repository_root / "bioseq" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        cls.root_cmake = (
            cls.repository_root / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        cls.io_root = cls.repository_root / "bioseq" / "io"

    def _inventory_body(self) -> str:
        """Return the BIOSEQ_IO_MODULES helper arguments."""
        match = re.search(
            r"bioseq_collect_authored_sources\(\s*BIOSEQ_IO_MODULES"
            r"(?P<body>.*?)\)",
            self.bioseq_cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(match, "missing guarded BIOSEQ_IO_MODULES inventory")
        return match.group("body")

    @staticmethod
    def _cmake_tokens(body: str) -> tuple[str, ...]:
        """Tokenize the simple keyword/path helper invocation."""
        tokens = re.findall(r'"[^"\n]*"|[^\s]+', body)
        return tuple(
            token[1:-1] if token.startswith('"') else token
            for token in tokens
        )

    def test_io_inventory_uses_the_shared_fail_closed_helper(self) -> None:
        """Require the recursive source-only inventory and exact count."""
        self.assertRegex(
            self.bioseq_cmake,
            r'include\(\s*"\$\{CMAKE_CURRENT_LIST_DIR\}/\.\./cmake/'
            r'bioseq_source_inventory\.cmake"\s*\)',
        )
        body = self._inventory_body()
        self.assertEqual(
            self._cmake_tokens(body),
            (
                "BASE_DIRECTORY",
                "${CMAKE_CURRENT_LIST_DIR}/io",
                "RECURSE",
                "INCLUDE_PATTERNS",
                "*.cppm",
                "EXCLUDE_PATHS",
                *self.benchmark_modules,
                "EXPECTED_COUNT",
                "34",
            ),
        )
        for obsolete_contract in (
            "file(GLOB_RECURSE BIOSEQ_IO_MODULES",
            "list(REMOVE_ITEM BIOSEQ_IO_MODULES",
        ):
            with self.subTest(obsolete_contract=obsolete_contract):
                self.assertNotIn(obsolete_contract, self.bioseq_cmake)

    def test_benchmark_exclusions_are_complete_and_exact(self) -> None:
        """Keep all six benchmark modules outside the shared IO owner."""
        authored_benchmark_modules = tuple(
            sorted(
                path.relative_to(self.io_root).as_posix()
                for path in (self.io_root / "benchmark").glob("*.cppm")
                if path.is_file()
            )
        )
        self.assertEqual(authored_benchmark_modules, self.benchmark_modules)

    def test_guarded_result_matches_the_exact_production_tree(self) -> None:
        """Freeze the reviewed 34-path shared IO owner."""
        all_modules = tuple(sorted(
            path.relative_to(self.io_root).as_posix()
            for path in self.io_root.rglob("*.cppm")
            if path.is_file()
        ))
        expected_modules = tuple(sorted((
            *self.production_modules,
            *self.benchmark_modules,
        )))
        self.assertEqual(all_modules, expected_modules)
        production_modules = tuple(
            path for path in all_modules
            if path not in self.benchmark_modules
        )
        self.assertEqual(production_modules, self.production_modules)
        self.assertEqual(len(production_modules), 34)

    def test_downstream_owner_and_builder_registration_are_stable(self) -> None:
        """Keep the same consumer and run this contract through the builder."""
        self.assertEqual(
            self.bioseq_cmake.count(
                "list(APPEND BIOSEQ_NOPG_MODULES ${BIOSEQ_IO_MODULES})"
            ),
            1,
        )
        self.assertEqual(
            self.root_cmake.count(
                "cmake/tests/contract/test_bioseq_io_source_inventory.py"
            ),
            1,
        )


if __name__ == "__main__":
    unittest.main()
