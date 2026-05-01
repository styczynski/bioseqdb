/**
 * @file run_config.cppm
 * @brief Process-wide bench run configuration.
 *
 * Holds the resolved `--everything` / `--fast` selections plus the
 * downstream tuning knobs (sample multipliers, thread count, per-call
 * budget) that used to live behind `BIOSEQ_BENCH_*` env vars. Centralising
 * them in one struct turns the "what knobs is this run using?"
 * question into a single read at the top of the bench, instead of
 * scattered `getenv` calls deep inside the matrix runner.
 *
 * Why a singleton rather than threading the config through the call
 * graph: the matrix runner is invoked from inside Catch2 TEST_CASE
 * bodies (`bench-full-matrix`, `rmq-repro`), and Catch2 doesn't give us
 * a way to plumb arbitrary state through `Session::run()`. The
 * alternative was a thread-local pointer that the bench installs before
 * `session.run()` and tears down afterwards, but the bench is a single
 * executable with one logical run per process — a function-local static
 * matches that lifecycle without ceremony.
 *
 * Threading: `bench_run_config()` is initialised once in `bench_main`
 * before the worker pool starts; from that point on every reader sees
 * a settled value. The struct is therefore read-only across threads;
 * we deliberately do NOT take a mutex on the read path. A late mutation
 * (e.g. inside a TEST_CASE) would race with workers, so the only
 * supported writer is `bench_main` and the only supported time is
 * before `session.run()`.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <thread>

export module bench.run_config;

export {

namespace bioseq::bench {

/**
 * @brief Policy applied to per-bucket sample counts.
 *
 * `DEFAULT` keeps the bench's standard sweep — generous samples on the
 * cheap buckets so the noise floor is around ±1%, single sample on
 * XXL so a 50-150 Mbp pair doesn't dominate wall-clock for hours.
 *
 * `FAST` cuts every count by ~3-4×; the noise floor moves to ~±3% but
 * the whole sweep finishes in roughly a third of the time. Combined
 * with full-hardware-concurrency thread count (rather than the cpu/2
 * default) it is the right setting for "I just need a smoke signal,
 * not a regression-grade reading".
 */
enum class SampleProfile : std::uint8_t {
    DEFAULT = 0,
    FAST    = 1,
};

/**
 * @brief Resolved bench-run configuration.
 *
 * `bench_main` populates one of these from the parsed CLI flags before
 * `Catch::Session::run()` fires. Every consumer (matrix runner,
 * bench_test TEST_CASE, regression checker) reads it via
 * `bench_run_config()`.
 */
struct BenchRunConfig {
    /// `--everything`: include the (k, w) Cartesian sweep on top of the
    /// standard length × error matrix. Defaults to false because the
    /// sweep adds ~30 algorithm rows × 6 pairs × ~50 samples to the
    /// MEDIUM/CCS bucket, and most performance investigations do not
    /// vary k/w.
    bool include_kw_sweep = false;

    /// `--fast`: reduce per-bucket sample counts and use full hardware
    /// concurrency. Picks `SampleProfile::FAST` and bumps the worker
    /// pool to `hardware_concurrency()` rather than the cpu/2 default.
    bool fast_mode = false;

    /// Number of worker threads. 0 means "derive from policy / hardware".
    std::size_t threads = 0;

    /// Per-call wall-clock budget (seconds). 0 disables the budget.
    double per_call_budget_s = 600.0;

    /// Selected sample profile — either DEFAULT or FAST.
    SampleProfile sample_profile = SampleProfile::DEFAULT;

    /// `noisy × long` cells (ONT_LEGACY / HIGH_INDEL_LOW_SUB on LONG
    /// and beyond) are skipped because even minimap2 cannot recover a
    /// primary in that regime. `--everything` flips this to true so
    /// the sweep is genuinely exhaustive.
    bool include_noisy_long = false;
};

/**
 * @brief Process-wide accessor. Initialised on first call to defaults.
 */
[[nodiscard]] inline BenchRunConfig& bench_run_config() {
    static BenchRunConfig cfg{};
    return cfg;
}

/**
 * @brief Resolve the worker-thread count from a run config.
 *
 * - DEFAULT profile: `hardware_concurrency() / 2`. Leaves the rest of
 *   the box for OS work and avoids hyperthread-induced cache contention
 *   on the tight aligner inner loops.
 * - FAST profile: full `hardware_concurrency()`. The user has opted
 *   into noisier numbers in exchange for wall-clock; saturate the box.
 *
 * Always at least 1 (the bench is the wrong place to no-op when
 * hardware_concurrency() returns 0).
 */
[[nodiscard]] inline std::size_t resolve_thread_count(const BenchRunConfig& cfg) noexcept {
    if (cfg.threads != 0) return cfg.threads;
    const auto hw = std::thread::hardware_concurrency();
    if (hw == 0) return 1;
    if (cfg.fast_mode) return hw;
    return std::max<std::size_t>(1u, hw / 2u);
}

}  // namespace bioseq::bench

}  // export
