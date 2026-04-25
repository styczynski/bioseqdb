/**
 * @file instrumentation/pmu/pmu.cppm
 * @brief Portable PMU group abstraction.
 *
 * The four-event canon group (cycles, instructions, branches,
 * branch_misses) is exposed across platforms. Each platform backend lives
 * in a sibling module guarded by platform macros; this header is the
 * uniform interface the region/loop scopes call into.
 *
 * Hot-path read overhead:
 *   - macOS: one sysctl (kpc_get_thread_counters), ~300 ns on M-series.
 *   - Linux: one read() on a perf-event group fd, ~200-400 ns depending
 *     on kernel version.
 *
 * The cost is paid only when Args::enable_counters is true and the
 * region is taken (skipped == false). The enable_counters bool sits
 * directly on context_args_profiling_full so the compiler eliminates
 * every PMU read in instantiations that don't ask for them.
 */
module;

#include <cstdint>

export module bioseq_instr.pmu;

import bioseq_instr.pmu.macos;
import bioseq_instr.pmu.linux;
import bioseq_instr.pmu.stub;

export namespace bioseq::instr::pmu {

/// Canonical 4-event sample. Always 32 bytes regardless of platform.
struct counters_t {
    std::uint64_t cycles        = 0;
    std::uint64_t instructions  = 0;
    std::uint64_t branches      = 0;
    std::uint64_t branch_misses = 0;
};

/// Initialise per-thread counter group. Idempotent: lazy-cached
/// thread-locally inside the backend, so the second-and-onward calls
/// short-circuit on a single load + branch. Returns false when no PMU
/// access is available on this platform (Windows, missing root on
/// macOS, perf_event_paranoid too high on Linux).
[[nodiscard]] inline bool init_thread_counters() noexcept {
#if defined(__APPLE__)
    return macos_backend::init_thread();
#elif defined(__linux__)
    return linux_backend::init_thread();
#else
    return stub::init();
#endif
}

/// Tear down per-thread counter group. Idempotent if init_thread_counters
/// returned false. Called from ThreadState's destructor on threads that
/// opted into PMU; safe to call from any thread.
inline void shutdown_thread_counters() noexcept {
#if defined(__APPLE__)
    macos_backend::shutdown_thread();
#elif defined(__linux__)
    linux_backend::shutdown_thread();
#else
    stub::shutdown();
#endif
}

/// True if the platform backend is functional on this host. False on
/// Windows or when PMU access was denied at init time.
[[nodiscard]] inline bool counters_available() noexcept {
#if defined(__APPLE__)
    return macos_backend::counters_available();
#elif defined(__linux__)
    return linux_backend::counters_available();
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
inline counters_t read_thread_counters() noexcept {
#if defined(__APPLE__)
    if (!macos_backend::init_thread()) return counters_t{};
    auto raw = macos_backend::read();
    return counters_t{
        .cycles        = raw.cycles,
        .instructions  = raw.instructions,
        .branches      = raw.branches,
        .branch_misses = raw.branch_misses,
    };
#elif defined(__linux__)
    if (!linux_backend::init_thread()) return counters_t{};
    auto raw = linux_backend::read();
    return counters_t{
        .cycles        = raw.cycles,
        .instructions  = raw.instructions,
        .branches      = raw.branches,
        .branch_misses = raw.branch_misses,
    };
#else
    return counters_t{};
#endif
}

/// Subtract two counter samples in field-wise fashion, taking care to
/// handle the rare case where the kernel returns out-of-order values
/// (counter rollover, scheduling artifacts) by clamping at zero.
[[nodiscard, gnu::always_inline]]
inline counters_t sub_clamped(const counters_t& end,
                              const counters_t& begin) noexcept {
    auto safe_sub = [](std::uint64_t a, std::uint64_t b) noexcept {
        return a >= b ? a - b : std::uint64_t{0};
    };
    return counters_t{
        .cycles        = safe_sub(end.cycles,        begin.cycles),
        .instructions  = safe_sub(end.instructions,  begin.instructions),
        .branches      = safe_sub(end.branches,      begin.branches),
        .branch_misses = safe_sub(end.branch_misses, begin.branch_misses),
    };
}

}  // namespace bioseq::instr::pmu
