/**
 * @file pipeline/pipeline_stats_parts/aggregation.cppm
 * @brief Combine pipeline work counters and validate statement statistics.
 */
export module bioseq.pipeline.stats:aggregation;

import std;
export import :identities;
export import :provider_telemetry;
export import :environment_attestation;
import bioseq.indexing.minimizer_hashmap;
import bioseq.prefilter.execution_contract;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
export import bioseq.pipeline.minimap_parameters_contract;
#endif

export namespace bioseq::pipeline_stats {

/// Work counters recorded once per buffer, plus identities reported once.
struct pipeline_operational_stats {
    std::array<pipeline_stage_timing, k_pipeline_stage_count> timing{};
    std::uint64_t gpu_runtime_init_ns = 0;
    std::uint64_t gpu_pipeline_cache_loads = 0;
    std::uint64_t gpu_pipeline_cache_seed_bytes = 0;
    std::uint64_t gpu_pipeline_cache_load_ns = 0;
    std::uint64_t gpu_pipeline_creation_ns = 0;
    std::uint64_t gpu_submit_cpu_ns = 0;
    std::uint64_t gpu_wait_cpu_ns = 0;
    std::uint64_t gpu_ordinal_readback_cpu_ns = 0;
    std::uint64_t gpu_result_readback_cpu_ns = 0;
    std::uint64_t gpu_device_critical_path_ns = 0;
    std::uint64_t gpu_timestamp_query_failures = 0;
    std::uint64_t target_rows = 0;
    std::uint64_t cpu_prepared_index_builds = 0;
    std::uint64_t cpu_prepared_index_reuses = 0;
    std::uint64_t cpu_prepared_unique_keys = 0;
    std::uint64_t cpu_prepared_duplicate_build_attempts = 0;
    // Keep the full 64-bit checksum. Zero is valid, so a separate flag
    // records presence. These values are reported once and never added.
    std::uint64_t cpu_prepared_key_set_checksum = 0;
    bool cpu_prepared_key_set_checksum_available = false;
    std::uint64_t cpu_prepared_build_set_checksum = 0;
    bool cpu_prepared_build_set_checksum_available = false;
    std::uint64_t persisted_index_rows = 0;
    std::uint64_t missing_persisted_index_rows = 0;
    std::uint64_t recomputed_target_index_rows = 0;
    std::uint64_t persisted_generation_read_sessions = 0;
    std::uint64_t persisted_relation_opens = 0;
    std::uint64_t persisted_index_scans = 0;
    std::uint64_t persisted_head_scans = 0;
    std::uint64_t persisted_head_rows_read = 0;
    std::uint64_t persisted_head_bytes_read = 0;
    std::uint64_t persisted_manifest_directory_scans = 0;
    std::uint64_t persisted_manifest_rows_read = 0;
    std::uint64_t persisted_manifest_bytes_read = 0;
    std::uint64_t persisted_directory_chunks_read = 0;
    std::uint64_t persisted_directory_bytes_read = 0;
    std::uint64_t persisted_metadata_repeated_reads = 0;
    std::uint64_t persisted_metadata_heap_rows_read = 0;
    std::uint64_t persisted_metadata_bytes_read = 0;
    std::uint64_t persisted_payload_heap_rows_read = 0;
    std::uint64_t persisted_payload_bytes_read = 0;
    std::uint64_t persisted_heap_rows_read = 0;
    std::uint64_t persisted_toast_values_read = 0;
    std::uint64_t persisted_detoast_bytes = 0;
    std::uint64_t persisted_explicit_copy_bytes = 0;
    std::uint64_t persisted_wanted_subkeys = 0;
    std::uint64_t persisted_read_subkeys = 0;
    std::uint64_t persisted_wanted_extents = 0;
    std::uint64_t persisted_wanted_chunks = 0;
    std::uint64_t persisted_skipped_rows = 0;
    std::uint64_t persisted_repeated_chunk_reads = 0;
    std::uint64_t persisted_extents_read = 0;
    std::uint64_t persisted_chunks_read = 0;
    std::uint64_t persisted_bytes_read = 0;
    std::uint64_t persisted_bulk_read_calls = 0;
    std::uint64_t persisted_planned_request_groups = 0;
    std::uint64_t persisted_executed_request_groups = 0;
    std::uint64_t persisted_range_scan_calls = 0;
    std::uint64_t persisted_skip_budget_rows = 0;
    std::uint64_t persisted_hash_build_calls = 0;
    std::uint64_t persisted_assembly_ns = 0;
    std::uint64_t grouped_soa_reader_capacity_accounting_passes = 0;
    std::uint64_t grouped_soa_reader_global_position_storage_preparations = 0;
    std::uint64_t grouped_soa_reader_global_position_storage_allocations = 0;
    std::uint64_t grouped_soa_reader_position_append_growth_reservations = 0;
    std::uint64_t grouped_soa_reader_retained_position_count = 0;
    std::uint64_t grouped_soa_reader_retained_position_capacity = 0;
    std::uint64_t grouped_soa_reader_directory_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_query_hash_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_query_record_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_ordinal_route_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_selected_extent_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_current_selected_control_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_global_position_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_control_payload_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_owned_capacity_bytes = 0;
    std::uint64_t grouped_soa_reader_admitted_bytes = 0;
    std::uint64_t grouped_soa_reader_exact_position_image_checks = 0;
    std::uint64_t grouped_soa_reader_exact_position_image_failures = 0;
    std::uint64_t grouped_soa_reader_retained_layout_checks = 0;
    std::uint64_t grouped_soa_reader_retained_layout_failures = 0;
    bool are_grouped_soa_reader_memory_stats_saturated = false;
    grouped_soa_provider_stats grouped_soa_provider{};
    bioseq::indexing::prepared_fragment_anchor_batch_stats fragment_native_query_stats{};
    hash_major_index_reader_stats hash_major_index_reader{};
    // Count continuation vector batches and resident vector_stream calls
    // separately. has_nthash_vector_activity() accepts work from either kernel.
    std::uint64_t nthash_vector_batches = 0;
    std::uint64_t nthash_resident_vector_stream_kernel_calls = 0;
    std::uint64_t nthash_scalar_backend_branches = 0;
    std::uint64_t nthash_scalar_tail_symbols = 0;
    std::uint64_t nthash_masked_tail_lanes = 0;
    std::uint64_t simd_chain_vector_batches = 0;
    std::uint64_t simd_chain_backtrack_vector_batches = 0;
    std::uint64_t simd_chain_scalar_backend_branches = 0;
    std::uint64_t simd_chain_backtrack_scalar_branches = 0;
    std::uint64_t simd_chaining_exact_policy_calls = 0;
    std::uint64_t simd_chaining_bounded_policy_calls = 0;
    std::uint64_t simd_chaining_bounded_score_batches = 0;
    std::uint64_t simd_chaining_bounded_cutoff_stops = 0;
    std::uint64_t simd_chaining_bounded_candidates_skipped = 0;
    std::uint64_t simd_chaining_policy_rejections = 0;
    bool are_simd_chaining_policy_stats_saturated = false;
    block_aligner_stage_stats block_aligner_stats{};
    ::bioseq::prefilter::prefilter_execution_stats prefilter_stats{};
    bool prefilter_execution_stats_available = false;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    minimizer_algorithm_identity requested_minimizer_algorithm = minimizer_algorithm_identity::none;
    minimizer_algorithm_identity observed_minimizer_algorithm = minimizer_algorithm_identity::none;
    chaining_algorithm_identity requested_chaining_algorithm = chaining_algorithm_identity::none;
    chaining_algorithm_identity observed_chaining_algorithm = chaining_algorithm_identity::none;
    base_alignment_algorithm_identity requested_base_alignment_algorithm =
        base_alignment_algorithm_identity::none;
    base_alignment_algorithm_identity observed_base_alignment_algorithm =
        base_alignment_algorithm_identity::none;
#endif
    std::uint64_t downstream_active_wall_ns = 0;
    std::uint64_t downstream_active_wall_invocations = 0;
    std::uint64_t downstream_active_wall_unavailable_invocations = 0;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    ::bioseq::simd::benchmark_e2e_interval_union_receipt benchmark_e2e_interval_union{};
    std::uint64_t benchmark_e2e_interval_union_invocations = 0;
    bool mapq_execution_available = false;
    bool mapq_executed = false;
    bool mapq_execution_conflict = false;
#endif
    std::uint64_t cpu_seed_positions_rejected_outside_collection = 0;
    std::uint64_t gpu_partition_select_dispatches = 0;
    std::uint64_t gpu_partition_anchor_merge_dispatches = 0;
    std::uint64_t gpu_index_build_candidates = 0;
    std::uint64_t gpu_index_build_canonical_emitted = 0;
    std::uint64_t gpu_index_build_overlap_duplicates_rejected = 0;
    std::uint64_t gpu_index_build_noncanonical_extras_rejected = 0;
    std::uint64_t gpu_index_build_surviving_extras = 0;
    std::uint64_t gpu_persisted_resident_uploads = 0;
    std::uint64_t gpu_persisted_resident_cache_hits = 0;
    std::uint64_t gpu_overlap_anchor_duplicates = 0;
    std::uint64_t gpu_seed_dispatches = 0;
    std::uint64_t gpu_chain_dispatches = 0;
    std::uint64_t gpu_chain_backtrack_dispatches = 0;
    // Coordinate voting has its own counter because it does not run
    // tiled-DP chaining or predecessor backtracking.
    std::uint64_t gpu_coordinate_vote_dispatches = 0;
    std::uint64_t gpu_extend_dispatches = 0;
    std::uint64_t gpu_target_window_request_dispatches = 0;
    std::uint64_t gpu_target_window_logical_units_requested = 0;
    std::uint64_t host_target_window_requests_consumed = 0;
    std::uint64_t host_target_window_physical_units_read = 0;
    std::uint64_t host_target_window_remap_units_filled = 0;
    std::uint64_t host_target_window_packed_bytes_read = 0;
    std::uint64_t gpu_queue_submits = 0;
    std::uint64_t gpu_host_waits = 0;
    std::uint64_t gpu_buffer_creates = 0;
    std::uint64_t gpu_memory_allocations = 0;
    std::uint64_t gpu_pipeline_creates = 0;
    std::uint64_t host_pair_materialization_calls = 0;
    std::uint64_t host_biological_hash_filter_calls = 0;
    std::uint64_t host_query_hash_readbacks = 0;
    std::uint64_t host_query_symbol_packing_calls = 0;
    std::uint64_t host_target_symbol_packing_calls = 0;
    std::uint64_t host_alignment_job_classification_calls = 0;
    std::uint64_t host_anchor_concatenation_calls = 0;
    std::uint64_t host_cigar_construction_calls = 0;
    std::uint64_t pair_sort_calls = 0;
    std::uint64_t full_segment_hydrations = 0;
    std::uint64_t backend_declines = 0;
    std::uint64_t backend_fallbacks = 0;
    // Count persisted-source reads and pair-index conversions here.
    // Direct grouped-SoA validation has its own counter below.
    std::uint64_t persisted_source_parent_rows_read = 0;
    std::uint64_t persisted_source_catalog_rows_read = 0;
    std::uint64_t persisted_source_catalog_index_scans = 0;
    std::uint64_t persisted_source_fragment_rows_read = 0;
    std::uint64_t persisted_source_decoded_bp = 0;
    std::uint64_t persisted_pair_index_conversion_calls = 0;
    std::uint64_t persisted_segment_rebase_calls = 0;
    // Count completed grouped-SoA reader validation. Planning cannot
    // determine the stored format of a particular target row.
    std::uint64_t indexing_grouped_soa_validate_calls = 0;
    // Measure PostgreSQL work while opening and preparing persisted indexes.
    // Backend acceptance still depends on the requested and recorded stages.
    std::uint64_t cold_index_target_relation_reuses = 0;
    std::uint64_t cold_index_target_relation_extra_opens = 0;
    std::uint64_t cold_index_parent_random_reads = 0;
    std::uint64_t cold_index_policy_heap_opens = 0;
    std::uint64_t cold_index_policy_index_opens = 0;
    std::uint64_t cold_index_source_heap_opens = 0;
    std::uint64_t cold_index_source_index_opens = 0;
    std::uint64_t cold_index_fragment_heap_opens = 0;
    std::uint64_t cold_index_sidecar_heap_opens = 0;
    std::uint64_t cold_index_sidecar_index_opens = 0;
    std::uint64_t cold_index_policy_schema_validations = 0;
    std::uint64_t cold_index_source_schema_validations = 0;
    std::uint64_t cold_index_sidecar_schema_validations = 0;
    std::uint64_t cold_index_head_scans = 0;
    std::uint64_t cold_index_directory_scans = 0;
    std::uint64_t cold_index_payload_range_scans = 0;
    std::uint64_t cold_index_lock_acquires = 0;
    std::uint64_t cold_index_lock_releases = 0;
    std::uint64_t cold_index_snapshot_registers = 0;
    std::uint64_t cold_index_route_empty = 0;
    std::uint64_t cold_index_route_selective = 0;
    std::uint64_t cold_index_route_full = 0;
    std::uint64_t cold_index_route_candidate_rows = 0;
    std::uint64_t cold_index_route_full_rows = 0;
    std::uint64_t cold_index_route_candidate_bytes = 0;
    std::uint64_t cold_index_route_full_bytes = 0;
    std::uint64_t cold_index_route_selective_cost = 0;
    std::uint64_t cold_index_route_full_cost = 0;
    std::uint64_t cold_index_memory_reroutes = 0;
    std::uint64_t cold_index_payload_bytes_requested = 0;
    std::uint64_t cold_index_payload_bytes_delivered = 0;
    std::uint64_t cold_index_payload_rows_requested = 0;
    std::uint64_t cold_index_payload_rows_delivered = 0;
    std::uint64_t cold_index_payload_rows_scanned = 0;
    std::uint64_t cold_index_payload_bytes_scanned = 0;
    std::uint64_t cold_index_payload_rows_skipped = 0;
    std::uint64_t cold_index_heap_tid_adjacent = 0;
    std::uint64_t cold_index_heap_tid_forward_gap = 0;
    std::uint64_t cold_index_heap_tid_backward_jump = 0;
    std::uint64_t cold_index_extent_crc_bytes = 0;
    std::uint64_t cold_index_extent_crc_raw_bytes_hashed = 0;
    std::uint64_t cold_index_extent_crc_combined_bytes = 0;
    std::uint64_t cold_index_extent_crc_combine_rows = 0;
    std::uint64_t cold_index_row_crc_bytes = 0;
    std::uint64_t cold_index_tuple_form_bytes = 0;
    std::uint64_t cold_index_toast_values = 0;
    std::uint64_t cold_index_detoast_bytes = 0;
    std::uint64_t cold_index_policy_ns = 0;
    std::uint64_t cold_index_source_ns = 0;
    std::uint64_t cold_index_bind_ns = 0;
    std::uint64_t cold_index_head_ns = 0;
    std::uint64_t cold_index_directory_ns = 0;
    std::uint64_t cold_index_payload_ns = 0;
    std::uint64_t cold_index_decode_ns = 0;
    std::uint64_t cold_index_transfer_ns = 0;
    std::uint64_t cold_index_close_ns = 0;
    std::uint64_t cold_index_shared_blks_hit = 0;
    std::uint64_t cold_index_shared_blks_read = 0;
    // Keep identity conflicts separate from counter overflow. Full 64-bit
    // identities must remain unchanged when counters reach their SQL limit.
    bool has_operational_identity_conflict = false;
    bool are_stats_saturated = false;

    // Compare fields, including presence and failure flags. Object padding
    // must not affect equality of statistics.
    [[nodiscard]] constexpr bool
    operator==(const pipeline_operational_stats&) const noexcept = default;

    /// Check whether either nthash kernel recorded vector work.
    [[nodiscard]] constexpr bool has_nthash_vector_activity() const noexcept {
        return nthash_vector_batches != 0 || nthash_resident_vector_stream_kernel_calls != 0;
    }

    /// Add completed prefilter statistics and mark them present. Invalid combined
    /// counters set both the prefilter and operational saturation flags.
    void record_prefilter_execution_stats(
        const ::bioseq::prefilter::prefilter_execution_stats& stats) noexcept {
        prefilter_stats.merge(stats);
        prefilter_execution_stats_available = true;
        if (!prefilter_stats.is_internally_consistent()) {
            prefilter_stats.is_saturated = true;
            are_stats_saturated = true;
        }
    }

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    /// Combine requested and recorded algorithms separately. Differences between
    /// them preserve fallback information. Conflicting repeated selections or
    /// missing required identities set the conflict flag.
    void record_algorithm_identities(
        const minimizer_algorithm_identity requested_minimizer,
        const minimizer_algorithm_identity observed_minimizer,
        const chaining_algorithm_identity requested_chaining,
        const chaining_algorithm_identity observed_chaining,
        const base_alignment_algorithm_identity requested_base_alignment,
        const base_alignment_algorithm_identity observed_base_alignment) noexcept {
        requested_minimizer_algorithm =
            merge_minimizer_algorithm_identity(requested_minimizer_algorithm, requested_minimizer);
        observed_minimizer_algorithm =
            merge_minimizer_algorithm_identity(observed_minimizer_algorithm, observed_minimizer);
        requested_chaining_algorithm =
            merge_chaining_algorithm_identity(requested_chaining_algorithm, requested_chaining);
        observed_chaining_algorithm =
            merge_chaining_algorithm_identity(observed_chaining_algorithm, observed_chaining);
        requested_base_alignment_algorithm = merge_base_alignment_algorithm_identity(
            requested_base_alignment_algorithm, requested_base_alignment);
        observed_base_alignment_algorithm = merge_base_alignment_algorithm_identity(
            observed_base_alignment_algorithm, observed_base_alignment);
        has_operational_identity_conflict =
            has_operational_identity_conflict ||
            requested_minimizer == minimizer_algorithm_identity::none ||
            observed_minimizer == minimizer_algorithm_identity::none ||
            requested_chaining == chaining_algorithm_identity::none ||
            observed_chaining == chaining_algorithm_identity::none ||
            ((requested_base_alignment == base_alignment_algorithm_identity::none) !=
             (observed_base_alignment == base_alignment_algorithm_identity::none)) ||
            requested_minimizer_algorithm == minimizer_algorithm_identity::mixed_invalid ||
            observed_minimizer_algorithm == minimizer_algorithm_identity::mixed_invalid ||
            requested_chaining_algorithm == chaining_algorithm_identity::mixed_invalid ||
            observed_chaining_algorithm == chaining_algorithm_identity::mixed_invalid ||
            requested_base_alignment_algorithm ==
                base_alignment_algorithm_identity::mixed_invalid ||
            observed_base_alignment_algorithm == base_alignment_algorithm_identity::mixed_invalid;
    }
#endif

    /// Add a call's downstream active time, or record that its timing was unavailable.
    void record_downstream_active_wall(const std::uint64_t elapsed_ns,
                                       const bool available) noexcept {
        are_stats_saturated =
            merge_counter(downstream_active_wall_invocations, 1u) || are_stats_saturated;
        if (!available) {
            are_stats_saturated =
                merge_counter(downstream_active_wall_unavailable_invocations, 1u) ||
                are_stats_saturated;
            return;
        }
        are_stats_saturated =
            merge_counter(downstream_active_wall_ns, elapsed_ns) || are_stats_saturated;
    }

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    /** Store the finalized statement interval union. A second report invalidates it. */
    void record_benchmark_e2e_interval_union(
        ::bioseq::simd::benchmark_e2e_interval_union_receipt receipt) noexcept {
        if (benchmark_e2e_interval_union_invocations != 0u) {
            benchmark_e2e_interval_union.available = false;
            benchmark_e2e_interval_union.invalid = true;
            benchmark_e2e_interval_union_invocations = (std::numeric_limits<std::uint64_t>::max)();
            return;
        }
        benchmark_e2e_interval_union = std::move(receipt);
        benchmark_e2e_interval_union_invocations = 1u;
    }
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    /// Record whether MAPQ ran and flag disagreement between observations.
    void record_mapq_execution(const bool executed) noexcept {
        if (!mapq_execution_available) {
            mapq_execution_available = true;
            mapq_executed = executed;
        } else if (mapq_executed != executed) {
            mapq_execution_conflict = true;
            has_operational_identity_conflict = true;
        }
    }
#endif

    /// Add one buffer's work counters once, regardless of its number of jobs.
    /// Preserve identity conflicts and overflow when combining nested records.
    void merge(const pipeline_operational_stats& rhs) noexcept {
        using prefilter_stats_type = ::bioseq::prefilter::prefilter_execution_stats;
        const prefilter_stats_type empty_prefilter_stats{};
        if (prefilter_execution_stats_available && !prefilter_stats.is_internally_consistent()) {
            prefilter_stats.is_saturated = true;
            are_stats_saturated = true;
        } else if (!prefilter_execution_stats_available &&
                   prefilter_stats != empty_prefilter_stats) {
            prefilter_stats.is_saturated = true;
            prefilter_execution_stats_available = true;
            are_stats_saturated = true;
        }
        if (rhs.prefilter_execution_stats_available) {
            record_prefilter_execution_stats(rhs.prefilter_stats);
        } else if (rhs.prefilter_stats != empty_prefilter_stats) {
            prefilter_stats_type malformed = rhs.prefilter_stats;
            malformed.is_saturated = true;
            record_prefilter_execution_stats(malformed);
        }
        bool is_saturated = are_stats_saturated || rhs.are_stats_saturated;
        has_operational_identity_conflict =
            has_operational_identity_conflict || rhs.has_operational_identity_conflict;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (rhs.requested_minimizer_algorithm != minimizer_algorithm_identity::none ||
            rhs.observed_minimizer_algorithm != minimizer_algorithm_identity::none ||
            rhs.requested_chaining_algorithm != chaining_algorithm_identity::none ||
            rhs.observed_chaining_algorithm != chaining_algorithm_identity::none ||
            rhs.requested_base_alignment_algorithm != base_alignment_algorithm_identity::none ||
            rhs.observed_base_alignment_algorithm != base_alignment_algorithm_identity::none) {
            record_algorithm_identities(rhs.requested_minimizer_algorithm,
                                        rhs.observed_minimizer_algorithm,
                                        rhs.requested_chaining_algorithm,
                                        rhs.observed_chaining_algorithm,
                                        rhs.requested_base_alignment_algorithm,
                                        rhs.observed_base_alignment_algorithm);
        }
        if (rhs.mapq_execution_available) {
            record_mapq_execution(rhs.mapq_executed);
        } else if (rhs.mapq_executed) {
            mapq_execution_conflict = true;
            has_operational_identity_conflict = true;
        }
        mapq_execution_conflict = mapq_execution_conflict || rhs.mapq_execution_conflict;
        has_operational_identity_conflict =
            has_operational_identity_conflict || mapq_execution_conflict;
        if (rhs.benchmark_e2e_interval_union_invocations != 0u) {
            if (benchmark_e2e_interval_union_invocations == 0u &&
                rhs.benchmark_e2e_interval_union_invocations == 1u) {
                benchmark_e2e_interval_union = rhs.benchmark_e2e_interval_union;
                benchmark_e2e_interval_union_invocations = 1u;
            } else {
                benchmark_e2e_interval_union.available = false;
                benchmark_e2e_interval_union.invalid = true;
                benchmark_e2e_interval_union_invocations =
                    (std::numeric_limits<std::uint64_t>::max)();
            }
        }
#endif
        merge_report_once_identity(cpu_prepared_key_set_checksum,
                                   cpu_prepared_key_set_checksum_available,
                                   rhs.cpu_prepared_key_set_checksum,
                                   rhs.cpu_prepared_key_set_checksum_available,
                                   has_operational_identity_conflict);
        merge_report_once_identity(cpu_prepared_build_set_checksum,
                                   cpu_prepared_build_set_checksum_available,
                                   rhs.cpu_prepared_build_set_checksum,
                                   rhs.cpu_prepared_build_set_checksum_available,
                                   has_operational_identity_conflict);
        auto add = [&is_saturated](std::uint64_t& destination, std::uint64_t increment) noexcept {
            is_saturated = merge_counter(destination, increment) || is_saturated;
        };
        for (std::size_t i = 0; i < timing.size(); ++i) {
            add(timing[i].host_wall_ns, rhs.timing[i].host_wall_ns);
            add(timing[i].gpu_device_ns, rhs.timing[i].gpu_device_ns);
        }
        add(downstream_active_wall_ns, rhs.downstream_active_wall_ns);
        add(downstream_active_wall_invocations, rhs.downstream_active_wall_invocations);
        add(downstream_active_wall_unavailable_invocations,
            rhs.downstream_active_wall_unavailable_invocations);
        if (rhs.downstream_active_wall_unavailable_invocations >
            rhs.downstream_active_wall_invocations) {
            is_saturated = true;
        }
        add(gpu_runtime_init_ns, rhs.gpu_runtime_init_ns);
        add(gpu_pipeline_cache_loads, rhs.gpu_pipeline_cache_loads);
        add(gpu_pipeline_cache_seed_bytes, rhs.gpu_pipeline_cache_seed_bytes);
        add(gpu_pipeline_cache_load_ns, rhs.gpu_pipeline_cache_load_ns);
        add(gpu_pipeline_creation_ns, rhs.gpu_pipeline_creation_ns);
        add(gpu_submit_cpu_ns, rhs.gpu_submit_cpu_ns);
        add(gpu_wait_cpu_ns, rhs.gpu_wait_cpu_ns);
        add(gpu_ordinal_readback_cpu_ns, rhs.gpu_ordinal_readback_cpu_ns);
        add(gpu_result_readback_cpu_ns, rhs.gpu_result_readback_cpu_ns);
        add(gpu_device_critical_path_ns, rhs.gpu_device_critical_path_ns);
        add(gpu_timestamp_query_failures, rhs.gpu_timestamp_query_failures);
        add(target_rows, rhs.target_rows);
        add(cpu_prepared_index_builds, rhs.cpu_prepared_index_builds);
        add(cpu_prepared_index_reuses, rhs.cpu_prepared_index_reuses);
        add(cpu_prepared_unique_keys, rhs.cpu_prepared_unique_keys);
        add(cpu_prepared_duplicate_build_attempts, rhs.cpu_prepared_duplicate_build_attempts);
        add(persisted_index_rows, rhs.persisted_index_rows);
        add(missing_persisted_index_rows, rhs.missing_persisted_index_rows);
        add(recomputed_target_index_rows, rhs.recomputed_target_index_rows);
        add(persisted_generation_read_sessions, rhs.persisted_generation_read_sessions);
        add(persisted_relation_opens, rhs.persisted_relation_opens);
        add(persisted_index_scans, rhs.persisted_index_scans);
        add(persisted_head_scans, rhs.persisted_head_scans);
        add(persisted_head_rows_read, rhs.persisted_head_rows_read);
        add(persisted_head_bytes_read, rhs.persisted_head_bytes_read);
        add(persisted_manifest_directory_scans, rhs.persisted_manifest_directory_scans);
        add(persisted_manifest_rows_read, rhs.persisted_manifest_rows_read);
        add(persisted_manifest_bytes_read, rhs.persisted_manifest_bytes_read);
        add(persisted_directory_chunks_read, rhs.persisted_directory_chunks_read);
        add(persisted_directory_bytes_read, rhs.persisted_directory_bytes_read);
        add(persisted_metadata_repeated_reads, rhs.persisted_metadata_repeated_reads);
        add(persisted_metadata_heap_rows_read, rhs.persisted_metadata_heap_rows_read);
        add(persisted_metadata_bytes_read, rhs.persisted_metadata_bytes_read);
        add(persisted_payload_heap_rows_read, rhs.persisted_payload_heap_rows_read);
        add(persisted_payload_bytes_read, rhs.persisted_payload_bytes_read);
        add(persisted_heap_rows_read, rhs.persisted_heap_rows_read);
        add(persisted_toast_values_read, rhs.persisted_toast_values_read);
        add(persisted_detoast_bytes, rhs.persisted_detoast_bytes);
        add(persisted_explicit_copy_bytes, rhs.persisted_explicit_copy_bytes);
        add(persisted_wanted_subkeys, rhs.persisted_wanted_subkeys);
        add(persisted_read_subkeys, rhs.persisted_read_subkeys);
        add(persisted_wanted_extents, rhs.persisted_wanted_extents);
        add(persisted_wanted_chunks, rhs.persisted_wanted_chunks);
        add(persisted_skipped_rows, rhs.persisted_skipped_rows);
        add(persisted_repeated_chunk_reads, rhs.persisted_repeated_chunk_reads);
        add(persisted_extents_read, rhs.persisted_extents_read);
        add(persisted_chunks_read, rhs.persisted_chunks_read);
        add(persisted_bytes_read, rhs.persisted_bytes_read);
        add(persisted_bulk_read_calls, rhs.persisted_bulk_read_calls);
        add(persisted_planned_request_groups, rhs.persisted_planned_request_groups);
        add(persisted_executed_request_groups, rhs.persisted_executed_request_groups);
        add(persisted_range_scan_calls, rhs.persisted_range_scan_calls);
        add(persisted_skip_budget_rows, rhs.persisted_skip_budget_rows);
        add(persisted_hash_build_calls, rhs.persisted_hash_build_calls);
        add(persisted_assembly_ns, rhs.persisted_assembly_ns);
        add(grouped_soa_reader_capacity_accounting_passes,
            rhs.grouped_soa_reader_capacity_accounting_passes);
        add(grouped_soa_reader_global_position_storage_preparations,
            rhs.grouped_soa_reader_global_position_storage_preparations);
        add(grouped_soa_reader_global_position_storage_allocations,
            rhs.grouped_soa_reader_global_position_storage_allocations);
        add(grouped_soa_reader_position_append_growth_reservations,
            rhs.grouped_soa_reader_position_append_growth_reservations);
        add(grouped_soa_reader_retained_position_count,
            rhs.grouped_soa_reader_retained_position_count);
        add(grouped_soa_reader_retained_position_capacity,
            rhs.grouped_soa_reader_retained_position_capacity);
        add(grouped_soa_reader_directory_capacity_bytes,
            rhs.grouped_soa_reader_directory_capacity_bytes);
        add(grouped_soa_reader_query_hash_capacity_bytes,
            rhs.grouped_soa_reader_query_hash_capacity_bytes);
        add(grouped_soa_reader_query_record_capacity_bytes,
            rhs.grouped_soa_reader_query_record_capacity_bytes);
        add(grouped_soa_reader_ordinal_route_capacity_bytes,
            rhs.grouped_soa_reader_ordinal_route_capacity_bytes);
        add(grouped_soa_reader_selected_extent_capacity_bytes,
            rhs.grouped_soa_reader_selected_extent_capacity_bytes);
        add(grouped_soa_reader_current_selected_control_capacity_bytes,
            rhs.grouped_soa_reader_current_selected_control_capacity_bytes);
        add(grouped_soa_reader_global_position_capacity_bytes,
            rhs.grouped_soa_reader_global_position_capacity_bytes);
        add(grouped_soa_reader_control_payload_capacity_bytes,
            rhs.grouped_soa_reader_control_payload_capacity_bytes);
        add(grouped_soa_reader_owned_capacity_bytes, rhs.grouped_soa_reader_owned_capacity_bytes);
        add(grouped_soa_reader_admitted_bytes, rhs.grouped_soa_reader_admitted_bytes);
        add(grouped_soa_reader_exact_position_image_checks,
            rhs.grouped_soa_reader_exact_position_image_checks);
        add(grouped_soa_reader_exact_position_image_failures,
            rhs.grouped_soa_reader_exact_position_image_failures);
        add(grouped_soa_reader_retained_layout_checks,
            rhs.grouped_soa_reader_retained_layout_checks);
        add(grouped_soa_reader_retained_layout_failures,
            rhs.grouped_soa_reader_retained_layout_failures);
        are_grouped_soa_reader_memory_stats_saturated =
            are_grouped_soa_reader_memory_stats_saturated ||
            rhs.are_grouped_soa_reader_memory_stats_saturated;
        is_saturated = grouped_soa_provider.merge(rhs.grouped_soa_provider,
                                                  has_operational_identity_conflict) ||
                       is_saturated;
        are_grouped_soa_reader_memory_stats_saturated =
            are_grouped_soa_reader_memory_stats_saturated || grouped_soa_provider.is_saturated;
        fragment_native_query_stats.merge(rhs.fragment_native_query_stats);
        is_saturated = fragment_native_query_stats.saturated || is_saturated;
        is_saturated = hash_major_index_reader.merge(rhs.hash_major_index_reader,
                                                     has_operational_identity_conflict) ||
                       is_saturated;
        add(nthash_vector_batches, rhs.nthash_vector_batches);
        add(nthash_resident_vector_stream_kernel_calls,
            rhs.nthash_resident_vector_stream_kernel_calls);
        add(nthash_scalar_backend_branches, rhs.nthash_scalar_backend_branches);
        add(nthash_scalar_tail_symbols, rhs.nthash_scalar_tail_symbols);
        add(nthash_masked_tail_lanes, rhs.nthash_masked_tail_lanes);
        add(simd_chain_vector_batches, rhs.simd_chain_vector_batches);
        add(simd_chain_backtrack_vector_batches, rhs.simd_chain_backtrack_vector_batches);
        add(simd_chain_scalar_backend_branches, rhs.simd_chain_scalar_backend_branches);
        add(simd_chain_backtrack_scalar_branches, rhs.simd_chain_backtrack_scalar_branches);
        add(simd_chaining_exact_policy_calls, rhs.simd_chaining_exact_policy_calls);
        add(simd_chaining_bounded_policy_calls, rhs.simd_chaining_bounded_policy_calls);
        add(simd_chaining_bounded_score_batches, rhs.simd_chaining_bounded_score_batches);
        add(simd_chaining_bounded_cutoff_stops, rhs.simd_chaining_bounded_cutoff_stops);
        add(simd_chaining_bounded_candidates_skipped, rhs.simd_chaining_bounded_candidates_skipped);
        add(simd_chaining_policy_rejections, rhs.simd_chaining_policy_rejections);
        are_simd_chaining_policy_stats_saturated = are_simd_chaining_policy_stats_saturated ||
                                                   rhs.are_simd_chaining_policy_stats_saturated;
        is_saturated = block_aligner_stats.merge(rhs.block_aligner_stats) || is_saturated;
        add(cpu_seed_positions_rejected_outside_collection,
            rhs.cpu_seed_positions_rejected_outside_collection);
        add(gpu_partition_select_dispatches, rhs.gpu_partition_select_dispatches);
        add(gpu_partition_anchor_merge_dispatches, rhs.gpu_partition_anchor_merge_dispatches);
        add(gpu_index_build_candidates, rhs.gpu_index_build_candidates);
        add(gpu_index_build_canonical_emitted, rhs.gpu_index_build_canonical_emitted);
        add(gpu_index_build_overlap_duplicates_rejected,
            rhs.gpu_index_build_overlap_duplicates_rejected);
        add(gpu_index_build_noncanonical_extras_rejected,
            rhs.gpu_index_build_noncanonical_extras_rejected);
        add(gpu_index_build_surviving_extras, rhs.gpu_index_build_surviving_extras);
        add(gpu_persisted_resident_uploads, rhs.gpu_persisted_resident_uploads);
        add(gpu_persisted_resident_cache_hits, rhs.gpu_persisted_resident_cache_hits);
        add(gpu_overlap_anchor_duplicates, rhs.gpu_overlap_anchor_duplicates);
        add(gpu_seed_dispatches, rhs.gpu_seed_dispatches);
        add(gpu_chain_dispatches, rhs.gpu_chain_dispatches);
        add(gpu_chain_backtrack_dispatches, rhs.gpu_chain_backtrack_dispatches);
        add(gpu_coordinate_vote_dispatches, rhs.gpu_coordinate_vote_dispatches);
        add(gpu_extend_dispatches, rhs.gpu_extend_dispatches);
        add(gpu_target_window_request_dispatches, rhs.gpu_target_window_request_dispatches);
        add(gpu_target_window_logical_units_requested,
            rhs.gpu_target_window_logical_units_requested);
        add(host_target_window_requests_consumed, rhs.host_target_window_requests_consumed);
        add(host_target_window_physical_units_read, rhs.host_target_window_physical_units_read);
        add(host_target_window_remap_units_filled, rhs.host_target_window_remap_units_filled);
        add(host_target_window_packed_bytes_read, rhs.host_target_window_packed_bytes_read);
        add(gpu_queue_submits, rhs.gpu_queue_submits);
        add(gpu_host_waits, rhs.gpu_host_waits);
        add(gpu_buffer_creates, rhs.gpu_buffer_creates);
        add(gpu_memory_allocations, rhs.gpu_memory_allocations);
        add(gpu_pipeline_creates, rhs.gpu_pipeline_creates);
        add(host_pair_materialization_calls, rhs.host_pair_materialization_calls);
        add(host_biological_hash_filter_calls, rhs.host_biological_hash_filter_calls);
        add(host_query_hash_readbacks, rhs.host_query_hash_readbacks);
        add(host_query_symbol_packing_calls, rhs.host_query_symbol_packing_calls);
        add(host_target_symbol_packing_calls, rhs.host_target_symbol_packing_calls);
        add(host_alignment_job_classification_calls, rhs.host_alignment_job_classification_calls);
        add(host_anchor_concatenation_calls, rhs.host_anchor_concatenation_calls);
        add(host_cigar_construction_calls, rhs.host_cigar_construction_calls);
        add(pair_sort_calls, rhs.pair_sort_calls);
        add(full_segment_hydrations, rhs.full_segment_hydrations);
        add(backend_declines, rhs.backend_declines);
        add(backend_fallbacks, rhs.backend_fallbacks);
        add(persisted_source_parent_rows_read, rhs.persisted_source_parent_rows_read);
        add(persisted_source_catalog_rows_read, rhs.persisted_source_catalog_rows_read);
        add(persisted_source_catalog_index_scans, rhs.persisted_source_catalog_index_scans);
        add(persisted_source_fragment_rows_read, rhs.persisted_source_fragment_rows_read);
        add(persisted_source_decoded_bp, rhs.persisted_source_decoded_bp);
        add(persisted_pair_index_conversion_calls, rhs.persisted_pair_index_conversion_calls);
        add(persisted_segment_rebase_calls, rhs.persisted_segment_rebase_calls);
        add(indexing_grouped_soa_validate_calls, rhs.indexing_grouped_soa_validate_calls);
        add(cold_index_target_relation_reuses, rhs.cold_index_target_relation_reuses);
        add(cold_index_target_relation_extra_opens, rhs.cold_index_target_relation_extra_opens);
        add(cold_index_parent_random_reads, rhs.cold_index_parent_random_reads);
        add(cold_index_policy_heap_opens, rhs.cold_index_policy_heap_opens);
        add(cold_index_policy_index_opens, rhs.cold_index_policy_index_opens);
        add(cold_index_source_heap_opens, rhs.cold_index_source_heap_opens);
        add(cold_index_source_index_opens, rhs.cold_index_source_index_opens);
        add(cold_index_fragment_heap_opens, rhs.cold_index_fragment_heap_opens);
        add(cold_index_sidecar_heap_opens, rhs.cold_index_sidecar_heap_opens);
        add(cold_index_sidecar_index_opens, rhs.cold_index_sidecar_index_opens);
        add(cold_index_policy_schema_validations, rhs.cold_index_policy_schema_validations);
        add(cold_index_source_schema_validations, rhs.cold_index_source_schema_validations);
        add(cold_index_sidecar_schema_validations, rhs.cold_index_sidecar_schema_validations);
        add(cold_index_head_scans, rhs.cold_index_head_scans);
        add(cold_index_directory_scans, rhs.cold_index_directory_scans);
        add(cold_index_payload_range_scans, rhs.cold_index_payload_range_scans);
        add(cold_index_lock_acquires, rhs.cold_index_lock_acquires);
        add(cold_index_lock_releases, rhs.cold_index_lock_releases);
        add(cold_index_snapshot_registers, rhs.cold_index_snapshot_registers);
        add(cold_index_route_empty, rhs.cold_index_route_empty);
        add(cold_index_route_selective, rhs.cold_index_route_selective);
        add(cold_index_route_full, rhs.cold_index_route_full);
        add(cold_index_route_candidate_rows, rhs.cold_index_route_candidate_rows);
        add(cold_index_route_full_rows, rhs.cold_index_route_full_rows);
        add(cold_index_route_candidate_bytes, rhs.cold_index_route_candidate_bytes);
        add(cold_index_route_full_bytes, rhs.cold_index_route_full_bytes);
        add(cold_index_route_selective_cost, rhs.cold_index_route_selective_cost);
        add(cold_index_route_full_cost, rhs.cold_index_route_full_cost);
        add(cold_index_memory_reroutes, rhs.cold_index_memory_reroutes);
        add(cold_index_payload_bytes_requested, rhs.cold_index_payload_bytes_requested);
        add(cold_index_payload_bytes_delivered, rhs.cold_index_payload_bytes_delivered);
        add(cold_index_payload_rows_requested, rhs.cold_index_payload_rows_requested);
        add(cold_index_payload_rows_delivered, rhs.cold_index_payload_rows_delivered);
        add(cold_index_payload_rows_scanned, rhs.cold_index_payload_rows_scanned);
        add(cold_index_payload_bytes_scanned, rhs.cold_index_payload_bytes_scanned);
        add(cold_index_payload_rows_skipped, rhs.cold_index_payload_rows_skipped);
        add(cold_index_heap_tid_adjacent, rhs.cold_index_heap_tid_adjacent);
        add(cold_index_heap_tid_forward_gap, rhs.cold_index_heap_tid_forward_gap);
        add(cold_index_heap_tid_backward_jump, rhs.cold_index_heap_tid_backward_jump);
        add(cold_index_extent_crc_bytes, rhs.cold_index_extent_crc_bytes);
        add(cold_index_extent_crc_raw_bytes_hashed, rhs.cold_index_extent_crc_raw_bytes_hashed);
        add(cold_index_extent_crc_combined_bytes, rhs.cold_index_extent_crc_combined_bytes);
        add(cold_index_extent_crc_combine_rows, rhs.cold_index_extent_crc_combine_rows);
        add(cold_index_row_crc_bytes, rhs.cold_index_row_crc_bytes);
        add(cold_index_tuple_form_bytes, rhs.cold_index_tuple_form_bytes);
        add(cold_index_toast_values, rhs.cold_index_toast_values);
        add(cold_index_detoast_bytes, rhs.cold_index_detoast_bytes);
        add(cold_index_policy_ns, rhs.cold_index_policy_ns);
        add(cold_index_source_ns, rhs.cold_index_source_ns);
        add(cold_index_bind_ns, rhs.cold_index_bind_ns);
        add(cold_index_head_ns, rhs.cold_index_head_ns);
        add(cold_index_directory_ns, rhs.cold_index_directory_ns);
        add(cold_index_payload_ns, rhs.cold_index_payload_ns);
        add(cold_index_decode_ns, rhs.cold_index_decode_ns);
        add(cold_index_transfer_ns, rhs.cold_index_transfer_ns);
        add(cold_index_close_ns, rhs.cold_index_close_ns);
        add(cold_index_shared_blks_hit, rhs.cold_index_shared_blks_hit);
        add(cold_index_shared_blks_read, rhs.cold_index_shared_blks_read);
        are_stats_saturated = is_saturated;
    }
};

static_assert(std::is_trivially_copyable_v<pipeline_operational_stats>);
static_assert(std::is_standard_layout_v<pipeline_operational_stats>);

/// Combined job and work statistics for a statement or execution node.
/// Counters clamp at SQL limits and keep a failure flag.
struct pipeline_execution_stats {
    executed_backend requested_backend = executed_backend::none;
    executed_backend actual_backend = executed_backend::none;
    simd_chaining_policy_identity requested_simd_chaining_policy =
        simd_chaining_policy_identity::none;
    simd_chaining_policy_identity observed_simd_chaining_policy =
        simd_chaining_policy_identity::none;
    simd_alignment_algorithm_identity requested_simd_alignment_algorithm =
        simd_alignment_algorithm_identity::none;
    simd_alignment_algorithm_identity observed_simd_alignment_algorithm =
        simd_alignment_algorithm_identity::none;
    target_index_source requested_target_index_source = target_index_source::none;
    target_index_source actual_target_index_source = target_index_source::none;
    std::uint64_t persisted_generation_id = 0;
    std::uint64_t persisted_source_identity = 0;
    bool has_mixed_persisted_identity = false;
    std::uint64_t attempted_alignment_jobs = 0;
    std::uint64_t fully_attested_alignment_jobs = 0;
    std::uint64_t unattested_alignment_jobs = 0;
    // Remember the first attempted job so an aggregate failure can identify
    // the earliest affected job. These fields are internal to merging.
    std::uint64_t first_attempted_target_row = k_unset_job_ordinal;
    std::uint64_t first_attempted_query_ordinal = k_unset_job_ordinal;
    std::uint64_t first_unattested_target_row = k_unset_job_ordinal;
    std::uint64_t first_unattested_query_ordinal = k_unset_job_ordinal;
    execution_environment_snapshot environment{};
    pipeline_job_stage_coverage stages{};
    pipeline_operational_stats operational{};

    /// Mark every attempted job as unvalidated after an aggregate failure.
    /// Keep the first job's coordinates for the failure report.
    void reclassify_all_attempted_jobs() noexcept {
        if (attempted_alignment_jobs == 0)
            return;
        fully_attested_alignment_jobs = 0;
        unattested_alignment_jobs = attempted_alignment_jobs;
        // The failure now affects all jobs, including the first attempt.
        first_unattested_target_row = first_attempted_target_row;
        first_unattested_query_ordinal = first_attempted_query_ordinal;
    }

    /// Validate and add one target-query job. The caller must submit each job once.
    /// Conflicts, rejected execution or overflow remove validation from all jobs.
    void merge_job(const pipeline_job_execution_stats& job
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                   , const bool authenticated_query_empty_recomputed_bypass = false
#endif
                   ) noexcept {
        bool attested = is_job_execution_stats_valid(job
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            , operational.observed_minimizer_algorithm
            , operational.observed_chaining_algorithm
            , operational.observed_base_alignment_algorithm
            , authenticated_query_empty_recomputed_bypass
#endif
        );
        if ((requested_backend != executed_backend::none &&
             requested_backend != job.requested_backend) ||
            (actual_backend != executed_backend::none && actual_backend != job.actual_backend) ||
            (requested_simd_chaining_policy != simd_chaining_policy_identity::none &&
             requested_simd_chaining_policy != job.requested_simd_chaining_policy) ||
            (observed_simd_chaining_policy != simd_chaining_policy_identity::none &&
             observed_simd_chaining_policy != job.observed_simd_chaining_policy) ||
            (requested_simd_alignment_algorithm != simd_alignment_algorithm_identity::none &&
             requested_simd_alignment_algorithm != job.requested_simd_alignment_algorithm) ||
            (observed_simd_alignment_algorithm != simd_alignment_algorithm_identity::none &&
             observed_simd_alignment_algorithm != job.observed_simd_alignment_algorithm) ||
            (requested_target_index_source != target_index_source::none &&
             requested_target_index_source != job.requested_target_index_source) ||
            (actual_target_index_source != target_index_source::none &&
             actual_target_index_source != job.actual_target_index_source) ||
            (!execution_environment_snapshot::is_unset(environment) &&
             (!execution_environment_snapshot::has_same_payload_identity(environment,
                                                                         job.environment) ||
              environment.is_mixed_invalid || job.environment.is_mixed_invalid))) {
            attested = false;
        }

        if (job.actual_target_index_source == target_index_source::persisted) {
            const bool complete_job_identity =
                job.persisted_generation_id != 0 && job.persisted_source_identity != 0;
            if (!complete_job_identity) {
                has_mixed_persisted_identity = true;
                attested = false;
            } else if (persisted_generation_id == 0 && persisted_source_identity == 0) {
                persisted_generation_id = job.persisted_generation_id;
                persisted_source_identity = job.persisted_source_identity;
            } else if (persisted_generation_id != job.persisted_generation_id ||
                       persisted_source_identity != job.persisted_source_identity) {
                has_mixed_persisted_identity = true;
                attested = false;
            }
        } else if (job.persisted_generation_id != 0 || job.persisted_source_identity != 0) {
            has_mixed_persisted_identity = true;
            attested = false;
        }

        requested_backend = merge_backend_identity(requested_backend, job.requested_backend);
        actual_backend = merge_backend_identity(actual_backend, job.actual_backend);
        requested_simd_chaining_policy = merge_simd_chaining_policy_identity(
            requested_simd_chaining_policy, job.requested_simd_chaining_policy);
        observed_simd_chaining_policy = merge_simd_chaining_policy_identity(
            observed_simd_chaining_policy, job.observed_simd_chaining_policy);
        requested_simd_alignment_algorithm = merge_simd_alignment_algorithm_identity(
            requested_simd_alignment_algorithm, job.requested_simd_alignment_algorithm);
        observed_simd_alignment_algorithm = merge_simd_alignment_algorithm_identity(
            observed_simd_alignment_algorithm, job.observed_simd_alignment_algorithm);
        requested_target_index_source = merge_target_index_identity(
            requested_target_index_source, job.requested_target_index_source);
        actual_target_index_source =
            merge_target_index_identity(actual_target_index_source, job.actual_target_index_source);
        environment.merge(job.environment);

        if (first_attempted_target_row == k_unset_job_ordinal) {
            first_attempted_target_row = job.logical_target_row;
            first_attempted_query_ordinal = job.query_ordinal;
        }
        bool is_saturated = merge_counter(attempted_alignment_jobs, 1);
        if (attested) {
            is_saturated = merge_counter(fully_attested_alignment_jobs, 1) || is_saturated;
        } else {
            is_saturated = merge_counter(unattested_alignment_jobs, 1) || is_saturated;
            if (first_unattested_target_row == k_unset_job_ordinal) {
                first_unattested_target_row = job.logical_target_row;
                first_unattested_query_ordinal = job.query_ordinal;
            }
        }
        is_saturated = stages.merge(job.stages) || is_saturated;
        operational.are_stats_saturated = operational.are_stats_saturated || is_saturated;
        if (!attested || unattested_alignment_jobs != 0 || operational.are_stats_saturated ||
            operational.has_operational_identity_conflict || operational.backend_declines != 0 ||
            operational.backend_fallbacks != 0 ||
            operational.simd_chaining_policy_rejections != 0 ||
            operational.are_simd_chaining_policy_stats_saturated) {
            reclassify_all_attempted_jobs();
        }
    }

    /// Add a buffer's work statistics once and invalidate job validation on failure.
    void merge_operational(const pipeline_operational_stats& batch) noexcept {
        operational.merge(batch);
        if (operational.are_stats_saturated || operational.has_operational_identity_conflict ||
            operational.backend_declines != 0 || operational.backend_fallbacks != 0 ||
            operational.simd_chaining_policy_rejections != 0 ||
            operational.are_simd_chaining_policy_stats_saturated) {
            reclassify_all_attempted_jobs();
        }
    }

    /// Check for recorded GPU activity, including setup, dispatch or timing.
    [[nodiscard]] static constexpr bool
    has_gpu_execution_stats(const pipeline_execution_stats& stats) noexcept {
        const auto& operational = stats.operational;
        for (std::size_t i = 0; i < k_pipeline_stage_count; ++i) {
            if (stats.stages.coverage[i]
                        .executed_jobs[engine_index(execution_engine::vulkan_gpu)] != 0 ||
                operational.timing[i].gpu_device_ns != 0) {
                return true;
            }
        }
        return operational.gpu_runtime_init_ns != 0 || operational.gpu_pipeline_cache_loads != 0 ||
               operational.gpu_pipeline_cache_seed_bytes != 0 ||
               operational.gpu_pipeline_cache_load_ns != 0 ||
               operational.gpu_pipeline_creation_ns != 0 || operational.gpu_submit_cpu_ns != 0 ||
               operational.gpu_wait_cpu_ns != 0 || operational.gpu_ordinal_readback_cpu_ns != 0 ||
               operational.gpu_result_readback_cpu_ns != 0 ||
               operational.gpu_device_critical_path_ns != 0 ||
               operational.gpu_timestamp_query_failures != 0 ||
               operational.gpu_partition_select_dispatches != 0 ||
               operational.gpu_partition_anchor_merge_dispatches != 0 ||
               operational.gpu_index_build_candidates != 0 ||
               operational.gpu_index_build_canonical_emitted != 0 ||
               operational.gpu_index_build_overlap_duplicates_rejected != 0 ||
               operational.gpu_index_build_noncanonical_extras_rejected != 0 ||
               operational.gpu_index_build_surviving_extras != 0 ||
               operational.gpu_persisted_resident_uploads != 0 ||
               operational.gpu_persisted_resident_cache_hits != 0 ||
               operational.gpu_overlap_anchor_duplicates != 0 ||
               operational.gpu_seed_dispatches != 0 || operational.gpu_chain_dispatches != 0 ||
               operational.gpu_chain_backtrack_dispatches != 0 ||
               operational.gpu_coordinate_vote_dispatches != 0 ||
               operational.gpu_extend_dispatches != 0 ||
               operational.gpu_target_window_request_dispatches != 0 ||
               operational.gpu_target_window_logical_units_requested != 0 ||
               operational.gpu_queue_submits != 0 || operational.gpu_host_waits != 0 ||
               operational.gpu_buffer_creates != 0 || operational.gpu_memory_allocations != 0 ||
               operational.gpu_pipeline_creates != 0;
    }

    /// Check the environment of a mixed-backend diagnostic whose jobs are all
    /// unvalidated. This keeps diagnostic details available after route validation fails.
    [[nodiscard]] static constexpr bool
    is_diagnostic_environment_valid(const pipeline_execution_stats& stats) noexcept {
        if (stats.actual_backend != executed_backend::mixed_invalid ||
            stats.attempted_alignment_jobs == 0 || stats.fully_attested_alignment_jobs != 0 ||
            stats.unattested_alignment_jobs != stats.attempted_alignment_jobs) {
            return false;
        }
        if (is_concrete_backend(stats.requested_backend)) {
            return is_environment_valid_for_backend(stats.environment, stats.requested_backend);
        }
        if (stats.requested_backend != executed_backend::mixed_invalid) {
            return false;
        }
        const executed_backend environment_backend = has_gpu_execution_stats(stats)
                                                         ? executed_backend::vulkan_gpu
                                                         : executed_backend::scalar_cpu;
        return is_environment_valid_for_backend(stats.environment, environment_backend);
    }

    /// Check the environment before combining aggregates. Empty job totals need
    /// an unset environment. Completed work needs a valid route or diagnostic record.
    [[nodiscard]] static constexpr bool
    is_aggregate_environment_operand_valid(const pipeline_execution_stats& stats) noexcept {
        if (stats.attempted_alignment_jobs == 0) {
            return execution_environment_snapshot::is_unset(stats.environment);
        }
        if (is_concrete_backend(stats.actual_backend)) {
            return is_environment_valid_for_backend(stats.environment, stats.actual_backend);
        }
        // Mixed-backend diagnostics may keep a valid environment when every
        // job is marked unvalidated. That preserves useful failure details.
        return is_diagnostic_environment_valid(stats);
    }

    /// Check that stage counts match the route and job total. Required preparation
    /// stages cover all jobs, except allowed empty-query bypasses. Later stages
    /// may cover a subset. Unexpected engines and fallbacks fail validation.
    [[nodiscard]] static constexpr bool
    is_aggregate_stage_coverage_valid(const pipeline_execution_stats& stats) noexcept {
        if (!is_concrete_backend(stats.actual_backend) ||
            !is_concrete_target_index_source(stats.actual_target_index_source)) {
            return false;
        }
        for (std::size_t i = 0; i < k_pipeline_stage_count; ++i) {
            const auto stage = static_cast<pipeline_stage>(i);
            const stage_coverage& coverage = stats.stages.coverage[i];
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            if (stats.actual_backend == executed_backend::hybrid_cpu_vulkan) {
                const bool query_empty_recomputed_target_stage =
                    stats.actual_target_index_source == target_index_source::recomputed &&
                    stats.operational.observed_minimizer_algorithm ==
                        minimizer_algorithm_identity::vulkan_minimap_integer &&
                    (stage == pipeline_stage::target_sketch ||
                     stage == pipeline_stage::target_index_build);
                if (!is_hybrid_stage_coverage_exclusive(coverage,
                        stats.actual_target_index_source, stage,
                        stats.operational.observed_minimizer_algorithm,
                        stats.operational.observed_chaining_algorithm,
                        stats.operational.observed_base_alignment_algorithm,
                        stats.attempted_alignment_jobs) ||
                    (is_front_end_eligibility_required(stats.actual_target_index_source, stage) &&
                     !query_empty_recomputed_target_stage &&
                     coverage.eligible_jobs != stats.attempted_alignment_jobs)) {
                    return false;
                }
                continue;
            }
            const execution_engine expected_engine = expected_execution_engine(
                stats.actual_backend == executed_backend::mixed_cpu ? executed_backend::scalar_cpu
                                                                    : stats.actual_backend,
                stats.actual_target_index_source,
                stage);
#else
            const execution_engine expected_engine = expected_execution_engine(
                stats.actual_backend, stats.actual_target_index_source, stage);
#endif
            if (coverage.fallback_jobs != 0 ||
                coverage.eligible_jobs > stats.attempted_alignment_jobs) {
                return false;
            }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            if (stats.actual_backend == executed_backend::mixed_cpu) {
                const std::size_t scalar = engine_index(execution_engine::scalar_cpu);
                const std::size_t simd = engine_index(execution_engine::simd_cpu);
                const bool selectable_cpu_stage = stage == pipeline_stage::query_sketch ||
                                                  stage == pipeline_stage::target_sketch ||
                                                  stage == pipeline_stage::chain_score ||
                                                  stage == pipeline_stage::chain_backtrack ||
                                                  stage == pipeline_stage::left_extension ||
                                                  stage == pipeline_stage::gap_extension ||
                                                  stage == pipeline_stage::right_extension;
                if (selectable_cpu_stage) {
                    for (std::size_t engine = 0; engine < coverage.executed_jobs.size(); ++engine) {
                        if (engine != scalar && engine != simd &&
                            coverage.executed_jobs[engine] != 0) {
                            return false;
                        }
                    }
                    if (coverage.executed_jobs[scalar] + coverage.executed_jobs[simd] !=
                        coverage.eligible_jobs) {
                        return false;
                    }
                    if (is_front_end_eligibility_required(stats.actual_target_index_source,
                                                          stage) &&
                        coverage.eligible_jobs != stats.attempted_alignment_jobs) {
                        return false;
                    }
                    continue;
                }
            }
#endif
            if (expected_engine == execution_engine::count) {
                if (coverage.eligible_jobs != 0)
                    return false;
                for (const std::uint64_t executed : coverage.executed_jobs) {
                    if (executed != 0)
                        return false;
                }
                continue;
            }
            for (std::size_t engine = 0; engine < coverage.executed_jobs.size(); ++engine) {
                const std::uint64_t expected =
                    engine == engine_index(expected_engine) ? coverage.eligible_jobs : 0;
                if (coverage.executed_jobs[engine] != expected)
                    return false;
            }
            if (is_front_end_eligibility_required(stats.actual_target_index_source, stage) &&
                coverage.eligible_jobs != stats.attempted_alignment_jobs) {
                return false;
            }
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (stats.actual_backend == executed_backend::hybrid_cpu_vulkan &&
            stats.actual_target_index_source == target_index_source::recomputed &&
            stats.operational.observed_minimizer_algorithm ==
                minimizer_algorithm_identity::vulkan_minimap_integer) {
            const auto target_sketch = stats.stages.coverage[
                stage_index(pipeline_stage::target_sketch)].eligible_jobs;
            const auto target_index = stats.stages.coverage[
                stage_index(pipeline_stage::target_index_build)].eligible_jobs;
            if (target_sketch != target_index) {
                return false;
            }
        }
#endif
        return true;
    }

    /// Recognize an untouched aggregate that can be merged without changing a result.
    /// Zero jobs alone is insufficient: labels, environment, counters and flags
    /// must also have their initial values.
    [[nodiscard]] static constexpr bool
    is_exact_empty_stats_operand(const pipeline_execution_stats& stats) noexcept {
        return stats.attempted_alignment_jobs == 0 && stats.fully_attested_alignment_jobs == 0 &&
               stats.unattested_alignment_jobs == 0 &&
               stats.requested_backend == executed_backend::none &&
               stats.actual_backend == executed_backend::none &&
               stats.requested_simd_chaining_policy == simd_chaining_policy_identity::none &&
               stats.observed_simd_chaining_policy == simd_chaining_policy_identity::none &&
               stats.requested_simd_alignment_algorithm ==
                   simd_alignment_algorithm_identity::none &&
               stats.observed_simd_alignment_algorithm == simd_alignment_algorithm_identity::none &&
               stats.requested_target_index_source == target_index_source::none &&
               stats.actual_target_index_source == target_index_source::none &&
               stats.persisted_generation_id == 0 && stats.persisted_source_identity == 0 &&
               !stats.has_mixed_persisted_identity &&
               stats.first_attempted_target_row == k_unset_job_ordinal &&
               stats.first_attempted_query_ordinal == k_unset_job_ordinal &&
               stats.first_unattested_target_row == k_unset_job_ordinal &&
               stats.first_unattested_query_ordinal == k_unset_job_ordinal &&
               execution_environment_snapshot::is_unset(stats.environment) &&
               stats.stages == pipeline_job_stage_coverage{} &&
               stats.operational == pipeline_operational_stats{};
    }

    /// Check whether merging may preserve all jobs' validated status.
    /// Accept an untouched empty record or a complete valid aggregate with no
    /// fallback, overflow or identity conflict.
    [[nodiscard]] static constexpr bool
    can_retain_full_attestation(const pipeline_execution_stats& stats) noexcept {
        if (is_exact_empty_stats_operand(stats))
            return true;
        if (stats.attempted_alignment_jobs == 0 ||
            stats.fully_attested_alignment_jobs != stats.attempted_alignment_jobs ||
            stats.unattested_alignment_jobs != 0 ||
            stats.first_attempted_target_row == k_unset_job_ordinal ||
            stats.first_attempted_query_ordinal == k_unset_job_ordinal ||
            stats.first_unattested_target_row != k_unset_job_ordinal ||
            stats.first_unattested_query_ordinal != k_unset_job_ordinal ||
            stats.requested_backend != stats.actual_backend ||
            !is_concrete_backend(stats.actual_backend) ||
            !is_simd_chaining_policy_identity_valid_for_backend(
                stats.actual_backend,
                stats.requested_simd_chaining_policy,
                stats.observed_simd_chaining_policy) ||
            !is_simd_alignment_algorithm_identity_valid_for_backend(
                stats.actual_backend,
                stats.requested_simd_alignment_algorithm,
                stats.observed_simd_alignment_algorithm) ||
            stats.requested_target_index_source != stats.actual_target_index_source ||
            !is_concrete_target_index_source(stats.actual_target_index_source) ||
            stats.has_mixed_persisted_identity ||
            stats.operational.has_operational_identity_conflict ||
            stats.operational.are_stats_saturated || stats.operational.backend_declines != 0 ||
            stats.operational.backend_fallbacks != 0 ||
            stats.operational.simd_chaining_policy_rejections != 0 ||
            stats.operational.are_simd_chaining_policy_stats_saturated ||
            execution_environment_snapshot::has_no_highway_fields(stats.environment) ||
            !is_environment_valid_for_backend(stats.environment, stats.actual_backend) ||
            !is_aggregate_stage_coverage_valid(stats)) {
            return false;
        }
        if (stats.actual_target_index_source == target_index_source::persisted) {
            return stats.persisted_generation_id != 0 &&
                   stats.persisted_generation_id <= k_sql_bigint_value_max &&
                   stats.persisted_source_identity != 0;
        }
        return stats.persisted_generation_id == 0 && stats.persisted_source_identity == 0;
    }

    /// Combine completed buffer or node statistics. Validate both inputs before
    /// merging so missing or conflicting records cannot become accepted totals.
    void merge(const pipeline_execution_stats& rhs) noexcept {
        // Check both inputs before copying fields. A valid incoming environment
        // cannot repair a missing environment in an earlier nonempty record.
        const bool lhs_environment_valid = is_aggregate_environment_operand_valid(*this);
        const bool rhs_environment_valid = is_aggregate_environment_operand_valid(rhs);
        const bool environment_drift =
            lhs_environment_valid && rhs_environment_valid && attempted_alignment_jobs != 0 &&
            rhs.attempted_alignment_jobs != 0 &&
            !execution_environment_snapshot::has_same_payload_identity(environment,
                                                                       rhs.environment);
        const bool lhs_empty = is_exact_empty_stats_operand(*this);
        const bool rhs_empty = is_exact_empty_stats_operand(rhs);
        const bool backend_identity_compatible =
            lhs_empty || rhs_empty ||
            (requested_backend == rhs.requested_backend && actual_backend == rhs.actual_backend);
        const bool simd_policy_identity_compatible =
            lhs_empty || rhs_empty ||
            (requested_simd_chaining_policy == rhs.requested_simd_chaining_policy &&
             observed_simd_chaining_policy == rhs.observed_simd_chaining_policy);
        const bool simd_alignment_identity_compatible =
            lhs_empty || rhs_empty ||
            (requested_simd_alignment_algorithm == rhs.requested_simd_alignment_algorithm &&
             observed_simd_alignment_algorithm == rhs.observed_simd_alignment_algorithm);
        const bool source_identity_compatible =
            lhs_empty || rhs_empty ||
            (requested_target_index_source == rhs.requested_target_index_source &&
             actual_target_index_source == rhs.actual_target_index_source);
        const bool persisted_identity_compatible =
            lhs_empty || rhs_empty ||
            (persisted_generation_id == rhs.persisted_generation_id &&
             persisted_source_identity == rhs.persisted_source_identity &&
             has_mixed_persisted_identity == rhs.has_mixed_persisted_identity);
        // Different executed Highway targets invalidate the combined result
        // immediately. A truly empty record remains safe to merge.
        const bool highway_binding_identity_compatible =
            lhs_empty || rhs_empty ||
            (environment.highway_executed_target == rhs.environment.highway_executed_target &&
             environment.is_highway_executed_target_mixed ==
                 rhs.environment.is_highway_executed_target_mixed);
        const bool retain_full_attestation =
            can_retain_full_attestation(*this) && can_retain_full_attestation(rhs) &&
            lhs_environment_valid && rhs_environment_valid && !environment_drift &&
            backend_identity_compatible && simd_policy_identity_compatible &&
            simd_alignment_identity_compatible && source_identity_compatible &&
            persisted_identity_compatible && highway_binding_identity_compatible;

        const std::uint64_t lhs_attempted = attempted_alignment_jobs;
        const std::uint64_t rhs_attempted = rhs.attempted_alignment_jobs;
        const std::uint64_t lhs_fully_attested =
            retain_full_attestation ? fully_attested_alignment_jobs : 0;
        const std::uint64_t lhs_unattested =
            retain_full_attestation ? unattested_alignment_jobs : lhs_attempted;
        const std::uint64_t rhs_fully_attested =
            retain_full_attestation ? rhs.fully_attested_alignment_jobs : 0;
        const std::uint64_t rhs_unattested =
            retain_full_attestation ? rhs.unattested_alignment_jobs : rhs_attempted;

        fully_attested_alignment_jobs = lhs_fully_attested;
        unattested_alignment_jobs = lhs_unattested;
        if (!retain_full_attestation && (lhs_attempted != 0 || rhs_attempted != 0)) {
            if (lhs_attempted != 0) {
                first_unattested_target_row = first_unattested_target_row != k_unset_job_ordinal
                                                  ? first_unattested_target_row
                                                  : first_attempted_target_row;
                first_unattested_query_ordinal =
                    first_unattested_query_ordinal != k_unset_job_ordinal
                        ? first_unattested_query_ordinal
                        : first_attempted_query_ordinal;
            } else {
                first_unattested_target_row = rhs.first_unattested_target_row != k_unset_job_ordinal
                                                  ? rhs.first_unattested_target_row
                                                  : rhs.first_attempted_target_row;
                first_unattested_query_ordinal =
                    rhs.first_unattested_query_ordinal != k_unset_job_ordinal
                        ? rhs.first_unattested_query_ordinal
                        : rhs.first_attempted_query_ordinal;
            }
        }

        const auto identity_invalid = [](target_index_source source,
                                         std::uint64_t generation,
                                         std::uint64_t identity) noexcept {
            if (source == target_index_source::persisted) {
                return generation == 0 || identity == 0;
            }
            if (source == target_index_source::recomputed || source == target_index_source::none) {
                return generation != 0 || identity != 0;
            }
            return true;
        };
        if (identity_invalid(
                actual_target_index_source, persisted_generation_id, persisted_source_identity) ||
            identity_invalid(rhs.actual_target_index_source,
                             rhs.persisted_generation_id,
                             rhs.persisted_source_identity)) {
            has_mixed_persisted_identity = true;
        }
        requested_backend = merge_backend_identity(requested_backend, rhs.requested_backend);
        actual_backend = merge_backend_identity(actual_backend, rhs.actual_backend);
        requested_simd_chaining_policy = merge_simd_chaining_policy_identity(
            requested_simd_chaining_policy, rhs.requested_simd_chaining_policy);
        observed_simd_chaining_policy = merge_simd_chaining_policy_identity(
            observed_simd_chaining_policy, rhs.observed_simd_chaining_policy);
        requested_simd_alignment_algorithm = merge_simd_alignment_algorithm_identity(
            requested_simd_alignment_algorithm, rhs.requested_simd_alignment_algorithm);
        observed_simd_alignment_algorithm = merge_simd_alignment_algorithm_identity(
            observed_simd_alignment_algorithm, rhs.observed_simd_alignment_algorithm);
        requested_target_index_source = merge_target_index_identity(
            requested_target_index_source, rhs.requested_target_index_source);
        actual_target_index_source =
            merge_target_index_identity(actual_target_index_source, rhs.actual_target_index_source);

        if ((persisted_generation_id == 0) != (persisted_source_identity == 0) ||
            (rhs.persisted_generation_id == 0) != (rhs.persisted_source_identity == 0)) {
            has_mixed_persisted_identity = true;
        }
        if (rhs.persisted_generation_id != 0 && rhs.persisted_source_identity != 0) {
            if (persisted_generation_id == 0 && persisted_source_identity == 0) {
                persisted_generation_id = rhs.persisted_generation_id;
                persisted_source_identity = rhs.persisted_source_identity;
            } else if (persisted_generation_id != rhs.persisted_generation_id ||
                       persisted_source_identity != rhs.persisted_source_identity) {
                has_mixed_persisted_identity = true;
            }
        }
        has_mixed_persisted_identity =
            has_mixed_persisted_identity || rhs.has_mixed_persisted_identity;

        if (!execution_environment_snapshot::is_unset(rhs.environment) ||
            rhs.attempted_alignment_jobs != 0) {
            environment.merge(rhs.environment);
        }
        if (!lhs_environment_valid || !rhs_environment_valid || environment_drift) {
            environment.is_mixed_invalid = true;
        }

        bool is_saturated = merge_counter(attempted_alignment_jobs, rhs.attempted_alignment_jobs);
        is_saturated =
            merge_counter(fully_attested_alignment_jobs, rhs_fully_attested) || is_saturated;
        is_saturated = merge_counter(unattested_alignment_jobs, rhs_unattested) || is_saturated;
        is_saturated = stages.merge(rhs.stages) || is_saturated;
        if (first_unattested_target_row == k_unset_job_ordinal &&
            rhs.first_unattested_target_row != k_unset_job_ordinal) {
            first_unattested_target_row = rhs.first_unattested_target_row;
            first_unattested_query_ordinal = rhs.first_unattested_query_ordinal;
        }
        if (first_attempted_target_row == k_unset_job_ordinal &&
            rhs.first_attempted_target_row != k_unset_job_ordinal) {
            first_attempted_target_row = rhs.first_attempted_target_row;
            first_attempted_query_ordinal = rhs.first_attempted_query_ordinal;
        }
        operational.merge(rhs.operational);
        operational.are_stats_saturated = operational.are_stats_saturated || is_saturated;
        if (operational.are_stats_saturated || operational.has_operational_identity_conflict ||
            operational.backend_declines != 0 || operational.backend_fallbacks != 0 ||
            operational.simd_chaining_policy_rejections != 0 ||
            operational.are_simd_chaining_policy_stats_saturated ||
            requested_simd_chaining_policy == simd_chaining_policy_identity::mixed_invalid ||
            observed_simd_chaining_policy == simd_chaining_policy_identity::mixed_invalid ||
            requested_simd_alignment_algorithm ==
                simd_alignment_algorithm_identity::mixed_invalid ||
            observed_simd_alignment_algorithm == simd_alignment_algorithm_identity::mixed_invalid) {
            reclassify_all_attempted_jobs();
        }
    }
};

static_assert(std::is_trivially_copyable_v<pipeline_execution_stats>);
static_assert(std::is_standard_layout_v<pipeline_execution_stats>);

} // namespace bioseq::pipeline_stats
