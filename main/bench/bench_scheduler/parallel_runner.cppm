/**
 * @file parallel_runner.cppm
 * @brief Index-based parallel case runner for the generic bench scheduler.
 *
 * The runner is the only piece of glue between the bench's worker pool
 * and the live progress dashboard. Callers describe their workload by
 * **count** (how many cases) and **closure** (a callable that knows how
 * to materialise case `i` into a result), and the runner takes care of:
 *
 *   * spawning `resolved_thread_count(opts)` worker threads,
 *   * an atomic ticket-server (`std::atomic<size_t>`) so workers pull
 *     unique case indices without any per-task allocation,
 *   * timing each executor invocation and forwarding it into the
 *     dashboard's rolling-throughput / ETA window,
 *   * surfacing the per-worker label the executor sets via
 *     `TaskContext::set_label`,
 *   * catching exceptions so one bad case drops a single result and
 *     does not abort the whole run.
 *
 * It deliberately does NOT know about:
 *
 *   * **Case shape** — the executor closes over its own data; the
 *     runner sees only an index. This was the result of a reviewer
 *     pass: an earlier draft templated on `CaseT` and indexed into a
 *     vector of cases, but every existing caller already builds case
 *     data lazily from outer arrays (`tasks[i] = {cfg_idx, pair_idx}`),
 *     so the indirection bought nothing.
 *   * **Wall-clock budget** — the aligner already tracks per-config
 *     deadlines in its own closure; baking budget enforcement into the
 *     runner would force an "abort policy" abstraction that's only
 *     used by one caller.
 *   * **Case grouping / abort coordination** — same reason. The
 *     aligner's `aborted_per_config` array lives in its closure as a
 *     `std::vector<std::atomic<std::uint8_t>>`; the runner is unaware.
 *
 * Threading: the `TaskContext` instance is per-task — a fresh
 * stack-allocated object for each executor invocation, owned by the
 * worker. No shared mutable state crosses tasks except the dashboard
 * (already thread-safe) and the result vector (each slot written by
 * exactly one task).
 */
module;

#include "common.h"

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

export module bench.scheduler.parallel_runner;

import mlogging;
import bench.scheduler.options;
import bench.scheduler.progress_ui;

export {

namespace bioseq::bench_scheduler {

/**
 * @brief Per-task context handed to the executor.
 *
 * Lives only for the duration of one `executor(case_idx, ctx)` call —
 * stack-allocated by the worker thread, destroyed on return. Holds a
 * dashboard pointer + bucket / worker IDs so the executor can update
 * its label without having to thread the dashboard through its own
 * call chain.
 *
 * Not thread-safe; no two workers share a TaskContext.
 */
class TaskContext {
public:
    TaskContext(progress::Dashboard& dashboard,
                std::size_t bucket_id,
                std::size_t worker_id) noexcept
        : dashboard_{&dashboard}
        , bucket_id_{bucket_id}
        , worker_id_{worker_id}
    {}

    TaskContext(const TaskContext&) = delete;
    TaskContext& operator=(const TaskContext&) = delete;

    /// Update the per-worker label rendered by the dashboard. Called at
    /// most once per task in practice (the aligner builds one label
    /// before launching its sample loop) — not in any hot inner loop,
    /// so the move-into-string cost is negligible.
    void set_label(std::string label) {
        dashboard_->record_task_started(bucket_id_, worker_id_, std::move(label));
        label_set_ = true;
    }

    /// Mark this task as failed. Surfaces in the dashboard's
    /// per-bucket `failed=` counter and is reflected in the
    /// `record_task_finished(..., task_failed=true)` call the runner
    /// emits after the executor returns.
    void mark_failed() noexcept { failed_ = true; }

    /// Mark this task as skipped — the executor decided not to do the
    /// work (e.g. caller's per-group abort flag tripped). The runner
    /// emits `record_task_skipped` instead of `record_task_finished`,
    /// so the dashboard's per-bucket `budget_skipped=` counter is
    /// honest. The result slot is still populated by the executor's
    /// return value; callers typically return a metadata-only
    /// `ResultT` so the chart layer can show "this cell was skipped"
    /// without a totally blank entry.
    void mark_skipped() noexcept { skipped_ = true; }

    /// Push a one-shot status string into the dashboard's notices band.
    /// Used by callers to announce skip / abort decisions ("ABORT
    /// bioseq-RMQ on long/ont_legacy after 612 s").
    void post_notice(std::string message) {
        dashboard_->post_notice(std::move(message));
    }

    /// Record that this task collected `n` timed samples. The runner
    /// forwards the total to `dashboard.record_task_finished` so the
    /// per-bucket "samples=" gauge in the FTXUI dashboard is honest.
    /// Multiple calls accumulate (executor can record samples in
    /// batches if it loops over phases). Default 0 is correct for
    /// callers that don't have a "sample count" concept.
    void add_samples_collected(std::size_t n) noexcept {
        samples_collected_ += n;
    }

    /// Internal accessors used by `run_parallel_indexed`.
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] bool skipped() const noexcept { return skipped_; }
    [[nodiscard]] bool label_was_set() const noexcept { return label_set_; }
    [[nodiscard]] std::size_t samples_collected() const noexcept { return samples_collected_; }
    [[nodiscard]] progress::Dashboard& dashboard() noexcept { return *dashboard_; }
    [[nodiscard]] std::size_t bucket_id() const noexcept { return bucket_id_; }
    [[nodiscard]] std::size_t worker_id() const noexcept { return worker_id_; }

private:
    progress::Dashboard* dashboard_;
    std::size_t bucket_id_;
    std::size_t worker_id_;
    bool failed_ = false;
    bool skipped_ = false;
    bool label_set_ = false;
    std::size_t samples_collected_ = 0;
};

namespace detail {

/// Worker body. Pulls case indices from a shared atomic counter, runs
/// the executor for each, times the call, and stamps the result into
/// the per-task slot. We split this out of the public template so the
/// non-template body lives in one place — the template instantiation
/// is just the executor type.
template<typename ResultT, typename ExecFn>
inline void run_worker(
    std::size_t worker_id,
    std::size_t case_count,
    std::atomic<std::size_t>& next_case,
    std::vector<ResultT>& results,
    std::vector<std::uint8_t>& slot_failed,
    progress::Dashboard& dashboard,
    std::size_t bucket_id,
    ExecFn& executor)
{
    using clock = std::chrono::steady_clock;
    while (true) {
        const std::size_t case_idx = next_case.fetch_add(1, std::memory_order_relaxed);
        if (case_idx >= case_count) return;

        TaskContext ctx{dashboard, bucket_id, worker_id};
        const auto t0 = clock::now();
        try {
            results[case_idx] = executor(case_idx, ctx);
        } catch (const std::exception& ex) {
            JustLog("[bench.scheduler.parallel_runner] EXCEPTION on case {}: {}",
                    case_idx, ex.what());
            ctx.mark_failed();
        } catch (...) {
            JustLog("[bench.scheduler.parallel_runner] UNKNOWN EXCEPTION on case {}",
                    case_idx);
            ctx.mark_failed();
        }
        const auto t1 = clock::now();
        const double seconds = std::chrono::duration<double>(t1 - t0).count();

        const bool failed = ctx.failed();
        const bool skipped = ctx.skipped();
        slot_failed[case_idx] = failed ? std::uint8_t{1} : std::uint8_t{0};

        if (skipped) {
            // The executor opted out (e.g. per-group abort flag was
            // tripped before this task started). Bump
            // `record_task_skipped` so the dashboard's bucket-level
            // `budget_skipped=` counter is honest, and never call
            // `record_task_started` / `record_task_finished` for this
            // slot — the gauge denominator advances via
            // `record_task_skipped` itself.
            dashboard.record_task_skipped(bucket_id);
        } else {
            // If the executor never set a label, push a placeholder so
            // the dashboard's `record_task_finished` matches a started
            // slot — the dashboard's `in_flight_tasks` counter is
            // paired (started/finished) and asymmetry would leave it
            // stuck at non-zero forever.
            if (!ctx.label_was_set()) {
                dashboard.record_task_started(bucket_id, worker_id, std::string{"(unlabeled)"});
            }
            // The executor reports its sample count via
            // `TaskContext::add_samples_collected` so the dashboard's
            // bucket gauge displays "samples=K" correctly. Callers that
            // don't have a sample concept simply don't call it; the
            // field stays 0.
            dashboard.record_task_finished(bucket_id, worker_id, seconds,
                                           ctx.samples_collected(), failed);
        }

        // The exception path doesn't read `results[case_idx]` — left
        // at its default-constructed value. `slot_failed[case_idx]`
        // is the canonical signal callers consult to drop the slot.
    }
}

}  // namespace detail

/**
 * @brief Run `case_count` independent cases in parallel.
 *
 * Registers the bucket with `dashboard`, dispatches `[0, case_count)`
 * to a pool of `resolved_thread_count(opts)` workers, and returns a
 * vector of `ResultT` indexed by case index.
 *
 * @tparam ResultT  Default-constructible. Each case's result lands at
 *                  index `case_idx` in the returned vector. Cases that
 *                  threw remain default-constructed; the parallel
 *                  `failed_indices` vector tells the caller which
 *                  slots are valid.
 * @tparam ExecFn   Callable with signature `ResultT(std::size_t, TaskContext&)`.
 *                  Closes over the caller's case data. Called from
 *                  multiple threads concurrently — must be thread-safe
 *                  with respect to anything it reads from the closure.
 *
 * @param case_count    Number of cases. The runner walks `[0, case_count)`.
 * @param bucket_label  Display label for the dashboard's bucket row.
 * @param opts          Thread-count knob. See SchedulerOptions docs.
 * @param dashboard     Process-wide dashboard. Pass
 *                      `bench_scheduler::progress::dashboard()`.
 * @param executor      Per-case work. Receives a TaskContext for label
 *                      / notice / failure-flagging.
 *
 * @return ParallelRunOutcome with the per-case results and a parallel
 *         vector of failure flags (1 = the executor threw or called
 *         `mark_failed()`).
 */
template<typename ResultT>
struct ParallelRunOutcome {
    std::vector<ResultT> results;
    /// Parallel to `results`. `1` when the executor threw or set
    /// `TaskContext::mark_failed`. The caller decides how to interpret
    /// failure (drop the slot, surface a partial record, ...).
    std::vector<std::uint8_t> failed;
    /// Bucket id assigned by the dashboard. Caller may use it to post
    /// extra notices / update the bucket total later if needed.
    std::size_t bucket_id = 0;
};

template<typename ResultT, typename ExecFn>
    requires std::is_default_constructible_v<ResultT>
          && std::invocable<ExecFn&, std::size_t, TaskContext&>
          && std::same_as<
                 std::invoke_result_t<ExecFn&, std::size_t, TaskContext&>,
                 ResultT>
[[nodiscard]] inline ParallelRunOutcome<ResultT> run_parallel_indexed(
    std::size_t case_count,
    std::string bucket_label,
    const SchedulerOptions& opts,
    progress::Dashboard& dashboard,
    ExecFn&& executor)
{
    ParallelRunOutcome<ResultT> outcome;
    outcome.results.resize(case_count);
    outcome.failed.assign(case_count, std::uint8_t{0});
    outcome.bucket_id = dashboard.register_bucket(std::move(bucket_label), case_count);
    if (case_count == 0) return outcome;

    const std::size_t n_threads = resolved_thread_count(opts);
    dashboard.register_workers(n_threads);

    std::atomic<std::size_t> next_case{0};

    if (n_threads <= 1) {
        detail::run_worker<ResultT, ExecFn>(
            /*worker_id=*/0, case_count, next_case,
            outcome.results, outcome.failed,
            dashboard, outcome.bucket_id, executor);
        return outcome;
    }

    std::vector<std::thread> workers;
    workers.reserve(n_threads);
    for (std::size_t worker_id = 0; worker_id < n_threads; ++worker_id) {
        workers.emplace_back([&, worker_id]() {
            detail::run_worker<ResultT, ExecFn>(
                worker_id, case_count, next_case,
                outcome.results, outcome.failed,
                dashboard, outcome.bucket_id, executor);
        });
    }
    for (auto& th : workers) th.join();
    return outcome;
}

}  // namespace bioseq::bench_scheduler

}  // export
