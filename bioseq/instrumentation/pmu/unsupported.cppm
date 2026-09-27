/**
 * @file instrumentation/pmu/unsupported.cppm
 * @brief Fallback for platforms without hardware-counter support.
 *
 * Initialization succeeds without setting anything up Availability  is false,
 * and every read returns zero counts.
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

/** @brief Return success, without initializing counters. */
[[nodiscard]] inline bool initialize() noexcept { return true; }

/** @brief Do nothing because this backend owns no counter resources. */
inline void shutdown() noexcept {}

/** @brief Return a zero-valued counter sample. */
[[nodiscard]] inline counter_sample read_counters() noexcept { return counter_sample{}; }

/** @brief Report that hardware counters are unavailable. */
[[nodiscard]] inline bool are_thread_counters_available() noexcept { return false; }

}  // namespace bioseq::instrumentation::pmu::unsupported
