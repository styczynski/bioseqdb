#include "common_tests.h"
#include "common.h"

#include <catch2/catch_session.hpp>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

import mlogging;
import testing;
import bench.chart_builder;

/**
 * @file bench_main.cpp
 * @brief Entry point for the bench executable.
 *
 * Drives Catch2's session loop (the bench TEST_CASEs in
 * bench_test.cpp populate the chart-builder accumulators while they
 * run), then — once the session returns — emits the per-chart plotly
 * report.json next to the binary.
 *
 * The output path can be overridden via the `BIOSEQ_BENCH_REPORT_PATH`
 * environment variable. Defaults to `report.json` in the current
 * working directory; the build script invokes the binary from the
 * top-level build dir so the file lands next to the other build
 * artefacts.
 *
 *    ./bioseq/bioseq_bench [chart-data]                    # produce report.json
 *    BIOSEQ_BENCH_REPORT_PATH=/tmp/r.json ./bioseq/bioseq_bench
 */
 int main( int argc, char* argv[] ) {
    log_configure(true);
    JustLog("bioseq aligner benchmarking utilities");
    Catch::Session session; // There must be exactly one instance

    // writing to session.configData() here sets defaults
    // this is the preferred way to set them

    int returnCode = session.applyCommandLine( argc, argv );
    if( returnCode != 0 ) // Indicates a command line error
        return returnCode;

    // writing to session.configData() or session.Config() here
    // overrides command line args
    // only do this if you know you need to

    returnCode = session.run();

    // Emit the plotly report after Catch2's session has finished so
    // every TEST_CASE has had a chance to populate
    // bioseq::bench::benchmark_samples(). The chart builder is a
    // no-op when the accumulator is empty (e.g. when the user runs
    // only the smoke-tagged cases), so the file is still produced.
    const char* env_path = std::getenv("BIOSEQ_BENCH_REPORT_PATH");
    const std::string report_path = env_path != nullptr ? env_path : "report.json";
    if (!bioseq::bench::write_report_json_to_file(report_path)) {
        JustLog("bench: failed to write report.json to {}", report_path);
    }

    // returnCode encodes the type of error that occurred. See the
    // integer constants in catch_session.hpp for more information
    // on what each return code means.

    return returnCode;
}