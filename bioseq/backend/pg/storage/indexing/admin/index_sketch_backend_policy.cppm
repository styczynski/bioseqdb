/**
 * @file admin/index_sketch_backend_policy.cppm
 * @brief Choose the minimizer producer recorded in a stored index.
 */
export module bioseq.postgres.storage.indexing.admin.index_sketch_backend_policy;

import bioseq.pipeline.runtime_contract;
import bioseq.postgres.configuration.aligner;
import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::storage::indexing {

// Map an algorithm to its index producer. Builds with a source identity pass
// the effective algorithm selected for that source. Callers without a source
// identity pass the requested setting.
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
        // Keep Vulkan as the producer identity. Runtime preparation checks whether
        // the requested GPU build can run.
        return minimizer_sketch_backend::vulkan_segment;
    }
    // Unknown enum values use the scalar backend. The GUC parser normally
    // rejects them before this function is called.
    return minimizer_sketch_backend::scalar;
}

static_assert(resolve_index_sketch_backend(
                  bioseq::simd::minimizer_algorithm::simd_minimap2_reference) ==
              bioseq::minimizers::minimizer_sketch_backend::exact_simd);
static_assert(resolve_index_sketch_backend(
                  bioseq::simd::minimizer_algorithm::vulkan_minimap_integer) ==
              bioseq::minimizers::minimizer_sketch_backend::vulkan_segment);

// The SQL setting is converted by enum value. Keep each named value equal
// to its runtime counterpart so the conversion selects the intended algorithm.
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

/** Return the index producer selected by the current minimizer algorithm setting. */
[[nodiscard]] inline bioseq::minimizers::minimizer_sketch_backend
resolve_index_sketch_backend_from_gucs() noexcept {
    return resolve_index_sketch_backend(
        static_cast<bioseq::simd::minimizer_algorithm>(
            static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value())));
}

} // namespace bioseq::storage::indexing
