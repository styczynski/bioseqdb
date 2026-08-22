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

/**
 * @brief Report that the current runtime has no sampling-service backend.
 *
 * @param[in] configuration Requested frequency and ring capacity.
 * @return Always `false` because sampling is currently unavailable.
 */
[[nodiscard]] inline bool start_sampling(const sampler_configuration& configuration) noexcept {
    (void)configuration;
    return false;
}

/** @brief Perform the idempotent no-op sampling shutdown. */
inline void stop_sampling() noexcept {}

/** @brief Report that the sampling service is not running. */
[[nodiscard]] inline bool is_sampling() noexcept { return false; }

}  // namespace bioseq::instrumentation::sampler
