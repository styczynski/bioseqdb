/**
 * @file instrumentation/global_state.cppm
 * @brief Process-wide profiler state — minimal stub.
 *
 * The bioseqdb design installs the profiling context per scan via
 * bioseq::context::installed_context_scope and tears it down at end of scan.
 * The "thread registry" pattern (Caliper's GlobalState::threads) is therefore
 * unnecessary in v1: there is exactly one active context at a time per
 * Postgres backend, and the summary writer pulls aggregates directly from
 * the currently-installed context.
 *
 * This module exists for two reasons:
 *   1. To provide a global enable flag (`runtime_enabled`) used by
 *      bioseq::instr::set_runtime_enabled(false) for live pause without
 *      recompilation.
 *   2. To provide a hook point for v2 features (PMU init, sampler thread)
 *      that need process-wide state.
 */
module;

#include <atomic>

export module bioseq_instr.global_state;

export namespace bioseq::instr {

/// Process-wide runtime enable flag. False prevents region/loop scopes
/// from doing any work even inside a profiling instantiation.
inline std::atomic<bool> g_runtime_enabled{true};

[[gnu::always_inline]]
inline bool runtime_enabled() noexcept {
    return g_runtime_enabled.load(std::memory_order_relaxed);
}

inline void set_runtime_enabled(bool on) noexcept {
    g_runtime_enabled.store(on, std::memory_order_relaxed);
}

}  // namespace bioseq::instr
