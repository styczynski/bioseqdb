/**
 * @file instrumentation/pmu/index.cppm
 * @brief Portable per-thread performance-monitoring counters.
 *
 * The same four-event sample is exposed on each supported platform. An
 * unavailable platform or permission failure produces a zero sample.
 *
 * Hot-path read overhead:
 *   - macOS: one sysctl (kpc_get_thread_counters), ~300 ns on M-series.
 *   - Linux: one read() on a perf-event group fd, ~200-400 ns depending
 *     on kernel version.
 *
 * The cost is paid only when the selected profiling configuration enables
 * counters and the region is taken (skipped == false). The enable_counters bool sits
 * directly on production_profiling_configuration so the compiler eliminates
 * every PMU read in instantiations that don't ask for them.
 */
module;

#include <cstdint>

export module bioseq.instrumentation.pmu;

import bioseq.instrumentation.pmu.macos;
import bioseq.instrumentation.pmu.linux;
import bioseq.instrumentation.pmu.unsupported;

export namespace bioseq::instrumentation::pmu {

/** @brief Stable four-counter sample shared by region and loop metrics. */
struct counter_sample {
    std::uint64_t cycles        = 0;
    std::uint64_t instructions  = 0;
    std::uint64_t branches      = 0;
    std::uint64_t branch_misses = 0;
};
static_assert(sizeof(counter_sample) == 32);
static_assert(alignof(counter_sample) == alignof(std::uint64_t));

/// Initialise per-thread counter group. Idempotent: lazy-cached
/// thread-locally inside the backend, so the second-and-onward calls
/// short-circuit on a single load + branch. Returns false when no PMU
/// access is available on this platform (Windows, missing root on
/// macOS, perf_event_paranoid too high on Linux).
[[nodiscard]] inline bool initialize_thread_counters() noexcept {
#if defined(__APPLE__)
    return macos::initialize_thread();
#elif defined(__linux__)
    return linux::initialize_thread();
#else
    return unsupported::initialize();
#endif
}

/// Tear down per-thread counter group. Idempotent if initialize_thread_counters
/// returned false. Safe to call from any thread.
inline void shutdown_thread_counters() noexcept {
#if defined(__APPLE__)
    macos::shutdown_thread();
#elif defined(__linux__)
    linux::shutdown_thread();
#else
    unsupported::shutdown();
#endif
}

/// True if the platform backend is functional on this host. False on
/// Windows or when PMU access was denied at init time.
[[nodiscard]] inline bool are_thread_counters_available() noexcept {
#if defined(__APPLE__)
    return macos::are_thread_counters_available();
#elif defined(__linux__)
    return linux::are_thread_counters_available();
#else
    return false;
#endif
}

/// Read the current counter values. The kernel hands back monotonically-
/// increasing counter values; the caller subtracts a baseline taken at
/// scope-begin to obtain the delta. Returns zeros when the per-thread
/// group is not initialised, including on the very first call (which
/// triggers a lazy init in this same call so the *second* call has the
/// fast path).
[[nodiscard, gnu::always_inline]]
inline counter_sample read_thread_counters() noexcept {
#if defined(__APPLE__)
    if (!macos::initialize_thread()) return counter_sample{};
    const auto backend_sample = macos::read_counters();
    return counter_sample{
        .cycles = backend_sample.cycles,
        .instructions = backend_sample.instructions,
        .branches = backend_sample.branches,
        .branch_misses = backend_sample.branch_misses,
    };
#elif defined(__linux__)
    if (!linux::initialize_thread()) return counter_sample{};
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

/// Subtract two counter samples in field-wise fashion, taking care to
/// handle the rare case where the kernel returns out-of-order values
/// (counter rollover, scheduling artifacts) by clamping at zero.
[[nodiscard, gnu::always_inline]]
inline counter_sample subtract_clamped(
    const counter_sample& ending_sample,
    const counter_sample& beginning_sample) noexcept
{
    auto subtract_without_underflow = [](std::uint64_t end, std::uint64_t begin) noexcept {
        return end >= begin ? end - begin : std::uint64_t{0};
    };
    return counter_sample{
        .cycles = subtract_without_underflow(ending_sample.cycles, beginning_sample.cycles),
        .instructions = subtract_without_underflow(
            ending_sample.instructions, beginning_sample.instructions),
        .branches = subtract_without_underflow(
            ending_sample.branches, beginning_sample.branches),
        .branch_misses = subtract_without_underflow(
            ending_sample.branch_misses, beginning_sample.branch_misses),
    };
}

}  // namespace bioseq::instrumentation::pmu
