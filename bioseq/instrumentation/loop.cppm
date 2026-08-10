/**
 * @file instrumentation/loop.cppm
 * @brief Scoped loop timing, iteration, work, and hardware-counter metrics.
 *
 * The templated overload follows pipeline dispatch. The non-templated overload
 * uses the separately installed benchmark profiler state.
 */
module;

#include <cstdint>

export module bioseq.instrumentation.loop;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;
import bioseq.instrumentation.aggregation;
import bioseq.instrumentation.profiler_state;
import bioseq.instrumentation.clock;
import bioseq.instrumentation.runtime;
import bioseq.instrumentation.pmu;

export namespace bioseq::instrumentation {

template<profiling_configuration p_configuration_type>
struct [[nodiscard]] loop_scope {
    const loop_descriptor* descriptor = nullptr;
    std::uint64_t start_time_ns = 0;
    std::uint64_t iterations = 0;
    std::uint64_t work = 0;
    bool is_skipped = true;
    pmu::counter_sample counter_start{};

    [[gnu::always_inline]]
    explicit loop_scope(const loop_descriptor* loop_descriptor) noexcept {
        if constexpr (p_configuration_type::enable_profiling) {
            if (!is_runtime_enabled() || loop_descriptor == nullptr || loop_descriptor->id == 0) return;
            auto* profiler = active_thread_profiler<p_configuration_type>;
            if (profiler == nullptr) return;
            descriptor = loop_descriptor;
            start_time_ns = clock::now_ns();
            if constexpr (p_configuration_type::enable_counters) {
                counter_start = pmu::read_thread_counters();
            }
            profiler->stack_depth += 1;
            is_skipped = false;
        } else {
            (void)loop_descriptor;
        }
    }

    [[gnu::always_inline]] ~loop_scope() noexcept {
        if constexpr (p_configuration_type::enable_profiling) {
            if (is_skipped) return;
            const auto end_ns = clock::now_ns();
            const auto duration_ns = end_ns - start_time_ns;
            pmu::counter_sample counter_delta{};
            if constexpr (p_configuration_type::enable_counters) {
                const auto counter_end = pmu::read_thread_counters();
                counter_delta = pmu::subtract_clamped(counter_end, counter_start);
            }
            auto* profiler = active_thread_profiler<p_configuration_type>;
            if (profiler == nullptr) return;
            if (profiler->stack_depth > 0) profiler->stack_depth -= 1;
            auto* aggregate = profiler->loop_aggregates.find_or_create(descriptor->id);
            if (aggregate != nullptr) {
                record_loop_duration(*aggregate, duration_ns, iterations, work);
                if constexpr (p_configuration_type::enable_counters) {
                    aggregate->total_cycles += counter_delta.cycles;
                    aggregate->total_instructions += counter_delta.instructions;
                    aggregate->total_branches += counter_delta.branches;
                    aggregate->total_branch_misses += counter_delta.branch_misses;
                }
            }
        }
    }

    [[gnu::always_inline]] void record_iteration(std::uint64_t work_units = 1) noexcept {
        if constexpr (p_configuration_type::enable_profiling) {
            iterations += 1;
            work += work_units;
        } else {
            (void)work_units;
        }
    }

    loop_scope(loop_scope&&) = delete;
    loop_scope(const loop_scope&) = delete;
    loop_scope& operator=(loop_scope&&) = delete;
    loop_scope& operator=(const loop_scope&) = delete;
};

template<profiling_configuration p_configuration_type>
[[nodiscard, gnu::always_inline]]
inline loop_scope<p_configuration_type> loop(const loop_descriptor* descriptor) noexcept {
    return loop_scope<p_configuration_type>(descriptor);
}

/// Non-templated convenience overload — see region.cppm for rationale.
/// Pins to `benchmark_profiling_configuration` so the per-iteration
/// loop scope skips the PMU read on construction/destruction.
[[nodiscard, gnu::always_inline]]
inline loop_scope<benchmark_profiling_configuration>
loop(const loop_descriptor* descriptor) noexcept {
    return loop_scope<benchmark_profiling_configuration>(descriptor);
}

}  // namespace bioseq::instrumentation
