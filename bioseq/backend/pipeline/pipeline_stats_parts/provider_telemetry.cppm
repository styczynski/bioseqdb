/**
 * @file pipeline/pipeline_stats_parts/provider_telemetry.cppm
 * @brief Storage reader, mapped-file and provider statistics.
 */
export module bioseq.pipeline.stats:provider_telemetry;

import std;
import :identities;

export namespace bioseq::pipeline_stats {

/// Statistics, from reading a hash-major index. Repeated reader and memory
/// ownership records must agree Work counters are added with overflow checks.
struct hash_major_index_reader_stats {
    std::uint64_t layout_revision = 0;
    std::uint64_t sessions = 0;
    bool identity_available = false;
    std::uint64_t generation_id = 0;
    std::array<std::byte, 16> source_identity{};
    std::uint64_t source_content_crc64 = 0;
    std::array<std::byte, 32> storage_contract_digest{};
    std::uint64_t directory_rows = 0;
    std::uint64_t directory_bytes = 0;
    std::uint64_t control_rows = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t posting_rows = 0;
    std::uint64_t posting_bytes = 0;
    std::uint64_t directory_row_crc_receipts = 0;
    std::uint64_t directory_stream_crc_receipts = 0;
    std::uint64_t control_row_crc_receipts = 0;
    std::uint64_t control_block_crc_receipts = 0;
    std::uint64_t control_semantic_receipts = 0;
    std::uint64_t posting_row_crc_receipts = 0;
    std::uint64_t posting_ranges_delivered = 0;
    std::uint64_t posting_range_crc_receipts = 0;
    std::uint64_t posting_semantic_receipts = 0;
    std::uint64_t authorized_posting_bytes = 0;
    std::uint64_t query_count = 0;
    std::uint64_t selected_block_count = 0;
    std::uint64_t accepted_hash_count = 0;
    std::uint64_t retained_position_count = 0;
    std::uint64_t directory_peak_bytes = 0;
    std::uint64_t control_peak_bytes = 0;
    std::uint64_t posting_peak_bytes = 0;
    std::uint64_t peak_admitted_bytes = 0;
    bool owner_bytes_available = false;
    std::uint64_t retained_owner_bytes = 0;
    std::uint64_t admitted_bytes = 0;
    std::uint64_t generic_source_fallback_calls = 0;
    std::uint64_t failures = 0;
    bool is_saturated = false;

    [[nodiscard]] constexpr bool
    operator==(const hash_major_index_reader_stats&) const noexcept = default;

    /// Add reader work and keep the largest memory peaks. Conflicting source
    /// or ownership records set has_identity_conflict. Return true on counter overflow.
    [[nodiscard]] bool merge(const hash_major_index_reader_stats& rhs,
                             bool& has_identity_conflict) noexcept {
        bool overflow = is_saturated || rhs.is_saturated;
        const auto add = [&overflow](std::uint64_t& destination,
                                     const std::uint64_t increment) noexcept {
            overflow = merge_counter(destination, increment) || overflow;
        };
        if (rhs.layout_revision != 0u) {
            if (layout_revision == 0u) {
                layout_revision = rhs.layout_revision;
            } else if (layout_revision != rhs.layout_revision) {
                has_identity_conflict = true;
            }
        }
        const bool destination_identity_payload =
            generation_id != 0u || source_identity != std::array<std::byte, 16>{} ||
            source_content_crc64 != 0u || storage_contract_digest != std::array<std::byte, 32>{};
        const bool incoming_identity_payload =
            rhs.generation_id != 0u || rhs.source_identity != std::array<std::byte, 16>{} ||
            rhs.source_content_crc64 != 0u ||
            rhs.storage_contract_digest != std::array<std::byte, 32>{};
        if ((!identity_available && destination_identity_payload) ||
            (!rhs.identity_available && incoming_identity_payload)) {
            has_identity_conflict = true;
        }
        if (rhs.identity_available) {
            if (!identity_available) {
                identity_available = true;
                generation_id = rhs.generation_id;
                source_identity = rhs.source_identity;
                source_content_crc64 = rhs.source_content_crc64;
                storage_contract_digest = rhs.storage_contract_digest;
            } else if (generation_id != rhs.generation_id ||
                       source_identity != rhs.source_identity ||
                       source_content_crc64 != rhs.source_content_crc64 ||
                       storage_contract_digest != rhs.storage_contract_digest) {
                has_identity_conflict = true;
            }
        }
        const bool destination_owner_payload = retained_owner_bytes != 0u || admitted_bytes != 0u;
        const bool incoming_owner_payload =
            rhs.retained_owner_bytes != 0u || rhs.admitted_bytes != 0u;
        if ((!owner_bytes_available && destination_owner_payload) ||
            (!rhs.owner_bytes_available && incoming_owner_payload)) {
            has_identity_conflict = true;
        }
        if (rhs.owner_bytes_available) {
            if (!owner_bytes_available) {
                owner_bytes_available = true;
                retained_owner_bytes = rhs.retained_owner_bytes;
                admitted_bytes = rhs.admitted_bytes;
            } else if (retained_owner_bytes != rhs.retained_owner_bytes ||
                       admitted_bytes != rhs.admitted_bytes) {
                has_identity_conflict = true;
            }
        }
        add(sessions, rhs.sessions);
        add(directory_rows, rhs.directory_rows);
        add(directory_bytes, rhs.directory_bytes);
        add(control_rows, rhs.control_rows);
        add(control_bytes, rhs.control_bytes);
        add(posting_rows, rhs.posting_rows);
        add(posting_bytes, rhs.posting_bytes);
        add(directory_row_crc_receipts, rhs.directory_row_crc_receipts);
        add(directory_stream_crc_receipts, rhs.directory_stream_crc_receipts);
        add(control_row_crc_receipts, rhs.control_row_crc_receipts);
        add(control_block_crc_receipts, rhs.control_block_crc_receipts);
        add(control_semantic_receipts, rhs.control_semantic_receipts);
        add(posting_row_crc_receipts, rhs.posting_row_crc_receipts);
        add(posting_ranges_delivered, rhs.posting_ranges_delivered);
        add(posting_range_crc_receipts, rhs.posting_range_crc_receipts);
        add(posting_semantic_receipts, rhs.posting_semantic_receipts);
        add(authorized_posting_bytes, rhs.authorized_posting_bytes);
        add(query_count, rhs.query_count);
        add(selected_block_count, rhs.selected_block_count);
        add(accepted_hash_count, rhs.accepted_hash_count);
        add(retained_position_count, rhs.retained_position_count);
        directory_peak_bytes = std::max(directory_peak_bytes, rhs.directory_peak_bytes);
        control_peak_bytes = std::max(control_peak_bytes, rhs.control_peak_bytes);
        posting_peak_bytes = std::max(posting_peak_bytes, rhs.posting_peak_bytes);
        peak_admitted_bytes = std::max(peak_admitted_bytes, rhs.peak_admitted_bytes);
        add(generic_source_fallback_calls, rhs.generic_source_fallback_calls);
        add(failures, rhs.failures);
        is_saturated = overflow;
        return overflow;
    }
};

/// Report codes for mapped-file preallocation and mapping advice.
/// These  fixed values describe success or failure separately from OS error codes.
enum class grouped_soa_operation_result : std::uint64_t {
    not_attempted = 0u,
    succeeded = 1u,
    failed = 2u,
};

/// Memory layout used to track validation of mapped extents.
enum class grouped_soa_state_layout : std::uint64_t {
    unselected = 0u,
    compact_aligned_planes = 1u,
    cache_line_padded_entries = 2u,
};

/// Access adfice selected for a read-only mapping.
enum class grouped_soa_mapping_advice : std::uint64_t {
    none = 0u,
    random = 1u,
};

static_assert(static_cast<std::uint64_t>(grouped_soa_operation_result::not_attempted) == 0u);
static_assert(static_cast<std::uint64_t>(grouped_soa_operation_result::succeeded) == 1u);
static_assert(static_cast<std::uint64_t>(grouped_soa_operation_result::failed) == 2u);
static_assert(static_cast<std::uint64_t>(grouped_soa_state_layout::unselected) == 0u);
static_assert(static_cast<std::uint64_t>(grouped_soa_state_layout::compact_aligned_planes) == 1u);
static_assert(static_cast<std::uint64_t>(grouped_soa_state_layout::cache_line_padded_entries) ==
              2u);
static_assert(static_cast<std::uint64_t>(grouped_soa_mapping_advice::none) == 0u);
static_assert(static_cast<std::uint64_t>(grouped_soa_mapping_advice::random) == 1u);

/// Grouped-SoA storage activity, including provider selection, mapped-file
/// writes and lazy checksum checks. Counters clamp at the SQL bigint limit.
/// Presence flags distinguish an absent provider from provider code zero.
struct grouped_soa_provider_stats {
    std::uint64_t provider_configured = 0u;
    std::uint64_t provider_captured = 0u;
    std::uint64_t provider_persisted = 0u;
    std::uint64_t provider_opened = 0u;
    std::uint64_t provider_head_scans = 0u;
    std::uint64_t provider_head_rows = 0u;
    std::uint64_t provider_descriptor_scans = 0u;
    std::uint64_t provider_descriptor_rows = 0u;

    std::uint64_t payload_tuples_formed = 0u;
    std::uint64_t payload_tuples_inserted = 0u;
    std::uint64_t payload_tuples_reread = 0u;
    std::uint64_t mapped_sink_calls = 0u;
    std::uint64_t mapped_sink_bytes = 0u;
    std::uint64_t mapped_sink_flushes = 0u;
    std::uint64_t mapped_sink_coalescer_capacity_bytes = 0u;
    std::uint64_t mapped_sink_pwrite_calls = 0u;
    std::uint64_t mapped_sink_short_writes = 0u;
    std::uint64_t mapped_sink_eintr_retries = 0u;
    std::uint64_t mapped_sink_preallocation_bytes = 0u;
    std::uint64_t mapped_sink_preallocation_result = 0u;

    std::uint64_t header_crc_bytes = 0u;
    std::uint64_t directory_crc_bytes = 0u;
    std::uint64_t manifest_crc_bytes = 0u;
    std::uint64_t seal_aggregate_payload_crc_bytes = 0u;
    std::uint64_t open_aggregate_payload_crc_bytes = 0u;

    std::uint64_t lazy_extent_cas_winners = 0u;
    std::uint64_t lazy_extent_waiters = 0u;
    std::uint64_t lazy_extent_executions = 0u;
    std::uint64_t lazy_extent_reuses = 0u;
    std::uint64_t lazy_extent_crc_bytes = 0u;
    std::uint64_t lazy_extent_state_bytes = 0u;
    std::uint64_t lazy_extent_is_invalid_receipts = 0u;
    std::uint64_t lazy_state_layout = 0u;
    std::uint64_t mapping_bytes = 0u;
    std::uint64_t mapping_count = 0u;
    std::uint64_t mapping_resourceowner_releases = 0u;

    std::uint64_t file_action_high_water = 0u;
    std::uint64_t file_action_identity_mismatches = 0u;
    std::uint64_t reaper_descriptors_examined = 0u;
    std::uint64_t reaper_relation_rows_deleted = 0u;
    std::uint64_t reaper_payload_bytes_deleted = 0u;
    std::uint64_t reaper_files_queued = 0u;
    std::uint64_t reaper_remaining = 0u;
    std::uint64_t orphan_entries_examined = 0u;
    std::uint64_t orphan_files_acted = 0u;
    std::uint64_t orphan_bytes_acted = 0u;
    std::uint64_t orphan_remaining = 0u;
    std::uint64_t orphan_cursor_wraps = 0u;

    std::uint64_t madvise_choice = 0u;
    std::uint64_t madvise_result = 0u;

    // Presence flags distinguish an unrecorded value from a recorded zero.
    // These flags and conflict state are kept out  of serialized reports.
    bool provider_configured_available = false;
    bool provider_captured_available = false;
    bool provider_persisted_available = false;
    bool provider_opened_available = false;
    bool mapped_sink_preallocation_result_available = false;
    bool lazy_state_layout_available = false;
    bool madvise_choice_available = false;
    bool madvise_result_available = false;
    bool has_stable_value_conflict = false;
    bool is_saturated = false;

    [[nodiscard]] constexpr bool
    operator==(const grouped_soa_provider_stats&) const noexcept = default;

    /// Add to a counter and keep track of any SQL bigint overflow.
    void record_counter(std::uint64_t& destination, std::uint64_t increment) noexcept {
        is_saturated = merge_counter(destination, increment) || is_saturated;
    }

    /// Record the largest file-action occupancy seen, capped at SQL bigint max.
    void record_file_action_high_water(std::uint64_t value) noexcept {
        if (value > k_sql_bigint_value_max) {
            value = k_sql_bigint_value_max;
            is_saturated = true;
        }
        file_action_high_water = std::max(file_action_high_water, value);
    }

    /// Record the provider selected when configuring the operation.
    void record_provider_configured(std::uint64_t provider) noexcept {
        record_stable_value(provider_configured,
                            provider_configured_available,
                            provider,
                            has_stable_value_conflict);
    }

    /// Record the provider captured by this operation's execution state.
    void record_provider_captured(std::uint64_t provider) noexcept {
        record_stable_value(
            provider_captured, provider_captured_available, provider, has_stable_value_conflict);
    }

    /// Record the provider stored in the grouped-SoA image.
    void record_provider_persisted(std::uint64_t provider) noexcept {
        record_stable_value(
            provider_persisted, provider_persisted_available, provider, has_stable_value_conflict);
    }

    /// Record the provider found when opening the grouped-SoA image.
    void record_provider_opened(std::uint64_t provider) noexcept {
        record_stable_value(
            provider_opened, provider_opened_available, provider, has_stable_value_conflict);
    }

    /// Record whether mapped-file preallocation was attempted and succeeded.
    void record_mapped_sink_preallocation_result(grouped_soa_operation_result result) noexcept {
        record_stable_value(mapped_sink_preallocation_result,
                            mapped_sink_preallocation_result_available,
                            static_cast<std::uint64_t>(result),
                            has_stable_value_conflict);
    }

    /// Record the layout  used for lazy extent-validation state
    void record_lazy_state_layout(grouped_soa_state_layout layout) noexcept {
        record_stable_value(lazy_state_layout,
                            lazy_state_layout_available,
                            static_cast<std::uint64_t>(layout),
                            has_stable_value_conflict);
    }

    /// Record the mapping-advice choice and whether the request succeeded.
    void record_madvise(grouped_soa_mapping_advice choice,
                        grouped_soa_operation_result result) noexcept {
        record_stable_value(madvise_choice,
                            madvise_choice_available,
                            static_cast<std::uint64_t>(choice),
                            has_stable_value_conflict);
        record_stable_value(madvise_result,
                            madvise_result_available,
                            static_cast<std::uint64_t>(result),
                            has_stable_value_conflict);
    }

    /// Require all four providjr observations to be present, equal and conflict-free.
    [[nodiscard]] constexpr bool provider_identity_complete_and_consistent() const noexcept {
        return provider_configured_available && provider_captured_available &&
               provider_persisted_available && provider_opened_available &&
               !has_stable_value_conflict && provider_configured == provider_captured &&
               provider_configured == provider_persisted && provider_configured == provider_opened;
    }

    /// Combine provider statistics, checking that, repeated codes agree.
    /// Counts are added and file-action occupancy keeps its maximum.
    /// Return true on counter overflow and report identity conflicts separately.
    [[nodiscard]] bool merge(const grouped_soa_provider_stats& rhs,
                             bool& has_identity_conflict) noexcept {
        bool overflow = is_saturated || rhs.is_saturated;
        bool conflict = has_stable_value_conflict || rhs.has_stable_value_conflict;
        merge_stable_value(provider_configured,
                           provider_configured_available,
                           rhs.provider_configured,
                           rhs.provider_configured_available,
                           conflict);
        merge_stable_value(provider_captured,
                           provider_captured_available,
                           rhs.provider_captured,
                           rhs.provider_captured_available,
                           conflict);
        merge_stable_value(provider_persisted,
                           provider_persisted_available,
                           rhs.provider_persisted,
                           rhs.provider_persisted_available,
                           conflict);
        merge_stable_value(provider_opened,
                           provider_opened_available,
                           rhs.provider_opened,
                           rhs.provider_opened_available,
                           conflict);
        merge_stable_value(mapped_sink_preallocation_result,
                           mapped_sink_preallocation_result_available,
                           rhs.mapped_sink_preallocation_result,
                           rhs.mapped_sink_preallocation_result_available,
                           conflict);
        merge_stable_value(lazy_state_layout,
                           lazy_state_layout_available,
                           rhs.lazy_state_layout,
                           rhs.lazy_state_layout_available,
                           conflict);
        merge_stable_value(madvise_choice,
                           madvise_choice_available,
                           rhs.madvise_choice,
                           rhs.madvise_choice_available,
                           conflict);
        merge_stable_value(madvise_result,
                           madvise_result_available,
                           rhs.madvise_result,
                           rhs.madvise_result_available,
                           conflict);

        const auto add = [&overflow](std::uint64_t& destination, std::uint64_t increment) noexcept {
            overflow = merge_counter(destination, increment) || overflow;
        };
        add(provider_head_scans, rhs.provider_head_scans);
        add(provider_head_rows, rhs.provider_head_rows);
        add(provider_descriptor_scans, rhs.provider_descriptor_scans);
        add(provider_descriptor_rows, rhs.provider_descriptor_rows);
        add(payload_tuples_formed, rhs.payload_tuples_formed);
        add(payload_tuples_inserted, rhs.payload_tuples_inserted);
        add(payload_tuples_reread, rhs.payload_tuples_reread);
        add(mapped_sink_calls, rhs.mapped_sink_calls);
        add(mapped_sink_bytes, rhs.mapped_sink_bytes);
        add(mapped_sink_flushes, rhs.mapped_sink_flushes);
        add(mapped_sink_coalescer_capacity_bytes, rhs.mapped_sink_coalescer_capacity_bytes);
        add(mapped_sink_pwrite_calls, rhs.mapped_sink_pwrite_calls);
        add(mapped_sink_short_writes, rhs.mapped_sink_short_writes);
        add(mapped_sink_eintr_retries, rhs.mapped_sink_eintr_retries);
        add(mapped_sink_preallocation_bytes, rhs.mapped_sink_preallocation_bytes);
        add(header_crc_bytes, rhs.header_crc_bytes);
        add(directory_crc_bytes, rhs.directory_crc_bytes);
        add(manifest_crc_bytes, rhs.manifest_crc_bytes);
        add(seal_aggregate_payload_crc_bytes, rhs.seal_aggregate_payload_crc_bytes);
        add(open_aggregate_payload_crc_bytes, rhs.open_aggregate_payload_crc_bytes);
        add(lazy_extent_cas_winners, rhs.lazy_extent_cas_winners);
        add(lazy_extent_waiters, rhs.lazy_extent_waiters);
        add(lazy_extent_executions, rhs.lazy_extent_executions);
        add(lazy_extent_reuses, rhs.lazy_extent_reuses);
        add(lazy_extent_crc_bytes, rhs.lazy_extent_crc_bytes);
        add(lazy_extent_state_bytes, rhs.lazy_extent_state_bytes);
        add(lazy_extent_is_invalid_receipts, rhs.lazy_extent_is_invalid_receipts);
        add(mapping_bytes, rhs.mapping_bytes);
        add(mapping_count, rhs.mapping_count);
        add(mapping_resourceowner_releases, rhs.mapping_resourceowner_releases);
        const auto bounded_file_action_high_water =
            std::min(rhs.file_action_high_water, k_sql_bigint_value_max);
        overflow = overflow || rhs.file_action_high_water > k_sql_bigint_value_max;
        file_action_high_water = std::max(file_action_high_water, bounded_file_action_high_water);
        add(file_action_identity_mismatches, rhs.file_action_identity_mismatches);
        add(reaper_descriptors_examined, rhs.reaper_descriptors_examined);
        add(reaper_relation_rows_deleted, rhs.reaper_relation_rows_deleted);
        add(reaper_payload_bytes_deleted, rhs.reaper_payload_bytes_deleted);
        add(reaper_files_queued, rhs.reaper_files_queued);
        add(reaper_remaining, rhs.reaper_remaining);
        add(orphan_entries_examined, rhs.orphan_entries_examined);
        add(orphan_files_acted, rhs.orphan_files_acted);
        add(orphan_bytes_acted, rhs.orphan_bytes_acted);
        add(orphan_remaining, rhs.orphan_remaining);
        add(orphan_cursor_wraps, rhs.orphan_cursor_wraps);

        is_saturated = overflow;
        has_stable_value_conflict = conflict;
        has_identity_conflict = has_identity_conflict || conflict;
        return overflow;
    }

  private:
    /// Store the first value and mark a conflict if a  later value differs.
    static void record_stable_value(std::uint64_t& destination,
                                    bool& destination_available,
                                    std::uint64_t incoming,
                                    bool& conflict) noexcept {
        if (!destination_available) {
            destination = incoming;
            destination_available = true;
            return;
        }
        if (destination != incoming)
            conflict = true;
    }

    /// Merge an optional value and detect disagreement. Nonzero data with its
    /// presence flag unset also marks a conflict.
    static void merge_stable_value(std::uint64_t& destination,
                                   bool& destination_available,
                                   std::uint64_t incoming,
                                   bool incoming_available,
                                   bool& conflict) noexcept {
        if ((!destination_available && destination != 0u) ||
            (!incoming_available && incoming != 0u)) {
            conflict = true;
        }
        if (!incoming_available)
            return;
        record_stable_value(destination, destination_available, incoming, conflict);
    }
};

} // namespace bioseq::pipeline_stats
