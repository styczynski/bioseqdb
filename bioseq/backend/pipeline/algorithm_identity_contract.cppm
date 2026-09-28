/**
 * @file
 * @brief Names used to display the selected pipeline algorithms.
 */
export module bioseq.pipeline.algorithm_identity_contract;

import bioseq.pipeline.runtime_contract;

export namespace bioseq::simd {

/// Return the chaining algorithm label, or "unknown" for an invalid value
[[nodiscard]] constexpr const char* chaining_algorithm_name(chaining_algorithm mode) {
    switch (mode) {
    case chaining_algorithm::scalar_minimap2:
        return "Minimap2 Scalar";
    case chaining_algorithm::simd_minimap2:
        return "Minimap2 SIMD";
    case chaining_algorithm::range_minimum_query:
        return "Minimap2 RMQ";
    case chaining_algorithm::vulkan_tiled_dynamic_programming:
        return "Vulkan tiled DP";
    }
    return "unknown";
}

/// Return the base aligner label, or "unknown" for  an invalid value.
[[nodiscard]] constexpr const char*
base_alignment_algorithm_name(base_alignment_algorithm mode) {
    switch (mode) {
    case base_alignment_algorithm::ksw2:
        return "KSW2";
    case base_alignment_algorithm::block_aligner:
        return "Block Aligner";
    case base_alignment_algorithm::ksw2_batched:
        return "KSW2 (inter-region batched)";
    }
    return "unknown";
}

/// Return the minimizer algorithm label, or "unknown" for an invalid value.
[[nodiscard]] constexpr const char* minimizer_algorithm_name(minimizer_algorithm mode) {
    switch (mode) {
    case minimizer_algorithm::scalar:
        return "Scalar Minimizers";
    case minimizer_algorithm::simd_minimap2_reference:
        return "SIMD Minimap2 (slow reference)";
    case minimizer_algorithm::two_stack_nthash:
        return "Two-Stack nthash SIMD Minimizers";
    case minimizer_algorithm::vulkan_minimap_integer:
        return "Vulkan minimap-int minimizers";
    case minimizer_algorithm::bounded_nthash_simd:
        return "Bounded u32 ntHash SIMD Minimizers";
    }
    return "unknown";
}

} // namespace bioseq::simd
