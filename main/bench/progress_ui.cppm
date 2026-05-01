/**
 * @file progress_ui.cppm
 * @brief FTXUI-based live progress dashboard for the bench matrix runner.
 *
 * Why this module exists
 * ----------------------
 * The bench runs for 15-20 minutes end-to-end and dispatches thousands of
 * (algorithm, length-bucket, error-bucket, pair) tasks across a worker
 * pool. Without surface area for "what is happening right now", a stuck
 * task is indistinguishable from a healthy long-running one — the user
 * already reported a 40 minute hang where there was no signal at all.
 *
 * The dashboard answers four questions the bench operator keeps asking
 * the terminal:
 *
 *   1. *How far along are we?* — per-bucket gauges (done / total) and
 *      a global gauge across all registered buckets so the operator sees
 *      both the local bucket horizon and the run-wide horizon.
 *   2. *How fast is it going?* — rolling throughput (tasks/min) and the
 *      mean wall-clock per task across the most-recently-completed
 *      window. Slowdowns show up as the throughput number dropping.
 *   3. *When will it finish?* — ETA computed as
 *      `remaining_tasks * mean_seconds_per_task` over the rolling window.
 *      We deliberately use a recent window rather than the whole-run
 *      mean so the ETA reacts within seconds when the run hits a heavy
 *      bucket (XXL pairs are minutes-per-task, SHORT is milliseconds —
 *      a global mean is dominated by SHORT and would lie about XXL).
 *   4. *What is each worker doing right now?* — every worker thread
 *      registers a short label (`{algorithm} on {length}/{error} pair#N`)
 *      while it runs a task, so the operator can see whether the bench
 *      is wedged on a single config or making fan-wise progress.
 *
 * Architecture
 * ------------
 *
 *   - Workers call `record_task_started`, `record_task_finished`,
 *     `register_bucket` from inside `matrix_runner.cppm`. These are
 *     small atomic-and-mutex operations — they do not allocate inside
 *     the hot path beyond the one-time worker label string.
 *   - One render thread, started by `start()` and joined by `stop()`,
 *     wakes every `kRenderIntervalMs` (default 250 ms), takes a snapshot
 *     of the dashboard state under a single mutex, builds an FTXUI
 *     document, renders it to a fixed-width `Screen`, and prints it to
 *     stderr. The previous frame is wiped via `Screen::ResetPosition()`
 *     so the dashboard updates in-place without scrolling.
 *
 * TTY detection
 * -------------
 * If stderr is not a TTY (CI, output redirection), the in-place
 * `ResetPosition()` ANSI sequence would garble the redirected log.
 * In that case we fall back to plain text — one progress line per bucket
 * every `kFallbackLineIntervalMs` (default 5 s). The operator gets
 * coarse-grained progress in their log without escape codes.
 *
 * Why dom-only rather than full FTXUI Component
 * ---------------------------------------------
 * The bench is a non-interactive long-running batch. We do not need
 * keyboard input, focus management, or resizing event handling. The
 * dom-only `Render(screen, element); screen.Print();` loop covers
 * exactly what we need at a fraction of the surface area, with no
 * background event-loop thread fighting the render thread for the
 * mutex.
 */
module;

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

export module bench.progress_ui;

export {

namespace bioseq::bench::progress {

/// Render every 250 ms — fast enough to feel "live" on short tasks, slow
/// enough that the FTXUI dom build (a few hundred small allocations per
/// frame) costs ~0.1% of one core. The render thread is the only writer
/// to stderr while it runs, so a stale 250 ms snapshot never reorders
/// against the bench output.
inline constexpr int kRenderIntervalMs = 250;

/// Plain-text fallback line cadence when stderr is not a TTY. 5 s keeps
/// the redirected log readable while still surfacing a stuck run within
/// ~tens of seconds.
inline constexpr int kFallbackLineIntervalMs = 5'000;

/// Number of recently-finished tasks to remember for the rolling
/// throughput window. 64 is large enough to soak per-task variance
/// without being so deep that the window stale-dates against bucket
/// transitions (a SHORT-bucket throughput would survive into LONG by
/// minutes if we kept thousands of samples).
inline constexpr std::size_t kRollingWindowCapacity = 64;

/**
 * @brief Per-bucket cumulative state.
 *
 * One instance is registered per `(length_bucket, error_bucket)` pair
 * (and one for the k/w sweep when `--everything` is set). Counters are
 * monotonic across the bucket's lifetime; throughput is derived in the
 * render thread from the per-bucket completion timestamps stored in the
 * dashboard's rolling window.
 */
struct BucketSnapshot {
    std::size_t bucket_id = 0;
    std::string label;
    std::size_t total_tasks = 0;
    std::size_t finished_tasks = 0;
    std::size_t in_flight_tasks = 0;
    std::size_t skipped_tasks = 0;
    std::size_t failed_tasks = 0;
    std::uint64_t samples_collected = 0;
    /// Sum of completed task wall-clocks for this bucket. Lets the
    /// render thread compute a per-bucket mean even after the rolling
    /// window has rotated past this bucket's completions.
    double sum_seconds = 0.0;
    /// Wall-clock time of the first `record_task_started` in this
    /// bucket. Used to compute "elapsed in this bucket" in the
    /// dashboard.
    std::chrono::steady_clock::time_point first_started{};
    bool started = false;
    /// Wall-clock time of the last `record_task_finished` in this
    /// bucket — even if the bucket later receives more starts, the
    /// header shows a coherent "ended N seconds ago" once a bucket has
    /// no remaining work.
    std::chrono::steady_clock::time_point last_finished{};
    bool any_finished = false;
};

/**
 * @brief Active-task label slot, one per worker thread.
 *
 * Workers identify themselves by a small zero-based id that is allocated
 * by `register_workers(N)` at the start of each `run_matrix_for_bucket`
 * call. The slot is updated under the dashboard mutex so the render
 * thread always sees a coherent label for "what worker K is running
 * right now". An empty `label` means the slot is idle (between tasks
 * or after the bench has finished).
 */
struct WorkerSlotSnapshot {
    std::size_t worker_id = 0;
    std::string label;
    std::chrono::steady_clock::time_point started_at{};
    bool active = false;
};

namespace detail {

/// Internal mutable state — accessed under `mu_`.
struct DashboardState {
    std::vector<BucketSnapshot> buckets;
    std::vector<WorkerSlotSnapshot> worker_slots;
    /// Rolling deque of (finish_time, seconds) tuples — used to compute
    /// throughput over the last `kRollingWindowCapacity` completions.
    /// `seconds` is the wall-clock for that single task.
    std::deque<std::pair<std::chrono::steady_clock::time_point, double>> recent_completions;
    /// Total tasks finished across every bucket since `start()` — used
    /// for the global gauge and the run-wide throughput readout.
    std::size_t total_finished = 0;
    /// Sum of `bucket.total_tasks` across every bucket registered so
    /// far. Re-summed each frame so a late `register_bucket` (k/w sweep
    /// after the matrix run) updates the global gauge correctly.
    std::size_t total_registered = 0;
    /// Last warning / status line published by the runner — one-shot
    /// announcements like "skipping (LONG, ONT_LEGACY) — even minimap2
    /// fails this combo". Cleared after rendering once so transient
    /// notices don't accumulate.
    std::vector<std::string> pending_notices;
    std::chrono::steady_clock::time_point session_start{};
    bool session_started = false;
};

/// Format a duration as `HhMmSs` / `MmSs` / `SSs.MMMms` depending on
/// magnitude. Keeps the dashboard readable across the full range from
/// "still on the SHORT bucket" (sub-second) to "running for 25 minutes".
[[nodiscard]] inline std::string format_duration_seconds(double seconds) {
    if (!(seconds >= 0.0)) seconds = 0.0;
    if (seconds < 1.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0fms", seconds * 1000.0);
        return std::string{buf};
    }
    if (seconds < 60.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fs", seconds);
        return std::string{buf};
    }
    if (seconds < 3'600.0) {
        const int minutes = static_cast<int>(seconds) / 60;
        const int residual_seconds = static_cast<int>(seconds) % 60;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dm%02ds", minutes, residual_seconds);
        return std::string{buf};
    }
    const int hours = static_cast<int>(seconds) / 3'600;
    const int residual_minutes = (static_cast<int>(seconds) % 3'600) / 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%dh%02dm", hours, residual_minutes);
    return std::string{buf};
}

}  // namespace detail

/**
 * @brief Process-wide dashboard singleton.
 *
 * Every public function below is thread-safe — workers call into it
 * concurrently. Internally everything is serialised on `mu_` because
 * the throughput calculations need a coherent view across multiple
 * counters; per-counter atomics would race on the rolling deque.
 */
class Dashboard {
public:
    Dashboard() = default;
    Dashboard(const Dashboard&) = delete;
    Dashboard& operator=(const Dashboard&) = delete;

    /**
     * @brief Spin up the render thread and announce start of the run.
     *
     * Idempotent: a second call while the dashboard is already running
     * is a no-op (the bench harness has only one main entry, but
     * `bench-full-matrix` and the regression-repro test cases both
     * touch the dashboard, and double-init would otherwise spawn two
     * render threads fighting over stderr).
     */
    void start() {
        std::lock_guard<std::mutex> lock(mu_);
        if (running_) return;
        state_.session_start = std::chrono::steady_clock::now();
        state_.session_started = true;
        running_ = true;
        stop_requested_ = false;
        is_tty_ = ::isatty(STDERR_FILENO) != 0;
        // Back-date the fallback timer by one full interval so the
        // first redirected-stderr line emits as soon as the dashboard
        // has buckets to report — otherwise an operator running
        // `bench &> log` would see no progress for 5 s after launch.
        last_fallback_line_ = state_.session_start -
            std::chrono::milliseconds(kFallbackLineIntervalMs);
        previous_reset_position_.clear();
        render_thread_ = std::thread([this]() { render_loop(); });
    }

    /**
     * @brief Signal the render thread, render one final frame, join.
     *
     * The "render one final frame" matters: when the bench finishes
     * with the dashboard mid-frame, we want the operator to see the
     * end-state (every gauge at 100%, every active slot empty) rather
     * than the most recent in-flight snapshot.
     */
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!running_) return;
            stop_requested_ = true;
        }
        cv_.notify_all();
        if (render_thread_.joinable()) render_thread_.join();
        std::lock_guard<std::mutex> lock(mu_);
        running_ = false;
    }

    /**
     * @brief Pre-allocate one label slot per worker thread for the
     *        lifetime of one bucket.
     *
     * The matrix runner calls this once per `run_matrix_for_bucket`
     * with the worker count it is about to spawn. A shrink (next bucket
     * uses fewer threads) keeps the trailing slots inactive but
     * preserves their identity in the rendered table so the layout
     * doesn't jump between buckets.
     */
    void register_workers(std::size_t worker_count) {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_.worker_slots.size() < worker_count) {
            state_.worker_slots.resize(worker_count);
            for (std::size_t i = 0; i < worker_count; ++i) {
                state_.worker_slots[i].worker_id = i;
            }
        }
    }

    /**
     * @brief Add a bucket to the dashboard. Returns the assigned id.
     *
     * The id is the bucket's index in the registration order — workers
     * pass the same id to every later call. We avoid an external
     * map keyed by bucket label because two distinct buckets can
     * legitimately share the same display label (e.g. the matrix run
     * and the k/w sweep both touch MEDIUM/CCS).
     */
    [[nodiscard]] std::size_t register_bucket(std::string label, std::size_t total_tasks) {
        std::lock_guard<std::mutex> lock(mu_);
        BucketSnapshot snap;
        snap.bucket_id = state_.buckets.size();
        snap.label = std::move(label);
        snap.total_tasks = total_tasks;
        state_.buckets.push_back(std::move(snap));
        recompute_total_registered();
        return state_.buckets.back().bucket_id;
    }

    /**
     * @brief A bucket may be registered before its `total_tasks` is
     *        known, or its task list may shrink after `should_skip_cell`
     *        rules apply. This setter keeps the gauge denominator in
     *        sync with what will actually run.
     */
    void update_bucket_total(std::size_t bucket_id, std::size_t new_total) {
        std::lock_guard<std::mutex> lock(mu_);
        if (bucket_id >= state_.buckets.size()) return;
        state_.buckets[bucket_id].total_tasks = new_total;
        recompute_total_registered();
    }

    /// Atomically mark `worker_id` as running `task_label` inside `bucket_id`.
    void record_task_started(std::size_t bucket_id,
                             std::size_t worker_id,
                             std::string task_label) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mu_);
        if (bucket_id < state_.buckets.size()) {
            auto& bucket = state_.buckets[bucket_id];
            bucket.in_flight_tasks += 1;
            if (!bucket.started) {
                bucket.started = true;
                bucket.first_started = now;
            }
        }
        if (worker_id < state_.worker_slots.size()) {
            auto& slot = state_.worker_slots[worker_id];
            slot.label = std::move(task_label);
            slot.started_at = now;
            slot.active = true;
        }
    }

    /// Mark `worker_id` idle and bump the bucket's completion counters.
    /// `samples_collected` is the count of timed iterations the runner
    /// actually captured — distinct from "tasks finished" because the
    /// budget can abort a task with zero useful samples.
    void record_task_finished(std::size_t bucket_id,
                              std::size_t worker_id,
                              double seconds,
                              std::size_t samples_collected,
                              bool task_failed) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mu_);
        if (bucket_id < state_.buckets.size()) {
            auto& bucket = state_.buckets[bucket_id];
            if (bucket.in_flight_tasks > 0) bucket.in_flight_tasks -= 1;
            bucket.finished_tasks += 1;
            if (task_failed) bucket.failed_tasks += 1;
            bucket.samples_collected += samples_collected;
            bucket.sum_seconds += seconds;
            bucket.last_finished = now;
            bucket.any_finished = true;
        }
        if (worker_id < state_.worker_slots.size()) {
            auto& slot = state_.worker_slots[worker_id];
            slot.label.clear();
            slot.active = false;
        }
        state_.total_finished += 1;
        state_.recent_completions.emplace_back(now, seconds);
        if (state_.recent_completions.size() > kRollingWindowCapacity) {
            state_.recent_completions.pop_front();
        }
    }

    /// Note a (config, bucket) cell that was skipped without ever
    /// running. Keeps the bucket gauge denominator honest.
    void record_task_skipped(std::size_t bucket_id) {
        std::lock_guard<std::mutex> lock(mu_);
        if (bucket_id < state_.buckets.size()) {
            auto& bucket = state_.buckets[bucket_id];
            bucket.skipped_tasks += 1;
            bucket.finished_tasks += 1;  // keep gauge denominator coherent
        }
        state_.total_finished += 1;
    }

    /// Push a one-shot status string the next render frame will display
    /// in the notices band, then drop. Used for "SKIP X / Y" lines so
    /// the operator notices the runner deliberately bypassing a cell.
    void post_notice(std::string message) {
        std::lock_guard<std::mutex> lock(mu_);
        state_.pending_notices.push_back(std::move(message));
    }

private:
    /// Re-derive the global gauge denominator from the per-bucket totals.
    /// Called whenever a bucket is added or its total updated.
    void recompute_total_registered() {
        std::size_t total = 0;
        for (const auto& bucket : state_.buckets) total += bucket.total_tasks;
        state_.total_registered = total;
    }

    /// Snapshot copy. Returning by value lets the render thread render
    /// without holding the mutex, so workers don't stall on a slow draw.
    [[nodiscard]] detail::DashboardState snapshot_locked() const {
        return state_;
    }

    void render_loop() {
        while (true) {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(kRenderIntervalMs),
                         [this]() { return stop_requested_; });
            const bool stopping = stop_requested_;
            // Skip drawing entirely until at least one bucket has been
            // registered. Catch2 emits BENCHMARK tables to stdout for
            // the `[simple-benchmark]` TEST_CASEs that run before
            // `[bench-full-matrix]`; if the dashboard rendered empty
            // frames during that window the cursor-up sequence in
            // `ResetPosition` would walk over Catch2's stdout output.
            // The `register_bucket` call inside `run_matrix_for_bucket`
            // marks the start of the matrix sweep — that is when the
            // dashboard should claim the screen.
            if (state_.buckets.empty() && !stopping) {
                lock.unlock();
                continue;
            }
            detail::DashboardState snapshot = state_;
            // Drain notices: we only show each notice once.
            state_.pending_notices.clear();
            lock.unlock();

            if (is_tty_) {
                draw_tty_frame(snapshot, /*final_frame=*/stopping);
            } else {
                draw_fallback_line(snapshot, /*final_frame=*/stopping);
            }

            if (stopping) break;
        }
    }

    /// Build the FTXUI document and print it to stderr in-place.
    void draw_tty_frame(const detail::DashboardState& snapshot, bool final_frame) {
        using namespace ftxui;

        const auto session_seconds = snapshot.session_started
            ? std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - snapshot.session_start).count()
            : 0.0;
        const auto rolling = compute_rolling_stats(snapshot);

        // Collapse fully-completed buckets that are NOT the most-recent
        // few into a single summary row. The matrix sweep registers
        // 6 length × 6 error = 36 buckets (plus the optional k/w sweep
        // and the rmq-repro test case), which would overflow most
        // operator terminals (~40-50 rows) and confuse `ResetPosition`'s
        // line-count math. We always show the in-flight + queued
        // buckets in detail and the last `kRecentlyCompletedDetail`
        // completed buckets in detail; older completions roll up into
        // one "N buckets completed (M tasks, S total)" line at the top.
        constexpr std::size_t kRecentlyCompletedDetail = 4;

        std::size_t collapsed_completed_count = 0;
        std::size_t collapsed_completed_tasks = 0;
        double collapsed_completed_seconds = 0.0;
        std::vector<const BucketSnapshot*> visible_buckets;
        visible_buckets.reserve(snapshot.buckets.size());

        // Walk in reverse: collect the trailing detail-eligible buckets
        // and aggregate the rest.
        std::size_t recent_detail_kept = 0;
        for (auto it = snapshot.buckets.rbegin(); it != snapshot.buckets.rend(); ++it) {
            const auto& bucket = *it;
            const bool is_done = bucket.total_tasks > 0 &&
                                 bucket.finished_tasks >= bucket.total_tasks &&
                                 bucket.in_flight_tasks == 0;
            if (is_done && recent_detail_kept >= kRecentlyCompletedDetail) {
                ++collapsed_completed_count;
                collapsed_completed_tasks += bucket.finished_tasks;
                collapsed_completed_seconds += bucket.sum_seconds;
                continue;
            }
            if (is_done) ++recent_detail_kept;
            visible_buckets.push_back(&bucket);
        }
        // visible_buckets is in reverse-registration order; flip back so
        // the rendering reads "oldest active bucket → newest active".
        std::reverse(visible_buckets.begin(), visible_buckets.end());

        Elements bucket_rows;
        bucket_rows.reserve(visible_buckets.size() + 1);
        if (collapsed_completed_count > 0) {
            char summary_buf[160];
            std::snprintf(summary_buf, sizeof(summary_buf),
                          "+%zu earlier bucket(s) complete  ·  %zu tasks  ·  %s total",
                          collapsed_completed_count,
                          collapsed_completed_tasks,
                          detail::format_duration_seconds(collapsed_completed_seconds).c_str());
            bucket_rows.push_back(text(summary_buf) | dim);
        }
        for (const auto* bucket_ptr : visible_buckets) {
            bucket_rows.push_back(
                render_bucket_row(*bucket_ptr, rolling.mean_seconds_per_task));
        }

        Elements worker_rows;
        worker_rows.reserve(snapshot.worker_slots.size());
        for (const auto& slot : snapshot.worker_slots) {
            worker_rows.push_back(render_worker_row(slot));
        }

        Elements notice_rows;
        notice_rows.reserve(snapshot.pending_notices.size());
        for (const auto& notice : snapshot.pending_notices) {
            notice_rows.push_back(text(notice) | dim);
        }

        const float global_progress = snapshot.total_registered == 0
            ? 0.0f
            : static_cast<float>(snapshot.total_finished) /
              static_cast<float>(snapshot.total_registered);
        const double global_eta_seconds = compute_eta_seconds(
            snapshot.total_registered, snapshot.total_finished, rolling);

        Element document =
            window(text(" bioseq bench progress ") | bold,
                vbox({
                    hbox({
                        text("Elapsed: ") | bold,
                        text(detail::format_duration_seconds(session_seconds)),
                        separatorEmpty(),
                        text("Tasks: ") | bold,
                        text(std::to_string(snapshot.total_finished) + " / " +
                             std::to_string(snapshot.total_registered)),
                        separatorEmpty(),
                        text("Throughput: ") | bold,
                        text(rolling.tasks_per_minute_text),
                        separatorEmpty(),
                        text("Mean/task: ") | bold,
                        text(rolling.mean_per_task_text),
                        separatorEmpty(),
                        text("ETA: ") | bold,
                        text(detail::format_duration_seconds(global_eta_seconds)),
                    }),
                    hbox({
                        text("All buckets ") | bold,
                        gauge(global_progress) | flex,
                        text(" " + percent_label(global_progress)),
                    }),
                    separator(),
                    text("Buckets") | bold,
                    bucket_rows.empty()
                        ? text("(no buckets registered yet)") | dim
                        : vbox(std::move(bucket_rows)),
                    separator(),
                    text("Workers") | bold,
                    worker_rows.empty()
                        ? text("(no workers registered yet)") | dim
                        : vbox(std::move(worker_rows)),
                    notice_rows.empty()
                        ? filler()
                        : vbox({separator(), text("Notices") | bold,
                                vbox(std::move(notice_rows))}),
                }));

        const auto term = Terminal::Size();
        const int desired_height = compute_desired_height(snapshot);
        const int width = term.dimx > 40 ? term.dimx : 80;
        const int height = std::min(desired_height, term.dimy > 4 ? term.dimy : desired_height);
        Screen screen = Screen::Create(Dimension::Fixed(width),
                                       Dimension::Fixed(height));
        Render(screen, document);

        // FTXUI's `Screen::Print()` hard-codes stdout. Catch2's reporter
        // also writes to stdout — mixing the two interleaves the
        // dashboard frame with test output. Take the rendered string
        // and emit it to stderr ourselves so the dashboard lives on a
        // dedicated stream the operator can redirect independently.
        std::cerr << previous_reset_position_;
        std::cerr << screen.ToString();
        std::cerr << std::flush;
        previous_reset_position_ = screen.ResetPosition(/*clear=*/!final_frame);
        if (final_frame) {
            // Append a newline so subsequent stderr messages don't land
            // on top of the final frame.
            std::cerr << '\n' << std::flush;
        }
    }

    /// Plain-text per-line fallback for redirected stderr.
    void draw_fallback_line(const detail::DashboardState& snapshot, bool final_frame) {
        const auto now = std::chrono::steady_clock::now();
        const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_fallback_line_).count();
        if (!final_frame && since_last < kFallbackLineIntervalMs) return;
        last_fallback_line_ = now;

        const auto rolling = compute_rolling_stats(snapshot);
        const double session_seconds = snapshot.session_started
            ? std::chrono::duration<double>(now - snapshot.session_start).count()
            : 0.0;
        const double eta_seconds = compute_eta_seconds(
            snapshot.total_registered, snapshot.total_finished, rolling);

        std::ostringstream line;
        line << "[bench-progress] elapsed=" << detail::format_duration_seconds(session_seconds)
             << " tasks=" << snapshot.total_finished << '/' << snapshot.total_registered
             << " throughput=" << rolling.tasks_per_minute_text
             << " mean=" << rolling.mean_per_task_text
             << " eta=" << detail::format_duration_seconds(eta_seconds);
        for (const auto& bucket : snapshot.buckets) {
            if (bucket.total_tasks == 0) continue;
            line << " | " << bucket.label << ' ' << bucket.finished_tasks
                 << '/' << bucket.total_tasks
                 << " (in_flight=" << bucket.in_flight_tasks << ')';
        }
        std::cerr << line.str() << '\n' << std::flush;
        for (const auto& notice : snapshot.pending_notices) {
            std::cerr << "[bench-progress] notice: " << notice << '\n';
        }
    }

    /// Aggregate the rolling-completion deque into the human-readable
    /// readouts the dashboard header and ETA need. Pulled out so both
    /// the TTY and fallback paths share one implementation.
    struct RollingStats {
        double mean_seconds_per_task = 0.0;
        double tasks_per_minute = 0.0;
        std::string tasks_per_minute_text;
        std::string mean_per_task_text;
    };
    [[nodiscard]] RollingStats compute_rolling_stats(
        const detail::DashboardState& snapshot) const
    {
        RollingStats stats;
        if (snapshot.recent_completions.empty()) {
            stats.tasks_per_minute_text = "—";
            stats.mean_per_task_text = "—";
            return stats;
        }
        double sum_seconds = 0.0;
        for (const auto& entry : snapshot.recent_completions) sum_seconds += entry.second;
        const std::size_t window_size = snapshot.recent_completions.size();
        stats.mean_seconds_per_task = sum_seconds / static_cast<double>(window_size);

        // Throughput across the window: tasks divided by the wall-clock
        // span between the first and last entry. Falls back to the
        // mean-per-task reciprocal if the span is zero (single entry).
        const auto& front = snapshot.recent_completions.front();
        const auto& back  = snapshot.recent_completions.back();
        const double span_seconds = std::chrono::duration<double>(
            back.first - front.first).count();
        if (window_size > 1 && span_seconds > 0.0) {
            stats.tasks_per_minute =
                (static_cast<double>(window_size - 1) / span_seconds) * 60.0;
        } else if (stats.mean_seconds_per_task > 0.0) {
            stats.tasks_per_minute = 60.0 / stats.mean_seconds_per_task;
        }

        char tpm_buf[32];
        std::snprintf(tpm_buf, sizeof(tpm_buf), "%.1f /min", stats.tasks_per_minute);
        stats.tasks_per_minute_text = tpm_buf;
        stats.mean_per_task_text =
            detail::format_duration_seconds(stats.mean_seconds_per_task);
        return stats;
    }

    /// ETA = remaining_tasks * mean_seconds. We deliberately use the
    /// rolling mean (last 64 completions) rather than the run-wide mean
    /// because tasks vary by ~3 orders of magnitude across length
    /// buckets — the rolling mean reacts within seconds to a transition.
    [[nodiscard]] static double compute_eta_seconds(std::size_t total,
                                                    std::size_t done,
                                                    const RollingStats& rolling) {
        if (total <= done || rolling.mean_seconds_per_task <= 0.0) return 0.0;
        return static_cast<double>(total - done) * rolling.mean_seconds_per_task;
    }

    /// Layout for the "Buckets" section. Each row: `label  [gauge]
    /// done/total in_flight=N samples=K mean=Xs elapsed=Ts`.
    [[nodiscard]] static ftxui::Element render_bucket_row(const BucketSnapshot& bucket,
                                                          double rolling_mean_seconds)
    {
        using namespace ftxui;
        const float frac = bucket.total_tasks == 0
            ? 0.0f
            : static_cast<float>(bucket.finished_tasks) /
              static_cast<float>(bucket.total_tasks);
        const double bucket_mean = bucket.finished_tasks > 0
            ? bucket.sum_seconds / static_cast<double>(bucket.finished_tasks)
            : 0.0;
        const double remaining = bucket.total_tasks > bucket.finished_tasks
            ? static_cast<double>(bucket.total_tasks - bucket.finished_tasks)
            : 0.0;
        const double bucket_eta = remaining > 0.0
            ? remaining * (bucket_mean > 0.0 ? bucket_mean : rolling_mean_seconds)
            : 0.0;
        const auto bucket_elapsed = bucket.started
            ? std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - bucket.first_started).count()
            : 0.0;

        Element gauge_widget = gauge(frac);
        if (bucket.total_tasks == 0) {
            gauge_widget = text("(skipped — 0 tasks)") | dim;
        } else if (frac >= 1.0f) {
            gauge_widget = gauge_widget | color(Color::Green);
        } else if (bucket.in_flight_tasks > 0) {
            gauge_widget = gauge_widget | color(Color::Yellow);
        }

        char counts_buf[64];
        std::snprintf(counts_buf, sizeof(counts_buf),
                      "%zu/%zu",
                      bucket.finished_tasks, bucket.total_tasks);

        // Build detail text via std::string to avoid the snprintf
        // glyph-byte-count trap (UTF-8 separators in the label can
        // truncate mid-codepoint and produce garbled output). Using
        // std::string keeps the buffer sized to the actual content
        // and lets `dim` decorate the whole tail uniformly.
        std::string detail_text =
            "in_flight=" + std::to_string(bucket.in_flight_tasks) +
            " samples=" + std::to_string(bucket.samples_collected) +
            " mean=" + detail::format_duration_seconds(bucket_mean) +
            " elapsed=" + detail::format_duration_seconds(bucket_elapsed) +
            " eta=" + detail::format_duration_seconds(bucket_eta);
        if (bucket.failed_tasks > 0) {
            detail_text += " failed=" + std::to_string(bucket.failed_tasks);
        }
        if (bucket.skipped_tasks > 0) {
            detail_text += " budget_skipped=" + std::to_string(bucket.skipped_tasks);
        }

        return hbox({
            text(bucket.label) | size(WIDTH, EQUAL, 38),
            text(" "),
            gauge_widget | flex,
            text(" "),
            text(counts_buf) | size(WIDTH, EQUAL, 14),
            text(" "),
            text(detail_text) | dim,
        });
    }

    [[nodiscard]] static ftxui::Element render_worker_row(const WorkerSlotSnapshot& slot) {
        using namespace ftxui;
        char id_buf[16];
        std::snprintf(id_buf, sizeof(id_buf), "W%02zu", slot.worker_id);
        if (!slot.active || slot.label.empty()) {
            return hbox({
                text(id_buf) | size(WIDTH, EQUAL, 5) | dim,
                text(" idle") | dim,
            });
        }
        const double slot_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - slot.started_at).count();
        return hbox({
            text(id_buf) | size(WIDTH, EQUAL, 5) | bold,
            text(" "),
            text(slot.label) | flex,
            text(" "),
            text(detail::format_duration_seconds(slot_seconds))
                | size(WIDTH, EQUAL, 8) | dim,
        });
    }

    [[nodiscard]] static std::string percent_label(float fraction) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%5.1f%%", fraction * 100.0f);
        return std::string{buf};
    }

    [[nodiscard]] static int compute_desired_height(const detail::DashboardState& snapshot) {
        // Header (1) + global gauge (1) + separator + "Buckets" header
        // + per-bucket rows + separator + "Workers" header + per-worker
        // rows + optional notices block + window borders.
        //
        // Bucket rows are computed from the collapsed view — at most
        // (in-flight + queued + recently-completed-detail-window + 1
        // summary line) — not the raw bucket count. This keeps the
        // dashboard fitting in a typical 40-50-row terminal even when
        // 36+ buckets are registered. The constants here track the
        // collapsing logic in `draw_tty_frame`.
        constexpr std::size_t kRecentlyCompletedDetail = 4;
        std::size_t active_or_queued = 0;
        std::size_t completed_count = 0;
        for (const auto& bucket : snapshot.buckets) {
            const bool is_done = bucket.total_tasks > 0 &&
                                 bucket.finished_tasks >= bucket.total_tasks &&
                                 bucket.in_flight_tasks == 0;
            if (is_done) ++completed_count;
            else ++active_or_queued;
        }
        const std::size_t shown_completed = std::min(completed_count, kRecentlyCompletedDetail);
        const std::size_t collapsed_summary_row = completed_count > kRecentlyCompletedDetail ? 1 : 0;
        const int bucket_rows = std::max<int>(
            1,
            static_cast<int>(active_or_queued + shown_completed + collapsed_summary_row));

        const int notice_height = snapshot.pending_notices.empty()
            ? 0
            : static_cast<int>(snapshot.pending_notices.size() + 2);
        const int base = 1   // header line
                       + 1   // global gauge
                       + 1   // separator
                       + 1   // "Buckets" header
                       + bucket_rows
                       + 1   // separator
                       + 1   // "Workers" header
                       + std::max<int>(1, static_cast<int>(snapshot.worker_slots.size()))
                       + notice_height
                       + 2;  // window border
        return base;
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    detail::DashboardState state_;
    std::thread render_thread_;
    bool running_ = false;
    bool stop_requested_ = false;
    bool is_tty_ = false;
    std::chrono::steady_clock::time_point last_fallback_line_{};
    /// Saved cursor-reset string from the previous frame. Printed at
    /// the start of each new frame so the dashboard rewrites itself in
    /// place.
    std::string previous_reset_position_;
};

/**
 * @brief Process-wide dashboard accessor.
 *
 * The bench runs as a single executable, so a plain function-local
 * static is the right shape — no DI plumbing through Catch2's TEST_CASE
 * boundary, no allocation lifecycle. The matrix runner pulls this
 * accessor in via `import bench.progress_ui` and routes every event
 * through it.
 */
[[nodiscard]] inline Dashboard& dashboard() {
    static Dashboard d;
    return d;
}

}  // namespace bioseq::bench::progress

}  // export
