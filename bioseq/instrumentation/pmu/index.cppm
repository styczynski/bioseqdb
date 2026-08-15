/**
 * @file instrumentation/pmu/index.cppm
 * @brief Portable per-thread performance-monitoring counters.
 *
 * Supported backends expose the same four-event sample. Unavailable counters
 * produce zero values, and disabled configurations eliminate reads at compile time.
 */
module;

#include <cstdint>

export module bioseq.instrumentation.pmu;

import bioseq.instrumentation.pmu.macos;
import bioseq.instrumentation.pmu.linux;
import bioseq.instrumentation.pmu.unsupported;

export namespace bioseq::instrumentation::pmu {

/**
 * @brief Hold the stable four-counter sample shared by region and loop metrics.
 *
 * Fields preserve kernel/backend order so conversion and field-wise subtraction
 * cannot silently exchange counter identities. Values are raw monotonically
 * increasing readings until `subtract_clamped()` produces a scope delta.
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
 * @brief Initialize the calling thread's performance-counter group.
 *
 * Initialization is cached per thread, so later calls take only the backend's
 * availability branch.
 *
 * @return `true` when the thread can read all counters, or `false` when the
 * platform or host permissions do not provide the complete group.
 */
[[nodiscard]] inline bool initialize_thread_counters() noexcept {
#if defined(__APPLE__)
    return macos::initialize_thread();
#elif defined(__linux__)
    return linux::initialize_thread();
#else
    return unsupported::initialize();
#endif
}

/**
 * @brief Close the calling thread's counter group when it is initialized.
 *
 * The operation is idempotent after failed initialization or prior shutdown.
 */
inline void shutdown_thread_counters() noexcept {
#if defined(__APPLE__)
    macos::shutdown_thread();
#elif defined(__linux__)
    linux::shutdown_thread();
#else
    unsupported::shutdown();
#endif
}

/**
 * @brief Report whether the calling thread has a functional counter group.
 *
 * @return `true` only after successful initialization on the current thread.
 */
[[nodiscard]] inline bool are_thread_counters_available() noexcept {
#if defined(__APPLE__)
    return macos::are_thread_counters_available();
#elif defined(__linux__)
    return linux::are_thread_counters_available();
#else
    return false;
#endif
}

/**
 * @brief Read one coherent sample from the calling thread's counter group.
 *
 * Backend values are monotonically increasing raw counts. Callers take a
 * baseline and use `subtract_clamped()` to obtain a region delta. A failed
 * lazy initialization returns an all-zero sample.
 *
 * @return Counters in cycles, instructions, branches, branch-misses order.
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
    if (!linux::initialize_thread()) {
        return counter_sample{};
    }
    const auto backend_sample = linux::read_counters();
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
 * @brief Compute a field-wise nonnegative counter delta.
 *
 * A counter that appears to move backwards because of rollover or scheduling
 * artifacts contributes zero. The other counter fields remain independent.
 *
 * @param[in] ending_sample Raw sample taken at scope end.
 * @param[in] beginning_sample Raw sample taken at scope beginning.
 * @return Counter deltas in the stable public field order.
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
