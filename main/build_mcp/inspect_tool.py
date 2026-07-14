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

Returns a single JSON document matching the schema in
IMPROVE_BUILD_MCP_IMPL_PLAN_1.md §3.

Latency story: lazy with disk-cached llc fallback. First call after a
fresh build is ~25-100s (one llc per relevant .bc, capped via
max_llc_bc_files=4). Subsequent calls reuse the cached `.s` and
complete in ~6-9s. Cache invalidates on .bc mtime.

Memory contract: this module is invoked from the long-lived MCP server.
A single llc-emitted `.s` can be ~273 MB. We therefore stream-scan each
`.s` exactly once per call and never hold contents across calls. See
plan Risk #16.

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


# --------------------------- function block extraction ---------------------

def extract_function_block(s_text: str, mangled: str) -> Optional[tuple[int, int, str]]:
    """Find `<mangled>:` (must start at column 0) and return its full
    function body delimited by matched `.cfi_startproc`/`.cfi_endproc`.

    Mach-O assembly may use a one-extra-`_` prefix convention vs the binary
    symbol table; we accept either form.

    Returns (start_line_1based, end_line_1based, snippet) or None.
    """
    candidates = [mangled]
    if mangled.startswith("_"):
        candidates.append(mangled[1:])
    else:
        candidates.append("_" + mangled)
    lines = s_text.splitlines()

    start = None
    for i, line in enumerate(lines):
        for cand in candidates:
            if line.startswith(cand + ":"):
                # Sanity: the next ~8 lines should contain .cfi_startproc.
                # Without this the regex would match unrelated labels that
                # happen to share the prefix (e.g. `_ZN…__local_label`).
                for j in range(i, min(i + 8, len(lines))):
                    if ".cfi_startproc" in lines[j]:
                        start = i
                        break
                if start is not None:
                    break
        if start is not None:
            break

    if start is None:
        return None

    end = None
    depth = 0
    for j in range(start, len(lines)):
        if ".cfi_startproc" in lines[j]:
            depth += 1
        elif ".cfi_endproc" in lines[j]:
            depth -= 1
            if depth == 0:
                end = j
                break
    if end is None:
        return None
    snippet = "\n".join(lines[start: end + 1])
    return start + 1, end + 1, snippet


# --------------------------- .file/.loc resolution -------------------------

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


def parse_file_table(s_text: str) -> dict[int, str]:
    """Return {file_index → "comp_dir/file_name"} from .file directives."""
    table: dict[int, str] = {}
    for line in s_text.splitlines():
        m = FILE_DIR_RE.match(line)
        if m:
            idx = int(m.group(1))
            comp_dir = m.group(2) or ""
            fname = m.group(3) or ""
            table[idx] = (str(Path(comp_dir) / fname) if comp_dir
                          else fname)
    return table


def scan_inline_site(text: str, s_file: Path,
                     source_basename: str,
                     func_name: str) -> Optional[dict]:
    """Walk a single `.s` file's text and accumulate inline-site signals.

    Returns the per-file inline-site dict, or None if the .s contains
    neither matching `.loc` directives nor matching DEBUG_VALUE
    annotations (so we don't emit empty entries for unrelated TUs).
    """
    if source_basename not in text:
        return None
    ftab = parse_file_table(text)
    target_indices = {idx for idx, path in ftab.items()
                      if path.endswith(source_basename)}
    if not target_indices:
        return None
    loc_hits = 0
    first_loc_line: Optional[int] = None
    last_loc_line: Optional[int] = None
    debug_value_hits = 0
    for i, line in enumerate(text.splitlines(), start=1):
        m = LOC_RE.match(line)
        if m and int(m.group(1)) in target_indices:
            loc_hits += 1
            if first_loc_line is None:
                first_loc_line = i
            last_loc_line = i
        if "DEBUG_VALUE" in line:
            m = DEBUG_VALUE_RE.search(line)
            if m and m.group(1).strip() == func_name:
                debug_value_hits += 1
    if not (loc_hits or debug_value_hits):
        return None
    return {
        "assembly_listing_file": str(s_file),
        "loc_directive_count": loc_hits,
        "first_loc_line": first_loc_line,
        "last_loc_line": last_loc_line,
        "debug_value_hits": debug_value_hits,
        "resolved_file_indices": sorted(target_indices),
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
    """Inspect a function. Returns the JSON-serialisable dict described in
    IMPROVE_BUILD_MCP_IMPL_PLAN_1.md §3.

    On error: returns `{"error": "...", ...}` — never raises. Callers
    serialise the dict to JSON and ship it back to the AI agent.

    Side effects: writes objdump output and (when llc_on_bc=True) cached
    `.s` files to disk. Reads but does not modify the build directory.
    """
    if not function_name:
        return _error("function_name must be a non-empty string")

    scratch_dir = scratch_dir or DEFAULT_SCRATCH_DIR
    binary_root = build_dir / binary_subdir
    candidate_names = artifact_names or (target,)
    binary_candidates = tuple(binary_root / name for name in candidate_names)
    binary = next((path for path in binary_candidates if path.exists()),
                  binary_candidates[0])

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
        },
        "build_metadata": {
            "object_format": obj_format,
            "arch_hint": arch_hint,
            "tools": {
                "nm": tools["nm"],
                "objdump": tools["objdump"],
                "demangler": tools["demangler"],
                "llc": tools["llc"],
                "objdump_supports_disassemble_symbols": objdump_supports_filter,
                "nm_supports_demangle": nm_supports_demangle,
            },
        },
        "matches": [],
        "inline_sites": [],
        "summary": {},
    }

    # Stream-scan .s files: read each ONCE per call, do BOTH the
    # function-block extraction (for matches) AND the inline-site sweep
    # in the same pass, then drop the text. This is the Phase B contract
    # against unbounded MCP-server memory growth (plan Risk #16).
    asm_listings_for_match: dict[str, dict] = {}
    inline_sites: list[dict] = []
    source_basename_resolved = source_basename or f"{function_name}.cppm"

    for s_file in s_files:
        try:
            text = s_file.read_text(errors="replace")
        except OSError as e:
            warnings.append(f"failed to read {s_file}: {e}")
            continue

        for sym in matches:
            mangled = sym["mangled"]
            if mangled in asm_listings_for_match:
                continue  # first match wins; don't reread the symbol later
            block = extract_function_block(text, mangled)
            if block:
                start, end, txt = block
                asm_listings_for_match[mangled] = {
                    "assembly_listing_file": str(s_file),
                    "assembly_listing_lines": [start, end],
                    "assembly_listing_snippet_first_lines":
                        "\n".join(txt.splitlines()[:snippet_lines]),
                }

        site = scan_inline_site(text, s_file,
                                source_basename_resolved, function_name)
        if site:
            inline_sites.append(site)
        # `text` falls out of scope here — reclaimed before the next .s.

    # Per-match objdump + histogram. Done after the .s sweep so the .s
    # text is already discarded by the time we hold disasm strings.
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
        snippet = "\n".join(disasm_text.splitlines()[:snippet_lines])

        asm_info = asm_listings_for_match.get(mangled, {})
        out["matches"].append({
            "demangled_name": sym["demangled"],
            "mangled_name": mangled,
            "address": f"0x{sym['address']:x}",
            "size_bytes": sym.get("size"),
            "instruction_histogram": hist,
            "objdump_file": str(objdump_path),
            "objdump_snippet_first_lines": snippet,
            "assembly_listing_file": asm_info.get("assembly_listing_file"),
            "assembly_listing_lines": asm_info.get("assembly_listing_lines"),
            "assembly_listing_snippet_first_lines":
                asm_info.get("assembly_listing_snippet_first_lines"),
        })

    out["inline_sites"] = inline_sites
    out["summary"] = {
        "matched_symbols": len(matches),
        "total_matches_before_cap": total_matches_before_cap,
        "total_symbols_scanned": len(syms),
        "asm_listings_scanned": len(s_files),
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
                         "sweep. Defaults to '<function>.cppm'.")
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
