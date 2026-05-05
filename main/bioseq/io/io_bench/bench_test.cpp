/**
 * @file bench_test.cpp
 * @brief Catch2 driver for the IO bench matrix.
 *
 * One TEST_CASE — `[io-bench-matrix]` — runs `run_io_bench_matrix(...)`
 * to walk every (format × length_bucket) cell and append samples to
 * the `io_bench_samples()` accumulator. The post-session pipeline in
 * `io_bench_main.cpp` consumes the accumulator: regression check +
 * report emission.
 *
 * The TEST_CASE asserts the accumulator is non-empty so a CI run that
 * silently skipped the matrix (e.g. a bug in the cell loop) fails
 * loudly via Catch2's normal channel rather than emitting an empty
 * report.
 *
 * `common_simd.h` is included BEFORE `common_tests.h` so the Highway
 * headers land in this TU's global module. Without that anchor every
 * imported BMI in the chain (bioseq_simd.utils, bioseq_sequences.simd,
 * bioseq_io.simd_kernels, …) would each ship its own GMF copy of
 * `hwy::N_NEON::operator==`; clang's BMI loader then trips its
 * "definition with same mangled name" check on AArch64. Same trick as
 * `bench/aligners/bench_test.cpp` and `backend/sequences/simd_test.cpp`.
 */
#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <cstdint>
#include <string_view>

import mlogging;
import bench.scheduler.run_config;
import bioseq_io_bench.matrix_runner;
import bioseq_io_bench.samples;

#ifndef BIOSEQ_IO_TEST_DATA_DIR
#error "BIOSEQ_IO_TEST_DATA_DIR must be defined by the build system"
#endif

TEST_CASE("IO bench matrix — every (format × length × reader) cell",
          "[io-bench-matrix]") {
    const bool fast_mode =
        bioseq::bench_scheduler::core_run_config().fast_mode;
    bioseq::io_bench::run_io_bench_matrix(
        fast_mode,
        /*base_seed=*/9876543210ull,
        /*test_data_dir=*/std::string_view{BIOSEQ_IO_TEST_DATA_DIR});
    REQUIRE(!bioseq::io_bench::io_bench_samples().empty());
}

TEST_CASE("IO bench writer matrix — every (format × length × writer) cell",
          "[io-bench-writer-matrix]") {
    const bool fast_mode =
        bioseq::bench_scheduler::core_run_config().fast_mode;
    bioseq::io_bench::run_writer_bench_matrix(
        fast_mode,
        /*base_seed=*/1234567890ull,
        /*test_data_dir=*/std::string_view{BIOSEQ_IO_TEST_DATA_DIR});
    REQUIRE(!bioseq::io_bench::io_bench_writer_samples().empty());
}
