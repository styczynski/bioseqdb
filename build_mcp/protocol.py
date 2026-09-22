"""MCP schemas, framing, and request dispatch."""

from __future__ import annotations

from typing import List, Dict, Set, Optional, Any, Mapping

import argparse
import base64
import contextlib
import ctypes
import errno
import fcntl
import hashlib
import json
import math
import os
import platform as host_platform
import re
import select
import secrets
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass, field
from pathlib import Path

from build_presets import (
    ALL_ALPHABETS_UNIT_INTEGRATION_MODE,
    BENCH_CLI_MODES,
    CATCH_FILTER_MODES,
    FIXED_BUILD_ONLY_MODES,
    LINT_FOCUSED_MODE,
    LINT_FULL_MODE,
    LINT_MODES,
    POSTGRES_ERROR_SAFETY_MODE,
    REMOTE_RUN_BUILD_MODES,
    VALID_MODES,
    _MAX_CATCH2_TEST_SPECS,
    _MAX_CATCH2_TEST_SPEC_LENGTH,
    _MAX_LINT_PATHS,
    _MAX_LINT_PATH_LENGTH,
    get_modes_short_documentation,
)
from build_worker import (
    INSPECT_TARGETS,
    MAX_BUILD_PHASE_TIMEOUT_SEC,
    tool_inspect,
    tool_import_e2e_artifacts,
    tool_export_e2e_artifacts,
    tool_papers_build,
    tool_papers_inspect,
    tool_start_build,
    tool_status,
)
from process_state import (
    _log,
)
from remote import (
    REMOTE_BUILD_PRESET,
    REMOTE_BUILD_PRESETS,
    REMOTE_MANAGE_OPS,
    REMOTE_OP_TIMEOUTS,
    tool_remote_debug,
    tool_remote_git,
    tool_remote_manage,
    tool_remote_run,
    tool_remote_snapshot,
    tool_remote_ssh,
    tool_remote_status,
    tool_remote_status_ps,
)
from subagent import tool_subagent



import bootstrap
from inspect_tool import (
    MATCHES_WARN_THRESHOLD,
    MAX_INSPECT_MATCHES,
    MAX_INSPECT_SNIPPET_LINES,
    MAX_RETAINED_SNIPPET_BYTES,
    MAX_TOTAL_RETAINED_SNIPPET_BYTES,
)



PROTOCOL_VERSION = "2024-11-05"

SERVER_INFO = {"name": "bioseq-build-mcp", "version": "1.1.0"}

TOOL_DEFINITIONS = [
    {
        "name": "inspect_postgres_crash",
        "description": "Inspect the saved Ubuntu PostgreSQL Apport crash through gdb in read-only postmortem mode. No live attach, builds, or inferior execution. Report timestamp is included; historical cores need not represent the latest failure.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "export_e2e_artifacts",
        "description": "Read-only validated frozen E2E runtime and dependency inventory for copying to an identical machine family; excludes compiled objects and databases.",
        "inputSchema": {"type": "object", "properties": {"build_receipt_id": {"type": "string"}},
                        "required": ["build_receipt_id"], "additionalProperties": False},
    },
    {
        "name": "import_e2e_artifacts",
        "description": "Validate exact copied frozen E2E artifacts against an original build receipt; register an explicit import receipt without building or benchmarking.",
        "inputSchema": {"type": "object", "properties": {"source_receipt": {"type": "object"},
                        "check_only": {"type": "boolean", "default": False}},
                        "required": ["source_receipt"], "additionalProperties": False},
    },
    {
        "name": "external_aligners",
        "description": "Build or run only the isolated external-aligner benchmark on its provisioned AWS host.",
        "inputSchema": {"type": "object", "properties": {
            "action": {"type": "string", "enum": ["status", "build", "run", "publish", "cancel", "recover"]},
            "tool": {"type": "string", "enum": ["blastn", "mmseqs2", "winnowmap", "lexicmap", "harness"]},
            "config": {"type": "string"}
        }, "additionalProperties": False},
    },
    {
        "name": "start_build",
        "description": (
            "Schedule a build/test/benchmark run. The build runs detached so "
            "it survives MCP server restarts. If a run of the same lock group "
            "or resolved build tree is already in progress (non-SQL benchmarks "
            "may run with tests when their trees differ; "
            "unit-integration and benchmark-e2e serialize because both install "
            "and restart the shared PostgreSQL extension) "
            "returns the current status instead of starting a duplicate. "
            f"{get_modes_short_documentation()} "
            f"\nNote: If you want to iterate quickly then use build_only parameter."
            f"\n\nBuild tree selection (`debug` parameter):"
            f"\n  - debug=false (DEFAULT): uses the mode family's fast preset "
            f"(core, isolated tests, cache-only, or combined provider). "
            f"-O3 + NDEBUG only, no debug info, no "
            f"per-TU temps. This is the fast everyday build — preferred for "
            f"every normal iteration, including running tests and benches."
            f"\n  - debug=true: uses that family's corresponding isolated "
            f"debug preset. Same -O3 + NDEBUG, "
            f"plus -g (DWARF "
            f"symbols) and -save-temps=obj (writes per-TU .s/.ii/.bc beside "
            f"each .o). REQUIRED by the `inspect` tool: it pulls disassembly "
            f"from objdump --line-numbers (needs DWARF) and pre-LTO "
            f"per-TU listings (needs .s files). ~2x slower to compile, "
            f"~2-3x peak RSS per TU, ~3x disk usage versus debug=false. "
            f"Only set this when you intend to call `inspect` afterwards. "
            f"The two build trees are kept on separate disk paths so "
            f"toggling `debug` does not invalidate the other tree's ninja "
            f"graph — switching is cheap once both have been populated once."
            f"\n\nFocused tests: for Catch correctness modes "
            f"({', '.join(sorted(CATCH_FILTER_MODES))}), pass Catch2 test-spec "
            f"expressions in filter.tags. They are OR-combined and applied to "
            f"every executable in the mode; a binary with no matching test is "
            f"allowed so tags local to one executable work as intended."
            f"\n\nFocused lint: {LINT_FOCUSED_MODE} requires filter.paths "
            "with exact repository-relative authored units or build-relative "
            "generated units. Generated diagnostics include authored Ninja "
            "owner candidates. lint-full accepts no filter."
            f"\nGuarded build-only calls may set fresh=true and "
            f"build_timeout_sec up to {MAX_BUILD_PHASE_TIMEOUT_SEC}; "
            "TERM begins one minute before that hard cutoff. Runtime-only "
            "calls require a matching successful build receipt. Lint modes "
            "use their mode-wide analysis deadline and reject build_timeout_sec."
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "mode": {
                    "type": "string",
                    "enum": sorted(VALID_MODES),
                    "description": "Which run to start.",
                },
                "build_only": {
                   "type": "boolean",
                   "description": (
                       "Build the targets without running tests, benchmarks, "
                       "or other runtime commands. Useful for a fast compile "
                       "check. Required for fixed local compile-gate modes: "
                       f"{', '.join(sorted(FIXED_BUILD_ONLY_MODES))}."
                   ),
                   "default": False,
                },
                "fresh": {
                    "type": "boolean",
                    "default": False,
                    "description": (
                        "Build-only. Configure a new isolated MCP-owned tree "
                        "under build/mcp without deleting or reusing the "
                        "canonical build tree."
                    ),
                },
                "build_timeout_sec": {
                    "type": "integer",
                    "minimum": 60,
                    "maximum": MAX_BUILD_PHASE_TIMEOUT_SEC,
                    "description": (
                        "Build-only configure/generate/compile/link cutoff. "
                        "At one minute before the cutoff the exact worker "
                        "descendant groups receive TERM; validated survivors "
                        "receive KILL by the cutoff. Lint modes reject this "
                        "build-phase-only control."
                    ),
                },
                "run_only": {
                    "type": "boolean",
                    "default": False,
                    "description": (
                        "Skip configure/generate/compile/link completely and "
                        "run only install/tests/benchmarks from the exact "
                        "tree pinned by reuse_build_receipt."
                    ),
                },
                "reuse_build_receipt": {
                    "type": "string",
                    "minLength": 1,
                    "description": (
                        "Opaque id returned by a successful build-only call. "
                        "Required with run_only and rejected otherwise."
                    ),
                },
                "debug": {
                   "type": "boolean",
                   "description": (
                       "Select the mode family's isolated debug build tree "
                       "(core, test, cache-only, or combined). Adds -g + "
                       "-save-temps=obj "
                       "so the `inspect` tool can extract per-function "
                       "disassembly and pre-LTO assembly listings. ~2x slower "
                       "and uses ~3x more memory per TU than the default "
                       "fast build — only set this when you actually need "
                       "the inspect artefacts. Default false (uses the "
                       "corresponding fast preset)."
                   ),
                   "default": False,
                },
                "filter": {
                    "type": "object",
                    "additionalProperties": False,
                    "description": (
                        "Mode-dependent subset selection. For Catch correctness modes "
                        f"({', '.join(sorted(CATCH_FILTER_MODES))}), only tags is accepted: "
                        "each value is a bounded Catch2 test-spec expression; the values are "
                        "OR-combined and applied to every executable in the mode. For CLI "
                        f"benchmark modes ({', '.join(BENCH_CLI_MODES)}), tags/benchmarks/cases "
                        "map to repeatable --filter flags; arrays are OR-sets and axes AND "
                        f"together. {LINT_FOCUSED_MODE} accepts only paths. Other modes do "
                        "not accept filter. Omit it for the full run."
                    ),
                    "properties": {
                        "tags":       {"type": "array", "items": {"type": "string", "minLength": 1}, "minItems": 1,
                                       "description": "Catch correctness modes: Catch2 test-spec expressions. Bench modes: benchmark tag OR-set."},
                        "cases":      {"type": "array", "items": {"type": "string", "minLength": 1}, "minItems": 1,
                                       "description": "Within selected benchmarks, run only these matrix.over cases (algorithm names)."},
                        "benchmarks": {"type": "array", "items": {"type": "string", "minLength": 1}, "minItems": 1,
                                       "description": (
                                           "Run only these benchmark keys. (Maps to the "
                                           "bench CLI's singular `benchmark=`.)"
                                       )},
                        "paths": {
                            "type": "array",
                            "minItems": 1,
                            "maxItems": _MAX_LINT_PATHS,
                            "items": {
                                "type": "string",
                                "minLength": 1,
                                "maxLength": _MAX_LINT_PATH_LENGTH,
                                "pattern": (
                                    "^(?!/)(?!.*(?:^|/)\\.\\.(?:/|$))"
                                    "(?!.*//)(?!.*\\\\).+\\.(?:c|cc|cpp|cppm|cxx)$"
                                ),
                            },
                            "description": (
                                "lint-focused only. Exact normalized source-root-relative "
                                "authored units or build-root-relative generated units."
                            ),
                        },
                    }
                },
                "bench_e2e_config": {
                    "type": "string",
                    "minLength": 1,
                    "description": (
                        "Local benchmark-e2e only. Repository-root-relative or "
                        "absolute path to an existing repository-contained .json "
                        "configuration, exposed to the executable through "
                        "BIOSEQ_E2E_CONFIG_OVERRIDE."
                    ),
                },
                "debug_report": {
                    "type": "boolean",
                    "default": False,
                    "description": (
                        "Bench-only. Pass --debug to the bench binary: stream a crash-resilient "
                        "report_debug_<N>.jsonl (one JSON event per case) next to report_<N>.json. "
                        "Requires a non-empty `filter` (debug is for targeted runs). NOTE: this is "
                        "NOT the build-tree `debug` flag above. Supplying this property, even "
                       "as false, is rejected for Catch correctness modes."
                    )
                }
            },
            "allOf": [
                {
                    "if": {
                        "properties": {
                            "mode": {"const": ALL_ALPHABETS_UNIT_INTEGRATION_MODE},
                        },
                        "required": ["mode"],
                    },
                    "then": {
                        "properties": {"debug": {"const": False}},
                    },
                },
                {
                    "if": {
                        "properties": {
                            "mode": {"const": LINT_FOCUSED_MODE},
                        },
                        "required": ["mode"],
                    },
                    "then": {
                        "required": ["filter"],
                        "properties": {
                            "filter": {
                                "required": ["paths"],
                                "not": {
                                    "anyOf": [
                                        {"required": ["tags"]},
                                        {"required": ["benchmarks"]},
                                        {"required": ["cases"]},
                                    ],
                                },
                            },
                        },
                    },
                    "else": {
                        "properties": {
                            "filter": {
                                "not": {"required": ["paths"]},
                            },
                        },
                    },
                },
                {
                    "if": {
                        "properties": {
                            "mode": {"enum": sorted(LINT_MODES)},
                        },
                        "required": ["mode"],
                    },
                    "then": {
                        "required": ["build_only"],
                        "properties": {
                            "build_only": {"const": True},
                        },
                        "not": {
                            "anyOf": [
                                {"required": ["debug"]},
                                {"required": ["debug_report"]},
                                {"required": ["bench_e2e_config"]},
                                {"required": ["build_timeout_sec"]},
                                {"required": ["run_only"]},
                                {"required": ["reuse_build_receipt"]},
                            ],
                        },
                    },
                },
                {
                    "if": {
                        "properties": {
                            "mode": {"const": LINT_FULL_MODE},
                        },
                        "required": ["mode"],
                    },
                    "then": {
                        "not": {"required": ["filter"]},
                    },
                },
                {
                    "if": {
                        "properties": {"run_only": {"const": True}},
                        "required": ["run_only"],
                    },
                    "then": {
                        "required": ["reuse_build_receipt"],
                        "not": {
                            "anyOf": [
                                {
                                    "properties": {
                                        "build_only": {"const": True},
                                    },
                                    "required": ["build_only"],
                                },
                                {"required": ["fresh"]},
                                {"required": ["build_timeout_sec"]},
                            ],
                        },
                    },
                    "else": {
                        "not": {"required": ["reuse_build_receipt"]},
                    },
                },
                {
                    "if": {
                        "anyOf": [
                            {
                                "properties": {"fresh": {"const": True}},
                                "required": ["fresh"],
                            },
                            {"required": ["build_timeout_sec"]},
                        ],
                    },
                    "then": {
                        "properties": {"build_only": {"const": True}},
                        "required": ["build_only"],
                    },
                },
                {
                    "if": {
                        "properties": {
                            "mode": {"enum": sorted(CATCH_FILTER_MODES)},
                        },
                        "required": ["mode"],
                    },
                    "then": {
                        "not": {"required": ["debug_report"]},
                        "properties": {
                            "filter": {
                                "not": {
                                    "anyOf": [
                                        {"required": ["benchmarks"]},
                                        {"required": ["cases"]},
                                    ]
                                },
                                "properties": {
                                    "tags": {
                                        "maxItems": _MAX_CATCH2_TEST_SPECS,
                                        "items": {
                                            "type": "string",
                                            "minLength": 1,
                                            "maxLength": _MAX_CATCH2_TEST_SPEC_LENGTH,
                                            "pattern": "^(?!-)[^\\u0000-\\u001F\\u007F]+$",
                                        },
                                    }
                                },
                            }
                        },
                    },
                },
                {
                    "if": {
                        "properties": {
                            "mode": {
                                "const": POSTGRES_ERROR_SAFETY_MODE,
                            },
                        },
                        "required": ["mode"],
                    },
                    "then": {
                        "not": {
                            "anyOf": [
                                {"required": ["debug"]},
                                {"required": ["filter"]},
                                {"required": ["debug_report"]},
                                {"required": ["bench_e2e_config"]},
                            ],
                        },
                    },
                },
            ],
            "required": ["mode"],
        },
    },
    {
        "name": "status",
        "description": (
            "With no arguments, report the closed path-free local activity "
            "aggregate and free disk. With `mode`, report the status of a "
            "previously scheduled run. "
            "While running, returns current step + predicted finish. After "
            "completion, returns total duration, log file path, and (for "
            "bench modes) the path to the new framework report_<N>.json under "
            "reporting/. "
            "Focused unit/integration runs also display the "
            "exact Catch2 args persisted for all four executables."
        ),
        "inputSchema": {
            "oneOf": [
                {
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {},
                },
                {
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "mode": {
                            "type": "string",
                            "enum": sorted(VALID_MODES),
                            "description": "Which run to query.",
                        },
                    },
                    "required": ["mode"],
                },
            ],
        },
    },
    {
        "name": "inspect",
        "description": (
            "Inspect a function in the latest build. Returns a "
            "JSON document with: demangled symbols matching the substring; "
            "the linked-binary disassembly per match (post-LTO ground "
            "truth); the per-TU compiler-emitted .s listing block (pre-LTO "
            "compiler intent, with .loc source-line annotations); an "
            "instruction histogram (total / SIMD / branches / loads / "
            "stores / atomic_loads / atomic_stores / neon_struct_loadstore); "
            "and a sweep of inline sites where the function appears as "
            ".loc directives or DEBUG_VALUE annotations in other TUs' .s "
            "files (useful for [[gnu::always_inline]] functions that have "
            "no standalone symbol). Full post-LTO objdump text is written "
            "under /tmp/bioseq_inspect/. Pre-LTO assembly listings remain "
            "in the selected build tree; each match returns that build path "
            "and exact line range. JSON snippets are byte-bounded, while "
            "those file paths preserve access to complete evidence.\n\n"
            "Fast core/test/cache trees do "
            "NOT carry per-TU .s/.bc files or DWARF. Call `start_build` for "
            "the requested target's mode with debug=true first to produce "
            "the matching isolated debug tree. If you pass "
            "no `debug` argument here, the tool auto-detects: the debug "
            "tree wins when its build.ninja exists, otherwise we fall "
            "back to the fast tree (the response's "
            "`build_metadata.debug_tree_used` field records which one was "
            "used; if it is false and you see 'no per-TU `.s` files' "
            "warnings, schedule a `start_build mode=… debug=true` and "
            "re-run inspect).\n\n"
            "Use this to compare two implementations of the same function "
            "(e.g. before/after a refactor) at the instruction level to "
            "spot vectorisation regressions, register spills, "
            "instruction-count deltas, branch-pattern changes, etc.\n\n"
            "Recommended: pass a fully-qualified namespace prefix as "
            "`function_name` (e.g. "
            "'void bioseq::simd::N_NEON::collect_minimizers_two_stack_nthash_simd@') "
            "rather than the bare function name; bare names like 'hash' "
            "match thousands of unrelated symbols (std::hash, "
            "boost::hash, lambdas, etc.). The `summary.total_matches_before_cap` "
            "field surfaces the un-capped count so you can decide whether "
            "to retry with a tighter prefix; a top-level warning fires "
            f"when the count exceeds {MATCHES_WARN_THRESHOLD}.\n\n"
            "Latency and observability: when available, ripgrep first scans "
            "compiler listings in native bounded memory for exact symbol "
            "labels plus an explicitly supplied source_basename; only "
            "candidate listings enter the bounded Python parser. One "
            "assembly-phase deadline covers both rg and every fallback "
            "streaming chunk. If rg is absent or fails while time remains, "
            "the parser streams every listing once; if the shared deadline "
            "is exhausted it stops safely and reports incomplete evidence. "
            "Fallback latency is proportional to total .s bytes. The lazy llc fallback can add "
            "up to ~25s per selected .bc on a cold tree and reuses a sibling "
            ".s until the .bc mtime changes. build_metadata.tools.rg records "
            "the prefilter tool. summary.asm_listings_discovered, "
            "asm_listings_candidates, asm_listings_parsed, "
            "asm_listings_unparsed, "
            "asm_listing_bytes_streamed, asm_listing_scan_duration_ms and "
            "asm_listing_scan_incomplete expose actual work/completeness. "
            "Summary also reports snippet byte limits/truncation and honest "
            "source-state overflow/entries-cleared/records-dropped counts; "
            "warnings identify every clamp or fail-closed omission.\n\n"
            "Caveats:\n"
            "  - llvm-objdump symbolises every branch target by finding "
            "the closest *named* symbol with a smaller address — even when "
            "the offset is huge (e.g. `bl 0x1004f5ec0 <_write+0x1004f5ec0>` "
            "is NOT a call to _write, just the closest libsystem anchor). "
            "Treat `<symbol+offset>` annotations with suspicion when the "
            "offset is > 0x10000.\n"
            "  - macOS+ThinLTO drops inline-frame source line info, so the "
            "post-LTO disassembly's --line-numbers output is sparse. The "
            "pre-LTO `.s` view (assembly_listing_*) preserves source "
            "mapping via .loc directives.\n"
            "  - At clang -O3 the default -discard-value-names strips "
            "DEBUG_VALUE annotations for templated/inlined code, so "
            "inline_sites[].debug_value_hits is often 0. The .loc "
            "directive count is the primary signal.\n"
            "  - Post-LTO extraction is not a whole-DSO disassembly: objdump "
            "is filtered to each exact matched symbol and has a 120-second "
            "per-process timeout; its c++filt pass has a 60-second timeout. "
            "The one whole-DSO nm/c++filt symbol enumeration is separately "
            "time-bounded to 60 seconds per process."
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "function_name": {
                    "type": "string",
                    "description":
                        "Substring matched against demangled C++ names. "
                        "Examples: "
                        "'void bioseq::simd::N_NEON::collect_minimizers_two_stack_nthash_simd@' "
                        "(prefix-anchored, recommended), or "
                        "'collect_minimizers_two_stack_nthash_simd' "
                        "(broader, may match many lambdas/closures). "
                        "Use 'address:0x...' to select the containing text symbol "
                        "for a known unrelocated instruction address, or "
                        "'file-offset:0x...' for a Linux kernel fault file offset.",
                },
                "target": {
                    "type": "string",
                    "enum": sorted(INSPECT_TARGETS),
                    "default": "bioseq_benchmark",
                    "description":
                        "Which target binary or extension DSO to inspect. The "
                        "target map resolves its core, test, cache-only, or "
                        "combined preset family and binary subdirectory.",
                },
                "max_matches": {
                    "type": "integer",
                    "default": 4,
                    "minimum": 1,
                    "maximum": MAX_INSPECT_MATCHES,
                    "description":
                        "Cap on the number of returned symbols. Largest "
                        "first (by computed size); out-of-range direct calls "
                        "are clamped with a warning.",
                },
                "snippet_lines": {
                    "type": "integer",
                    "default": 200,
                    "minimum": 1,
                    "maximum": MAX_INSPECT_SNIPPET_LINES,
                    "description":
                        "Maximum lines of asm/disasm to inline in JSON, also "
                        f"bounded to {MAX_RETAINED_SNIPPET_BYTES} bytes per "
                        f"snippet and {MAX_TOTAL_RETAINED_SNIPPET_BYTES} "
                        "bytes total. Full objdump text is under /tmp; full "
                        "assembly listings remain at returned build-tree "
                        "paths.",
                },
                "source_basename": {
                    "type": "string",
                    "description":
                        "Basename of the source file (e.g. "
                        "'two_stacks_minimizers.cppm') used to filter "
                        "the inline-site .loc sweep. When omitted, that sweep "
                        "is skipped with a warning; exact standalone pre-LTO "
                        "symbol-block inspection remains enabled.",
                },
                "max_llc_bc_files": {
                    "type": "integer",
                    "default": 4,
                    "minimum": 0,
                    "description":
                        "Cap on the number of .bc files we llc-compile "
                        "for the pre-LTO view when no .s is present. Each "
                        "conversion costs ~25s on the bench-main TU. "
                        "Raise to widen the search; set to 0 to disable.",
                },
                "llc_on_bc": {
                    "type": "boolean",
                    "default": True,
                    "description":
                        "Whether to run the llc-on-.bc fallback when no "
                        ".s files are present (clang ThinLTO case). "
                        "Disabling it prevents only .bc-to-.s synthesis; it "
                        "does not disable scanning existing .s evidence.",
                },
                "debug": {
                    "type": "boolean",
                    "description":
                        "Force the inspect tool to read from the requested "
                        "target's matching family. Core, "
                        "test, cache-only, and combined targets each retain "
                        "their own fast/debug trees. true selects debug, "
                        "false the fast tree. Omit to auto-detect: "
                        "prefer the debug tree when available, fall back "
                        "to fast. Set this explicitly only when you need "
                        "to pin the tree (e.g. compare debug vs fast).",
                },
                "reuse_build_receipt": {
                    "type": "string",
                    "minLength": 1,
                    "description": (
                        "Select and validate an exact successful build tree "
                        "instead of auto-discovering a recent tree. A debug "
                        "receipt is required unless historical_artifact_only "
                        "is true. The receipt mode must own the requested "
                        "target."
                    ),
                },
                "historical_artifact_only": {
                    "type": "boolean",
                    "default": False,
                    "description": (
                        "Inspect only the exact artifact pinned by "
                        "reuse_build_receipt, without requiring its recorded "
                        "source graph to match the current worktree. This "
                        "read-only historical mode is required for a "
                        "non-debug receipt, disables .bc-to-.s synthesis, "
                        "and never proves the current worktree. Receipt seal, "
                        "commit, configuration, target ownership, and exact "
                        "artifact hashes remain validated."
                    ),
                },
            },
            "required": ["function_name"],
        },
    },
    {
        "name": "papers-build",
        "description": (
            "Configure `main/papers/CMakeLists.txt` with CMake and build only "
            "the `bioseqdb_thesis` target sourced from "
            "`main/papers/src/content/bioseqdb/thesis`. The build runs in a "
            "detached worker so a clean TeX environment can finish after the "
            "MCP request returns. Call with action='start' to schedule a new "
            "build (or get the currently-running build), then poll with "
            "action='status'. A completed response includes the generated PDF "
            "path, digest, size, exact CMake commands, log path, and log tail."
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "action": {
                    "type": "string",
                    "enum": ["start", "status"],
                    "default": "start",
                    "description": (
                        "Start a new thesis build, or poll the most recently "
                        "scheduled build."
                    ),
                },
            },
            "required": [],
        },
    },
    {
        "name": "papers-inspect",
        "description": (
            "Inspect the PDF produced by papers-build. With no `pages`, renders "
            "up to 80 pages as lightweight overview PNGs and returns them "
            "directly as MCP image content in page order. With `pages=[...]`, "
            "renders up to 20 selected pages at higher resolution for focused "
            "visual review. The leading JSON manifest includes stable local "
            "image paths, PDF metadata, source-staleness detection, LaTeX "
            "overfull/underfull-box and reference warnings, Poppler text "
            "bounding-box checks, empty-text-page hints, and explicit agent "
            "review instructions. Automated diagnostics supplement, but do "
            "not replace, visual confirmation of clipping, overlap, margins, "
            "figure/table placement, blank areas, and broken glyphs."
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "pages": {
                    "type": "array",
                    "items": {"type": "integer", "minimum": 1},
                    "minItems": 1,
                    "maxItems": 20,
                    "uniqueItems": True,
                    "description": (
                        "Optional one-based page numbers for focused review. "
                        "Omit to render the whole-document overview."
                    ),
                },
                "dpi": {
                    "type": "integer",
                    "minimum": 36,
                    "maximum": 300,
                    "description": (
                        "Raster resolution. Defaults to 48 for the overview "
                        "and 160 when explicit pages are supplied."
                    ),
                },
                "include_images": {
                    "type": "boolean",
                    "default": True,
                    "description": (
                        "Return rendered PNGs as MCP image content. Stable "
                        "image paths remain in the JSON manifest when false "
                        "or when the 8 MiB response cap is reached."
                    ),
                },
            },
            "required": [],
        },
    },
    {
        "name": "subagent",
        "description": (
            "Persistent multi-account subagent workforce. A repository-scoped registry and "
            "message bus are atomically written under one cross-process lock and shared by ROOT "
            "and every recursively spawned agent. Numeric IDs are monotonic and never reused; "
            "descriptions, server-derived parents, labels, lifecycle, quotas, logs, metrics, and "
            "bus messages survive MCP restarts. Agent terminals use a private tmux socket and "
            "survive an MCP crash. Caller ID plus an opaque capability are inherited through the "
            "environment. An unmarked caller defaults to ID 0, but only the oldest validated "
            "unmarked process can be ROOT; managed-process ancestry blocks marker deletion. These "
            "controls are cooperative defense-in-depth, not a hostile security boundary, because "
            "every --yolo agent runs as the same OS user.\n\n"
            "Commands:\n"
            "- status: lazily sample each available account's weekly quota no more than once per "
            "five minutes through the structured Codex endpoint that backs /status, then return "
            "weekly percent remaining, every registered agent, and its "
            "account. Metrics include direct/recursive child counts (ever and live); tokens for "
            "self, self plus direct children, and the full recursive subtree; interruption count; "
            "and time since state change, last interruption, newest CLI log, and newest assistant "
            "message. Unknown token data is explicitly incomplete, never zero. After six valid "
            "five-minute samples, status reports average depletion plus per-account and aggregate "
            "linear exhaustion estimates. More than 10 interruptions warns that resumes resend "
            "growing context.\n"
            "- create: atomically reserve budget/account/ID before launching a tmux-backed agent. "
            "instructions are the task; description is human-readable workforce metadata of at "
            "least 200 characters after surrounding whitespace is removed.\n"
            "- interrupt: send Escape to a WORKING Codex agent. Without instructions it becomes "
            "INTERRUPTED; meaningful instructions receive the coordination prefix and transition "
            "it to WORKING. Cursor print mode is one-shot: the controller requires its documented "
            "system/init chat ID, sends Escape, briefly observes whether the headless process "
            "exits, then atomically replaces it with an owned idle supervisor or an exact-chat "
            "resume. Each Cursor invocation has a generation-specific stream log, also copied to "
            "the agent's aggregate log, so delayed output from an earlier process cannot prove a "
            "new resume. If headless Cursor does not acknowledge Escape, operation_state and warnings "
            "say FORCED_STOP instead of pretending interactive cancellation. Without a chat ID, "
            "the replacement is rejected without stopping current work. Bare and instruction-"
            "bearing interrupts both use the five-minute quota cache and return warnings when "
            "quota is stale, unknown, or below 3%.\n"
            "- logs: read a sanitized tail of at most 2,000 lines and 60,000 characters from any "
            "agent's persistent CLI log; logs are open regardless of tier.\n"
            "- kill: terminate the owned process while preserving record, logs, metrics, vendor "
            "conversation ID, and KILLED state. Kill is the sole operation allowed to supersede a "
            "stale cleanup-required transaction, so ambiguous delivery never strands paid budget.\n"
            "- repurpose: update description/label/ceiling and submit a new assignment. KILLED "
            "agents resume the exact vendor conversation on the original account. If that account "
            "is exhausted, the tool honestly rejects resume and recommends load-balanced create.\n"
            "- bus-write / bus-read: append to the locked shared JSON bus or read its newest "
            "messages (25 by default); authorship is derived from caller identity.\n\n"
            "Public lifecycle has exactly four states. ROOT is ID 0 with no managed process. "
            "WORKING means a launch/resume or active assignment is consuming budget. INTERRUPTED "
            "means an owned runtime is live but idle after explicit interruption or natural Codex "
            "turn completion and still consumes budget (Cursor uses an idle supervisor). KILLED "
            "means no live managed provider process, "
            "including requested kill or normal completion of one-shot Cursor; operation_state "
            "records the precise completion/failure reason. Killed records may retain resumable "
            "vendor context.\n\n"
            "Tiers and fixed live-budget costs: specialist = gpt-5.6-sol/max (100) for the hardest "
            "architecture/research/implementation; senior = gpt-5.6-sol/high (70) for difficult "
            "design, review, and implementation; mid = gpt-5.6-terra/high (50) for ordinary "
            "nontrivial work; contractor = Cursor gpt-5.5 with Cursor-selected dynamic reasoning "
            "effort (45) for substantial bounded work and consumes a separate Cursor account "
            "quota rather than Codex account quota; intern = gpt-5.6-terra/medium (22) for bounded "
            "simple work; and "
            "contractor-intern = fast, cheap, weaker Cursor Composer 2.5 (canonical Cursor CLI "
            "model ID composer-2.5; cost 1) for simplistic or "
            "mechanical tasks only. Use contractor-intern first only when the work is truly "
            "trivial, then escalate to intern if needed. The hardcoded simultaneous budget is "
            "4000; it is adjustable in code but callers must never alter it to force allocation. "
            "WORKING and INTERRUPTED both count. Codex allocation chooses fewer live agents, then "
            "more weekly quota. Below 3%, an account gets no new agents, but existing work may "
            "continue even at 0%; avoid interruptions because the next resume can fail.\n\n"
            "On create, omitted/null highest_spawn_tier defaults to the new agent's tier; an "
            "explicit value can narrow or raise it only within the caller's authority. On "
            "repurpose, omission preserves the ceiling and explicit null resets it to the target's "
            "own tier. This authority governs create, interrupt, kill, and repurpose; status, logs, "
            "and bus-read are open. Targeted commands default agent_id to 0, whose mutation is "
            "rejected. Repurpose label omission preserves the old label. Labels general-purpose, "
            "review, audit, and implementation are display-only matters of taste. create, "
            "repurpose, and instruction-bearing interrupt prepend only a compact coordination "
            "prefix with ID, tier, bus count, effective tiers, and anti-passive-delegation advice.\n\n"
            "ROOT MUST use quota and activity metrics to prevent delegation degradation. An agent "
            "that waits while recursively delegating makes interns do the substantive work and "
            "degrades quality; uncontrolled recursion burns quota on duplicates. Specialists and "
            "seniors may delegate justified parallel slices but must do most work themselves. "
            "Lower tiers should spawn only in critical cases. Several specialists may be justified "
            "for a genuinely complex refactor plan; an army for a minor utility edit is wasteful."
        ),
        "inputSchema": {
            "type": "object",
            "oneOf": [
                {
                    "title": "status",
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "command": {"type": "string", "const": "status"},
                    },
                    "required": ["command"],
                },
                {
                    "title": "create",
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "command": {"type": "string", "const": "create"},
                        "reasoning_effort": {
                            "type": "string",
                            "enum": ["contractor-intern", "intern", "contractor", "mid", "senior", "specialist"],
                            "description": "Portable workforce tier; mapped to the documented model and vendor effort. The contractor tier uses Cursor gpt-5.5 with dynamic effort and consumes a separate Cursor account quota.",
                        },
                        "instructions": {
                            "type": "string", "minLength": 1, "maxLength": 100000,
                            "description": "Concrete task sent to the new agent; whitespace-only text is rejected at runtime.",
                        },
                        "description": {
                            "type": "string", "minLength": 200, "maxLength": 8000,
                            "description": "Human-readable purpose shown in status; at least 200 characters after trimming.",
                        },
                        "highest_spawn_tier": {
                            "type": ["string", "null"],
                            "enum": [None, "contractor-intern", "intern", "contractor", "mid", "senior", "specialist"],
                            "description": "Omitted/null defaults to the new agent's tier; explicit values cannot exceed caller authority.",
                        },
                        "label": {
                            "type": "string",
                            "enum": ["general-purpose", "review", "audit", "implementation"],
                            "default": "general-purpose",
                            "description": "Display-only organizational label with no scheduling or permission effect.",
                        },
                    },
                    "required": ["command", "reasoning_effort", "instructions", "description"],
                },
                {
                    "title": "interrupt",
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "command": {"type": "string", "const": "interrupt"},
                        "agent_id": {
                            "type": "integer", "minimum": 0, "default": 0,
                            "description": "Target numeric ID; omitted means ROOT (0), whose mutation is rejected.",
                        },
                        "instructions": {
                            "type": "string", "minLength": 1, "maxLength": 100000,
                            "description": "Optional new assignment; omission is Escape-only and whitespace-only text is rejected.",
                        },
                    },
                    "required": ["command"],
                },
                {
                    "title": "logs",
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "command": {"type": "string", "const": "logs"},
                        "agent_id": {"type": "integer", "minimum": 0, "default": 0},
                        "lines": {
                            "type": "integer", "minimum": 1, "maximum": 2000, "default": 200,
                            "description": "Tail line count; the response is additionally capped at 60,000 characters.",
                        },
                    },
                    "required": ["command"],
                },
                {
                    "title": "kill",
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "command": {"type": "string", "const": "kill"},
                        "agent_id": {"type": "integer", "minimum": 0, "default": 0},
                    },
                    "required": ["command"],
                },
                {
                    "title": "repurpose",
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "command": {"type": "string", "const": "repurpose"},
                        "agent_id": {"type": "integer", "minimum": 0, "default": 0},
                        "instructions": {
                            "type": "string", "minLength": 1, "maxLength": 100000,
                            "description": "New assignment submitted in the retained context.",
                        },
                        "description": {
                            "type": "string", "minLength": 200, "maxLength": 8000,
                            "description": "Replacement human-readable purpose shown in status.",
                        },
                        "highest_spawn_tier": {
                            "type": ["string", "null"],
                            "enum": [None, "contractor-intern", "intern", "contractor", "mid", "senior", "specialist"],
                            "description": "Omission preserves the ceiling; null resets it to the target's own tier.",
                        },
                        "label": {
                            "type": "string",
                            "enum": ["general-purpose", "review", "audit", "implementation"],
                            "description": "Display-only label; omission preserves the existing label.",
                        },
                    },
                    "required": ["command", "instructions", "description"],
                },
                {
                    "title": "bus-read",
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "command": {"type": "string", "const": "bus-read"},
                        "message_count": {"type": "integer", "minimum": 1, "maximum": 200, "default": 25},
                    },
                    "required": ["command"],
                },
                {
                    "title": "bus-write",
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "command": {"type": "string", "const": "bus-write"},
                        "message": {"type": "string", "minLength": 1, "maxLength": 20000},
                    },
                    "required": ["command", "message"],
                },
            ],
        },
    },
]

_REMOTE_MUTEX_NOTE = (
    "\n\nMUTEX & POLLING: every remote tool EXCEPT remote-ssh shares ONE global "
    "lock — at most one remote operation runs at a time (so you can't, e.g., fire "
    "`up` ten times and stack ten paid EC2 instances, or change the box's branch "
    "mid-build). Long operations (provision/build) run DETACHED and return "
    "immediately; poll by re-calling ANY remote tool (a running slot reports "
    "progress and will NOT start a second op). When an op finishes, the NEXT remote "
    "tool call returns its result once and frees the slot — so polling a finished "
    "build never silently launches a rebuild. The full output is always streamed to "
    "a log file whose path is returned (open it with Read). Use remote-ssh (which is "
    "NOT under the mutex) to inspect the box while a long op holds the slot."
)

_FROM_SCRATCH_WARNING = (
    "DANGER: default false. In normal circumstances agents should NEVER set "
    "from_scratch=true. It bypasses the cached AMI/snapshot path and starts a "
    "new EC2 box from the base Ubuntu image, losing the startup/build-cache "
    "benefit. Use true only when the USER has explicitly confirmed a clean "
    "from-scratch rebuild."
)

_REMOTE_RUN_COMMON_PROPS = {
    "branch": {
        "type": "string",
        "description": (
            "Branch to build. remote-run builds the latest PUSHED commit: it "
            "git-fetches and checks out origin/<branch> on the box before building "
            "(see `sync`). Defaults to your current local branch. The branch MUST be "
            "pushed to origin, and must contain the build_mcp updates that provide "
            "the box-side --run-build-sync entry point."
        ),
    },
    "sync": {
        "type": "boolean",
        "default": True,
        "description": (
            "Before building, fetch origin and `git checkout -B <branch> "
            "origin/<branch>` on the box so it builds the latest pushed commit "
            "(force-resets the box's working branch — the box is disposable). Set "
            "false to build whatever is already checked out on the box (e.g. when "
            "you arranged it via remote-git, or to avoid clobbering a manual state)."
        ),
    },
    "auto_provision": {
        "type": "boolean",
        "default": True,
        "description": (
            "If no box is currently running, provision one (provisioner up --wait) "
            "before building. This launches a PAID EC2 instance. Set false to fail "
            "fast instead when no box exists."
        ),
    },
    "from_scratch": {
        "type": "boolean",
        "default": False,
        "description": (
            _FROM_SCRATCH_WARNING + " When false, provisioner.py up automatically "
            "uses the newest cached bioseq AMI in the target region if one exists."
        ),
    },
    "preset": {
        "type": "string",
        "enum": sorted(REMOTE_BUILD_PRESETS),
        "default": REMOTE_BUILD_PRESET,
        "description": (
            "Remote CMake preset exported as BIOSEQ_BUILD_PRESET for the box-side "
            "build worker. The default is the Clang/libc++ AWS preset; use "
            "aws-x64-linux-gcc161 for the separate GCC 16.1 build tree."
        ),
    },
}


TOOL_DEFINITIONS += [
    {
        "name": "remote-status",
        "description": (
            "Show the AWS benchmark box status — wraps `provisioner.py status`: "
            "Terraform outputs (instance id / public ip / region), the live EC2 "
            "state, a count of running benchmark instances (cost guardrail), and the "
            "on-box PROVISION_STATUS marker (VERIFY_OK_FAILS_ON_OUR_CODE = ready). "
            "Equivalent to remote-manage with operation='status'." + _REMOTE_MUTEX_NOTE
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {},
            "required": [],
        },
    },
    {
        "name": "remote-debug",
        "description": (
            "Read-only diagnostics for the AWS benchmark box and the local remote "
            "MCP worker. Unlike remote-status/remote-manage/remote-run, this tool "
            "does NOT acquire the remote mutex and can be called while a long build "
            "owns the slot. It reports local worker process state, the worker log "
            "tail, Terraform outputs, EC2 instance/status-check data, and recent "
            "CloudWatch CPU via AWS APIs. Set include_ssh=true to also attempt a "
            "short bounded read-only SSH process probe; by default it avoids SSH so "
            "it still works when SSH banner exchange is slow during saturated "
            "compiles."
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "log_lines": {
                    "type": "integer", "minimum": 1, "maximum": 500, "default": 80,
                    "description": "Number of local remote-worker log lines to include.",
                },
                "cloudwatch_minutes": {
                    "type": "integer", "minimum": 5, "maximum": 360, "default": 120,
                    "description": "Lookback window for AWS/EC2 CPUUtilization datapoints.",
                },
                "include_ssh": {
                    "type": "boolean", "default": False,
                    "description": "Also attempt a bounded read-only SSH ps/free/df probe.",
                },
                "include_ssm": {
                    "type": "boolean", "default": False,
                    "description": (
                        "Also attempt a read-only AWS Systems Manager RunCommand "
                        "ps/free/df probe when the SSM agent is Online."
                    ),
                },
                "ssh_timeout": {
                    "type": "integer", "minimum": 5, "maximum": 120, "default": 20,
                    "description": "Subprocess timeout for the optional SSH probe.",
                },
                "ssm_timeout": {
                    "type": "integer", "minimum": 10, "maximum": 120, "default": 30,
                    "description": "Timeout for the optional SSM read-only probe.",
                },
            },
            "required": [],
        },
    },
    {
        "name": "remote-status-ps",
        "description": (
            "Run a concise read-only SSH status snapshot on the AWS box without "
            "acquiring the remote mutex. It prints date, host/provision status, "
            "repo commit, load, `swapon --show`, `free -h`, root/build disk "
            "usage, and the current cmake/ninja/clang/gcc/linker/bioseq process list. "
            "Use it while a long remote-run owns the slot to inspect memory, CPU, "
            "swap, and active build processes."
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "timeout": {
                    "type": "integer", "minimum": 5, "maximum": REMOTE_OP_TIMEOUTS["ssh"],
                    "default": 300,
                    "description": "Subprocess timeout in seconds for the SSH snapshot.",
                },
            },
            "required": [],
        },
    },
    {
        "name": "remote-manage",
        "description": (
            "Manage the AWS benchmark box — wraps `provisioner.py <operation>` for "
            "operation in up/down/status/logs/verify/wait/output:\n"
            "  up     — create/update the box (OpenTofu apply). Provisions a PAID "
            "instance. Defaults: uses the newest cached bioseq AMI/snapshot in the "
            "target region when available, branch=your current local branch, "
            "region=the existing stack's region, wait=true (block until on-box "
            "provisioning is terminal). Refuses if a box is already running unless "
            "yes=true. " + _FROM_SCRATCH_WARNING + "\n"
            "  down   — create a cached AMI/snapshot of the current root volume, "
            "then destroy the box and managed resources (always non-interactive).\n"
            "  status — same as remote-status.\n"
            "  logs   — tail /var/log/bioseq-provision.log (last `lines`).\n"
            "  verify — re-run verify.sh on the box (cmake configure + build; "
            "classify failures into the PROVISION_STATUS verdict).\n"
            "  wait   — block until on-box provisioning reaches a terminal status.\n"
            "  output — print Terraform outputs as JSON." + _REMOTE_MUTEX_NOTE
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "operation": {
                    "type": "string",
                    "enum": sorted(REMOTE_MANAGE_OPS),
                    "description": "Which provisioner.py command to run.",
                },
                "branch": {"type": "string",
                           "description": "[up] branch to clone (default: your current local branch)."},
                "region": {"type": "string",
                           "description": "[up/status/down] AWS region (default: the existing stack's region)."},
                "instance_type": {"type": "string",
                                  "description": "[up] EC2 instance type (default m4.4xlarge = 16 vCPU/64 GiB)."},
                "ami": {"type": "string",
                        "description": ("[up] override AMI id (default: newest cached "
                                        "bioseq AMI if available, otherwise latest "
                                        "Ubuntu 24.04 x86_64).")},
                "from_scratch": {
                    "type": "boolean",
                    "default": False,
                    "description": (
                        "[up] " + _FROM_SCRATCH_WARNING + " When false, `up` "
                        "uses the newest cached bioseq AMI if one exists."
                    ),
                },
                "volume_size": {"type": "integer", "minimum": 1,
                                "description": "[up] root volume GiB (default 300)."},
                "no_verify": {"type": "boolean", "default": False,
                              "description": "[up] skip the on-box build verification."},
                "wait": {"type": "boolean", "default": True,
                         "description": "[up] block until on-box provisioning finishes (default true)."},
                "timeout": {"type": "integer", "minimum": 1, "default": 120,
                            "description": "[up/wait] timeout in minutes (default 120)."},
                "yes": {"type": "boolean", "default": False,
                        "description": "[up] override the running-instance guardrail and apply anyway."},
                "lines": {"type": "integer", "minimum": 1, "default": 200,
                          "description": "[logs] number of trailing log lines to show (default 200)."},
            },
            "required": ["operation"],
        },
    },
    {
        "name": "remote-ssh",
        "description": (
            "Run a shell command on the AWS box and return its output (wraps "
            "`provisioner.py ssh`, non-interactive). This is the ONLY remote tool "
            "NOT under the remote mutex — use it as the read-only escape hatch to "
            "inspect the box (e.g. `cat /opt/bioseq/PROVISION_STATUS`, `git -C "
            "~/bioseqdb log -1`, `nvidia-smi`, `free -h`) while a long remote-run "
            "holds the slot. Synchronous and bounded by the configured remote SSH "
            "timeout (currently long enough to tolerate saturated compiles). For "
            "long-running work use remote-run / remote-manage, not this; for "
            "non-SSH diagnostics use remote-debug."
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "command": {
                    "type": "string",
                    "description": ("Shell command to run on the box via `bash -lc`. "
                                    "The repo is at ~/bioseqdb."),
                },
            },
            "required": ["command"],
        },
    },
    {
        "name": "remote-snapshot",
        "description": (
            "Create a reusable cached AWS AMI from the current benchmark box root "
            "volume. This captures installed dependencies (clang/LLVM, GCC, CMake, "
            "Ninja, PostgreSQL, vcpkg) plus the existing repo/build directory so "
            "future remote provisioning can start from preexisting artifacts instead "
            "of rebuilding from scratch. Prefer running this after expensive setup or "
            "before teardown; `remote-manage` operation='down' already snapshots "
            "automatically before destroying the EC2 instance. Normal `remote-run`/"
            "`remote-manage up` calls then use the newest cached AMI by default. "
            "Avoid from_scratch=true unless the USER explicitly confirmed a clean "
            "base-image rebuild. By default AWS may reboot the instance briefly to "
            "make the image filesystem-consistent; no EC2 instance is destroyed by "
            "this tool." + _REMOTE_MUTEX_NOTE
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "region": {
                    "type": "string",
                    "description": "AWS region of the current stack (default: Terraform outputs/env).",
                },
                "name": {
                    "type": "string",
                    "description": "Optional AMI name (default bioseq-benchmark-aws-cache-<UTC timestamp>).",
                },
                "description": {
                    "type": "string",
                    "description": "Optional AMI description.",
                },
                "timeout": {
                    "type": "integer",
                    "minimum": 1,
                    "default": 120,
                    "description": "Wait timeout in minutes for AMI availability.",
                },
                "no_wait": {
                    "type": "boolean",
                    "default": False,
                    "description": "Return after create-image is requested instead of waiting for AMI availability.",
                },
                "no_reboot": {
                    "type": "boolean",
                    "default": False,
                    "description": (
                        "Ask AWS not to reboot the instance before imaging. Faster, "
                        "but the filesystem is only crash-consistent; leave false for "
                        "normal cache snapshots."
                    ),
                },
            },
            "required": [],
        },
    },
    {
        "name": "remote-run",
        "description": (
            "Run a build/test/benchmark or an inspect on the AWS box — the remote "
            "twin of start_build + inspect. Ensures a ready box exists (provisioning "
            "+ waiting if needed, using the newest cached bioseq AMI/snapshot by "
            "default), git-syncs the box to the requested branch's latest PUSHED "
            "commit (unless sync=false), then runs the operation on the box using "
            "the box-side server.py with the aws-x64-linux preset. " + _FROM_SCRATCH_WARNING + "\n\n"
            "Two operation shapes (oneOf):\n"
            "  operation='build' — runs any remote-compatible start_build mode "
            f"({', '.join(REMOTE_RUN_BUILD_MODES)}) with the same build_only/debug/"
            "filter/debug_report semantics as the local start_build tool, including "
            "focused Catch2 filter.tags for every closed Catch correctness "
            "mode, including CMake-owned private-cluster cache execution.\n"
            "  operation='inspect' — runs the inspect tool on a box-side binary, "
            "with the local tool's exact-label prefilter, one end-to-end "
            "assembly deadline, bounded streaming fallback, snippet/state "
            "budgets, and source_basename semantics. "
            "Omitting source_basename skips only the inline-site sweep.\n\n"
            "IMPORTANT CAVEATS:\n"
            "  - The box is x86_64 Linux. Build/test/bench modes expect the "
            "provisioned PostgreSQL + bioseq DB to be ready on the box; failures in "
            "our code should now be treated as real regressions, not as an expected "
            "::NEON/macOS dispatch.h gap.\n"
            "  - remote inspect needs a successfully-built binary on the box, so it "
            "won't return useful disassembly until the build succeeds. Paths in its "
            "JSON are box-side paths: objdump files are under the box's /tmp, while "
            "assembly listings remain in its build tree; use remote-ssh for follow-up "
            "reads." + _REMOTE_MUTEX_NOTE
        ),
        "inputSchema": {
            "type": "object",
            "oneOf": [
                {
                    "title": "build",
                    "type": "object",
                    "additionalProperties": False,
                    "required": ["operation", "mode"],
                    "properties": {
                        "operation": {"const": "build",
                                      "description": "Run a build/test/benchmark mode."},
                        "mode": {
                            "type": "string",
                            "enum": list(REMOTE_RUN_BUILD_MODES),
                            "description": ("Which run to build/execute — same modes "
                                            "as the remote-compatible subset of "
                                            "the local start_build tool."),
                        },
                        "build_only": {
                            "type": "boolean", "default": False,
                            "description": ("Only compile the targets; do not run the "
                                            "tests/benches. The reliable path on the "
                                            "box today (the build stops at the ::NEON "
                                            "gap regardless)."),
                        },
                        "debug": {
                            "type": "boolean", "default": False,
                            "description": ("Use the debug build tree (adds -g + "
                                            "-save-temps for inspect). Rarely needed "
                                            "on the box."),
                        },
                        "filter": {
                            "type": "object",
                            "additionalProperties": False,
                            "description": (
                                "Mode-dependent subset selection, identical to local "
                                "start_build. Catch correctness modes accept only tags as "
                                "bounded Catch2 test-spec expressions and apply their "
                                "OR expression to every executable in the mode. CLI benchmark modes "
                                f"({', '.join(BENCH_CLI_MODES)}) translate tags/benchmarks/cases "
                                "to repeatable --filter flags. Other modes reject filter."
                            ),
                            "properties": {
                                "tags": {
                                    "type": "array",
                                    "items": {"type": "string", "minLength": 1},
                                    "minItems": 1,
                                    "description": "Catch correctness modes: Catch2 test-spec expressions. Bench modes: benchmark tag OR-set.",
                                },
                                "cases": {
                                    "type": "array",
                                    "items": {"type": "string", "minLength": 1},
                                    "minItems": 1,
                                    "description": "Within selected benchmarks, run only these matrix.over cases (algorithm names).",
                                },
                                "benchmarks": {
                                    "type": "array",
                                    "items": {"type": "string", "minLength": 1},
                                    "minItems": 1,
                                    "description": "Run only these benchmark keys. (Maps to the bench CLI's singular `benchmark=`.)",
                                },
                            },
                        },
                        "debug_report": {
                            "type": "boolean",
                            "default": False,
                            "description": (
                                "Bench-only. Pass --debug to the bench binary and "
                                "stream report_debug_<N>.jsonl next to report_<N>.json. "
                                "Requires a non-empty filter. Supplying this property is "
                                "rejected for unit-integration mode."
                            ),
                        },
                        "bench_e2e_config": {
                            "type": "string",
                            "minLength": 1,
                            "pattern": "^(?!/)(?!.*(?:^|/)\\.\\.(?:/|$)).+\\.json$",
                            "description": (
                                "benchmark-e2e only. Repository-relative JSON path "
                                "present in the pushed branch/on-box checkout. "
                                "Uses the same ordinary E2E configuration "
                                "contract as local start_build."
                            ),
                        },
                        **_REMOTE_RUN_COMMON_PROPS,
                    },
                    "allOf": [
                        {
                            "if": {
                                "properties": {
                                    "mode": {"enum": sorted(CATCH_FILTER_MODES)},
                                },
                                "required": ["mode"],
                            },
                            "then": {
                                "not": {"required": ["debug_report"]},
                                "properties": {
                                    "filter": {
                                        "not": {
                                            "anyOf": [
                                                {"required": ["benchmarks"]},
                                                {"required": ["cases"]},
                                            ]
                                        },
                                        "properties": {
                                            "tags": {
                                                "maxItems": _MAX_CATCH2_TEST_SPECS,
                                                "items": {
                                                    "type": "string",
                                                    "minLength": 1,
                                                    "maxLength": _MAX_CATCH2_TEST_SPEC_LENGTH,
                                                    "pattern": "^(?!-)[^\\u0000-\\u001F\\u007F]+$",
                                                },
                                            }
                                        },
                                    }
                                },
                            },
                        }
                    ],
                },
                {
                    "title": "inspect",
                    "type": "object",
                    "additionalProperties": False,
                    "required": ["operation", "function_name"],
                    "properties": {
                        "operation": {"const": "inspect",
                                      "description": "Inspect a function in a box-side binary."},
                        "function_name": {
                            "type": "string",
                            "description": ("Substring matched against demangled C++ "
                                            "names (prefix-anchored recommended), same "
                                            "as the local inspect tool."),
                        },
                        "target": {
                            "type": "string",
                            "enum": sorted(INSPECT_TARGETS),
                            "default": "bioseq_benchmark",
                            "description": "Which target binary to inspect on the box.",
                        },
                        "max_matches": {"type": "integer", "minimum": 1,
                                        "maximum": MAX_INSPECT_MATCHES, "default": 4,
                                        "description": "Clamped cap on returned symbols."},
                        "snippet_lines": {"type": "integer", "minimum": 1,
                                          "maximum": MAX_INSPECT_SNIPPET_LINES,
                                          "default": 200,
                                          "description": "Clamped lines of bounded asm/disasm snippets to inline."},
                        "source_basename": {"type": "string",
                                            "description": ("Source basename for the inline-site sweep; omission skips "
                                                            "that sweep but preserves standalone pre-LTO blocks.")},
                        "max_llc_bc_files": {"type": "integer", "minimum": 0, "default": 4,
                                             "description": "Cap on .bc files llc-compiled for the pre-LTO view."},
                        "llc_on_bc": {"type": "boolean", "default": True,
                                      "description": "Run the llc-on-.bc fallback when no .s is present."},
                        "debug": {"type": "boolean",
                                  "description": "Force the debug/fast build tree (omit to auto-detect)."},
                        **_REMOTE_RUN_COMMON_PROPS,
                    },
                },
            ],
        },
    },
    {
        "name": "remote-git",
        "description": (
            "Proxy a git command to the box's repo (~/bioseqdb), under the remote "
            "mutex. Pass just the git subcommand and args in `command` — e.g. "
            "{\"command\": \"pull\"}, {\"command\": \"checkout my-branch\"}, "
            "{\"command\": \"fetch origin\"}, {\"command\": \"merge origin/x -X "
            "theirs\"}, {\"command\": \"reset --hard origin/main\"}. A leading 'git ' "
            "is tolerated. For read-only git inspection while another remote op is "
            "running, use remote-ssh instead (it is not under the mutex)." + _REMOTE_MUTEX_NOTE
        ),
        "inputSchema": {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "command": {
                    "type": "string",
                    "description": ("git subcommand + args to run in ~/bioseqdb on the "
                                    "box, e.g. 'checkout research/aws-machine' or "
                                    "'merge origin/feature -X theirs'."),
                },
            },
            "required": ["command"],
        },
    },
]


TOOL_DEFINITIONS += [
    {
        "name": "doc_create",
        "description": (
            "Create a new project document under DOCS/CURRENT/<TYPE>/ with consistent "
            "frontmatter and an auto-allocated DOC id (max existing + 1). The file is "
            "immediately FTS-indexed into QMD (vector embeddings refresh in the background) so "
            "doc_search finds it. Returns the absolute path — open it with Read and write the "
            "body. Routing: bench topics -> BENCHMARK_FRAMEWORK, bugfix -> BUGFIX, else by type. "
            "Topics are validated against DOCS/.topics unless allow_new_topic=true."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "type": {
                    "type": "string",
                    "enum": ["guidelines", "human_notes", "tech_spec", "research", "report",
                             "impl_plan", "prompt", "external_doc", "bugfix"],
                    "description": "Document type (decides directory + frontmatter).",
                },
                "title": {"type": "string",
                          "description": "Human title; becomes the filename slug."},
                "topics": {"type": "array", "items": {"type": "string"},
                           "description": "1+ topic tags (closed vocabulary in .topics)."},
                "summary": {"type": "string", "description": "<=400 char one-line summary."},
                "origin": {"type": "string",
                           "enum": ["ai_output", "human_prompt", "external_dump",
                                    "human_notes", "mixed"], "default": "ai_output"},
                "supersedes": {"type": "array", "items": {"type": "string"},
                               "description": "DOC ids this replaces, e.g. [\"DOC0310\"]."},
                "allow_new_topic": {"type": "boolean", "default": False,
                                    "description": "Permit topics not yet in .topics."},
            },
            "required": ["type", "title", "topics"],
        },
    },
    {
        "name": "doc_archive",
        "description": (
            "Archive a document: move it from DOCS/CURRENT/ to DOCS/ARCHIVE/ "
            "(preserving the <TYPE>/ subdir), set `archived: true` (and status: archived unless "
            "superseded) in its frontmatter, and refresh the QMD index. `doc` accepts a DOC id "
            "(DOC0123 or 123), an exact filename, or a unique path substring."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"doc": {"type": "string",
                           "description": "DOC id, filename, or unique path fragment."}},
            "required": ["doc"],
        },
    },
    {
        "name": "doc_search",
        "description": (
            "Search project documents via QMD hybrid retrieval (BM25 + vector) + LLM reranking "
            "over a warm local model server. Returns the BEST hit's absolute path + the "
            "most-relevant text fragment, plus, for the other hits, their metadata (path, "
            "doc_id, title, context, score). Searches CURRENT by default; set "
            "include_archived=true to also search ARCHIVE."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string",
                          "description": "Natural-language or keyword query."},
                "limit": {"type": "integer", "default": 10, "minimum": 1, "maximum": 50},
                "min_score": {"type": "number", "default": 0.0, "minimum": 0, "maximum": 1},
                "include_archived": {"type": "boolean", "default": False},
                "intent": {"type": "string",
                           "description": "Optional disambiguation context."},
            },
            "required": ["query"],
        },
    },
    {
        "name": "doc_read",
        "description": (
            "Resolve a document selector (DOC id, filename, or path fragment) to its absolute "
            "on-disk path plus metadata, so you can open it with the Read tool. Does not return "
            "the body itself."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"doc": {"type": "string",
                           "description": "DOC id, filename, or unique path fragment."}},
            "required": ["doc"],
        },
    },
]

_OUTPUT_FRAMING = "content-length"

def _write_message(msg: dict) -> None:
    body = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    if _OUTPUT_FRAMING == "newline":
        sys.stdout.write(body.decode("utf-8") + "\n")
        sys.stdout.flush()
        return

    sys.stdout.buffer.write(
        f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body
    )
    sys.stdout.buffer.flush()

def _parse_jsonrpc_body(body: bytes, source: str) -> dict:
    try:
        return json.loads(body.decode("utf-8"))
    except json.JSONDecodeError as e:
        _log(f"malformed JSON-RPC {source} dropped: {e}")
        return {"_dropped": True}

def _read_message() -> dict | None:
    """Read standard MCP Content-Length or newline-delimited JSON frames."""
    global _OUTPUT_FRAMING

    stdin = sys.stdin.buffer
    first = stdin.read(1)
    while first in (b"\r", b"\n"):
        first = stdin.read(1)
    if not first:
        return None

    if first == b"{":
        _OUTPUT_FRAMING = "newline"
        line = first + stdin.readline()
        return _parse_jsonrpc_body(line.strip(), "line")

    _OUTPUT_FRAMING = "content-length"
    header_lines = [first + stdin.readline()]
    while True:
        line = stdin.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        header_lines.append(line)

    content_length = None
    for line in header_lines:
        decoded = line.decode("ascii", errors="replace").strip()
        name, sep, value = decoded.partition(":")
        if sep and name.lower() == "content-length":
            try:
                content_length = int(value.strip())
            except ValueError:
                _log(f"invalid Content-Length dropped: {value.strip()!r}")
                return {"_dropped": True}
            break

    if content_length is None:
        header = b"".join(header_lines).decode("ascii", errors="replace").strip()
        _log(f"missing Content-Length header dropped: {header!r}")
        return {"_dropped": True}

    body = stdin.read(content_length)
    if len(body) != content_length:
        _log(f"short JSON-RPC body dropped: got {len(body)}, expected {content_length}")
        return None

    return _parse_jsonrpc_body(body, "frame")

def _make_response(req_id, *, result=None, error=None) -> dict:
    msg = {"jsonrpc": "2.0", "id": req_id}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    return msg

def _handle_initialize(params: dict) -> dict:
    return {
        "protocolVersion": PROTOCOL_VERSION,
        "capabilities": {"tools": {}},
        "serverInfo": SERVER_INFO,
    }

def _handle_tools_list(params: dict) -> dict:
    return {"tools": TOOL_DEFINITIONS}

def parse_bool(parameter_description: str, value: Any) -> bool:
    if value is None:
        return False
    if isinstance(value, str):
        value = value.lower()
        if value not in ['true', 'false', 'y', 'n']:
            raise RuntimeError(f"Value passed as {parameter_description} is not a valid bool. Got string '{value}' which is neither 'true' or 'false'")
        return (value == "true" or value == "y")
    if type(value) == bool:
        return value
    raise RuntimeError(f"Value passed as {parameter_description} is unsupported type '{type(value)}' (with value: '{value}')")


def _schema_root_properties(schema: object) -> set[str]:
    if not isinstance(schema, dict):
        return set()
    result = set(schema.get("properties", {}))
    for keyword in ("allOf", "anyOf", "oneOf"):
        variants = schema.get(keyword, [])
        if isinstance(variants, list):
            for variant in variants:
                result.update(_schema_root_properties(variant))
    for keyword in ("if", "then", "else"):
        result.update(_schema_root_properties(schema.get(keyword)))
    return result


def _reject_unknown_tool_arguments(name: object, arguments: dict) -> None:
    definition = next(
        (item for item in TOOL_DEFINITIONS if item.get("name") == name),
        None,
    )
    if definition is None:
        return
    schema = definition.get("inputSchema")
    allowed = _schema_root_properties(schema)
    if isinstance(schema, dict):
        variants = schema.get("oneOf")
        if isinstance(variants, list):
            for variant in variants:
                if not isinstance(variant, dict):
                    continue
                properties = variant.get("properties")
                if not isinstance(properties, dict):
                    continue
                if any(
                    isinstance(property_schema, dict)
                    and "const" in property_schema
                    and arguments.get(property_name)
                        == property_schema["const"]
                    for property_name, property_schema in properties.items()
                ):
                    allowed = _schema_root_properties(variant)
                    break
    unknown = sorted(set(arguments) - allowed)
    if unknown:
        raise RuntimeError(
            f"{name} does not accept unknown argument(s): "
            + ", ".join(unknown)
        )


def _handle_tools_call(params: dict) -> dict:
    name = params.get("name")
    if "arguments" not in params:
        args = {}
    else:
        args = params["arguments"]
        if not isinstance(args, dict):
            raise RuntimeError("tool arguments must be an object")
    # subagent performs complete branch-aware validation itself so direct and
    # public MCP calls return the same stable JSON envelope.  The generic
    # helper only rejects unknown keys and would otherwise turn these expected
    # validation failures into transport-level JSON-RPC errors.
    if name != "subagent":
        _reject_unknown_tool_arguments(name, args)
    if name == "inspect_postgres_crash":
        from crash_inspect import inspect_postgres_crash
        return {"content": [{"type": "text", "text": inspect_postgres_crash()}]}
    if name == "external_aligners":
        from external_aligners import tool_external_aligners
        return {"content": [{"type": "text", "text": tool_external_aligners(**args)}]}
    if name == "start_build":
        mode = args.get("mode", "")
        build_only: bool = parse_bool("build_only", args.get("build_only", False))
        debug: bool = parse_bool("debug", args.get("debug", False))
        bench_filter = args.get("filter")                       # dict | None
        debug_report: bool = parse_bool("debug_report", args.get("debug_report", False))
        fresh: bool = parse_bool("fresh", args.get("fresh", False))
        run_only: bool = parse_bool("run_only", args.get("run_only", False))
        build_timeout_raw = args.get("build_timeout_sec")
        if build_timeout_raw is None:
            build_timeout_sec = None
        elif (
            isinstance(build_timeout_raw, int)
            and not isinstance(build_timeout_raw, bool)
        ):
            build_timeout_sec = build_timeout_raw
        else:
            raise RuntimeError(
                "build_timeout_sec must be an integer when supplied")
        bench_e2e_config = args.get("bench_e2e_config")
        reuse_build_receipt = args.get("reuse_build_receipt")
        text = tool_start_build(mode, build_only=build_only, debug=debug,
                                bench_filter=bench_filter,
                                debug_report=debug_report,
                                debug_report_supplied="debug_report" in args,
                                debug_supplied="debug" in args,
                                filter_supplied="filter" in args,
                                bench_e2e_config=bench_e2e_config,
                                fresh=fresh,
                                build_timeout_sec=build_timeout_sec,
                                run_only=run_only,
                                reuse_build_receipt=reuse_build_receipt)
    elif name == "status":
        if "mode" not in args:
            text = tool_status()
        else:
            mode = args["mode"]
            if not isinstance(mode, str):
                raise RuntimeError("status mode must be a string when supplied")
            if mode not in VALID_MODES:
                raise RuntimeError("status mode must be a supported build mode")
            text = tool_status(mode)
    elif name == "inspect":
        text = tool_inspect(args)
    elif name == "import_e2e_artifacts":
        text = tool_import_e2e_artifacts(args)
    elif name == "export_e2e_artifacts":
        text = tool_export_e2e_artifacts(args)
    elif name == "papers-build":
        text = tool_papers_build(args)
    elif name == "papers-inspect":
        return tool_papers_inspect(args)
    elif name == "subagent":
        text = tool_subagent(args)
    elif name == "remote-status":
        text = tool_remote_status()
    elif name == "remote-debug":
        text = tool_remote_debug(args)
    elif name == "remote-status-ps":
        text = tool_remote_status_ps(args)
    elif name == "remote-manage":
        text = tool_remote_manage(args)
    elif name == "remote-ssh":
        text = tool_remote_ssh(args.get("command", ""))
    elif name == "remote-snapshot":
        text = tool_remote_snapshot(args)
    elif name == "remote-run":
        text = tool_remote_run(args)
    elif name == "remote-git":
        text = tool_remote_git(args.get("command", ""))
    elif name in ("doc_create", "doc_archive", "doc_search", "doc_read"):
        # Doc-tool requests are JSON-Schema-validated server-side inside operations
        # (docs/schemas.py). ensure_doc_env() lazily provisions qmd + jsonschema on first use.
        from docs.qmd_manager import get_manager

        qmd_manager = get_manager()
        qmd_manager.begin_external_update()
        try:
            bootstrap.ensure_doc_env()
        finally:
            qmd_manager.end_external_update()
        from docs import operations
        text = getattr(operations, name)(args)
    else:
        return {
            "content": [{"type": "text", "text": f"unknown tool: {name}"}],
            "isError": True,
        }
    return {"content": [{"type": "text", "text": text}]}

HANDLERS = {
    "initialize": _handle_initialize,
    "tools/list": _handle_tools_list,
    "tools/call": _handle_tools_call,
}

def _serve() -> None:
    _log(f"starting MCP server (pid={os.getpid()})")
    while True:
        msg = _read_message()
        if msg is None:
            _log("stdin closed; exiting")
            return
        if msg.get("_dropped"):
            continue
        if "id" not in msg:
            # Notification — no response is expected. We still log so users
            # can confirm the client handshake reached us.
            _log(f"notification: {msg.get('method')}")
            continue
        method = msg.get("method")
        params = msg.get("params") or {}
        handler = HANDLERS.get(method)
        if handler is None:
            _write_message(_make_response(
                msg["id"],
                error={"code": -32601, "message": f"method not found: {method}"},
            ))
            continue
        try:
            result = handler(params)
            _write_message(_make_response(msg["id"], result=result))
        except Exception as e:
            _log(f"handler {method} raised: {e}")
            _write_message(_make_response(
                msg["id"],
                error={"code": -32000, "message": f"{type(e).__name__}: {e}"},
            ))
