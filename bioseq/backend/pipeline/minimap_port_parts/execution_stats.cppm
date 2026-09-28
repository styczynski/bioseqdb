/**
 * @file pipeline/minimap_port_parts/execution_stats.cppm
 * @brief Keep alignment results and the records used to check their execuion
 */
module;

#include <new>

export module bioseq.pipeline.minimap_port:execution_stats;

import std;
import :results;
import :alignment;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
import bioseq.alignment.contracts;
#endif
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.highway.dispatch_contract;
import bioseq.prefilter.execution_contract;
import bioseq.pipeline.minimap_parameters_contract;
import bioseq.sequences.kmers.two_stack_minimizers;
import bioseq.sequences.cigar;
import bioseq.alignment.block_aligner;
import bioseq.chaining.simd;
import bioseq.indexing.minimizer_hashmap;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
import bioseq.indexing.minimizer_sequence.generation;
import bioseq.gpu.vulkan.runtime;
import bioseq.pipeline.stats;
#endif

export namespace bioseq::simd {

// Alignment results, execution records and PAF formatting.

// The CIGAR module formats CIGAR operations for output.

/// Statistics from completed Vulkan stages in a CPU/GPU pipeline.
/// Record work after runtime success, including valid empty-input bypasses.
struct partial_vulkan_execution_stats {
    /// Sketch completion awaiting a query or target role. Move this count to
    /// query_sketch_dispatches or target_sketch_dispatches before returning results.
    std::uint64_t sketch_dispatches = 0;
    std::uint64_t query_sketch_dispatches = 0;
    std::uint64_t target_sketch_dispatches = 0;
    std::uint64_t target_index_build_dispatches = 0;
    std::uint64_t seed_lookup_dispatches = 0;
    // Count  the index representation used by completed seed lookups.
    // Accepted jobs require these counts to add up to all seed lookups.
    std::uint64_t seed_open_addressed_lookup_dispatches = 0;
    std::uint64_t seed_sorted_dense_lookup_dispatches = 0;
    std::uint64_t chain_score_dispatches = 0;
    // Copy sort counts from, the completed runtime result. Selecting a sort
    // method alone does not count as execution.
    std::uint64_t seed_bitonic_sort_dispatches = 0;
    std::uint64_t seed_radix_sort_operations = 0;
    std::uint64_t seed_radix_anchor_count = 0;
    std::uint64_t seed_radix_passes = 0;
    std::uint64_t seed_radix_histogram_dispatches = 0;
    std::uint64_t seed_radix_scan_dispatches = 0;
    std::uint64_t seed_radix_scatter_dispatches = 0;
    std::uint64_t seed_host_sort_fallbacks = 0;
    // Record anchor counts, order checks and checksums independently of
    // index storage checks. This lets different index paths compare their
    // resulting anchor streams.
    std::uint64_t seed_anchor_stats_operations = 0;
    std::uint64_t seed_canonical_anchor_count = 0;
    std::uint64_t seed_canonical_anchor_order_checks = 0;
    std::uint64_t seed_canonical_anchor_order_accepts = 0;
    std::uint64_t seed_canonical_tie_normalization_runs = 0;
    std::uint64_t seed_canonical_tie_normalization_anchors = 0;
    std::uint64_t seed_anchor_digest_lane0 = 0;
    std::uint64_t seed_anchor_digest_lane1 = 0;
    // Count target-index uploads and their memory use. Cache reuse adds no
    // upload. Keep the largest load factor and probe length  across uploads.
    std::uint64_t gpu_index_materializations = 0;
    std::uint64_t gpu_index_open_addressed_materializations = 0;
    std::uint64_t gpu_index_sorted_dense_materializations = 0;
    std::uint64_t gpu_index_unique_keys = 0;
    std::uint64_t gpu_index_slot_capacity = 0;
    std::uint64_t gpu_index_logical_resident_bytes = 0;
    std::uint64_t gpu_index_actual_allocation_bytes = 0;
    std::uint64_t gpu_index_fully_touched_bytes = 0;
    std::uint64_t gpu_index_staging_bytes = 0;
    std::uint64_t gpu_index_permit_releases = 0;
    std::uint64_t gpu_index_process_footprint_reflections = 0;
    std::uint64_t gpu_index_reflected_footprint_bytes = 0;
    std::uint64_t gpu_index_max_load_ppm = 0;
    std::uint64_t gpu_index_max_probe = 0;
    std::uint64_t gpu_index_count_admissions = 0;
    std::uint64_t gpu_index_main_peak_admissions = 0;
    std::uint64_t gpu_index_resident_transfers = 0;
    std::uint64_t gpu_index_adoption_rereserve_attempts = 0;
    std::uint64_t gpu_index_adoption_successes = 0;
    std::uint64_t gpu_index_owned_resident_bytes = 0;
    std::uint64_t gpu_index_gate_enabled = 0;
    std::uint64_t gpu_index_gate_word_count = 0;
    std::uint64_t gpu_index_gate_mask = 0;
    std::uint64_t gpu_index_gate_set_bits = 0;
    std::uint64_t gpu_index_gate_digest_lo = 0;
    std::uint64_t gpu_index_gate_digest_hi = 0;
    // Record a shared target build on one query only, so combining query
    // results counts the build once.
    std::uint64_t gpu_index_gate_count_dispatches = 0;
    std::uint64_t gpu_index_gated_emit_dispatches = 0;
    std::uint64_t gpu_index_ungated_emit_dispatches = 0;
    std::uint64_t gpu_index_exact_count_dispatches = 0;
    std::uint64_t gpu_index_exact_count_prefix_scan_dispatches = 0;
    std::uint64_t gpu_index_exact_count_control_readbacks = 0;
    std::uint64_t gpu_index_deterministic_tile_emit_dispatches = 0;
    std::uint64_t gpu_index_atomic_append_dispatches = 0;
    std::uint64_t gpu_index_emit_validation_control_readbacks = 0;
    std::uint64_t gpu_index_counted_minimizer_count = 0;
    std::uint64_t gpu_index_verified_emit_count = 0;
    std::uint64_t gpu_index_count_emit_parity_verified = 0;
    std::uint64_t gpu_index_target_minimizer_capacity_estimate = 0;
    std::uint64_t gpu_index_capacity_estimate_underestimated = 0;
    std::uint64_t gpu_index_arena_trimmed_before_permit_shrink = 0;
    std::uint64_t gpu_index_retained_minimizer_count = 0;
    std::uint64_t gpu_index_distinct_minimizer_count = 0;
    // Record completed source-direct sorted-dense index preparation. An
    // empty target can finish successfully without a GPU dispatch, so its
    // completion has separate counters.
    std::uint64_t gpu_index_source_direct_operations = 0;
    std::uint64_t gpu_index_source_direct_terminal_successes = 0;
    std::uint64_t gpu_index_source_direct_empty_bypasses = 0;
    std::uint64_t gpu_index_source_direct_zero_query_bypasses = 0;
    std::uint64_t gpu_index_source_direct_planning_receipts = 0;
    std::uint64_t gpu_index_source_direct_terminal_receipts = 0;
    std::uint64_t gpu_index_source_direct_materializations = 0;
    std::uint64_t gpu_index_source_direct_resident_publications = 0;
    std::uint64_t gpu_index_source_direct_logical_control_bytes = 0;
    std::uint64_t gpu_index_source_direct_logical_position_bytes = 0;
    std::uint64_t gpu_index_source_direct_control_requirement_bytes = 0;
    std::uint64_t gpu_index_source_direct_position_requirement_bytes = 0;
    std::uint64_t gpu_index_source_direct_allocation_request_bytes = 0;
    std::uint64_t gpu_index_source_direct_actual_control_bytes = 0;
    std::uint64_t gpu_index_source_direct_actual_position_bytes = 0;
    std::uint64_t gpu_index_source_direct_source_releases = 0;
    std::uint64_t gpu_index_source_direct_source_released_bytes = 0;
    std::uint64_t gpu_index_source_direct_checksum_matches = 0;
    std::uint64_t gpu_index_source_direct_source_traversals = 0;
    std::uint64_t gpu_index_source_direct_mapped_traversals = 0;
    std::uint64_t gpu_index_source_direct_reflected_authorities = 0;
    std::uint64_t gpu_index_source_direct_retained_authorities = 0;
    std::uint64_t gpu_index_source_direct_retained_resident_permit_bytes = 0;
    std::uint64_t gpu_index_source_direct_cache_preinsertions = 0;
    std::uint64_t gpu_index_source_direct_expected_seed_consumers = 0;
    std::uint64_t gpu_index_source_direct_completed_seed_consumers = 0;
    std::uint64_t gpu_index_source_direct_required_lookups = 0;
    std::uint64_t gpu_index_source_direct_required_hits = 0;
    std::uint64_t gpu_index_source_direct_required_misses = 0;
    std::uint64_t gpu_index_source_direct_fallback_attempts = 0;
    std::uint64_t gpu_index_source_direct_mapped_prefix_passes = 0;
    std::uint64_t gpu_index_source_direct_mapped_prefix_lookups = 0;
    std::uint64_t gpu_index_source_direct_mapped_prefix_comparisons = 0;
    std::uint64_t gpu_index_source_direct_seed_slab_requested_bytes = 0;
    std::uint64_t gpu_index_source_direct_seed_slab_initial_capacity = 0;
    std::uint64_t gpu_index_source_direct_seed_slab_actual_bytes = 0;
    std::uint64_t gpu_index_source_direct_seed_slab_retained_bytes = 0;
    std::uint64_t gpu_index_source_direct_seed_slab_permit_releases = 0;
    std::uint64_t gpu_index_source_direct_seed_reserved_bytes = 0;
    std::uint64_t gpu_index_source_direct_seed_planned_vulkan_bytes = 0;
    std::uint64_t gpu_index_source_direct_seed_actual_vulkan_bytes = 0;
    std::uint64_t gpu_index_source_direct_seed_roundtrips = 0;
    std::uint64_t gpu_index_source_direct_zero_anchor_bypasses = 0;
    std::uint64_t gpu_index_source_direct_seed_fence_completions = 0;
    std::uint64_t gpu_index_source_direct_cache_cleanup_releases = 0;
    std::uint64_t gpu_index_source_direct_resident_cleanup_bytes = 0;
    std::uint64_t gpu_index_source_direct_pipeline_completions = 0;
    // Record owned index storagf released after the last seed consumer
    // finishes and before chaining starts.
    std::uint64_t post_seed_owned_gpu_cache_evictions = 0;
    std::uint64_t post_seed_owned_gpu_resident_bytes_released = 0;
    std::uint64_t post_seed_owned_cpu_index_releases = 0;
    std::uint64_t post_seed_owned_cpu_position_bytes_released = 0;
    bool is_saturated = false;

    static constexpr void
    add(std::uint64_t& destination, std::uint64_t increment, bool& saturated_flag) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)() - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            saturated_flag = true;
            return;
        }
        destination += increment;
    }

    constexpr void merge(const partial_vulkan_execution_stats& other) noexcept {
        bool overflow = is_saturated || other.is_saturated;
        const std::uint64_t anchor_operations_before = seed_anchor_stats_operations;
        add(sketch_dispatches, other.sketch_dispatches, overflow);
        add(query_sketch_dispatches, other.query_sketch_dispatches, overflow);
        add(target_sketch_dispatches, other.target_sketch_dispatches, overflow);
        add(target_index_build_dispatches, other.target_index_build_dispatches, overflow);
        add(seed_lookup_dispatches, other.seed_lookup_dispatches, overflow);
        add(seed_open_addressed_lookup_dispatches,
            other.seed_open_addressed_lookup_dispatches,
            overflow);
        add(seed_sorted_dense_lookup_dispatches,
            other.seed_sorted_dense_lookup_dispatches,
            overflow);
        add(chain_score_dispatches, other.chain_score_dispatches, overflow);
        add(seed_bitonic_sort_dispatches, other.seed_bitonic_sort_dispatches, overflow);
        add(seed_radix_sort_operations, other.seed_radix_sort_operations, overflow);
        add(seed_radix_anchor_count, other.seed_radix_anchor_count, overflow);
        add(seed_radix_passes, other.seed_radix_passes, overflow);
        add(seed_radix_histogram_dispatches, other.seed_radix_histogram_dispatches, overflow);
        add(seed_radix_scan_dispatches, other.seed_radix_scan_dispatches, overflow);
        add(seed_radix_scatter_dispatches, other.seed_radix_scatter_dispatches, overflow);
        add(seed_host_sort_fallbacks, other.seed_host_sort_fallbacks, overflow);
        add(seed_anchor_stats_operations, other.seed_anchor_stats_operations, overflow);
        add(seed_canonical_anchor_count, other.seed_canonical_anchor_count, overflow);
        add(seed_canonical_anchor_order_checks, other.seed_canonical_anchor_order_checks, overflow);
        add(seed_canonical_anchor_order_accepts,
            other.seed_canonical_anchor_order_accepts,
            overflow);
        add(seed_canonical_tie_normalization_runs,
            other.seed_canonical_tie_normalization_runs,
            overflow);
        add(seed_canonical_tie_normalization_anchors,
            other.seed_canonical_tie_normalization_anchors,
            overflow);
        if (other.seed_anchor_stats_operations != 0u) {
            if (anchor_operations_before == 0u) {
                seed_anchor_digest_lane0 = other.seed_anchor_digest_lane0;
                seed_anchor_digest_lane1 = other.seed_anchor_digest_lane1;
            } else {
                seed_anchor_digest_lane0 =
                    std::rotl(seed_anchor_digest_lane0, 19) ^ other.seed_anchor_digest_lane0 ^
                    (other.seed_canonical_anchor_count * 0x9e3779b97f4a7c15ull);
                seed_anchor_digest_lane1 =
                    std::rotl(seed_anchor_digest_lane1, 37) ^ other.seed_anchor_digest_lane1 ^
                    (other.seed_canonical_anchor_count * 0xbf58476d1ce4e5b9ull);
            }
        }
        add(gpu_index_materializations, other.gpu_index_materializations, overflow);
        add(gpu_index_open_addressed_materializations,
            other.gpu_index_open_addressed_materializations,
            overflow);
        add(gpu_index_sorted_dense_materializations,
            other.gpu_index_sorted_dense_materializations,
            overflow);
        add(gpu_index_unique_keys, other.gpu_index_unique_keys, overflow);
        add(gpu_index_slot_capacity, other.gpu_index_slot_capacity, overflow);
        add(gpu_index_logical_resident_bytes, other.gpu_index_logical_resident_bytes, overflow);
        add(gpu_index_actual_allocation_bytes, other.gpu_index_actual_allocation_bytes, overflow);
        add(gpu_index_fully_touched_bytes, other.gpu_index_fully_touched_bytes, overflow);
        add(gpu_index_staging_bytes, other.gpu_index_staging_bytes, overflow);
        add(gpu_index_permit_releases, other.gpu_index_permit_releases, overflow);
        add(gpu_index_process_footprint_reflections,
            other.gpu_index_process_footprint_reflections,
            overflow);
        add(gpu_index_reflected_footprint_bytes,
            other.gpu_index_reflected_footprint_bytes,
            overflow);
        add(gpu_index_count_admissions, other.gpu_index_count_admissions, overflow);
        add(gpu_index_main_peak_admissions, other.gpu_index_main_peak_admissions, overflow);
        add(gpu_index_resident_transfers, other.gpu_index_resident_transfers, overflow);
        add(gpu_index_adoption_rereserve_attempts,
            other.gpu_index_adoption_rereserve_attempts,
            overflow);
        add(gpu_index_adoption_successes, other.gpu_index_adoption_successes, overflow);
        add(gpu_index_owned_resident_bytes, other.gpu_index_owned_resident_bytes, overflow);
        add(gpu_index_gate_enabled, other.gpu_index_gate_enabled, overflow);
        add(gpu_index_gate_word_count, other.gpu_index_gate_word_count, overflow);
        add(gpu_index_gate_mask, other.gpu_index_gate_mask, overflow);
        add(gpu_index_gate_set_bits, other.gpu_index_gate_set_bits, overflow);
        add(gpu_index_gate_digest_lo, other.gpu_index_gate_digest_lo, overflow);
        add(gpu_index_gate_digest_hi, other.gpu_index_gate_digest_hi, overflow);
        add(gpu_index_gate_count_dispatches, other.gpu_index_gate_count_dispatches, overflow);
        add(gpu_index_gated_emit_dispatches, other.gpu_index_gated_emit_dispatches, overflow);
        add(gpu_index_ungated_emit_dispatches, other.gpu_index_ungated_emit_dispatches, overflow);
        add(gpu_index_exact_count_dispatches, other.gpu_index_exact_count_dispatches, overflow);
        add(gpu_index_exact_count_prefix_scan_dispatches,
            other.gpu_index_exact_count_prefix_scan_dispatches,
            overflow);
        add(gpu_index_exact_count_control_readbacks,
            other.gpu_index_exact_count_control_readbacks,
            overflow);
        add(gpu_index_deterministic_tile_emit_dispatches,
            other.gpu_index_deterministic_tile_emit_dispatches,
            overflow);
        add(gpu_index_atomic_append_dispatches, other.gpu_index_atomic_append_dispatches, overflow);
        add(gpu_index_emit_validation_control_readbacks,
            other.gpu_index_emit_validation_control_readbacks,
            overflow);
        add(gpu_index_counted_minimizer_count, other.gpu_index_counted_minimizer_count, overflow);
        add(gpu_index_verified_emit_count, other.gpu_index_verified_emit_count, overflow);
        add(gpu_index_count_emit_parity_verified,
            other.gpu_index_count_emit_parity_verified,
            overflow);
        add(gpu_index_target_minimizer_capacity_estimate,
            other.gpu_index_target_minimizer_capacity_estimate,
            overflow);
        add(gpu_index_capacity_estimate_underestimated,
            other.gpu_index_capacity_estimate_underestimated,
            overflow);
        add(gpu_index_arena_trimmed_before_permit_shrink,
            other.gpu_index_arena_trimmed_before_permit_shrink,
            overflow);
        add(gpu_index_retained_minimizer_count, other.gpu_index_retained_minimizer_count, overflow);
        add(gpu_index_distinct_minimizer_count, other.gpu_index_distinct_minimizer_count, overflow);
        add(gpu_index_source_direct_operations, other.gpu_index_source_direct_operations, overflow);
        add(gpu_index_source_direct_terminal_successes,
            other.gpu_index_source_direct_terminal_successes,
            overflow);
        add(gpu_index_source_direct_empty_bypasses,
            other.gpu_index_source_direct_empty_bypasses,
            overflow);
        add(gpu_index_source_direct_zero_query_bypasses,
            other.gpu_index_source_direct_zero_query_bypasses,
            overflow);
        add(gpu_index_source_direct_planning_receipts,
            other.gpu_index_source_direct_planning_receipts,
            overflow);
        add(gpu_index_source_direct_terminal_receipts,
            other.gpu_index_source_direct_terminal_receipts,
            overflow);
        add(gpu_index_source_direct_materializations,
            other.gpu_index_source_direct_materializations,
            overflow);
        add(gpu_index_source_direct_resident_publications,
            other.gpu_index_source_direct_resident_publications,
            overflow);
        add(gpu_index_source_direct_logical_control_bytes,
            other.gpu_index_source_direct_logical_control_bytes,
            overflow);
        add(gpu_index_source_direct_logical_position_bytes,
            other.gpu_index_source_direct_logical_position_bytes,
            overflow);
        add(gpu_index_source_direct_control_requirement_bytes,
            other.gpu_index_source_direct_control_requirement_bytes,
            overflow);
        add(gpu_index_source_direct_position_requirement_bytes,
            other.gpu_index_source_direct_position_requirement_bytes,
            overflow);
        add(gpu_index_source_direct_allocation_request_bytes,
            other.gpu_index_source_direct_allocation_request_bytes,
            overflow);
        add(gpu_index_source_direct_actual_control_bytes,
            other.gpu_index_source_direct_actual_control_bytes,
            overflow);
        add(gpu_index_source_direct_actual_position_bytes,
            other.gpu_index_source_direct_actual_position_bytes,
            overflow);
        add(gpu_index_source_direct_source_releases,
            other.gpu_index_source_direct_source_releases,
            overflow);
        add(gpu_index_source_direct_source_released_bytes,
            other.gpu_index_source_direct_source_released_bytes,
            overflow);
        add(gpu_index_source_direct_checksum_matches,
            other.gpu_index_source_direct_checksum_matches,
            overflow);
        add(gpu_index_source_direct_source_traversals,
            other.gpu_index_source_direct_source_traversals,
            overflow);
        add(gpu_index_source_direct_mapped_traversals,
            other.gpu_index_source_direct_mapped_traversals,
            overflow);
        add(gpu_index_source_direct_reflected_authorities,
            other.gpu_index_source_direct_reflected_authorities,
            overflow);
        add(gpu_index_source_direct_retained_authorities,
            other.gpu_index_source_direct_retained_authorities,
            overflow);
        add(gpu_index_source_direct_retained_resident_permit_bytes,
            other.gpu_index_source_direct_retained_resident_permit_bytes,
            overflow);
        add(gpu_index_source_direct_cache_preinsertions,
            other.gpu_index_source_direct_cache_preinsertions,
            overflow);
        add(gpu_index_source_direct_expected_seed_consumers,
            other.gpu_index_source_direct_expected_seed_consumers,
            overflow);
        add(gpu_index_source_direct_completed_seed_consumers,
            other.gpu_index_source_direct_completed_seed_consumers,
            overflow);
        add(gpu_index_source_direct_required_lookups,
            other.gpu_index_source_direct_required_lookups,
            overflow);
        add(gpu_index_source_direct_required_hits,
            other.gpu_index_source_direct_required_hits,
            overflow);
        add(gpu_index_source_direct_required_misses,
            other.gpu_index_source_direct_required_misses,
            overflow);
        add(gpu_index_source_direct_fallback_attempts,
            other.gpu_index_source_direct_fallback_attempts,
            overflow);
        add(gpu_index_source_direct_mapped_prefix_passes,
            other.gpu_index_source_direct_mapped_prefix_passes,
            overflow);
        add(gpu_index_source_direct_mapped_prefix_lookups,
            other.gpu_index_source_direct_mapped_prefix_lookups,
            overflow);
        add(gpu_index_source_direct_mapped_prefix_comparisons,
            other.gpu_index_source_direct_mapped_prefix_comparisons,
            overflow);
        add(gpu_index_source_direct_seed_slab_requested_bytes,
            other.gpu_index_source_direct_seed_slab_requested_bytes,
            overflow);
        add(gpu_index_source_direct_seed_slab_initial_capacity,
            other.gpu_index_source_direct_seed_slab_initial_capacity,
            overflow);
        add(gpu_index_source_direct_seed_slab_actual_bytes,
            other.gpu_index_source_direct_seed_slab_actual_bytes,
            overflow);
        add(gpu_index_source_direct_seed_slab_retained_bytes,
            other.gpu_index_source_direct_seed_slab_retained_bytes,
            overflow);
        add(gpu_index_source_direct_seed_slab_permit_releases,
            other.gpu_index_source_direct_seed_slab_permit_releases,
            overflow);
        add(gpu_index_source_direct_seed_reserved_bytes,
            other.gpu_index_source_direct_seed_reserved_bytes,
            overflow);
        add(gpu_index_source_direct_seed_planned_vulkan_bytes,
            other.gpu_index_source_direct_seed_planned_vulkan_bytes,
            overflow);
        add(gpu_index_source_direct_seed_actual_vulkan_bytes,
            other.gpu_index_source_direct_seed_actual_vulkan_bytes,
            overflow);
        add(gpu_index_source_direct_seed_roundtrips,
            other.gpu_index_source_direct_seed_roundtrips,
            overflow);
        add(gpu_index_source_direct_zero_anchor_bypasses,
            other.gpu_index_source_direct_zero_anchor_bypasses,
            overflow);
        add(gpu_index_source_direct_seed_fence_completions,
            other.gpu_index_source_direct_seed_fence_completions,
            overflow);
        add(gpu_index_source_direct_cache_cleanup_releases,
            other.gpu_index_source_direct_cache_cleanup_releases,
            overflow);
        add(gpu_index_source_direct_resident_cleanup_bytes,
            other.gpu_index_source_direct_resident_cleanup_bytes,
            overflow);
        add(gpu_index_source_direct_pipeline_completions,
            other.gpu_index_source_direct_pipeline_completions,
            overflow);
        add(post_seed_owned_gpu_cache_evictions,
            other.post_seed_owned_gpu_cache_evictions,
            overflow);
        add(post_seed_owned_gpu_resident_bytes_released,
            other.post_seed_owned_gpu_resident_bytes_released,
            overflow);
        add(post_seed_owned_cpu_index_releases, other.post_seed_owned_cpu_index_releases, overflow);
        add(post_seed_owned_cpu_position_bytes_released,
            other.post_seed_owned_cpu_position_bytes_released,
            overflow);
        gpu_index_max_load_ppm = std::max(gpu_index_max_load_ppm, other.gpu_index_max_load_ppm);
        gpu_index_max_probe = std::max(gpu_index_max_probe, other.gpu_index_max_probe);
        is_saturated = overflow;
    }

    [[nodiscard]] constexpr bool has_any_dispatch() const noexcept {
        return is_saturated || sketch_dispatches != 0 || query_sketch_dispatches != 0 ||
               target_sketch_dispatches != 0 || target_index_build_dispatches != 0 ||
               seed_lookup_dispatches != 0 || seed_open_addressed_lookup_dispatches != 0 ||
               seed_sorted_dense_lookup_dispatches != 0 || chain_score_dispatches != 0 ||
               seed_bitonic_sort_dispatches != 0 || seed_radix_sort_operations != 0 ||
               seed_radix_anchor_count != 0 || seed_radix_passes != 0 ||
               seed_radix_histogram_dispatches != 0 || seed_radix_scan_dispatches != 0 ||
               seed_radix_scatter_dispatches != 0 || seed_host_sort_fallbacks != 0 ||
               seed_anchor_stats_operations != 0 || seed_canonical_anchor_count != 0 ||
               seed_canonical_anchor_order_checks != 0 ||
               seed_canonical_anchor_order_accepts != 0 ||
               seed_canonical_tie_normalization_runs != 0 ||
               seed_canonical_tie_normalization_anchors != 0 || seed_anchor_digest_lane0 != 0 ||
               seed_anchor_digest_lane1 != 0 || gpu_index_gate_count_dispatches != 0 ||
               gpu_index_gated_emit_dispatches != 0 || gpu_index_ungated_emit_dispatches != 0 ||
               gpu_index_exact_count_dispatches != 0 ||
               gpu_index_exact_count_prefix_scan_dispatches != 0 ||
               gpu_index_exact_count_control_readbacks != 0 ||
               gpu_index_deterministic_tile_emit_dispatches != 0 ||
               gpu_index_atomic_append_dispatches != 0 ||
               gpu_index_emit_validation_control_readbacks != 0 ||
               gpu_index_counted_minimizer_count != 0 || gpu_index_verified_emit_count != 0 ||
               gpu_index_count_emit_parity_verified != 0 ||
               gpu_index_target_minimizer_capacity_estimate != 0 ||
               gpu_index_capacity_estimate_underestimated != 0 ||
               gpu_index_arena_trimmed_before_permit_shrink != 0 ||
               gpu_index_source_direct_operations != 0 ||
               gpu_index_source_direct_empty_bypasses != 0 ||
               gpu_index_source_direct_planning_receipts != 0 ||
               gpu_index_source_direct_terminal_receipts != 0;
    }

    [[nodiscard]] constexpr bool is_exact_target_index_route_consistent() const noexcept {
        const std::uint64_t builds = target_index_build_dispatches;
        if (builds == 0u) {
            return gpu_index_exact_count_dispatches == 0u &&
                   gpu_index_exact_count_prefix_scan_dispatches == 0u &&
                   gpu_index_exact_count_control_readbacks == 0u &&
                   gpu_index_deterministic_tile_emit_dispatches == 0u &&
                   gpu_index_atomic_append_dispatches == 0u &&
                   gpu_index_emit_validation_control_readbacks == 0u &&
                   gpu_index_counted_minimizer_count == 0u && gpu_index_verified_emit_count == 0u &&
                   gpu_index_count_emit_parity_verified == 0u &&
                   gpu_index_target_minimizer_capacity_estimate == 0u &&
                   gpu_index_capacity_estimate_underestimated == 0u &&
                   gpu_index_arena_trimmed_before_permit_shrink == 0u &&
                   gpu_index_gate_enabled == 0u && gpu_index_gate_count_dispatches == 0u &&
                   gpu_index_gated_emit_dispatches == 0u &&
                   gpu_index_ungated_emit_dispatches == 0u &&
                   gpu_index_retained_minimizer_count == 0u &&
                   gpu_index_distinct_minimizer_count == 0u;
        }
        // Query zero records the one target build shared by this batch
        if (builds != 1u) {
            return false;
        }
        const bool empty = gpu_index_retained_minimizer_count == 0u;
        return target_sketch_dispatches == builds && gpu_index_exact_count_dispatches == builds &&
               gpu_index_exact_count_prefix_scan_dispatches == builds * 3u &&
               gpu_index_exact_count_control_readbacks == builds &&
               gpu_index_deterministic_tile_emit_dispatches == (empty ? 0u : builds) &&
               gpu_index_atomic_append_dispatches == 0u &&
               gpu_index_emit_validation_control_readbacks == (empty ? 0u : builds) &&
               gpu_index_counted_minimizer_count == gpu_index_retained_minimizer_count &&
               gpu_index_verified_emit_count == gpu_index_retained_minimizer_count &&
               gpu_index_count_emit_parity_verified == builds &&
               gpu_index_target_minimizer_capacity_estimate > 0u &&
               gpu_index_capacity_estimate_underestimated ==
                   (gpu_index_retained_minimizer_count >
                            gpu_index_target_minimizer_capacity_estimate
                        ? 1u
                        : 0u) &&
               gpu_index_arena_trimmed_before_permit_shrink == builds &&
               (gpu_index_gate_enabled == 1u
                    ? gpu_index_gate_count_dispatches == 1u &&
                          gpu_index_gated_emit_dispatches ==
                              gpu_index_deterministic_tile_emit_dispatches &&
                          gpu_index_ungated_emit_dispatches == 0u
                    : gpu_index_gate_enabled == 0u && gpu_index_gate_count_dispatches == 0u &&
                          gpu_index_gated_emit_dispatches == 0u &&
                          gpu_index_ungated_emit_dispatches ==
                              gpu_index_deterministic_tile_emit_dispatches);
    }

    /// Check source-direct index  preparation, which has no GPU target-build stage.
    /// Require its completion records and empty build/gate counters. The runtime
    /// separately validates the resident index contents and storage.
    [[nodiscard]] constexpr bool
    is_exact_source_direct_target_index_route_consistent() const noexcept {
        return target_index_build_dispatches == 0u && gpu_index_source_direct_operations == 1u &&
               gpu_index_source_direct_planning_receipts == 1u &&
               gpu_index_source_direct_terminal_receipts == 1u &&
               gpu_index_source_direct_terminal_successes == 1u &&
               gpu_index_count_admissions == 0u && gpu_index_main_peak_admissions == 0u &&
               gpu_index_resident_transfers == 0u && gpu_index_adoption_rereserve_attempts == 0u &&
               gpu_index_adoption_successes == 0u && gpu_index_owned_resident_bytes == 0u &&
               gpu_index_gate_enabled == 0u && gpu_index_gate_word_count == 0u &&
               gpu_index_gate_mask == 0u && gpu_index_gate_set_bits == 0u &&
               gpu_index_gate_digest_lo == 0u && gpu_index_gate_digest_hi == 0u &&
               gpu_index_gate_count_dispatches == 0u && gpu_index_gated_emit_dispatches == 0u &&
               gpu_index_ungated_emit_dispatches == 0u && gpu_index_exact_count_dispatches == 0u &&
               gpu_index_exact_count_prefix_scan_dispatches == 0u &&
               gpu_index_exact_count_control_readbacks == 0u &&
               gpu_index_deterministic_tile_emit_dispatches == 0u &&
               gpu_index_atomic_append_dispatches == 0u &&
               gpu_index_emit_validation_control_readbacks == 0u &&
               gpu_index_counted_minimizer_count == 0u && gpu_index_verified_emit_count == 0u &&
               gpu_index_count_emit_parity_verified == 0u &&
               gpu_index_target_minimizer_capacity_estimate == 0u &&
               gpu_index_capacity_estimate_underestimated == 0u &&
               gpu_index_arena_trimmed_before_permit_shrink == 0u;
    }

    [[nodiscard]] constexpr bool
    is_exact_stage_mask_consistent(bool expect_query_sketch,
                                   bool expect_target_sketch,
                                   bool expect_target_index_build,
                                   bool expect_seed_lookup,
                                   bool expect_chain_score) const noexcept {
        const auto matches = [](std::uint64_t count, bool expected) noexcept {
            return expected ? count > 0 : count == 0;
        };
        return !is_saturated && sketch_dispatches == 0 &&
               is_exact_target_index_route_consistent() &&
               matches(query_sketch_dispatches, expect_query_sketch) &&
               matches(target_sketch_dispatches, expect_target_sketch) &&
               matches(target_index_build_dispatches, expect_target_index_build) &&
               matches(seed_lookup_dispatches, expect_seed_lookup) &&
               matches(chain_score_dispatches, expect_chain_score);
    }

    [[nodiscard]] constexpr bool
    is_exact_source_direct_stage_mask_consistent(bool expect_query_sketch,
                                                 bool expect_target_sketch,
                                                 bool expect_seed_lookup,
                                                 bool expect_chain_score) const noexcept {
        const auto matches = [](std::uint64_t count, bool expected) noexcept {
            return expected ? count > 0 : count == 0;
        };
        return !is_saturated && sketch_dispatches == 0u &&
               is_exact_source_direct_target_index_route_consistent() &&
               matches(query_sketch_dispatches, expect_query_sketch) &&
               matches(target_sketch_dispatches, expect_target_sketch) &&
               matches(seed_lookup_dispatches, expect_seed_lookup) &&
               matches(chain_score_dispatches, expect_chain_score);
    }

    [[nodiscard]] constexpr bool is_exact_sketch_token_consistent() const noexcept {
        if (sketch_dispatches != 1) {
            return false;
        }
        partial_vulkan_execution_stats expected{};
        expected.sketch_dispatches = 1;
        return *this == expected;
    }

    constexpr bool operator==(const partial_vulkan_execution_stats&) const noexcept = default;
};

/// Query zero records the shared target build so batch totals count it once.
[[nodiscard]] constexpr bool is_ordered_target_build_owner_query(std::size_t query_index) noexcept {
    return query_index == 0u;
}

static_assert([] {
    partial_vulkan_execution_stats stats{};
    stats.chain_score_dispatches = 1;
    return stats.is_exact_stage_mask_consistent(false, false, false, false, true);
}());
static_assert([] {
    partial_vulkan_execution_stats stats{};
    stats.query_sketch_dispatches = 1;
    stats.target_sketch_dispatches = 1;
    stats.target_index_build_dispatches = 1;
    stats.gpu_index_exact_count_dispatches = 1;
    stats.gpu_index_exact_count_prefix_scan_dispatches = 3;
    stats.gpu_index_exact_count_control_readbacks = 1;
    stats.gpu_index_deterministic_tile_emit_dispatches = 1;
    stats.gpu_index_emit_validation_control_readbacks = 1;
    stats.gpu_index_counted_minimizer_count = 53;
    stats.gpu_index_verified_emit_count = 53;
    stats.gpu_index_count_emit_parity_verified = 1;
    stats.gpu_index_target_minimizer_capacity_estimate = 100;
    stats.gpu_index_arena_trimmed_before_permit_shrink = 1;
    stats.gpu_index_ungated_emit_dispatches = 1;
    stats.gpu_index_retained_minimizer_count = 53;
    stats.seed_lookup_dispatches = 1;
    stats.chain_score_dispatches = 1;
    return stats.is_exact_stage_mask_consistent(true, true, true, true, true) &&
           !stats.is_exact_stage_mask_consistent(true, true, false, true, true);
}());
static_assert([] {
    partial_vulkan_execution_stats stats{};
    stats.query_sketch_dispatches = 1u;
    stats.target_sketch_dispatches = 1u;
    stats.seed_lookup_dispatches = 1u;
    stats.chain_score_dispatches = 1u;
    stats.gpu_index_materializations = 1u;
    stats.gpu_index_sorted_dense_materializations = 1u;
    stats.gpu_index_retained_minimizer_count = 53u;
    stats.gpu_index_distinct_minimizer_count = 17u;
    stats.gpu_index_source_direct_operations = 1u;
    stats.gpu_index_source_direct_planning_receipts = 1u;
    stats.gpu_index_source_direct_terminal_receipts = 1u;
    stats.gpu_index_source_direct_terminal_successes = 1u;
    if (!stats.is_exact_source_direct_stage_mask_consistent(true, true, true, true)) {
        return false;
    }
    if (stats.is_exact_stage_mask_consistent(true, true, false, true, true)) {
        return false;
    }
    stats.gpu_index_gate_enabled = 1u;
    return !stats.is_exact_source_direct_stage_mask_consistent(true, true, true, true);
}());
static_assert([] {
    partial_vulkan_execution_stats lhs{};
    lhs.gpu_index_materializations = 1;
    lhs.gpu_index_open_addressed_materializations = 1;
    lhs.gpu_index_unique_keys = 3;
    lhs.gpu_index_max_probe = 7;
    partial_vulkan_execution_stats rhs{};
    rhs.gpu_index_materializations = 1;
    rhs.gpu_index_open_addressed_materializations = 1;
    rhs.gpu_index_unique_keys = 5;
    rhs.gpu_index_max_probe = 4;
    lhs.merge(rhs);
    return lhs.gpu_index_materializations == 2 &&
           lhs.gpu_index_open_addressed_materializations == 2 && lhs.gpu_index_unique_keys == 8 &&
           lhs.gpu_index_max_probe == 7;
}());

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/** How a GPU seed job finished, including empty-query and empty-target cases. */
enum class vulkan_seed_frontend_completion_kind : std::uint8_t {
    invalid = 0,
    mpf_runtime,
    mpf_zero_query,
    mpf_empty_target,
    mps_runtime,
    mps_zero_query,
};

/** Target format and persisted or generated source used by a GPU seed batch. */
enum class vulkan_seed_target_completion_kind : std::uint8_t {
    invalid = 0,
    mpf_persisted,
    mpf_runtime_generated,
    mps_persisted,
    mps_runtime_generated,
};

/**
 * Record the target preparation shared by a batch of seed jobs. Values come
 * from completed runtime records. The batch publisher adds these  physical
 * work counts once, even when many queries use the same target.
 */
struct vulkan_seed_target_execution {
    vulkan_seed_target_completion_kind completion =
        vulkan_seed_target_completion_kind::invalid;
    bioseq::gpu::vulkan::gpu_device_uuid device_uuid{};
    std::uint64_t target_operation_identity = 0u;
    std::uint64_t target_len = 0u;
    std::uint64_t persisted_generation_id = 0u;
    std::uint64_t persisted_source_identity = 0u;
    std::uint64_t persisted_cold_load_identity = 0u;
    std::uint64_t producer_roundtrips = 0u;
    std::uint64_t resident_materializations = 0u;
    std::uint64_t resident_ingestions = 0u;
    bool canonical_empty = false;
    bool query_empty_bypass = false;
    bool producer_receipt_present = false;

    /** Return persisted, recomputed or none from the target completion kind. */
    [[nodiscard]] constexpr ::bioseq::pipeline_stats::target_index_source
    target_source() const noexcept {
        using source = ::bioseq::pipeline_stats::target_index_source;
        switch (completion) {
        case vulkan_seed_target_completion_kind::mpf_persisted:
        case vulkan_seed_target_completion_kind::mps_persisted:
            return source::persisted;
        case vulkan_seed_target_completion_kind::mpf_runtime_generated:
        case vulkan_seed_target_completion_kind::mps_runtime_generated:
            return source::recomputed;
        case vulkan_seed_target_completion_kind::invalid:
            return source::none;
        }
        return source::none;
    }

    /** Check whether a target-sketch, completion record is present. */
    [[nodiscard]] constexpr bool target_sketch_completed() const noexcept {
        // A short or empty sketh can complete without submitting GPU work.
        // Count its completion separately from producer roundtrips.
        return producer_receipt_present;
    }

    /** Check whether a generated target completed preparation without a query bypass. */
    [[nodiscard]] constexpr bool target_index_build_executed() const noexcept {
        return target_source() ==
                   ::bioseq::pipeline_stats::target_index_source::recomputed &&
               !query_empty_bypass && producer_receipt_present;
    }

    /** Check whether device storage was created for the target  index. */
    [[nodiscard]] constexpr bool device_index_upload_executed() const noexcept {
        return resident_materializations != 0u;
    }

    /** Check target identity and the completion counts required by its storage route. */
    [[nodiscard]] bool is_internally_consistent() const noexcept {
        const auto source = target_source();
        const bool persisted =
            source == ::bioseq::pipeline_stats::target_index_source::persisted;
        const bool device_bound = std::ranges::any_of(
            device_uuid, [](const std::uint8_t byte) noexcept { return byte != 0u; });
        if (source == ::bioseq::pipeline_stats::target_index_source::none ||
            !device_bound || target_operation_identity == 0u || target_len == 0u ||
            (persisted ? persisted_generation_id == 0u ||
                             persisted_source_identity == 0u
                       : persisted_generation_id != 0u ||
                             persisted_source_identity != 0u ||
                             persisted_cold_load_identity != 0u)) {
            return false;
        }
        const bool mpf =
            completion == vulkan_seed_target_completion_kind::mpf_persisted ||
            completion == vulkan_seed_target_completion_kind::mpf_runtime_generated;
        if (mpf) {
            const std::uint64_t expected_ingestions =
                persisted && !canonical_empty ? 1u : 0u;
            if (persisted_cold_load_identity != 0u ||
                resident_ingestions != expected_ingestions ||
                resident_materializations > 2u ||
                (canonical_empty && resident_materializations != 0u) ||
                (!canonical_empty && resident_materializations != 2u) ||
                (persisted &&
                 (producer_roundtrips != 0u || producer_receipt_present)) ||
                (query_empty_bypass &&
                 (!canonical_empty || producer_roundtrips != 0u ||
                  resident_materializations != 0u || producer_receipt_present)) ||
                (!persisted && !query_empty_bypass && !producer_receipt_present)) {
                return false;
            }
            return true;
        }
        const bool mps =
            completion == vulkan_seed_target_completion_kind::mps_persisted ||
            completion == vulkan_seed_target_completion_kind::mps_runtime_generated;
        return mps && !query_empty_bypass && resident_materializations != 0u &&
               resident_ingestions != 0u &&
               (persisted ? persisted_cold_load_identity != 0u &&
                                producer_roundtrips == 0u &&
                                !producer_receipt_present
                          : producer_receipt_present);
    }
};

/**
 * Copy the completed seed job details needed by its CPU stages. The shared
 * runtime operation keeps the validation token Each result carries this
 * smaller record and can check it against that token before publication.
 */
struct vulkan_seed_frontend_job_execution {
    vulkan_seed_frontend_completion_kind completion =
        vulkan_seed_frontend_completion_kind::invalid;
    partial_vulkan_execution_stats seed_stats{};
    std::uint64_t operation_identity = 0u;
    std::uint64_t query_authority_identity = 0u;
    std::uint64_t query_ordinal = 0u;
    std::uint64_t minimizer_count = 0u;
    std::uint64_t target_len = 0u;
    std::uint64_t target_position_offset = 0u;
    std::uint64_t anchor_count = 0u;
    vulkan_seed_target_completion_kind target_completion =
        vulkan_seed_target_completion_kind::invalid;
    std::uint64_t target_operation_identity = 0u;
    std::uint64_t persisted_generation_id = 0u;
    std::uint64_t persisted_source_identity = 0u;
    std::size_t operation_job_index = 0u;

    /** Return persisted, recomputed or none from the target representation. */
    [[nodiscard]] constexpr ::bioseq::pipeline_stats::target_index_source
    target_source() const noexcept {
        using source = ::bioseq::pipeline_stats::target_index_source;
        switch (target_completion) {
        case vulkan_seed_target_completion_kind::mpf_persisted:
        case vulkan_seed_target_completion_kind::mps_persisted:
            return source::persisted;
        case vulkan_seed_target_completion_kind::mpf_runtime_generated:
        case vulkan_seed_target_completion_kind::mps_runtime_generated:
            return source::recomputed;
        case vulkan_seed_target_completion_kind::invalid:
            return source::none;
        }
        return source::none;
    }

    /** Check seed-only counters, anchor ordering and the job's target identity. */
    [[nodiscard]] bool is_internally_consistent() const noexcept {
        partial_vulkan_execution_stats expected{};
        expected.seed_lookup_dispatches = seed_stats.seed_lookup_dispatches;
        expected.seed_open_addressed_lookup_dispatches =
            seed_stats.seed_open_addressed_lookup_dispatches;
        expected.seed_sorted_dense_lookup_dispatches =
            seed_stats.seed_sorted_dense_lookup_dispatches;
        expected.seed_bitonic_sort_dispatches = seed_stats.seed_bitonic_sort_dispatches;
        expected.seed_radix_sort_operations = seed_stats.seed_radix_sort_operations;
        expected.seed_radix_anchor_count = seed_stats.seed_radix_anchor_count;
        expected.seed_radix_passes = seed_stats.seed_radix_passes;
        expected.seed_radix_histogram_dispatches =
            seed_stats.seed_radix_histogram_dispatches;
        expected.seed_radix_scan_dispatches = seed_stats.seed_radix_scan_dispatches;
        expected.seed_radix_scatter_dispatches = seed_stats.seed_radix_scatter_dispatches;
        expected.seed_host_sort_fallbacks = seed_stats.seed_host_sort_fallbacks;
        expected.seed_anchor_stats_operations = seed_stats.seed_anchor_stats_operations;
        expected.seed_canonical_anchor_count = seed_stats.seed_canonical_anchor_count;
        expected.seed_canonical_anchor_order_checks =
            seed_stats.seed_canonical_anchor_order_checks;
        expected.seed_canonical_anchor_order_accepts =
            seed_stats.seed_canonical_anchor_order_accepts;
        expected.seed_canonical_tie_normalization_runs =
            seed_stats.seed_canonical_tie_normalization_runs;
        expected.seed_canonical_tie_normalization_anchors =
            seed_stats.seed_canonical_tie_normalization_anchors;
        expected.seed_anchor_digest_lane0 = seed_stats.seed_anchor_digest_lane0;
        expected.seed_anchor_digest_lane1 = seed_stats.seed_anchor_digest_lane1;
        const bool seed_fields_only = seed_stats == expected && !seed_stats.is_saturated;
        const bool canonical = seed_stats.seed_anchor_stats_operations == 1u &&
                               seed_stats.seed_canonical_anchor_count == anchor_count &&
                               seed_stats.seed_canonical_anchor_order_checks == 1u &&
                               seed_stats.seed_canonical_anchor_order_accepts == 1u &&
                               seed_stats.seed_host_sort_fallbacks == 0u;
        const bool sort_shape =
            seed_stats.seed_bitonic_sort_dispatches <= 1u &&
            seed_stats.seed_radix_sort_operations <= 1u &&
            !(seed_stats.seed_bitonic_sort_dispatches != 0u &&
              seed_stats.seed_radix_sort_operations != 0u) &&
            (seed_stats.seed_bitonic_sort_dispatches == 0u || anchor_count != 0u) &&
            (seed_stats.seed_radix_sort_operations == 0u
                 ? seed_stats.seed_radix_anchor_count == 0u &&
                       seed_stats.seed_radix_passes == 0u &&
                       seed_stats.seed_radix_histogram_dispatches == 0u &&
                       seed_stats.seed_radix_scan_dispatches == 0u &&
                       seed_stats.seed_radix_scatter_dispatches == 0u
                 : anchor_count != 0u &&
                       seed_stats.seed_radix_anchor_count == anchor_count &&
                       seed_stats.seed_radix_passes != 0u &&
                       seed_stats.seed_radix_histogram_dispatches ==
                           seed_stats.seed_radix_passes &&
                       seed_stats.seed_radix_scan_dispatches ==
                           seed_stats.seed_radix_passes &&
                       seed_stats.seed_radix_scatter_dispatches ==
                           seed_stats.seed_radix_passes);
        const auto source = target_source();
        const bool persisted_target =
            source == ::bioseq::pipeline_stats::target_index_source::persisted;
        const bool target_identity =
            source != ::bioseq::pipeline_stats::target_index_source::none &&
            target_operation_identity != 0u &&
            (persisted_target ? persisted_generation_id != 0u &&
                                    persisted_source_identity != 0u
                              : persisted_generation_id == 0u &&
                                    persisted_source_identity == 0u);
        if (!seed_fields_only || !canonical || !sort_shape || !target_identity ||
            query_authority_identity == 0u ||
            query_ordinal > (std::numeric_limits<std::uint32_t>::max)() || target_len == 0u ||
            target_position_offset >
                (std::numeric_limits<std::uint64_t>::max)() - (target_len - 1u)) {
            return false;
        }
        const bool runtime_seed =
            completion == vulkan_seed_frontend_completion_kind::mpf_runtime ||
            completion == vulkan_seed_frontend_completion_kind::mps_runtime;
        if (runtime_seed) {
            if (completion == vulkan_seed_frontend_completion_kind::mps_runtime) {
                // MPS performs one lookup per binding window. The saved  operation
                // supplies the window count checked before publication.
                return operation_identity != 0u && minimizer_count != 0u &&
                       seed_stats.seed_lookup_dispatches != 0u &&
                       seed_stats.seed_open_addressed_lookup_dispatches == 0u &&
                       seed_stats.seed_sorted_dense_lookup_dispatches ==
                           seed_stats.seed_lookup_dispatches;
            }
            return operation_identity != 0u && minimizer_count != 0u &&
                   seed_stats.seed_lookup_dispatches == 1u &&
                   ((seed_stats.seed_open_addressed_lookup_dispatches == 1u &&
                     seed_stats.seed_sorted_dense_lookup_dispatches == 0u) ||
                    (seed_stats.seed_open_addressed_lookup_dispatches == 0u &&
                     seed_stats.seed_sorted_dense_lookup_dispatches == 1u));
        }
        const bool empty = completion == vulkan_seed_frontend_completion_kind::mpf_zero_query ||
                           completion == vulkan_seed_frontend_completion_kind::mpf_empty_target ||
                           completion == vulkan_seed_frontend_completion_kind::mps_zero_query;
        const bool mps_zero =
            completion == vulkan_seed_frontend_completion_kind::mps_zero_query;
        return empty && (mps_zero ? operation_identity != 0u : operation_identity == 0u) &&
               (completion == vulkan_seed_frontend_completion_kind::mpf_empty_target ||
                minimizer_count == 0u) &&
               anchor_count == 0u && seed_stats.seed_lookup_dispatches == 0u &&
               seed_stats.seed_open_addressed_lookup_dispatches == 0u &&
               seed_stats.seed_sorted_dense_lookup_dispatches == 0u &&
               seed_stats.seed_bitonic_sort_dispatches == 0u &&
               seed_stats.seed_radix_sort_operations == 0u;
    }

    /** Check this job record against its saved runtime operation token. */
    [[nodiscard]] bool matches_runtime_operation(
        const bioseq::gpu::vulkan::runtime_pipeline_operation_token& operation,
        const std::uint32_t max_workgroup_count_x) const noexcept {
        if (!is_internally_consistent()) {
            return false;
        }
        using operation_kind = bioseq::gpu::vulkan::runtime_pipeline_operation_kind;
        if (completion == vulkan_seed_frontend_completion_kind::mpf_zero_query ||
            completion == vulkan_seed_frontend_completion_kind::mpf_empty_target) {
            return operation.audit().kind == operation_kind::invalid;
        }
        const auto& audit = operation.audit();
        const bool mpf = completion == vulkan_seed_frontend_completion_kind::mpf_runtime;
        const bool valid_operation =
            mpf ? operation.is_valid_for_mpf_seed_anchor_roundtrip(max_workgroup_count_x)
                : operation.is_valid_for_grouped_soa_seed_anchor_roundtrip();
        if (!valid_operation || audit.operation_identity != operation_identity ||
            operation_job_index >= audit.jobs.size() ||
            (mpf && (audit.kind != operation_kind::mpf_seed_anchor_roundtrip ||
                     operation_job_index != 0u)) ||
            (!mpf && audit.kind != operation_kind::grouped_soa_seed_anchor_roundtrip)) {
            return false;
        }
        const auto& job = audit.jobs[operation_job_index];
        const std::uint64_t expected_mps_lookups =
            minimizer_count != 0u ? job.binding_window_count : 0u;
        return (mpf || seed_stats.seed_lookup_dispatches == expected_mps_lookups) &&
               job.minimizer_count == minimizer_count && job.target_len == target_len &&
               job.target_position_offset == target_position_offset &&
               job.anchor_count == anchor_count && job.range_count == 0u &&
               job.query_authority_identity == query_authority_identity &&
               job.anchor_digest_lo == seed_stats.seed_anchor_digest_lane0 &&
               job.anchor_digest_hi == seed_stats.seed_anchor_digest_lane1 &&
               job.anchor_order_checked && job.anchor_order_accepted;
    }
};
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/// Transient CPU target format kept until PostgreSQL checks the job record.
enum class transient_cpu_target_generation_representation : std::uint8_t {
    none,
    minimizer_fragment_spill,
    minimizer_sequence_publication,
};
#endif

/**
 * @brief Own alignment results and the statistics collected while producing them.
 *
 * Results stay in order. Execution checks use the saved stage records, and
 * print() writes PAF fields and auxiliary tags to the debug log.
 */
struct match_result_collection {
    match_result_vector results;
    two_stack_execution_stats exact_simd_query_execution_stats{};
    two_stack_execution_stats exact_simd_target_execution_stats{};
    bounded_nthash_execution_stats bounded_nthash_query_execution_stats{};
    bounded_nthash_execution_stats bounded_nthash_target_execution_stats{};
    bioseq::chaining::simd_chaining_execution_stats simd_chaining_stats{};
    block_aligner_execution_stats block_aligner_stats{};
    scalar_pipeline_execution_stats scalar_pipeline_stats{};
    ksw2_batched_execution_stats ksw2_batched_stats{};
    ::bioseq::prefilter::prefilter_execution_stats prefilter_stats{};
    partial_vulkan_execution_stats partial_vulkan_stats{};
    bioseq::indexing::prepared_fragment_anchor_batch_stats fragment_native_query_stats{};
    bool exact_simd_query_execution_stats_available = false;
    bool exact_simd_target_execution_stats_available = false;
    bool bounded_nthash_query_execution_stats_available = false;
    bool bounded_nthash_target_execution_stats_available = false;
    bool simd_chaining_execution_stats_available = false;
    bool block_aligner_execution_stats_available = false;
    bool scalar_pipeline_execution_stats_available = false;
    bool ksw2_batched_execution_stats_available = false;
    bool prefilter_execution_stats_available = false;
    bool fragment_native_query_stats_available = false;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    // Keep the completed generation rdcord in test and benchmark builds.
    // Fragment spills and encoded minimizer sequences need different checks.
    std::optional<bioseq::indexing::minimizer_sequence_generation_execution_receipt>
        transient_cpu_target_generation_receipt{};
    transient_cpu_target_generation_representation
        transient_cpu_target_generation_representation_kind =
            transient_cpu_target_generation_representation::none;
    bool transient_cpu_target_generation_receipt_conflict = false;
    bool transient_cpu_target_generation_stats_published = false;
#endif
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    // Keep the completed Vulkan operation  token until PostgreSQL publishes
    // the job. The token is internal and is not a report counter.
    ::bioseq::gpu::vulkan::runtime_pipeline_operation_token completed_vulkan_chaining{};
    // Keep one operation inline. Segmented results allocate space only, for
    // additional completed operations.
    admitted_alignment_result_vector<
        ::bioseq::gpu::vulkan::runtime_pipeline_operation_token, false>
        additional_completed_vulkan_chaining{};

    [[nodiscard]] bool has_valid_completed_vulkan_chaining(
        const std::uint64_t expected_dispatches) const noexcept {
        if (expected_dispatches == 0u ||
            additional_completed_vulkan_chaining.size() != expected_dispatches - 1u ||
            !completed_vulkan_chaining.is_valid_for_chaining_tiled_dp()) {
            return false;
        }
        for (const auto& operation : additional_completed_vulkan_chaining) {
            if (!operation.is_valid_for_chaining_tiled_dp())
                return false;
        }
        return true;
    }

    /// Move all completed chaining tokens before adding their counters.
    /// Reserve storage first so allocation failure leaves both sets of tokens in place.
    void retain_completed_vulkan_chaining(match_result_collection& source) {
        using kind = ::bioseq::gpu::vulkan::runtime_pipeline_operation_kind;
        if (this == &source) {
            throw std::logic_error{"chaining receipt collection cannot merge itself"};
        }
        const bool have_source = source.completed_vulkan_chaining.audit().kind != kind::invalid;
        if (!have_source) {
            if (!source.additional_completed_vulkan_chaining.empty()) {
                throw std::logic_error{"segmented chaining receipts lack their first operation"};
            }
            return;
        }
        const bool have_first = completed_vulkan_chaining.audit().kind != kind::invalid;
        if (!have_first && !additional_completed_vulkan_chaining.empty()) {
            throw std::logic_error{"destination chaining receipts lack their first operation"};
        }
        const auto incoming = bioseq::common::checked_add(
            source.additional_completed_vulkan_chaining.size(), std::size_t{have_first ? 1u : 0u});
        const auto required = incoming
            ? bioseq::common::checked_add(additional_completed_vulkan_chaining.size(), *incoming)
            : std::optional<std::size_t>{};
        if (!required) {
            throw std::overflow_error{"segmented chaining receipt count overflow"};
        }
        const std::size_t old_capacity = additional_completed_vulkan_chaining.capacity();
        std::size_t capacity = old_capacity;
        if (*required > old_capacity) {
            const std::size_t maximum = std::vector<
                ::bioseq::gpu::vulkan::runtime_pipeline_operation_token>{}.max_size();
            const std::size_t grown = old_capacity <= maximum / 2u
                ? old_capacity * 2u : maximum;
            capacity = std::max(*required, grown);
        }
        additional_completed_vulkan_chaining.set_logical_bounds(*required, capacity);
        additional_completed_vulkan_chaining.reserve(capacity);
        if (have_first) {
            additional_completed_vulkan_chaining.push_back(
                std::move(source.completed_vulkan_chaining));
        } else {
            completed_vulkan_chaining = std::move(source.completed_vulkan_chaining);
        }
        for (auto& operation : source.additional_completed_vulkan_chaining) {
            additional_completed_vulkan_chaining.push_back(std::move(operation));
        }
        source.additional_completed_vulkan_chaining.clear();
    }
    // Record the algorithms after the selected pipeline finishes successfully
    minimizer_algorithm observed_minimizer_algorithm = minimizer_algorithm::scalar;
    chaining_algorithm observed_chaining_algorithm = chaining_algorithm::scalar_minimap2;
    base_alignment_algorithm observed_base_alignment_algorithm = base_alignment_algorithm::ksw2;
    bool observed_minimizer_algorithm_available = false;
    bool observed_chaining_algorithm_available = false;
    bool observed_base_alignment_algorithm_available = false;
    bool observed_minimizer_algorithm_conflict = false;
    bool observed_chaining_algorithm_conflict = false;
    bool observed_base_alignment_algorithm_conflict = false;
#endif
    // Count one timing result per pipeline invocation, even when several
    // query workers contributed to its combined active interval.
    std::uint64_t downstream_active_wall_ns = 0;
    std::uint64_t downstream_active_wall_invocations = 0;
    std::uint64_t downstream_active_wall_unavailable_invocations = 0;
    bool downstream_active_wall_saturated = false;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    bool mapq_execution_available = false;
    bool mapq_executed = false;
    bool mapq_execution_conflict = false;
#endif
    // Keep CPU stage durations with  each result. PostgreSQL adds them to
    // statement totals after dispatch, avoiding shared writes by query workers.
    std::uint64_t query_sketch_host_wall_ns = 0;
    std::uint64_t persisted_index_decode_host_wall_ns = 0;
    std::uint64_t seed_lookup_host_wall_ns = 0;
    std::uint64_t chain_score_host_wall_ns = 0;
    std::uint64_t region_generation_host_wall_ns = 0;
    std::uint64_t alignment_result_host_wall_ns = 0;
    bool is_host_stage_timing_saturated = false;
    // Track stages while the pipeline is running, then turn their start
    // times into elapsed durations before returning the result.
    std::uint64_t region_host_stage_started_ns = 0;
    std::uint64_t alignment_result_host_stage_started_ns = 0;
    bool is_region_host_stage_active = false;
    bool is_alignment_result_host_stage_active = false;
    downstream_active_wall_tracker* downstream_active_wall_tracker_ = nullptr;
    bool downstream_active_wall_tracker_entered_ = false;

    /// Create an empty result collection and execution-stat aggregate.
    match_result_collection() = default;

    /// Adopt a completed ordered result vector.
    explicit match_result_collection(match_result_vector&& results_vec)
        : results(std::move(results_vec)) {}

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    /// Keep one completed target-generation record. Reject invalid, repeated or
    /// already-published records and keep the conflict flag set.
    [[nodiscard]] bool record_transient_cpu_target_generation_receipt(
        const transient_cpu_target_generation_representation representation,
        bioseq::indexing::minimizer_sequence_generation_execution_receipt receipt) noexcept {
        const bool terminal_receipt_valid =
            representation ==
                    transient_cpu_target_generation_representation::minimizer_fragment_spill
                ? receipt.is_spill_valid()
            : representation ==
                    transient_cpu_target_generation_representation::minimizer_sequence_publication
                ? receipt.is_internally_consistent()
                : false;
        if (!terminal_receipt_valid || transient_cpu_target_generation_receipt.has_value() ||
            transient_cpu_target_generation_representation_kind !=
                transient_cpu_target_generation_representation::none ||
            transient_cpu_target_generation_receipt_conflict ||
            transient_cpu_target_generation_stats_published) {
            transient_cpu_target_generation_receipt_conflict = true;
            return false;
        }
        transient_cpu_target_generation_receipt.emplace(std::move(receipt));
        transient_cpu_target_generation_representation_kind = representation;
        return true;
    }

  private:
    /// Move the target-generation record during collection merging.
    /// Duplicate, malformed or already-published records leave a conflict flag.
    void retain_transient_cpu_target_generation_receipt(
        match_result_collection& source) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<
                      bioseq::indexing::minimizer_sequence_generation_execution_receipt>);
        if (this == &source) {
            transient_cpu_target_generation_receipt_conflict = true;
            return;
        }
        if (source.transient_cpu_target_generation_receipt_conflict ||
            source.transient_cpu_target_generation_stats_published) {
            transient_cpu_target_generation_receipt_conflict = true;
        }
        if (!source.transient_cpu_target_generation_receipt.has_value()) {
            if (source.transient_cpu_target_generation_representation_kind !=
                transient_cpu_target_generation_representation::none) {
                transient_cpu_target_generation_receipt_conflict = true;
            }
            return;
        }
        if (transient_cpu_target_generation_receipt.has_value() ||
            transient_cpu_target_generation_representation_kind !=
                transient_cpu_target_generation_representation::none ||
            transient_cpu_target_generation_stats_published) {
            transient_cpu_target_generation_receipt_conflict = true;
            return;
        }
        transient_cpu_target_generation_receipt.emplace(
            std::move(*source.transient_cpu_target_generation_receipt));
        transient_cpu_target_generation_representation_kind =
            source.transient_cpu_target_generation_representation_kind;
        source.transient_cpu_target_generation_receipt.reset();
        source.transient_cpu_target_generation_representation_kind =
            transient_cpu_target_generation_representation::none;
    }

  public:
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    /**
     * @brief Reserve  result cwpacity before appending the next batch.
     * @param additional_upper_bound Maximum results the batch may append.
     * @return Maximum total result count after the append.
     */
    [[gnu::always_inline]] std::size_t
    prepare_result_append(const std::size_t additional_upper_bound) {
        if (additional_upper_bound >
            (std::numeric_limits<std::size_t>::max)() - results.size()) {
            throw std::length_error{"match result append bound overflowed"};
        }
        const std::size_t required = results.size() + additional_upper_bound;
        // Grow spare capacity geometrically across batches. Keep the allowed
        // result count separate from this larger allocation bound.
        std::size_t capacity_bound = results.capacity();
        if (required > capacity_bound) {
            const std::size_t maximum_capacity = std::vector<match_result>{}.max_size();
            const std::size_t grown_capacity = capacity_bound <= maximum_capacity / 2u
                ? capacity_bound * 2u
                : maximum_capacity;
            capacity_bound = std::max(required, grown_capacity);
        }
        results.set_logical_bounds(required, capacity_bound);
        results.reserve(capacity_bound);
        return required;
    }
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    /// Record the minimizer used by a completed pipeline and flag later disagreement.
    void
    record_completed_minimizer_algorithm_identity(const minimizer_algorithm minimizer) noexcept {
        if (!observed_minimizer_algorithm_available) {
            observed_minimizer_algorithm = minimizer;
            observed_minimizer_algorithm_available = true;
        } else if (observed_minimizer_algorithm != minimizer) {
            observed_minimizer_algorithm_conflict = true;
        }
    }

    /// Record the chainer used by  a completed pipeline and flag later disagreement.
    void record_completed_chaining_algorithm_identity(const chaining_algorithm chaining) noexcept {
        if (!observed_chaining_algorithm_available) {
            observed_chaining_algorithm = chaining;
            observed_chaining_algorithm_available = true;
        } else if (observed_chaining_algorithm != chaining) {
            observed_chaining_algorithm_conflict = true;
        }
    }

    /// Record the CPU aligner, after its kernel returns and flag later disagreement.
    void record_completed_base_alignment_algorithm_identity(
        const base_alignment_algorithm alignment) noexcept {
        if (!observed_base_alignment_algorithm_available) {
            observed_base_alignment_algorithm = alignment;
            observed_base_alignment_algorithm_available = true;
        } else if (observed_base_alignment_algorithm != alignment) {
            observed_base_alignment_algorithm_conflict = true;
        }
    }

    /// Record the minimizer and chainer used by a completed pipeline
    void record_completed_algorithm_identity(const minimizer_algorithm minimizer,
                                             const chaining_algorithm chaining) noexcept {
        record_completed_minimizer_algorithm_identity(minimizer);
        record_completed_chaining_algorithm_identity(chaining);
    }

    /// Record whether MAPQ ran and flag disagreement between completed pipelines.
    void record_mapq_execution(const bool executed) noexcept {
        if (!mapq_execution_available) {
            mapq_execution_available = true;
            mapq_executed = executed;
        } else if (mapq_executed != executed) {
            mapq_execution_conflict = true;
        }
    }
#endif

    /// Connect this query's downstream timing events to the shared invocation tracker.
    void attach_downstream_active_wall_tracker(downstream_active_wall_tracker& tracker) noexcept {
        if (downstream_active_wall_tracker_ != nullptr || downstream_active_wall_tracker_entered_) {
            tracker.invalidate();
            return;
        }
        downstream_active_wall_tracker_ = &tracker;
    }

    /// Detach the invocation trackeh after the pipeline finishes. An unmatched
    /// active interval is closed and marked invalid.
    void detach_downstream_active_wall_tracker() noexcept {
        if (downstream_active_wall_tracker_entered_ && downstream_active_wall_tracker_ != nullptr) {
            downstream_active_wall_tracker_->invalidate();
            downstream_active_wall_tracker_->leave();
        }
        downstream_active_wall_tracker_entered_ = false;
        downstream_active_wall_tracker_ = nullptr;
    }

    /// Add one completed invocation's active  time or unavailable-timing count.
    void
    record_downstream_active_wall_receipt(const downstream_active_wall_receipt& receipt) noexcept {
        if (downstream_active_wall_invocations == (std::numeric_limits<std::uint64_t>::max)()) {
            downstream_active_wall_saturated = true;
        } else {
            ++downstream_active_wall_invocations;
        }
        if (!receipt.available) {
            if (downstream_active_wall_unavailable_invocations ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                downstream_active_wall_saturated = true;
            } else {
                ++downstream_active_wall_unavailable_invocations;
            }
            return;
        }
        if (receipt.elapsed_ns >
            (std::numeric_limits<std::uint64_t>::max)() - downstream_active_wall_ns) {
            downstream_active_wall_saturated = true;
            downstream_active_wall_ns = (std::numeric_limits<std::uint64_t>::max)();
            return;
        }
        downstream_active_wall_ns += receipt.elapsed_ns;
    }

    /// Merge completed exact-SIMD query-sketch statistics.
    void record_exact_simd_query_execution_stats(const two_stack_execution_stats& stats) noexcept {
        exact_simd_query_execution_stats.merge(stats);
        exact_simd_query_execution_stats_available = true;
    }

    /// Merge completed exact-SIMD target-sketch statistics.
    void record_exact_simd_target_execution_stats(const two_stack_execution_stats& stats) noexcept {
        exact_simd_target_execution_stats.merge(stats);
        exact_simd_target_execution_stats_available = true;
    }

    /// Merge completed bounded-ntHash query-sketch statistics.
    void record_bounded_nthash_query_execution_stats(
        const bounded_nthash_execution_stats& stats) noexcept {
        bounded_nthash_query_execution_stats.merge(stats);
        bounded_nthash_query_execution_stats_available = true;
    }

    /// Merge completed bounded-ntHash target-sketch statistics.
    void record_bounded_nthash_target_execution_stats(
        const bounded_nthash_execution_stats& stats) noexcept {
        bounded_nthash_target_execution_stats.merge(stats);
        bounded_nthash_target_execution_stats_available = true;
    }

    /// Merge completed SIMD chaining statistics.
    void record_simd_chaining_execution_stats(
        const bioseq::chaining::simd_chaining_execution_stats& stats) noexcept {
        simd_chaining_stats.merge(stats);
        simd_chaining_execution_stats_available = true;
    }

    /// Merge completed Block Aligner statistics.
    void record_block_aligner_execution_stats(const block_aligner_execution_stats& stats) noexcept {
        block_aligner_stats.merge(stats);
        block_aligner_execution_stats_available = true;
    }

    /// Merge completed scalar pipeline statistics.
    void
    record_scalar_pipeline_execution_stats(const scalar_pipeline_execution_stats& stats) noexcept {
        scalar_pipeline_stats.merge(stats);
        scalar_pipeline_execution_stats_available = true;
    }

    /// Merge completed batched-KSW2 statistics.
    void record_ksw2_batched_execution_stats(const ksw2_batched_execution_stats& stats) noexcept {
        ksw2_batched_stats.merge(stats);
        ksw2_batched_execution_stats_available = true;
    }

    /// Merge completed prefilter statistics.
    void record_prefilter_execution_stats(
        const ::bioseq::prefilter::prefilter_execution_stats& stats) noexcept {
        prefilter_stats.merge(stats);
        prefilter_execution_stats_available = true;
    }

    /// Add scalar query-sketch work to the query part of the CPU statistics.
    void record_scalar_query_minimizer_execution_stats(
        const scalar_minimizer_execution_stats& stats) noexcept {
        scalar_pipeline_execution_stats routed{};
        routed.query_minimizers = stats;
        record_scalar_pipeline_execution_stats(routed);
    }

    /// Add scalar target-sketch work to the target part of the CPU statistics.
    void record_scalar_target_minimizer_execution_stats(
        const scalar_minimizer_execution_stats& stats) noexcept {
        scalar_pipeline_execution_stats routed{};
        routed.target_minimizers = stats;
        record_scalar_pipeline_execution_stats(routed);
    }

    /// Merge completed partial-Vulkan statistics.
    void
    record_partial_vulkan_execution_stats(const partial_vulkan_execution_stats& stats) noexcept {
        partial_vulkan_stats.merge(stats);
    }

    /// Add statistics from query lookup directly over minimizer fragments.
    void record_fragment_native_query_stats(
        const bioseq::indexing::prepared_fragment_anchor_batch_stats& stats) noexcept {
        fragment_native_query_stats.merge(stats);
        fragment_native_query_stats_available = true;
    }

    /// Add host-stage nanoseconds, clamping overflow and keeping its failure flag.
    void record_host_stage_time(std::uint64_t& destination,
                                const std::uint64_t elapsed_ns) noexcept {
        if (elapsed_ns > (std::numeric_limits<std::uint64_t>::max)() - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            is_host_stage_timing_saturated = true;
            return;
        }
        destination += elapsed_ns;
    }

    /// Add query-sketch host time.
    void record_query_sketch_host_time(const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(query_sketch_host_wall_ns, elapsed_ns);
    }

    /// Add persisted-index decode host time.
    void record_persisted_index_decode_host_time(const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(persisted_index_decode_host_wall_ns, elapsed_ns);
    }

    /// Add seed-lookup host time.
    void record_seed_lookup_host_time(const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(seed_lookup_host_wall_ns, elapsed_ns);
    }

    /// Add chain-score host time.
    void record_chain_score_host_time(const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(chain_score_host_wall_ns, elapsed_ns);
    }

    /// Add region-generation host time.
    void record_region_generation_host_time(const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(region_generation_host_wall_ns, elapsed_ns);
    }

    /// Add host time for alignment and result construction.
    void record_alignment_result_host_time(const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(alignment_result_host_wall_ns, elapsed_ns);
    }

    /// Start timing region generation  and selection after chaining.
    /// Overlapping stage starts invalidate timing.
    void begin_region_host_stage(const std::uint64_t started_ns) noexcept {
        if (is_region_host_stage_active || is_alignment_result_host_stage_active) {
            is_host_stage_timing_saturated = true;
            if (downstream_active_wall_tracker_ != nullptr) {
                downstream_active_wall_tracker_->invalidate();
            }
        }
        region_host_stage_started_ns = started_ns;
        is_region_host_stage_active = true;
    }

    /// Finish region timing and, when enabled, start alignment/result timing.
    /// The shared tracker records its own synchronized start for the combined
    /// active-time measurement across queries.
    void transition_region_to_alignment_result_host_stage(const std::uint64_t boundary_ns,
                                                          const bool downstream_enabled) noexcept {
        if (!is_region_host_stage_active || boundary_ns < region_host_stage_started_ns ||
            is_alignment_result_host_stage_active) {
            is_host_stage_timing_saturated = true;
            if (downstream_active_wall_tracker_ != nullptr) {
                downstream_active_wall_tracker_->invalidate();
            }
            downstream_active_wall_tracker_entered_ = false;
            is_region_host_stage_active = false;
            is_alignment_result_host_stage_active = false;
            return;
        }
        record_region_generation_host_time(
            std::max<std::uint64_t>(1u, boundary_ns - region_host_stage_started_ns));
        is_region_host_stage_active = false;
        if (downstream_enabled) {
            alignment_result_host_stage_started_ns = boundary_ns;
            is_alignment_result_host_stage_active = true;
            if (downstream_active_wall_tracker_ != nullptr) {
                downstream_active_wall_tracker_->enter();
                downstream_active_wall_tracker_entered_ = true;
            }
        }
    }

    /// Finish timing alignment, filters, MAPQ and result construction at the sink.
    void finish_alignment_result_host_stage(const std::uint64_t finished_ns) noexcept {
        if (!is_alignment_result_host_stage_active) {
            return;
        }
        if (finished_ns < alignment_result_host_stage_started_ns) {
            is_host_stage_timing_saturated = true;
            if (downstream_active_wall_tracker_entered_ &&
                downstream_active_wall_tracker_ != nullptr) {
                downstream_active_wall_tracker_->invalidate();
            }
            downstream_active_wall_tracker_entered_ = false;
            is_alignment_result_host_stage_active = false;
            return;
        }
        if (downstream_active_wall_tracker_entered_ && downstream_active_wall_tracker_ != nullptr) {
            downstream_active_wall_tracker_->leave();
        }
        record_alignment_result_host_time(
            std::max<std::uint64_t>(1u, finished_ns - alignment_result_host_stage_started_ns));
        downstream_active_wall_tracker_entered_ = false;
        is_alignment_result_host_stage_active = false;
    }

    /// Combine stage statistics while leaving alignment results in, place
    /// Test and benchmark builds also move saved  operation records. Benchmark
    /// merges may alocate storage for additional Vulkan tokens.
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    void combine_execution_stats(match_result_collection& other) {
        retain_completed_vulkan_chaining(other);
        retain_transient_cpu_target_generation_receipt(other);
#elif defined(BIOSEQ_TEST_INDEXES)
    void combine_execution_stats(match_result_collection& other) noexcept {
        retain_transient_cpu_target_generation_receipt(other);
#else
    void combine_execution_stats(const match_result_collection& other) noexcept {
#endif
        if (other.exact_simd_query_execution_stats_available) {
            record_exact_simd_query_execution_stats(other.exact_simd_query_execution_stats);
        }
        if (other.exact_simd_target_execution_stats_available) {
            record_exact_simd_target_execution_stats(other.exact_simd_target_execution_stats);
        }
        if (other.bounded_nthash_query_execution_stats_available) {
            record_bounded_nthash_query_execution_stats(other.bounded_nthash_query_execution_stats);
        }
        if (other.bounded_nthash_target_execution_stats_available) {
            record_bounded_nthash_target_execution_stats(
                other.bounded_nthash_target_execution_stats);
        }
        if (other.simd_chaining_execution_stats_available) {
            record_simd_chaining_execution_stats(other.simd_chaining_stats);
        }
        if (other.block_aligner_execution_stats_available) {
            record_block_aligner_execution_stats(other.block_aligner_stats);
        }
        if (other.scalar_pipeline_execution_stats_available) {
            record_scalar_pipeline_execution_stats(other.scalar_pipeline_stats);
        }
        if (other.ksw2_batched_execution_stats_available) {
            record_ksw2_batched_execution_stats(other.ksw2_batched_stats);
        }
        if (other.prefilter_execution_stats_available) {
            record_prefilter_execution_stats(other.prefilter_stats);
        }
        partial_vulkan_stats.merge(other.partial_vulkan_stats);
        if (other.fragment_native_query_stats_available) {
            record_fragment_native_query_stats(other.fragment_native_query_stats);
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (other.observed_minimizer_algorithm_available) {
            if (!observed_minimizer_algorithm_available) {
                observed_minimizer_algorithm = other.observed_minimizer_algorithm;
                observed_minimizer_algorithm_available = true;
            } else if (observed_minimizer_algorithm != other.observed_minimizer_algorithm) {
                observed_minimizer_algorithm_conflict = true;
            }
        }
        if (other.observed_chaining_algorithm_available) {
            if (!observed_chaining_algorithm_available) {
                observed_chaining_algorithm = other.observed_chaining_algorithm;
                observed_chaining_algorithm_available = true;
            } else if (observed_chaining_algorithm != other.observed_chaining_algorithm) {
                observed_chaining_algorithm_conflict = true;
            }
        }
        if (other.observed_base_alignment_algorithm_available) {
            if (!observed_base_alignment_algorithm_available) {
                observed_base_alignment_algorithm = other.observed_base_alignment_algorithm;
                observed_base_alignment_algorithm_available = true;
            } else if (observed_base_alignment_algorithm !=
                       other.observed_base_alignment_algorithm) {
                observed_base_alignment_algorithm_conflict = true;
            }
        }
        observed_minimizer_algorithm_conflict =
            observed_minimizer_algorithm_conflict || other.observed_minimizer_algorithm_conflict ||
            (!other.observed_minimizer_algorithm_available &&
             other.observed_minimizer_algorithm != minimizer_algorithm::scalar);
        observed_chaining_algorithm_conflict =
            observed_chaining_algorithm_conflict || other.observed_chaining_algorithm_conflict ||
            (!other.observed_chaining_algorithm_available &&
             other.observed_chaining_algorithm != chaining_algorithm::scalar_minimap2);
        observed_base_alignment_algorithm_conflict =
            observed_base_alignment_algorithm_conflict ||
            other.observed_base_alignment_algorithm_conflict ||
            (!other.observed_base_alignment_algorithm_available &&
             other.observed_base_alignment_algorithm != base_alignment_algorithm::ksw2);
        if (other.mapq_execution_available) {
            record_mapq_execution(other.mapq_executed);
        } else if (other.mapq_executed) {
            mapq_execution_conflict = true;
        }
        mapq_execution_conflict = mapq_execution_conflict || other.mapq_execution_conflict;
#endif
        const auto merge_downstream_counter = [this](std::uint64_t& destination,
                                                     const std::uint64_t incoming) noexcept {
            if (incoming > (std::numeric_limits<std::uint64_t>::max)() - destination) {
                destination = (std::numeric_limits<std::uint64_t>::max)();
                downstream_active_wall_saturated = true;
            } else {
                destination += incoming;
            }
        };
        merge_downstream_counter(downstream_active_wall_ns, other.downstream_active_wall_ns);
        merge_downstream_counter(downstream_active_wall_invocations,
                                 other.downstream_active_wall_invocations);
        merge_downstream_counter(downstream_active_wall_unavailable_invocations,
                                 other.downstream_active_wall_unavailable_invocations);
        if (other.downstream_active_wall_unavailable_invocations >
                other.downstream_active_wall_invocations ||
            (other.downstream_active_wall_invocations == 0u &&
             other.downstream_active_wall_ns != 0u)) {
            downstream_active_wall_saturated = true;
        }
        downstream_active_wall_saturated =
            downstream_active_wall_saturated || other.downstream_active_wall_saturated;
        record_query_sketch_host_time(other.query_sketch_host_wall_ns);
        record_persisted_index_decode_host_time(other.persisted_index_decode_host_wall_ns);
        record_seed_lookup_host_time(other.seed_lookup_host_wall_ns);
        record_chain_score_host_time(other.chain_score_host_wall_ns);
        record_region_generation_host_time(other.region_generation_host_wall_ns);
        record_alignment_result_host_time(other.alignment_result_host_wall_ns);
        is_host_stage_timing_saturated =
            is_host_stage_timing_saturated || other.is_host_stage_timing_saturated ||
            is_region_host_stage_active || is_alignment_result_host_stage_active ||
            other.is_region_host_stage_active || other.is_alignment_result_host_stage_active;
    }

    /// Check recorded exact-SIMD query work, requiring vector activity when requested.
    [[nodiscard]] bool
    is_exact_simd_query_execution_consistent(bool require_nonempty_vector_work) const noexcept {
        return exact_simd_query_execution_stats_available &&
               exact_simd_query_execution_stats.is_internally_consistent(
                   require_nonempty_vector_work);
    }

    /// Check recorded exact-SIMD target work, requiring vector activity when requested.
    [[nodiscard]] bool
    is_exact_simd_target_execution_consistent(bool require_nonempty_vector_work) const noexcept {
        return exact_simd_target_execution_stats_available &&
               exact_simd_target_execution_stats.is_internally_consistent(
                   require_nonempty_vector_work);
    }

    /// Check bounded-ntHash query work, requiring vector activity when requested.
    [[nodiscard]] bool
    is_bounded_nthash_query_execution_consistent(bool require_nonempty_vector_work) const noexcept {
        return bounded_nthash_query_execution_stats_available &&
               bounded_nthash_query_execution_stats.is_internally_consistent(
                   require_nonempty_vector_work);
    }

    /// Check bounded-ntHash target work, requiring vector activity when requested.
    [[nodiscard]] bool is_bounded_nthash_target_execution_consistent(
        bool require_nonempty_vector_work) const noexcept {
        return bounded_nthash_target_execution_stats_available &&
               bounded_nthash_target_execution_stats.is_internally_consistent(
                   require_nonempty_vector_work);
    }

    /// Check SIMD chaining against the selected policy and required vector activity.
    [[nodiscard]] bool
    is_simd_chaining_execution_consistent(simd_chaining_policy selected_policy,
                                          bool require_nonempty_vector_work) const noexcept {
        return simd_chaining_execution_stats_available &&
               simd_chaining_stats.is_internally_consistent(selected_policy,
                                                            require_nonempty_vector_work);
    }

    /// Check the batched-KSW2 record, requiring completed work when requested.
    [[nodiscard]] bool
    is_ksw2_batched_execution_consistent(const bool require_nonempty_work) const noexcept {
        return ksw2_batched_execution_stats_available &&
               ksw2_batched_stats.is_coherent(require_nonempty_work);
    }

    /// Check the selected SIMD sketcher and require  all Vulkan statistics to be empty.
    [[nodiscard]] bool
    is_selected_simd_sketch_execution_consistent(minimizer_algorithm selected,
                                                 bool require_target_sketch,
                                                 bool require_nonempty_vector_work) const noexcept {
        return partial_vulkan_stats == partial_vulkan_execution_stats{} &&
               is_selected_simd_sketch_component_consistent(
                   selected, require_target_sketch, require_nonempty_vector_work);
    }

    /// Check the selected SIMD sketcher without judging other pipeline stages.
    /// Require query work and the requested target work, with records for the
    /// other SIMD sketcher empty.
    [[nodiscard]] bool
    is_selected_simd_sketch_component_consistent(minimizer_algorithm selected,
                                                bool require_target_sketch,
                                                bool require_nonempty_vector_work) const noexcept {

        const bool exact_query_absent =
            !exact_simd_query_execution_stats_available &&
            exact_simd_query_execution_stats == two_stack_execution_stats{};
        const bool exact_target_absent =
            !exact_simd_target_execution_stats_available &&
            exact_simd_target_execution_stats == two_stack_execution_stats{};
        const bool bounded_query_absent =
            !bounded_nthash_query_execution_stats_available &&
            bounded_nthash_query_execution_stats == bounded_nthash_execution_stats{};
        const bool bounded_target_absent =
            !bounded_nthash_target_execution_stats_available &&
            bounded_nthash_target_execution_stats == bounded_nthash_execution_stats{};

        switch (selected) {
        case minimizer_algorithm::two_stack_nthash:
            return bounded_query_absent && bounded_target_absent &&
                   is_exact_simd_query_execution_consistent(require_nonempty_vector_work) &&
                   (require_target_sketch
                        ? is_exact_simd_target_execution_consistent(require_nonempty_vector_work)
                        : exact_target_absent);
        case minimizer_algorithm::bounded_nthash_simd:
            return exact_query_absent && exact_target_absent &&
                   is_bounded_nthash_query_execution_consistent(require_nonempty_vector_work) &&
                   (require_target_sketch ? is_bounded_nthash_target_execution_consistent(
                                                require_nonempty_vector_work)
                                          : bounded_target_absent);
        default:
            return false;
        }
    }

    /// Check SIMD sketching, chaining and Block Aligner together with CPU seed
    /// lookup. Scalar sketching, chaining and KSW2 work must be absent.
    [[nodiscard]] bool
    is_all_simd_execution_consistent(minimizer_algorithm selected,
                                     simd_chaining_policy selected_chaining_policy,
                                     bool require_target_sketch,
                                     bool require_nonempty_vector_work) const noexcept {
        const auto& scalar = scalar_pipeline_stats;
        const bool is_scalar_seed_only_consistent =
            scalar_pipeline_execution_stats_available && !scalar.is_saturated &&
            scalar.query_minimizers == scalar_minimizer_execution_stats{} &&
            scalar.target_minimizers == scalar_minimizer_execution_stats{} &&
            scalar.seed_lookup_calls > 0u &&
            (!require_nonempty_vector_work || scalar.seed_query_minimizers > 0u) &&
            scalar.chain_stage_completion_calls == 0u &&
            scalar.chain_stage_completed_anchor_inputs == 0u && scalar.chain_score_calls == 0u &&
            scalar.chain_backtrack_calls == 0u && scalar.chain_anchor_inputs == 0u &&
            scalar.ksw2_left_calls == 0u && scalar.ksw2_gap_calls == 0u &&
            scalar.ksw2_right_calls == 0u && scalar.ksw2_input_cell_area == 0u;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        const bool exact_identity =
            observed_minimizer_algorithm_available && observed_chaining_algorithm_available &&
            observed_base_alignment_algorithm_available && !observed_minimizer_algorithm_conflict &&
            !observed_chaining_algorithm_conflict && !observed_base_alignment_algorithm_conflict &&
            observed_minimizer_algorithm == selected &&
            observed_chaining_algorithm == chaining_algorithm::simd_minimap2 &&
            observed_base_alignment_algorithm == base_alignment_algorithm::block_aligner;
        if (!exact_identity) {
            return false;
        }
#endif
        return is_scalar_seed_only_consistent && !ksw2_batched_execution_stats_available &&
               ksw2_batched_stats == ksw2_batched_execution_stats{} &&
               is_selected_simd_sketch_execution_consistent(
                   selected, require_target_sketch, require_nonempty_vector_work) &&
               is_simd_chaining_execution_consistent(selected_chaining_policy,
                                                     require_nonempty_vector_work) &&
               is_block_aligner_execution_consistent(require_nonempty_vector_work);
    }

    /// Check scalar sketching, seed lookup, chaining and any required KSW2 alignment.
    /// Reject SIMD, Block Aligner, batched-KSW2 and Vulkan records.
    [[nodiscard]] bool
    is_all_scalar_execution_consistent(const bool require_target_sketch,
                                       const bool require_alignment) const noexcept {
        const bool simd_chain_absent =
            !simd_chaining_execution_stats_available &&
            simd_chaining_stats == bioseq::chaining::simd_chaining_execution_stats{};
        const bool block_absent = !block_aligner_execution_stats_available &&
                                  block_aligner_stats == block_aligner_execution_stats{};
        const bool batched_absent = !ksw2_batched_execution_stats_available &&
                                    ksw2_batched_stats == ksw2_batched_execution_stats{};
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        const bool exact_identity =
            observed_minimizer_algorithm_available && observed_chaining_algorithm_available &&
            (!require_alignment || observed_base_alignment_algorithm_available) &&
            !observed_minimizer_algorithm_conflict && !observed_chaining_algorithm_conflict &&
            !observed_base_alignment_algorithm_conflict &&
            observed_minimizer_algorithm == minimizer_algorithm::scalar &&
            observed_chaining_algorithm == chaining_algorithm::scalar_minimap2 &&
            (!require_alignment ||
             observed_base_alignment_algorithm == base_alignment_algorithm::ksw2);
        if (!exact_identity) {
            return false;
        }
#endif
        return is_selected_scalar_sketch_execution_consistent(require_target_sketch,
                                                              /*require_nonempty_work=*/true) &&
               scalar_pipeline_stats.is_route_consistent(require_target_sketch,
                                                         require_alignment) &&
               simd_chain_absent && block_absent && batched_absent &&
               partial_vulkan_stats == partial_vulkan_execution_stats{};
    }

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    /// Check the seleced  sketch, seed, chaining and CPU alignment components.
    /// GPU seed jobs also need a valid shared job record. Their saved runtime
    /// token and index storage are checked at the producer boundary.
    [[nodiscard]] bool
    is_selected_components_execution_consistent_for_observed(
        minimizer_algorithm selected_minimizer,
        chaining_algorithm selected_chaining,
        base_alignment_algorithm selected_alignment,
        simd_chaining_policy selected_chaining_policy,
        bool require_target_sketch,
        bool require_alignment,
        minimizer_algorithm observed_minimizer,
        chaining_algorithm observed_chaining,
        base_alignment_algorithm observed_alignment,
        const vulkan_seed_frontend_job_execution* shared_vulkan_seed = nullptr) const noexcept {
        const auto& scalar = scalar_pipeline_stats;
        const auto& gpu = partial_vulkan_stats;
        const bool gpu_seed = selected_minimizer == minimizer_algorithm::vulkan_minimap_integer;
        const bool gpu_chain =
            selected_chaining == chaining_algorithm::vulkan_tiled_dynamic_programming;
        const bool minimizer_identity_valid =
            observed_minimizer == selected_minimizer ||
             (selected_minimizer == minimizer_algorithm::simd_minimap2_reference &&
              observed_minimizer == minimizer_algorithm::scalar);
        const bool chaining_identity_valid = observed_chaining == selected_chaining;
        const bool alignment_identity_valid =
            !require_alignment || observed_alignment == selected_alignment;
        const bool seed_lookup_valid = gpu_seed
            ? shared_vulkan_seed != nullptr &&
                  shared_vulkan_seed->is_internally_consistent() &&
                  scalar.seed_lookup_calls == 0u && scalar.seed_query_minimizers == 0u
            : scalar_pipeline_execution_stats_available && scalar.seed_lookup_calls > 0u &&
                  scalar.seed_query_minimizers > 0u;
        if (!minimizer_identity_valid || !chaining_identity_valid || !alignment_identity_valid ||
            !seed_lookup_valid || scalar.is_saturated || ksw2_batched_execution_stats_available ||
            ksw2_batched_stats != ksw2_batched_execution_stats{}) {
            return false;
        }

        if (!gpu_seed) {
            // Host seeding permits GPU statistics only for the selected Vulkan
            // chainer. Comparing the full record catches unrelated GPU work
            partial_vulkan_execution_stats expected{};
            if (gpu_chain) {
                if (gpu.chain_score_dispatches == 0u) {
                    return false;
                }
                expected.chain_score_dispatches = gpu.chain_score_dispatches;
            }
            if (gpu != expected) {
                return false;
            }
        } else {
            partial_vulkan_execution_stats expected{};
            if (gpu_chain) {
                expected.chain_score_dispatches = gpu.chain_score_dispatches;
            }
            if (gpu != expected || require_target_sketch) {
                return false;
            }
        }

        const bool detailed_sketch_stats_absent =
            scalar.query_minimizers == scalar_minimizer_execution_stats{} &&
            scalar.target_minimizers == scalar_minimizer_execution_stats{} &&
            !exact_simd_query_execution_stats_available &&
            !exact_simd_target_execution_stats_available &&
            exact_simd_query_execution_stats == two_stack_execution_stats{} &&
            exact_simd_target_execution_stats == two_stack_execution_stats{} &&
            !bounded_nthash_query_execution_stats_available &&
            !bounded_nthash_target_execution_stats_available &&
            bounded_nthash_query_execution_stats == bounded_nthash_execution_stats{} &&
            bounded_nthash_target_execution_stats == bounded_nthash_execution_stats{};
        const bool sketch_valid = [this, selected_minimizer, observed_minimizer,
                                    require_target_sketch,
                                    detailed_sketch_stats_absent]() noexcept {
            switch (selected_minimizer) {
            case minimizer_algorithm::scalar:
                return is_selected_scalar_sketch_component_consistent(
                    require_target_sketch, /*require_nonempty_work=*/true);
            case minimizer_algorithm::two_stack_nthash:
            case minimizer_algorithm::bounded_nthash_simd:
                return scalar_pipeline_stats.query_minimizers == scalar_minimizer_execution_stats{} &&
                       scalar_pipeline_stats.target_minimizers == scalar_minimizer_execution_stats{} &&
                       is_selected_simd_sketch_component_consistent(
                           selected_minimizer, require_target_sketch,
                           /*require_nonempty_vector_work=*/true);
            case minimizer_algorithm::simd_minimap2_reference:
                if (observed_minimizer == minimizer_algorithm::scalar) {
                    // DNA5 can execute the scalar sketcher when the reference SIMD
                    // sketcher was requested. Keep the recorded effective choice.
                    return is_selected_scalar_sketch_component_consistent(
                               require_target_sketch, /*require_nonempty_work=*/true) ||
                           detailed_sketch_stats_absent;
                }
                return detailed_sketch_stats_absent;
            case minimizer_algorithm::vulkan_minimap_integer:
                return detailed_sketch_stats_absent;
            }
            return false;
        }();

        const bool scalar_chain_observed = scalar.chain_stage_completion_calls > 0u &&
                                           scalar.chain_score_calls == scalar.chain_backtrack_calls;
        const bool scalar_minimap2_chain_absent =
            scalar.chain_stage_completion_calls == 0u &&
            scalar.chain_stage_completed_anchor_inputs == 0u && scalar.chain_score_calls == 0u &&
            scalar.chain_backtrack_calls == 0u && scalar.chain_anchor_inputs == 0u;
        const bool simd_chain_absent =
            !simd_chaining_execution_stats_available &&
            simd_chaining_stats == bioseq::chaining::simd_chaining_execution_stats{};
        const bool chain_valid = [this, selected_chaining, selected_chaining_policy,
                                   scalar_chain_observed, scalar_minimap2_chain_absent,
                                   simd_chain_absent]() noexcept {
            switch (selected_chaining) {
            case chaining_algorithm::scalar_minimap2:
                return scalar_chain_observed && simd_chain_absent;
            case chaining_algorithm::range_minimum_query:
                return scalar_chain_observed && simd_chain_absent;
            case chaining_algorithm::vulkan_tiled_dynamic_programming:
                // Vulkan chaining backtracks on the host as part of its own call.
                // Scalar minimap2 and RMQ counters must  stay empty here.
                return scalar_minimap2_chain_absent && simd_chain_absent;
            case chaining_algorithm::simd_minimap2:
                return scalar_minimap2_chain_absent &&
                       is_simd_chaining_execution_consistent(selected_chaining_policy,
                                                             /*require_nonempty_vector_work=*/true);
            }
            return false;
        }();

        const bool scalar_alignment_observed = scalar.ksw2_left_calls != 0u ||
                                               scalar.ksw2_gap_calls != 0u ||
                                               scalar.ksw2_right_calls != 0u;
        const bool block_absent = !block_aligner_execution_stats_available &&
                                  block_aligner_stats == block_aligner_execution_stats{};
        const bool alignment_valid =
            selected_alignment == base_alignment_algorithm::ksw2
                ? block_absent && (!require_alignment ||
                                   (scalar_alignment_observed && scalar.ksw2_input_cell_area > 0u))
                : selected_alignment == base_alignment_algorithm::block_aligner &&
                      !scalar_alignment_observed && scalar.ksw2_input_cell_area == 0u &&
                      is_block_aligner_execution_consistent(require_alignment);
        return sketch_valid && chain_valid && alignment_valid;
    }

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    /// Validate the selected components using the algorithms recorded by this result.
    [[nodiscard]] bool
    is_selected_components_execution_consistent(
        minimizer_algorithm selected_minimizer,
        chaining_algorithm selected_chaining,
        base_alignment_algorithm selected_alignment,
        simd_chaining_policy selected_chaining_policy,
        bool require_target_sketch,
        bool require_alignment,
        const vulkan_seed_frontend_job_execution* shared_vulkan_seed = nullptr) const noexcept {
        if (!observed_minimizer_algorithm_available ||
            !observed_chaining_algorithm_available ||
            (require_alignment && !observed_base_alignment_algorithm_available) ||
            observed_minimizer_algorithm_conflict || observed_chaining_algorithm_conflict ||
            observed_base_alignment_algorithm_conflict) {
            return false;
        }
        return is_selected_components_execution_consistent_for_observed(
            selected_minimizer,
            selected_chaining,
            selected_alignment,
            selected_chaining_policy,
            require_target_sketch,
            require_alignment,
            observed_minimizer_algorithm,
            observed_chaining_algorithm,
            observed_base_alignment_algorithm,
            shared_vulkan_seed);
    }
#endif
#endif

    /// Check scalar sketches and require all Vulkan and SIMD sketch records to be empty.
    [[nodiscard]] bool is_selected_scalar_sketch_execution_consistent(
        const bool require_target_sketch, const bool require_nonempty_work) const noexcept {
        return partial_vulkan_stats == partial_vulkan_execution_stats{} &&
               is_selected_scalar_sketch_component_consistent(
                   require_target_sketch, require_nonempty_work);
    }

    /// Check scalar sketch records while allowing independently selected GPU stages.
    [[nodiscard]] bool is_selected_scalar_sketch_component_consistent(
        const bool require_target_sketch, const bool require_nonempty_work) const noexcept {
        const bool exact_absent = !exact_simd_query_execution_stats_available &&
                                  !exact_simd_target_execution_stats_available &&
                                  exact_simd_query_execution_stats == two_stack_execution_stats{} &&
                                  exact_simd_target_execution_stats == two_stack_execution_stats{};
        const bool bounded_absent =
            !bounded_nthash_query_execution_stats_available &&
            !bounded_nthash_target_execution_stats_available &&
            bounded_nthash_query_execution_stats == bounded_nthash_execution_stats{} &&
            bounded_nthash_target_execution_stats == bounded_nthash_execution_stats{};
        const bool is_target_valid =
            require_target_sketch
                ? scalar_pipeline_stats.target_minimizers.is_coherent(require_nonempty_work)
                : scalar_pipeline_stats.target_minimizers == scalar_minimizer_execution_stats{};
        return scalar_pipeline_execution_stats_available &&
               scalar_pipeline_stats.query_minimizers.is_coherent(require_nonempty_work) &&
               is_target_valid && exact_absent && bounded_absent;
    }

    /// Check for recorded sketching, chaining, alignment or Vulkan work.
    [[nodiscard]] bool has_any_algorithm_execution_stats() const noexcept {
        return exact_simd_query_execution_stats_available ||
               exact_simd_target_execution_stats_available ||
               bounded_nthash_query_execution_stats_available ||
               bounded_nthash_target_execution_stats_available ||
               simd_chaining_execution_stats_available || block_aligner_execution_stats_available ||
               scalar_pipeline_execution_stats_available ||
               ksw2_batched_execution_stats_available || partial_vulkan_stats.has_any_dispatch();
    }

    /// Check Block Aligner, statistics. If nonempty SIMD work is required, accept
    /// vector DP or a SIMD diagonal certificate for every nonempty call.
    [[nodiscard]] bool
    is_block_aligner_execution_consistent(bool require_nonempty_simd_work) const noexcept {
        if (!block_aligner_execution_stats_available ||
            !block_aligner_stats.is_internally_consistent()) {
            return false;
        }
        if (!require_nonempty_simd_work) {
            return true;
        }
        const auto& stats = block_aligner_stats;
        return stats.executed_calls() > 0 && stats.nonempty_calls > 0 &&
               stats.nonempty_calls_with_simd == stats.nonempty_calls &&
               ((stats.simd_dp_vector_batches > 0 && stats.simd_dp_cells > 0) ||
                stats.simd_diagonal_certificate_calls > 0);
    }

    /// Return the number of alignment results.
    [[nodiscard]] std::size_t size() const {
        return results.size();
    }

    /// Return whether the collection has no alignment results.
    [[nodiscard]] bool is_empty() const {
        return results.empty();
    }

    /// Borrow the ordered alignment results.
    [[nodiscard]] std::span<const match_result> view() const {
        return {results.data(), results.size()};
    }

    /// Summarize cesult fields and CIGAR contents while their storage is alive.
    [[nodiscard]] match_result_set_identity biological_identity() const noexcept {
        return make_match_result_set_identity(view());
    }

    /**
     * @brief Write each result to the debug log using PAF fields and tags.
     *
     * Coordinates are  zero-based half-open intervals. CIGAR results include
     * score tags and an NM estimate from block_length minus num_matches.
     * An empty collection writes an unmapped placeholder row.
     *
     * @param query_name Query name for the first PAF column.
     * @param target_name Target name for the sixth PAF column.
     */
    void print(const std::string& query_name = "query",
               const std::string& target_name = "target") const {
        if (results.empty()) {
            bioseq_log<logging_level::debug>("{}\t0\t0\t0\t*\t*\t0\t0\t0\t0\t0\t0", query_name);
            return;
        }

        for (const auto& match : results) {
            const char strand_char = match.is_reverse ? '-' : '+';
            const char type_char = match.is_primary ? 'P' : 'S';

            std::string line = fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                                           query_name,
                                           match.query_len,
                                           match.query_start,
                                           match.query_end,
                                           strand_char,
                                           target_name,
                                           match.target_len,
                                           match.target_start,
                                           match.target_end,
                                           match.num_matches,
                                           match.block_length,
                                           match.mapq);

            if (match.has_cigar && !match.cigar.empty()) {
                // Estimate NM from block length minus the reported match count and
                // clamp a negative difference to zero.
                const std::int64_t edit_distance =
                    std::max<std::int64_t>(0,
                                           static_cast<std::int64_t>(match.block_length) -
                                               static_cast<std::int64_t>(match.num_matches));
                line += fmt::format("\tNM:i:{}\tms:i:{}\tAS:i:{}\tnn:i:0",
                                    edit_distance,
                                    match.dp_max_score,
                                    match.dp_score);
            }

            line += fmt::format("\ttp:A:{}", type_char);
            line += fmt::format("\tcm:i:{}", match.seed_count);
            line += fmt::format("\ts1:i:{}", match.chain_score);

            if (match.is_primary) {
                line += fmt::format("\ts2:i:{}", match.secondary_chain_score);
            }

            if (match.divergence >= 0.0f && match.divergence <= 1.0f) {
                const char* divergence_tag =
                    (match.has_cigar && !match.cigar.empty()) ? "\tde:f:" : "\tdv:f:";
                if (match.divergence == 0.0f) {
                    line += divergence_tag;
                    line += '0';
                } else {
                    line += fmt::format(
                        "{}{:.4f}", divergence_tag, static_cast<double>(match.divergence));
                }
            }

            line += "\trl:i:0";

            if (match.has_cigar && !match.cigar.empty()) {
                line += "\tcg:Z:";
                line += format_cigar(match.cigar);
            }

            bioseq_log<logging_level::debug>("{}", line);
        }
    }
};

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
/** Result collections with a separate memory reservation for their outer array. */
using match_result_collection_vector =
    admitted_alignment_result_vector<match_result_collection, false>;
static_assert(std::is_nothrow_default_constructible_v<match_result_collection>);
static_assert(std::is_nothrow_move_constructible_v<match_result_collection>);
#else
using match_result_collection_vector = std::vector<match_result_collection>;
#endif

#if defined(BIOSEQ_TEST_INDEXES)
using test_match_result_collection_vector =
    test_outer_alignment_result_vector<match_result_collection>;
#endif

} // namespace bioseq::simd
