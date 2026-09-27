/**
 * @file bioseq_caching/bench/standalone/index.cppm
 * @brief Standalone cache benchmark process API.
 */

export module bioseq.cache.benchmark.standalone;

import bioseq.cache.benchmark.common;

export namespace bioseq::cache_benchmark {

/** Run the standalone cache benchmark against its fixed scenario. */
int run_standalone_cache_benchmark(int argc, char** argv) noexcept {
    return run_benchmark(argc, argv, scenario::standalone);
}

}  // namespace bioseq::cache_benchmark
