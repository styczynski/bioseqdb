/**
 * @file admin/index_sketch_backend_policy.cppm
 * @brief Resolves the persisted minimizer producer selected by active GUCs.
 */
export module bioseq.postgres.storage.indexing.admin.index_sketch_backend_policy;

import bioseq.pipeline.runtime_contract;
import bioseq.postgres.configuration.aligner;
import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::storage::indexing {

// Select the target index sketch backend from one algorithm value. READY
// callers first select the generated identity for the source and pass its
// effective algorithm; legacy callers without an identity pass the requested
// algorithm. Persisted minimizer semantics do not depend on later stages.
[[nodiscard]] constexpr bioseq::minimizers::minimizer_sketch_backend
resolve_index_sketch_backend(
    const bioseq::simd::minimizer_algorithm algorithm) noexcept {
    using bioseq::minimizers::minimizer_sketch_backend;
    using bioseq::simd::minimizer_algorithm;
    switch (algorithm) {
    case minimizer_algorithm::scalar:
        return minimizer_sketch_backend::scalar;
    case minimizer_algorithm::simd_minimap2_reference:
    case minimizer_algorithm::two_stack_nthash:
        return minimizer_sketch_backend::exact_simd;
    case minimizer_algorithm::bounded_nthash_simd:
        return minimizer_sketch_backend::bounded_nthash_simd;
    case minimizer_algorithm::vulkan_minimap_integer:
        // Availability is checked by ensure_vulkan_index_build_runtime_ready
        // before generation.  Never relabel a disabled Vulkan request as a
        // scalar producer, because that would publish a false owner identity.
        return minimizer_sketch_backend::vulkan_segment;
    }
    // The GUC parser owns enum validation. Preserve the historical fail-safe
    // scalar route if a future value reaches this noexcept policy boundary.
    return minimizer_sketch_backend::scalar;
}

static_assert(resolve_index_sketch_backend(
                  bioseq::simd::minimizer_algorithm::simd_minimap2_reference) ==
              bioseq::minimizers::minimizer_sketch_backend::exact_simd);
static_assert(resolve_index_sketch_backend(
                  bioseq::simd::minimizer_algorithm::vulkan_minimap_integer) ==
              bioseq::minimizers::minimizer_sketch_backend::vulkan_segment);

// The existing SQL boundary converts the GUC enum by its wire ordinal. Check
// every named choice so this conversion cannot silently select another kernel.
static_assert(static_cast<int>(bioseq::aligner::minimizer_algorithm::scalar) ==
              static_cast<int>(bioseq::simd::minimizer_algorithm::scalar));
static_assert(static_cast<int>(bioseq::aligner::minimizer_algorithm::simd_minimap2_reference) ==
              static_cast<int>(bioseq::simd::minimizer_algorithm::simd_minimap2_reference));
static_assert(static_cast<int>(bioseq::aligner::minimizer_algorithm::two_stack_nthash) ==
              static_cast<int>(bioseq::simd::minimizer_algorithm::two_stack_nthash));
static_assert(static_cast<int>(bioseq::aligner::minimizer_algorithm::vulkan_minimap_integer) ==
              static_cast<int>(bioseq::simd::minimizer_algorithm::vulkan_minimap_integer));
static_assert(static_cast<int>(bioseq::aligner::minimizer_algorithm::bounded_nthash_simd) ==
              static_cast<int>(bioseq::simd::minimizer_algorithm::bounded_nthash_simd));

/**
 * @brief Resolve the exact persisted minimizer producer selected by current GUCs.
 * @return The producer backend whose identity is persisted with the generation.
 */
[[nodiscard]] inline bioseq::minimizers::minimizer_sketch_backend
resolve_index_sketch_backend_from_gucs() noexcept {
    return resolve_index_sketch_backend(
        static_cast<bioseq::simd::minimizer_algorithm>(
            static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value())));
}

} // namespace bioseq::storage::indexing
