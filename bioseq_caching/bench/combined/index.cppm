/**
 * @file bioseq_caching/bench/combined/index.cppm
 * @brief Combined Bioseq and cache benchmark process API.
 */

export module bioseq.cache.benchmark.combined;

import bioseq.cache.benchmark.common;

export namespace bioseq::cache_benchmark {

/** Run the combined Bioseq and cache benchmark against its fixed scenario. */
int run_combined_cache_benchmark(int argc, char** argv) noexcept {
    return run_benchmark(argc, argv, scenario::bioseq_2gbp);
}

}  // namespace bioseq::cache_benchmark
