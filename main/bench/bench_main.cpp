/**
 * @file bench_main.cpp
 * @brief Entry point for the aligner-bench Catch2 binary.
 *
 * Drives Catch2's session loop (the bench TEST_CASEs in `bench_test.cpp`
 * populate the chart-builder accumulators while they run), then —
 * once the session returns — emits the per-chart plotly report to disk,
 * runs the regression check from `regression_thresholds.json`, prints
 * any violations as a console table, and propagates the failure into
 * the process exit code.
 *
 * After the bench_scheduler refactor:
 *
 *   * file-system / git utilities (next-report-path picker, commit-hash
 *     resolver) live in `bench.scheduler.report_files` so the IO bench
 *     and any future bench target can reuse them;
 *   * the regression rule engine is generic
 *     (`bench.scheduler.regression`); the aligner-specific cell
 *     aggregation + production rule defaults live in
 *     `bench.aligners.regression_glue`;
 *   * the chart-builder, run-config, and matrix-runner all moved under
 *     `bench.aligners.*`; this file imports them via the aligner
 *     namespace.
 *
 * ## Custom CLI flags
 *
 *   * `--everything` — opt into the (k, w) Cartesian sweep on top of
 *                      the standard length × error matrix AND run the
 *                      noisy×long combinations (ONT_LEGACY /
 *                      HIGH_INDEL_LOW_SUB on LONG / VERY_LONG / XL /
 *                      XXL).
 *   * `--fast`       — reduce per-bucket sample counts ~3-4× and
 *                      saturate worker threads. Higher noise floor
 *                      (~±3% vs ±1%); use for iteration, not sign-off.
 *
 * Either or both can be passed; their effects compose.
 */
#include "common_tests.h"
#include "common.h"

#include <catch2/catch_session.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import mlogging;
import testing;
import bench.scheduler;
import bench.aligners.chart_builder;
import bench.aligners.matrix_runner;
import bench.aligners.regression_glue;
import bench.aligners.run_config;
import bench.aligners.samples;

#ifndef BIOSEQ_BENCH_REPORT_DIR
#error "BIOSEQ_BENCH_REPORT_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_BENCH_SOURCE_DIR
#error "BIOSEQ_BENCH_SOURCE_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_BENCH_THRESHOLDS_PATH
#error "BIOSEQ_BENCH_THRESHOLDS_PATH must be defined by the build system"
#endif

namespace {

/// Strip the project-specific flags out of argv before forwarding to
/// Catch2. Returns the filtered argv (pointers into the original
/// `argv` array — no allocation, no reordering of remaining flags).
struct ParsedArgs {
    std::vector<char*> filtered_argv;
    bool everything = false;
    bool fast = false;
};
[[nodiscard]] ParsedArgs parse_bench_flags(int argc, char* argv[]) {
    ParsedArgs parsed;
    parsed.filtered_argv.reserve(static_cast<std::size_t>(argc));
    if (argc > 0) parsed.filtered_argv.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--everything") {
            parsed.everything = true;
            continue;
        }
        if (arg == "--fast") {
            parsed.fast = true;
            continue;
        }
        parsed.filtered_argv.push_back(argv[i]);
    }
    return parsed;
}

/**
 * @brief Catch2 listener that re-clamps fmtlog to ERR on every test case
 *        start.
 *
 * The `testing.cppm` module's `testRunStarting` hook calls
 * `log_configure(true)`, which sets fmtlog back to DBG. Anything we set
 * in `main()` before `session.run()` gets overwritten the moment
 * Catch2's run begins. This second listener re-clamps the level on
 * every `testCaseStarting` notification — those fire after
 * `testRunStarting`, so by the time any bench code runs the level is
 * back to ERR.
 *
 * Without this, the bench's stderr fills with tens of thousands of
 * DBG/INF lines per run (sketch dumps, per-pair traces) and the FTXUI
 * dashboard plus the regression-check table become unreadable.
 */
class BenchLogLevelClamp : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testCaseStarting(Catch::TestCaseInfo const&) override {
        fmtlog::setLogLevel(fmtlog::ERR);
    }
};

}  // namespace

CATCH_REGISTER_LISTENER(BenchLogLevelClamp)

int main( int argc, char* argv[] ) {
    // Silence DBG/INF/WRN noise during bench so the regression-check
    // table at the end is the loudest thing on stderr. The progress
    // dashboard renders to stderr too, so cutting fmtlog to ERR-only
    // also keeps the dashboard frame from being interleaved with
    // per-iteration trace lines.
    log_configure(true);
    fmtlog::setLogLevel(fmtlog::ERR);

    // Parse our project-specific flags (--everything, --fast) and
    // forward everything else to Catch2.
    ParsedArgs parsed = parse_bench_flags(argc, argv);

    // Resolve and stash the run config so matrix_runner / bench_test
    // see the right knobs by the time `session.run()` starts firing
    // TEST_CASEs. The split is intentional:
    //
    //   * `bench_scheduler::core_run_config()` carries every knob a
    //     hypothetical IO bench would also need (threads, fast_mode,
    //     per_call_budget_s);
    //   * `bench::aligner_run_config()` carries the aligner-only
    //     knobs (sample profile, k/w sweep gating, noisy×long opt-in).
    //
    // Both must be set before `session.run()` because workers later
    // read them on the hot path; we deliberately do not take a mutex
    // on the read side.
    auto& core_cfg = bioseq::bench_scheduler::core_run_config();
    core_cfg.fast_mode = parsed.fast;
    // Threads stays at 0 so `resolve_thread_count` derives it from
    // hardware (cpu/2 default; full hw_concurrency() under fast_mode).
    core_cfg.threads = 0;
    core_cfg.per_call_budget_s = parsed.fast ? 300.0 : 600.0;

    auto& aligner_cfg = bioseq::bench::aligner_run_config();
    aligner_cfg.include_kw_sweep   = parsed.everything;
    aligner_cfg.include_noisy_long = parsed.everything;
    aligner_cfg.sample_profile = parsed.fast
        ? bioseq::bench::SampleProfile::FAST
        : bioseq::bench::SampleProfile::DEFAULT;

    // Surface the resolved knobs through fmtlog at ERR level — the
    // only level that survives `setLogLevel(ERR)` above. A one-shot
    // config line in the scrollback is what reviewers grep for when
    // they want to confirm "did this report come from a --fast run?".
    loge("[bench_main] config: everything={} fast={} threads={} profile={} budget={}s",
         parsed.everything ? "true" : "false",
         parsed.fast       ? "true" : "false",
         bioseq::bench_scheduler::resolve_thread_count(core_cfg),
         core_cfg.fast_mode ? "fast" : "default",
         core_cfg.per_call_budget_s);

    // Boot the live FTXUI progress dashboard. The render thread starts
    // right away but defers actually painting until at least one bucket
    // has been registered, so it does NOT garble the Catch2 BENCHMARK
    // tables emitted by the `[simple-benchmark]` TEST_CASEs that run
    // before `[bench-full-matrix]`.
    bioseq::bench_scheduler::progress::dashboard().start();

    Catch::Session session;  // There must be exactly one instance.

    int returnCode = session.applyCommandLine(
        static_cast<int>(parsed.filtered_argv.size()),
        parsed.filtered_argv.data());
    if (returnCode != 0) {  // Command-line error.
        bioseq::bench_scheduler::progress::dashboard().stop();
        return returnCode;
    }

    returnCode = session.run();

    // Stop the dashboard *before* we emit the regression-check table
    // and report-write summary — those are durable lines we want the
    // operator to see in scrollback, not overwritten by the next
    // dashboard frame.
    bioseq::bench_scheduler::progress::dashboard().stop();

    // Run the regression check before writing the report so we can
    // include the violations array directly in the document. Reading
    // the same accumulator the chart builder reads keeps the two views
    // in sync — JSON consumers and console readers see the same
    // numbers. The thresholds path is baked in at build time; no env
    // var is consulted.
    const std::string thresholds_path = std::string{BIOSEQ_BENCH_THRESHOLDS_PATH};
    const auto regression_config =
        bioseq::bench_scheduler::regression::load_config_from_file(thresholds_path);
    const auto violations = bioseq::bench::compute_aligner_violations(
        bioseq::bench::benchmark_samples(), regression_config);

    // Reports always land in `main/reporting/`, regardless of cwd or
    // build mode. The directory is created on demand. Every run
    // produces a numbered file in the same place so before/after diffs
    // across commits stay trivial.
    const std::filesystem::path report_dir = BIOSEQ_BENCH_REPORT_DIR;
    const std::string report_path =
        bioseq::bench_scheduler::next_numbered_report_path(report_dir);
    const std::string commit_hash =
        bioseq::bench_scheduler::detect_git_commit_hash(BIOSEQ_BENCH_SOURCE_DIR);

    if (!bioseq::bench::write_report_json_to_file(
            report_path, violations, commit_hash)) {
        loge("[bench_main] failed to write report to {}", report_path);
    } else {
        loge("[bench_main] wrote report to {} (commit {})",
             report_path, commit_hash);
    }

    bioseq::bench_scheduler::regression::print_violations_table(violations);

    // Convert regression failures into a non-zero exit code so CI /
    // `set -e` shells catch the failure. We only override when Catch2
    // itself returned 0 — preserving its dedicated exit codes
    // (compilation issues, command-line errors, etc.) when something
    // bigger went wrong than just a regression.
    if (!violations.empty() && returnCode == 0) {
        returnCode = 2;
    }

    return returnCode;
}
