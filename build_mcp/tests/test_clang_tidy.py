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
            minimap2 = source_root / "bench/tool_minimap2/minimap2/index.c"
            sneakysnake = (
                source_root / "bench/tool_sneakysnake/SneakySnake/index.cpp"
            )
            ordinary_build_source = build_dir / "scratch.cppm"
            for path in (
                authored,
                generated,
                vendor,
                minimap2,
                sneakysnake,
                ordinary_build_source,
            ):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("export module example;\n", encoding="utf-8")
            compiler = source_root / "toolchain/clang++"
            entries = []
            for path in (
                authored,
                generated,
                vendor,
                minimap2,
                sneakysnake,
                ordinary_build_source,
            ):
                entries.append(
                    {
                        "directory": str(build_dir),
                        "file": str(path),
                        "arguments": [
                            str(compiler),
                            "-std=gnu++26",
                            "-O3",
                            "-DNDEBUG",
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
            (build_dir / "example.modmap").write_text("", encoding="utf-8")
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
                    "-O3",
                    "-DNDEBUG",
                    "@example.modmap",
                    "-fmodule-file=example=example.pcm",
                ),
            )
            self.assertEqual(
                commands[0].response_files[0][0],
                "build/test-opt/example.modmap",
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

    def test_compile_profiles_are_exactly_o3_with_ndebug(self) -> None:
        """Reject deoptimized, competing, and assertion-enabled lint graphs."""

        source = Path("/repository/bioseq/source.cppm")
        clang_tidy._validate_compile_profile(
            source, ("-std=gnu++26", "-O3", "-DNDEBUG")
        )
        invalid_profiles = (
            ("-O2", "-DNDEBUG"),
            ("-O3", "-O2", "-DNDEBUG"),
            ("-O3",),
            ("-O3", "-DNDEBUG", "-UNDEBUG"),
        )
        for arguments in invalid_profiles:
            with self.subTest(arguments=arguments):
                with self.assertRaises(clang_tidy.ClangTidyError):
                    clang_tidy._validate_compile_profile(source, arguments)

    def test_response_files_cannot_hide_deoptimized_or_assertion_flags(self) -> None:
        """Expand authenticated response files before profile verification."""

        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary).resolve()
            build_dir = source_root / "build/test-opt"
            source = source_root / "bioseq/source.cppm"
            build_dir.mkdir(parents=True)
            source.parent.mkdir(parents=True)
            source.write_text("export module source;\n", encoding="utf-8")
            response = build_dir / "flags.rsp"
            for hidden_flag, expected_error in (
                ("-O0", "competing optimization"),
                ("-UNDEBUG", "NDEBUG undefinition"),
            ):
                with self.subTest(hidden_flag=hidden_flag):
                    response.write_text(hidden_flag, encoding="utf-8")
                    (build_dir / "compile_commands.json").write_text(
                        json.dumps(
                            [
                                {
                                    "directory": str(build_dir),
                                    "file": str(source),
                                    "arguments": [
                                        str(source_root / "toolchain/clang++"),
                                        "-O3",
                                        "-DNDEBUG",
                                        "@flags.rsp",
                                        "-c",
                                        str(source),
                                    ],
                                }
                            ]
                        ),
                        encoding="utf-8",
                    )
                    with self.assertRaisesRegex(
                        clang_tidy.ClangTidyError, expected_error
                    ):
                        clang_tidy._load_compile_commands(source_root, build_dir)

    def test_owned_non_clang_driver_fails_closed(self) -> None:
        """Never turn an unrecognized owned compile unit into a coverage skip."""

        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary).resolve()
            build_dir = source_root / "build/test-opt"
            source = source_root / "bioseq/source.cppm"
            source.parent.mkdir(parents=True)
            source.write_text("export module source;\n", encoding="utf-8")
            build_dir.mkdir(parents=True)
            (build_dir / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build_dir),
                            "file": str(source),
                            "arguments": [
                                "/usr/bin/g++",
                                "-O3",
                                "-DNDEBUG",
                                "-c",
                                str(source),
                            ],
                        }
                    ]
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                clang_tidy.ClangTidyError, "cannot be replayed"
            ):
                clang_tidy._load_compile_commands(source_root, build_dir)

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
                    "arguments": [
                        str(compiler),
                        "-O3",
                        "-DNDEBUG",
                        "-c",
                        str(source),
                    ],
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
            commands[0].compile_database,
            "codegen/generator-build/compile_commands.json",
        )
        self.assertEqual(
            [item["path"] for item in database_evidence],
            ["compile_commands.json", "codegen/generator-build/compile_commands.json"],
        )

    def test_full_inventory_requires_generator_database_contribution(self) -> None:
        """Do not authenticate full coverage from an empty nested database."""

        root = Path("/repository")
        primary_authored = clang_tidy.CompileCommand(
            source=root / "bioseq/authored.cppm",
            directory=root / "build/test-opt",
            compiler=Path("/tool/clang++"),
            compiler_arguments=("-O3", "-DNDEBUG"),
            display_path="bioseq/authored.cppm",
            is_generated=False,
            variant=0,
        )
        primary_generated = clang_tidy.CompileCommand(
            source=root / "build/test-opt/bioseq/generated/output.cppm",
            directory=root / "build/test-opt",
            compiler=Path("/tool/clang++"),
            compiler_arguments=("-O3", "-DNDEBUG"),
            display_path="bioseq/generated/output.cppm",
            is_generated=True,
            variant=0,
        )
        generator_authored = clang_tidy.CompileCommand(
            source=root / "codegen/highway/index.cppm",
            directory=root / "build/test-opt/codegen/generator-build",
            compiler=Path("/tool/clang++"),
            compiler_arguments=("-O3", "-DNDEBUG"),
            display_path="codegen/highway/index.cppm",
            is_generated=False,
            variant=0,
            compile_database="codegen/generator-build/compile_commands.json",
        )
        evidence = [
            {"path": "compile_commands.json", "sha256": "a" * 64},
            {
                "path": "codegen/generator-build/compile_commands.json",
                "sha256": "b" * 64,
            },
        ]
        with self.assertRaisesRegex(
            clang_tidy.ClangTidyError, "generator compile database"
        ):
            clang_tidy._validate_full_inventory(
                database_evidence=evidence,
                selected=(primary_authored, primary_generated),
                available_authored=(primary_authored,),
                available_generated=(primary_generated,),
            )
        clang_tidy._validate_full_inventory(
            database_evidence=evidence,
            selected=(primary_authored, primary_generated, generator_authored),
            available_authored=(primary_authored, generator_authored),
            available_generated=(primary_generated,),
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
            'dpkg --compare-versions "$clang_tidy_have" ge "$LLVM_MIN_VERSION"',
            '"clang-tidy-$LLVM_VERSION"',
            '"clang-tools-$LLVM_VERSION"',
            'CLANG_TIDY_ACTUAL_VERSION=',
            (
                'dpkg --compare-versions "$CLANG_TIDY_ACTUAL_VERSION" ge '
                '"$LLVM_MIN_VERSION"'
            ),
            'clang-tidy $CLANG_TIDY_ACTUAL_VERSION is below required',
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
        self.assertIn("  cert-*,", policy)
        self.assertIn("  misc-*,", policy)
        self.assertNotIn("  -misc-include-cleaner,", policy)
        self.assertNotIn("  -misc-use-internal-linkage,", policy)


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
        template_flatten = self._command(
            "bioseq/generated/template_flatten/instance_0.cppm", generated=True
        )
        hot = self._command("bioseq/backend/sequences/indexing/index.cppm")
        packed_partition = self._command(
            "bioseq/backend/sequences/packed_sequence_parts/packing.cppm"
        )
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
        ordinary_module = self._command("bioseq/io/source.cppm")
        self.assertEqual(
            set(clang_tidy._disabled_checks(ordinary_module)),
            set(clang_tidy._NAMED_MODULE_DISABLED_CHECKS),
        )
        self.assertEqual(
            clang_tidy._suppression_profiles(ordinary_module),
            ("named_module",),
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
        self.assertIn(
            "portability-simd-intrinsics", clang_tidy._disabled_checks(highway)
        )
        self.assertIn("modernize-use-ranges", clang_tidy._disabled_checks(hot))
        self.assertNotIn(
            "portability-simd-intrinsics", clang_tidy._disabled_checks(hot)
        )
        for check in (
            "readability-function-size",
            "readability-function-cognitive-complexity",
        ):
            for command in (hot, template_flatten, alignment_hwy, vector_compare):
                with self.subTest(check=check, path=command.display_path):
                    self.assertNotIn(check, clang_tidy._disabled_checks(command))
        self.assertEqual(
            clang_tidy._suppression_profiles(template_flatten),
            ("named_module", "zero_cost_hot_path"),
        )
        self.assertEqual(
            clang_tidy._suppression_profiles(highway),
            ("named_module", "generated_highway", "zero_cost_hot_path"),
        )
        self.assertIn(
            "modernize-use-ranges",
            clang_tidy._disabled_checks(packed_partition),
        )
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
            "readability-use-anyofallof",
        }
        for command in (alignment_hwy, vector_compare):
            with self.subTest(path=command.display_path):
                self.assertTrue(
                    zero_cost_rewrite_checks.issubset(
                        clang_tidy._disabled_checks(command)
                    )
                )
                self.assertNotIn(
                    "portability-simd-intrinsics",
                    clang_tidy._disabled_checks(command),
                )
        for command in (fmtlog, logging_index, logging_formatters):
            with self.subTest(path=command.display_path):
                self.assertEqual(
                    set(clang_tidy._disabled_checks(command)),
                    set(clang_tidy._NAMED_MODULE_DISABLED_CHECKS),
                )

    def test_source_suppressions_are_exact_explained_and_balanced(self) -> None:
        """Inventory real NOLINT comments without mistaking generated strings."""

        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "source.cppm"
            source.write_text(
                'constexpr auto generated = "// NOLINT(*): emitted text";\n'
                'constexpr auto raw = R"tag(/* NOLINT(*) -- emitted */)tag";\n'
                "int ABI_name; // NOLINT(readability-identifier-naming) -- ABI name\n"
                "int ABI_value; /* NOLINT(readability-identifier-naming) -- ABI field */\n"
                "/**\n"
                " * NOLINTNEXTLINE(misc-unused-parameters): generated ABI\n"
                " */\n"
                "void callback(int ignored);\n",
                encoding="utf-8",
            )
            self.assertEqual(
                clang_tidy._source_suppressions(source),
                (
                    {
                        "line": 3,
                        "kind": "NOLINT",
                        "checks": ["readability-identifier-naming"],
                        "reason": "ABI name",
                    },
                    {
                        "line": 4,
                        "kind": "NOLINT",
                        "checks": ["readability-identifier-naming"],
                        "reason": "ABI field",
                    },
                    {
                        "line": 6,
                        "kind": "NOLINTNEXTLINE",
                        "checks": ["misc-unused-parameters"],
                        "reason": "generated ABI",
                    },
                ),
            )
            invalid_sources = (
                "// NOLINT -- broad\n",
                "// NOLINT(*) -- wildcard\n",
                "/** NOLINT(*) -- wildcard */\n",
                "// NOLINT(readability-identifier-naming)\n",
                "// NOLINTBEGIN(readability-identifier-naming) -- region\n",
                (
                    "// NOLINTBEGIN(readability-identifier-naming) -- region\n"
                    "// NOLINTEND(readability-magic-numbers) -- mismatch\n"
                ),
            )
            for content in invalid_sources:
                with self.subTest(content=content):
                    source.write_text(content, encoding="utf-8")
                    with self.assertRaises(clang_tidy.ClangTidyError):
                        clang_tidy._source_suppressions(source)

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
            self.assertIn(
                "bioseq/backend/pipeline/template.cppm", owners.ninja_candidates
            )
            self.assertIn(
                "cmake/bioseq_template_flatten_codegen.cmake",
                owners.heuristic_candidates,
            )
            self.assertIn(
                "codegen/namespace_materialization/index.cppm",
                owners.heuristic_candidates,
            )
            self.assertEqual(
                len(owners.ninja_candidates), clang_tidy._MAX_OWNER_CANDIDATES
            )
            self.assertEqual(owners.query_status, "ninja-authored-inputs")

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

        self.assertEqual(owners.ninja_candidates, ())
        self.assertIn("codegen/CMakeLists.txt", owners.heuristic_candidates)
        self.assertEqual(owners.query_status, "ninja-no-authored-inputs")
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

    def test_unknown_generated_owner_is_unresolved_not_root_owned(self) -> None:
        """Never invent root CMakeLists as an owner when evidence is absent."""

        root = Path("/repository")
        command = clang_tidy.CompileCommand(
            source=root / "build/test-opt/generated/unknown.cppm",
            directory=root / "build/test-opt",
            compiler=Path("/tool/clang++"),
            compiler_arguments=("-O3", "-DNDEBUG"),
            display_path="generated/unknown.cppm",
            is_generated=True,
            variant=0,
        )
        query = mock.Mock(returncode=1, stdout="", stderr="unknown target")
        with mock.patch.object(clang_tidy.subprocess, "run", return_value=query):
            owners = clang_tidy._generated_owner_candidates(
                command,
                source_root=root,
                build_dir=root / "build/test-opt",
            )

        self.assertEqual(owners.ninja_candidates, ())
        self.assertEqual(owners.heuristic_candidates, ())
        self.assertEqual(owners.query_status, "ninja-exit-1")


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
            suppression = {
                "line": 7,
                "kind": "NOLINT",
                "checks": ["readability-identifier-naming"],
                "reason": "ABI name",
            }
            diagnostics = [
                {
                    "path": "generated.cppm",
                    "line": line,
                    "column": 1,
                    "severity": "warning",
                    "message": "diagnostic",
                    "check": "bugprone-example",
                }
                for line in range(1, 5)
            ]
            common_result = {
                "variant": 0,
                "compile_database": "compile_commands.json",
                "response_files": [],
                "suppression_profiles": ["named_module"],
                "disabled_checks": ["misc-include-cleaner"],
                "ninja_owner_candidates": [],
                "heuristic_owner_candidates": [],
                "working_directory": "/repository/build/test-opt",
                "compiler": "/tool/clang++",
                "duration_seconds": 0.25,
                "generator_hint": None,
                "invocation": ["/tool/clang-tidy"],
                "output": "",
            }
            results = [
                {
                    **common_result,
                    "path": "bioseq/authored.cppm",
                    "generated": False,
                    "exit_code": 0,
                    "diagnostics": [],
                    "diagnostic_count": 0,
                    "owner_query_status": None,
                    "source_suppressions": [suppression],
                },
                {
                    **common_result,
                    "path": "bioseq/generated/first.cppm",
                    "generated": True,
                    "exit_code": 1,
                    "diagnostics": diagnostics[:2],
                    "diagnostic_count": 2,
                    "ninja_owner_candidates": ["codegen/highway/index.cppm"],
                    "owner_query_status": "ninja-authored-inputs",
                    "source_suppressions": [],
                },
                {
                    **common_result,
                    "path": "bioseq/generated/second.cppm",
                    "generated": True,
                    "exit_code": 2,
                    "diagnostics": diagnostics[2:],
                    "diagnostic_count": 2,
                    "heuristic_owner_candidates": [
                        "cmake/bioseq_sql_codegen.cmake"
                    ],
                    "owner_query_status": "ninja-exit-1",
                    "source_suppressions": [],
                },
            ]
            report_path.write_text(
                json.dumps(
                    {
                        "schema_version": 2,
                        "scope": "focused",
                        "available_count": 5,
                        "available_path_count": 5,
                        "available_authored_count": 2,
                        "available_authored_path_count": 2,
                        "available_generated_count": 3,
                        "available_generated_path_count": 3,
                        "selected_count": 3,
                        "selected_path_count": 3,
                        "authored_count": 1,
                        "authored_path_count": 1,
                        "generated_count": 2,
                        "generated_path_count": 2,
                        "optimization_verified_count": 5,
                        "optimization_policy": {
                            "optimization_flags": ["-O3"],
                            "ndebug_required": True,
                        },
                        "source_suppression_count": 1,
                        "diagnostic_count": 4,
                        "failure_count": 2,
                        "results": results,
                        "compile_databases": [
                            {
                                "path": "compile_commands.json",
                                "sha256": "d" * 64,
                            }
                        ],
                        "compile_database_contributions": {
                            "compile_commands.json": 5
                        },
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
            full_report = json.loads(report_path.read_text(encoding="utf-8"))
            full_report["scope"] = "full"
            report_path.write_text(json.dumps(full_report), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "cover every available"):
                build_worker._clang_tidy_report_summary(
                    report_path,
                    expected_scope="full",
                )

            authenticated_full = json.loads(json.dumps(full_report))
            authenticated_full.update(
                {
                    "available_count": 3,
                    "available_path_count": 3,
                    "available_authored_count": 1,
                    "available_authored_path_count": 1,
                    "available_generated_count": 2,
                    "available_generated_path_count": 2,
                    "optimization_verified_count": 3,
                }
            )
            authenticated_full["compile_databases"].append(
                {
                    "path": "codegen/generator-build/compile_commands.json",
                    "sha256": "e" * 64,
                }
            )
            authenticated_full["compile_database_contributions"] = {
                "compile_commands.json": 2,
                "codegen/generator-build/compile_commands.json": 1,
            }
            authenticated_full["results"][2]["compile_database"] = (
                "codegen/generator-build/compile_commands.json"
            )
            report_path.write_text(
                json.dumps(authenticated_full),
                encoding="utf-8",
            )
            full_summary = build_worker._clang_tidy_report_summary(
                report_path,
                expected_scope="full",
            )

            missing_generator_contribution = json.loads(
                json.dumps(authenticated_full)
            )
            missing_generator_contribution["compile_database_contributions"] = {
                "compile_commands.json": 3,
                "codegen/generator-build/compile_commands.json": 0,
            }
            missing_generator_contribution["results"][2]["compile_database"] = (
                "compile_commands.json"
            )
            report_path.write_text(
                json.dumps(missing_generator_contribution),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "generator database"):
                build_worker._clang_tidy_report_summary(
                    report_path,
                    expected_scope="full",
                )

            full_report["scope"] = "focused"
            full_report["results"][0]["exit_code"] = 1
            report_path.write_text(json.dumps(full_report), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "aggregates"):
                build_worker._clang_tidy_report_summary(
                    report_path,
                    expected_scope="focused",
                )

        self.assertEqual(summary["selected_count"], 3)
        self.assertEqual(summary["available_count"], 5)
        self.assertEqual(summary["optimization_verified_count"], 5)
        self.assertEqual(summary["failure_count"], 2)
        self.assertEqual(summary["compile_database_count"], 1)
        self.assertEqual(summary["toolchain"], {"major": 22})
        self.assertEqual(full_summary["compile_database_count"], 2)


if __name__ == "__main__":
    unittest.main()
