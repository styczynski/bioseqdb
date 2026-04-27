/**
 * @file instrumentation/loop.cppm
 * @brief LoopScope<Args> RAII type with tick().
 *
 * Two flavours of `loop()` ship; the non-templated overload fixes
 * Args = context_args_profiling_full so legacy EASY_BLOCK call sites
 * inside non-templated helpers can opt into the profiler without
 * threading Args through their callers. See region.cppm for the same
 * pattern and rationale.
 */
module;

#include <cstdint>

export module bioseq_instr.loop;

import bioseq_instr.config;
import bioseq_instr.descriptor;
import bioseq_instr.aggregate;
import bioseq_instr.thread_state;
import bioseq_instr.clock;
import bioseq_instr.global_state;
import bioseq_instr.pmu;

export namespace bioseq::instr {

template<is_context_args Args>
struct [[nodiscard]] LoopScope {
    const LoopDescriptor* desc = nullptr;
    std::uint64_t         t_begin_ns = 0;
    std::uint64_t         iterations = 0;
    std::uint64_t         work = 0;
    bool                  skipped = true;
    pmu::counters_t       pmu_begin{};

    [[gnu::always_inline]]
    explicit LoopScope(const LoopDescriptor* d) noexcept {
        if constexpr (Args::enable_profiling) {
            if (!runtime_enabled() || d == nullptr || d->id == 0) return;
            auto* prof = tls_profiler<Args>;
            if (prof == nullptr) return;
            desc = d;
            t_begin_ns = clock::now_ns();
            if constexpr (Args::enable_counters) {
                pmu_begin = pmu::read_thread_counters();
            }
            prof->stack_depth += 1;
            skipped = false;
        } else {
            (void)d;
        }
    }

    [[gnu::always_inline]] ~LoopScope() noexcept {
        if constexpr (Args::enable_profiling) {
            if (skipped) return;
            const auto end_ns = clock::now_ns();
            const auto delta_ns = end_ns - t_begin_ns;
            pmu::counters_t pmu_delta{};
            if constexpr (Args::enable_counters) {
                const auto pmu_end = pmu::read_thread_counters();
                pmu_delta = pmu::sub_clamped(pmu_end, pmu_begin);
            }
            auto* prof = tls_profiler<Args>;
            if (prof == nullptr) return;
            if (prof->stack_depth > 0) prof->stack_depth -= 1;
            auto* agg = prof->loop_aggregates.get_or_create(desc->id);
            if (agg != nullptr) {
                loop_aggregate_record(*agg, delta_ns, iterations, work);
                if constexpr (Args::enable_counters) {
                    agg->total_cycles        += pmu_delta.cycles;
                    agg->total_instructions  += pmu_delta.instructions;
                    agg->total_branches      += pmu_delta.branches;
                    agg->total_branch_misses += pmu_delta.branch_misses;
                }
            }
        }
    }

    [[gnu::always_inline]] void tick(std::uint64_t units = 1) noexcept {
        if constexpr (Args::enable_profiling) {
            iterations += 1;
            work += units;
        } else {
            (void)units;
        }
    }

    LoopScope(LoopScope&&) = delete;
    LoopScope(const LoopScope&) = delete;
    LoopScope& operator=(LoopScope&&) = delete;
    LoopScope& operator=(const LoopScope&) = delete;
};

template<is_context_args Args>
[[nodiscard, gnu::always_inline]]
inline LoopScope<Args> loop(const LoopDescriptor* d) noexcept {
    return LoopScope<Args>(d);
}

/// Non-templated convenience overload — see region.cppm for rationale.
/// Pins to `context_args_profiling_timing_only` so the per-iteration
/// loop scope skips the PMU read on construction/destruction.
[[nodiscard, gnu::always_inline]]
inline LoopScope<context_args_profiling_timing_only>
loop(const LoopDescriptor* d) noexcept {
    return LoopScope<context_args_profiling_timing_only>(d);
}

}  // namespace bioseq::instr
