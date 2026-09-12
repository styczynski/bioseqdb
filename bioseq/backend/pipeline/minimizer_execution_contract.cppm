/**
 * @file pipeline/minimizer_execution_contract.cppm
 * @brief Target-neutral minimizer execution receipts and route validation.
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
    /// Private proof token set only after the checked reference bridge returns.
    bool checked_reference_simd_bridge_completed = false;
#endif
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

/// Public scalar K/W values outside generated capabilities retain the
/// pre-existing generic raw pipeline when persisted-index use is optional.
/// Explicit no-index and required-persisted contracts remain generated and
/// fail closed, as do every non-scalar algorithm request.
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

/// Typed callers must not use another alphabet's registered shape domain.
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

/// Resolve one generated execution layout, retaining the source packing when
/// that exact profile exists.  The alternate word width is consulted only
/// when the generated registry has no native-width binding (notably DNA4
/// minimap2-reference k20, whose existing kernel consumes u64 storage).
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

/// Authenticate a concrete record against the generated selection rather
/// than independently deriving its physical L/K representation.
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

/// Select the exact generated layout required by the legacy
/// minimap_sketch_data destination.  Asking the registry for the record's full
/// position extent deliberately selects its L28 band for short queries; the
/// exact record visitor below still rejects incompatible K/hash layouts such as
/// scalar minimap K20.  Requested algorithm and runtime K/W are never changed.
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

/// Runtime-alphabet form of the explicit legacy-destination policy.
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

/// Resolve a host/GPU sketch backend from an authenticated effective
/// algorithm. Registered profiles determine the effective backend independently
/// of the preserved requested identity, including legacy scalar dispositions.
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

/// Validate the public range payload against an authenticated effective
/// algorithm. The existing reference-index wrapper validates its typed kernel
/// receipt before returning; reference counters remain deliberately absent
/// from this public pipeline payload because ordinary builds do not populate
/// its TEST_INDEXES-only vector counters.
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
    // This legacy facade carries no generated execution layout. Keep
    // reference on scalar here so its shared exact_simd transport slot can
    // never be mistaken for the two-stack kernel. Layout-aware callers use
    // minimizer_sketch_backend_for_effective_algorithm instead.
    return minimizer_sketch_backend::scalar;
}

} // namespace bioseq::simd::pipe
