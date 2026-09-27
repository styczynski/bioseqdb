/**
 * @file bioseq_caching/bench/combined/index.cppm
 * @brief Entry point for benchmarking Bioseq alignment and result caching.
 */

export module bioseq.cache.benchmark.combined;

import bioseq.cache.benchmark.common;

export namespace bioseq::cache_benchmark {

/** Runs the two-billion-base Bioseq cache benchmark. */
int run_combined_cache_benchmark(int argc, char** argv) noexcept {
    return run_benchmark(argc, argv, scenario::bioseq_2gbp);
}

}  // namespace bioseq::cache_benchmark
