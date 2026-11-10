#!/usr/bin/env python3
"""Contracts for the guarded Bioseq IO module inventory."""

from __future__ import annotations

from pathlib import Path
import hashlib
import importlib.util
import json
import re
import tempfile
import unittest


class BioseqIoSourceInventoryContractTest(unittest.TestCase):
    """Keep IO production and benchmark module ownership exact."""

    benchmark_modules = (
        "benchmark/alignment.cppm",
        "benchmark/common.cppm",
        "benchmark/index.cppm",
        "benchmark/lazy_expression.cppm",
        "benchmark/main.cppm",
        "benchmark/readers.cppm",
        "benchmark/writers.cppm",
    )
    production_modules = tuple(sorted((
        "alignment/bgzf.cppm",
        "alignment/index.cppm",
        "alignment/record_codec.cppm",
        "alignment/sequence_codec.cppm",
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
        "identifier_parser/canonical.cppm",
        "identifier_parser/formats.cppm",
        "identifier_parser/index.cppm",
        "identifier_parser/preservation.cppm",
        "options.cppm",
        "pipeline.cppm",
        "reader.cppm",
        "reader/alignment_read.cppm",
        "reader/batch_pipeline.cppm",
        "reader/stream_pipeline.cppm",
        "reader/types.cppm",
        "record_schema.cppm",
        "simd.cppm",
        "sinks.cppm",
        "source.cppm",
        "sources.cppm",
        "write_pipeline.cppm",
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
                "43",
            ),
        )
        for obsolete_contract in (
            "file(GLOB_RECURSE BIOSEQ_IO_MODULES",
            "list(REMOVE_ITEM BIOSEQ_IO_MODULES",
        ):
            with self.subTest(obsolete_contract=obsolete_contract):
                self.assertNotIn(obsolete_contract, self.bioseq_cmake)

    def test_benchmark_exclusions_are_complete_and_exact(self) -> None:
        """Keep all seven benchmark modules outside the shared IO owner."""
        authored_benchmark_modules = tuple(
            sorted(
                path.relative_to(self.io_root).as_posix()
                for path in (self.io_root / "benchmark").glob("*.cppm")
                if path.is_file()
            )
        )
        self.assertEqual(authored_benchmark_modules, self.benchmark_modules)

    def test_guarded_result_matches_the_exact_production_tree(self) -> None:
        """Freeze the reviewed 43-path shared IO owner."""
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
        self.assertEqual(len(production_modules), 43)


    def test_alignment_qualification_keeps_old_samples_and_exact_matched_panels(self) -> None:
        config = json.loads((self.io_root / "benchmark/configuration.json").read_text())
        block = config["bench_io_alignment"]
        self.assertEqual(block["iteration_order"], {
            "mode": "interleaved_seeded", "seed": 20261109})
        self.assertEqual(block["matrix"]["for"]["ALIGNMENT_RECORD_IO"], 7)
        self.assertEqual(config["test_cases"]["ALIGNMENT_RECORD_IO"]["data"],
                         {"rows": 4096, "length": 150})
        panels = {
            "ALIGNMENT_QUAL_SHORT": (4096, 150, 6),
            "ALIGNMENT_QUAL_SMALL_UNCOMPRESSED": (256, 150, 0),
            "ALIGNMENT_QUAL_LONG_HIGH_COMPRESSION": (128, 8192, 9),
        }
        expected = set()
        for name, (rows, length, level) in panels.items():
            self.assertEqual(block["matrix"]["for"][name], 9)
            self.assertEqual(config["test_cases"][name]["data"], {
                "rows": rows, "length": length, "compression_level": level, "warm_inputs": 1})
            for alphabet in ("dna4", "dna5", "dna15", "aa10li", "aa10murphy", "aa20", "aa27"):
                for fmt in ("sam", "bam"):
                    for operation in ("read", "write", "native_encode", "read_cold"):
                        for workers in (1, 4):
                            expected.add((name, alphabet, fmt, operation, workers))
        observed = set()
        rules = block["matrix"]["metrics"]["wall_clock_ns"]["rules"]
        self.assertEqual(len(rules), len(expected))
        for _, lhs, comparator, rhs in rules:
            self.assertEqual(lhs["key"], "bioseq_alignment")
            self.assertEqual(rhs["key"], "samtools_alignment")
            self.assertEqual({key: value for key, value in lhs.items() if key != "key"},
                             {key: value for key, value in rhs.items() if key != "key"})
            observed.add(tuple(lhs[key] for key in ("test_case", "alphabet", "format", "operation", "workers")))
            self.assertEqual(comparator, {
                "compare": "<", "tolerance%": 0, "aggregate": "p50",
                "pairing": "by_iteration_ratio", "paired_reduction": "ratio_of_medians",
                "warmup_repetitions": 2, "blocking": True})
        self.assertEqual(observed, expected)
        for name in ("record_count", "full_record_match"):
            comparator = block["matrix"]["metrics"][name]["rules"][0][2]
            self.assertEqual(comparator["pairing"], "by_iteration_exact")
            self.assertEqual(comparator["tolerance%"], 0)

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



class SamtoolsSnapshotContractTest(unittest.TestCase):
    """Exercise actual snapshot verification using tiny native-source fixtures."""

    @classmethod
    def setUpClass(cls) -> None:
        path = Path(__file__).resolve().parents[2] / "bioseq_samtools_snapshot.py"
        spec = importlib.util.spec_from_file_location("bioseq_samtools_snapshot", path)
        cls.snapshot = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.snapshot)

    def write_source(self, source: Path, contents: str = "first") -> None:
        (source / "htslib-1.25/htslib").mkdir(parents=True, exist_ok=True)
        (source / "sam.c").write_text(contents)
        (source / "htslib-1.25/htslib/sam.h").write_text("header")
        paths = ["sam.c", "htslib-1.25/htslib/sam.h"]
        files = {name: hashlib.sha256((source / name).read_bytes()).hexdigest()
                 for name in paths}
        (source / "source_manifest.json").write_text(json.dumps({"sha256_files": files}))
        (source / "source_lock.json").write_text(json.dumps({
            "schema_version": 1, "file_manifest": "source_manifest.json",
            "repository_added_files": ["source_lock.json", "source_manifest.json"],
            "modifications_to_upstream_files": [], "upstream_file_count": len(files),
            "upstream_content_bytes": sum((source / name).stat().st_size for name in paths),
        }))

    def test_source_and_configuration_changes_select_fresh_work_trees(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source, output = Path(temporary) / "source", Path(temporary) / "output"
            self.write_source(source)
            first = self.snapshot.prepare(source, output, {"flags": "-O3"})
            header = Path(first["compiled_source"]) / "htslib-1.25/htslib/sam.h"
            self.assertEqual(header.read_text(), "header")
            before = header.stat().st_mtime_ns
            self.assertEqual(first, self.snapshot.prepare(source, output, {"flags": "-O3"}))
            self.assertEqual(header.stat().st_mtime_ns, before)
            # A configured work tree may contain generated build products.
            (header.parents[2] / "config.h").write_text("generated")
            self.assertEqual(first, self.snapshot.prepare(source, output, {"flags": "-O3"}))
            configured = self.snapshot.prepare(source, output, {"flags": "-O2"})
            self.assertNotEqual(first["build_sha256"], configured["build_sha256"])
            self.assertEqual(first["source_snapshot"], configured["source_snapshot"])
            # Returning A -> B -> A reselects the original verified work tree.
            self.assertEqual(first, self.snapshot.prepare(source, output, {"flags": "-O3"},
                             expected_build_id=first["build_sha256"]))
            selected_before = (output / "runtime_identity.json").read_bytes()
            with self.assertRaisesRegex(ValueError, "reconfigure"):
                self.snapshot.prepare(source, output, {"flags": "-O2"},
                                      expected_build_id=first["build_sha256"])
            self.assertEqual((output / "runtime_identity.json").read_bytes(), selected_before)
            self.write_source(source, "second")
            second = self.snapshot.prepare(source, output, {"flags": "-O3"})
            self.assertNotEqual(first["compiled_source"], second["compiled_source"])
            self.assertEqual((Path(first["compiled_source"]) / "sam.c").read_text(), "first")
            self.assertEqual((Path(second["compiled_source"]) / "sam.c").read_text(), "second")

    def test_bad_pin_or_source_inventory_fails_before_staging(self) -> None:
        for mutation in ("source", "header", "extra", "symlink", "traversal"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                source, output = Path(temporary) / "source", Path(temporary) / "output"
                self.write_source(source)
                if mutation == "source":
                    (source / "sam.c").write_text("changed without pin")
                elif mutation == "header":
                    (source / "htslib-1.25/htslib/sam.h").write_text("changed")
                elif mutation == "extra":
                    (source / "unmanifested.c").write_text("untracked input")
                elif mutation == "symlink":
                    (source / "sam.c").unlink()
                    (source / "sam.c").symlink_to(source / "htslib-1.25/htslib/sam.h")
                else:
                    (source / "source_manifest.json").write_text(json.dumps({
                        "sha256_files": {"../outside": "0" * 64, "sam.c": "0" * 64}}))
                with self.assertRaises(ValueError):
                    self.snapshot.prepare(source, output, {"flags": "-O3"})
                self.assertFalse((output / "runtime_identity.json").exists())

    def test_existing_snapshot_or_compiled_header_is_never_silently_repaired(self) -> None:
        for tree in ("compiled_source", "source_snapshot"):
            with self.subTest(tree=tree), tempfile.TemporaryDirectory() as temporary:
                source, output = Path(temporary) / "source", Path(temporary) / "output"
                self.write_source(source)
                identity = self.snapshot.prepare(source, output, {"flags": "-O3"})
                (Path(identity[tree]) / "htslib-1.25/htslib/sam.h").write_text("stale header")
                with self.assertRaises(ValueError):
                    self.snapshot.prepare(source, output, {"flags": "-O3"})

    def test_duplicate_manifest_keys_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            self.write_source(source)
            (source / "source_manifest.json").write_text(
                '{"sha256_files":{},"sha256_files":{}}')
            with self.assertRaisesRegex(ValueError, "duplicate"):
                self.snapshot.source_identity(source)

    def test_missing_work_tree_with_surviving_external_stamps_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source, output = Path(temporary) / "source", Path(temporary) / "output"
            self.write_source(source)
            identity = self.snapshot.prepare(source, output, {"flags": "-O3"})
            work = Path(identity["compiled_source"])
            (work.parent / "external").mkdir()
            work.rename(work.with_name("removed-work"))
            with self.assertRaisesRegex(ValueError, "surviving ExternalProject"):
                self.snapshot.prepare(source, output, {"flags": "-O3"})
            self.assertFalse(work.exists())

if __name__ == "__main__":
    unittest.main()
