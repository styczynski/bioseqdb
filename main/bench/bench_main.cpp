#include "common_tests.h"
#include "common.h"

#include <catch2/catch_session.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

import mlogging;
import testing;
import bench.chart_builder;
import bench.matrix_runner;
import bench.regression_check;
import bench.run_config;
import bench.progress_ui;

/**
 * @file bench_main.cpp
 * @brief Entry point for the bench executable.
 *
 * Drives Catch2's session loop (the bench TEST_CASEs in bench_test.cpp
 * populate the chart-builder accumulators while they run), then — once
 * the session returns — emits the per-chart plotly report to disk, runs
 * the regression check from `regression_thresholds.json`, prints any
 * violations as a console table, and propagates the failure into the
 * process exit code.
 *
 * ## Custom CLI flags
 *
 * Two project-specific flags are parsed *before* arguments are forwarded
 * to Catch2:
 *
 *   * `--everything`  — opt into the (k, w) Cartesian sweep on top of
 *                       the standard length × error matrix, AND run the
 *                       noisy×long combinations (ONT_LEGACY /
 *                       HIGH_INDEL_LOW_SUB on LONG / VERY_LONG / XL /
 *                       XXL) that are otherwise skipped. The default
 *                       sweep is "everything except things that even
 *                       minimap2 cannot recover from".
 *   * `--fast`        — reduce per-bucket sample counts by ~3-4× and
 *                       saturate the box (`hardware_concurrency()`
 *                       worker threads instead of cpu/2). The trade-off
 *                       is a higher noise floor (~±3% vs ±1%).
 *
 * Either or both can be passed; their effects compose. Every other
 * flag is forwarded to Catch2 unchanged (`--list-tests`, tag filters,
 * `--reporter`, etc.).
 *
 * ## Output naming
 *
 * The report file name is **always** of the form `report_build_<N>.json`,
 * with `<N>` chosen as one above the highest existing `report_build_*.json`
 * in `main/reporting/` (the canonical, build-mode-independent location).
 * Each run appends a new file; we deliberately never overwrite previous
 * reports because before/after diffs across commits are the primary
 * investigation tool. Keep the existing reports around — they're cheap
 * (a few MB each) and load-bearing for reproducibility.
 *
 * The reporting directory path is baked in at compile time as
 * `BIOSEQ_BENCH_REPORT_DIR` (a CMake target compile definition) so the
 * binary doesn't have to guess where the source tree lives at runtime.
 *
 * ## Commit hash
 *
 * The commit hash is resolved at runtime by spawning `git rev-parse HEAD`
 * inside the repository root passed in via `BIOSEQ_BENCH_SOURCE_DIR`
 * (also a build-time define). On any failure (no git, detached worktree,
 * etc.) the field is set to `"unknown"`. This used to be sourced from
 * a `BIOSEQ_GIT_COMMIT` env var set by `build.sh`; the env var is now
 * gone — the bench binary self-stamps its own commit so an operator
 * who runs the binary directly still gets a coherent report.
 *
 * ## Logging
 *
 * `log_configure(true)` sets fmtlog to DBG. The bench produces tens of
 * thousands of DBG lines per run (sketch dumps, per-pair traces) which
 * drown the regression-check console table. We dial it down to ERR-only
 * here so the bench output stays focused on the regression result and
 * the FTXUI progress dashboard.
 */

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

/// Pick the next free `report_build_<N>.json` in the reporting directory.
/// Walks the directory once, parses any existing `report_build_<N>.json`
/// filenames, returns N+1 (or 1 if none exist). Creates the directory
/// if it does not yet exist — the path is `${PROJECT_ROOT}/main/reporting`
/// so we do not need to walk parents.
[[nodiscard]] std::string next_report_path(const std::filesystem::path& dir) {
    static const std::regex pattern{R"(^report_build_(\d+)\.json$)"};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    long long max_index = 0;
    if (std::filesystem::exists(dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            std::smatch match;
            if (!std::regex_match(name, match, pattern)) continue;
            // Use std::from_chars rather than std::stoll: from_chars
            // returns an error code for unparseable / overflowing
            // input rather than throwing, which keeps this function
            // exception-free per CLAUDE.md §1.
            const std::string index_str = match[1].str();
            long long parsed_index = 0;
            const auto result = std::from_chars(
                index_str.data(),
                index_str.data() + index_str.size(),
                parsed_index);
            if (result.ec != std::errc{}) continue;
            if (parsed_index > max_index) max_index = parsed_index;
        }
    }
    return (dir / ("report_build_" + std::to_string(max_index + 1) + ".json")).string();
}

/// Resolve the current commit hash by spawning `git rev-parse HEAD`
/// inside the repository root. The build system passes the absolute
/// repo path in via `BIOSEQ_BENCH_SOURCE_DIR` so this works regardless
/// of where the binary is invoked from. Any failure (no git on PATH,
/// detached worktree, repo deleted) falls back to `"unknown"`.
[[nodiscard]] std::string detect_git_commit_hash() {
    constexpr std::string_view source_dir = BIOSEQ_BENCH_SOURCE_DIR;
    // `2>/dev/null` swallows git's "not a git repository" stderr noise.
    // `-C <dir>` makes git operate on the repository regardless of cwd
    // (the bench is launched from build/<mode>/, not the repo root).
    std::string command = "git -C \"";
    command.append(source_dir);
    command.append("\" rev-parse HEAD 2>/dev/null");
    std::FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) return std::string{"unknown"};
    std::array<char, 64> buffer{};
    std::string commit_hash;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        commit_hash.append(buffer.data());
    }
    const int rc = ::pclose(pipe);
    if (rc != 0) return std::string{"unknown"};
    while (!commit_hash.empty() &&
           (commit_hash.back() == '\n' || commit_hash.back() == '\r' ||
            commit_hash.back() == ' '  || commit_hash.back() == '\t')) {
        commit_hash.pop_back();
    }
    if (commit_hash.empty()) return std::string{"unknown"};
    return commit_hash;
}

/// Strip our custom flags out of argv before forwarding to Catch2.
/// Returns the filtered argv (pointers into the original `argv` array
/// — no allocation, no reordering of remaining flags).
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
 * The testing module (`testing.cppm`) registers a `testRunListener` whose
 * `testRunStarting` hook calls `log_configure(true)`, which sets fmtlog
 * back to DBG. Anything we set in `main()` before `session.run()` gets
 * overwritten the moment Catch2's run begins. This second listener
 * re-clamps the level on every `testCaseStarting` notification — those
 * fire after `testRunStarting`, so by the time any bench code runs the
 * level is back to ERR.
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
    // TEST_CASEs.
    auto& config = bioseq::bench::bench_run_config();
    config.include_kw_sweep = parsed.everything;
    config.include_noisy_long = parsed.everything;
    config.fast_mode = parsed.fast;
    config.sample_profile = parsed.fast
        ? bioseq::bench::SampleProfile::FAST
        : bioseq::bench::SampleProfile::DEFAULT;
    // Threads stays at 0 so `resolve_thread_count` derives it from the
    // profile (cpu/2 default, full hw concurrency for --fast).
    config.threads = 0;
    config.per_call_budget_s = parsed.fast ? 300.0 : 600.0;

    // Surface the resolved knobs through fmtlog at ERR level — it's
    // the only level that survives `setLogLevel(ERR)` above. The
    // dashboard surfaces threads / budget too, but a one-shot config
    // line in the scrollback is what reviewers grep for when they
    // want to confirm "did this report come from a --fast run?".
    loge("[bench_main] config: everything={} fast={} threads={} profile={} budget={}s",
         parsed.everything ? "true" : "false",
         parsed.fast       ? "true" : "false",
         bioseq::bench::resolve_thread_count(config),
         config.fast_mode ? "fast" : "default",
         config.per_call_budget_s);

    // Boot the live FTXUI progress dashboard. The render thread starts
    // right away but defers actually painting until at least one bucket
    // has been registered (`progress_ui.cppm` checks that internally),
    // so it does NOT garble the Catch2 BENCHMARK tables emitted by the
    // `[simple-benchmark]` TEST_CASEs that run before
    // `[bench-full-matrix]`.
    bioseq::bench::progress::dashboard().start();

    Catch::Session session; // There must be exactly one instance

    int returnCode = session.applyCommandLine(
        static_cast<int>(parsed.filtered_argv.size()),
        parsed.filtered_argv.data());
    if( returnCode != 0 ) { // Indicates a command line error
        bioseq::bench::progress::dashboard().stop();
        return returnCode;
    }

    returnCode = session.run();

    // Stop the dashboard *before* we emit the regression-check table
    // and report-write summary — those are durable lines we want the
    // operator to see in scrollback, not overwritten by the next
    // dashboard frame.
    bioseq::bench::progress::dashboard().stop();

    // Run the regression check before writing the report so we can
    // include the violations array directly in the document. Reading
    // the same accumulator the chart builder reads keeps the two views
    // in sync — the JSON consumer and the console reader see the same
    // numbers. The thresholds path is baked in at build time; no env
    // var is consulted.
    const std::string thresholds_path = std::string{BIOSEQ_BENCH_THRESHOLDS_PATH};
    const auto regression_config =
        bioseq::bench::load_config_from_file(thresholds_path);
    const auto violations = bioseq::bench::compute_violations(
        bioseq::bench::benchmark_samples(), regression_config);

    // Reports always land in `main/reporting/`, regardless of cwd or
    // build mode. The directory is created on demand. The previous
    // `BIOSEQ_BENCH_REPORT_PATH` env-var override is gone — every run
    // should produce a numbered file in the same place so before/after
    // diffs across commits stay trivial.
    const std::filesystem::path report_dir = BIOSEQ_BENCH_REPORT_DIR;
    const std::string report_path = next_report_path(report_dir);

    const std::string commit_hash = detect_git_commit_hash();

    if (!bioseq::bench::write_report_json_to_file(
            report_path, violations, commit_hash)) {
        loge("[bench_main] failed to write report to {}", report_path);
    } else {
        loge("[bench_main] wrote report to {} (commit {})",
             report_path, commit_hash);
    }

    bioseq::bench::print_violations_table(violations);

    // Convert regression failures into a non-zero exit code so CI /
    // `set -e` shells catch the failure. We only override when Catch2
    // itself returned 0 — preserving its dedicated exit codes
    // (compilation issues, command-line errors etc.) when something
    // bigger went wrong than just a regression.
    if (!violations.empty() && returnCode == 0) {
        returnCode = 2;
    }

    return returnCode;
}
