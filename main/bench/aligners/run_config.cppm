/**
 * @file run_config.cppm
 * @brief Aligner-bench-specific run knobs.
 *
 * Generic CLI fields (`threads`, `fast_mode`, `per_call_budget_s`)
 * live in `bench.scheduler.run_config::CoreRunConfig`. This module
 * carries only the aligner-domain decisions that don't make sense
 * for any other harness:
 *
 *   * `SampleProfile` — per-length-bucket sample count policy.
 *     `DEFAULT` keeps the regression-grade sweep (~±1% noise floor);
 *     `FAST` cuts every count by ~3-4× (~±3% noise floor) and is the
 *     right setting for "I just need a smoke signal".
 *   * `include_kw_sweep` — gates the (k, w) Cartesian sweep on top
 *     of the standard length × error matrix.
 *   * `include_noisy_long` — opts back in to the ONT_LEGACY /
 *     HIGH_INDEL_LOW_SUB cells on LONG / VERY_LONG / XL / XXL
 *     buckets that are otherwise skipped because even minimap2
 *     cannot recover a primary in that regime.
 *
 * Threading: same rules as `CoreRunConfig`. `aligner_run_config()` is
 * populated once in `bench_main` before workers spawn and is then
 * read-only across threads.
 */
module;

#include <cstdint>

export module bench.aligners.run_config;

export {

namespace bioseq::bench {

/**
 * @brief Policy applied to per-bucket sample counts.
 *
 * `DEFAULT` keeps the bench's standard sweep — generous samples on
 * the cheap buckets so the noise floor is around ±1%, single sample
 * on XXL so a 50-150 Mbp pair doesn't dominate wall-clock for hours.
 *
 * `FAST` cuts every count by ~3-4×; the noise floor moves to ~±3%
 * but the whole sweep finishes in roughly a third of the time. The
 * matching scheduler-level decision (saturate worker threads) lives
 * in `bench_scheduler::CoreRunConfig::fast_mode`.
 */
enum class SampleProfile : std::uint8_t {
    DEFAULT = 0,
    FAST    = 1,
};

/**
 * @brief Aligner-bench knobs. Composes with `CoreRunConfig` —
 *        the runner reads `threads` / `per_call_budget_s` from the
 *        scheduler-side singleton, and the aligner-specific
 *        sample-profile decisions from this one.
 */
struct AlignerRunConfig {
    /// `--everything`: include the (k, w) Cartesian sweep on top of
    /// the standard length × error matrix. Defaults false because
    /// the sweep adds ~30 algorithm rows × 6 pairs × ~50 samples to
    /// the MEDIUM/CCS bucket, and most performance investigations do
    /// not vary k or w.
    bool include_kw_sweep = false;

    /// `--everything`: also run the noisy×long combinations
    /// (ONT_LEGACY / HIGH_INDEL_LOW_SUB on LONG and beyond) that are
    /// otherwise skipped because minimap2 itself cannot recover a
    /// primary there.
    bool include_noisy_long = false;

    /// Sample-count policy — DEFAULT for regression-grade runs, FAST
    /// for iteration. The CLI flag `--fast` flips this here AND
    /// flips `bench_scheduler::CoreRunConfig::fast_mode` in
    /// `bench_main`.
    SampleProfile sample_profile = SampleProfile::DEFAULT;
};

/**
 * @brief Process-wide accessor. Initialised on first call to defaults.
 */
[[nodiscard]] inline AlignerRunConfig& aligner_run_config() {
    static AlignerRunConfig cfg{};
    return cfg;
}

}  // namespace bioseq::bench

}  // export
