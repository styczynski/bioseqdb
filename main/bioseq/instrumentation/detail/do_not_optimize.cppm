/**
 * @file instrumentation/detail/do_not_optimize.cppm
 * @brief Compiler barrier helpers used by calibration code.
 *
 * Marked [[deprecated]] so any inadvertent use outside calibration code
 * triggers a build-time warning (Part 25.18 of the design doc).
 */
module;

#include <cstdint>

export module bioseq.instr.detail.do_not_optimize;

export namespace bioseq::instr::detail {

/// Force the compiler to keep `value` materialised. Used to prevent
/// dead-code elimination of synthetic benchmark workloads. NEVER call
/// from regular code paths.
template<class T>
[[deprecated("calibration helper; do not call from regular code paths"),
 gnu::always_inline]]
inline void do_not_optimize(T const& value) noexcept {
#if defined(__clang__)
    __asm__ volatile("" : : "r,m"(value) : "memory");
#elif defined(__GNUC__)
    __asm__ volatile("" : : "r,m"(value) : "memory");
#else
    (void)value;
#endif
}

/// Read-side variant: force the compiler to read `value` from memory.
template<class T>
[[deprecated("calibration helper; do not call from regular code paths"),
 gnu::always_inline]]
inline void do_not_optimize(T& value) noexcept {
#if defined(__clang__)
    __asm__ volatile("" : "+r,m"(value) : : "memory");
#elif defined(__GNUC__)
    __asm__ volatile("" : "+r,m"(value) : : "memory");
#else
    (void)value;
#endif
}

}  // namespace bioseq::instr::detail
