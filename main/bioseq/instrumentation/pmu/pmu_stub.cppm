/**
 * @file instrumentation/pmu/pmu_stub.cppm
 * @brief Zero-overhead PMU stub.
 *
 * Used on Windows and any platform without a working PMU backend.
 * Returns the four-event canon as zeros. The user-facing module
 * `bioseq_instr.pmu` already provides the same fallback inline; this
 * file exists so the platform-conditional aggregator in mod.cppm has a
 * symmetric shape (one stub per backend).
 */
module;

#include <cstdint>

export module bioseq_instr.pmu.stub;

export namespace bioseq::instr::pmu::stub {

struct counters_t {
    std::uint64_t cycles        = 0;
    std::uint64_t instructions  = 0;
    std::uint64_t branches      = 0;
    std::uint64_t branch_misses = 0;
};

[[nodiscard]] inline bool init() noexcept { return true; }
[[nodiscard]] inline bool init_thread() noexcept { return false; }
inline void shutdown() noexcept {}
inline void shutdown_thread() noexcept {}
[[nodiscard]] inline counters_t read() noexcept { return counters_t{}; }
[[nodiscard]] inline bool counters_available() noexcept { return false; }

}  // namespace bioseq::instr::pmu::stub
