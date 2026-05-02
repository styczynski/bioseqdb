/**
 * @file options.cppm
 * @brief Process-wide configuration for the generic benchmark scheduler.
 *
 * The scheduler is the domain-agnostic half of the bench harness. It owns
 * worker-pool dispatch, progress UI hookups, JSON I/O, and the regression
 * rule engine. Domain knobs (sample profile, k/w sweep gating, length
 * buckets) live in `bench.aligners.run_config` — those are aligner
 * concepts and the scheduler must stay unaware of them.
 *
 * The only field the scheduler genuinely needs is `threads`. Other
 * possible knobs (per-task budget, retry policy) currently live in the
 * caller's executor closure: the aligner already tracks per-config
 * abort flags and per-task wall-clock there, and the scheduler doesn't
 * gain anything by duplicating them.
 */
module;

#include <algorithm>
#include <cstddef>
#include <thread>

export module bench.scheduler.options;

export {

namespace bioseq::bench_scheduler {

/**
 * @brief Knobs the parallel runner reads. Construct fresh per
 *        `run_parallel_indexed` call — the struct is cheap to copy.
 */
struct SchedulerOptions {
    /// Worker-thread count. 0 means "auto = max(1, hardware_concurrency() / 2)".
    /// The aligner CLI resolves `--fast` to `hardware_concurrency()` before
    /// constructing this struct; the scheduler does not know about
    /// "fast mode", it just runs `threads` workers.
    std::size_t threads = 0;
};

/**
 * @brief Clamp the worker count for `SchedulerOptions` to "at least 1".
 *
 * Callers of `run_parallel_indexed` are expected to populate
 * `SchedulerOptions::threads` with the *already resolved* count
 * (typically via `bench_scheduler::to_scheduler_options(CoreRunConfig)`).
 * This helper exists so the parallel runner can defend against a
 * zero `threads` field (which would deadlock the worker pool) without
 * silently falling back to a hardware-derived default — that
 * fallback is the caller's concern, not the runner's.
 */
[[nodiscard]] inline std::size_t resolved_thread_count(const SchedulerOptions& opts) noexcept {
    return opts.threads == 0 ? std::size_t{1} : opts.threads;
}

}  // namespace bioseq::bench_scheduler

}  // export
