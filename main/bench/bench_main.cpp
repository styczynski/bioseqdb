#include "common_tests.h"
#include "common.h"

#include <catch2/catch_session.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <string>
#include <system_error>
#include <vector>

import mlogging;
import testing;
import bench.chart_builder;
import bench.matrix_runner;
import bench.regression_check;

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
 * ## Output naming
 *
 * The report file name is **always** of the form `report_build_<N>.json`,
 * with `<N>` chosen as one above the highest existing `report_build_*.json`
 * in the working directory. Each run appends a new file; we deliberately
 * never overwrite previous reports because agents tend to redirect the
 * bench output to a temp location and then lose it, making before/after
 * comparisons painful. Keep the existing reports around — they're cheap
 * (a few MB each) and load-bearing for reproducibility.
 *
 * ## Commit hash
 *
 * The build script exports `BIOSEQ_GIT_COMMIT` so the binary can stamp
 * the current HEAD into the JSON document. If the env var is missing
 * (e.g. running from a non-git checkout) the field is set to "unknown".
 *
 * ## Logging
 *
 * `log_configure(true)` sets fmtlog to DBG. The bench produces tens of
 * thousands of DBG lines per run (sketch dumps, per-pair traces) which
 * drown the regression-check console table. We dial it down to ERR-only
 * here so the bench output stays focused on the regression result and
 * the per-bucket progress lines (those go through stderr, not fmtlog).
 *
 * ## Env var overrides (for advanced users / one-off runs)
 *
 *    BIOSEQ_BENCH_REPORT_PATH         — exact output path (skips numbering)
 *    BIOSEQ_BENCH_REGRESSION_THRESHOLDS — thresholds JSON path
 *    BIOSEQ_GIT_COMMIT                — commit hash to embed (otherwise "unknown")
 *    BIOSEQ_BENCH_THREADS             — worker threads (default cpu/2)
 *    BIOSEQ_BENCH_SAMPLES_MULTIPLIER  — sample-count multiplier (default 20)
 *    BIOSEQ_BENCH_PER_CALL_BUDGET_S   — per-pair wall-clock budget (default 600s)
 */

namespace {

/// Pick the next free `report_build_<N>.json` in the working directory.
/// Walks the cwd once, parses any existing `report_build_<N>.json`
/// filenames, returns N+1 (or 1 if none exist).
[[nodiscard]] std::string next_report_path(const std::filesystem::path& dir) {
    static const std::regex pattern{R"(^report_build_(\d+)\.json$)"};
    long long max_index = 0;
    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            std::smatch match;
            if (!std::regex_match(name, match, pattern)) continue;
            try {
                const long long idx = std::stoll(match[1].str());
                if (idx > max_index) max_index = idx;
            } catch (...) {
                // Skip malformed indices silently — they're not ours to interpret.
            }
        }
    }
    return (dir / ("report_build_" + std::to_string(max_index + 1) + ".json")).string();
}

}  // namespace

int main( int argc, char* argv[] ) {
    // Silence DBG/INF/WRN noise during bench so the regression-check
    // table at the end is the loudest thing on stderr. The build
    // script sets BIOSEQ_BENCH_LOG_LEVEL=verbose to undo this for
    // debugging — keep this knob undocumented in production guides.
    log_configure(true);
    if (const char* env = std::getenv("BIOSEQ_BENCH_LOG_LEVEL");
        env == nullptr || std::string{env} != "verbose") {
        fmtlog::setLogLevel(fmtlog::ERR);
    }

    Catch::Session session; // There must be exactly one instance

    int returnCode = session.applyCommandLine( argc, argv );
    if( returnCode != 0 ) // Indicates a command line error
        return returnCode;

    returnCode = session.run();

    // Run the regression check before writing the report so we can
    // include the violations array directly in the document. Reading
    // the same accumulator the chart builder reads keeps the two views
    // in sync — the JSON consumer and the console reader see the same
    // numbers.
    const std::string thresholds_path =
        bioseq::bench::resolve_thresholds_path();
    const auto regression_config =
        bioseq::bench::load_config_from_file(thresholds_path);
    const auto violations = bioseq::bench::compute_violations(
        bioseq::bench::benchmark_samples(), regression_config);

    // Resolve output filename. BIOSEQ_BENCH_REPORT_PATH is an explicit
    // override (used by the existing CI / dev scripts that pin the
    // path); otherwise we pick the next `report_build_<N>.json` in cwd.
    std::string report_path;
    if (const char* env_path = std::getenv("BIOSEQ_BENCH_REPORT_PATH");
        env_path != nullptr && env_path[0] != '\0') {
        report_path = env_path;
    } else {
        report_path = next_report_path(std::filesystem::current_path());
    }

    const char* commit_env = std::getenv("BIOSEQ_GIT_COMMIT");
    const std::string commit_hash =
        commit_env != nullptr && commit_env[0] != '\0' ? commit_env : "unknown";

    if (!bioseq::bench::write_report_json_to_file(
            report_path, violations, commit_hash)) {
        std::fprintf(stderr,
                     "[bench_main] failed to write report to %s\n",
                     report_path.c_str());
    } else {
        std::fprintf(stderr,
                     "[bench_main] wrote report to %s (commit %s)\n",
                     report_path.c_str(), commit_hash.c_str());
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
