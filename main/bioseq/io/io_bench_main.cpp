/**
 * @file io_bench_main.cpp
 * @brief Entry point for the bioseq_io_bench Catch2 binary.
 *
 * Mirrors `main/bench/bench_main.cpp`:
 *
 *   1. Parse the IO-bench-specific flags (`--fast`).
 *   2. Populate `bench_scheduler::core_run_config()` from the flags.
 *   3. Boot the FTXUI dashboard, run Catch2's session.
 *   4. Compute regression violations from `io_bench_samples()`.
 *   5. Hand off to `bench_scheduler::finalize_bench_run` which
 *      writes the report, prints the violations table, and returns
 *      the exit code.
 *
 * Spec §18.1.
 */
#include "common_tests.h"
#include "common.h"

#include <catch2/catch_session.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import mlogging;
import testing;
import bench.scheduler.catch_listener;
import bench.scheduler.harness;
import bench.scheduler.progress_ui;
import bench.scheduler.regression;
import bench.scheduler.run_config;
import bioseq_io_bench.regression_glue;
import bioseq_io_bench.samples;
import bioseq_io_bench.chart_builder;

#ifndef BIOSEQ_IO_REPORT_DIR
#error "BIOSEQ_IO_REPORT_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_IO_SOURCE_DIR
#error "BIOSEQ_IO_SOURCE_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_IO_THRESHOLDS_PATH
#error "BIOSEQ_IO_THRESHOLDS_PATH must be defined by the build system"
#endif

CATCH_REGISTER_LISTENER(bioseq::bench_scheduler::StandardLogClamp)

namespace {

struct ParsedArgs {
    std::vector<char*> filtered_argv;
    bool fast = false;
};

[[nodiscard]] ParsedArgs parse_io_bench_flags(int argc, char* argv[]) {
    ParsedArgs parsed;
    parsed.filtered_argv.reserve(static_cast<std::size_t>(argc));
    if (argc > 0) parsed.filtered_argv.push_back(argv[0]);
    for (int arg_index = 1; arg_index < argc; ++arg_index) {
        const std::string_view current_arg{argv[arg_index]};
        if (current_arg == "--fast") {
            parsed.fast = true;
            continue;
        }
        parsed.filtered_argv.push_back(argv[arg_index]);
    }
    return parsed;
}

}  // namespace

int main(int argc, char* argv[]) {
    log_configure(true);
    fmtlog::setLogLevel(fmtlog::ERR);

    ParsedArgs parsed = parse_io_bench_flags(argc, argv);

    auto& core_cfg = bioseq::bench_scheduler::core_run_config();
    core_cfg.fast_mode = parsed.fast;
    core_cfg.threads = 0;
    core_cfg.per_call_budget_s = parsed.fast ? 60.0 : 300.0;

    loge("[io_bench_main] config: fast={} threads={} budget={}s",
         parsed.fast ? "true" : "false",
         bioseq::bench_scheduler::resolve_thread_count(core_cfg),
         core_cfg.per_call_budget_s);

    bioseq::bench_scheduler::progress::dashboard().start();

    Catch::Session session;
    int return_code = session.applyCommandLine(
        static_cast<int>(parsed.filtered_argv.size()),
        parsed.filtered_argv.data());
    if (return_code != 0) {
        bioseq::bench_scheduler::progress::dashboard().stop();
        return return_code;
    }
    return_code = session.run();

    const auto regression_config =
        bioseq::bench_scheduler::regression::load_config_from_file(
            std::string{BIOSEQ_IO_THRESHOLDS_PATH});
    const auto violations = bioseq::io_bench::compute_io_violations(
        bioseq::io_bench::io_bench_samples(),
        bioseq::io_bench::io_bench_writer_samples(),
        regression_config);

    const bioseq::bench_scheduler::BenchHarnessOptions harness_opts{
        .repo_root  = BIOSEQ_IO_SOURCE_DIR,
        .report_dir = std::filesystem::path{BIOSEQ_IO_REPORT_DIR},
        .log_tag    = "io_bench_main",
    };
    return bioseq::bench_scheduler::finalize_bench_run(
        return_code, harness_opts,
        std::span<const bioseq::bench_scheduler::regression::Violation>{violations},
        bioseq::io_bench::emit_io_report);
}
