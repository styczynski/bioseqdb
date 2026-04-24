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
int main(int /*argc*/, char** /*argv*/) {
    return 0;
}
