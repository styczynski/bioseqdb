/**
 * @file io_bench_main.cpp
 * @brief Entry point for the bioseq_io_bench Catch2 binary.
 *
 * Mirrors `main/bench/bench_main.cpp` shape: a hand-owned `main()` that
 * drives a `Catch::Session`, with fmtlog clamped to ERR so the FASTA /
 * FASTQ test cases don't drown the output. Spec §18.1.
 *
 * For v1 we wire only the entry point + a few correctness tests; the
 * full bench matrix and regression-thresholds machinery (paralleling
 * `main/bench/regression_check.cppm`, `chart_builder.cppm`, etc.) is
 * a phase-5 follow-up.
 */
#include "common_tests.h"
#include "common.h"

#include <catch2/catch_session.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

import mlogging;
import testing;

namespace {

class IoBenchLogLevelClamp : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testCaseStarting(Catch::TestCaseInfo const&) override {
        fmtlog::setLogLevel(fmtlog::ERR);
    }
};

}  // namespace

CATCH_REGISTER_LISTENER(IoBenchLogLevelClamp)

int main(int argc, char* argv[]) {
    log_configure(true);
    fmtlog::setLogLevel(fmtlog::ERR);

    Catch::Session session;
    int rc = session.applyCommandLine(argc, argv);
    if (rc != 0) return rc;
    rc = session.run();
    return rc;
}
