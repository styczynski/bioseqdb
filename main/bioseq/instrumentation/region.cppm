/**
 * @file instrumentation/region.cppm
 * @brief RegionScope<Args> RAII type and the user-facing region() helper.
 *
 * Hot-path entry point. The scope reads the typed profiler block from
 * `tls_profiler<Args>`. When Args::enable_profiling is false, every
 * body is empty and the type collapses.
 *
 * Two flavours of `region()` ship:
 *   - the explicitly-templated `region<Args>(desc)` used inside templated
 *     pipeline scopes (the dispatch fork's lambda body); and
 *   - the non-templated `region(desc)` that fixes Args =
 *     context_args_profiling_full. The non-templated form is the
 *     migration target for the legacy EASY_BLOCK call sites buried in
 *     non-templated chain/seed helpers — when the dispatch fork picks
 *     no-profiling, `tls_profiler<context_args_profiling_full>` stays
 *     null and the scope short-circuits with a single load + branch.
 */
module;

#include <cstdint>

export module bioseq_instr.region;

import bioseq_instr.config;
import bioseq_instr.descriptor;
import bioseq_instr.aggregate;
import bioseq_instr.thread_state;
import bioseq_instr.clock;
import bioseq_instr.global_state;
import bioseq_instr.pmu;

export namespace bioseq::instr {

/// Stack-local state captured at region begin. ~16 bytes; never allocated.
struct Region {
    const RegionDescriptor* desc = nullptr;
    std::uint64_t           t_begin_ns = 0;
    bool                    skipped = true;
};

template<is_context_args Args>
struct [[nodiscard]] RegionScope {
    Region r{};
    /// PMU baseline. Sized to zero bytes when enable_counters is false
    /// thanks to the conditional storage; the field exists in every
    /// instantiation but consumes no space when unused, so the scope's
    /// size is unchanged in the common no-counters paths.
    pmu::counters_t pmu_begin{};

    [[gnu::always_inline]]
    explicit RegionScope(const RegionDescriptor* d) noexcept {
        if constexpr (Args::enable_profiling) {
            if (!runtime_enabled() || d == nullptr || d->id == 0) return;
            auto* prof = tls_profiler<Args>;
            if (prof == nullptr) return;
            r.desc = d;
            r.t_begin_ns = clock::now_ns();
            if constexpr (Args::enable_counters) {
                pmu_begin = pmu::read_thread_counters();
            }
            prof->stack_depth += 1;
            r.skipped = false;
        } else {
            (void)d;
        }
    }

    [[gnu::always_inline]] ~RegionScope() noexcept {
        if constexpr (Args::enable_profiling) {
            if (r.skipped) return;
            const auto end_ns = clock::now_ns();
            const auto delta_ns = end_ns - r.t_begin_ns;
            // Read counters BEFORE touching the aggregate so the delta
            // brackets only the work between the two now_ns / read calls.
            pmu::counters_t pmu_delta{};
            if constexpr (Args::enable_counters) {
                const auto pmu_end = pmu::read_thread_counters();
                pmu_delta = pmu::sub_clamped(pmu_end, pmu_begin);
            }
            auto* prof = tls_profiler<Args>;
            if (prof == nullptr) return;
            if (prof->stack_depth > 0) prof->stack_depth -= 1;
            auto* agg = prof->aggregates.get_or_create(r.desc->id);
            if (agg != nullptr) {
                aggregate_record(*agg, delta_ns);
                if constexpr (Args::enable_counters) {
                    agg->total_cycles        += pmu_delta.cycles;
                    agg->total_instructions  += pmu_delta.instructions;
                    agg->total_branches      += pmu_delta.branches;
                    agg->total_branch_misses += pmu_delta.branch_misses;
                }
            }
        }
    }

    RegionScope(RegionScope&&) = delete;
    RegionScope(const RegionScope&) = delete;
    RegionScope& operator=(RegionScope&&) = delete;
    RegionScope& operator=(const RegionScope&) = delete;
};

template<is_context_args Args>
[[nodiscard, gnu::always_inline]]
inline RegionScope<Args> region(const RegionDescriptor* d) noexcept {
    return RegionScope<Args>(d);
}

/// Non-templated convenience overload. Routes to the
/// `context_args_profiling_timing_only` instantiation. This is the
/// canonical pin for legacy EASY_BLOCK call sites in non-templated
/// helpers (chaining, kmer extraction, KSW2). The previous default was
/// `context_args_profiling_full`, which always read the per-thread PMU
/// counters at scope begin/end (one sysctl on macOS, one read() syscall
/// on Linux — ~300-400 ns per region scope). The bench's per-section
/// region count is in the tens of thousands, so PMU reads were burning
/// 9-27 ms per section purely on instrumentation. The timing_only
/// variant drops PMU to leave just the clock read; PMU users can still
/// opt back in by explicitly writing `region<context_args_profiling_full>(d)`.
[[nodiscard, gnu::always_inline]]
inline RegionScope<context_args_profiling_timing_only>
region(const RegionDescriptor* d) noexcept {
    return RegionScope<context_args_profiling_timing_only>(d);
}

}  // namespace bioseq::instr
