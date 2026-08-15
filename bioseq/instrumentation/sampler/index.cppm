/**
 * @file instrumentation/sampler/index.cppm
 * @brief Portable sampling service boundary.
 *
 * Sampling is explicitly unavailable in the current runtime. Start requests
 * return false, stop is idempotent, and the service never reports running.
 */
module;

#include <cstdint>

export module bioseq.instrumentation.sampler;

export namespace bioseq::instrumentation::sampler {

/** @brief Requested sampling frequency and bounded ring capacity. */
struct sampler_configuration {
    std::uint32_t frequency_hz = 50;
    std::uint32_t ring_size = 4096;
};
static_assert(sizeof(sampler_configuration) == 8);

/// Start the sampler with the given config. Returns false if not
/// supported on this platform or if init failed.
[[nodiscard]] inline bool start_sampling(const sampler_configuration& configuration) noexcept;

/// Stop the sampler. Idempotent.
inline void stop_sampling() noexcept;

/// True if the sampler is currently running.
[[nodiscard]] inline bool is_sampling() noexcept;

}  // namespace bioseq::instrumentation::sampler

namespace bioseq::instrumentation::sampler {

[[nodiscard]] inline bool start_sampling(const sampler_configuration&) noexcept { return false; }
inline void stop_sampling() noexcept {}
[[nodiscard]] inline bool is_sampling() noexcept { return false; }

}  // namespace bioseq::instrumentation::sampler
