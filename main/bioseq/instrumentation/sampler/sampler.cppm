/**
 * @file instrumentation/sampler/sampler.cppm
 * @brief Statistical sampler — portable interface.
 *
 * Sampling fires SIGPROF (POSIX) or a queued APC (Windows) on a timer,
 * captures PC + cycle counter + active region, and pushes a Sample into
 * a per-thread ring. v1 status: surface only — disabled by default
 * (Args::enable_sampling = false). The hot-path region/loop scopes do
 * not interact with the sampler unless a future args struct opts in.
 */
module;

#include <cstdint>

export module bioseq.instr.sampler;

export namespace bioseq::instr::sampler {

struct config_t {
    std::uint32_t hz        = 50;     // samples per second per thread
    std::uint32_t ring_size = 4096;   // events per per-thread ring
};

/// Start the sampler with the given config. Returns false if not
/// supported on this platform or if init failed.
[[nodiscard]] bool start(const config_t& cfg) noexcept;

/// Stop the sampler. Idempotent.
void stop() noexcept;

/// True if the sampler is currently running.
[[nodiscard]] bool is_running() noexcept;

}  // namespace bioseq::instr::sampler

namespace bioseq::instr::sampler {

[[nodiscard]] inline bool start(const config_t&) noexcept { return false; }
inline void stop() noexcept {}
[[nodiscard]] inline bool is_running() noexcept { return false; }

}  // namespace bioseq::instr::sampler
