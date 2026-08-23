/**
 * @file pipeline/minimizer_execution_contract.cppm
 * @brief Target-neutral minimizer execution receipts and route validation.
 */
export module bioseq.pipeline.minimizer_execution_contract;

import std;
import bioseq.highway.dispatch_contract;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.pipeline.minimap_parameters_contract;

export namespace bioseq::simd {
using ::bioseq::minimizers::bounded_nthash_execution_stats;
using ::bioseq::minimizers::minimizer_sketch_backend;
using ::bioseq::minimizers::two_stack_execution_stats;
} // namespace bioseq::simd

export namespace bioseq::simd::pipe {

struct minimizer_range_execution_stats {
    scalar_minimizer_execution_stats scalar{};
    bool scalar_available = false;
    two_stack_execution_stats exact_simd{};
    bool exact_simd_available = false;
    bounded_nthash_execution_stats bounded_nthash{};
    bool bounded_nthash_available = false;
};

/// exact_simd has two independently valid, but biologically incompatible,
/// minimizer-selection routes for the p_window_size=10/p_kmer_size=15 dna4/u32 shape:
///
/// * the resident vector_stream route.
/// * the chronological continuation/B8 route used by persisted and fragmented
///   exact_simd index producers.
///
/// A query cannot select between them from its own storage shape.  Indexed
/// consumers must therefore require the continuation route explicitly. Only a
/// caller that owns both full-resident sides of one transient/no-index
/// operation may opt into the resident route.
enum class exact_simd_range_route : std::uint8_t {
    require_continuation,
    require_resident_vector_stream,
};

/// Preserve native-width topology by default. Persisted indexed-query
/// preparation is the narrow exception: its continuation/B8 storage
/// contract requires an exact u32 input packing.
enum class exact_simd_input_storage : std::uint8_t {
    preserve_native_width,
    require_indexed_u32,
};

[[nodiscard]] inline bool
exact_simd_stats_matches_range_route(exact_simd_range_route route,
                                     const two_stack_execution_stats& stats) noexcept {
    if (route == exact_simd_range_route::require_resident_vector_stream) {
        return stats.resident_vector_stream_kernel_calls > 0u;
    }
    // Check every resident-only field, rather than only the call count, so a
    // partially-populated or mixed stats also fails closed.
    return stats.resident_vector_stream_kernel_calls == 0u &&
           stats.resident_vector_stream_single_calls == 0u &&
           stats.resident_vector_stream_dual_calls == 0u &&
           stats.resident_vector_stream_nonempty_work_calls == 0u &&
           stats.resident_vector_stream_input_bp == 0u &&
           stats.resident_vector_stream_minimizers_emitted == 0u;
}

/// Compare the biological selection sub-route actually attested by two
/// exact_simd operations. This is required even when both operations carry the
/// same high-level exact_simd backend label: resident vector_stream and
/// continuation/B8 stats are not lookup-compatible.
[[nodiscard]] inline bool
exact_simd_stats_share_selection_route(const two_stack_execution_stats& lhs,
                                       const two_stack_execution_stats& rhs) noexcept {
    if (!lhs.is_internally_consistent(/*require_nonempty_vector_work=*/false) ||
        !rhs.is_internally_consistent(/*require_nonempty_vector_work=*/false)) {
        return false;
    }
    const bool lhs_resident = lhs.resident_vector_stream_kernel_calls > 0u;
    const bool rhs_resident = rhs.resident_vector_stream_kernel_calls > 0u;
    return lhs_resident == rhs_resident;
}

/// Validate the complete range-stats payload against the selected CPU sketch
/// backend.  Presence bits are not sufficient validation on their own: a
/// non-default stats object with a cleared bit is rejected as hidden
/// cross-route stats object. Vulkan is deliberately not accepted here because
/// this stats type carries only CPU/simd operation stats.
[[nodiscard]] inline bool minimizer_range_stats_matches_backend(
    minimizer_sketch_backend backend,
    const minimizer_range_execution_stats& stats,
    bool require_nonempty_vector_work = false,
    exact_simd_range_route exact_simd_route =
        exact_simd_range_route::require_continuation) noexcept {
    const bool scalar_absent =
        !stats.scalar_available && stats.scalar == scalar_minimizer_execution_stats{};
    const bool exact_absent =
        !stats.exact_simd_available && stats.exact_simd == two_stack_execution_stats{};
    const bool bounded_absent = !stats.bounded_nthash_available &&
                                stats.bounded_nthash == bounded_nthash_execution_stats{};

    switch (backend) {
    case minimizer_sketch_backend::exact_simd:
        return scalar_absent && bounded_absent && stats.exact_simd_available &&
               stats.exact_simd.is_internally_consistent(require_nonempty_vector_work) &&
               exact_simd_stats_matches_range_route(exact_simd_route, stats.exact_simd);
    case minimizer_sketch_backend::bounded_nthash_simd:
        return scalar_absent && exact_absent && stats.bounded_nthash_available &&
               stats.bounded_nthash.is_internally_consistent(require_nonempty_vector_work);
    case minimizer_sketch_backend::scalar:
        return stats.scalar_available &&
               stats.scalar.is_coherent(require_nonempty_vector_work) && exact_absent &&
               bounded_absent;
    case minimizer_sketch_backend::vulkan_segment:
        return false;
    }
    return false;
}

// Resolve the biological target-sketch backend from the minimizer selector
// itself. Chaining and alignment choices must not silently substitute scalar
// sketch semantics for an explicitly selected exact_simd/bounded ntHash producer.
// Unsupported simd shapes fail in their checked drivers.
[[nodiscard]] inline minimizer_sketch_backend
target_index_sketch_backend_from_params(const minimap2_parameters& params) noexcept {
    if (params.minimizer_algorithm == minimizer_algorithm::bounded_nthash_simd) {
        return minimizer_sketch_backend::bounded_nthash_simd;
    }
    if (params.vulkan_enabled && params.vulkan_gpu_index_build_enabled &&
        params.minimizer_algorithm == minimizer_algorithm::vulkan_minimap_integer) {
        return minimizer_sketch_backend::vulkan_segment;
    }
    if (params.minimizer_algorithm == minimizer_algorithm::two_stack_nthash) {
        return minimizer_sketch_backend::exact_simd;
    }
    return minimizer_sketch_backend::scalar;
}

} // namespace bioseq::simd::pipe
