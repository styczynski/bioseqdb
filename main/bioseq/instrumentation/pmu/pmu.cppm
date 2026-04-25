/**
 * @file instrumentation/pmu/pmu.cppm
 * @brief Portable PMU group abstraction.
 *
 * The four-event canon group (cycles, instructions, branches,
 * branch_misses) is exposed across platforms. Each platform backend lives
 * in a sibling module guarded by platform macros; this header is the
 * uniform interface.
 *
 * v1 status: the public surface is in place; all backends ship the
 * stubbed implementation that returns zeros. Linux perf_event and macOS
 * kperf backends are sketched in pmu_linux.cppm / pmu_macos.cppm — they
 * are activated by replacing the stub. The hot path (region/loop scopes)
 * does not currently invoke read_thread_counters so there is no runtime
 * cost yet; wiring is deferred to a follow-up that opts in via
 * Args::enable_counters and lifts the field reads in begin/end.
 */
module;

#include <cstdint>

export module bioseq_instr.pmu;

export namespace bioseq::instr::pmu {

/// Canonical 4-event sample. Always 32 bytes regardless of platform.
struct counters_t {
    std::uint64_t cycles        = 0;
    std::uint64_t instructions  = 0;
    std::uint64_t branches      = 0;
    std::uint64_t branch_misses = 0;
};

/// Initialise per-thread counter group. Called once per thread on first
/// region begin in a profiling instantiation. Returns false when no PMU
/// access is available on this platform.
bool init_thread_counters() noexcept;

/// Tear down per-thread counter group. Called from ThreadState's
/// destructor; idempotent if init_thread_counters returned false.
void shutdown_thread_counters() noexcept;

/// Read the current counter values. Returns zeros if the per-thread
/// group is not initialised.
counters_t read_thread_counters() noexcept;

/// True if the platform backend is functional on this host. False on
/// Windows or when PMU access was denied at init time.
[[nodiscard]] bool counters_available() noexcept;

}  // namespace bioseq::instr::pmu

// =====================================================================
// Inline default (stub) implementation. Linux / macOS / Windows specific
// implementations are sketched in adjacent modules and activated when
// the project chooses to wire them up. Until then this stub returns
// zeros — the aggregate fields stay zero and the summary writer prints
// `cycles=0 instructions=0 ...` which is the documented v1 behaviour.
// =====================================================================

namespace bioseq::instr::pmu {

[[nodiscard]] inline bool counters_available() noexcept { return false; }

inline bool init_thread_counters() noexcept { return false; }
inline void shutdown_thread_counters() noexcept {}
inline counters_t read_thread_counters() noexcept { return counters_t{}; }

}  // namespace bioseq::instr::pmu
