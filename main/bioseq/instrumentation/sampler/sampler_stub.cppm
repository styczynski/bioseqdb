/**
 * @file instrumentation/sampler/sampler_stub.cppm
 * @brief No-op sampler — used when SIGPROF is unavailable or sampling is
 *        deliberately disabled.
 */
module;

#include <cstdint>

export module bioseq.instr.sampler.stub;

export namespace bioseq::instr::sampler::stub_backend {

[[nodiscard]] inline bool start_for_thread(std::uint32_t) noexcept { return false; }
inline void stop_for_thread() noexcept {}

}  // namespace bioseq::instr::sampler::stub_backend
