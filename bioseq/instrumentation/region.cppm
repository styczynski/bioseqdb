/**
 * @file instrumentation/region.cppm
 * @brief Scoped region timing and hardware-counter measurement.
 *
 * The explicitly templated overload follows the caller's dispatch
 * configuration. The non-templated overload targets the independently
 * installed benchmark profiler state. Both paths return immediately when no
 * matching state is installed.
 */
module;

#include <cstdint>

export module bioseq.instrumentation.region;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;
import bioseq.instrumentation.aggregation;
import bioseq.instrumentation.profiler_state;
import bioseq.instrumentation.clock;
import bioseq.instrumentation.runtime;
import bioseq.instrumentation.pmu;

export namespace bioseq::instrumentation {

/// Stack-local state captured at region begin. It occupies about 16 bytes and is never allocated.
struct region_measurement {
    const region_descriptor* descriptor = nullptr;
    std::uint64_t start_time_ns = 0;
    bool is_skipped = true;
};

/** @brief Scoped region measurement for one compile-time configuration. */
template<profiling_configuration p_configuration_type>
struct [[nodiscard]] region_scope {
    region_measurement measurement{};
    pmu::counter_sample counter_start{};

    /** @brief Begin a region measurement when profiling is active. */
    [[gnu::always_inline]]
    inline explicit region_scope(const region_descriptor* descriptor) noexcept {
        if constexpr (p_configuration_type::enable_profiling) {
            if (!is_runtime_enabled() || descriptor == nullptr || descriptor->id == 0) return;
            auto* profiler = active_thread_profiler<p_configuration_type>;
            if (profiler == nullptr) return;
            measurement.descriptor = descriptor;
            measurement.start_time_ns = clock::now_ns();
            if constexpr (p_configuration_type::enable_counters) {
                counter_start = pmu::read_thread_counters();
            }
            profiler->stack_depth += 1;
            measurement.is_skipped = false;
        } else {
            (void)descriptor;
        }
    }

    /** @brief Record elapsed time and available PMU deltas. */
    [[gnu::always_inline]] inline ~region_scope() noexcept {
        if constexpr (p_configuration_type::enable_profiling) {
            if (measurement.is_skipped) return;
            const auto end_ns = clock::now_ns();
            const auto duration_ns = end_ns - measurement.start_time_ns;
            pmu::counter_sample counter_delta{};
            if constexpr (p_configuration_type::enable_counters) {
                const auto counter_end = pmu::read_thread_counters();
                counter_delta = pmu::subtract_clamped(counter_end, counter_start);
            }
            auto* profiler = active_thread_profiler<p_configuration_type>;
            if (profiler == nullptr) return;
            if (profiler->stack_depth > 0) profiler->stack_depth -= 1;
            auto* aggregate = profiler->aggregates.find_or_create(measurement.descriptor->id);
            if (aggregate != nullptr) {
                record_region_duration(*aggregate, duration_ns);
                if constexpr (p_configuration_type::enable_counters) {
                    aggregate->total_cycles += counter_delta.cycles;
                    aggregate->total_instructions += counter_delta.instructions;
                    aggregate->total_branches += counter_delta.branches;
                    aggregate->total_branch_misses += counter_delta.branch_misses;
                }
            }
        }
    }

    region_scope(region_scope&&) = delete;
    region_scope(const region_scope&) = delete;
    region_scope& operator=(region_scope&&) = delete;
    region_scope& operator=(const region_scope&) = delete;
};

/** @brief Create a region scope for an explicit profiling configuration. */
template<profiling_configuration p_configuration_type>
[[nodiscard, gnu::always_inline]]
inline region_scope<p_configuration_type> region(const region_descriptor* descriptor) noexcept {
    return region_scope<p_configuration_type>(descriptor);
}

/** @brief Measure a region against the benchmark-owned profiler state. */
[[nodiscard, gnu::always_inline]]
inline region_scope<benchmark_profiling_configuration>
region(const region_descriptor* descriptor) noexcept {
    return region_scope<benchmark_profiling_configuration>(descriptor);
}

}  // namespace bioseq::instrumentation
