/**
 * @file run_config.cppm
 * @brief Process-wide generic CLI knobs for any bench harness.
 *
 * Holds the subset of run-time configuration that is meaningful for
 * every bench, regardless of domain:
 *
 *   * `threads` — explicit override; 0 means "let the scheduler pick";
 *   * `fast_mode` — generic "smoke-signal-not-regression-grade" flag;
 *   * `per_call_budget_s` — generic per-task wall-clock deadline (the
 *     scheduler does not enforce it; the aligner's executor consults
 *     this value before deciding to abort a stuck task).
 *
 * Domain-specific knobs (sample profile per length bucket, k/w sweep
 * gating, file-format selection on an IO bench, ...) live in the
 * harness's own run-config module — see
 * `bench.aligners.run_config::AlignerRunConfig` for the aligner side.
 *
 * Threading: `core_run_config()` is initialised once in the bench
 * harness's `main()` before the worker pool starts; from that point
 * on every reader sees a settled value. Read-only across threads;
 * we deliberately do NOT take a mutex on the read path. A late
 * mutation would race with workers, so the only supported writer is
 * `main()` and the only supported time is before `session.run()`.
 */
module;

#include <algorithm>
#include <cstddef>
#include <thread>

export module bench.scheduler.run_config;

import bench.scheduler.options;

export {

namespace bioseq::bench_scheduler {

/**
 * @brief Generic CLI-driven knobs every bench harness needs.
 *
 * Compose this with a harness-specific config (see
 * `bench.aligners.run_config::AlignerRunConfig`) when more knobs are
 * needed. Composition over inheritance because the generic struct is
 * not a polymorphic type — the scheduler only ever sees the generic
 * fields.
 */
struct CoreRunConfig {
    /// Worker-thread count. 0 means "pick from policy + hardware":
    /// `hardware_concurrency() / 2` by default, full
    /// `hardware_concurrency()` when `fast_mode == true`. Caller can
    /// override with any positive integer.
    std::size_t threads = 0;

    /// "Smoke-signal-not-regression-grade" flag. When true, harness
    /// is expected to reduce per-bucket sample counts (the noise
    /// floor goes from ~±1% to ~±3%) AND saturate the worker pool to
    /// `hardware_concurrency()` so the run finishes faster. The
    /// scheduler itself only consumes the saturate-threads bit; the
    /// sample-count drop is a domain decision.
    bool fast_mode = false;

    /// Per-task wall-clock budget, in seconds. 0 disables. The
    /// scheduler does not enforce this; the executor lambda is
    /// responsible for noticing when a single task has run for too
    /// long and aborting cooperatively. Centralising the value here
    /// means the aligner doesn't grow a separate "budget" knob.
    double per_call_budget_s = 0.0;
};

/**
 * @brief Process-wide accessor. Initialised on first call to defaults.
 *
 * Function-local static; matches the bench's "one logical run per
 * process" lifecycle without ceremony. Identical pattern to
 * `progress::dashboard()`.
 */
[[nodiscard]] inline CoreRunConfig& core_run_config() {
    static CoreRunConfig cfg{};
    return cfg;
}

/**
 * @brief Resolve the worker-thread count from a `CoreRunConfig`.
 *
 *   * `threads != 0`: honour the override exactly.
 *   * `fast_mode == true`: full `hardware_concurrency()` — saturate
 *     the box; user has opted into noisier numbers.
 *   * otherwise: `hardware_concurrency() / 2` — leaves headroom for
 *     OS work and avoids hyperthread-induced cache contention.
 *
 * Always at least 1 (`hardware_concurrency()` returns 0 on hosts
 * that can't query the topology; the bench is the wrong place to
 * no-op there).
 *
 * The companion `bench.scheduler.options::resolved_thread_count`
 * does NOT do hardware fall-back — it just clamps a pre-resolved
 * `SchedulerOptions::threads` to "at least 1". `CoreRunConfig` is
 * the **human-facing** config that knows about hardware; the
 * `SchedulerOptions` consumed by `run_parallel_indexed` is the bare
 * "I have already decided how many workers I want" struct. Use this
 * function to translate from one to the other.
 */
[[nodiscard]] inline std::size_t resolve_thread_count(const CoreRunConfig& cfg) noexcept {
    if (cfg.threads != 0) return cfg.threads;
    const auto hw = std::thread::hardware_concurrency();
    if (hw == 0) return 1;
    if (cfg.fast_mode) return hw;
    return std::max<std::size_t>(1u, hw / 2u);
}

/**
 * @brief Convenience: fold a `CoreRunConfig` into a
 *        `SchedulerOptions` for `run_parallel_indexed`.
 *
 * This is the **only** correct way to populate `SchedulerOptions`
 * from a human-edited config — it pre-resolves the thread count so
 * the runner never has to do hardware queries on the hot path.
 */
[[nodiscard]] inline SchedulerOptions to_scheduler_options(const CoreRunConfig& cfg) noexcept {
    SchedulerOptions out;
    out.threads = resolve_thread_count(cfg);
    return out;
}

}  // namespace bioseq::bench_scheduler

}  // export
