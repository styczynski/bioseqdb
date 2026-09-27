/**
 * @file pipeline/minimizer_execution_contract.cppm
 * @brief Selects minimizer layouts and checks which algorithm produced a sketch.
 */
export module bioseq.pipeline.minimizer_execution_contract;

import std;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.indexing.type;
import bioseq.sequences;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.minimizer_physical_registry;
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
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    /// The checked reference SIMD call finished successfully.
    bool checked_reference_simd_bridge_completed = false;
#endif
};

/// Select how exact_simd visits minimizer windows. For DNA4 with k=15, w=10
/// and 32-bit input words, resident vector streams and chronological
/// continuation can choose different seeds.
///
/// Index lookups require continuation to match the stored index. Resident
/// vector streams are allowed when the caller prepares both full sequences
/// for the same operation without a persisted index.
enum class exact_simd_range_route : std::uint8_t {
    require_continuation,
    require_resident_vector_stream,
};

/// Keep the source's packed word width unless an indexed query requires
/// the 32-bit input used by its continuation kernel.
enum class exact_simd_input_storage : std::uint8_t {
    preserve_native_width,
    require_indexed_u32,
};

/// Allow the generic scalar path when k and w have no registered layout
/// and persisted-index use is optional. Other requests use checked layouts.
[[nodiscard]] constexpr bool use_generic_raw_optional_scalar_minimizer_route(
    const minimap2_parameters& params,
    const ::bioseq::indexing::index_read_requirement read_requirement,
    const std::optional<std::uint16_t> alphabet_id = std::nullopt) noexcept {
    if (read_requirement != ::bioseq::indexing::index_read_requirement::allow ||
        params.minimizer_algorithm != minimizer_algorithm::scalar) {
        return false;
    }
    if (params.kmer_size > 0 && params.window_size > 0 &&
        static_cast<std::uint64_t>(params.kmer_size) <=
            (std::numeric_limits<std::uint16_t>::max)() &&
        static_cast<std::uint64_t>(params.window_size) <=
            (std::numeric_limits<std::uint16_t>::max)()) {
        for (const auto& profile : ::bioseq::minimizers::registered_minimizer_runtime_profiles()) {
            if (profile.requested_algorithm == minimizer_algorithm::scalar &&
                (!alphabet_id.has_value() || profile.alphabet_id == *alphabet_id) &&
                ::bioseq::minimizers::minimizer_runtime_profile_accepts_shape(
                    profile, static_cast<std::uint16_t>(params.kmer_size),
                    static_cast<std::uint16_t>(params.window_size))) {
                return false;
            }
        }
    }
    return true;
}

/// Check generic scalar support for the caller's alphabet.
template <sequence_alphabet p_alphabet>
[[nodiscard]] constexpr bool use_generic_raw_optional_scalar_minimizer_route(
    const minimap2_parameters& params,
    const ::bioseq::indexing::index_read_requirement read_requirement) noexcept {
    if constexpr (::bioseq::minimizers::minimizer_profile_has_alphabet<p_alphabet>()) {
        return use_generic_raw_optional_scalar_minimizer_route(
            params, read_requirement,
            ::bioseq::minimizers::minimizer_profile_alphabet_id<p_alphabet>());
    } else {
        return read_requirement == ::bioseq::indexing::index_read_requirement::allow &&
               params.minimizer_algorithm == minimizer_algorithm::scalar;
    }
}

/// Select a registered layout using the source's word width when possible.
/// If that width has no valid layout, try the other width. The algorithm,
/// k-mer length, and window size stay the same.
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline ::bioseq::minimizers::minimizer_physical_layout_selection
select_minimizer_execution_layout_prefer_native(
    const minimizer_algorithm requested_algorithm,
    const std::uint64_t maximum_sequence_bp,
    const std::uint16_t runtime_kmer_size,
    const std::uint16_t runtime_window_size,
    const std::uint8_t native_storage_bits) {
    const auto select_u32 = [requested_algorithm,
                             maximum_sequence_bp,
                             runtime_kmer_size,
                             runtime_window_size] {
        return ::bioseq::minimizers::select_minimizer_physical_layout<
            p_alphabet,
            std::uint32_t>(requested_algorithm,
                           maximum_sequence_bp,
                           runtime_kmer_size,
                           runtime_window_size);
    };
    const auto select_u64 = [requested_algorithm,
                             maximum_sequence_bp,
                             runtime_kmer_size,
                             runtime_window_size] {
        return ::bioseq::minimizers::select_minimizer_physical_layout<
            p_alphabet,
            std::uint64_t>(requested_algorithm,
                           maximum_sequence_bp,
                           runtime_kmer_size,
                           runtime_window_size);
    };
    if (native_storage_bits == 32u) {
        try {
            return select_u32();
        } catch (const std::invalid_argument&) {
            return select_u64();
        }
    }
    if (native_storage_bits == 64u) {
        try {
            return select_u64();
        } catch (const std::invalid_argument&) {
            return select_u32();
        }
    }
    throw std::invalid_argument{"native minimizer storage must be u32 or u64"};
}

/// Check that a record type, alphabet, k, and w match the selected layout.
template <class p_record, sequence_alphabet p_alphabet>
[[nodiscard]] inline bool minimizer_execution_layout_matches_record(
    const ::bioseq::minimizers::minimizer_physical_layout_selection& layout,
    const std::uint16_t runtime_kmer_size,
    const std::uint16_t runtime_window_size) {
    if (layout.alphabet_id !=
            ::bioseq::minimizers::minimizer_profile_alphabet_id<p_alphabet>() ||
        layout.runtime_kmer_size != runtime_kmer_size ||
        layout.runtime_window_size != runtime_window_size) {
        return false;
    }
    try {
        return ::bioseq::minimizers::visit_minimizer_physical_layout(
            layout,
            []<class p_selected_record>() {
                return std::same_as<std::remove_cvref_t<p_record>, p_selected_record>;
            });
    } catch (const std::invalid_argument&) {
        return false;
    }
}

/// Find a layout that produces minimap_sketch_data records.
/// Requesting the record's full position range keeps its 28-bit position
/// field even for short queries. Reject queries or hash layouts that do not
/// fit that record, without changing the requested algorithm, k, or w.
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline std::optional<
    ::bioseq::minimizers::minimizer_physical_layout_selection>
try_select_legacy_minimap_sketch_data_execution_layout(
    const minimizer_algorithm requested_algorithm,
    const std::uint64_t maximum_query_bp,
    const std::uint16_t runtime_kmer_size,
    const std::uint16_t runtime_window_size,
    const std::uint8_t native_storage_bits) {
    using legacy_record = ::bioseq::minimizers::minimap_sketch_data;
    constexpr auto legacy_position_extent = static_cast<std::uint64_t>(
        ::bioseq::minimizers::k_max_minimizer_sequence_length<legacy_record>);
    if (maximum_query_bp > legacy_position_extent) {
        return std::nullopt;
    }
    std::optional<::bioseq::minimizers::minimizer_physical_layout_selection> selection;
    try {
        selection = select_minimizer_execution_layout_prefer_native<p_alphabet>(
            requested_algorithm,
            legacy_position_extent,
            runtime_kmer_size,
            runtime_window_size,
            native_storage_bits);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    if (!selection ||
        !minimizer_execution_layout_matches_record<legacy_record, p_alphabet>(
            *selection, runtime_kmer_size, runtime_window_size)) {
        return std::nullopt;
    }
    return selection;
}

/// Select a minimap_sketch_data layout from a runtime DNA4 or DNA5 alphabet ID.
[[nodiscard]] inline std::optional<
    ::bioseq::minimizers::minimizer_physical_layout_selection>
try_select_legacy_minimap_sketch_data_execution_layout(
    const minimizer_algorithm requested_algorithm,
    const std::uint16_t alphabet_id,
    const std::uint64_t maximum_query_bp,
    const std::uint16_t runtime_kmer_size,
    const std::uint16_t runtime_window_size,
    const std::uint8_t native_storage_bits) {
    if (alphabet_id ==
        ::bioseq::minimizers::minimizer_profile_alphabet_id<::seq::dna4>()) {
        return try_select_legacy_minimap_sketch_data_execution_layout<::seq::dna4>(
            requested_algorithm,
            maximum_query_bp,
            runtime_kmer_size,
            runtime_window_size,
            native_storage_bits);
    }
    if (alphabet_id ==
        ::bioseq::minimizers::minimizer_profile_alphabet_id<::seq::dna5>()) {
        return try_select_legacy_minimap_sketch_data_execution_layout<::seq::dna5>(
            requested_algorithm,
            maximum_query_bp,
            runtime_kmer_size,
            runtime_window_size,
            native_storage_bits);
    }
    return std::nullopt;
}

/// Map the effective algorithm to its scalar, SIMD, or Vulkan sketch backend.
/// Pass the algorithm selected by the profile, which can differ from the
/// original request.
[[nodiscard]] constexpr std::optional<minimizer_sketch_backend>
minimizer_sketch_backend_for_effective_algorithm(
    const minimizer_algorithm effective_algorithm) noexcept {
    switch (effective_algorithm) {
    case minimizer_algorithm::scalar:
        return minimizer_sketch_backend::scalar;
    case minimizer_algorithm::simd_minimap2_reference:
    case minimizer_algorithm::two_stack_nthash:
        return minimizer_sketch_backend::exact_simd;
    case minimizer_algorithm::bounded_nthash_simd:
        return minimizer_sketch_backend::bounded_nthash_simd;
    case minimizer_algorithm::vulkan_minimap_integer:
        return minimizer_sketch_backend::vulkan_segment;
    }
    return std::nullopt;
}

[[nodiscard]] inline bool
exact_simd_stats_matches_range_route(exact_simd_range_route route,
                                     const two_stack_execution_stats& stats) noexcept {
    if (route == exact_simd_range_route::require_resident_vector_stream) {
        return stats.resident_vector_stream_kernel_calls > 0u;
    }
    // A continuation run must leave every resident-stream counter at zero.
    // Checking all fields also rejects partially filled or mixed statistics.
    return stats.resident_vector_stream_kernel_calls == 0u &&
           stats.resident_vector_stream_single_calls == 0u &&
           stats.resident_vector_stream_dual_calls == 0u &&
           stats.resident_vector_stream_nonempty_work_calls == 0u &&
           stats.resident_vector_stream_input_bp == 0u &&
           stats.resident_vector_stream_minimizers_emitted == 0u;
}

/// Check whether two valid exact_simd runs used the same seed-selection route.
/// Resident streams and chronological continuation can choose different
/// seeds, so their query and target sketches cannot be mixed for lookup.
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

/// Check that the statistics describe only the selected CPU sketch backend.
/// Other backends must have both a cleared presence flag and empty counters.
/// Vulkan uses separate statistics and is rejected here.
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
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    const bool reference_absent = !stats.checked_reference_simd_bridge_completed;
#else
    constexpr bool reference_absent = true;
#endif

    switch (backend) {
    case minimizer_sketch_backend::exact_simd:
        return scalar_absent && bounded_absent && reference_absent &&
               stats.exact_simd_available &&
               stats.exact_simd.is_internally_consistent(require_nonempty_vector_work) &&
               exact_simd_stats_matches_range_route(exact_simd_route, stats.exact_simd);
    case minimizer_sketch_backend::bounded_nthash_simd:
        return scalar_absent && exact_absent && reference_absent &&
               stats.bounded_nthash_available &&
               stats.bounded_nthash.is_internally_consistent(require_nonempty_vector_work);
    case minimizer_sketch_backend::scalar:
        return stats.scalar_available &&
               stats.scalar.is_coherent(require_nonempty_vector_work) && exact_absent &&
               bounded_absent && reference_absent;
    case minimizer_sketch_backend::vulkan_segment:
        return false;
    }
    return false;
}

/// Check execution statistics against the effective minimizer algorithm.
/// The reference SIMD wrapper checks its own counters before returning. This
/// record keeps those counters empty and, in test or benchmark builds, also
/// requires confirmation that the checked wrapper completed.
[[nodiscard]] inline bool minimizer_range_stats_matches_effective_algorithm(
    const minimizer_algorithm effective_algorithm,
    const minimizer_range_execution_stats& stats,
    const bool require_nonempty_vector_work = false,
    const exact_simd_range_route exact_simd_route =
        exact_simd_range_route::require_continuation) noexcept {
    if (effective_algorithm == minimizer_algorithm::simd_minimap2_reference) {
        const bool other_families_absent =
            !stats.scalar_available &&
            stats.scalar == scalar_minimizer_execution_stats{} &&
            !stats.exact_simd_available &&
            stats.exact_simd == two_stack_execution_stats{} &&
            !stats.bounded_nthash_available &&
            stats.bounded_nthash == bounded_nthash_execution_stats{};
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        return other_families_absent && stats.checked_reference_simd_bridge_completed;
#else
        return other_families_absent;
#endif
    }
    const auto backend =
        minimizer_sketch_backend_for_effective_algorithm(effective_algorithm);
    return backend.has_value() &&
           minimizer_range_stats_matches_backend(*backend,
                                                 stats,
                                                 require_nonempty_vector_work,
                                                 exact_simd_route);
}

// Choose target sketching from the minimizer setting. Chaining and
// alignment settings do not affect this choice. The generated-layout path
// uses minimizer_sketch_backend_for_effective_algorithm instead.
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
    // This older entry point has no generated layout to distinguish the
    // reference SIMD kernel from two-stack SIMD. Keep the reference request
    // on its scalar path here.
    return minimizer_sketch_backend::scalar;
}

} // namespace bioseq::simd::pipe
