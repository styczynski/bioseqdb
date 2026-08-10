/**
 * @file instrumentation/pmu/unsupported.cppm
 * @brief Explicit behavior when thread PMU counters are unavailable.
 *
 * Used on Windows and any platform without a working PMU backend.
 * Returns the four-event canon as zeros. The user-facing module
 * The initialization and shutdown results retain the established portable
 * fallback behavior, and reads always return a zero sample.
 */
module;

#include <cstdint>

export module bioseq.instrumentation.pmu.unsupported;

export namespace bioseq::instrumentation::pmu::unsupported {

struct counter_sample {
    std::uint64_t cycles        = 0;
    std::uint64_t instructions  = 0;
    std::uint64_t branches      = 0;
    std::uint64_t branch_misses = 0;
};
static_assert(sizeof(counter_sample) == 32);

[[nodiscard]] inline bool initialize() noexcept { return true; }
inline void shutdown() noexcept {}
[[nodiscard]] inline counter_sample read_counters() noexcept { return counter_sample{}; }
[[nodiscard]] inline bool are_thread_counters_available() noexcept { return false; }

}  // namespace bioseq::instrumentation::pmu::unsupported
