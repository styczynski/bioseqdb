/**
 * @file instrumentation/clock.cppm
 * @brief Monotonic timestamps and raw hardware-counter ticks.
 *
 * now_ns returns nanoseconds. now_cycles returns the architecture's counter
 * ticks, or zero when no counter implementation is available
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
 * @brief Choose the Linux clock used for timing.
 *
 * BIOSEQ_BENCHMARK_E2E_INTERVALS uses CLOCK_MONOTONIC so benchmark intervals
 * can be compared with the minimap2 helper. Other builds use CLOCK_MONOTONIC_RAW.
 */
inline constexpr clockid_t now_ns_clock_id =
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    CLOCK_MONOTONIC;
#else
    CLOCK_MONOTONIC_RAW;
#endif
#endif

/**
 * @brief Read monotonic time in nanoseconds.
 *
 * Apple uses mach_absolute_time. Linux uses now_ns_clock_id, and other
 * platforms use std::chrono::steady_clock. Compare timestamps from the same clock.
 *
 * @return Monotonic timestamp in nanoseconds.
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
 * @brief Read  raw hardware-counter ticks.
 *
 * x86 uses RDTSCP and AArch64 uses CNTVCT_EL0. Differences measure elapsed
 * ticks when the platform counter is monotonic. Use readings from the same
 * counter. These ticks have diffrent units from now_ns.
 *
 * @return Counter ticks, or zero on an unsupported, architecture.
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
