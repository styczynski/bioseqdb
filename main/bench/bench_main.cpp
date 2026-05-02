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
 * Most of that pipeline is generic and lives in
 * `bench.scheduler.harness`; this file is the aligner-specific
 * harness wiring (CLI flag parsing, run-config singleton population,
 * domain-specific compute_violations + emit_report callbacks).
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
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
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

// Generic Catch2 listener — clamps fmtlog to ERR on every test case
// start so the FTXUI dashboard + regression-check table aren't drowned
// by per-iteration trace lines. Defined once in
// `bench.scheduler.catch_listener`; every harness registers the same
// type, no aligner-specific subclass needed.
CATCH_REGISTER_LISTENER(bioseq::bench_scheduler::StandardLogClamp)

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

}  // namespace

int main( int argc, char* argv[] ) {
    log_configure(true);
    fmtlog::setLogLevel(fmtlog::ERR);

    // Parse our project-specific flags (--everything, --fast) and
    // forward everything else to Catch2.
    ParsedArgs parsed = parse_bench_flags(argc, argv);

    // Two-tier run config: scheduler-side `core_run_config` carries
    // every knob a hypothetical IO bench would also need (threads,
    // fast_mode, per_call_budget_s); aligner-side `aligner_run_config`
    // carries the aligner-only knobs (sample profile, k/w sweep
    // gating, noisy×long opt-in). Both must be set before
    // `session.run()` because workers later read them on the hot path.
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

    loge("[bench_main] config: everything={} fast={} threads={} profile={} budget={}s",
         parsed.everything ? "true" : "false",
         parsed.fast       ? "true" : "false",
         bioseq::bench_scheduler::resolve_thread_count(core_cfg),
         core_cfg.fast_mode ? "fast" : "default",
         core_cfg.per_call_budget_s);

    // Boot the live FTXUI progress dashboard. Render thread starts
    // immediately but defers painting until at least one bucket has
    // been registered, so it does NOT garble Catch2 BENCHMARK tables
    // emitted by the `[simple-benchmark]` TEST_CASEs that run before
    // `[bench-full-matrix]`.
    bioseq::bench_scheduler::progress::dashboard().start();

    Catch::Session session;
    int returnCode = session.applyCommandLine(
        static_cast<int>(parsed.filtered_argv.size()),
        parsed.filtered_argv.data());
    if (returnCode != 0) {  // Command-line error.
        bioseq::bench_scheduler::progress::dashboard().stop();
        return returnCode;
    }
    returnCode = session.run();

    // Compute aligner-domain violations from `benchmark_samples()`.
    // The threshold-rule load is generic; the BenchSample → cell-key
    // aggregation and metric extraction live in
    // `bench.aligners.regression_glue`.
    const auto regression_config =
        bioseq::bench_scheduler::regression::load_config_from_file(
            std::string{BIOSEQ_BENCH_THRESHOLDS_PATH});
    const auto violations = bioseq::bench::compute_aligner_violations(
        bioseq::bench::benchmark_samples(), regression_config);

    // Hand off to the scheduler-side post-session pipeline: stops the
    // dashboard, picks the next `report_build_<N>.json` path, stamps
    // the commit hash, calls our chart-emit lambda, prints the
    // violations table, returns the exit code.
    const bioseq::bench_scheduler::BenchHarnessOptions harness_opts{
        .repo_root  = BIOSEQ_BENCH_SOURCE_DIR,
        .report_dir = std::filesystem::path{BIOSEQ_BENCH_REPORT_DIR},
        .log_tag    = "bench_main",
    };
    return bioseq::bench_scheduler::finalize_bench_run(
        returnCode, harness_opts,
        std::span<const bioseq::bench_scheduler::regression::Violation>{violations},
        bioseq::bench::emit_report);
}
