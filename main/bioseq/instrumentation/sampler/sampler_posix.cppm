/**
 * @file instrumentation/sampler/sampler_posix.cppm
 * @brief POSIX backend sketch — timer_create + SIGPROF (Linux),
 *        dispatch_source_t (macOS).
 *
 * v1 status: not yet activated. The sampler interface
 * (bioseq_instr.sampler) returns false from start(); this module
 * preserves the symbol surface for the future signal-driven
 * implementation.
 */
module;

#include <cstdint>

#if defined(__linux__) || defined(__APPLE__)
#  include <signal.h>
#endif

export module bioseq.instr.sampler.posix;

export namespace bioseq::instr::sampler::posix_backend {

#if defined(__linux__) || defined(__APPLE__)

[[nodiscard]] inline bool start_for_thread(std::uint32_t /*hz*/) noexcept {
    return false;
}
inline void stop_for_thread() noexcept {}

#else

[[nodiscard]] inline bool start_for_thread(std::uint32_t) noexcept { return false; }
inline void stop_for_thread() noexcept {}

#endif

}  // namespace bioseq::instr::sampler::posix_backend
