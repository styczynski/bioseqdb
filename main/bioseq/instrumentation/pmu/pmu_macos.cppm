/**
 * @file instrumentation/pmu/pmu_macos.cppm
 * @brief Apple Silicon kperf backend sketch (Part 10.1 / Part 14 of the
 *        design doc).
 *
 * v1 status: not yet wired into the hot path. The full kperf path
 * dlopen()s /System/Library/PrivateFrameworks/kperf.framework, configures
 * the canonical 4 events (FIXED_CYCLES, FIXED_INSTRUCTIONS, INST_BRANCH,
 * BRANCH_MISPRED_NONSPEC), and reads via kpc_get_thread_counters. macOS
 * may require additional entitlements; bencher's reference implementation
 * gracefully degrades to timing only when forbidden. This module
 * preserves the symbol surface for the future activation.
 */
module;

#include <cstdint>

#if defined(__APPLE__)
#  include <dlfcn.h>
#endif

export module bioseq_instr.pmu.macos;

export namespace bioseq::instr::pmu::macos_backend {

#if defined(__APPLE__)

/// Per-thread kperf state. Holds the dlopen'd framework handles and the
/// kernel-counter mapping. Initialised once per process; the per-thread
/// `read_counters` call only takes the kpc_get_thread_counters syscall.
struct PerThreadState {
    void*         kperf_handle      = nullptr;   // dlopen kperf.framework
    void*         kperfdata_handle  = nullptr;   // dlopen kperfdata.framework
    bool          configured        = false;
};

/// Reserved for future macOS activation. Remains a stub until the
/// dlopen + kpep_db_create + kpc_set_config sequence is wired in.
[[nodiscard]] inline bool init(PerThreadState&) noexcept { return false; }
inline void shutdown(PerThreadState&) noexcept {}

#else

struct PerThreadState {};
[[nodiscard]] inline bool init(PerThreadState&) noexcept { return false; }
inline void shutdown(PerThreadState&) noexcept {}

#endif

}  // namespace bioseq::instr::pmu::macos_backend
