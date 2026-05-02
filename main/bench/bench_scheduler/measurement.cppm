/**
 * @file measurement.cppm
 * @brief Single-thread timed-sample loop with RSS sampling.
 *
 * Pulled out of `bench/aligners/matrix_runner.cppm` so the
 * domain-specific code never has to touch `std::chrono`,
 * `mach_task_basic_info`, or `/proc/self/statm`. Domain executors
 * call `collect_timed_samples(N, [&]{ run_one_sample(); })` and
 * receive a populated `TimedSamplesOutcome` — sorted latencies plus
 * the loop-mean RSS delta replicated per slot.
 *
 * Why one RSS reading per loop, not per sample:
 *
 *   `task_info` (macOS) and `/proc/self/statm` (Linux) cost ~µs per
 *   call. Sampling RSS inside the per-sample t0..t1 window would add
 *   noise to latency measurements; sampling outside but per sample
 *   doubles the number of syscalls. The existing aligner bench
 *   replicates the loop-mean delta across every sample slot so the
 *   chart layer keeps its "sample_peak_rss_delta_bytes" column with
 *   `n_samples` entries — preserve that exact shape so report
 *   consumers don't notice the move.
 *
 * Wall-clock comes from `std::chrono::steady_clock`, the only clock
 * the project uses for performance measurement (CODE_STYLE.md doesn't
 * single it out, but every other bench helper in the project uses
 * steady_clock — keep the convention here).
 */
module;

#include "common.h"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_init.h>
#include <mach/task.h>
#include <mach/task_info.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

export module bench.scheduler.measurement;

export {

namespace bioseq::bench_scheduler {

/**
 * @brief Output of `collect_timed_samples`.
 *
 * Both vectors have length `n_samples`. `sample_latencies_ns` is
 * sorted ascending so percentile reads on the chart side are O(1).
 * `sample_peak_rss_delta_bytes` is the loop-mean RSS delta
 * (positive only) replicated per slot — see file header for why.
 */
struct TimedSamplesOutcome {
    std::vector<double> sample_latencies_ns;
    std::vector<double> sample_peak_rss_delta_bytes;
};

/**
 * @brief Return the *current* process resident-set-size in bytes.
 *
 * Live value, not the lifetime peak (the previous bench used
 * `getrusage(RUSAGE_SELF).ru_maxrss` and got monotonic readings;
 * once any sample pushed the high-water mark every later delta read
 * 0). On hosts the function can't query (Windows, exotic Unix) it
 * returns 0; the chart layer treats all-zero RSS columns as "RSS
 * not available — hide the trace".
 */
[[nodiscard]] inline std::uint64_t current_resident_bytes() noexcept {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                    reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::uint64_t>(info.resident_size);
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return 0;
    long size_pages = 0, resident_pages = 0;
    if (std::fscanf(f, "%ld %ld", &size_pages, &resident_pages) != 2) {
        std::fclose(f);
        return 0;
    }
    std::fclose(f);
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return 0;
    return static_cast<std::uint64_t>(resident_pages) *
           static_cast<std::uint64_t>(page_size);
#else
    return 0;
#endif
}

/**
 * @brief Single-shot wall-clock timing of a callable.
 *
 * Returns the elapsed time in nanoseconds. Domain code that needs
 * to time *one* call (a curated `[simple-benchmark]` case, a
 * one-shot eval invocation) should use this instead of reaching for
 * `<chrono>`. Keeps the convention "every clock read in the bench
 * runs through `bench_scheduler::measurement`" so any future timer
 * upgrade (e.g. switching to `clock_gettime(CLOCK_MONOTONIC_RAW)`)
 * is a single-file change.
 */
template<std::invocable Fn>
[[nodiscard]] inline double measure_elapsed_ns(Fn&& fn) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    fn();
    const auto t1 = clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

/**
 * @brief Run `n_samples` invocations of `sample`, measure latency
 *        per sample, take one RSS reading around the whole loop.
 *
 * The caller is expected to do a *separate* warm-up invocation
 * outside this helper so cold-allocator first-call costs don't
 * pollute the percentile estimate. We do not run a warm-up here
 * because the caller often needs the warm-up's *output* (e.g. the
 * primary alignment's correctness flags) and doing it inside would
 * either swallow that output or force a callback contract.
 *
 * Returns immediately if `n_samples == 0` (empty vectors,
 * caller-distinguishable from a successful zero-sample run by
 * inspecting the vectors' sizes).
 */
template<std::invocable SampleFn>
[[nodiscard]] inline TimedSamplesOutcome collect_timed_samples(
    std::size_t n_samples, SampleFn&& sample)
{
    using clock = std::chrono::steady_clock;

    TimedSamplesOutcome out;
    if (n_samples == 0) return out;
    out.sample_latencies_ns.reserve(n_samples);

    // Memory-tracking syscalls are kept *outside* the per-sample
    // t0..t1 window so they don't pollute latency measurements.
    const auto rss_baseline = current_resident_bytes();
    for (std::size_t sample_index = 0; sample_index < n_samples; ++sample_index) {
        const auto t0 = clock::now();
        sample();
        const auto t1 = clock::now();
        const double elapsed_ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count();
        out.sample_latencies_ns.push_back(elapsed_ns);
    }
    const auto rss_after = current_resident_bytes();
    const std::int64_t rss_delta_bytes =
        static_cast<std::int64_t>(rss_after) -
        static_cast<std::int64_t>(rss_baseline);
    // Negative deltas (allocator returned memory between calls) are
    // clamped to zero so the chart axis stays positive.
    const double mean_rss_delta = (rss_delta_bytes > 0 && n_samples > 0)
        ? static_cast<double>(rss_delta_bytes) / static_cast<double>(n_samples)
        : 0.0;
    out.sample_peak_rss_delta_bytes.assign(n_samples, mean_rss_delta);

    // Sort once so percentile reads on the chart side are O(1).
    // Only the latency vector needs sorting — the RSS column is
    // uniform by construction (loop-mean replicated per slot), so
    // the sort would be a no-op `O(n log n)` cost.
    std::sort(out.sample_latencies_ns.begin(),
              out.sample_latencies_ns.end());
    return out;
}

}  // namespace bioseq::bench_scheduler

}  // export
