#!/usr/bin/env python3
"""Contract tests for content-driven website benchmark bundle generation."""

from __future__ import annotations

import copy
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
import types
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock
from urllib.parse import urljoin, urlsplit


WEBSITE_ROOT = Path(__file__).resolve().parents[2]
GENERATOR = WEBSITE_ROOT / "tools" / "generate_benchmark_bundle.py"
REPORT_LITERAL = re.compile(r"report_[0-9]+\.json")


def _aggregate(value: int) -> dict[str, object]:
    return {
        "samples_count": 1,
        "stats": {
            "min": value,
            "max": value,
            "mean": value,
            "mean90": value,
            "sum": value,
            "stddev": 0,
        },
        "percentiles": {
            "P01": value,
            "P20": value,
            "P50": value,
            "P80": value,
            "P99": value,
        },
        "k-best": {"top10": [value], "bottom10": [value]},
        "histogram": [
            {"from": value, "to": value, "count": 1 if index == 0 else 0}
            for index in range(10)
        ],
    }


def _configuration(benchmark: str = "bench_alpha") -> dict[str, object]:
    return {
        "params": {
            "algo": {
                "_": {"default": "baseline"},
                "mode": {"default": "x"},
            }
        },
        "test_cases": {"tiny": {"description": "Tiny public case"}},
        "regression_rules": [],
        benchmark: {
            "matrix": {
                "over": {"algo": {"mode": ["x", "y"]}},
                "for": {"tiny": 1},
                "metrics": {"wall_clock_ns": {}},
                "by_regions": False,
            }
        },
    }


def _report(timestamp: int, *, fast: bool = False) -> dict[str, object]:
    created = datetime.fromtimestamp(timestamp, timezone.utc).isoformat().replace(
        "+00:00", "Z"
    )
    tuples = []
    for index, mode in enumerate(("x", "y"), start=1):
        tuples.append(
            {
                "tuple": {
                    "algo": {"_": "baseline", "mode": mode},
                },
                "test_cases": {
                    "tiny": {
                        "all": {"wall_clock_ns": _aggregate(index)},
                    }
                },
            }
        )
    return {
        "meta": {
            "commit": "0123456789abcdef0123456789abcdef01234567",
            "dirty": False,
            "timestamp": timestamp,
            "created": created,
            "fast": fast,
            "history_eligible": not fast,
        },
        "raw_evidence": None,
        "paired_rule_evidence": [],
        "rule_violations": [],
        "regressions": [],
        "sanity_warnings": [],
        "benchmarks": {
            "bench_alpha": {
                "title": "Alpha",
                "description": "Public benchmark",
                "location": "bench/alpha.cpp",
                "test_case_descriptions": {"tiny": "Tiny public case"},
                "execution_tuples": tuples,
            }
        },
    }


class BenchmarkBundleContract(unittest.TestCase):
    maxDiff = None

    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(
            prefix="bioseq-benchmark-bundle-contract-"
        )
        self.addCleanup(self._temporary.cleanup)
        temporary_root = Path(self._temporary.name)
        self.repository = temporary_root / "repository"
        self.reporting = self.repository / "reporting"
        self.configuration = self.repository / "config" / "settings.json"
        self.output = temporary_root / "generated" / "benchmark-bundle"
        self.reporting.mkdir(parents=True)
        self.configuration.parent.mkdir(parents=True)
        self._write_json(self.configuration, _configuration())

    @staticmethod
    def _write_json(path: Path, value: object) -> bytes:
        payload = (
            json.dumps(value, ensure_ascii=False, indent=2).encode("utf-8") + b"\n"
        )
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        return payload

    @staticmethod
    def _load_inventory_validator():
        validator_path = (
            WEBSITE_ROOT / "tests" / "contract" / "test_generated_site_validation.py"
        )
        fake_html5lib = types.ModuleType("html5lib")
        fake_html5parser = types.ModuleType("html5lib.html5parser")
        fake_html5parser.ParseError = type("FakeHtml5ParseError", (Exception,), {})
        fake_html5lib.html5parser = fake_html5parser
        spec = importlib.util.spec_from_file_location(
            "benchmark_generated_site_validator_probe", validator_path
        )
        if spec is None or spec.loader is None:
            raise AssertionError("cannot load generated-site validator module")
        module = importlib.util.module_from_spec(spec)
        with mock.patch.dict(
            sys.modules,
            {
                "html5lib": fake_html5lib,
                "html5lib.html5parser": fake_html5parser,
            },
        ):
            spec.loader.exec_module(module)
        return module._benchmark_data_inventory_problems

    def _run(
        self,
        *,
        reporting: Path | None = None,
        output: Path | None = None,
        build_root: Path | None = None,
        override: Path | None = None,
        expect_success: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        command = [
            sys.executable,
            str(GENERATOR),
            "--repository-root",
            str(self.repository),
            "--output-root",
            str(output or self.output),
            "--reporting-root",
            str(reporting or self.reporting),
            "--configuration",
            str(self.configuration),
        ]
        if override is not None:
            command.extend(("--manifest-override", str(override)))
        if build_root is not None:
            command.extend(("--build-root", str(build_root)))
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
            timeout=15,
        )
        if expect_success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("generate_benchmark_bundle:", result.stderr)
        return result

    def _manifest(self) -> dict[str, object]:
        return json.loads(
            (self.output / "benchmarks-manifest.json").read_text(encoding="utf-8")
        )

    def test_incomplete_evidence_is_excluded_not_published(self) -> None:
        source = self.reporting / "interrupted.json"
        document = _report(1_700_000_000)
        empty_duration = {"count": 0, "min": None, "p50": None,
                          "mean": None, "p95": None}
        document["raw_evidence"] = {
            "schema": 1, "path": "report_1.raw.jsonl", "run_id": "interrupted",
            "complete": False, "record_count": 0, "prefix_bytes": 1,
            "append_order_checksum": "0000000000000000",
            "schedule_sorted_checksum": "0000000000000000",
            "outer_e2e_ns": empty_duration, "index_and_map_ns": empty_duration,
            "cells": [],
        }
        original = self._write_json(source, document)
        summary = json.loads(self._run().stdout)
        self.assertEqual(summary["report_count"], 0)
        self.assertTrue(any("incomplete raw evidence" in item
                            for item in summary["excluded"]))
        self.assertNotIn("interrupted.json", "\n".join(self._manifest()["sources"]))
        self.assertEqual(source.read_bytes(), original)
        document["raw_evidence"]["complete"] = "false"
        self._write_json(source, document)
        self.assertIn("completion flag", self._run(expect_success=False).stderr)

    def test_rejected_e2e_samples_are_excluded_without_relaxing_timings(self) -> None:
        source = self.reporting / "rejected.json"
        document = _report(1_700_000_000)
        sample = {
            "schema": 1, "benchmark": "bench_alpha", "test_case": "tiny",
            "algorithm": "algo", "group_index": 0, "group_count": 1,
            "tuple_index": 0, "repeat": 0, "params": {"mode": "x"},
            "outer_e2e_ns": None, "raw_record_id": 0,
            "record_checksum": "0000000000000000",
            "payload_schema": "bioseq.e2e.raw-sample",
            "tuple_status": {"accepted": 0},
            "schedule": {
                "benchmark_index": 0, "test_case_index": 0, "tuple_index": 0,
                "group_index": 0, "repeat": 0, "mode": "item_major",
                "configured_seed": "0000000000000000",
                "shuffle_seed": "0000000000000000", "order_position": 0,
            },
        }
        document["benchmarks"]["bench_alpha"]["raw_samples"] = [sample]
        original = self._write_json(source, document)
        summary = json.loads(self._run().stdout)
        self.assertEqual(summary["report_count"], 0)
        self.assertTrue(any("rejected E2E tuple" in item
                            for item in summary["excluded"]))
        self.assertEqual(source.read_bytes(), original)

        # A supposedly accepted sample with an absent/negative duration still
        # fails closed; exclusion is not a generic invalid-data escape hatch.
        sample["tuple_status"]["accepted"] = 1
        for duration in (None, -1):
            sample["outer_e2e_ns"] = duration
            self._write_json(source, document)
            self.assertIn("outer_e2e_ns", self._run(expect_success=False).stderr)
        sample["outer_e2e_ns"] = 100
        self._write_json(source, document)
        self.assertEqual(json.loads(self._run().stdout)["report_count"], 1)

    def test_cmake_owned_build_subdirectory_is_not_authored_source(self) -> None:
        build = self.repository / "build" / "website"
        build.mkdir(parents=True)
        cache = build / "CMakeCache.txt"
        cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={self.repository}\n")
        output = build / "website" / "benchmark-bundle"
        output.mkdir(parents=True)  # Ninja pre-creates BYPRODUCT parent directories.
        unowned = output / "preserve-me"
        unowned.write_text("authored content")
        rejected = self._run(output=output, build_root=build, expect_success=False)
        self.assertIn("ownership marker", rejected.stderr)
        self.assertEqual(unowned.read_text(), "authored content")
        unowned.unlink()
        self._run(output=output, build_root=build)
        first = (output / "benchmarks-manifest.json").read_bytes()
        self._run(output=output, build_root=build)
        self.assertEqual((output / "benchmarks-manifest.json").read_bytes(), first)

        for unsafe in (build, self.repository / "website" / "authored-output"):
            rejected = self._run(output=unsafe, build_root=build, expect_success=False)
            self.assertIn("below the declared CMake build root", rejected.stderr)
        rejected = self._run(output=output, build_root=self.repository, expect_success=False)
        self.assertIn("below the declared CMake build root", rejected.stderr)
        cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={self.repository.parent}\n")
        rejected = self._run(output=output, build_root=build, expect_success=False)
        self.assertIn("different source tree", rejected.stderr)
        cache.unlink()
        outside_cache = self.repository.parent / "cache.txt"
        outside_cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={self.repository}\n")
        cache.symlink_to(outside_cache)
        rejected = self._run(output=output, build_root=build, expect_success=False)
        self.assertIn("below the declared CMake build root", rejected.stderr)
        self.assertEqual((output / "benchmarks-manifest.json").read_bytes(), first)

    def test_add_delete_metadata_order_deployment_paths_and_exact_bytes(self) -> None:
        older = self.reporting / "history" / "zeta.json"
        older_payload = self._write_json(older, _report(1_700_000_000))
        reporting_config = self.reporting / "public" / "additional-config.json"
        config_payload = self._write_json(
            reporting_config, _configuration("bench_beta")
        )
        self._write_json(self.reporting / "unrelated.json", {"cache": "receipt"})
        self._write_json(
            self.reporting / "unrelated-metadata.json",
            {"meta": {"kind": "cache receipt"}, "results": []},
        )
        self._write_json(
            self.reporting / "debug" / "diagnostic.json",
            _report(1_700_000_100),
        )
        self._write_json(
            self.reporting / "history" / "discard.partial.json",
            _report(1_700_000_200),
        )
        incompatible_report = _report(1_700_000_250)
        incompatible_report["benchmarks"]["bench_unknown"] = (
            incompatible_report["benchmarks"].pop("bench_alpha")
        )
        self._write_json(
            self.reporting / "history" / "incompatible.json",
            incompatible_report,
        )
        (self.reporting / "events.jsonl").write_text(
            '{"benchmarks": "debug stream"}\n', encoding="utf-8"
        )

        first = self._run()
        first_summary = json.loads(first.stdout)
        self.assertEqual(first_summary["configuration_count"], 2)
        self.assertEqual(first_summary["report_count"], 1)
        self.assertTrue(
            any("no supported public configuration" in item
                for item in first_summary["excluded"])
        )
        first_sources = self._manifest()["sources"]
        self.assertIn("benchmark-data/reporting/history/zeta.json", first_sources)
        self.assertNotIn("diagnostic.json", "\n".join(first_sources))
        self.assertNotIn("discard.partial.json", "\n".join(first_sources))
        self.assertEqual(
            (self.output / "benchmark-data" / "reporting" / "history" / "zeta.json").read_bytes(),
            older_payload,
        )
        self.assertEqual(
            (
                self.output
                / "benchmark-data"
                / "reporting"
                / "public"
                / "additional-config.json"
            ).read_bytes(),
            config_payload,
        )

        # The new report is deliberately lexicographically earlier. Report
        # metadata, not filename folklore, must put it first among reports.
        newest = self.reporting / "history" / "alpha.json"
        newest_payload = self._write_json(newest, _report(1_700_000_300))
        self._run()
        sources = self._manifest()["sources"]
        report_sources = [source for source in sources if "/history/" in source]
        self.assertEqual(
            report_sources,
            [
                "benchmark-data/reporting/history/alpha.json",
                "benchmark-data/reporting/history/zeta.json",
            ],
        )
        self.assertEqual(
            (self.output / report_sources[0]).read_bytes(), newest_payload
        )
        for source in sources:
            parsed = urlsplit(source)
            self.assertFalse(parsed.scheme)
            self.assertFalse(parsed.netloc)
            self.assertFalse(source.startswith("/"))
            self.assertEqual(
                urljoin("https://example.test/", source),
                f"https://example.test/{source}",
            )
            self.assertEqual(
                urljoin("https://example.test/bioseqdb/", source),
                f"https://example.test/bioseqdb/{source}",
            )

        newest.unlink()
        self._run()
        after_delete = self._manifest()["sources"]
        self.assertNotIn("benchmark-data/reporting/history/alpha.json", after_delete)
        self.assertIn("benchmark-data/reporting/history/zeta.json", after_delete)

    def test_malformed_partial_credentials_and_path_escapes_fail_closed(self) -> None:
        good = self.reporting / "public.json"
        self._write_json(good, _report(1_700_001_000))
        self._run()
        original_manifest = (self.output / "benchmarks-manifest.json").read_bytes()
        unexpected = self.output / "user-file.txt"
        unexpected.write_text("preserve", encoding="utf-8")
        owned_result = self._run(expect_success=False)
        self.assertIn("unknown entries", owned_result.stderr)
        self.assertEqual(unexpected.read_text(encoding="utf-8"), "preserve")
        unexpected.unlink()

        malformed = self.reporting / "malformed.json"
        malformed.write_bytes(b'{"benchmarks": ')
        malformed_result = self._run(expect_success=False)
        self.assertIn("malformed UTF-8 JSON", malformed_result.stderr)
        self.assertEqual(
            (self.output / "benchmarks-manifest.json").read_bytes(),
            original_manifest,
        )
        malformed.unlink()

        partial = self.reporting / "partial.json"
        partial_document = _report(1_700_001_100)
        del partial_document["rule_violations"]
        self._write_json(partial, partial_document)
        partial_result = self._run(expect_success=False)
        self.assertIn("report fields do not match", partial_result.stderr)
        partial.unlink()

        missing_history = self.reporting / "missing-history.json"
        missing_history_document = _report(1_700_001_125)
        del missing_history_document["meta"]["history_eligible"]
        self._write_json(missing_history, missing_history_document)
        missing_history_result = self._run(expect_success=False)
        self.assertIn("report meta fields", missing_history_result.stderr)
        missing_history.unlink()

        malformed_finding = self.reporting / "malformed-finding.json"
        malformed_finding_document = _report(1_700_001_140)
        malformed_finding_document["rule_violations"] = [{}]
        self._write_json(malformed_finding, malformed_finding_document)
        malformed_finding_result = self._run(expect_success=False)
        self.assertIn("fields do not match", malformed_finding_result.stderr)
        malformed_finding.unlink()

        selective = self.reporting / "selective.json"
        selective_document = _report(1_700_001_150)
        selective_document["benchmarks"]["bench_alpha"][
            "execution_tuples"
        ].pop()
        self._write_json(selective, selective_document)
        selective_result = self._run()
        selective_summary = json.loads(selective_result.stdout)
        self.assertEqual(selective_summary["report_count"], 2)
        self.assertIn("selective.json", "\n".join(self._manifest()["sources"]))
        selective.unlink()

        incompatible = self.reporting / "incompatible-tuple.json"
        incompatible_document = _report(1_700_001_175)
        incompatible_document["benchmarks"]["bench_alpha"][
            "execution_tuples"
        ][0]["tuple"]["algo"]["mode"] = "unknown"
        self._write_json(incompatible, incompatible_document)
        incompatible_result = self._run()
        incompatible_summary = json.loads(incompatible_result.stdout)
        self.assertTrue(
            any("execution tuple is incompatible" in item
                for item in incompatible_summary["excluded"])
        )
        self.assertNotIn(
            "incompatible-tuple.json", "\n".join(self._manifest()["sources"])
        )
        incompatible.unlink()

        credential = self.reporting / "credential.json"
        credential_document = _report(1_700_001_200)
        credential_document["meta"]["github_pat"] = "must-not-publish"
        self._write_json(credential, credential_document)
        credential_result = self._run(expect_success=False)
        self.assertIn("contains credentials", credential_result.stderr)
        credential.unlink()

        outside = self.repository.parent / "outside.json"
        self._write_json(outside, _report(1_700_001_300))
        escaped = self.reporting / "escaped.json"
        escaped.symlink_to(outside)
        escape_result = self._run(expect_success=False)
        self.assertIn("must not traverse a symlink", escape_result.stderr)
        escaped.unlink()

        private_target = self.reporting / "private" / "hidden.json"
        self._write_json(private_target, _report(1_700_001_400))
        internal_link = self.reporting / "linked-public.json"
        internal_link.symlink_to(private_target)
        internal_result = self._run(expect_success=False)
        self.assertIn("must not traverse a symlink", internal_result.stderr)
        internal_link.unlink()

        reporting_escape = self._run(reporting=self.repository.parent, expect_success=False)
        self.assertIn("escapes the repository root", reporting_escape.stderr)

        conflict = self.reporting / "conflicting-config.json"
        conflict_document = _configuration()
        conflict_document["params"]["algo"]["_"]["default"] = "different"
        self._write_json(conflict, conflict_document)
        conflict_result = self._run(expect_success=False)
        self.assertIn("conflicts with", conflict_result.stderr)
        conflict.unlink()

        output_escape = self._run(
            output=self.repository / "generated", expect_success=False
        )
        self.assertIn("outside the repository source tree", output_escape.stderr)

        output_target = self.repository.parent / "output-target"
        output_target.mkdir()
        marker = output_target / "preserve-me"
        marker.write_text("present", encoding="utf-8")
        output_link = self.repository.parent / "output-link"
        output_link.symlink_to(output_target, target_is_directory=True)
        output_link_result = self._run(output=output_link, expect_success=False)
        self.assertIn("output must not be a symlink", output_link_result.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8"), "present")

        unowned_output = self.repository.parent / "unowned-output"
        unowned_output.mkdir()
        unowned_marker = unowned_output / "preserve-me"
        unowned_marker.write_text("present", encoding="utf-8")
        unowned_result = self._run(output=unowned_output, expect_success=False)
        self.assertIn("without the bundle ownership marker", unowned_result.stderr)
        self.assertEqual(unowned_marker.read_text(encoding="utf-8"), "present")

    def test_fast_diagnostics_are_validated_but_not_published(self) -> None:
        fast = self.reporting / "fast.json"
        self._write_json(fast, _report(1_700_002_000, fast=True))
        result = self._run()
        summary = json.loads(result.stdout)
        self.assertEqual(summary["report_count"], 0)
        self.assertTrue(
            any("fast diagnostic report" in item for item in summary["excluded"])
        )
        self.assertNotIn("fast.json", "\n".join(self._manifest()["sources"]))

        malformed_fast = copy.deepcopy(_report(1_700_002_100, fast=True))
        malformed_fast["benchmarks"]["bench_alpha"]["execution_tuples"].pop()
        self._write_json(fast, malformed_fast)
        incomplete = self._run()
        incomplete_summary = json.loads(incomplete.stdout)
        self.assertTrue(
            any("fast diagnostic report" in item
                for item in incomplete_summary["excluded"])
        )

    def test_generated_validator_rejects_unlisted_and_remote_local_data(self) -> None:
        validate_inventory = self._load_inventory_validator()
        site = self.repository.parent / "validator-site"
        manifest = site / "benchmarks-manifest.json"
        data_root = site / "benchmark-data"
        data_root.mkdir(parents=True)
        manifest.write_text('{"sources": []}\n', encoding="utf-8")
        published = data_root / "public.json"
        published.write_text("{}\n", encoding="utf-8")

        self.assertEqual(
            validate_inventory(
                manifest,
                {published.resolve()},
                has_local_source=True,
                remote_count=0,
            ),
            [],
        )
        unlisted = data_root / "debug.raw.jsonl"
        unlisted.write_text('{"debug": true}\n', encoding="utf-8")
        local_problems = validate_inventory(
            manifest,
            {published.resolve()},
            has_local_source=True,
            remote_count=0,
        )
        self.assertTrue(
            any("benchmark inventory mismatch" in problem
                and "debug.raw.jsonl" in problem
                for problem in local_problems)
        )

        remote_problems = validate_inventory(
            manifest,
            set(),
            has_local_source=False,
            remote_count=1,
        )
        self.assertTrue(
            any("remote benchmark override retains local data" in problem
                for problem in remote_problems)
        )
        published.unlink()
        unlisted.unlink()
        self.assertEqual(
            validate_inventory(
                manifest,
                set(),
                has_local_source=False,
                remote_count=1,
            ),
            [],
        )

    def test_public_raw_override_is_bounded_and_byte_preserving(self) -> None:
        self._write_json(self.reporting / "public.json", _report(1_700_003_000))
        override = self.repository.parent / "public-raw.json"
        override_payload = self._write_json(
            override,
            {
                "sources": [
                    "https://raw.githubusercontent.com/bioseqdb/bioseqdb/"
                    "0123456789abcdef0123456789abcdef01234567/reporting/public.json"
                ]
            },
        )
        self._run(override=override)
        self.assertEqual(
            (self.output / "benchmarks-manifest.json").read_bytes(),
            override_payload,
        )
        self.assertEqual(
            list((self.output / "benchmark-data").rglob("*")),
            [],
            "remote override bundles must not retain unreferenced local payloads",
        )

        for unsafe in (
            "https://user@raw.githubusercontent.com/org/repo/main/report.json",
            "https://raw.githubusercontent.com/org/repo/main/../secret.json",
            "https://raw.githubusercontent.com/org/repo/main/report.json?token=x",
            "https://raw.githubusercontent.com/org/repo/main/report.json",
            "https://github.com/org/repo/blob/main/report.json",
        ):
            self._write_json(override, {"sources": [unsafe]})
            result = self._run(override=override, expect_success=False)
            self.assertIn("not a safe public raw URL", result.stderr)

    def test_production_generation_path_has_no_report_specific_literal(self) -> None:
        matches: list[str] = []
        for path in sorted(WEBSITE_ROOT.rglob("*")):
            relative = path.relative_to(WEBSITE_ROOT)
            production_validator = (
                relative
                == Path("tests/contract/test_generated_site_validation.py")
            )
            if not path.is_file() or (
                "tests" in relative.parts and not production_validator
            ):
                continue
            if (
                path.suffix
                not in {".cmake", ".html", ".js", ".json", ".md", ".py", ".txt"}
                and path.name != "CMakeLists.txt"
            ):
                continue
            try:
                contents = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            for match in REPORT_LITERAL.finditer(contents):
                matches.append(f"{relative}:{match.group(0)}")
        self.assertEqual(matches, [])


if __name__ == "__main__":
    unittest.main()
