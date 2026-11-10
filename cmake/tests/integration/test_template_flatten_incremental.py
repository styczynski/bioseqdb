#!/usr/bin/env python3
"""Exercise Ninja's regeneration edge and enabled-width projection without C++."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[3]


class TemplateFlattenIncrementalTest(unittest.TestCase):
    def test_topology_and_profile(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bioseq-topology-") as temporary:
            root = Path(temporary)
            source = root / "input.cppm"
            source.write_text('''export module fixture.input;
namespace fixture {
using alphabet [[bioseq::template_flatten(
    "identity", "alphabet", "dna4", "::seq::dna4", "dna5", "::seq::dna5")]] = int;
static constexpr unsigned width [[bioseq::template_flatten(
    "identity", "width", "w1", "1", "w2", "2", "w4", "4", "w8", "8")]] = 8;
int value() { return 1; }
}
''')
            (root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 4.2)
project(topology NONE)
include("@REPOSITORY@/cmake/bioseq_build_config_codegen.cmake")
include("@REPOSITORY@/cmake/bioseq_template_flatten_codegen.cmake")
file(APPEND "${CMAKE_BINARY_DIR}/configure-count.txt" "configure\n")
if(NOT PROFILE)
  set(PROFILE all_alphabets_uint64)
endif()
bioseq_template_flatten_probe(
  NAME fixture SOURCE "${CMAKE_SOURCE_DIR}/input.cppm"
  OUTPUT_ROOT "${CMAKE_BINARY_DIR}/generated/fixture"
  OUTPUT_MODULE fixture.output PROFILE "${PROFILE}"
  OUT_CONFIGURE_TOPOLOGY_MANIFEST manifest OUT_TOPOLOGY_DIGEST digest
  OUT_ENABLED_ORDINALS ordinals OUT_ENABLED_SOURCES sources)
bioseq_template_flatten_read_group(ENABLED_ONLY
  TOPOLOGY_MANIFEST "${manifest}" ROLE identity GROUP width
  OUT_LABELS labels OUT_REPLACEMENTS replacements OUT_ORDINALS case_ordinals)
file(WRITE "${CMAKE_BINARY_DIR}/widths.txt" "${labels}|${replacements}|${case_ordinals}")
add_custom_target(fixture ALL DEPENDS "${manifest}")
'''.replace("@REPOSITORY@", REPOSITORY.as_posix()))
            build = root / "build"

            def run(*arguments: str) -> str:
                result = subprocess.run(arguments, text=True, capture_output=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout + result.stderr

            cmake = shutil.which("cmake")
            ninja = shutil.which("ninja")
            self.assertIsNotNone(cmake)
            self.assertIsNotNone(ninja)
            run(cmake, "-S", str(root), "-B", str(build), "-G", "Ninja")
            run(ninja, "-C", str(build))
            count = build / "configure-count.txt"
            inventory = build / "generated/fixture/topology.configure.json"
            before = inventory.read_bytes(), inventory.stat().st_mtime_ns
            self.assertEqual(count.read_text().splitlines(), ["configure"])
            self.assertEqual((build / "widths.txt").read_text(), "w8|8|3")
            self.assertNotIn("source_sha256", json.loads(inventory.read_text()))

            # Body changes and timestamp-only edits must not run CMake again.
            source.write_text(source.read_text().replace("return 1;", "return 2;"))
            run(ninja, "-C", str(build))
            self.assertEqual(before, (inventory.read_bytes(), inventory.stat().st_mtime_ns))
            source.touch()
            run(ninja, "-C", str(build))
            self.assertEqual(count.read_text().splitlines(), ["configure"])
            self.assertIn("no work to do", run(ninja, "-C", str(build)))

            # Coordinate changes must regenerate before consuming the new graph.
            source.write_text(source.read_text().replace('"w4", "4", ', ''))
            run(ninja, "-C", str(build))
            self.assertEqual(len(count.read_text().splitlines()), 2)
            self.assertEqual((build / "widths.txt").read_text(), "w8|8|2")
            run(cmake, "-S", str(root), "-B", str(build), "-DPROFILE=all_coordinates")
            run(ninja, "-C", str(build))
            self.assertEqual((build / "widths.txt").read_text(), "w1;w2;w8|1;2;8|0;1;2")
            source.write_text(source.read_text().replace('"w8", "8"', '"w4", "4", "w8", "8"'))
            run(ninja, "-C", str(build))
            self.assertEqual((build / "widths.txt").read_text(), "w1;w2;w4;w8|1;2;4;8|0;1;2;3")

            # Module identity is topology even when the coordinate axes agree.
            previous_count = len(count.read_text().splitlines())
            source.write_text(source.read_text().replace("fixture.input;", "fixture.renamed;"))
            run(ninja, "-C", str(build))
            self.assertEqual(len(count.read_text().splitlines()), previous_count + 1)
            self.assertEqual(json.loads(inventory.read_text())["source_module"], "fixture.renamed")

            # A deleted declared byproduct must be recreated, even after a no-op.
            inventory.unlink()
            run(ninja, "-C", str(build))
            self.assertTrue(inventory.is_file())

    def test_registry_body_and_identity_changes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bioseq-registry-") as temporary:
            root = Path(temporary)
            (root / "cmake").mkdir()
            for filename in ("bioseq_build_config_codegen.cmake", "scan_biological_alphabet_registry.cmake"):
                shutil.copyfile(REPOSITORY / "cmake" / filename, root / "cmake" / filename)
            registry = root / "bioseq/backend/sequences/packed_sequence_parts/core.cppm"
            registry.parent.mkdir(parents=True)
            registry.write_text('using supported_biological_alphabets = supported_alphabets<'
                                'alphabet_entry<seq::dna4, "dna4">>;\nint body() { return 1; }\n')
            (root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 4.2)
project(registry NONE)
include(cmake/bioseq_build_config_codegen.cmake)
_bioseq_biological_alphabet_entries(entries)
file(APPEND "${CMAKE_BINARY_DIR}/configure-count.txt" "configure\n")
''')
            build = root / "build"

            def run(*arguments: str) -> str:
                result = subprocess.run(arguments, text=True, capture_output=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout + result.stderr

            run("cmake", "-S", str(root), "-B", str(build), "-G", "Ninja")
            run("ninja", "-C", str(build))
            inventory = build / "generated/biological-alphabets.txt"
            before = inventory.read_bytes(), inventory.stat().st_mtime_ns
            registry.write_text(registry.read_text().replace("return 1;", "return 2;"))
            run("ninja", "-C", str(build))
            self.assertEqual(before, (inventory.read_bytes(), inventory.stat().st_mtime_ns))
            count = build / "configure-count.txt"
            self.assertEqual(len(count.read_text().splitlines()), 1)
            registry.write_text(registry.read_text().replace("dna4", "dna5"))
            run("ninja", "-C", str(build))
            self.assertEqual(len(count.read_text().splitlines()), 2)
            self.assertEqual(inventory.read_text(), "dna5;::seq::dna5\n")
            self.assertIn("no work to do", run("ninja", "-C", str(build)))


if __name__ == "__main__":
    unittest.main()
