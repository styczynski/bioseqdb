/**
 * @file effective_minimap_parameters.cppm
 * @brief Adjust default minimizer and chaining settings for short sequence pairs.
 */
module;

#include <cstddef>

export module bioseq.postgres.scheduler.dispatch_processor.effective_minimap_parameters;

import bioseq.pipeline.minimap_port;

export namespace bioseq::simd {

/**
 * @brief Use smaller minimizer and chaining thresholds for short sequence pairs.
 * Apply this adjustment only while kmer_size, window_size, min_chain_score,
 * and min_seed_count all have their default values.
 * @param[in] base Requested parameters.
 * @param[in] min_bp Length of the shorter sequence in bases.
 * @return The adjusted parameters, with other settings preserved.
 */
[[nodiscard]] inline minimap2_parameters
effective_minimap_parameters_for_pair(const minimap2_parameters& base,
                                      const std::size_t min_bp) noexcept {
    const bool all_defaults = base.kmer_size == 15 && base.window_size == 10 &&
                              base.min_chain_score == 40 && base.min_seed_count == 3;
    if (!all_defaults) {
        return base;
    }
    minimap2_parameters scaled = base;
    if (min_bp < 50) {
        scaled.kmer_size = 8;
        scaled.window_size = 3;
        scaled.min_chain_score = 10;
        scaled.min_seed_count = 1;
    } else if (min_bp < 200) {
        scaled.kmer_size = 10;
        scaled.window_size = 5;
        scaled.min_chain_score = 20;
        scaled.min_seed_count = 2;
    }
    return scaled;
}

} // namespace bioseq::simd
