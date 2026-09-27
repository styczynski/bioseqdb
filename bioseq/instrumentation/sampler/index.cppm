/**
 * @file instrumentation/sampler/index.cppm
 * @brief Sampling-service interface with an unavailable implementation.
 *
 * Starting returns false. Stopping does nothing, and is_sampling always returns false.
 */
module;

#include <cstdint>

export module bioseq.instrumentation.sampler;

export namespace bioseq::instrumentation::sampler {

/** @brief Requested sampling frequency and ring size. */
struct sampler_configuration {
    std::uint32_t frequency_hz = 50;
    std::uint32_t ring_size = 4096;
};
static_assert(sizeof(sampler_configuration) == 8);

/**
 * @brief Report that sampling is unavailable.
 * @param[in] configuration Requested frequency and ring size, currently ignored.
 * @return Always false.
 */
[[nodiscard]] inline bool start_sampling(const sampler_configuration& configuration) noexcept {
    (void)configuration;
    return false;
}

/** @brief Do nothing because no sampler is running. */
inline void stop_sampling() noexcept {}

/** @brief Report that the sampling service is not running. */
[[nodiscard]] inline bool is_sampling() noexcept { return false; }

}  // namespace bioseq::instrumentation::sampler
