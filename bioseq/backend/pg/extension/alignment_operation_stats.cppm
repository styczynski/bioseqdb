/**
 * @file alignment_operation_stats.cppm
 * @brief Alignment reader and persisted-GPU execution statistics.
 */
module;

#include <bioseq/pg_compat.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

module bioseq.postgres.extension;

import bioseq.pipeline.stats;
import bioseq.postgres.support;
import bioseq.postgres.table.alignment_execution;

namespace {

namespace alignment_exec = bioseq::postgres::table::alignment_execution;
using bioseq::pg::detail::fixed_hex_bytes;
using bioseq::pg::detail::fixed_hex_u64;

/** Serializes grouped-SoA and native fragment reader counters as JSON. */
[[nodiscard]] std::string minimizer_fragment_reader_stats_json(
        const alignment_exec::alignment_execution_snapshot& snapshot)
{
    constexpr std::uint32_t revision_number = 6u;
    const auto& operational = snapshot.pipeline.operational;
    std::string result =
        R"({"schema":"bioseq.grouped_soa_reader_memory_stats","revision":)"
        + std::to_string(revision_number);
    result.reserve(12'288u);
	const auto append_signed = [&result](
			const std::string_view key,
			const std::int64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	append_signed("execution_id", snapshot.execution_id);
	append_signed("backend_pid", snapshot.backend_pid);
	append("database_oid", snapshot.database_oid);
	append(
		"grouped_soa_reader_capacity_accounting_passes",
		operational.grouped_soa_reader_capacity_accounting_passes);
	append(
		"grouped_soa_reader_global_position_storage_preparations",
		operational.grouped_soa_reader_global_position_storage_preparations);
	append(
		"grouped_soa_reader_global_position_storage_allocations",
		operational.grouped_soa_reader_global_position_storage_allocations);
	append(
		"grouped_soa_reader_position_append_growth_reservations",
		operational.grouped_soa_reader_position_append_growth_reservations);
	append(
		"grouped_soa_reader_retained_position_count",
		operational.grouped_soa_reader_retained_position_count);
	append(
		"grouped_soa_reader_retained_position_capacity",
		operational.grouped_soa_reader_retained_position_capacity);
	append(
		"grouped_soa_reader_directory_capacity_bytes",
		operational.grouped_soa_reader_directory_capacity_bytes);
	append(
		"grouped_soa_reader_query_hash_capacity_bytes",
		operational.grouped_soa_reader_query_hash_capacity_bytes);
	append(
		"grouped_soa_reader_query_record_capacity_bytes",
		operational.grouped_soa_reader_query_record_capacity_bytes);
	append(
		"grouped_soa_reader_ordinal_route_capacity_bytes",
		operational.grouped_soa_reader_ordinal_route_capacity_bytes);
	append(
		"grouped_soa_reader_selected_extent_capacity_bytes",
		operational.grouped_soa_reader_selected_extent_capacity_bytes);
	append(
		"grouped_soa_reader_current_selected_control_capacity_bytes",
		operational.grouped_soa_reader_current_selected_control_capacity_bytes);
	append(
		"grouped_soa_reader_global_position_capacity_bytes",
		operational.grouped_soa_reader_global_position_capacity_bytes);
	append(
		"grouped_soa_reader_control_payload_capacity_bytes",
		operational.grouped_soa_reader_control_payload_capacity_bytes);
	append(
		"grouped_soa_reader_owned_capacity_bytes",
		operational.grouped_soa_reader_owned_capacity_bytes);
	append(
		"grouped_soa_reader_admitted_bytes",
		operational.grouped_soa_reader_admitted_bytes);
	append(
		"grouped_soa_reader_exact_position_image_checks",
		operational.grouped_soa_reader_exact_position_image_checks);
	append(
		"grouped_soa_reader_exact_position_image_failures",
		operational.grouped_soa_reader_exact_position_image_failures);
	append(
		"grouped_soa_reader_retained_layout_checks",
		operational.grouped_soa_reader_retained_layout_checks);
    append(
        "grouped_soa_reader_retained_layout_failures",
        operational.grouped_soa_reader_retained_layout_failures);
    {
        const auto& stats = operational.fragment_native_query_stats;
        append("fragment_native_waves", stats.waves);
        append("fragment_native_queries", stats.queries);
        append("fragment_native_occurrence_fill_ns",
            stats.occurrence_fill_ns);
        append("fragment_native_occurrence_radix_ns",
            stats.occurrence_radix_ns);
        append("fragment_native_unique_plan_build_ns",
            stats.unique_plan_build_ns);
        append("fragment_native_query_plan_radix_fill_ns",
            stats.query_plan_radix_fill_ns);
        append("fragment_native_control_count_pass_ns",
            stats.control_count_pass_ns);
        append("fragment_native_rejected_control_validation_ns",
            stats.rejected_control_validation_ns);
        append("fragment_native_orientation_layout_ns",
            stats.orientation_layout_ns);
        append("fragment_native_direct_emission_ns",
            stats.direct_emission_ns);
        append("fragment_native_local_sort_ns", stats.local_sort_ns);
        append("fragment_native_tie_normalization_ns",
            stats.tie_normalization_ns);
        append("fragment_native_control_count_bytes",
            stats.control_count_bytes);
        append("fragment_native_rejected_control_validation_bytes",
            stats.rejected_control_validation_bytes);
        append("fragment_native_pass2_position_bytes",
            stats.pass2_position_bytes);
        append("fragment_native_pass3_position_bytes",
            stats.pass3_position_bytes);
        append("fragment_native_anchors_constructed",
            stats.anchors_constructed);
        append("fragment_native_local_sort_bytes",
            stats.local_sort_bytes);
        append("fragment_native_tie_normalization_bytes",
            stats.tie_normalization_bytes);
        append("fragment_native_exact_final_bytes",
            stats.exact_final_bytes);
        append("fragment_native_peak_exact_final_bytes",
            stats.peak_exact_final_bytes);
        append("fragment_native_global_sort_calls_invoked",
            stats.global_sort_calls_invoked);
        append("fragment_native_global_sort_calls_skipped",
            stats.global_sort_calls_skipped);
        append("fragment_native_candidate_anchor_bytes",
            stats.candidate_anchor_bytes);
        append("fragment_native_plan_ordinal_bytes",
            stats.plan_ordinal_bytes);
        append("fragment_native_compaction_bytes",
            stats.compaction_bytes);
        append("fragment_native_final_copy_bytes",
            stats.final_copy_bytes);
        append("fragment_native_peak_occurrence_capacity_bytes",
            stats.peak_occurrence_capacity_bytes);
        append("fragment_native_peak_query_plan_capacity_bytes",
            stats.peak_query_plan_capacity_bytes);
        append("fragment_native_peak_query_radix_capacity_bytes",
            stats.peak_query_radix_capacity_bytes);
        append("fragment_native_peak_occurrence_count_capacity_bytes",
            stats.peak_occurrence_count_capacity_bytes);
        append("fragment_native_peak_block_layout_capacity_bytes",
            stats.peak_block_layout_capacity_bytes);
        append("fragment_native_peak_block_stats_capacity_bytes",
            stats.peak_block_stats_capacity_bytes);
        append("fragment_native_occurrence_allocations",
            stats.occurrence_allocations);
        append("fragment_native_query_plan_allocations",
            stats.query_plan_allocations);
        append("fragment_native_query_radix_allocations",
            stats.query_radix_allocations);
        append("fragment_native_occurrence_count_allocations",
            stats.occurrence_count_allocations);
        append("fragment_native_block_layout_allocations",
            stats.block_layout_allocations);
        append("fragment_native_block_stats_allocations",
            stats.block_stats_allocations);
        append("fragment_native_query_occurrences",
            stats.query_occurrences);
        append("fragment_native_unique_query_plans",
            stats.unique_query_plans);
        append("fragment_native_packed_occurrence_waves",
            stats.packed_occurrence_waves);
        append("fragment_native_wide_occurrence_waves",
            stats.wide_occurrence_waves);
        result += R"(,"are_fragment_native_stats_saturated":)";
        result += stats.saturated ? "true" : "false";
    }
	result += R"(,"are_grouped_soa_reader_memory_stats_saturated":)";
	result += operational.are_grouped_soa_reader_memory_stats_saturated
		? "true" : "false";
	result += R"(,"are_stats_saturated":)";
	result += operational.are_stats_saturated ? "true" : "false";
	{
		const auto& stats = operational.grouped_soa_provider;
		append("provider_configured", stats.provider_configured);
		append("provider_captured", stats.provider_captured);
		append("provider_persisted", stats.provider_persisted);
		append("provider_opened", stats.provider_opened);
		append("provider_head_scans", stats.provider_head_scans);
		append("provider_head_rows", stats.provider_head_rows);
		append("provider_descriptor_scans",
			stats.provider_descriptor_scans);
		append("provider_descriptor_rows", stats.provider_descriptor_rows);
		append("payload_tuples_formed", stats.payload_tuples_formed);
		append("payload_tuples_inserted", stats.payload_tuples_inserted);
		append("payload_tuples_reread", stats.payload_tuples_reread);
		append("mapped_sink_calls", stats.mapped_sink_calls);
		append("mapped_sink_bytes", stats.mapped_sink_bytes);
		append("mapped_sink_flushes", stats.mapped_sink_flushes);
		append("mapped_sink_coalescer_capacity_bytes",
			stats.mapped_sink_coalescer_capacity_bytes);
		append("mapped_sink_pwrite_calls", stats.mapped_sink_pwrite_calls);
		append("mapped_sink_short_writes", stats.mapped_sink_short_writes);
		append("mapped_sink_eintr_retries", stats.mapped_sink_eintr_retries);
		append("mapped_sink_preallocation_bytes",
			stats.mapped_sink_preallocation_bytes);
		append("mapped_sink_preallocation_result",
			stats.mapped_sink_preallocation_result);
		append("header_crc_bytes", stats.header_crc_bytes);
		append("directory_crc_bytes", stats.directory_crc_bytes);
		append("manifest_crc_bytes", stats.manifest_crc_bytes);
		append("seal_aggregate_payload_crc_bytes",
			stats.seal_aggregate_payload_crc_bytes);
		append("open_aggregate_payload_crc_bytes",
			stats.open_aggregate_payload_crc_bytes);
		append("lazy_extent_cas_winners", stats.lazy_extent_cas_winners);
		append("lazy_extent_waiters", stats.lazy_extent_waiters);
		append("lazy_extent_executions", stats.lazy_extent_executions);
		append("lazy_extent_reuses", stats.lazy_extent_reuses);
		append("lazy_extent_crc_bytes", stats.lazy_extent_crc_bytes);
		append("lazy_extent_state_bytes", stats.lazy_extent_state_bytes);
		append("lazy_extent_invalid_receipts",
			stats.lazy_extent_is_invalid_receipts);
		append("lazy_state_layout", stats.lazy_state_layout);
		append("mapping_bytes", stats.mapping_bytes);
		append("mapping_count", stats.mapping_count);
		append("mapping_resourceowner_releases",
			stats.mapping_resourceowner_releases);
		append("file_action_high_water", stats.file_action_high_water);
		append("file_action_identity_mismatches",
			stats.file_action_identity_mismatches);
		append("reaper_descriptors_examined",
			stats.reaper_descriptors_examined);
		append("reaper_relation_rows_deleted",
			stats.reaper_relation_rows_deleted);
		append("reaper_payload_bytes_deleted",
			stats.reaper_payload_bytes_deleted);
		append("reaper_files_queued", stats.reaper_files_queued);
		append("reaper_remaining", stats.reaper_remaining);
		append("orphan_entries_examined", stats.orphan_entries_examined);
		append("orphan_files_acted", stats.orphan_files_acted);
		append("orphan_bytes_acted", stats.orphan_bytes_acted);
		append("orphan_remaining", stats.orphan_remaining);
		append("orphan_cursor_wraps", stats.orphan_cursor_wraps);
		append("madvise_choice", stats.madvise_choice);
			append("madvise_result", stats.madvise_result);
		}
    {
        const auto& stats = operational.fragment_native_query_stats;
        append("fragment_native_peak_query_group_offset_capacity_bytes",
            stats.peak_query_group_offset_capacity_bytes);
        append("fragment_native_peak_query_membership_filter_capacity_bytes",
            stats.peak_query_membership_filter_capacity_bytes);
        append("fragment_native_query_group_offset_allocations",
            stats.query_group_offset_allocations);
        append("fragment_native_query_membership_filter_allocations",
            stats.query_membership_filter_allocations);
    }
	{
		const auto& stats = operational.fragment_native_query_stats;
		append("fragment_native_compact_direct_lookup_ns",
			stats.compact_direct_lookup_ns);
		append("fragment_native_compact_direct_selection_shards",
			stats.compact_direct_selection_shards);
		append("fragment_native_compact_direct_shard_query_layouts",
			stats.compact_direct_shard_query_layouts);
		append("fragment_native_compact_direct_layout_shards_completed",
			stats.compact_direct_layout_shards_completed);
		append("fragment_native_compact_direct_emission_shards_completed",
			stats.compact_direct_emission_shards_completed);
		append("fragment_native_compact_direct_posting_visit_passes",
			stats.compact_direct_posting_visit_passes);
		append("fragment_native_compact_direct_selected_postings",
			stats.compact_direct_selected_postings);
		append("fragment_native_compact_direct_layout_postings_visited",
			stats.compact_direct_layout_postings_visited);
		append("fragment_native_compact_direct_emission_postings_visited",
			stats.compact_direct_emission_postings_visited);
		append("fragment_native_compact_direct_target_control_scan_bytes",
			stats.compact_direct_target_control_scan_bytes);
		append("fragment_native_compact_direct_target_control_scan_passes",
			stats.compact_direct_target_control_scan_passes);
		append("fragment_native_compact_direct_fallbacks",
			stats.compact_direct_fallbacks);
		append(
			"fragment_native_compact_direct_traversal_rank_checkpoints_examined",
			stats.compact_direct_traversal_rank_checkpoints_examined);
		append(
			"fragment_native_compact_direct_traversal_membership_words_examined",
			stats.compact_direct_traversal_membership_words_examined);
		append("fragment_native_compact_direct_rejected_posting_reads",
			stats.compact_direct_rejected_posting_reads);
	}
	result += '}';
    return result;
}

/** Serializes hash-major persisted-index reader counters as JSON. */
[[nodiscard]] std::string hash_major_index_reader_stats_json(
		const alignment_exec::alignment_execution_snapshot& snapshot)
{
	const auto& operational = snapshot.pipeline.operational;
	const auto& stats = operational.hash_major_index_reader;
	std::string result =
		R"({"schema":"bioseq.hash_major_reader_stats","revision":1)";
	result.reserve(3'072);
	const auto append_signed = [&result](
			const std::string_view key,
			const std::int64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	const auto append_string = [&result](
			const std::string_view key,
			const std::string_view value) {
		result += ",\"";
		result.append(key);
		result += "\":\"";
		result.append(value);
		result += '"';
	};
	append_signed("execution_id", snapshot.execution_id);
	append_signed("backend_pid", snapshot.backend_pid);
	append("database_oid", snapshot.database_oid);
	append("format_revision", stats.layout_revision);
	append("sessions", stats.sessions);
	append("generation_id", stats.generation_id);
	append_string(
		"source_identity",
		fixed_hex_bytes(stats.source_identity));
	append_string(
		"source_content_crc64",
		fixed_hex_u64(stats.source_content_crc64));
	append_string(
		"compatibility_digest",
		fixed_hex_bytes(stats.storage_contract_digest));
	append("directory_rows", stats.directory_rows);
	append("directory_bytes", stats.directory_bytes);
	append("control_rows", stats.control_rows);
	append("control_bytes", stats.control_bytes);
	append("posting_rows", stats.posting_rows);
	append("posting_bytes", stats.posting_bytes);
	append(
		"directory_row_crc_receipts",
		stats.directory_row_crc_receipts);
	append(
		"directory_stream_crc_receipts",
		stats.directory_stream_crc_receipts);
	append(
		"control_row_crc_receipts",
		stats.control_row_crc_receipts);
	append(
		"control_block_crc_receipts",
		stats.control_block_crc_receipts);
	append(
		"control_semantic_receipts",
		stats.control_semantic_receipts);
	append(
		"posting_row_crc_receipts",
		stats.posting_row_crc_receipts);
	append(
		"posting_ranges_delivered",
		stats.posting_ranges_delivered);
	append(
		"posting_range_crc_receipts",
		stats.posting_range_crc_receipts);
	append(
		"posting_semantic_receipts",
		stats.posting_semantic_receipts);
	append(
		"authorized_posting_bytes",
		stats.authorized_posting_bytes);
	append("query_count", stats.query_count);
	append("selected_block_count", stats.selected_block_count);
	append("accepted_hash_count", stats.accepted_hash_count);
	append(
		"retained_position_count",
		stats.retained_position_count);
	append(
		"directory_stage_peak_bytes",
		stats.directory_peak_bytes);
	append(
		"control_stage_peak_bytes",
		stats.control_peak_bytes);
	append(
		"posting_stage_peak_bytes",
		stats.posting_peak_bytes);
	append("peak_admitted_bytes", stats.peak_admitted_bytes);
	append("retained_owner_bytes", stats.retained_owner_bytes);
	append("admitted_bytes", stats.admitted_bytes);
	append(
		"assembly_calls",
		operational.persisted_hash_build_calls);
	append(
		"pair_index_conversion_calls",
		operational.persisted_pair_index_conversion_calls);
	append(
		"segment_rebase_calls",
		operational.persisted_segment_rebase_calls);
	append(
		"recompute_calls",
		operational.recomputed_target_index_rows);
	append(
		"generic_source_fallback_calls",
		stats.generic_source_fallback_calls);
	append("backend_fallbacks", operational.backend_fallbacks);
	append("failures", stats.failures);
	result += R"(,"is_saturated":)";
	result += stats.is_saturated ? "true" : "false";
	result += R"(,"has_operational_identity_conflict":)";
	result += operational.has_operational_identity_conflict
		? "true" : "false";
	result += R"(,"are_stats_saturated":)";
	result += operational.are_stats_saturated ? "true" : "false";
	result += '}';
	return result;
}

/** Serializes persisted GPU operation and job counters as JSON. */
[[nodiscard]] std::string persisted_gpu_operation_stats_json(
		const alignment_exec::alignment_execution_snapshot& snapshot)
{
	const auto& ledger = snapshot.persisted_gpu_operations;
	const auto& stats = snapshot.pipeline;
	const bool requires_persisted_stats =
		stats.actual_backend
			== bioseq::pipeline_stats::executed_backend::vulkan_gpu
		&& stats.actual_target_index_source
			== bioseq::pipeline_stats::target_index_source::persisted;
	const bool is_ledger_complete = ledger.is_complete();
	const bool is_pipeline_match = requires_persisted_stats
		? ledger.is_pipeline_match(stats)
		: ledger.operation_count() == 0u && ledger.job_count() == 0u;
	const bool is_complete = is_ledger_complete && is_pipeline_match;
	const bool is_saturated = ledger.is_saturated()
		|| stats.operational.are_stats_saturated;
	const auto* first_chunk = ledger.head();
	const bioseq::pipeline_stats::persisted_gpu_cache_validation_result*
		cache_validation = first_chunk == nullptr
		? nullptr
		: &first_chunk->operation().cache_qualification;
	bool is_cache_validation_complete = cache_validation != nullptr
		&& cache_validation->is_internally_consistent();
	for (const auto* chunk = first_chunk; chunk != nullptr;
		 chunk = chunk->next()) {
		is_cache_validation_complete = is_cache_validation_complete
			&& chunk->operation().cache_qualification
				== *cache_validation;
	}
	const bool is_consistent = requires_persisted_stats && snapshot.is_completed
		&& !snapshot.is_partial && snapshot.is_fully_drained && is_complete
		&& !is_saturated && ledger.dropped_operation_count() == 0u
		&& is_cache_validation_complete;

	std::string result =
		R"({"schema":"bioseq.persisted_gpu_operation_stats","revision":2)";
	result.reserve(2048u + static_cast<std::size_t>(ledger.job_count()) * 1400u);
	const auto append_unsigned = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	const auto append_signed = [&result](
			const std::string_view key,
			const std::int64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	const auto append_bool = [&result](
			const std::string_view key,
			const bool value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += value ? "true" : "false";
	};
	const auto append_hex = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":\"";
		result += fixed_hex_u64(value);
		result += '"';
	};
	const auto append_text = [&result](
			const std::string_view key,
			const std::string_view value) {
		constexpr std::string_view hex = "0123456789abcdef";
		result += ",\"";
		result.append(key);
		result += "\":\"";
		for (const unsigned char byte : value) {
			if (byte == static_cast<unsigned char>('"')
				|| byte == static_cast<unsigned char>('\\')) {
				result += '\\';
				result += static_cast<char>(byte);
			} else if (byte < 0x20u || byte > 0x7eu) {
				result += "\\u00";
				result += hex[byte >> 4u];
				result += hex[byte & 0x0fu];
			} else {
				result += static_cast<char>(byte);
			}
		}
		result += '"';
	};
	append_signed("execution_id", snapshot.execution_id);
	append_signed("backend_pid", snapshot.backend_pid);
	append_unsigned("database_oid", snapshot.database_oid);
	append_bool("is_completed", snapshot.is_completed);
	append_bool("is_partial", snapshot.is_partial);
	append_bool("is_fully_drained", snapshot.is_fully_drained);
	append_bool("is_consistent", is_consistent);
	append_bool("is_complete", is_complete);
	append_bool("is_saturated", is_saturated);
	append_unsigned(
		"dropped_operation_count", ledger.dropped_operation_count());
	append_unsigned("operation_count", ledger.operation_count());
	append_unsigned(
		"accepted_operation_count",
		ledger.is_internally_consistent() ? ledger.operation_count() : 0u);
	append_unsigned(
		"complete_operation_count",
		is_ledger_complete ? ledger.operation_count() : 0u);
	append_unsigned("job_count", ledger.job_count());
	append_unsigned(
		"accepted_job_count",
		ledger.is_internally_consistent() ? ledger.job_count() : 0u);
	result += R"(,"cache_validation":)";
	if (cache_validation == nullptr) {
		result += "null";
	} else {
		const auto& validation = *cache_validation;
		result += R"({"revision":)";
		result += std::to_string(
			validation.qualification_wire_revision);
		append_hex("identity", validation.identity);
		append_bool(
			"is_consistent", validation.is_internally_consistent());
		append_bool("is_saturated", validation.is_saturated);
		append_unsigned("archive_count", validation.archive_count);
		append_unsigned("archive_bytes", validation.archive_bytes);
		append_hex("archive_checksum", validation.archive_checksum);
		append_unsigned("receipt_count", validation.receipt_count);
		append_unsigned("hash_passes", validation.hash_passes);
		append_unsigned("hashed_bytes", validation.hashed_bytes);
		append_unsigned(
			"private_files_created",
			validation.private_files_created);
		append_unsigned(
			"private_files_unlinked",
			validation.private_files_unlinked);
		append_unsigned(
			"live_private_paths", validation.live_private_paths);
		append_unsigned(
			"pipeline_filesystem_hash_passes",
			validation.pipeline_filesystem_hash_passes);
		append_unsigned(
			"pipeline_filesystem_hashed_bytes",
			validation.pipeline_filesystem_hashed_bytes);
		append_unsigned(
			"helper_protocol_revision",
			validation.helper_wire_revision);
		append_text(
			"helper_build_identity",
			validation.helper_build_identity_text());
		append_text(
			"helper_source", validation.helper_source_text());
		append_unsigned(
			"helper_executable_size",
			validation.helper_executable_size);
		result += R"(,"helper_executable_sha256":")";
		result += fixed_hex_bytes(
			validation.helper_executable_sha256);
		result += '"';
		append_hex(
			"helper_metal_registry_id",
			validation.helper_metal_registry_id);
		result += R"(,"device_uuid":")";
		result += fixed_hex_bytes(validation.device_uuid);
		result += '"';
		append_unsigned("api_version", validation.api_version);
		append_unsigned("driver_version", validation.driver_version);
		append_unsigned("vendor_id", validation.vendor_id);
		append_unsigned("device_id", validation.device_id);
		result += R"(,"pipeline_cache_uuid":")";
		result += fixed_hex_bytes(validation.pipeline_cache_uuid);
		result += '"';
		append_hex(
			"seed_receipt_identity",
			validation.seed_receipt_identity);
		result += R"(,"seed_sha256":")";
		result += fixed_hex_bytes(validation.seed_sha256);
		result += '"';
		append_unsigned("seed_size", validation.seed_size);
		append_hex(
			"manifest_checksum", validation.manifest_checksum);
		result += '}';
	}
	result += R"(,"operations":[)";
	bool first_operation = true;
	for (const auto* chunk = ledger.head(); chunk != nullptr;
		 chunk = chunk->next()) {
		if (!first_operation) result += ',';
		first_operation = false;
		const auto& operation = chunk->operation();
		const auto& publication = operation.publication;
		const auto& environment = operation.environment;
		const auto& cache = operation.cache_metrics;
		result += '{';
		result += R"("operation_identity":")";
		result += fixed_hex_u64(operation.operation_identity);
		result += '"';
		append_unsigned("expected_jobs", publication.expected_jobs);
		append_unsigned("validated_jobs", publication.validated_jobs);
		append_unsigned("consumed_jobs", publication.consumed_jobs);
		append_unsigned("accepted_jobs", publication.accepted_jobs);
		append_unsigned("burned_jobs", publication.burned_jobs);
		append_unsigned("pending_jobs", publication.pending_jobs);
		append_unsigned("staged_jobs", publication.staged_jobs);
		append_unsigned("published_jobs", operation.published_jobs);
		append_unsigned("publication_commits", operation.publication_commits);
		append_unsigned(
			"query_pipeline_receipt_count",
			operation.query_pipeline_receipt_count);
		append_unsigned(
			"shape_pipeline_receipt_count",
			operation.shape_pipeline_receipt_count);
		append_unsigned("submission_count", operation.submission_count);
		append_hex(
			"query_pipeline_operation_identity",
			operation.query_pipeline_operation_identity);
		append_hex(
			"shape_pipeline_operation_identity",
			operation.shape_pipeline_operation_identity);
		result += R"(,"query_submission_ordinals":[)";
		result += std::to_string(operation.query_submission_ordinals[0]);
		result += ',';
		result += std::to_string(operation.query_submission_ordinals[1]);
		result += ']';
		append_hex("seed_receipt_identity", operation.seed_receipt_identity);
		append_hex("cache_token_identity", cache.token_identity);
		append_hex(
			"cache_validation_identity",
			operation.cache_qualification.identity);
		append_unsigned("generation_id", operation.generation_id);
		append_hex("source_identity", operation.source_identity);
		append_hex("cold_load_identity", operation.cold_load_identity);
		result += R"(,"source":"persisted_grouped_soa")";
		result += R"(,"environment":{"device_uuid":")";
		result += fixed_hex_bytes(environment.device_uuid);
		result += '"';
		append_unsigned("api_version", environment.api_version);
		append_unsigned("driver_version", environment.driver_version);
		append_unsigned("vendor_id", environment.vendor_id);
		append_unsigned("device_id", environment.device_id);
		result += R"(,"pipeline_cache_uuid":")";
		result += fixed_hex_bytes(environment.pipeline_cache_uuid);
		result += R"(","seed_sha256":")";
		result += fixed_hex_bytes(environment.seed_sha256);
		result += '"';
		append_unsigned("seed_size", environment.seed_size);
		append_hex("manifest_checksum", environment.manifest_checksum);
		result += '}';
		result += R"(,"cache_metrics":{"pipeline_cache_loads":)";
		result += std::to_string(cache.pipeline_cache_loads);
		append_unsigned(
			"pipeline_cache_seed_bytes", cache.pipeline_cache_seed_bytes);
		append_unsigned("pipeline_cache_load_ns", cache.pipeline_cache_load_ns);
		append_unsigned("pipeline_creates", cache.pipeline_creates);
		append_unsigned("pipeline_creation_ns", cache.pipeline_creation_ns);
		result += '}';
		result += R"(,"jobs":[)";
		bool first_job = true;
		for (const auto& job : chunk->jobs()) {
			if (!first_job) result += ',';
			first_job = false;
			result += '{';
			result += R"("logical_target_row":)";
			result += std::to_string(job.logical_target_row);
			append_unsigned(
				"logical_query_ordinal", job.logical_query_ordinal);
			append_unsigned("query_index", job.query_index);
			append_unsigned("query_length", job.query_length);
			append_unsigned(
				"query_minimizer_count", job.query_minimizer_count);
			append_unsigned("query_tile_count", job.query_tile_count);
			result += R"(,"query":{"count_dispatches":)";
			result += std::to_string(job.query_count_dispatches);
			append_unsigned("scan_dispatches", job.query_scan_dispatches);
			append_unsigned("emit_dispatches", job.query_emit_dispatches);
			append_unsigned(
				"completed_roundtrips", job.query_completed_roundtrips);
			result += '}';
			result += R"(,"direct":{"count_dispatches":)";
			result += std::to_string(job.direct_count_dispatches);
			append_unsigned("scan_dispatches", job.direct_scan_dispatches);
			append_unsigned("anchor_count", job.anchor_count);
			append_unsigned("emit_dispatches", job.direct_emit_dispatches);
			append_unsigned("sort_operations", job.sort_operations);
			append_unsigned(
				"range_build_dispatches", job.range_build_dispatches);
			append_unsigned("range_count", job.range_count);
			append_unsigned("chain_dispatches", job.chain_dispatches);
			append_unsigned(
				"chain_backtrack_dispatches",
				job.chain_backtrack_dispatches);
			append_unsigned(
				"coordinate_dispatches", job.coordinate_dispatches);
			result += '}';
			result += R"(,"submissions":[)";
			constexpr std::array<std::string_view, 4> stages{
				"count", "emit", "sort_range", "chain_coordinate"};
			for (std::size_t i = 0u; i < job.submissions.size(); ++i) {
				if (i != 0u) result += ',';
				const auto& submission = job.submissions[i];
				result += R"({"stage":")";
				result.append(stages[i]);
				result += R"(","operation_identity":")";
				result += fixed_hex_u64(submission.operation_identity);
				result += R"(","ordinal":)";
				result += std::to_string(submission.ordinal);
				result += R"(,"submit_wait_identity":")";
				result += fixed_hex_u64(submission.submit_wait_identity);
				result += R"("})";
			}
			result += ']';
			result += R"(,"status":{"count_checks":)";
			result += std::to_string(job.count_status_checks);
			append_unsigned("emit_checks", job.emit_status_checks);
			append_unsigned("status_or", job.status_or);
			append_unsigned("result_readbacks", job.result_readbacks);
			result += '}';
			result += R"(,"forbidden":{"coordinate_vote_dispatches":)";
			result += std::to_string(job.coordinate_vote_dispatches);
			append_unsigned(
				"cpu_or_simd_biological_calls",
				job.cpu_or_simd_biological_calls);
			append_unsigned("recompute_calls", job.recompute_calls);
			append_unsigned("fallback_calls", job.fallback_calls);
			result += '}';
			const auto& coordinate = job.coordinate;
			result += R"(,"coordinate":{"valid":)";
			result += coordinate.is_valid ? "true" : "false";
			append_unsigned("query_start", coordinate.query_start);
			append_unsigned("query_end", coordinate.query_end);
			append_unsigned("target_start", coordinate.target_start);
			append_unsigned("target_end", coordinate.target_end);
			append_unsigned("match_length", coordinate.match_length);
			append_unsigned("block_length", coordinate.block_length);
			append_signed("chain_score", coordinate.chain_score);
			append_signed("seed_count", coordinate.seed_count);
			append_bool("is_reverse", coordinate.is_reverse);
			result += '}';
			result += '}';
		}
		result += ']';
		result += '}';
	}
	result += ']';
	result += '}';
	return result;
}

} // namespace

namespace bioseq::pg::sql_routines {

using bioseq::pg::detail::validate_alignment_snapshot_environment;

namespace {

/** Validates and serializes an optional alignment execution snapshot. */
template<class p_serializer>
[[nodiscard]] std::optional<bioseq::postgres::support::jsonb_value>
serialize_alignment_snapshot(
		const std::optional<alignment_exec::alignment_execution_snapshot>& source,
		p_serializer&& encode) {
	if (!source.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::alignment_execution_snapshot& snapshot = *source;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::postgres::support::jsonb_value{encode(snapshot)};
}

} // namespace

/** @return Statistics from the last minimizer fragment reader execution. */
std::optional<bioseq::postgres::support::jsonb_value>
last_alignment_minimizer_fragment_reader_stats() {
	return serialize_alignment_snapshot(
		bioseq::pg::detail::last_alignment_execution_snapshot_ref(),
		minimizer_fragment_reader_stats_json);
}

/** @return Statistics from the last hash-major index reader execution. */
std::optional<bioseq::postgres::support::jsonb_value>
last_alignment_hash_major_index_reader_stats() {
	return serialize_alignment_snapshot(
		bioseq::pg::detail::last_alignment_execution_snapshot_ref(),
		hash_major_index_reader_stats_json);
}

/** @return Persisted GPU operation statistics from the last completed alignment. */
std::optional<bioseq::postgres::support::jsonb_value>
last_alignment_persisted_gpu_operation_stats() {
	return serialize_alignment_snapshot(
		bioseq::pg::detail::last_completed_alignment_snapshot_ref(),
		[](const alignment_exec::alignment_execution_snapshot& snapshot) {
			return persisted_gpu_operation_stats_json(snapshot);
		});
}

} // namespace bioseq::pg::sql_routines
