/**
 * @file admin/index_sketch_backend_policy.cppm
 * @brief Resolves the persisted minimizer producer selected by active GUCs.
 */
export module bioseq.postgres.storage.indexing.admin.index_sketch_backend_policy;

import bioseq.pipeline.runtime_contract;
import bioseq.postgres.configuration.aligner;
import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::storage::indexing {

// Select the target index sketch backend from the minimizer producer itself.
// Persisted minimizer semantics do not depend on the later chaining/alignment
// choices: changing either stage must not silently relabel an explicitly
// selected exact SIMD or bounded_nthash_simd index as scalar.
/**
 * @brief Resolve the exact persisted minimizer producer selected by current GUCs.
 * @return The producer backend whose identity is persisted with the generation.
 */
[[nodiscard]] inline bioseq::minimizers::minimizer_sketch_backend
resolve_index_sketch_backend_from_gucs() noexcept {
    using bioseq::minimizers::minimizer_sketch_backend;
    using bioseq::simd::minimizer_algorithm;
    if (bioseq::aligner::settings::vulkan::enabled &&
        bioseq::aligner::settings::vulkan::index_build::enabled &&
        static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value()) ==
            static_cast<int>(minimizer_algorithm::vulkan_minimap_integer)) {
        return minimizer_sketch_backend::vulkan_segment;
    }
    if (static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value()) ==
        static_cast<int>(minimizer_algorithm::two_stack_nthash)) {
        return minimizer_sketch_backend::exact_simd;
    }
    if (static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value()) ==
        static_cast<int>(minimizer_algorithm::bounded_nthash_simd)) {
        return minimizer_sketch_backend::bounded_nthash_simd;
    }
    return minimizer_sketch_backend::scalar;
}

} // namespace bioseq::storage::indexing
