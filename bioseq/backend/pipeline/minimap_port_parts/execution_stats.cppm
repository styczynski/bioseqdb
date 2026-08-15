/**
 * @file pipeline/minimap_port_parts/execution_stats.cppm
 * @brief Route receipts and owned mapping-result collections.
 */
export module bioseq.pipeline.minimap_port:execution_stats;

import std;
import :results;
import :alignment;
import bioseq.common.logging;
import bioseq.highway.dispatch_contract;
import bioseq.prefilter.execution_contract;
import bioseq.sequences.kmers.two_stack_minimizers;
import bioseq.sequences.cigar;
import bioseq.alignment.block_aligner;
import bioseq.chaining.simd;
import bioseq.indexing.minimizer_hashmap;

export namespace bioseq::simd {

// ============================================================================
// PAF output — match_result_collection
// ============================================================================

// CIGAR opcode formatting is owned by the CIGAR module.

/// Positive execution statistics for partially accelerated Vulkan routes.
/// Counters are incremented only after a selected
/// fail-loud Vulkan operation returns successfully. Selector flags alone never
/// populate this stats.
struct partial_vulkan_execution_stats {
    /// Role-neutral token emitted by a minimizer leaf. It must be consumed as
    /// either query_sketch or target_sketch before a match_result_collection is
    /// returned to callers.
    std::uint64_t sketch_dispatches = 0;
    std::uint64_t query_sketch_dispatches = 0;
    std::uint64_t target_sketch_dispatches = 0;
    std::uint64_t target_index_build_dispatches = 0;
    std::uint64_t seed_lookup_dispatches = 0;
    // Operation-originated representation branch from the completed seed
    // shader. Their sum must equal seed_lookup_dispatches for strict rows.
    std::uint64_t seed_open_addressed_lookup_dispatches = 0;
    std::uint64_t seed_sorted_dense_lookup_dispatches = 0;
    std::uint64_t chain_score_dispatches = 0;
    // Operation-local seed-sort statistics. A selected mode flag never writes
    // these counters; they are copied only from a completed runtime result.
    std::uint64_t seed_bitonic_sort_dispatches = 0;
    std::uint64_t seed_radix_sort_operations = 0;
    std::uint64_t seed_radix_anchor_count = 0;
    std::uint64_t seed_radix_passes = 0;
    std::uint64_t seed_radix_histogram_dispatches = 0;
    std::uint64_t seed_radix_scan_dispatches = 0;
    std::uint64_t seed_radix_scatter_dispatches = 0;
    std::uint64_t seed_host_sort_fallbacks = 0;
    // Biological identity statistics emitted by completed GPU seed roundtrips. The
    // count/order/digest stream is independent from resident storage
    // integrity checks and therefore remains comparable between distinct
    // resident/index construction routes.
    std::uint64_t seed_anchor_stats_operations = 0;
    std::uint64_t seed_canonical_anchor_count = 0;
    std::uint64_t seed_canonical_anchor_order_checks = 0;
    std::uint64_t seed_canonical_anchor_order_accepts = 0;
    std::uint64_t seed_canonical_tie_normalization_runs = 0;
    std::uint64_t seed_canonical_tie_normalization_anchors = 0;
    std::uint64_t seed_anchor_digest_lane0 = 0;
    std::uint64_t seed_anchor_digest_lane1 = 0;
    // Operation-originated target-index materialization statistics.
    // Additive fields describe actual upload operations, never cache hits;
    // max fields preserve the worst collision/load observation across them.
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
    // Operation-originated target-index kernel and shape statistics. These
    // fields are attached to the one query that owns the shared target build,
    // never to every query that subsequently consumes the resident index.
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
    // query-independent source-direct sorted-dense route.
    // These fields are copied from the operation receipt only after its
    // terminal predicate accepts.  They therefore describe completed work,
    // including the canonical-empty bypass which intentionally has no GPU
    // dispatch and is therefore not represented by dispatch counters.
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
    // Owned-only last-seed retirement statistics. These counters are populated
    // only after the completed seed operation has dropped its local resident
    // handle and the operation-owned lifetime coordinator has actually
    // released storage before chaining.
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
        // A Stage-C batch owns exactly one shared target build and attaches its
        // stats to query zero. More than one build is not an accepted strict row.
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

    /// A source-direct sorted-dense operation owns a resident target index but
    /// deliberately emits no Stage-C target-build dispatch. Its N/D fields are
    /// therefore positive statistics, not contradictory no-build residue. Keep
    /// the target-index ownership, gate, and build surface fail-closed while the
    /// direct operation's complete consistency check validates its resident shape.
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

/// A batch constructs one Stage-C target index and shares it with every query.
/// Query zero is the sole owner of the build/ownership stats so additive batch
/// aggregation cannot turn one target build into N apparent builds.
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

/**
 * @brief Owning container of alignment results with PAF output formatting.
 *
 * Wraps a std::vector<match_result> and provides a print() method that outputs
 * results in exact minimap2 PAF format, reproducing mm_write_paf4()
 * (format.c:427-460) with auxiliary tags from write_tags() (format.c:399-425).
 */
struct match_result_collection {
    std::vector<match_result> results;
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
    // Operation-local CPU/SIMD wall clocks. These travel with the result so
    // parallel rows never update shared statement statistics. The PostgreSQL
    // dispatch boundary consumes them exactly once into the existing stable
    // pipeline_stage timing slots.
    std::uint64_t query_sketch_host_wall_ns = 0;
    std::uint64_t persisted_index_decode_host_wall_ns = 0;
    std::uint64_t seed_lookup_host_wall_ns = 0;
    std::uint64_t chain_score_host_wall_ns = 0;
    std::uint64_t region_generation_host_wall_ns = 0;
    std::uint64_t alignment_result_host_wall_ns = 0;
    bool is_host_stage_timing_saturated = false;
    // Coarse clocks cross stage objects but never escape a live pipeline.
    // The boundary methods below turn them into completed, mergeable deltas
    // before the collection is returned to its caller.
    std::uint64_t region_host_stage_started_ns = 0;
    std::uint64_t alignment_result_host_stage_started_ns = 0;
    bool is_region_host_stage_active = false;
    bool is_alignment_result_host_stage_active = false;

    /// Create an empty result collection and execution-stat aggregate.
    match_result_collection() = default;

    /// Adopt a completed ordered result vector.
    explicit match_result_collection(std::vector<match_result>&& results_vec)
        : results(std::move(results_vec)) {}

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

    /// Route scalar query-sketch statistics into the scalar aggregate.
    void record_scalar_query_minimizer_execution_stats(
        const scalar_minimizer_execution_stats& stats) noexcept {
        scalar_pipeline_execution_stats routed{};
        routed.query_minimizers = stats;
        record_scalar_pipeline_execution_stats(routed);
    }

    /// Route scalar target-sketch statistics into the scalar aggregate.
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

    /// Merge completed fragment-native query statistics.
    void record_fragment_native_query_stats(
        const bioseq::indexing::prepared_fragment_anchor_batch_stats& stats) noexcept {
        fragment_native_query_stats.merge(stats);
        fragment_native_query_stats_available = true;
    }

    /// Add one host-stage duration and retain overflow as a sticky flag.
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

    /// Add alignment and result-materialization host time.
    void record_alignment_result_host_time(const std::uint64_t elapsed_ns) noexcept {
        record_host_stage_time(alignment_result_host_wall_ns, elapsed_ns);
    }

    /// Start the region-generation/selection owner after chaining completes.
    void begin_region_host_stage(const std::uint64_t started_ns) noexcept {
        if (is_region_host_stage_active || is_alignment_result_host_stage_active) {
            is_host_stage_timing_saturated = true;
        }
        region_host_stage_started_ns = started_ns;
        is_region_host_stage_active = true;
    }

    /// Close regions and, when alignment is enabled, reuse the exact same
    /// boundary as the start of the inclusive alignment/result owner.
    void transition_region_to_alignment_result_host_stage(const std::uint64_t boundary_ns,
                                                          const bool alignment_enabled) noexcept {
        if (!is_region_host_stage_active || boundary_ns < region_host_stage_started_ns ||
            is_alignment_result_host_stage_active) {
            is_host_stage_timing_saturated = true;
            is_region_host_stage_active = false;
            is_alignment_result_host_stage_active = false;
            return;
        }
        record_region_generation_host_time(
            std::max<std::uint64_t>(1u, boundary_ns - region_host_stage_started_ns));
        is_region_host_stage_active = false;
        if (alignment_enabled) {
            alignment_result_host_stage_started_ns = boundary_ns;
            is_alignment_result_host_stage_active = true;
        }
    }

    /// Close the inclusive alignment, post-alignment filters, MAPQ and result
    /// materialisation owner at the terminal sink.
    void finish_alignment_result_host_stage(const std::uint64_t finished_ns) noexcept {
        if (!is_alignment_result_host_stage_active) {
            return;
        }
        if (finished_ns < alignment_result_host_stage_started_ns) {
            is_host_stage_timing_saturated = true;
            is_alignment_result_host_stage_active = false;
            return;
        }
        record_alignment_result_host_time(
            std::max<std::uint64_t>(1u, finished_ns - alignment_result_host_stage_started_ns));
        is_alignment_result_host_stage_active = false;
    }

    /// Merge all completed execution statistics without moving results.
    void combine_execution_stats(const match_result_collection& other) noexcept {
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

    /// Validate exact-SIMD query execution against the selected work policy.
    [[nodiscard]] bool
    is_exact_simd_query_execution_consistent(bool require_nonempty_vector_work) const noexcept {
        return exact_simd_query_execution_stats_available &&
               exact_simd_query_execution_stats.is_internally_consistent(
                   require_nonempty_vector_work);
    }

    /// Validate exact-SIMD target execution against the selected work policy.
    [[nodiscard]] bool
    is_exact_simd_target_execution_consistent(bool require_nonempty_vector_work) const noexcept {
        return exact_simd_target_execution_stats_available &&
               exact_simd_target_execution_stats.is_internally_consistent(
                   require_nonempty_vector_work);
    }

    /// Validate bounded-ntHash query execution.
    [[nodiscard]] bool
    is_bounded_nthash_query_execution_consistent(bool require_nonempty_vector_work) const noexcept {
        return bounded_nthash_query_execution_stats_available &&
               bounded_nthash_query_execution_stats.is_internally_consistent(
                   require_nonempty_vector_work);
    }

    /// Validate bounded-ntHash target execution.
    [[nodiscard]] bool is_bounded_nthash_target_execution_consistent(
        bool require_nonempty_vector_work) const noexcept {
        return bounded_nthash_target_execution_stats_available &&
               bounded_nthash_target_execution_stats.is_internally_consistent(
                   require_nonempty_vector_work);
    }

    /// Validate SIMD chaining for the selected policy.
    [[nodiscard]] bool
    is_simd_chaining_execution_consistent(simd_chaining_policy selected_policy,
                                          bool require_nonempty_vector_work) const noexcept {
        return simd_chaining_execution_stats_available &&
               simd_chaining_stats.is_internally_consistent(selected_policy,
                                                            require_nonempty_vector_work);
    }

    /// Validate the batched-KSW2 execution receipt.
    [[nodiscard]] bool
    is_ksw2_batched_execution_consistent(const bool require_nonempty_work) const noexcept {
        return ksw2_batched_execution_stats_available &&
               ksw2_batched_stats.is_coherent(require_nonempty_work);
    }

    /// Validate the selected SIMD sketch route and forbid mixed backends.
    [[nodiscard]] bool
    is_selected_simd_sketch_execution_consistent(minimizer_algorithm selected,
                                                 bool require_target_sketch,
                                                 bool require_nonempty_vector_work) const noexcept {
        if (partial_vulkan_stats != partial_vulkan_execution_stats{}) {
            return false;
        }

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

    /// Validate the SIMD sketch/chaining/Block tuple plus its real CPU seed
    /// lookup. All other scalar biological dimensions remain forbidden.
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
        return is_scalar_seed_only_consistent && !ksw2_batched_execution_stats_available &&
               ksw2_batched_stats == ksw2_batched_execution_stats{} &&
               is_selected_simd_sketch_execution_consistent(
                   selected, require_target_sketch, require_nonempty_vector_work) &&
               is_simd_chaining_execution_consistent(selected_chaining_policy,
                                                     require_nonempty_vector_work) &&
               is_block_aligner_execution_consistent(require_nonempty_vector_work);
    }

    /// Validate a complete all-scalar pipeline route.
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
        return is_selected_scalar_sketch_execution_consistent(require_target_sketch,
                                                              /*require_nonempty_work=*/true) &&
               scalar_pipeline_stats.is_route_consistent(require_target_sketch,
                                                         require_alignment) &&
               simd_chain_absent && block_absent && batched_absent &&
               partial_vulkan_stats == partial_vulkan_execution_stats{};
    }

    /// Validate scalar sketches and require all accelerated sketch stats absent.
    [[nodiscard]] bool is_selected_scalar_sketch_execution_consistent(
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
               is_target_valid && exact_absent && bounded_absent &&
               partial_vulkan_stats == partial_vulkan_execution_stats{};
    }

    /// Return whether any algorithm emitted completed execution statistics.
    [[nodiscard]] bool has_any_algorithm_execution_stats() const noexcept {
        return exact_simd_query_execution_stats_available ||
               exact_simd_target_execution_stats_available ||
               bounded_nthash_query_execution_stats_available ||
               bounded_nthash_target_execution_stats_available ||
               simd_chaining_execution_stats_available || block_aligner_execution_stats_available ||
               scalar_pipeline_execution_stats_available ||
               ksw2_batched_execution_stats_available || partial_vulkan_stats.has_any_dispatch();
    }

    /// Validate Block Aligner execution and optional nonempty SIMD work.
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
    [[nodiscard]] size_t size() const {
        return results.size();
    }

    /// Return whether the collection has no alignment results.
    [[nodiscard]] bool is_empty() const {
        return results.empty();
    }

    /// Borrow the ordered alignment results.
    [[nodiscard]] std::span<const match_result> view() const {
        return std::span<const match_result>(results);
    }

    /// Compute the complete biological result stats while the owning result
    /// vector and all CIGAR payloads are still live.
    [[nodiscard]] match_result_set_identity biological_identity() const noexcept {
        return make_match_result_set_identity(view());
    }

    /**
     * @brief Print alignment results in exact minimap2 PAF format.
     *
     * PAF line format (tab-separated):
     *   col 1:  query name             -- user-provided
     *   col 2:  query sequence length
     *   col 3:  query start (0-based, closed)
     *   col 4:  query end   (0-based, open)
     *   col 5:  relative strand '+' or '-'
     *   col 6:  target name            -- user-provided
     *   col 7:  target sequence length
     *   col 8:  target start on original strand (0-based)
     *   col 9:  target end   on original strand (0-based)
     *   col 10: number of residue matches  (match_length)
     *   col 11: alignment block length     (block_length)
     *   col 12: mapping quality (0-255)
     *
     * Auxiliary tags: NM, ms, AS, nn, tp, cm, s1, s2, de/dv, rl, cg
     *
     * @param query_name  Query sequence name (col 1)
     * @param target_name Target sequence name (col 6)
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
                // NM = blen - mlen; both are sequence_size_t (uint64) and
                // blen >= mlen, but compute signed + clamp defensively.
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

} // namespace bioseq::simd
