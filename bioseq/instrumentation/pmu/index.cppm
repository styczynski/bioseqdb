/**
 * @file instrumentation/pmu/index.cppm
 * @brief Read hardware counters through the platform implementation.
 *
 * Reads return cycles, instructions, branches, and branch misses.
 * Failed initialization or reads produce zero values.
 */
module;

#include <cstdint>

export module bioseq.instrumentation.pmu;

import bioseq.instrumentation.pmu.macos;
import bioseq.instrumentation.pmu.linux_backend;
import bioseq.instrumentation.pmu.unsupported;

export namespace bioseq::instrumentation::pmu {

/**
 * @brief Hardware counts used by region and loop measurements.
 *
 * Take readings before and after a scope, then use subtract_clamped to
 * calculate the changes. Field names keep the four event types separate.
 */
struct counter_sample {
    std::uint64_t cycles = 0;
    std::uint64_t instructions = 0;
    std::uint64_t branches = 0;
    std::uint64_t branch_misses = 0;
};
static_assert(sizeof(counter_sample) == 32);
static_assert(alignof(counter_sample) == alignof(std::uint64_t));

/**
 * @brief Initialize the platform's counters for the calling thread.
 *
 * Linux and macOS cache their setup result.
 * @return Backend setup result. The unsupported stub returns true while
 * reporting counters unavailable.
 */
[[nodiscard]] inline bool initialize_thread_counters() noexcept {
#if defined(__APPLE__)
    return macos::initialize_thread();
#elif defined(__linux__)
    return linux_backend::initialize_thread();
#else
    return unsupported::initialize();
#endif
}

/**
 * @brief Ask the platform implementation to stop thread counter collection.
 *
 * Linux closes the event group. macOS requests that thread counting stop
 * and keeps its process-wide setup.
 */
inline void shutdown_thread_counters() noexcept {
#if defined(__APPLE__)
    macos::shutdown_thread();
#elif defined(__linux__)
    linux_backend::shutdown_thread();
#else
    unsupported::shutdown();
#endif
}

/**
 * @brief Return the platform's recorded availability state.
 * @return Thread setup state on Linux, process setup state on macOS, or false
 * on unsupported platforms.
 */
[[nodiscard]] inline bool are_thread_counters_available() noexcept {
#if defined(__APPLE__)
    return macos::are_thread_counters_available();
#elif defined(__linux__)
    return linux_backend::are_thread_counters_available();
#else
    return false;
#endif
}

/**
 * @brief Read the calling thread's hardware counters.
 *
 * Initialize them on first use. Take a baseline and use subtract_clamped
 * to measure changes over a scope.
 *
 * @return Raw counter values, or zeros when setup or reading fails.
 */
[[nodiscard, gnu::always_inline]]
inline counter_sample read_thread_counters() noexcept {
#if defined(__APPLE__)
    if (!macos::initialize_thread()) {
        return counter_sample{};
    }
    const auto backend_sample = macos::read_counters();
    return counter_sample{
        .cycles = backend_sample.cycles,
        .instructions = backend_sample.instructions,
        .branches = backend_sample.branches,
        .branch_misses = backend_sample.branch_misses,
    };
#elif defined(__linux__)
    if (!linux_backend::initialize_thread()) {
        return counter_sample{};
    }
    const auto backend_sample = linux_backend::read_counters();
    return counter_sample{
        .cycles = backend_sample.cycles,
        .instructions = backend_sample.instructions,
        .branches = backend_sample.branches,
        .branch_misses = backend_sample.branch_misses,
    };
#else
    return counter_sample{};
#endif
}

/**
 * @brief Calculate the change in each hardware counter.
 *
 * If an ending value is below its baseline, return zero for that counter.
 * This avoids unsigned underflow after a reset, rollover, or failed read.
 *
 * @param[in] ending_sample Reading at scope end.
 * @param[in] beginning_sample Reading at scope start.
 * @return Nonnegative change in each counter.
 */
[[nodiscard, gnu::always_inline]]
inline counter_sample subtract_clamped(const counter_sample& ending_sample,
                                       const counter_sample& beginning_sample) noexcept {
    constexpr auto subtract_without_underflow = [](std::uint64_t ending_value,
                                                   std::uint64_t beginning_value) noexcept {
        return ending_value >= beginning_value ? ending_value - beginning_value : std::uint64_t{0};
    };
    return counter_sample{
        .cycles = subtract_without_underflow(ending_sample.cycles, beginning_sample.cycles),
        .instructions =
            subtract_without_underflow(ending_sample.instructions, beginning_sample.instructions),
        .branches = subtract_without_underflow(ending_sample.branches, beginning_sample.branches),
        .branch_misses =
            subtract_without_underflow(ending_sample.branch_misses, beginning_sample.branch_misses),
    };
}

} // namespace bioseq::instrumentation::pmu
