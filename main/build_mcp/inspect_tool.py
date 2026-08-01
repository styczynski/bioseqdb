#!/usr/bin/env python3
"""bioseq-build MCP `inspect` tool.

Filename note: the plan (IMPROVE_BUILD_MCP_IMPL_PLAN_1.md §6) calls this
`inspect.py`. Renamed to `inspect_tool.py` so it doesn't shadow Python's
stdlib `inspect` when `server.py` is run directly (the script's
directory becomes sys.path[0], ahead of stdlib).

Inspects a function in the selected production or isolated test build by
combining:

  * post-LTO disassembly from `objdump --disassemble-symbols=<mangled>` —
    the binary ground-truth view of what actually runs;
  * pre-LTO per-TU `.s` listing extracted around the same `<mangled>:`
    label — the compiler's per-TU intent, with `.loc` source-line
    annotations preserved;
  * an arch-aware instruction histogram (total / SIMD / branches /
    loads / stores / atomics / NEON struct loadstore) so the AI agent
    can spot vectorisation regressions, register spills, branch-pattern
    changes etc. at a glance;
  * an inline-site sweep for `[[gnu::always_inline]]` functions that
    have no standalone symbol — counts `.loc <fileidx>` directives
    whose `<fileidx>` resolves to the source basename (primary signal,
    works at -O3) and `DEBUG_VALUE: <fname>:` annotations (secondary,
    often empty at clang -O3 due to `-discard-value-names`).

Returns one JSON document containing linked and compiler-listing evidence.

Latency story: exact symbol labels and an explicitly supplied source basename
prefilter the compiler listings in native code when ripgrep is available.
Only matching listings enter the Python parser. The lazy llc fallback remains
disk-cached and invalidates on .bc mtime. One assembly-phase deadline covers
both prefiltering and every streamed fallback chunk; expiry returns explicitly
incomplete evidence instead of overrunning the MCP request budget.

Memory contract: this module is invoked from the long-lived MCP server.
Compiler `.s` files can be several gigabytes. Candidate files are parsed once
in fixed-size chunks, retained physical lines are capped, and only requested
snippets plus aggregate `.file`/`.loc` state remain in memory. See plan
Risk #16.

Standalone CLI usage (kept for unit testing — the MCP server imports
`run_inspect` directly):

    python3 main/build_mcp/inspect_tool.py \\
        --build-dir main/build/bench-opt \\
        --target bioseq_bench \\
        --function 'collect_minimizers_two_stack_nthash_simd' \\
        --source-basename two_stacks_minimizers.cppm
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional


# --------------------------- defaults --------------------------------------

DEFAULT_SCRATCH_DIR = Path("/tmp/bioseq_inspect")

# Per-subprocess timeouts. nm + cxxfilt are fast; objdump per-symbol can be
# slow on huge LTO functions; llc is the slowest (~25 s per bench-main TU).
NM_TIMEOUT_SEC = 60
OBJDUMP_TIMEOUT_SEC = 120
DEMANGLER_TIMEOUT_SEC = 60
LLC_TIMEOUT_SEC = 300
HELP_TIMEOUT_SEC = 5
ASSEMBLY_PHASE_TIMEOUT_SEC = 120

# Assembly listings can be multiple gigabytes. Reads and retained line state
# stay bounded even if a malformed listing contains a line without a newline.
ASSEMBLY_READ_CHUNK_BYTES = 1024 * 1024
ASSEMBLY_MAX_RETAINED_LINE_BYTES = 1024 * 1024
ASSEMBLY_RG_MAX_ARG_BYTES = 24 * 1024
ASSEMBLY_MAX_SOURCE_STATE_ENTRIES = 16 * 1024
ASSEMBLY_MAX_SOURCE_STATE_BYTES = 4 * 1024 * 1024

# Request/result bounds. Full objdump output remains on disk and full pre-LTO
# listings remain in the build tree; these limits apply only to JSON snippets
# and in-memory source-index bookkeeping.
MIN_INSPECT_MATCHES = 1
MAX_INSPECT_MATCHES = 64
MIN_INSPECT_SNIPPET_LINES = 1
MAX_INSPECT_SNIPPET_LINES = 2000
MAX_RETAINED_SNIPPET_BYTES = 256 * 1024
MAX_TOTAL_RETAINED_SNIPPET_BYTES = 1024 * 1024


class AssemblyPhaseDeadlineExceeded(RuntimeError):
    """Raised internally when bounded source/pre-LTO work exhausts its budget."""

# Skip a `.bc` whose mtime is within this many seconds of "now" — protects
# the lazy llc path from racing a concurrent ninja write. See plan Risk #9.
BC_MTIME_GRACE_SEC = 5

# Cap on top-level `total_matches_before_cap` past which we add a
# user-facing "narrow your search" warning.
MATCHES_WARN_THRESHOLD = 50


# --------------------------- tool detection --------------------------------

PREFERRED_NM = ["llvm-nm", "nm"]
PREFERRED_OBJDUMP = ["llvm-objdump", "objdump"]
PREFERRED_DEMANGLER = ["llvm-cxxfilt", "c++filt"]
PREFERRED_LLC = ["llc"]
PREFERRED_RG = ["rg"]

# Probed only as a last resort — `which` already covers PATH. These exist
# because Apple's developer Xcode includes its own LLVM tools at
# /usr/bin, but Homebrew installs LLVM at non-standard prefixes that
# users sometimes don't add to PATH.
LLVM_HINTS = [
    "/usr/local/opt/llvm@21/bin",
    "/usr/local/opt/llvm/bin",
    "/opt/homebrew/opt/llvm@21/bin",
    "/opt/homebrew/opt/llvm/bin",
]


def _which(name: str) -> Optional[str]:
    p = shutil.which(name)
    if p:
        return p
    for d in LLVM_HINTS:
        cand = Path(d) / name
        if cand.exists() and os.access(cand, os.X_OK):
            return str(cand)
    return None


def detect_tools() -> dict:
    """Resolve every helper binary we may need. Missing tools come back as
    None so the caller can decide whether to degrade gracefully or error."""
    return {
        "nm": next((p for p in (_which(n) for n in PREFERRED_NM) if p), None),
        "objdump": next((p for p in (_which(n) for n in PREFERRED_OBJDUMP) if p), None),
        "demangler": next((p for p in (_which(n) for n in PREFERRED_DEMANGLER) if p), None),
        "llc": next((p for p in (_which(n) for n in PREFERRED_LLC) if p), None),
        "rg": next((p for p in (_which(n) for n in PREFERRED_RG) if p), None),
    }


def tool_supports(tool_path: Optional[str], flag: str) -> bool:
    if not tool_path:
        return False
    try:
        out = subprocess.run([tool_path, "--help"],
                             capture_output=True, text=True,
                             timeout=HELP_TIMEOUT_SEC)
        return flag in (out.stdout + out.stderr)
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return False


# --------------------------- platform / arch -------------------------------

# Mach-O cputype values (mach/machine.h). Top byte CPU_ARCH_ABI64=0x01000000
# is OR'd into the 64-bit variants.
_MACHO_CPU_X86_64 = 0x01000007
_MACHO_CPU_ARM64 = 0x0100000C

# ELF e_machine values (elf.h)
_ELF_EM_X86_64 = 62
_ELF_EM_AARCH64 = 183


def detect_object_format(path: Path) -> str:
    try:
        with path.open("rb") as f:
            magic = f.read(4)
    except OSError:
        return "unknown"
    if magic == b"\x7fELF":
        return "elf"
    if magic in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
                 b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce",
                 b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
        return "mach-o"
    return "unknown"


def detect_arch(path: Path, obj_format: str) -> str:
    """Best-effort arch detection from the binary's own header.

    Falls back to "unknown" rather than guessing — the histogram path uses
    the arch hint to pick the per-class mnemonic table; classifying ARM
    instructions as x86 (or vice versa) silently zeroes most counts.
    """
    try:
        with path.open("rb") as f:
            head = f.read(64)
    except OSError:
        return "unknown"
    if len(head) < 8:
        return "unknown"
    if obj_format == "mach-o":
        # 64-bit Mach-O: magic FEEDFACF (LE) -> cputype at offset 4 (LE int32).
        # Universal (fat) binaries (CAFEBABE) carry per-arch slices; we only
        # peek at the first slice's cputype, which is "good enough" for the
        # AI's class-table choice.
        if head[:4] == b"\xcf\xfa\xed\xfe" or head[:4] == b"\xfe\xed\xfa\xcf":
            cputype = struct.unpack_from("<i", head, 4)[0]
            if cputype == _MACHO_CPU_X86_64:
                return "x86_64"
            if cputype == _MACHO_CPU_ARM64:
                return "arm64"
        # Fat header: skip into first arch slice. cputype at offset 8.
        if head[:4] in (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
            cputype = struct.unpack_from(">i", head, 8)[0]
            if cputype == _MACHO_CPU_X86_64:
                return "x86_64"
            if cputype == _MACHO_CPU_ARM64:
                return "arm64"
    elif obj_format == "elf":
        # ELF e_machine at offset 18 (LE int16). All bioseq targets are LE.
        if len(head) >= 20:
            e_machine = struct.unpack_from("<H", head, 18)[0]
            if e_machine == _ELF_EM_X86_64:
                return "x86_64"
            if e_machine == _ELF_EM_AARCH64:
                return "arm64"
    return "unknown"


# --------------------------- symbol enumeration ----------------------------

def list_symbols(nm_path: str, binary: Path,
                 demangler: Optional[str],
                 warnings: list[str]) -> list[dict]:
    """Return [{address, size, type, mangled, demangled}] sorted by address.

    Mach-O `nm --print-size` always returns size=0; we compute size as the
    delta to the next-defined-symbol address. ELF `nm` returns sizes
    directly but the address-sweep gives us the same answer either way.

    Demangling is a SEPARATE pass through `c++filt` (rather than asking nm
    to demangle), because llvm-nm's `--demangle` re-orders / coalesces some
    weak symbols, breaking row-alignment with the mangled output. See plan
    Risk #8.

    `warnings` is appended to (not raised on) for non-fatal issues
    (cxxfilt timeout, cxxfilt rc!=0, row-count mismatch). nm itself is
    fatal — without a symbol table there's nothing to inspect.
    """
    raw_proc = subprocess.run(
        [nm_path, "--defined-only", str(binary)],
        capture_output=True, text=True, timeout=NM_TIMEOUT_SEC)
    if raw_proc.returncode != 0:
        raise RuntimeError(
            f"nm failed (rc={raw_proc.returncode}): "
            f"{raw_proc.stderr.strip()[:500]}")

    rows: list[tuple[int, str, str]] = []
    for line in raw_proc.stdout.splitlines():
        parts = line.strip().split(None, 2)
        if len(parts) != 3:
            continue
        addr_s, ty, name = parts
        try:
            addr = int(addr_s, 16)
        except ValueError:
            continue
        rows.append((addr, ty, name))
    rows.sort(key=lambda r: r[0])

    mangled_list = [r[2] for r in rows]
    demangled_names = mangled_list[:]  # safe default if anything below fails

    if demangler and mangled_list:
        mangled_text = "\n".join(mangled_list) + "\n"
        try:
            result = subprocess.run(
                [demangler], input=mangled_text,
                capture_output=True, text=True,
                timeout=DEMANGLER_TIMEOUT_SEC)
        except subprocess.TimeoutExpired:
            warnings.append(
                f"demangler ({demangler}) timed out after "
                f"{DEMANGLER_TIMEOUT_SEC}s on {len(mangled_list)} symbols; "
                "falling back to mangled-name match")
            result = None

        if result is not None:
            if result.returncode != 0:
                warnings.append(
                    f"demangler exited rc={result.returncode}: "
                    f"{result.stderr.strip()[:200]}; falling back to "
                    "mangled-name match")
            else:
                cooked = result.stdout.rstrip("\n").split("\n")
                # cxxfilt should output one line per input; defend against
                # the case where it emits one extra blank trailer.
                if len(cooked) == len(rows) + 1 and not cooked[-1]:
                    cooked = cooked[:-1]
                if len(cooked) == len(rows):
                    demangled_names = cooked
                else:
                    warnings.append(
                        f"demangler row count {len(cooked)} != "
                        f"symbol count {len(rows)}; falling back to "
                        "mangled-name match")

    out = []
    for i, (addr, ty, mangled) in enumerate(rows):
        size = (rows[i + 1][0] - addr) if i + 1 < len(rows) else None
        out.append({"address": addr, "size": size, "type": ty,
                    "mangled": mangled, "demangled": demangled_names[i]})
    return out


# --------------------------- assembly listing scan -------------------------

FILE_DIR_RE = re.compile(
    r'^\s*\.file\s+(\d+)(?:\s+"([^"]*)")?\s+"([^"]*)"\s*(?:md5\s+0x[0-9a-fA-F]+)?\s*$'
)
LOC_RE = re.compile(r'^\s*\.loc\s+(\d+)\s+(\d+)')
# Match `DEBUG_VALUE: <name>[<template-args>]:<var>`.
# `<name>` may contain `::` (e.g. std::vector::push_back) — we treat the
# end of the name as `:` that is NOT immediately followed by another `:`.
# See plan Risk #14.
DEBUG_VALUE_RE = re.compile(
    r"DEBUG_VALUE:\s+((?:[^<:]|::)+?)(?:<[^>]*>)?:(?!:)")


def _symbol_listing_labels(mangled: str,
                           exact_match_labels: set[str]) -> tuple[str, ...]:
    """Return the exact label plus an unambiguous Mach-O alternate.

    Adding/removing one underscore is accepted only when that alternate is not
    another matched symbol's exact spelling. This prevents `_foo` and `foo`
    from both claiming whichever body happens to appear first.
    """
    alternate = mangled[1:] if mangled.startswith("_") else "_" + mangled
    labels = [mangled]
    if alternate not in exact_match_labels:
        labels.append(alternate)
    return tuple(dict.fromkeys(labels))


def _assembly_candidate_pattern(matches: list[dict],
                                source_basename: Optional[str]) -> str:
    """Build the ripgrep expression used to find relevant `.s` files.

    Symbol alternatives are exact labels at column zero. A symbol prefix is
    not sufficient: the colon must be followed by whitespace or end-of-line.
    Source matching is included only when the caller supplied a basename.
    """
    exact_match_labels = {match["mangled"] for match in matches}
    labels = tuple(dict.fromkeys(
        label
        for match in matches
        for label in _symbol_listing_labels(
            match["mangled"], exact_match_labels)
    ))
    alternatives: list[str] = []
    if labels:
        alternatives.append(
            r"^(?:" + "|".join(re.escape(label) for label in labels)
            + r"):(?:\s|$)")
    if source_basename:
        alternatives.append(re.escape(source_basename))
    return "|".join(f"(?:{item})" for item in alternatives)


def _unique_paths(paths: list[Path]) -> list[Path]:
    return list(dict.fromkeys(paths))


def _argv_size_bytes(arguments: list[str]) -> int:
    """Conservative cross-platform encoded command-line size estimate."""
    return sum(len(os.fsencode(argument)) + 3 for argument in arguments)


def _rg_path_batches(paths: list[Path], base_command: list[str]
                     ) -> Optional[list[list[Path]]]:
    """Batch paths so every rg argv remains below the Windows-safe limit."""
    base_bytes = _argv_size_bytes(base_command)
    if base_bytes >= ASSEMBLY_RG_MAX_ARG_BYTES:
        return None
    batches: list[list[Path]] = []
    current: list[Path] = []
    current_bytes = base_bytes
    for path in paths:
        path_bytes = len(os.fsencode(path)) + 3
        if current and current_bytes + path_bytes > ASSEMBLY_RG_MAX_ARG_BYTES:
            batches.append(current)
            current = []
            current_bytes = base_bytes
        if current_bytes + path_bytes > ASSEMBLY_RG_MAX_ARG_BYTES:
            return None
        current.append(path)
        current_bytes += path_bytes
    if current:
        batches.append(current)
    return batches


def prefilter_assembly_files(
    s_files: list[Path],
    matches: list[dict],
    source_basename: Optional[str],
    rg_path: Optional[str],
    warnings: list[str],
    phase_deadline: Optional[float] = None,
) -> tuple[list[Path], str]:
    """Return `.s` files that can contain requested source/pre-LTO evidence.

    ripgrep performs the potentially multi-gigabyte discovery scan in bounded
    native memory. If it is absent or fails, correctness wins: every listing
    is returned and the bounded streaming parser below acts as the fallback.
    """
    unique_files = _unique_paths(s_files)
    pattern = _assembly_candidate_pattern(matches, source_basename)
    if not unique_files or not pattern:
        return [], "not-needed"
    if not rg_path:
        return unique_files, "streaming-fallback"

    matched_paths: set[Path] = set()
    by_spelling = {str(path): path for path in unique_files}
    by_absolute = {os.path.abspath(path): path for path in unique_files}
    base_command = [
        rg_path, "--files-with-matches", "--text", "--no-messages",
        "--regexp", pattern, "--",
    ]
    batches = _rg_path_batches(unique_files, base_command)
    if batches is None:
        warnings.append(
            "assembly prefilter query exceeds the 24-KiB argv budget; using "
            "bounded streaming fallback")
        return unique_files, "streaming-fallback"
    deadline = (phase_deadline if phase_deadline is not None
                else time.monotonic() + ASSEMBLY_PHASE_TIMEOUT_SEC)
    try:
        for batch in batches:
            timeout_remaining = deadline - time.monotonic()
            if timeout_remaining <= 0:
                raise subprocess.TimeoutExpired(
                    base_command, ASSEMBLY_PHASE_TIMEOUT_SEC)
            proc = subprocess.run(
                [*base_command, *(str(path) for path in batch)],
                capture_output=True,
                text=True,
                timeout=timeout_remaining,
            )
            if proc.returncode not in (0, 1):
                warnings.append(
                    f"assembly prefilter failed (rc={proc.returncode}): "
                    f"{proc.stderr.strip()[:200]}; using bounded streaming "
                    "fallback")
                return unique_files, "streaming-fallback"
            for line in proc.stdout.splitlines():
                candidate = by_spelling.get(line)
                if candidate is None:
                    candidate = by_absolute.get(os.path.abspath(line))
                if candidate is not None:
                    matched_paths.add(candidate)
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError) as exc:
        warnings.append(
            f"assembly prefilter unavailable ({exc}); using bounded "
            "streaming fallback")
        return unique_files, "streaming-fallback"

    return ([path for path in unique_files if path in matched_paths],
            "ripgrep")


def _bounded_binary_lines(stream: Any, stats: dict,
                          phase_deadline: Optional[float] = None) -> Any:
    """Yield decoded physical lines without retaining an unbounded line.

    Real compiler assembly lines are short. For malformed input, retain only
    the first MiB, discard the rest through its newline, and report the event
    in scan statistics while preserving physical line numbering.
    """
    pending = bytearray()
    truncated = False
    while True:
        if (phase_deadline is not None
                and time.monotonic() >= phase_deadline):
            raise AssemblyPhaseDeadlineExceeded(
                "assembly phase deadline exhausted while streaming")
        chunk = stream.read(ASSEMBLY_READ_CHUNK_BYTES)
        if not chunk:
            break
        stats["bytes_streamed"] += len(chunk)
        start = 0
        while start < len(chunk):
            newline = chunk.find(b"\n", start)
            end = len(chunk) if newline < 0 else newline
            if not truncated:
                room = ASSEMBLY_MAX_RETAINED_LINE_BYTES - len(pending)
                if room > 0:
                    pending.extend(chunk[start:min(end, start + room)])
                if end - start > max(room, 0):
                    truncated = True
            if newline < 0:
                break
            if truncated:
                stats["truncated_lines"] += 1
            stats["max_retained_line_bytes"] = max(
                stats["max_retained_line_bytes"], len(pending))
            yield pending.decode("utf-8", errors="replace")
            pending.clear()
            truncated = False
            start = newline + 1
    if pending or truncated:
        if truncated:
            stats["truncated_lines"] += 1
        stats["max_retained_line_bytes"] = max(
            stats["max_retained_line_bytes"], len(pending))
        yield pending.decode("utf-8", errors="replace")


def _new_snippet_budget() -> dict:
    return {
        "bytes_retained": 0,
        "snippets_truncated": 0,
        "bytes_rejected": 0,
    }


def _retain_capture_line(capture: dict, line: str,
                         snippet_lines: int, budget: dict) -> None:
    """Retain one snippet line within per-snippet and request byte limits."""
    if capture["snippet_budget_truncated"]:
        return
    if len(capture["snippet"]) >= snippet_lines:
        return
    encoded = line.encode("utf-8", errors="replace")
    separator_bytes = 1 if capture["snippet"] else 0
    required = separator_bytes + len(encoded)
    per_remaining = MAX_RETAINED_SNIPPET_BYTES - capture["snippet_bytes"]
    total_remaining = (
        MAX_TOTAL_RETAINED_SNIPPET_BYTES - budget["bytes_retained"])
    allowed = max(0, min(required, per_remaining, total_remaining))
    if allowed == required:
        capture["snippet"].append(line)
        capture["snippet_bytes"] += required
        budget["bytes_retained"] += required
        return

    if allowed > separator_bytes:
        retained = encoded[:allowed - separator_bytes].decode(
            "utf-8", errors="ignore")
        if retained:
            capture["snippet"].append(retained)
            retained_bytes = separator_bytes + len(
                retained.encode("utf-8", errors="replace"))
            capture["snippet_bytes"] += retained_bytes
            budget["bytes_retained"] += retained_bytes
            allowed = retained_bytes
        else:
            allowed = 0
    else:
        allowed = 0
    rejected = max(0, required - allowed)
    budget["bytes_rejected"] += rejected
    capture["snippet_bytes_rejected"] += rejected
    capture["snippet_budget_truncated"] = True
    budget["snippets_truncated"] += 1


def _release_capture_budget(capture: dict, budget: dict) -> None:
    """Release provisional bytes for a label that failed the CFI check."""
    budget["bytes_retained"] -= capture["snippet_bytes"]
    budget["bytes_rejected"] -= capture["snippet_bytes_rejected"]
    if capture["snippet_budget_truncated"]:
        budget["snippets_truncated"] -= 1
    capture["snippet"].clear()
    capture["snippet_bytes"] = 0


def _retain_text_snippet(text: str, snippet_lines: int,
                         budget: dict) -> tuple[str, int, bool]:
    """Retain a bounded snippet from an already-produced text artifact."""
    capture = {
        "snippet": [],
        "snippet_bytes": 0,
        "snippet_bytes_rejected": 0,
        "snippet_budget_truncated": False,
    }
    for raw_line in io.StringIO(text):
        if len(capture["snippet"]) >= snippet_lines:
            break
        line = raw_line.rstrip("\r\n")
        _retain_capture_line(capture, line, snippet_lines, budget)
        if capture["snippet_budget_truncated"]:
            break
    return ("\n".join(capture["snippet"]), capture["snippet_bytes"],
            capture["snippet_budget_truncated"])


def _stream_assembly_listing(
    s_file: Path,
    matches: list[dict],
    source_basename: Optional[str],
    function_name: str,
    snippet_lines: int,
    snippet_budget: Optional[dict] = None,
    phase_deadline: Optional[float] = None,
    exact_match_labels: Optional[set[str]] = None,
) -> tuple[dict[str, dict], Optional[dict], dict]:
    """Parse one candidate listing once with bounded retained state."""
    labels_to_symbols: dict[str, list[str]] = {}
    exact_match_labels = (exact_match_labels
                          if exact_match_labels is not None
                          else {match["mangled"] for match in matches})
    for match in matches:
        mangled = match["mangled"]
        for label in _symbol_listing_labels(mangled, exact_match_labels):
            labels_to_symbols.setdefault(label, []).append(mangled)
    label_re = (re.compile(
        r"^(" + "|".join(re.escape(label) for label in labels_to_symbols)
        + r"):(?:\s|$)") if labels_to_symbols else None)

    stats = {
        "bytes_streamed": 0,
        "truncated_lines": 0,
        "max_retained_line_bytes": 0,
    }
    blocks: dict[str, dict] = {}
    active: dict[str, dict] = {}
    file_table: dict[int, str] = {}
    loc_stats: dict[int, list[Optional[int]]] = {}
    debug_value_hits = 0
    retained_snippet_lines = max(0, snippet_lines)
    snippet_budget = snippet_budget or _new_snippet_budget()
    source_state_entries: set[int] = set()
    source_state_bytes = 0
    source_state_budget_exceeded = False
    source_state_overflow_events = 0
    source_state_entries_cleared = 0
    source_state_records_dropped = 0

    def fail_source_state_closed() -> None:
        nonlocal source_state_budget_exceeded
        nonlocal source_state_overflow_events
        nonlocal source_state_entries_cleared
        nonlocal source_state_records_dropped
        nonlocal source_state_bytes
        source_state_budget_exceeded = True
        source_state_overflow_events += 1
        source_state_entries_cleared += len(source_state_entries)
        source_state_records_dropped += 1  # the record that crossed the bound
        file_table.clear()
        loc_stats.clear()
        source_state_entries.clear()
        source_state_bytes = 0

    deadline_exceeded = False
    with s_file.open("rb") as stream:
        line_stream = enumerate(
            _bounded_binary_lines(stream, stats, phase_deadline), start=1)
        while True:
            try:
                line_number, line = next(line_stream)
            except StopIteration:
                break
            except AssemblyPhaseDeadlineExceeded:
                deadline_exceeded = True
                break
            # Advance active function captures before accepting another label.
            for mangled, capture in tuple(active.items()):
                _retain_capture_line(
                    capture, line, retained_snippet_lines, snippet_budget)
                capture["lines_seen"] += 1
                if ".cfi_startproc" in line:
                    capture["started"] = True
                    capture["depth"] += 1
                elif capture["started"] and ".cfi_endproc" in line:
                    capture["depth"] -= 1
                    if capture["depth"] == 0:
                        blocks[mangled] = {
                            "assembly_listing_file": str(s_file),
                            "assembly_listing_lines": [
                                capture["start_line"], line_number],
                            "assembly_listing_snippet_first_lines":
                                "\n".join(capture["snippet"]),
                            "assembly_listing_snippet_bytes":
                                capture["snippet_bytes"],
                            "assembly_listing_snippet_budget_truncated":
                                capture["snippet_budget_truncated"],
                        }
                        del active[mangled]
                        continue
                if not capture["started"] and capture["lines_seen"] >= 8:
                    _release_capture_budget(capture, snippet_budget)
                    del active[mangled]

            if label_re is not None:
                label_match = label_re.match(line)
                if label_match:
                    for mangled in labels_to_symbols[label_match.group(1)]:
                        if mangled in blocks or mangled in active:
                            continue
                        active[mangled] = {
                            "start_line": line_number,
                            "lines_seen": 1,
                            "started": ".cfi_startproc" in line,
                            "depth": 1 if ".cfi_startproc" in line else 0,
                            "snippet": [],
                            "snippet_bytes": 0,
                            "snippet_bytes_rejected": 0,
                            "snippet_budget_truncated": False,
                        }
                        _retain_capture_line(
                            active[mangled], line, retained_snippet_lines,
                            snippet_budget)

            if source_basename:
                file_match = FILE_DIR_RE.match(line)
                loc_match = LOC_RE.match(line)
                if source_state_budget_exceeded:
                    if file_match or loc_match:
                        source_state_records_dropped += 1
                elif file_match:
                    idx = int(file_match.group(1))
                    comp_dir = file_match.group(2) or ""
                    fname = file_match.group(3) or ""
                    resolved_path = (
                        str(Path(comp_dir) / fname) if comp_dir else fname)
                    old_path = file_table.get(idx)
                    entry_delta = 0 if idx in source_state_entries else 1
                    byte_delta = (
                        len(os.fsencode(resolved_path))
                        + (32 if entry_delta else 0))
                    if old_path is not None:
                        byte_delta -= len(os.fsencode(old_path))
                    if (len(source_state_entries) + entry_delta
                            > ASSEMBLY_MAX_SOURCE_STATE_ENTRIES
                            or source_state_bytes + byte_delta
                            > ASSEMBLY_MAX_SOURCE_STATE_BYTES):
                        fail_source_state_closed()
                    else:
                        source_state_entries.add(idx)
                        source_state_bytes += byte_delta
                        file_table[idx] = resolved_path
                if loc_match and not source_state_budget_exceeded:
                    idx = int(loc_match.group(1))
                    entry_delta = 0 if idx in source_state_entries else 1
                    byte_delta = 32 if entry_delta else 0
                    if (len(source_state_entries) + entry_delta
                            > ASSEMBLY_MAX_SOURCE_STATE_ENTRIES
                            or source_state_bytes + byte_delta
                            > ASSEMBLY_MAX_SOURCE_STATE_BYTES):
                        fail_source_state_closed()
                    else:
                        source_state_entries.add(idx)
                        source_state_bytes += byte_delta
                        values = loc_stats.setdefault(idx, [0, None, None])
                        values[0] = int(values[0] or 0) + 1
                        if values[1] is None:
                            values[1] = line_number
                        values[2] = line_number
                if (not source_state_budget_exceeded
                        and "DEBUG_VALUE" in line):
                    debug_match = DEBUG_VALUE_RE.search(line)
                    if (debug_match
                            and debug_match.group(1).strip() == function_name):
                        debug_value_hits += 1

    inline_site: Optional[dict] = None
    for capture in active.values():
        _release_capture_budget(capture, snippet_budget)

    if (source_basename and not source_state_budget_exceeded
            and not deadline_exceeded):
        target_indices = {
            idx for idx, path in file_table.items()
            if path.endswith(source_basename)
        }
        if target_indices:
            loc_hits = sum(
                int(loc_stats.get(idx, [0, None, None])[0] or 0)
                for idx in target_indices)
            first_lines = [
                loc_stats[idx][1] for idx in target_indices
                if idx in loc_stats and loc_stats[idx][1] is not None
            ]
            last_lines = [
                loc_stats[idx][2] for idx in target_indices
                if idx in loc_stats and loc_stats[idx][2] is not None
            ]
            if loc_hits or debug_value_hits:
                inline_site = {
                    "assembly_listing_file": str(s_file),
                    "loc_directive_count": loc_hits,
                    "first_loc_line": min(first_lines) if first_lines else None,
                    "last_loc_line": max(last_lines) if last_lines else None,
                    "debug_value_hits": debug_value_hits,
                    "resolved_file_indices": sorted(target_indices),
                }
    stats.update({
        "source_state_budget_exceeded": source_state_budget_exceeded,
        "source_state_overflow_events": source_state_overflow_events,
        "source_state_entries_cleared": source_state_entries_cleared,
        "source_state_records_dropped": source_state_records_dropped,
        "source_state_entries_retained": len(source_state_entries),
        "source_state_bytes_retained": source_state_bytes,
        "deadline_exceeded": deadline_exceeded,
    })
    return blocks, inline_site, stats


def scan_assembly_listings(
    s_files: list[Path],
    matches: list[dict],
    source_basename: Optional[str],
    function_name: str,
    snippet_lines: int,
    rg_path: Optional[str],
    warnings: list[str],
    snippet_budget: Optional[dict] = None,
) -> tuple[dict[str, dict], list[dict], dict]:
    """Prefilter and stream source/pre-LTO evidence from assembly listings."""
    phase_started = time.monotonic()
    phase_deadline = phase_started + ASSEMBLY_PHASE_TIMEOUT_SEC
    discovered = _unique_paths(s_files)
    candidates, prefilter_method = prefilter_assembly_files(
        discovered, matches, source_basename, rg_path, warnings,
        phase_deadline=phase_deadline)
    listings: dict[str, dict] = {}
    inline_sites: list[dict] = []
    bytes_streamed = 0
    truncated_lines = 0
    max_retained_line_bytes = 0
    source_state_budget_exceeded_files = 0
    source_state_overflow_events = 0
    source_state_entries_cleared = 0
    source_state_records_dropped = 0
    max_source_state_entries_retained = 0
    max_source_state_bytes_retained = 0
    snippet_budget = snippet_budget or _new_snippet_budget()
    scanned = 0
    scan_incomplete = False
    incomplete_reason: Optional[str] = None
    read_failures = 0
    all_exact_match_labels = {match["mangled"] for match in matches}
    for s_file in candidates:
        if time.monotonic() >= phase_deadline:
            scan_incomplete = True
            incomplete_reason = "assembly-phase-deadline"
            break
        unresolved_matches = [
            match for match in matches
            if match["mangled"] not in listings
        ]
        try:
            blocks, inline_site, file_stats = _stream_assembly_listing(
                s_file, unresolved_matches, source_basename, function_name,
                snippet_lines, snippet_budget, phase_deadline,
                all_exact_match_labels)
        except OSError as exc:
            warnings.append(f"failed to stream {s_file}: {exc}")
            scan_incomplete = True
            incomplete_reason = incomplete_reason or "listing-read-error"
            read_failures += 1
            continue
        bytes_streamed += file_stats["bytes_streamed"]
        truncated_lines += file_stats["truncated_lines"]
        max_retained_line_bytes = max(
            max_retained_line_bytes,
            file_stats["max_retained_line_bytes"],
        )
        source_state_budget_exceeded_files += int(
            file_stats["source_state_budget_exceeded"])
        source_state_overflow_events += file_stats[
            "source_state_overflow_events"]
        source_state_entries_cleared += file_stats[
            "source_state_entries_cleared"]
        source_state_records_dropped += file_stats[
            "source_state_records_dropped"]
        max_source_state_entries_retained = max(
            max_source_state_entries_retained,
            file_stats["source_state_entries_retained"],
        )
        max_source_state_bytes_retained = max(
            max_source_state_bytes_retained,
            file_stats["source_state_bytes_retained"],
        )
        for mangled, block in blocks.items():
            listings.setdefault(mangled, block)
        if inline_site is not None:
            inline_sites.append(inline_site)
        if file_stats["deadline_exceeded"]:
            scan_incomplete = True
            incomplete_reason = "assembly-phase-deadline"
            break
        scanned += 1
    if truncated_lines:
        warnings.append(
            f"{truncated_lines} overlong assembly line(s) were truncated to "
            f"{ASSEMBLY_MAX_RETAINED_LINE_BYTES} bytes while scanning")
    if snippet_budget["snippets_truncated"]:
        warnings.append(
            f"{snippet_budget['snippets_truncated']} assembly snippet(s) "
            "were byte-truncated; full evidence remains available via each "
            "assembly_listing_file and line range")
    if source_state_budget_exceeded_files:
        warnings.append(
            "assembly source-index state exceeded its bounded entry/byte "
            f"budget in {source_state_budget_exceeded_files} listing(s); "
            "inline-site evidence for those listings was omitted fail-closed")
    if incomplete_reason == "assembly-phase-deadline":
        warnings.append(
            f"assembly inspection stopped at its single "
            f"{ASSEMBLY_PHASE_TIMEOUT_SEC}s phase deadline; source/pre-LTO "
            "evidence is incomplete, while completed blocks remain valid")
    return listings, inline_sites, {
        "prefilter_method": prefilter_method,
        "listings_discovered": len(discovered),
        "listings_candidates": len(candidates),
        "listings_scanned": scanned,
        "bytes_streamed": bytes_streamed,
        "truncated_lines": truncated_lines,
        "max_retained_line_bytes": max_retained_line_bytes,
        "snippet_bytes_retained": snippet_budget["bytes_retained"],
        "snippet_bytes_rejected": snippet_budget["bytes_rejected"],
        "snippets_byte_truncated": snippet_budget["snippets_truncated"],
        "snippet_per_item_byte_limit": MAX_RETAINED_SNIPPET_BYTES,
        "snippet_total_byte_limit": MAX_TOTAL_RETAINED_SNIPPET_BYTES,
        "source_state_budget_exceeded_files":
            source_state_budget_exceeded_files,
        "source_state_overflow_events": source_state_overflow_events,
        "source_state_entries_cleared": source_state_entries_cleared,
        "source_state_records_dropped": source_state_records_dropped,
        "source_state_max_entries_retained":
            max_source_state_entries_retained,
        "source_state_max_bytes_retained": max_source_state_bytes_retained,
        "source_state_entry_limit": ASSEMBLY_MAX_SOURCE_STATE_ENTRIES,
        "source_state_byte_limit": ASSEMBLY_MAX_SOURCE_STATE_BYTES,
        "phase_deadline_seconds": ASSEMBLY_PHASE_TIMEOUT_SEC,
        "scan_incomplete": scan_incomplete,
        "incomplete_reason": incomplete_reason,
        "read_failures": read_failures,
        "listings_unparsed": max(0, len(candidates) - scanned),
    }


# --------------------------- objdump per-symbol disassembly ----------------

def disassemble_symbol(objdump_path: str, binary: Path,
                       mangled_name: str,
                       scratch_dir: Path,
                       demangler: Optional[str]) -> tuple[Path, list[str]]:
    """Disassemble a single symbol via `--disassemble-symbols=<mangled>`.

    The mangled name MUST be exactly what the binary's symbol table
    contains (i.e. the value in nm output's last column — already includes
    the Mach-O `_` prefix when applicable). See plan §4 "Symbol-name
    handling — VERIFIED".

    Returns (output_path, warnings).
    """
    warnings: list[str] = []
    digest = hashlib.sha1(mangled_name.encode("utf-8")).hexdigest()[:12]
    out_file = scratch_dir / f"objdump_{digest}.txt"

    try:
        proc = subprocess.run(
            [objdump_path,
             "--disassemble",
             "--no-show-raw-insn",
             "--line-numbers",
             f"--disassemble-symbols={mangled_name}",
             str(binary)],
            capture_output=True, text=True,
            timeout=OBJDUMP_TIMEOUT_SEC)
    except subprocess.TimeoutExpired:
        warnings.append(
            f"objdump timed out after {OBJDUMP_TIMEOUT_SEC}s on "
            f"{mangled_name[:60]}…")
        out_file.write_text(
            f"# objdump timed out on symbol {mangled_name!r}\n")
        return out_file, warnings

    raw = proc.stdout
    if proc.returncode != 0 or "missing symbol" in (proc.stderr or ""):
        warnings.append(
            f"objdump may have failed for {mangled_name[:60]}…: "
            f"{(proc.stderr or '').strip()[:200]}")

    if demangler:
        try:
            cf = subprocess.run([demangler], input=raw,
                                capture_output=True, text=True,
                                timeout=DEMANGLER_TIMEOUT_SEC)
            cooked = cf.stdout
            if cf.returncode != 0:
                warnings.append(
                    f"c++filt failed: {cf.stderr.strip()[:200]}")
        except subprocess.TimeoutExpired:
            warnings.append(f"c++filt timed out after {DEMANGLER_TIMEOUT_SEC}s")
            cooked = raw
    else:
        cooked = raw

    out_file.write_text(
        cooked or
        f"# objdump returned empty for symbol {mangled_name!r}\n"
        f"# stderr:\n{proc.stderr}\n")
    return out_file, warnings


# --------------------------- llc fallback (.bc → .s) -----------------------

def llc_compile(llc_path: str, bc_file: Path) -> Optional[Path]:
    """Lazily emit a `.s` next to the `.bc`. Cached: if the existing `.s`
    is newer than the `.bc`, re-use it. Returns the `.s` path or None on
    failure.

    Each call is ~25s on the bioseq bench-main TU. The lazy cache means
    we pay it once per build (until ninja regenerates the .bc).
    """
    s_file = bc_file.with_suffix(".s")
    if s_file.exists() and s_file.stat().st_mtime >= bc_file.stat().st_mtime:
        return s_file
    try:
        proc = subprocess.run(
            [llc_path, str(bc_file), "-O3", "-o", str(s_file)],
            capture_output=True, text=True, timeout=LLC_TIMEOUT_SEC)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0:
        return None
    return s_file


# --------------------------- instruction histograms ------------------------

# We classify instructions by mnemonic via explicit per-arch sets (NOT a
# regex of mnemonic fragments — see plan §11.8). We additionally treat any
# mnemonic with a dotted arrangement suffix (Apple syntax, e.g. `add.16b`)
# or any operand starting with `v\d+\.` (GNU syntax) as SIMD.

ARM64_BRANCH_MNEMONICS = {
    "b", "bl", "blr", "br", "ret", "cbz", "cbnz", "tbz", "tbnz",
    "b.eq", "b.ne", "b.cs", "b.hs", "b.cc", "b.lo", "b.mi", "b.pl",
    "b.vs", "b.vc", "b.hi", "b.ls", "b.ge", "b.lt", "b.gt", "b.le",
    "b.al", "b.nv",
}
ARM64_LOAD_MNEMONICS = {
    "ldr", "ldur", "ldp", "ldnp", "ldarb", "ldarh", "ldar",
    "ldaxr", "ldaxrb", "ldaxrh", "ldxp",
    "ldaprb", "ldaprh", "ldaprw", "ldapr",
    "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrsw",
    "ldraa", "ldrab",  # PAuth-protected loads
}
ARM64_STORE_MNEMONICS = {
    "str", "stur", "stp", "stnp", "stlr", "stlrb", "stlrh",
    "stxr", "stxrb", "stxrh", "stxp",
    "strb", "strh",
}
# LSE atomic RMW: load AND store. Counted under both atomic_loads and
# atomic_stores. Requires `-march=armv8.1-a+lse` or later.
ARM64_ATOMIC_RMW_MNEMONICS = {
    "swp", "swpa", "swpl", "swpal",
    "swpb", "swpab", "swplb", "swpalb",
    "swph", "swpah", "swplh", "swpalh",
    "cas", "casa", "casl", "casal",
    "casb", "casab", "caslb", "casalb",
    "cash", "casah", "caslh", "casalh",
    "casp", "caspa", "caspl", "caspal",
    "ldadd", "ldadda", "ldaddl", "ldaddal",
    "ldaddb", "ldaddab", "ldaddlb", "ldaddalb",
    "ldaddh", "ldaddah", "ldaddlh", "ldaddalh",
    "ldclr", "ldclra", "ldclrl", "ldclral",
    "ldeor", "ldeora", "ldeorl", "ldeoral",
    "ldset", "ldseta", "ldsetl", "ldsetal",
    "ldsmin", "ldsminl", "ldsmina", "ldsminal",
    "ldsmax", "ldsmaxl", "ldsmaxa", "ldsmaxal",
    "ldumin", "lduminl", "ldumina", "lduminal",
    "ldumax", "ldumaxl", "ldumaxa", "ldumaxal",
}

X86_BRANCH_MNEMONICS = {
    "jmp", "je", "jne", "jl", "jle", "jg", "jge", "js", "jns",
    "jz", "jnz", "jc", "jnc", "ja", "jae", "jb", "jbe", "jo",
    "jno", "jp", "jpe", "jnp", "jpo", "jrcxz", "jecxz",
    "call", "ret", "retq", "iret", "iretq",
    "loop", "loopne", "loopnz", "loopeq", "loope", "loopz",
    "syscall", "sysret", "sysenter", "sysexit", "int", "int3", "into",
}
# Mnemonics that move data into/out of memory. Whether each is a load or
# a store depends on operand order (Intel syntax: dest first), so we
# classify per-instruction in `_x86_mov_class()` below.
X86_MOV_LIKE_MNEMONICS = {
    "mov", "movzx", "movsx", "movsxd",
    "movq", "movd",
    "movups", "movupd", "movaps", "movapd",
    "movdqu", "movdqa", "movntdq", "movnti", "movntq",
    "vmovups", "vmovupd", "vmovaps", "vmovapd",
    "vmovdqu", "vmovdqa", "vmovq", "vmovd",
    "lddqu", "vlddqu",
}
X86_VECTOR_REG_RE = re.compile(r"\b[xyz]?mm\d+\b")
X86_INTEL_MEM_RE = re.compile(r"\[")

INSTR_LINE_RE = re.compile(r"^\s*[0-9a-f]+:\s+(\S+)(?:\s+(.*))?$")
ARM64_NEON_VECTOR_OPERAND_RE = re.compile(r"\bv\d+\.")
ARM64_NEON_STRUCT_LOADSTORE_RE = re.compile(r"^(ld|st)[1-4]$")


def _x86_mov_class(operands: str) -> tuple[bool, bool]:
    """Classify an x86 mov-family instruction as (is_load, is_store).

    Assumes Intel syntax (LLVM objdump default): `<dest>, <src>`. For AT&T
    syntax we'd see register operands prefixed with `%`; an `$` prefixed
    operand is an immediate. The disambiguation is "memory operand on src
    side → load; on dest side → store". `lea` is intentionally NOT in
    X86_MOV_LIKE_MNEMONICS — it computes an address but doesn't touch
    memory.
    """
    parts = operands.split(",", 1)
    if len(parts) != 2:
        return False, False
    dest, src = parts[0].strip(), parts[1].strip()
    src_mem = bool(X86_INTEL_MEM_RE.search(src))
    dest_mem = bool(X86_INTEL_MEM_RE.search(dest))
    return (src_mem and not dest_mem,
            dest_mem and not src_mem)


def instruction_histogram(disasm: str, arch_hint: str) -> dict:
    """Count instructions by class. arch_hint ∈ {arm64, x86_64, …}.

    Unknown arch still produces `total` and `simd` (which are
    arch-agnostic) but other counts will be zero.
    """
    h = {
        "total": 0,
        "simd": 0,
        "branches": 0,
        "loads": 0,
        "stores": 0,
        "atomic_loads": 0,
        "atomic_stores": 0,
        "neon_struct_loadstore": 0,
    }
    is_arm = arch_hint.startswith("arm")
    is_x86 = arch_hint.startswith("x86")

    for line in disasm.splitlines():
        m = INSTR_LINE_RE.match(line)
        if not m:
            continue
        h["total"] += 1
        mnem_full = m.group(1).lower()
        operands = (m.group(2) or "").lower()

        # Strip Apple-syntax arrangement suffix for class lookup, but
        # remember whether one was present (= it's a NEON instruction).
        # Guard against `b.eq` etc., which use `.` for branch conditions.
        has_dotted_suffix = "." in mnem_full and not mnem_full.startswith("b.")
        mnem_base = mnem_full.split(".", 1)[0] if has_dotted_suffix else mnem_full

        # SIMD: dotted suffix (Apple) OR vector operand register.
        if is_arm:
            if has_dotted_suffix or ARM64_NEON_VECTOR_OPERAND_RE.search(operands):
                h["simd"] += 1
        elif is_x86:
            if X86_VECTOR_REG_RE.search(operands):
                h["simd"] += 1
        else:
            # Unknown arch — best-effort: any dotted suffix is SIMD-ish.
            if has_dotted_suffix:
                h["simd"] += 1

        if is_arm:
            if mnem_full in ARM64_BRANCH_MNEMONICS or mnem_base in ARM64_BRANCH_MNEMONICS:
                h["branches"] += 1
            if mnem_base in ARM64_LOAD_MNEMONICS:
                h["loads"] += 1
                if mnem_base.startswith(("ldax", "ldap", "ldar", "ldxp")):
                    h["atomic_loads"] += 1
            if mnem_base in ARM64_STORE_MNEMONICS:
                h["stores"] += 1
                if mnem_base.startswith(("stx", "stl")):
                    h["atomic_stores"] += 1
            if mnem_base in ARM64_ATOMIC_RMW_MNEMONICS:
                h["atomic_loads"] += 1
                h["atomic_stores"] += 1
            # NEON struct load/store: `ld[1-4]` / `st[1-4]` (with or
            # without dot suffix). This bucket is a separate cost class
            # from regular ldr/str — see plan §11.8.
            if ARM64_NEON_STRUCT_LOADSTORE_RE.match(mnem_base):
                h["neon_struct_loadstore"] += 1
        elif is_x86:
            if mnem_full in X86_BRANCH_MNEMONICS:
                h["branches"] += 1
            if mnem_base in X86_MOV_LIKE_MNEMONICS:
                is_load, is_store = _x86_mov_class(operands)
                if is_load:
                    h["loads"] += 1
                if is_store:
                    h["stores"] += 1
            # x86 atomics carry a `lock` prefix; some assemblies emit it
            # as the leading mnemonic, others fold it into the line.
            if mnem_full == "lock" or operands.startswith("lock "):
                if "[" in operands:
                    h["atomic_loads"] += 1
                    h["atomic_stores"] += 1

    return h


# --------------------------- main entry point ------------------------------

def _error(message: str, **extra: Any) -> dict:
    """Build a JSON-serialisable error response. Never raises."""
    out = {"error": message}
    out.update(extra)
    return out


def _validated_bounded_int(
    name: str,
    value: Any,
    default: int,
    minimum: int,
    maximum: int,
    warnings: list[str],
) -> int:
    """Return a safe integer and make every default/clamp visible."""
    if type(value) is not int:
        warnings.append(
            f"{name}={value!r} is not an integer; using default {default}")
        return default
    bounded = min(max(value, minimum), maximum)
    if bounded != value:
        warnings.append(
            f"{name}={value} is outside [{minimum}, {maximum}]; "
            f"clamped to {bounded}")
    return bounded


def resolve_binary_candidate(
    binary_root: Path,
    target: str,
    artifact_names: Optional[tuple[str, ...]] = None,
) -> tuple[Path, tuple[Path, ...]]:
    """Resolve the first existing target artifact and retain all diagnostics."""
    candidate_names = artifact_names or (target,)
    binary_candidates = tuple(binary_root / name for name in candidate_names)
    binary = next((path for path in binary_candidates if path.is_file()),
                  binary_candidates[0])
    return binary, binary_candidates


def run_inspect(
    function_name: str,
    target: str,
    build_dir: Path,
    *,
    max_matches: int = 4,
    snippet_lines: int = 200,
    source_basename: Optional[str] = None,
    llc_on_bc: bool = True,
    max_llc_bc_files: int = 4,
    scratch_dir: Optional[Path] = None,
    binary_subdir: str = "bioseq",
    artifact_names: Optional[tuple[str, ...]] = None,
    shared_target_names: Optional[tuple[str, ...]] = None,
) -> dict:
    """Inspect a function and return linked plus compiler-listing evidence.

    On error: returns `{"error": "...", ...}` — never raises. Callers
    serialise the dict to JSON and ship it back to the AI agent.

    Side effects: writes objdump output and (when llc_on_bc=True) cached
    `.s` files to disk. Reads but does not modify the build directory.
    """
    if not function_name:
        return _error("function_name must be a non-empty string")

    scratch_dir = scratch_dir or DEFAULT_SCRATCH_DIR
    binary_root = build_dir / binary_subdir
    binary, binary_candidates = resolve_binary_candidate(
        binary_root, target, artifact_names)

    if not binary.exists():
        return _error(
            "target binary not found; checked "
            f"{', '.join(str(path) for path in binary_candidates)}. "
            "Run start_build with a "
            f"mode that produces target='{target}' first.",
            target=target,
            build_dir=str(build_dir),
            binary_subdir=binary_subdir,
        )

    try:
        scratch_dir.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        return _error(f"could not create scratch dir {scratch_dir}: {e}")

    tools = detect_tools()
    if not tools["nm"]:
        return _error(
            "nm/llvm-nm not found on PATH or LLVM hint dirs. Install "
            "binutils (Linux) or Xcode CLT (macOS) and try again.",
            tools_searched=PREFERRED_NM, hints=LLVM_HINTS)
    if not tools["objdump"]:
        return _error(
            "objdump/llvm-objdump not found on PATH or LLVM hint dirs.",
            tools_searched=PREFERRED_OBJDUMP, hints=LLVM_HINTS)

    warnings: list[str] = []
    max_matches = _validated_bounded_int(
        "max_matches", max_matches, 4,
        MIN_INSPECT_MATCHES, MAX_INSPECT_MATCHES, warnings)
    snippet_lines = _validated_bounded_int(
        "snippet_lines", snippet_lines, 200,
        MIN_INSPECT_SNIPPET_LINES, MAX_INSPECT_SNIPPET_LINES, warnings)
    if not tools["demangler"]:
        warnings.append(
            "no demangler (c++filt / llvm-cxxfilt) found on PATH; "
            "substring search will run on mangled names only — pass "
            "the Itanium-mangled form (e.g. _ZN6bioseq…) for an "
            "accurate match.")
    if llc_on_bc and not tools["llc"]:
        warnings.append(
            "llc not found on PATH or LLVM hint dirs; pre-LTO `.s` view "
            "will be unavailable for clang/ThinLTO builds. Install LLVM "
            "(brew install llvm, apt install llvm).")

    obj_format = detect_object_format(binary)
    arch_hint = detect_arch(binary, obj_format)
    objdump_supports_filter = tool_supports(
        tools["objdump"], "--disassemble-symbols")
    nm_supports_demangle = tool_supports(tools["nm"], "--demangle")
    if not objdump_supports_filter:
        # Plan §4.4: legacy binutils (<2.34) lacks --disassemble-symbols=.
        # Phase B doesn't implement the full-text fallback; surface the
        # capability gap so users know to upgrade binutils or use an LLVM
        # objdump. Calls will likely return empty results.
        warnings.append(
            f"{tools['objdump']} does not support --disassemble-symbols=; "
            "per-match disassembly will likely be empty. Upgrade binutils "
            "to >=2.34 or install LLVM (which provides llvm-objdump).")

    try:
        syms = list_symbols(tools["nm"], binary, tools["demangler"], warnings)
    except subprocess.TimeoutExpired:
        # Only nm uses a bare TimeoutExpired now (demangler is wrapped in
        # list_symbols). nm timing out on a multi-MB binary is fatal —
        # nothing else to do without a symbol table.
        return _error(f"nm timed out after {NM_TIMEOUT_SEC}s on {binary}")
    except (RuntimeError, OSError) as e:
        return _error(f"failed to enumerate symbols: {e}")

    stripped_note: Optional[str] = None
    if not syms:
        # Plan §8 Risk #10: surface a top-level note in summary so the AI
        # agent can distinguish "stripped binary" from "function not found".
        stripped_note = (
            f"binary appears stripped or has no defined text symbols "
            f"({binary}); inspect against the unstripped build artifact")
        warnings.append(stripped_note)

    all_matches = [s for s in syms
                   if function_name in (s.get("demangled") or "")
                   and s.get("type", "").lower() == "t"]
    all_matches.sort(key=lambda s: -(s.get("size") or 0))
    total_matches_before_cap = len(all_matches)
    matches = all_matches[:max_matches]

    target_dir = binary_root / "CMakeFiles" / f"{target}.dir"
    # DOC0269 — Domain N shared modules live in `bioseq_modules_nopg.dir`
    # (an OBJECT library linked by every non-PG consumer). Code from those
    # modules ends up in the consumer's binary but the per-TU `.s`/`.bc`
    # artifacts only exist under the lib's build dir. Sweep both.
    shared_names = (shared_target_names if shared_target_names is not None
                    else ("bioseq_modules_nopg",))
    shared_lib_dirs = tuple(
        binary_root / "CMakeFiles" / f"{name}.dir"
        for name in shared_names
    )
    search_dirs = [
        directory for directory in (target_dir, *shared_lib_dirs)
        if directory.exists()
    ]
    s_files: list[Path] = []
    for d in search_dirs:
        s_files.extend(d.rglob("*.s"))

    # llc fallback: when there are .bc but no .s, lazily compile.
    # `--max-llc-bc-files` caps the cold-path time. See plan §5.
    if llc_on_bc and tools["llc"] and search_dirs:
        bc_files: list[Path] = []
        for d in search_dirs:
            bc_files.extend(bc for bc in d.rglob("*.bc")
                            if bc.with_suffix(".s") not in s_files)
        if bc_files and not s_files:
            relevant_bcs: list[Path] = []
            now = time.time()
            for bc in bc_files:
                # Risk #9 mitigation: skip .bc files modified within the
                # grace window — they may be partially written by an
                # in-progress ninja build, and llc will fail or produce
                # garbage.
                try:
                    bc_mtime = bc.stat().st_mtime
                except OSError:
                    continue
                if now - bc_mtime < BC_MTIME_GRACE_SEC:
                    warnings.append(
                        f"skipping {bc.name}: modified <{BC_MTIME_GRACE_SEC}s "
                        f"ago (build in progress?)")
                    continue
                try:
                    r = subprocess.run(
                        [tools["nm"], "--defined-only", "--demangle", str(bc)],
                        capture_output=True, text=True, timeout=NM_TIMEOUT_SEC)
                except subprocess.TimeoutExpired:
                    warnings.append(f"nm timed out on {bc.name}")
                    continue
                if r.returncode != 0:
                    warnings.append(
                        f"nm failed on {bc.name}: {r.stderr.strip()[:200]}")
                    continue
                if function_name in r.stdout:
                    relevant_bcs.append(bc)
            if len(relevant_bcs) > max_llc_bc_files:
                warnings.append(
                    f"limiting llc fallback to {max_llc_bc_files} of "
                    f"{len(relevant_bcs)} candidate .bc files; raise "
                    f"max_llc_bc_files to widen the search")
                relevant_bcs = relevant_bcs[:max_llc_bc_files]
            for bc in relevant_bcs:
                s = llc_compile(tools["llc"], bc)
                if s:
                    s_files.append(s)
                else:
                    warnings.append(f"llc failed on {bc.name}")

    if not s_files:
        # Emit a single warning at the top level, the matches will still
        # be useful via the post-LTO objdump view.
        if llc_on_bc and tools["llc"]:
            warnings.append(
                "no per-TU `.s` files available — pre-LTO view is empty. "
                "Build the target's matching debug preset with "
                "-save-temps=obj, or rebuild if .bc files are stale.")
        else:
            warnings.append(
                "no per-TU `.s` files available and llc-fallback disabled "
                "or unavailable. Pre-LTO view is empty.")

    out: dict = {
        "query": {
            "function_name": function_name,
            "target": target,
            "build_dir": str(build_dir),
            "binary_subdir": binary_subdir,
            "binary": str(binary),
            "max_matches": max_matches,
            "snippet_lines": snippet_lines,
        },
        "build_metadata": {
            "object_format": obj_format,
            "arch_hint": arch_hint,
            "tools": {
                "nm": tools["nm"],
                "objdump": tools["objdump"],
                "demangler": tools["demangler"],
                "llc": tools["llc"],
                "rg": tools.get("rg"),
                "objdump_supports_disassemble_symbols": objdump_supports_filter,
                "nm_supports_demangle": nm_supports_demangle,
            },
        },
        "matches": [],
        "inline_sites": [],
        "summary": {},
    }

    # Candidate discovery is native when ripgrep is available. Candidate
    # listings are then parsed exactly once with bounded line/snippet state.
    # This preserves both source/pre-LTO views without materialising a
    # multi-gigabyte compiler listing in the long-lived MCP server.
    if not source_basename:
        warnings.append(
            "source_basename was not supplied; inline-site `.file`/`.loc` "
            "sweep skipped. Pass the source basename (for example "
            "'custom_scan.cppm') to request inline-source evidence; exact "
            "standalone pre-LTO symbol blocks are still inspected.")
    assembly_scan_started = time.monotonic()
    snippet_budget = _new_snippet_budget()
    asm_listings_for_match, inline_sites, assembly_scan_stats = (
        scan_assembly_listings(
            s_files=s_files,
            matches=matches,
            source_basename=source_basename or None,
            function_name=function_name,
            snippet_lines=snippet_lines,
            rg_path=tools.get("rg"),
            warnings=warnings,
            snippet_budget=snippet_budget,
        )
    )
    assembly_scan_duration_ms = int(
        (time.monotonic() - assembly_scan_started) * 1000)

    # Per-match objdump + histogram. Done after the .s sweep so the .s
    # text is already discarded by the time we hold disasm strings.
    assembly_snippets_truncated = snippet_budget["snippets_truncated"]
    for sym in matches:
        mangled = sym["mangled"]
        objdump_path, dis_warnings = disassemble_symbol(
            tools["objdump"], binary, mangled,
            scratch_dir, tools["demangler"])
        warnings.extend(dis_warnings)
        try:
            disasm_text = objdump_path.read_text()
        except OSError as e:
            warnings.append(
                f"could not read disassembly file {objdump_path}: {e}")
            disasm_text = ""
        hist = instruction_histogram(disasm_text, arch_hint)
        snippet, objdump_snippet_bytes, objdump_snippet_truncated = (
            _retain_text_snippet(disasm_text, snippet_lines, snippet_budget)
        )

        asm_info = asm_listings_for_match.get(mangled, {})
        out["matches"].append({
            "demangled_name": sym["demangled"],
            "mangled_name": mangled,
            "address": f"0x{sym['address']:x}",
            "size_bytes": sym.get("size"),
            "instruction_histogram": hist,
            "objdump_file": str(objdump_path),
            "objdump_snippet_first_lines": snippet,
            "objdump_snippet_bytes": objdump_snippet_bytes,
            "objdump_snippet_budget_truncated": objdump_snippet_truncated,
            "assembly_listing_file": asm_info.get("assembly_listing_file"),
            "assembly_listing_lines": asm_info.get("assembly_listing_lines"),
            "assembly_listing_snippet_first_lines":
                asm_info.get("assembly_listing_snippet_first_lines"),
            "assembly_listing_snippet_bytes":
                asm_info.get("assembly_listing_snippet_bytes"),
            "assembly_listing_snippet_budget_truncated":
                asm_info.get("assembly_listing_snippet_budget_truncated"),
        })

    post_lto_snippets_truncated = (
        snippet_budget["snippets_truncated"] - assembly_snippets_truncated)
    if post_lto_snippets_truncated:
        warnings.append(
            f"{post_lto_snippets_truncated} objdump snippet(s) were "
            "byte-truncated; full post-LTO evidence remains available via "
            "each objdump_file")

    out["inline_sites"] = inline_sites
    out["summary"] = {
        "matched_symbols": len(matches),
        "total_matches_before_cap": total_matches_before_cap,
        "total_symbols_scanned": len(syms),
        "asm_listings_discovered": assembly_scan_stats[
            "listings_discovered"],
        "asm_listings_candidates": assembly_scan_stats[
            "listings_candidates"],
        # Compatibility: historically this field meant listings discovered,
        # even though its name said "scanned". Keep that value stable and
        # expose the actual parser count separately.
        "asm_listings_scanned": assembly_scan_stats["listings_discovered"],
        "asm_listings_parsed": assembly_scan_stats["listings_scanned"],
        "asm_listing_bytes_streamed": assembly_scan_stats["bytes_streamed"],
        "asm_listing_prefilter_method": assembly_scan_stats[
            "prefilter_method"],
        "asm_listing_scan_duration_ms": assembly_scan_duration_ms,
        "asm_listing_phase_deadline_seconds": assembly_scan_stats[
            "phase_deadline_seconds"],
        "asm_listing_scan_incomplete": assembly_scan_stats[
            "scan_incomplete"],
        "asm_listing_incomplete_reason": assembly_scan_stats[
            "incomplete_reason"],
        "asm_listings_unparsed": assembly_scan_stats["listings_unparsed"],
        "asm_listing_read_failures": assembly_scan_stats["read_failures"],
        "asm_listing_truncated_lines": assembly_scan_stats[
            "truncated_lines"],
        "asm_listing_max_retained_line_bytes": assembly_scan_stats[
            "max_retained_line_bytes"],
        "retained_snippet_bytes": snippet_budget["bytes_retained"],
        "rejected_snippet_bytes": snippet_budget["bytes_rejected"],
        "snippets_byte_truncated": snippet_budget["snippets_truncated"],
        "snippet_per_item_byte_limit": MAX_RETAINED_SNIPPET_BYTES,
        "snippet_total_byte_limit": MAX_TOTAL_RETAINED_SNIPPET_BYTES,
        "asm_source_state_budget_exceeded_files": assembly_scan_stats[
            "source_state_budget_exceeded_files"],
        "asm_source_state_overflow_events": assembly_scan_stats[
            "source_state_overflow_events"],
        "asm_source_state_entries_cleared": assembly_scan_stats[
            "source_state_entries_cleared"],
        "asm_source_state_records_dropped": assembly_scan_stats[
            "source_state_records_dropped"],
        "asm_source_state_max_entries_retained": assembly_scan_stats[
            "source_state_max_entries_retained"],
        "asm_source_state_max_bytes_retained": assembly_scan_stats[
            "source_state_max_bytes_retained"],
        "asm_source_state_entry_limit": assembly_scan_stats[
            "source_state_entry_limit"],
        "asm_source_state_byte_limit": assembly_scan_stats[
            "source_state_byte_limit"],
        "total_loc_directives_in_inline_sites": sum(
            s["loc_directive_count"] for s in inline_sites),
        "total_debug_value_hits_in_inline_sites": sum(
            s["debug_value_hits"] for s in inline_sites),
    }
    if stripped_note:
        out["summary"]["note"] = stripped_note
    if total_matches_before_cap > MATCHES_WARN_THRESHOLD:
        warnings.append(
            f"function_name '{function_name}' matched "
            f"{total_matches_before_cap} symbols (capped to "
            f"{max_matches}). Pass a more specific prefix (e.g. with "
            f"the namespace) for a focused diff.")
    if warnings:
        out["warnings"] = warnings

    return out


# --------------------------- CLI for standalone testing --------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", required=True, type=Path)
    ap.add_argument("--target", required=True)
    ap.add_argument("--function", required=True,
                    help="Demangled function-name substring to search for")
    ap.add_argument("--max-matches", type=int, default=4)
    ap.add_argument("--scratch-dir", type=Path, default=DEFAULT_SCRATCH_DIR)
    ap.add_argument("--snippet-lines", type=int, default=200)
    ap.add_argument("--source-basename", default=None,
                    help="Basename of the source file (e.g. "
                         "'two_stacks_minimizers.cppm') for inline-site "
                         "sweep. When omitted, the inline-site sweep is "
                         "skipped; standalone symbol blocks remain enabled.")
    ap.add_argument("--no-llc", action="store_true",
                    help="Disable the llc-on-.bc fallback (skip the "
                         "pre-LTO view when no .s files are present).")
    ap.add_argument("--max-llc-bc-files", type=int, default=4)
    ap.add_argument("--binary-subdir", default="bioseq")
    args = ap.parse_args()

    out = run_inspect(
        function_name=args.function,
        target=args.target,
        build_dir=args.build_dir,
        max_matches=args.max_matches,
        snippet_lines=args.snippet_lines,
        source_basename=args.source_basename,
        llc_on_bc=not args.no_llc,
        max_llc_bc_files=args.max_llc_bc_files,
        scratch_dir=args.scratch_dir,
        binary_subdir=args.binary_subdir,
    )
    print(json.dumps(out, indent=2))
    return 0 if "error" not in out else 2


if __name__ == "__main__":
    sys.exit(main())
