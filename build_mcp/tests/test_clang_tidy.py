#!/usr/bin/env python3
"""Contract tests for the compile-database clang-tidy runner."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


BUILD_MCP_ROOT = Path(__file__).resolve().parents[1]
if str(BUILD_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(BUILD_MCP_ROOT))

import build_presets
import build_worker
import clang_tidy
import protocol


class ClangTidyCompileDatabaseTests(unittest.TestCase):
    """Verify owned-unit selection and exact compiler-argument replay."""

    def test_load_selects_authored_and_generated_units_only(self) -> None:
        """Exclude vendor, toolchain, and nongenerated build-tree units."""

        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary).resolve()
            build_dir = source_root / "build/test-opt"
            authored = source_root / "bioseq/authored.cppm"
            generated = build_dir / "bioseq/generated/example.cppm"
            vendor = source_root / "vendor/library.cpp"
            ordinary_build_source = build_dir / "scratch.cppm"
            for path in (authored, generated, vendor, ordinary_build_source):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("export module example;\n", encoding="utf-8")
            compiler = source_root / "toolchain/clang++"
            entries = []
            for path in (authored, generated, vendor, ordinary_build_source):
                entries.append(
                    {
                        "directory": str(build_dir),
                        "file": str(path),
                        "arguments": [
                            str(compiler),
                            "-std=gnu++26",
                            "@example.modmap",
                            "-fmodule-file=example=example.pcm",
                            "-fdiagnostics-color=always",
                            "-o",
                            "object.o",
                            "-c",
                            str(path),
                        ],
                    }
                )
            build_dir.mkdir(parents=True, exist_ok=True)
            (build_dir / "compile_commands.json").write_text(
                json.dumps(entries), encoding="utf-8"
            )

            commands = clang_tidy._load_compile_commands(source_root, build_dir)

            self.assertEqual(
                [command.display_path for command in commands],
                ["bioseq/authored.cppm", "bioseq/generated/example.cppm"],
            )
            self.assertEqual([command.is_generated for command in commands], [False, True])
            self.assertEqual(
                commands[0].compiler_arguments,
                (
                    "-std=gnu++26",
                    "@example.modmap",
                    "-fmodule-file=example=example.pcm",
                ),
            )

    def test_strip_driver_outputs_resolves_relative_compiler(self) -> None:
        """Remove joined output arguments without dropping semantic module flags."""

        directory = Path("/repository/build/test-opt")
        source = Path("/repository/bioseq/source.cppm")
        compiler, arguments = clang_tidy._strip_driver_outputs(
            [
                "toolchain/clang++",
                "-std=gnu++26",
                "-oobject.o",
                "-MFdependency.d",
                "@source.modmap",
                str(source),
            ],
            source=source,
            directory=directory,
        )

        self.assertEqual(compiler, directory / "toolchain/clang++")
        self.assertEqual(arguments, ("-std=gnu++26", "@source.modmap"))

    def test_load_merges_first_party_generator_database(self) -> None:
        """Include generator modules without admitting nested dependency sources."""

        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary).resolve()
            build_dir = source_root / "build/test-opt"
            generator_build = build_dir / "codegen/generator-build"
            generator_source = source_root / "codegen/highway/index.cppm"
            dependency_source = generator_build / "_deps/library/source.cpp"
            compiler = source_root / "toolchain/clang++"
            for path in (generator_source, dependency_source):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("export module example;\n", encoding="utf-8")
            build_dir.mkdir(parents=True, exist_ok=True)
            (build_dir / "compile_commands.json").write_text("[]", encoding="utf-8")
            nested_entries = [
                {
                    "directory": str(generator_build),
                    "file": str(source),
                    "arguments": [str(compiler), "-c", str(source)],
                }
                for source in (generator_source, dependency_source)
            ]
            (generator_build / "compile_commands.json").write_text(
                json.dumps(nested_entries), encoding="utf-8"
            )

            commands = clang_tidy._load_compile_commands(source_root, build_dir)
            database_evidence = clang_tidy._compile_database_evidence(build_dir)

        self.assertEqual(
            [command.display_path for command in commands],
            ["codegen/highway/index.cppm"],
        )
        self.assertEqual(
            [item["path"] for item in database_evidence],
            ["compile_commands.json", "codegen/generator-build/compile_commands.json"],
        )

    def test_focused_selection_requires_exact_compile_units(self) -> None:
        """Reject traversal and missing focused paths before tool execution."""

        root = Path("/repository")
        build = root / "build/test-opt"
        command = clang_tidy.CompileCommand(
            source=root / "bioseq/example.cppm",
            directory=build,
            compiler=Path("/tool/clang++"),
            compiler_arguments=("-std=gnu++26",),
            display_path="bioseq/example.cppm",
            is_generated=False,
            variant=0,
        )
        self.assertEqual(
            clang_tidy._select_commands(
                (command,), scope="focused", focused_paths=("bioseq/example.cppm",)
            ),
            (command,),
        )
        with self.assertRaisesRegex(ValueError, "normalized and relative"):
            clang_tidy._select_commands(
                (command,), scope="focused", focused_paths=("bioseq/../example.cppm",)
            )
        with self.assertRaisesRegex(ValueError, "no owned compile command"):
            clang_tidy._select_commands(
                (command,), scope="focused", focused_paths=("bioseq/missing.cppm",)
            )

    def test_matching_tidy_rejects_module_major_mismatch(self) -> None:
        """Reject a Clang 20 tidy binary for a Clang 22 module database."""

        with tempfile.TemporaryDirectory() as temporary:
            tool_root = Path(temporary)
            compiler = tool_root / "clang++"
            tidy = tool_root / "clang-tidy"
            compiler.touch()
            tidy.touch()
            compiler.chmod(0o755)
            tidy.chmod(0o755)
            command = clang_tidy.CompileCommand(
                source=tool_root / "example.cppm",
                directory=tool_root,
                compiler=compiler,
                compiler_arguments=(),
                display_path="bioseq/example.cppm",
                is_generated=False,
                variant=0,
            )
            with mock.patch.object(
                clang_tidy,
                "_tool_major_version",
                side_effect=((22, "Clang 22.1.6"), (20, "LLVM 20.1.8")),
            ):
                with self.assertRaisesRegex(RuntimeError, "must match"):
                    clang_tidy._matching_clang_tidy((command,))

    def test_missing_matching_tidy_has_install_guidance(self) -> None:
        """Name the co-located LLVM tools packages when clang-tidy is absent."""

        with tempfile.TemporaryDirectory() as temporary:
            compiler = Path(temporary) / "clang++"
            compiler.touch()
            command = clang_tidy.CompileCommand(
                source=Path(temporary) / "source.cppm",
                directory=Path(temporary),
                compiler=compiler,
                compiler_arguments=(),
                display_path="bioseq/source.cppm",
                is_generated=False,
                variant=0,
            )
            with mock.patch.object(
                clang_tidy,
                "_tool_major_version",
                return_value=(22, "Clang 22.1.6"),
            ):
                with self.assertRaisesRegex(RuntimeError, "clang-tidy-22") as raised:
                    clang_tidy._matching_clang_tidy((command,))

        message = str(raised.exception)
        self.assertIn(str(compiler.with_name("clang-tidy")), message)
        self.assertIn("clang-tools-22", message)
        self.assertIn("brew install llvm@22", message)

    def test_aws_bootstrap_provisions_matching_tidy_tools(self) -> None:
        """Keep the supported LLVM 22 apt image complete for module linting."""

        bootstrap = (
            BUILD_MCP_ROOT.parent / "infra/testing_infra/scripts/bootstrap.sh"
        ).read_text(encoding="utf-8")

        for contract in (
            'local clang_tidy="$root/bin/clang-tidy"',
            '[ -x "$clang" ] && [ -x "$clang_tidy" ] || return 1',
            '[ "${clang_have%%.*}" = "${clang_tidy_have%%.*}" ] || return 1',
            '"clang-tidy-$LLVM_VERSION"',
            '"clang-tools-$LLVM_VERSION"',
            'CLANG_TIDY_ACTUAL_VERSION=',
            'clang-tidy major $CLANG_TIDY_ACTUAL_VERSION does not match clang',
        ):
            with self.subTest(contract=contract):
                self.assertIn(contract, bootstrap)

    def test_policy_is_verified_by_the_matched_tidy(self) -> None:
        """Reject policy keys the selected clang-tidy binary cannot parse."""

        tidy = Path("/tool/clang-tidy")
        config = Path("/repository/.clang-tidy")
        completed = mock.Mock(
            returncode=0,
            stdout="No config errors detected.\n",
            stderr="",
        )
        with mock.patch.object(clang_tidy.subprocess, "run", return_value=completed) as run:
            verification = clang_tidy._verify_config(tidy, config)

        self.assertEqual(verification, "No config errors detected.")
        self.assertEqual(
            run.call_args.args[0],
            [str(tidy), f"--config-file={config}", "--verify-config"],
        )

    def test_root_policy_keeps_the_known_module_crash_suppression(self) -> None:
        """Keep the Clang 22 imported-module crash exclusion explicit and singular."""

        policy = (BUILD_MCP_ROOT.parent / ".clang-tidy").read_text(encoding="utf-8")

        self.assertEqual(policy.count("-bugprone-argument-comment"), 1)
        self.assertIn("sole toolchain crash exclusion", policy)


class ClangTidyPolicyTests(unittest.TestCase):
    """Verify boundary-specific exceptions and generated owner routing."""

    @staticmethod
    def _command(path: str, *, generated: bool = False) -> clang_tidy.CompileCommand:
        """Create a minimal immutable command for policy classification."""

        return clang_tidy.CompileCommand(
            source=Path("/repository") / path,
            directory=Path("/repository/build/test-opt"),
            compiler=Path("/tool/clang++"),
            compiler_arguments=(),
            display_path=path,
            is_generated=generated,
            variant=0,
        )

    def test_exceptions_are_scoped_to_declared_boundaries(self) -> None:
        """Keep ordinary code on the complete root policy."""

        ordinary_paths = (
            "bioseq/io/source.cppm",
            "bioseq/tests/unit/alignment/block_aligner_test.cpp",
            "bioseq/tests/unit/indexing/minimizer_hashmap_test.cpp",
            "bioseq/tests/unit/simd/vector_stream_test.cpp",
        )
        postgres = self._command("bioseq/backend/pg/extension/index.cppm")
        pg_vectorization = self._command(
            "bioseq/backend/pg/vectorization/vector_execution.cppm"
        )
        highway = self._command(
            "bioseq/generated/highway/neon/binder.cppm", generated=True
        )
        hot = self._command("bioseq/backend/sequences/indexing/index.cppm")
        alignment_hwy = self._command("bioseq/backend/alignment_hwy/ksw.cppm")
        vector_compare = self._command("bioseq/backend/vector_compare/index.cppm")
        fmtlog = self._command(
            "bioseq/backend/common/logging/fmtlog_backend.cppm"
        )
        logging_index = self._command("bioseq/backend/common/logging/index.cppm")
        logging_formatters = self._command(
            "bioseq/backend/common/logging/formatters.cppm"
        )
        for ordinary_path in ordinary_paths:
            with self.subTest(ordinary_path=ordinary_path):
                self.assertEqual(
                    clang_tidy._disabled_checks(self._command(ordinary_path)), ()
                )
        postgres_checks = set(clang_tidy._disabled_checks(postgres))
        pg_vectorization_checks = set(
            clang_tidy._disabled_checks(pg_vectorization)
        )
        self.assertTrue(
            set(clang_tidy._POSTGRES_BOUNDARY_DISABLED_CHECKS).issubset(
                postgres_checks
            )
        )
        self.assertIn("readability-identifier-naming", postgres_checks)
        self.assertNotIn(
            "readability-identifier-naming", pg_vectorization_checks
        )
        self.assertTrue(
            (
                set(clang_tidy._POSTGRES_BOUNDARY_DISABLED_CHECKS)
                - {"readability-identifier-naming"}
            ).issubset(pg_vectorization_checks)
        )
        self.assertIn(
            "readability-identifier-naming",
            clang_tidy._disabled_checks(highway),
        )
        self.assertIn("modernize-make-unique", clang_tidy._disabled_checks(highway))
        self.assertIn("modernize-use-ranges", clang_tidy._disabled_checks(hot))
        zero_cost_rewrite_checks = {
            "cppcoreguidelines-avoid-c-arrays",
            "cppcoreguidelines-avoid-magic-numbers",
            "cppcoreguidelines-owning-memory",
            "cppcoreguidelines-pro-bounds-avoid-unchecked-container-access",
            "cppcoreguidelines-pro-bounds-constant-array-index",
            "cppcoreguidelines-pro-bounds-pointer-arithmetic",
            "modernize-avoid-c-arrays",
            "modernize-make-shared",
            "modernize-make-unique",
            "portability-simd-intrinsics",
            "readability-use-anyofallof",
        }
        for command in (alignment_hwy, vector_compare):
            with self.subTest(path=command.display_path):
                self.assertTrue(
                    zero_cost_rewrite_checks.issubset(
                        clang_tidy._disabled_checks(command)
                    )
                )
        fmtlog_checks = set(clang_tidy._disabled_checks(fmtlog))
        self.assertTrue(
            {
                "cppcoreguidelines-avoid-c-arrays",
                "cppcoreguidelines-owning-memory",
                "cppcoreguidelines-pro-bounds-pointer-arithmetic",
                "cppcoreguidelines-pro-type-cstyle-cast",
                "modernize-*",
                "readability-*",
            }.issubset(fmtlog_checks)
        )
        self.assertEqual(clang_tidy._disabled_checks(logging_index), ())
        self.assertEqual(clang_tidy._disabled_checks(logging_formatters), ())

    def test_invocation_has_exact_line_filter_and_retains_broad_checks(self) -> None:
        """Prevent imported-module diagnostics from escaping the selected unit."""

        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary).resolve()
            source = source_root / "bioseq/io/source.cppm"
            build_dir = source_root / "build/test-opt"
            source.parent.mkdir(parents=True)
            build_dir.mkdir(parents=True)
            source.write_text("export module example;\n\n", encoding="utf-8")
            command = clang_tidy.CompileCommand(
                source=source,
                directory=build_dir,
                compiler=Path("/tool/clang++"),
                compiler_arguments=("-std=gnu++26",),
                display_path="bioseq/io/source.cppm",
                is_generated=False,
                variant=0,
            )

            invocation = clang_tidy._clang_tidy_invocation(
                Path("/tool/clang-tidy"), command, source_root=source_root
            )

            self.assertIn(f"--config-file={source_root / '.clang-tidy'}", invocation)
            self.assertIn("--header-filter=^$", invocation)
            self.assertIn("--use-color=false", invocation)
            line_filter_argument = next(
                argument for argument in invocation if argument.startswith("--line-filter=")
            )
            line_filter = json.loads(line_filter_argument.split("=", 1)[1])
            self.assertEqual(line_filter, [{"name": str(source), "lines": [[1, 2]]}])
            checks_argument = next(
                argument for argument in invocation if argument.startswith("--checks=")
            )
            self.assertIn("clang-analyzer-core.*", checks_argument)
            self.assertNotIn("--config=", " ".join(invocation))

    def test_generated_owner_candidates_prefer_authored_ninja_inputs(self) -> None:
        """Route generated diagnostics to source and generator inputs."""

        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary).resolve()
            build_dir = source_root / "build/test-opt"
            generated = build_dir / "bioseq/generated/template_flatten/instance_0.cppm"
            authored = source_root / "bioseq/backend/pipeline/template.cppm"
            generated.parent.mkdir(parents=True)
            authored.parent.mkdir(parents=True)
            generated.write_text("module generated;\n", encoding="utf-8")
            authored.write_text("export module authored;\n", encoding="utf-8")
            command = clang_tidy.CompileCommand(
                source=generated,
                directory=build_dir,
                compiler=Path("/tool/clang++"),
                compiler_arguments=(),
                display_path="bioseq/generated/template_flatten/instance_0.cppm",
                is_generated=True,
                variant=0,
            )
            noisy_inputs = "".join(
                f"    {source_root / f'aaa_owner_{index}.cppm'}\n"
                for index in range(20)
            )
            query = mock.Mock(
                returncode=0,
                stdout=(
                    "target:\n"
                    "  input: CUSTOM_COMMAND\n"
                    f"    {authored}\n"
                    f"{noisy_inputs}"
                    "    bioseq/generated/tool\n"
                    "  outputs:\n"
                ),
            )
            with mock.patch.object(clang_tidy.subprocess, "run", return_value=query):
                owners = clang_tidy._generated_owner_candidates(
                    command, source_root=source_root, build_dir=build_dir
                )
            self.assertIn("bioseq/backend/pipeline/template.cppm", owners)
            self.assertIn("cmake/bioseq_template_flatten_codegen.cmake", owners)
            self.assertIn("codegen/namespace_materialization/index.cppm", owners)
            self.assertEqual(len(owners), clang_tidy._MAX_OWNER_CANDIDATES)

    def test_generated_owner_query_uses_nested_generator_graph(self) -> None:
        """Resolve nested generator outputs against their own Ninja graph."""

        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary).resolve()
            build_dir = source_root / "build/test-opt"
            generator_build = build_dir / "codegen/generator-build"
            generated = generator_build / "generated/component_registry.cppm"
            generated.parent.mkdir(parents=True)
            generated.write_text("module generated;\n", encoding="utf-8")
            (generator_build / "build.ninja").touch()
            command = clang_tidy.CompileCommand(
                source=generated,
                directory=generator_build,
                compiler=Path("/tool/clang++"),
                compiler_arguments=(),
                display_path=(
                    "codegen/generator-build/generated/component_registry.cppm"
                ),
                is_generated=True,
                variant=0,
            )
            query = mock.Mock(returncode=0, stdout="target:\n  outputs:\n")
            with mock.patch.object(
                clang_tidy.subprocess, "run", return_value=query
            ) as run:
                owners = clang_tidy._generated_owner_candidates(
                    command,
                    source_root=source_root,
                    build_dir=build_dir,
                )

        self.assertIn("codegen/CMakeLists.txt", owners)
        self.assertEqual(
            run.call_args.args[0],
            [
                "ninja",
                "-C",
                str(generator_build),
                "-t",
                "query",
                "generated/component_registry.cppm",
            ],
        )


class ClangTidyBuilderIntegrationTests(unittest.TestCase):
    """Verify closed mode, schema, and report contracts."""

    def test_lint_modes_are_local_fixed_build_only_gates(self) -> None:
        """Expose focused and full modes without remote execution aliases."""

        for mode in build_presets.LINT_MODES:
            with self.subTest(mode=mode):
                self.assertIn(mode, build_presets.FIXED_BUILD_ONLY_MODES)
                self.assertIn(mode, build_presets.REMOTE_RUN_REJECTED_MODES)
                self.assertNotIn(mode, build_presets.REMOTE_RUN_BUILD_MODES)
                self.assertFalse(build_worker._build_receipt_required(mode, True))
        self.assertEqual(
            build_presets.MODES[build_presets.LINT_FOCUSED_MODE].targets,
            ["postgresql_bioseq"],
        )
        self.assertEqual(
            build_presets.MODES[build_presets.LINT_FULL_MODE].targets,
            ["all"],
        )

    def test_lint_filter_is_closed_and_normalized(self) -> None:
        """Accept only exact normalized compile-unit paths."""

        paths, error = build_presets.lint_filter_to_paths(
            {"paths": ["bioseq/a.cppm", "bioseq/a.cppm", "build/generated/a.cpp"]}
        )
        self.assertIsNone(error)
        self.assertEqual(paths, ["bioseq/a.cppm", "build/generated/a.cpp"])
        for invalid in (
            {"paths": []},
            {"paths": ["../a.cppm"]},
            {"paths": ["bioseq/a.cppm\n"]},
            {"paths": ["bioseq/a.hpp"]},
            {"paths": ["bioseq/a.cppm"], "tags": ["x"]},
        ):
            with self.subTest(invalid=invalid):
                _, error = build_presets.lint_filter_to_paths(invalid)
                self.assertIsNotNone(error)

    def test_public_schema_requires_focused_paths(self) -> None:
        """Publish paths on start_build while retaining closed root arguments."""

        schema = next(
            definition["inputSchema"]
            for definition in protocol.TOOL_DEFINITIONS
            if definition["name"] == "start_build"
        )
        self.assertFalse(schema["additionalProperties"])
        self.assertIn(
            build_presets.LINT_FOCUSED_MODE,
            schema["properties"]["mode"]["enum"],
        )
        self.assertIn("paths", schema["properties"]["filter"]["properties"])
        lint_gate = next(
            rule["then"]
            for rule in schema["allOf"]
            if rule.get("if", {}).get("properties", {}).get("mode", {}).get("enum")
            == sorted(build_presets.LINT_MODES)
        )
        self.assertIn("build_only", lint_gate["required"])
        self.assertTrue(lint_gate["properties"]["build_only"]["const"])
        rejected_properties = {
            item["required"][0] for item in lint_gate["not"]["anyOf"]
        }
        self.assertIn("build_timeout_sec", rejected_properties)
        full_gate = next(
            rule["then"]
            for rule in schema["allOf"]
            if rule.get("if", {}).get("properties", {}).get("mode", {}).get("const")
            == build_presets.LINT_FULL_MODE
        )
        self.assertEqual(full_gate["not"], {"required": ["filter"]})

    def test_worker_command_keeps_reports_inside_build_tree(self) -> None:
        """Construct focused runner arguments without source mutation flags."""

        build_dir = Path("/repository/build/test-opt")
        command = build_worker._clang_tidy_command(
            build_dir,
            build_presets.LINT_FOCUSED_MODE,
            ["bioseq/source.cppm"],
        )
        self.assertIn("--scope", command)
        self.assertIn("focused", command)
        self.assertIn("--path", command)
        self.assertNotIn("--fix", command)
        self.assertEqual(
            build_worker._clang_tidy_report_path(
                build_dir, build_presets.LINT_FOCUSED_MODE
            ),
            build_dir / "bioseq-clang-tidy/focused.json",
        )
        full_command = build_worker._clang_tidy_command(
            build_dir,
            build_presets.LINT_FULL_MODE,
            [],
        )
        self.assertIn("full", full_command)
        self.assertNotIn("--path", full_command)

    def test_worker_publishes_bounded_report_summary(self) -> None:
        """Validate the lint counters exposed through build status."""

        with tempfile.TemporaryDirectory() as temporary:
            report_path = Path(temporary) / "report.json"
            report_path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "scope": "focused",
                        "selected_count": 3,
                        "authored_count": 1,
                        "generated_count": 2,
                        "diagnostic_count": 4,
                        "failure_count": 2,
                        "compile_databases": [
                            {"path": "compile_commands.json", "sha256": "def"}
                        ],
                        "policy_sha256": "abc",
                        "toolchain": {"major": 22},
                    }
                ),
                encoding="utf-8",
            )

            summary = build_worker._clang_tidy_report_summary(
                report_path,
                expected_scope="focused",
            )
            with self.assertRaisesRegex(RuntimeError, "scheduled mode"):
                build_worker._clang_tidy_report_summary(
                    report_path,
                    expected_scope="full",
                )

        self.assertEqual(summary["selected_count"], 3)
        self.assertEqual(summary["failure_count"], 2)
        self.assertEqual(summary["compile_database_count"], 1)
        self.assertEqual(summary["toolchain"], {"major": 22})


if __name__ == "__main__":
    unittest.main()
