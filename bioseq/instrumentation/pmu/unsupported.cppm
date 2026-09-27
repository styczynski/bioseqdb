/**
 * @file instrumentation/pmu/unsupported.cppm
 * @brief Explicit behavior when thread PMU counters are unavailable.
 *
 * Initialization remains a no-op, availability stays false, and reads return
 * the four-event zero sample.
 */
module;

#include <cstdint>

export module bioseq.instrumentation.pmu.unsupported;

export namespace bioseq::instrumentation::pmu::unsupported {

/** @brief Four-counter zero sample used by the unsupported backend. */
struct counter_sample {
    std::uint64_t cycles        = 0;
    std::uint64_t instructions  = 0;
    std::uint64_t branches      = 0;
    std::uint64_t branch_misses = 0;
};
static_assert(sizeof(counter_sample) == 32);

/** @brief Initialize the no-op backend. */
[[nodiscard]] inline bool initialize() noexcept { return true; }

/** @brief Shut down the no-op backend. */
inline void shutdown() noexcept {}

/** @brief Return a zero-valued counter sample. */
[[nodiscard]] inline counter_sample read_counters() noexcept { return counter_sample{}; }

/** @brief Report that hardware counters are unavailable. */
[[nodiscard]] inline bool are_thread_counters_available() noexcept { return false; }

}  // namespace bioseq::instrumentation::pmu::unsupported
