/**
 * @file instrumentation/clock.cppm
 * @brief Portable wall-clock and cycle clock.
 *
 * Two functions: now_ns() returns a monotonic nanosecond timestamp,
 * now_cycles() returns a cycle counter (or 0 when unavailable).
 *
 * Implementations:
 *   - macOS arm64: mach_absolute_time + cached timebase ratio.
 *     mach_absolute_time on Apple Silicon is the cntvct_el0 read.
 *   - Linux: clock_gettime(CLOCK_MONOTONIC_RAW) (vDSO-accelerated).
 *   - Cycle counter: rdtscp on x86, mrs cntvct_el0 on arm64.
 */
module;

#include <chrono>
#include <cstdint>

#if defined(__APPLE__)
#  include <mach/mach_time.h>
#elif defined(__linux__)
#  include <time.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#  include <x86intrin.h>
#endif

export module bioseq_instr.clock;

export namespace bioseq::instr::clock {

/// Monotonic nanosecond timestamp. Cheap on every supported platform.
[[nodiscard, gnu::always_inline]]
inline std::uint64_t now_ns() noexcept {
#if defined(__APPLE__)
    static const auto tb = []() {
        mach_timebase_info_data_t v{};
        mach_timebase_info(&v);
        return v;
    }();
    return mach_absolute_time() * tb.numer / tb.denom;
#elif defined(__linux__)
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull
         + static_cast<std::uint64_t>(ts.tv_nsec);
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

/// Cycle counter. Returns 0 if not available.
[[nodiscard, gnu::always_inline]]
inline std::uint64_t now_cycles() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned aux;
    return __rdtscp(&aux);
#elif defined(__aarch64__)
    std::uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

}  // namespace bioseq::instr::clock
