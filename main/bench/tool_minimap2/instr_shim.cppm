/**
 * @file instr_shim.cppm
 * @brief C-callable trampolines into the bioseq instrumentation runtime.
 *
 * The reference minimap2 sources under main/bench/tool_minimap2/minimap2/
 * are .c files compiled as C++ TUs. They cannot `import bioseq_instr;`
 * (regular TUs can in C++26, but the minimap2 sources only see plain
 * `#include "common.h"` in upstream form, so we keep them macro-driven).
 *
 * To wire those EASY_BLOCK call sites into our profiler without touching
 * the minimap2 sources beyond a single-line replacement, this module
 * imports bioseq_instr and exposes three extern-"C" functions:
 *
 *   bench_instr_region_register(name, file, line) — cold-path; resolves
 *      and caches a stable RegionDescriptor* for the given name.
 *   bench_instr_region_begin(desc)                — hot path; reads
 *      tls_profiler, returns t_begin_ns or 0 if profiling is off.
 *   bench_instr_region_end(desc, t_begin_ns)      — hot path; closes
 *      the region and updates the per-thread aggregate.
 *
 * The companion header (instr_shim.h) declares these in C linkage and
 * wraps them in a `BENCH_INSTR_BLOCK("name")` macro that mirrors the
 * shape of the original `EASY_BLOCK("name")` call sites we are replacing.
 *
 * Why a module unit, not a free header: the bioseq_instr API is defined
 * in C++26 modules (.cppm). Plain headers can't `import` them, but a
 * .cppm module unit can. The minimap2 .c files reach the shim via plain
 * extern "C" calls — no module dependency on their side.
 */
module;

#include <cstdint>

export module bench.tool_minimap2.instr_shim;

import bioseq.instr;

namespace bench::tool_minimap2_instr_shim {

// Pin the trampolines to the canonical "timing only" args — the same
// args the bench harness now installs (see report.cppm). Both sides of
// the side-by-side comparison go through the same args so the per-region
// overhead is identical and the delta is honest. The trampolines
// short-circuit when no block is installed (i.e., outside the bench
// harness or when profiling was runtime-disabled via set_runtime_enabled(false)).
using args_t = bioseq::instr::context_args_profiling_timing_only;

}  // namespace bench::tool_minimap2_instr_shim

extern "C" {

/// Register-or-lookup a region by name. Cold path; intended to feed
/// a static-local cache at the call site so subsequent visits cost only
/// a single load.
[[gnu::cold]]
const void* bench_instr_region_register(
    const char* name,
    const char* /*file*/,
    std::uint32_t /*line*/) noexcept
{
    // We discard file/line on the registration call: every shim
    // invocation runs through this single trampoline so std::source_location
    // would point at this function regardless. The descriptor's name is
    // the only stable identity we expose. Dedup is by (name_hash, line)
    // and since 'line' is fixed to register_region_static's own line,
    // dedup degenerates to "by name", which is exactly what we want.
    auto* desc = bioseq::instr::register_region_static(name);
    return static_cast<const void*>(desc);
}

/// Open a region timer. Returns the t_begin_ns timestamp or 0 if
/// profiling is currently disabled (no installed tls_profiler, or the
/// process-wide flag is off, or the descriptor is invalid). The caller
/// must pass the exact same desc + return value to ..._end().
[[gnu::hot]]
std::uint64_t bench_instr_region_begin(const void* desc_void) noexcept {
    using args_t = bench::tool_minimap2_instr_shim::args_t;
    if (!bioseq::instr::runtime_enabled()) return 0;
    if (desc_void == nullptr) return 0;
    const auto* desc = static_cast<const bioseq::instr::RegionDescriptor*>(desc_void);
    if (desc->id == 0) return 0;
    auto* prof = bioseq::instr::tls_profiler<args_t>;
    if (prof == nullptr) return 0;
    prof->stack_depth += 1;
    return bioseq::instr::clock::now_ns();
}

/// Close a region timer. No-op if t_begin_ns is 0 (i.e., the matching
/// _begin call short-circuited). Otherwise records (now - t_begin) into
/// the per-thread RegionAggregate for `desc`.
[[gnu::hot]]
void bench_instr_region_end(const void* desc_void, std::uint64_t t_begin_ns) noexcept {
    using args_t = bench::tool_minimap2_instr_shim::args_t;
    if (t_begin_ns == 0) return;
    if (desc_void == nullptr) return;
    const auto* desc = static_cast<const bioseq::instr::RegionDescriptor*>(desc_void);
    if (desc->id == 0) return;
    const auto end_ns   = bioseq::instr::clock::now_ns();
    const auto delta_ns = end_ns - t_begin_ns;
    auto* prof = bioseq::instr::tls_profiler<args_t>;
    if (prof == nullptr) return;
    if (prof->stack_depth > 0) prof->stack_depth -= 1;
    auto* agg = prof->aggregates.get_or_create(desc->id);
    if (agg != nullptr) {
        bioseq::instr::aggregate_record(*agg, delta_ns);
    }
}

}  // extern "C"
