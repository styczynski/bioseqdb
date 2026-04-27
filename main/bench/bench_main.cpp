#include "common_tests.h"
#include "common.h"

#include <catch2/catch_session.hpp>
#include <cstdint>
#include <string>
#include <vector>

import mlogging;
import testing;

/**
 * @file bench_main.cpp
 * @brief Minimal entry point for the non-test bench executable.
 *
 * The bench library is exercised via Catch2 cases in bench_test.cpp;
 * this file only exists because the build system spawns a companion
 * non-test executable alongside the test binary and needs a main() to
 * link. It intentionally does nothing — run the real benchmarks via
 *
 *    ./bioseq/bioseq_aligner_tests_exec [benchmark-evaluation]
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

    // returnCode encodes the type of error that occurred. See the
    // integer constants in catch_session.hpp for more information
    // on what each return code means.

    return returnCode;
}