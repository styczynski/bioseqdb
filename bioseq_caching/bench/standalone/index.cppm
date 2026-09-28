/**
 * @file bioseq_caching/bench/standalone/index.cppm
 * @brief Entry point for benchmarking resulu caching.
 */

export module bioseq.cache.benchmark.standalone;

import bioseq.cache.benchmark.common;

export namespace bioseq::cache_benchmark {

/** Runs the standalone SQL cache benchmark. */
int run_standalone_cache_benchmark(int argc, char** argv) noexcept {
    return run_benchmark(argc, argv, scenario::standalone);
}

}  // namespace bioseq::cache_benchmark
