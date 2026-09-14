/**
 * @file instrumentation/clock.cppm
 * @brief Portable wall-clock and cycle clock.
 *
 * `now_ns()` returns monotonic nanoseconds. `now_cycles()` returns the native
 * cycle counter, or zero when the architecture has no supported counter.
 */
module;

#include <chrono>
#include <cstdint>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#elif defined(__linux__)
#include <time.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

export module bioseq.instrumentation.clock;

export namespace bioseq::instrumentation::clock {

#if defined(__linux__)
/**
 * @brief Clock selected for framework interval timestamps on Linux.
 *
 * The dedicated E2E interval-evidence tree must share CLOCK_MONOTONIC's
 * physical rate with the immutable minimap2 helper receipt. Ordinary Linux
 * builds retain CLOCK_MONOTONIC_RAW.
 */
inline constexpr clockid_t now_ns_clock_id =
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    CLOCK_MONOTONIC;
#else
    CLOCK_MONOTONIC_RAW;
#endif
#endif

/**
 * @brief Read the platform's monotonic clock in nanoseconds.
 *
 * Apple uses `mach_absolute_time`. Linux uses `CLOCK_MONOTONIC` only in the
 * dedicated E2E interval-evidence tree and otherwise retains
 * `CLOCK_MONOTONIC_RAW`. Other platforms use `std::chrono::steady_clock`.
 * The returned value is an absolute timestamp suitable only for same-clock
 * differences.
 *
 * @return Monotonic time in nanoseconds.
 */
[[nodiscard, gnu::always_inline]]
inline std::uint64_t now_ns() noexcept {
#if defined(__APPLE__)
    static const auto timebase = []() noexcept {
        mach_timebase_info_data_t result{};
        mach_timebase_info(&result);
        return result;
    }();
    return mach_absolute_time() * timebase.numer / timebase.denom;
#elif defined(__linux__)
    struct timespec timestamp{};
    clock_gettime(now_ns_clock_id, &timestamp);
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(timestamp.tv_nsec);
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
#endif
}

/**
 * @brief Read the architecture's unscaled cycle or virtual-counter ticks.
 *
 * The value is not converted to nanoseconds: x86 reads RDTSCP and AArch64
 * reads CNTVCT_EL0. Callers may subtract readings under the platform's
 * monotonic-counter assumptions, but must not mix them with `now_ns()`.
 *
 * @return Raw counter ticks, or zero when the architecture has no backend.
 */
[[nodiscard, gnu::always_inline]]
inline std::uint64_t now_cycles() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned processor_id{};
    return __rdtscp(&processor_id);
#elif defined(__aarch64__)
    std::uint64_t cycle_count{};
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cycle_count));
    return cycle_count;
#else
    return 0;
#endif
}

} // namespace bioseq::instrumentation::clock
