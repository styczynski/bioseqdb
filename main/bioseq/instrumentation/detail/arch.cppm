/**
 * @file instrumentation/detail/arch.cppm
 * @brief Architecture-specific intrinsics — rdtsc, cntvct_el0, QPC.
 *
 * The portable user-facing wrapper lives in bioseq_instr.clock; this
 * module surfaces the raw helpers for callers that want lower-level
 * access (calibration, sampler implementation).
 */
module;

#include <cstdint>

#if defined(__APPLE__)
#  include <mach/mach_time.h>
#endif

#if defined(_WIN32)
#  include <windows.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#  include <x86intrin.h>
#endif

export module bioseq.instr.detail.arch;

export namespace bioseq::instr::detail::arch {

/// Raw cycle counter. ~25 cycles on x86 (rdtscp), ~10 cycles on arm64
/// (mrs cntvct_el0). Not portable across cores on hardware without
/// invariant TSC; modern hardware (last 10 years) is fine.
[[nodiscard, gnu::always_inline]]
inline std::uint64_t rdtsc() noexcept {
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

/// Counter frequency in Hz, where known. 0 if unknown.
[[nodiscard]]
inline std::uint64_t counter_frequency_hz() noexcept {
#if defined(__aarch64__)
    std::uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
#elif defined(_WIN32)
    LARGE_INTEGER li;
    if (QueryPerformanceFrequency(&li)) {
        return static_cast<std::uint64_t>(li.QuadPart);
    }
    return 0;
#else
    return 0;
#endif
}

}  // namespace bioseq::instr::detail::arch
