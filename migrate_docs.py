#!/usr/bin/env python3
"""
migrate_docs — one-shot migration of bioseqdb markdown files into main/DOCS/.

Implements the rules in REORG_PLAN.md:
  - routing rule (importance-class override for bugfix and bench-framework)
  - slug derivation with generic-name rule + uniqueness assertion
  - DOC id assignment by git first-commit timestamp (lexical path tiebreaker)
  - YAML frontmatter prepended to every moved file
  - migration map artifact at main/DOCS/.migration_map.txt

Run modes:
  --dry-run    do not touch the filesystem; just print map + summary.
  --execute    perform git mv + frontmatter write.

Run order:
  python3 migrate_docs.py --dry-run    # inspect main/DOCS/.migration_map.txt
  python3 migrate_docs.py --execute
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from datetime import datetime

ROOT = Path(__file__).resolve().parent
DOCS_ROOT = ROOT / "main" / "DOCS"

# ---------------------------------------------------------------------------
# 1. Hand-written authoritative classification
#    This is the source of truth — drives both directory and topic assignment.
# ---------------------------------------------------------------------------

SPECIAL_HAND_MAP: dict[str, tuple[str, str, str, str, str]] = {
    # rel_path -> (type, dir, topic, origin, summary)
    "CODE_STYLE.md": (
        "guidelines", "GUIDELINES", "code-style", "human_notes",
        "Mandatory code style rules for all C++ work in bioseqdb."
    ),
    "LAUD.lol": (
        "guidelines", "GUIDELINES", "ai-collab", "human_notes",
        "AI agent guidelines — how Claude/agents should collaborate on this repo."
    ),
    "README.md": ("DELETE", "", "", "", ""),
    "main/SETUP.md": (
        "human_notes", "HUMAN_NOTES", "setup", "human_notes",
        "MacOS development environment setup instructions."
    ),
    "main/ROOT_TODO.md": (
        "human_notes", "HUMAN_NOTES", "todo", "human_notes",
        "Active human-curated TODO list for the repo."
    ),
    "main/bioseq/RESEARCH/CODE_GUIDELINES.md": (
        "guidelines", "GUIDELINES", "code-style", "human_notes",
        "Additional code guidelines for the bioseq research/algorithms code."
    ),
    "REORG_PLAN.md": (
        "human_notes", "HUMAN_NOTES", "todo", "human_notes",
        "Plan for the markdown reorganisation under main/DOCS/."
    ),
}

KEEP_IN_PLACE = {
    "main/bench_framework/README.md",
    "main/data/README.md",
}

# Generic basename set — when a file's slug is in this set, prepend parent dir.
GENERIC_SET = {
    "README", "PROMPT", "REPORT", "PLAN", "IMPL_PLAN", "IMPL_PROMPT",
    "TECH_SPEC", "IMPL_TECH_SPEC", "SUMMARY", "NOTES", "DOC", "DOCS",
    "RESEARCH", "INTRO", "INTRODUCTION", "CODE",
}

# Type → default directory
TYPE_TO_DIR = {
    "guidelines":   "GUIDELINES",
    "human_notes":  "HUMAN_NOTES",
    "tech_spec":    "TECH_SPECS",
    "research":     "RESEARCH",
    "report":       "REPORTS",
    "impl_plan":    "IMPL_PLANS",
    "prompt":       "PROMPTS",
    "external_doc": "EXTERNAL_DOCS",
    "bugfix":       "BUGFIX",
}

# Topics whose presence routes a doc into BENCHMARK_FRAMEWORK/ regardless of type
BENCH_FRAMEWORK_TOPICS = {"bench-framework", "benchmarking-speedup"}

# ---------------------------------------------------------------------------
# 2. Per-file classification
# ---------------------------------------------------------------------------

def base_name(rel: str) -> str:
    return os.path.basename(rel).replace(".md", "").replace(".lol", "")

def topic_external_from_path(rel: str) -> str:
    rel_low = rel.lower()
    if "plotly" in rel_low: return "plotly"
    if "hwy" in rel_low or "highway" in rel_low: return "highway-simd"
    if "simdjson" in rel_low: return "simdjson"
    if "kperf" in rel_low: return "kperf-profiler"
    if "foonathan" in rel_low: return "memory-foonathan"
    if "json-schema" in rel_low: return "json-schema"
    if "catch2" in rel_low: return "catch2"
    if "minimap" in rel_low: return "minimap2"
    if "mm_fast" in rel_low: return "mm-fast"
    if "simd_minimizers" in rel_low: return "minimizer-simd"
    if "introduction" in rel_low: return "engineering-overview"
    return "engineering-overview"

def classify(rec: dict) -> tuple[str, str, list[str], str]:
    """Return (type, dir_class, topics, origin)."""
    rel = rec["rel"]
    if rel in SPECIAL_HAND_MAP:
        t, d, topic, origin, _ = SPECIAL_HAND_MAP[rel]
        if t == "DELETE":
            return ("DELETE", "", [], "")
        return (t, d, [topic], origin)

    bn = base_name(rel)
    bn_upper = bn.upper()
    is_prompt = "PROMPT" in bn_upper
    is_report = "REPORT" in bn_upper
    is_spec = "TECH_SPEC" in bn_upper or "SPEC_" in bn_upper or bn_upper.endswith("_SPEC") or bn_upper.startswith("SPEC")
    is_plan = "IMPL_PLAN" in bn_upper or "IMPL_DESIGN" in bn_upper or bn_upper.endswith("_PLAN")
    is_impl_prompt = "IMPL_PROMPT" in bn_upper
    is_research = "RESEARCH" in bn_upper

    # --- External-doc files (internet dumps) by basename hints ---
    ext_doc_basename_hints = (
        "PLOTLY_REF", "SIMDJSON_REF", "HWY_DOCS", "KPERF_EXAMPLE",
        "FOONATHAN_MEMORY_DOCS", "MINIMAP2_DETAILS",
        "MEM_ALLOC_RESULTS_SUMMARY", "CATCH2_BENCHMARKING_HOW",
    )
    if any(h in bn_upper for h in ext_doc_basename_hints):
        return ("external_doc", "EXTERNAL_DOCS", [topic_external_from_path(rel)], "external_dump")
    if rel.endswith("json-schema-validator_readme.md"):
        return ("external_doc", "EXTERNAL_DOCS", ["json-schema"], "external_dump")
    if bn.lower() in ("minimap_cli", "minimap_hash64", "mm_fast", "introduction"):
        return ("external_doc", "EXTERNAL_DOCS", [topic_external_from_path(rel)], "external_dump")
    if bn.lower() in ("simd_minimizers_block_post", "simd_minimizers_notes"):
        return ("external_doc", "EXTERNAL_DOCS", ["minimizer-simd"], "external_dump")

    # --- NTHASH_NARROW_FIX/ — all bugfix ---
    if rel.startswith("NTHASH_NARROW_FIX/"):
        if is_plan: kind = "bugfix-plan"
        elif is_report: kind = "bugfix-report"
        else: kind = "bugfix-prompt"
        return ("bugfix", "BUGFIX", ["nthash", kind], "ai_output" if is_report or is_plan else "human_prompt")

    # --- Root files ---
    if "/" not in rel:
        # BENCH_V*
        if bn.startswith("BENCH_V") or bn == "BENCH_QUALITY_REPORT" or bn == "BENCH_MIGRATON":
            if is_spec:
                return ("tech_spec", "BENCHMARK_FRAMEWORK", ["bench-framework"], "ai_output")
            if "_CODE" in bn_upper:
                return ("report", "BENCHMARK_FRAMEWORK", ["bench-framework"], "ai_output")
            if "QUALITY_REPORT" in bn_upper:
                return ("report", "BENCHMARK_FRAMEWORK", ["bench-framework"], "ai_output")
            if "MIGRATON" in bn_upper or "MIGRATION" in bn_upper:
                return ("report", "BENCHMARK_FRAMEWORK", ["bench-framework"], "human_notes")
            if is_plan or "IMPL_DESIGN" in bn_upper or "IMPL_PLAN" in bn_upper or "PLAN_REPORT" in bn_upper:
                return ("impl_plan", "BENCHMARK_FRAMEWORK", ["bench-framework"], "ai_output")
            return ("impl_plan", "BENCHMARK_FRAMEWORK", ["bench-framework"], "ai_output")
        # STRANGE_TIMING family
        if "STRANGE_TIMING" in bn_upper:
            if "COMPREHESIVE" in bn_upper or "COMPREHENSIVE" in bn_upper or "REPORT" in bn_upper:
                return ("report", "REPORTS", ["perf-strange-timing"], "ai_output")
            if "PLAN" in bn_upper:
                return ("impl_plan", "IMPL_PLANS", ["perf-strange-timing"], "ai_output")
            if bn == "STRANGE_TIMING":
                return ("bugfix", "BUGFIX", ["perf-strange-timing", "bugfix-prompt"], "human_prompt")
            return ("bugfix", "BUGFIX", ["perf-strange-timing", "bugfix-prompt"], "human_prompt")
        if "IMPROVEMENT_STRANGE_TIMING" in bn_upper:
            if "PLAN" in bn_upper:
                return ("impl_plan", "IMPL_PLANS", ["perf-strange-timing"], "ai_output")
            return ("prompt", "PROMPTS", ["perf-strange-timing"], "human_prompt")
        # REGRESSION_NTHASH
        if "REGRESSION_NTHASH" in bn_upper:
            kind = "bugfix-report" if "REPORT" in bn_upper else "bugfix-prompt"
            return ("bugfix", "BUGFIX", ["nthash", kind], "ai_output" if kind == "bugfix-report" else "human_prompt")
        if "FIX_TESTS" in bn_upper:
            kind = "bugfix-report" if ("FINDINGS" in bn_upper or "SOLUTIONS" in bn_upper) else "bugfix-prompt"
            return ("bugfix", "BUGFIX", ["tests", kind], "ai_output" if kind == "bugfix-report" else "human_prompt")
        if "FIX_MINIMIZER" in bn_upper:
            return ("bugfix", "BUGFIX", ["minimizer", "bugfix-report"], "ai_output")
        if "IMPROVE_BUILD_MCP" in bn_upper:
            if "IMPL_PLAN" in bn_upper:
                return ("impl_plan", "IMPL_PLANS", ["build-mcp"], "ai_output")
            return ("prompt", "PROMPTS", ["build-mcp"], "human_prompt")
        if "PROMPT_DOUBLE_EQ_SYMBOL_HWY" in bn_upper:
            if "SUMMARY" in bn_upper:
                return ("report", "REPORTS", ["highway-simd"], "ai_output")
            return ("prompt", "PROMPTS", ["highway-simd"], "human_prompt")
        if bn == "PROMPT_BENCH_KMERS":
            return ("prompt", "PROMPTS", ["kmers", "bench-framework"], "human_prompt")
        if bn == "PROMPT_KMER_TEMPLATES":
            return ("prompt", "PROMPTS", ["kmers"], "human_prompt")
        if bn == "PROMPT_RADIX_SORT_OPT":
            return ("prompt", "PROMPTS", ["simd-sort"], "human_prompt")
        if bn == "QUICK_NTOEBOOK_PROMPT":
            return ("prompt", "PROMPTS", ["viewer"], "human_prompt")
        if bn == "MCP_PROMPT":
            return ("prompt", "PROMPTS", ["mcp"], "human_prompt")
        return ("prompt", "PROMPTS", ["engineering-overview"], "human_prompt")

    # --- OTHER_TOOLS_RESEARCH/ ---
    if rel.startswith("OTHER_TOOLS_RESEARCH/"):
        parts = rel.split("/")
        subtopic_map = {
            "bg-workers": "bg-workers",
            "bwa-meme": "bwa-meme",
            "mmseqs2": "mmseqs2",
            "new_speedup": "minimizer-speedup",
            "optimised_template_pipeline": "template-pipeline",
            "separate_reader": "io-reader",
            "simd-sort": "simd-sort",
            "sneaky_snake": "sneaky-snake",
        }
        if len(parts) > 2:
            topic = subtopic_map.get(parts[1], parts[1])
        else:
            # OTHER_TOOLS_RESEARCH/*.md — root-level inside research dir
            if "BENCHMARKING_REGRESS" in bn_upper:
                return ("impl_plan", "IMPL_PLANS", ["bench-framework"], "ai_output")
            if "INDEX_INTERFACE" in bn_upper:
                return ("tech_spec", "TECH_SPECS", ["postgres-query"], "ai_output")
            if "IMPL_PROMPT" in bn_upper:
                return ("prompt", "PROMPTS", ["engineering-overview"], "human_prompt")
            return ("prompt", "PROMPTS", ["engineering-overview"], "human_prompt")

        # is research a paper / tool research?
        research_topics = {"bwa-meme", "mmseqs2", "sneaky-snake", "minimizer-speedup",
                           "template-pipeline"}

        # Special subdir handling
        if parts[1] == "new_speedup" and "CREATE_VIEWER" in bn_upper:
            return ("prompt", "PROMPTS", ["viewer"], "human_prompt")
        if parts[1] == "new_speedup" and "COMPACT_MINIMIZERS" in bn_upper:
            return ("report", "REPORTS", [topic], "ai_output")
        if parts[1] == "new_speedup" and "TEMPLATED_MINIMIZER_PLAN" in bn_upper:
            return ("impl_plan", "IMPL_PLANS", [topic], "ai_output")
        if parts[1] == "separate_reader":
            # Separate reader has many specialised files
            if "TECH_SPEC" in bn_upper:
                return ("tech_spec", "TECH_SPECS", ["io-reader", "gzip-io"] if "GZIP" in bn_upper else ["io-reader"], "ai_output")
            if "IMPL_PLAN" in bn_upper or "IMPL_PROMPT" in bn_upper:
                if "IMPL_PROMPT" in bn_upper:
                    return ("prompt", "PROMPTS", ["io-reader"], "human_prompt")
                return ("impl_plan", "IMPL_PLANS", ["io-reader"], "ai_output")
            if "FIX_DRIFT" in bn_upper:
                return ("bugfix", "BUGFIX", ["io-reader", "bugfix-prompt"], "human_prompt")
            if "REPORT" in bn_upper and ("GZIP" in bn_upper or "IO_V" in bn_upper or "BENCH_GENERALISATION" in bn_upper or "UNIFY_READER" in bn_upper):
                return ("report", "REPORTS", ["io-reader"], "ai_output")
            if "PROMPT" in bn_upper:
                return ("prompt", "PROMPTS", ["io-reader"], "human_prompt")
            if "TODOS" in bn_upper:
                return ("impl_plan", "IMPL_PLANS", ["io-reader"], "ai_output")
            if "DEEP_DIVE_SUMMARY" in bn_upper:
                return ("research", "RESEARCH", ["io-reader"], "ai_output")
            if "WHY_NOTEBOOK_DIFFERENCE" in bn_upper:
                return ("report", "REPORTS", ["io-reader"], "ai_output")
            if "IMPROVE_SPEC" in bn_upper:
                return ("prompt", "PROMPTS", ["io-reader"], "human_prompt")
            return ("research", "RESEARCH", ["io-reader"], "ai_output")
        if parts[1] == "simd-sort":
            if "IMPL_PROMPT" in bn_upper:
                return ("prompt", "PROMPTS", ["simd-sort"], "human_prompt")
            if "REGRESSION" in bn_upper:
                return ("bugfix", "BUGFIX", ["simd-sort", "bugfix-report" if "REPORT" in bn_upper else "bugfix-prompt"], "ai_output")
            if "PERFORMANCE_REPORT" in bn_upper or bn == "REPORT":
                return ("research", "RESEARCH", ["simd-sort"], "ai_output")
            if bn == "PROMPT":
                return ("prompt", "PROMPTS", ["simd-sort"], "human_prompt")
            if "SORT_NET" in bn_upper:
                return ("research", "RESEARCH", ["simd-sort"], "ai_output")
            return ("research", "RESEARCH", ["simd-sort"], "ai_output")
        if parts[1] == "sneaky_snake":
            if "TECH_SPEC" in bn_upper:
                return ("tech_spec", "TECH_SPECS", ["sneaky-snake"], "ai_output")
            if bn == "PROMPT":
                return ("prompt", "PROMPTS", ["sneaky-snake"], "human_prompt")
            if bn == "REPORT":
                return ("research", "RESEARCH", ["sneaky-snake"], "ai_output")
            return ("research", "RESEARCH", ["sneaky-snake"], "ai_output")
        if parts[1] == "bg-workers":
            if "TECH_SPEC" in bn_upper:
                return ("tech_spec", "TECH_SPECS", ["bg-workers"], "ai_output")
            return ("prompt", "PROMPTS", ["bg-workers"], "human_prompt")
        if parts[1] == "optimised_template_pipeline":
            if bn == "REPORT":
                return ("research", "RESEARCH", ["template-pipeline"], "ai_output")
            return ("prompt", "PROMPTS", ["template-pipeline"], "human_prompt")
        if parts[1] == "mmseqs2":
            if bn == "REPORT":
                return ("research", "RESEARCH", ["mmseqs2"], "ai_output")
            return ("prompt", "PROMPTS", ["mmseqs2"], "human_prompt")
        if parts[1] == "bwa-meme":
            if bn == "REPORT":
                return ("research", "RESEARCH", ["bwa-meme"], "ai_output")
            return ("prompt", "PROMPTS", ["bwa-meme"], "human_prompt")
        if parts[1] == "new_speedup":
            if "PROMPT" in bn_upper:
                return ("prompt", "PROMPTS", ["minimizer-speedup"], "human_prompt")
            if "REPORT" in bn_upper:
                return ("report", "REPORTS", ["minimizer-speedup"], "ai_output")
            return ("impl_plan", "IMPL_PLANS", ["minimizer-speedup"], "ai_output")
        # Fallback
        return ("research", "RESEARCH", [topic], "ai_output")

    # --- main/bioseq/RESEARCH/BENCHMARKING_SPEEDUP/ ---
    if rel.startswith("main/bioseq/RESEARCH/BENCHMARKING_SPEEDUP/"):
        if "SPEC_BENCHMARKING_SPEEDUP_PROPOSAL" in bn_upper:
            return ("tech_spec", "BENCHMARK_FRAMEWORK", ["benchmarking-speedup"], "ai_output")
        if "POOR_RESULTS_REPORT" in bn_upper:
            return ("report", "BENCHMARK_FRAMEWORK", ["benchmarking-speedup"], "ai_output")
        if "PROMPT_BUGFIXING" in bn_upper:
            return ("bugfix", "BUGFIX", ["benchmarking-speedup", "bugfix-prompt"], "human_prompt")
        if "PROMPT_CHARTS" in bn_upper or "PROMPT_CLEANUP" in bn_upper or bn == "PROMPT_1":
            return ("prompt", "BENCHMARK_FRAMEWORK", ["benchmarking-speedup"], "human_prompt")
        if "REPORT_FOONATHAN_MEM_BUGS" in bn_upper:
            return ("bugfix", "BUGFIX", ["memory-foonathan", "bugfix-report"], "ai_output")
        if "REPORT_PLOTLY" in bn_upper:
            return ("report", "BENCHMARK_FRAMEWORK", ["plotly", "benchmarking-speedup"], "ai_output")
        if "REPORT_POOR" in bn_upper:
            return ("report", "BENCHMARK_FRAMEWORK", ["benchmarking-speedup"], "ai_output")
        if "REPORT_V4_INSTRUMENTATION" in bn_upper:
            return ("report", "BENCHMARK_FRAMEWORK", ["benchmarking-speedup"], "ai_output")
        if "SIMDJSON_REF" in bn_upper:
            return ("external_doc", "EXTERNAL_DOCS", ["simdjson"], "external_dump")
        if "PLOTLY_REF" in bn_upper:
            return ("external_doc", "EXTERNAL_DOCS", ["plotly"], "external_dump")
        if "SOME_BENCHMARKING_PAPERS" in bn_upper:
            return ("research", "RESEARCH", ["bench-papers"], "ai_output")
        if "simd_minimizers" in bn.lower():
            return ("external_doc", "EXTERNAL_DOCS", ["minimizer-simd"], "external_dump")
        return ("impl_plan", "BENCHMARK_FRAMEWORK", ["benchmarking-speedup"], "ai_output")

    # --- main/bioseq/RESEARCH/ (top level) ---
    if rel.startswith("main/bioseq/RESEARCH/"):
        if "BLOCK_ALIGNER_IMPL" in bn_upper:
            return ("impl_plan", "IMPL_PLANS", ["block-aligner"], "ai_output")
        if "BLOCK_ALIGNER_PAPER" in bn_upper:
            return ("research", "RESEARCH", ["block-aligner"], "ai_output")
        if "BLOCK_ALIGNER_SPEC" in bn_upper:
            return ("tech_spec", "TECH_SPECS", ["block-aligner"], "ai_output")
        if "CAN_WE_REMOVE_LOAD_STORE" in bn_upper:
            return ("research", "RESEARCH", ["perf-load-store"], "ai_output")
        if "COMPILE_ERR_FIX" in bn_upper:
            return ("bugfix", "BUGFIX", ["compile-errors", "bugfix-prompt"], "human_prompt")
        if "DIFFERENT_OUTPUT_BUG_REPORT" in bn_upper:
            return ("bugfix", "BUGFIX", ["aligner", "bugfix-report"], "ai_output")
        if bn.startswith("ENGINEERING_REPORT"):
            return ("report", "REPORTS", ["engineering-overview"], "ai_output")
        if "FIX_KMERS_TESTS" in bn_upper:
            return ("bugfix", "BUGFIX", ["kmers", "tests", "bugfix-prompt"], "human_prompt")
        if "FIX_SIDECAR" in bn_upper:
            kind = "bugfix-report" if "IMPL_DETAILS" in bn_upper else "bugfix-prompt"
            return ("bugfix", "BUGFIX", ["sidecar", kind], "ai_output" if kind == "bugfix-report" else "human_prompt")
        if bn == "FIXING_PROMPT":
            return ("bugfix", "BUGFIX", ["aligner", "bugfix-prompt"], "human_prompt")
        if "HOW_COLUMNAR_WORKS" in bn_upper:
            return ("research", "RESEARCH", ["columnar"], "ai_output")
        if "HOW_TO_CONNECT_ALIGNER" in bn_upper:
            return ("prompt", "PROMPTS", ["aligner"], "human_prompt")
        if "IMPROVE_PLANS_PROMPT" in bn_upper or "IMPROVE_PROMPT" in bn_upper:
            return ("prompt", "PROMPTS", ["aligner-improve"], "human_prompt")
        if "IMPROVE_REPORT" in bn_upper:
            return ("report", "REPORTS", ["aligner-improve"], "ai_output")
        if "NEW_PROFILING_RESEARCH" in bn_upper:
            return ("research", "RESEARCH", ["profiling"], "ai_output")
        if bn.startswith("OPT_REPORT"):
            return ("report", "REPORTS", ["aligner-improve"], "ai_output")
        if "PARADEDB_PG_SEARCH" in bn_upper:
            return ("research", "RESEARCH", ["paradedb"], "ai_output")
        if bn == "PROMPT_IMPL":
            return ("prompt", "PROMPTS", ["aligner"], "human_prompt")
        if "PROMPT_IMPROVE_BUILD_SH" in bn_upper:
            return ("prompt", "PROMPTS", ["build"], "human_prompt")
        if "PROMPT_INTEGRATION_TESTING" in bn_upper:
            return ("prompt", "PROMPTS", ["testing"], "human_prompt")
        if "PROMPT_KSW_ORIGINAL_SEGFAULT" in bn_upper:
            return ("bugfix", "BUGFIX", ["ksw2", "bugfix-prompt"], "human_prompt")
        if "PROMPT_POSTGRES_QUERY_BUGFIXING" in bn_upper:
            return ("bugfix", "BUGFIX", ["postgres-query", "bugfix-prompt"], "human_prompt")
        if "PROMPT_REFACTOR_ALIGNERS" in bn_upper:
            return ("prompt", "PROMPTS", ["aligner"], "human_prompt")
        if "PROMPT_RESEARCH_CALIPER" in bn_upper:
            return ("prompt", "PROMPTS", ["caliper-profiler"], "human_prompt")
        if "PROMPT_SETTING_UP_BENCH" in bn_upper:
            return ("prompt", "PROMPTS", ["bench-framework"], "human_prompt")
        if "PROMPT_STORE_MINIMIZERS" in bn_upper:
            return ("prompt", "PROMPTS", ["minimizer-storage"], "human_prompt")
        if "REFACTOR_PG_PROMPT" in bn_upper:
            return ("prompt", "PROMPTS", ["postgres-query"], "human_prompt")
        if "REPORT_BENCH_IMPROVEMENTS" in bn_upper:
            return ("report", "BENCHMARK_FRAMEWORK", ["bench-framework"], "ai_output")
        if "REPORT_POSTGRES_QUERY_BUGFIXING" in bn_upper:
            return ("bugfix", "BUGFIX", ["postgres-query", "bugfix-report"], "ai_output")
        if "RESEARCH_BLOCK_ALIGNER" in bn_upper:
            return ("prompt", "PROMPTS", ["block-aligner"], "human_prompt")
        if "RESEARCH_COLUMNAR_SOURCES" in bn_upper:
            return ("research", "RESEARCH", ["columnar"], "ai_output")
        if "RESEARCH_FOONATHAN_MEMORY" in bn_upper:
            return ("research", "RESEARCH", ["memory-foonathan"], "ai_output")
        if "RESEARCH_MM_FAST" in bn_upper:
            return ("research", "RESEARCH", ["mm-fast"], "ai_output")
        if bn == "RESEARCH":
            return ("prompt", "PROMPTS", ["aligner"], "human_prompt")
        if "SEAL_REPORT_BENCHMARKING" in bn_upper:
            return ("report", "REPORTS", ["seal"], "ai_output")
        if "SIMD_ACCELERATED_AND_RMQ_CHAINING" in bn_upper:
            return ("research", "RESEARCH", ["chaining"], "ai_output")
        if "SPEC_HOW_TO_CONNECT_ALIGNER" in bn_upper:
            return ("tech_spec", "TECH_SPECS", ["aligner"], "ai_output")
        if "SPEC_IMPL_PROMPT" in bn_upper:
            return ("prompt", "PROMPTS", ["aligner"], "human_prompt")
        if "SPEC_MEMORY_ALLOCATORS" in bn_upper:
            return ("tech_spec", "TECH_SPECS", ["memory-allocators"], "ai_output")
        if "SPEC_MINIMIZER_STORAGE" in bn_upper:
            return ("tech_spec", "TECH_SPECS", ["minimizer-storage"], "ai_output")
        if "SUMMARIZE_BENCHMARKING_MINIMIZERS_WORK" in bn_upper:
            return ("report", "REPORTS", ["minimizer"], "ai_output")
        if "WHY_KMERS_TESTS_FAILED" in bn_upper:
            return ("bugfix", "BUGFIX", ["kmers", "bugfix-report"], "ai_output")
        if "WHY_KSW2_SEGFAULTS" in bn_upper:
            return ("bugfix", "BUGFIX", ["ksw2", "bugfix-report"], "ai_output")
        if "ADDING_PROFILER" in bn_upper:
            return ("prompt", "PROMPTS", ["profiling"], "human_prompt")
        return ("report", "REPORTS", ["engineering-overview"], "ai_output")

    return ("prompt", "PROMPTS", ["engineering-overview"], "human_prompt")

# ---------------------------------------------------------------------------
# 3. Slug derivation
# ---------------------------------------------------------------------------

def slugify(name: str) -> str:
    """Uppercase snake from arbitrary name."""
    s = re.sub(r"[^A-Za-z0-9]+", "_", name)
    s = re.sub(r"_+", "_", s).strip("_")
    return s.upper()

def parent_slug(rel: str) -> str:
    parent = os.path.dirname(rel)
    if not parent: return ""
    last = os.path.basename(parent)
    return slugify(last)

def derive_slug(rel: str) -> str:
    bn = base_name(rel)
    s = slugify(bn)
    # Special case for LAUD.lol → AI_GUIDELINES
    if rel == "LAUD.lol":
        return "AI_GUIDELINES"
    # Generic-name rule
    must_prefix = s in GENERIC_SET or re.match(r"^(PROMPT|REPORT|PLAN|IMPL_PLAN)_\d", s) or re.match(r"^PROMPT_V\d", s) or re.match(r"^IMPL_PROMPT_V\d", s)
    if must_prefix:
        p = parent_slug(rel)
        if p:
            return f"{p}_{s}"
    return s

# ---------------------------------------------------------------------------
# 4. Frontmatter generation
# ---------------------------------------------------------------------------

def yaml_quote(s: str) -> str:
    s = s.replace('"', '\\"').replace("\n", " ").strip()
    return f'"{s}"'

def yaml_list(xs: list[str]) -> str:
    return "[" + ", ".join(xs) + "]"

def build_frontmatter(doc_type: str, date_dd_mm: str, first_commit: str | None,
                       origin: str, doc_id: str, topics: list[str], summary: str) -> str:
    lines = ["---"]
    lines.append(f"type: {doc_type}")
    lines.append(f"date: {date_dd_mm}")
    if first_commit:
        lines.append(f"first_commit: {first_commit}")
    lines.append(f"origin: {origin}")
    lines.append(f"doc_id: {doc_id}")
    lines.append(f"topics: {yaml_list(topics)}")
    lines.append(f"summary: {yaml_quote(summary)}")
    lines.append("status: active")
    lines.append("---")
    return "\n".join(lines) + "\n\n"

def auto_summary(head: str, fallback_title: str) -> str:
    """Extract a one-line summary from the head of the original file."""
    if not head:
        return fallback_title
    lines = [l.strip() for l in head.splitlines() if l.strip()]
    # Skip H1 title line if present, prefer first prose paragraph
    candidates = []
    for line in lines:
        if line.startswith("#"):
            continue
        if line.startswith(">"):
            continue
        if line.startswith("|"):
            continue
        candidates.append(line)
    text = " ".join(candidates[:2]) if candidates else (lines[0] if lines else fallback_title)
    text = re.sub(r"\s+", " ", text)
    text = text.strip("*_`-: ")
    if len(text) > 195:
        text = text[:192] + "..."
    return text or fallback_title

# ---------------------------------------------------------------------------
# 5. Main pipeline
# ---------------------------------------------------------------------------

def load_survey(path: Path) -> list[dict]:
    with open(path) as f:
        return json.load(f)

def assign_doc_ids(records: list[dict]) -> list[dict]:
    """Sort by (first_commit_date_asc, rel_path_asc) and assign DOC0001..N."""
    def sort_key(r):
        fc = r.get("first_commit", "")
        return (fc or "9999-99-99", r["rel"])
    sorted_records = sorted(records, key=sort_key)
    for i, r in enumerate(sorted_records, start=1):
        r["doc_id"] = f"DOC{i:04d}"
    return sorted_records

def parse_date(s: str) -> tuple[str, str]:
    """Return (DD-MM, YYYY-MM-DD) from a git ISO timestamp."""
    if not s:
        return ("01-01", "")
    try:
        # Format: "2026-05-10 13:38:00 +0200"
        dt_part = s.split(" ")[0]
        y, m, d = dt_part.split("-")
        return (f"{d}-{m}", dt_part)
    except Exception:
        return ("01-01", "")

def build_migration(survey_path: Path) -> list[dict]:
    records = load_survey(survey_path)
    # Filter to actually in-scope files
    in_scope = []
    for r in records:
        rel = r["rel"]
        if rel in KEEP_IN_PLACE:
            continue
        in_scope.append(r)
    # Drop the deletion target (README.md) from the renaming set later
    in_scope = assign_doc_ids(in_scope)

    plan = []
    for r in in_scope:
        rel = r["rel"]
        t, dir_class, topics, origin = classify(r)
        if t == "DELETE":
            plan.append({
                "old": rel, "action": "delete", "new": None, "doc_id": None,
                "type": None, "topics": [], "origin": "",
                "summary": "", "first_commit": "", "date": "",
            })
            continue
        # Importance-class override (already encoded in the dir_class from classify())
        directory = dir_class
        slug = derive_slug(rel)
        new_rel = f"main/DOCS/{directory}/{slug}__{r['doc_id']}.md"
        dd_mm, ymd = parse_date(r.get("first_commit", ""))
        summary = auto_summary(r.get("head", ""), slug.replace("_", " ").title())
        plan.append({
            "old": rel,
            "action": "move",
            "new": new_rel,
            "doc_id": r["doc_id"],
            "type": t,
            "topics": topics,
            "origin": origin or "ai_output",
            "summary": summary,
            "first_commit": ymd,
            "date": dd_mm,
        })

    # Assert uniqueness of new paths
    seen = {}
    for p in plan:
        if p["action"] != "move": continue
        if p["new"] in seen:
            raise RuntimeError(f"Slug collision: {p['new']} from both {seen[p['new']]} and {p['old']}")
        seen[p["new"]] = p["old"]

    return plan

def write_migration_map(plan: list[dict], path: Path) -> None:
    lines = []
    for p in plan:
        if p["action"] == "delete":
            lines.append(f"{p['old']}\tDELETE")
        else:
            lines.append(f"{p['old']}\t{p['new']}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")

def write_migration_summary(plan: list[dict], path: Path) -> None:
    """Group by target directory and print a table."""
    from collections import defaultdict
    by_dir = defaultdict(list)
    for p in plan:
        if p["action"] == "delete":
            by_dir["__DELETE__"].append(p)
        else:
            d = os.path.dirname(p["new"]).replace("main/DOCS/", "")
            by_dir[d].append(p)
    lines = ["# Migration summary", ""]
    for d in sorted(by_dir.keys()):
        files = by_dir[d]
        lines.append(f"## {d} ({len(files)} files)")
        lines.append("")
        lines.append("| DOC | new filename | type | topics | summary |")
        lines.append("|-----|--------------|------|--------|---------|")
        for p in sorted(files, key=lambda x: x.get("doc_id") or ""):
            if p["action"] == "delete":
                lines.append(f"| -   | {p['old']} | DELETE | -      | -       |")
            else:
                doc_id = p['doc_id']
                fname = os.path.basename(p['new'])
                topics = ", ".join(p['topics'])
                summary = p['summary'].replace('|', '\\|')[:120]
                lines.append(f"| {doc_id} | {fname} | {p['type']} | {topics} | {summary} |")
        lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))

def execute_migration(plan: list[dict]) -> None:
    """Perform the actual git mv and frontmatter writing."""
    # 1. Ensure target directories exist (git tracks files, not dirs)
    dirs_needed = set()
    for p in plan:
        if p["action"] == "move":
            dirs_needed.add(os.path.dirname(p["new"]))
    for d in dirs_needed:
        (ROOT / d).mkdir(parents=True, exist_ok=True)

    # 2. Delete the deletions
    for p in plan:
        if p["action"] == "delete":
            old = ROOT / p["old"]
            if old.exists():
                # Use git rm to keep history
                subprocess.run(["git", "rm", "-f", p["old"]], cwd=ROOT, check=True)

    # 3. Move files via git mv (preserves history)
    for p in plan:
        if p["action"] != "move":
            continue
        old = ROOT / p["old"]
        new = ROOT / p["new"]
        if not old.exists():
            print(f"WARN: {p['old']} missing, skipping")
            continue
        # Use git mv if file is tracked, else plain mv
        try:
            subprocess.run(
                ["git", "ls-files", "--error-unmatch", p["old"]],
                cwd=ROOT, check=True, capture_output=True
            )
            subprocess.run(["git", "mv", p["old"], p["new"]], cwd=ROOT, check=True)
        except subprocess.CalledProcessError:
            # Not tracked, just rename
            old.rename(new)

    # 4. Prepend frontmatter to each moved file
    for p in plan:
        if p["action"] != "move":
            continue
        new = ROOT / p["new"]
        if not new.exists():
            continue
        body = new.read_text(encoding="utf-8", errors="replace")
        # Strip any pre-existing frontmatter (start with --- and end with ---)
        if body.startswith("---\n"):
            end_idx = body.find("\n---\n", 4)
            if end_idx != -1:
                body = body[end_idx + 5:]
        fm = build_frontmatter(
            doc_type=p["type"],
            date_dd_mm=p["date"],
            first_commit=p["first_commit"],
            origin=p["origin"],
            doc_id=p["doc_id"],
            topics=p["topics"],
            summary=p["summary"],
        )
        new.write_text(fm + body, encoding="utf-8")

def write_counter(plan: list[dict]) -> None:
    max_id = 0
    for p in plan:
        if p.get("doc_id"):
            n = int(p["doc_id"][3:])
            if n > max_id: max_id = n
    DOCS_ROOT.mkdir(parents=True, exist_ok=True)
    (DOCS_ROOT / ".doc_counter").write_text(str(max_id) + "\n")

def write_topics_file() -> None:
    topics = [
        "aligner", "aligner-improve", "ai-collab", "bench-framework", "bench-papers",
        "benchmarking-speedup", "bg-workers", "block-aligner", "bugfix-plan",
        "bugfix-prompt", "bugfix-report", "build", "build-mcp", "bwa-meme",
        "caliper-profiler", "catch2", "chaining", "code-readme", "code-style",
        "columnar", "compile-errors", "engineering-overview", "gzip-io",
        "highway-simd", "io-reader", "json-schema", "kmers", "kperf-profiler",
        "ksw2", "mcp", "memory-allocators", "memory-foonathan", "minimap2",
        "minimizer", "minimizer-simd", "minimizer-speedup", "minimizer-storage",
        "mmseqs2", "mm-fast", "paradedb", "perf-load-store", "perf-strange-timing",
        "plotly", "postgres-query", "profiling", "seal", "setup", "sidecar",
        "simdjson", "simd-sort", "sneaky-snake", "template-pipeline", "tests",
        "testing", "todo", "viewer",
    ]
    DOCS_ROOT.mkdir(parents=True, exist_ok=True)
    (DOCS_ROOT / ".topics").write_text("\n".join(topics) + "\n")

def regenerate_index(plan: list[dict]) -> None:
    from collections import defaultdict
    by_dir = defaultdict(list)
    for p in plan:
        if p["action"] != "move":
            continue
        d = os.path.dirname(p["new"]).replace("main/DOCS/", "")
        by_dir[d].append(p)
    lines = ["# DOCS Index", "", "Auto-generated by `migrate_docs.py` / `newdoc.py index`.", ""]
    lines.append("All documents in `main/DOCS/`, grouped by directory.")
    lines.append("Within a directory, the file with the **highest DOC id** is the most recent.")
    lines.append("")
    for d in sorted(by_dir.keys()):
        files = by_dir[d]
        lines.append(f"## {d} ({len(files)} files)")
        lines.append("")
        lines.append("| DOC | filename | type | topics | summary |")
        lines.append("|-----|----------|------|--------|---------|")
        for p in sorted(files, key=lambda x: x['doc_id'], reverse=True):
            fname = os.path.basename(p['new'])
            topics = ", ".join(p['topics'])
            summary = p['summary'].replace('|', '\\|')[:140]
            lines.append(f"| {p['doc_id']} | {fname} | {p['type']} | {topics} | {summary} |")
        lines.append("")
    (DOCS_ROOT / "INDEX.md").write_text("\n".join(lines))

def write_docs_readme() -> None:
    content = """# main/DOCS — Documentation hub

This directory holds all the prompts, reports, tech specs, and external doc dumps
for the bioseqdb project. The structure is described in `../REORG_PLAN.md` (or
its migrated version under `HUMAN_NOTES/`).

## Quick reference for AI agents

* **Read code-style / AI rules first**: open `GUIDELINES/*` before doing anything else.
* **Find a doc by topic**: open `INDEX.md`, search for `topics: <your-topic>`.
* **Find the latest version**: within a directory, the file with the **highest DOC<NNNN>**
  is the most recent.
* **Create a new doc**: run `python3 ../../newdoc.py <type> "<title>" --topics tag1,tag2`.
  The script prints the absolute path of the new file and refreshes `INDEX.md`.
* **Never edit `doc_id`** once a doc is created. Never reuse a deleted doc's id.

## Directory layout

| Directory               | Holds                                                                      |
|-------------------------|----------------------------------------------------------------------------|
| `GUIDELINES/`           | Always-read code style and AI guidelines.                                  |
| `HUMAN_NOTES/`          | Human-curated notes (TODOs, setup, planning artifacts).                    |
| `TECH_SPECS/`           | Definitive AI-generated tech specs (sources of truth for designs).         |
| `RESEARCH/`             | AI digests of papers / libraries / techniques (long half-life).            |
| `REPORTS/`              | AI audit/quality/regression reports.                                       |
| `IMPL_PLANS/`           | AI implementation plans (often superseded; check highest DOC).             |
| `PROMPTS/`              | Archived human-written prompts (replay intent).                            |
| `EXTERNAL_DOCS/`        | Copies of upstream documentation (Plotly, Highway, foonathan-mem, ...).    |
| `BUGFIX/`               | Less-important bugfix prompts/reports/plans.                               |
| `BENCHMARK_FRAMEWORK/`  | Least-important benchmark-framework implementation history.                |

## Frontmatter cheat-sheet

Every file starts with:

```yaml
---
type: impl_plan
date: 10-05
first_commit: 2026-05-10        # migrated docs only
origin: ai_output
doc_id: DOC0198
topics: [bench-framework, simd]
summary: "One-line description, ≤200 chars."
status: active
---
```

Allowed `type` values: `guidelines`, `human_notes`, `tech_spec`, `research`, `report`,
`impl_plan`, `prompt`, `external_doc`, `bugfix`.

Allowed `origin` values: `ai_output`, `human_prompt`, `external_dump`, `human_notes`, `mixed`.

Allowed topic tags: see `.topics` (one per line).
"""
    (DOCS_ROOT / "README.md").write_text(content)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--execute", action="store_true")
    ap.add_argument("--survey", default="/tmp/md_survey.json")
    args = ap.parse_args()
    if not (args.dry_run or args.execute):
        ap.error("must specify --dry-run or --execute")

    plan = build_migration(Path(args.survey))
    print(f"Plan computed: {len(plan)} entries.")

    DOCS_ROOT.mkdir(parents=True, exist_ok=True)
    write_migration_map(plan, DOCS_ROOT / ".migration_map.txt")
    write_migration_summary(plan, DOCS_ROOT / ".migration_summary.md")
    print(f"Wrote {DOCS_ROOT / '.migration_map.txt'}")
    print(f"Wrote {DOCS_ROOT / '.migration_summary.md'}")

    if args.dry_run:
        print("Dry run complete; review .migration_summary.md, then run --execute.")
        return

    # Execute
    print("Executing migration…")
    execute_migration(plan)
    write_counter(plan)
    write_topics_file()
    regenerate_index(plan)
    write_docs_readme()
    print("Migration complete.")

if __name__ == "__main__":
    main()
