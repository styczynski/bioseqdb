/**
 * @file mod_alignment_evidence_impl.cppm
 * @brief Alignment reader and persisted-GPU execution evidence diagnostics.
 */
module;

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

module bioseq.mod;

import bioseq.pipeline.stats;
import bioseq.pg_utils;
import bioseq.table.scan_state;

namespace {

namespace alignment_exec = bioseq::pg::alignment_execution;
using bioseq::pg::detail::fixed_hex_bytes;
using bioseq::pg::detail::fixed_hex_u64;

[[nodiscard]] std::string format5_reader_memory_evidence_json(
		const alignment_exec::AlignmentExecutionSnapshot& snapshot)
{
	/*
	 * Keep this post-statement diagnostic separate from both frozen alignment
	 * stats SRFs and the version-1 cold-index object.  One immutable backend-
	 * local snapshot supplies the identity, counters and saturation flags.
	 */
	const auto& operational = snapshot.pipeline.operational;
	std::string result =
		R"({"schema":"bioseq.format5_reader_memory_evidence","version":1)";
	result.reserve(768);
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
		"format5_reader_capacity_accounting_passes",
		operational.format5_reader_capacity_accounting_passes);
	append(
		"format5_reader_global_position_storage_preparations",
		operational.format5_reader_global_position_storage_preparations);
	append(
		"format5_reader_global_position_storage_allocations",
		operational.format5_reader_global_position_storage_allocations);
	append(
		"format5_reader_position_append_growth_reservations",
		operational.format5_reader_position_append_growth_reservations);
	append(
		"format5_reader_retained_position_count",
		operational.format5_reader_retained_position_count);
	append(
		"format5_reader_retained_position_capacity",
		operational.format5_reader_retained_position_capacity);
	result += R"(,"format5_reader_memory_evidence_saturated":)";
	result += operational.format5_reader_memory_evidence_saturated
		? "true" : "false";
	result += R"(,"stats_saturated":)";
	result += operational.stats_saturated ? "true" : "false";
	result += '}';
	return result;
}

enum class Format5ReaderMemoryEvidenceVersion : std::uint32_t {
    exact_array_layout = 2u,
    fragment_native_query = 3u,
    provider_lifecycle = 4u,
    bucket_direct_query = 5u,
    compact_direct_one_decode = 6u,
};

[[nodiscard]] std::string format5_reader_memory_evidence_json_versioned(
        const alignment_exec::AlignmentExecutionSnapshot& snapshot,
        const Format5ReaderMemoryEvidenceVersion version)
{
    bool include_fragment_native_query = false;
    bool include_provider_lifecycle = false;
    bool include_bucket_direct_query = false;
    bool include_compact_direct_one_decode = false;
    switch (version) {
        case Format5ReaderMemoryEvidenceVersion::exact_array_layout:
            break;
        case Format5ReaderMemoryEvidenceVersion::fragment_native_query:
            include_fragment_native_query = true;
            break;
        case Format5ReaderMemoryEvidenceVersion::provider_lifecycle:
            include_fragment_native_query = true;
            include_provider_lifecycle = true;
            break;
        case Format5ReaderMemoryEvidenceVersion::bucket_direct_query:
            include_fragment_native_query = true;
            include_provider_lifecycle = true;
            include_bucket_direct_query = true;
            break;
        case Format5ReaderMemoryEvidenceVersion::compact_direct_one_decode:
            include_fragment_native_query = true;
            include_provider_lifecycle = true;
            include_bucket_direct_query = true;
            include_compact_direct_one_decode = true;
            break;
    }
    const std::uint32_t version_number =
        static_cast<std::uint32_t>(version);
    const auto& operational = snapshot.pipeline.operational;
    std::string result =
        R"({"schema":"bioseq.format5_reader_memory_evidence","version":)"
        + std::to_string(version_number);
    result.reserve(include_provider_lifecycle
        ? 12'288u
        : include_fragment_native_query ? 6'144u : 2'048u);
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
		"format5_reader_capacity_accounting_passes",
		operational.format5_reader_capacity_accounting_passes);
	append(
		"format5_reader_global_position_storage_preparations",
		operational.format5_reader_global_position_storage_preparations);
	append(
		"format5_reader_global_position_storage_allocations",
		operational.format5_reader_global_position_storage_allocations);
	append(
		"format5_reader_position_append_growth_reservations",
		operational.format5_reader_position_append_growth_reservations);
	append(
		"format5_reader_retained_position_count",
		operational.format5_reader_retained_position_count);
	append(
		"format5_reader_retained_position_capacity",
		operational.format5_reader_retained_position_capacity);
	append(
		"format5_reader_directory_capacity_bytes",
		operational.format5_reader_directory_capacity_bytes);
	append(
		"format5_reader_query_hash_capacity_bytes",
		operational.format5_reader_query_hash_capacity_bytes);
	append(
		"format5_reader_query_record_capacity_bytes",
		operational.format5_reader_query_record_capacity_bytes);
	append(
		"format5_reader_ordinal_route_capacity_bytes",
		operational.format5_reader_ordinal_route_capacity_bytes);
	append(
		"format5_reader_selected_extent_capacity_bytes",
		operational.format5_reader_selected_extent_capacity_bytes);
	append(
		"format5_reader_current_selected_control_capacity_bytes",
		operational.format5_reader_current_selected_control_capacity_bytes);
	append(
		"format5_reader_global_position_capacity_bytes",
		operational.format5_reader_global_position_capacity_bytes);
	append(
		"format5_reader_control_payload_capacity_bytes",
		operational.format5_reader_control_payload_capacity_bytes);
	append(
		"format5_reader_owned_capacity_bytes",
		operational.format5_reader_owned_capacity_bytes);
	append(
		"format5_reader_admitted_bytes",
		operational.format5_reader_admitted_bytes);
	append(
		"format5_reader_exact_position_image_checks",
		operational.format5_reader_exact_position_image_checks);
	append(
		"format5_reader_exact_position_image_failures",
		operational.format5_reader_exact_position_image_failures);
	append(
		"format5_reader_retained_layout_checks",
		operational.format5_reader_retained_layout_checks);
    append(
        "format5_reader_retained_layout_failures",
        operational.format5_reader_retained_layout_failures);
    if (include_fragment_native_query) {
        const auto& evidence = operational.fragment_native_query_evidence;
        append("fragment_native_waves", evidence.waves);
        append("fragment_native_queries", evidence.queries);
        append("fragment_native_occurrence_fill_ns",
            evidence.occurrence_fill_ns);
        append("fragment_native_occurrence_radix_ns",
            evidence.occurrence_radix_ns);
        append("fragment_native_unique_plan_build_ns",
            evidence.unique_plan_build_ns);
        append("fragment_native_query_plan_radix_fill_ns",
            evidence.query_plan_radix_fill_ns);
        append("fragment_native_control_count_pass_ns",
            evidence.control_count_pass_ns);
        append("fragment_native_rejected_control_validation_ns",
            evidence.rejected_control_validation_ns);
        append("fragment_native_orientation_layout_ns",
            evidence.orientation_layout_ns);
        append("fragment_native_direct_emission_ns",
            evidence.direct_emission_ns);
        append("fragment_native_local_sort_ns", evidence.local_sort_ns);
        append("fragment_native_tie_normalization_ns",
            evidence.tie_normalization_ns);
        append("fragment_native_control_count_bytes",
            evidence.control_count_bytes);
        append("fragment_native_rejected_control_validation_bytes",
            evidence.rejected_control_validation_bytes);
        append("fragment_native_pass2_position_bytes",
            evidence.pass2_position_bytes);
        append("fragment_native_pass3_position_bytes",
            evidence.pass3_position_bytes);
        append("fragment_native_anchors_constructed",
            evidence.anchors_constructed);
        append("fragment_native_local_sort_bytes",
            evidence.local_sort_bytes);
        append("fragment_native_tie_normalization_bytes",
            evidence.tie_normalization_bytes);
        append("fragment_native_exact_final_bytes",
            evidence.exact_final_bytes);
        append("fragment_native_peak_exact_final_bytes",
            evidence.peak_exact_final_bytes);
        append("fragment_native_global_sort_calls_invoked",
            evidence.global_sort_calls_invoked);
        append("fragment_native_global_sort_calls_skipped",
            evidence.global_sort_calls_skipped);
        append("fragment_native_candidate_anchor_bytes",
            evidence.candidate_anchor_bytes);
        append("fragment_native_plan_ordinal_bytes",
            evidence.plan_ordinal_bytes);
        append("fragment_native_compaction_bytes",
            evidence.compaction_bytes);
        append("fragment_native_final_copy_bytes",
            evidence.final_copy_bytes);
        append("fragment_native_peak_occurrence_capacity_bytes",
            evidence.peak_occurrence_capacity_bytes);
        append("fragment_native_peak_query_plan_capacity_bytes",
            evidence.peak_query_plan_capacity_bytes);
        append("fragment_native_peak_query_radix_capacity_bytes",
            evidence.peak_query_radix_capacity_bytes);
        append("fragment_native_peak_occurrence_count_capacity_bytes",
            evidence.peak_occurrence_count_capacity_bytes);
        append("fragment_native_peak_block_layout_capacity_bytes",
            evidence.peak_block_layout_capacity_bytes);
        append("fragment_native_peak_block_evidence_capacity_bytes",
            evidence.peak_block_evidence_capacity_bytes);
        append("fragment_native_occurrence_allocations",
            evidence.occurrence_allocations);
        append("fragment_native_query_plan_allocations",
            evidence.query_plan_allocations);
        append("fragment_native_query_radix_allocations",
            evidence.query_radix_allocations);
        append("fragment_native_occurrence_count_allocations",
            evidence.occurrence_count_allocations);
        append("fragment_native_block_layout_allocations",
            evidence.block_layout_allocations);
        append("fragment_native_block_evidence_allocations",
            evidence.block_evidence_allocations);
        append("fragment_native_query_occurrences",
            evidence.query_occurrences);
        append("fragment_native_unique_query_plans",
            evidence.unique_query_plans);
        append("fragment_native_packed_occurrence_waves",
            evidence.packed_occurrence_waves);
        append("fragment_native_wide_occurrence_waves",
            evidence.wide_occurrence_waves);
        result += R"(,"fragment_native_evidence_saturated":)";
        result += evidence.saturated ? "true" : "false";
    }
	result += R"(,"format5_reader_memory_evidence_saturated":)";
	result += operational.format5_reader_memory_evidence_saturated
		? "true" : "false";
	result += R"(,"stats_saturated":)";
	result += operational.stats_saturated ? "true" : "false";
	if (include_provider_lifecycle) {
		const auto& evidence = operational.format5_provider_evidence;
		append("provider_configured", evidence.provider_configured);
		append("provider_captured", evidence.provider_captured);
		append("provider_persisted", evidence.provider_persisted);
		append("provider_opened", evidence.provider_opened);
		append("provider_head_scans", evidence.provider_head_scans);
		append("provider_head_rows", evidence.provider_head_rows);
		append("provider_descriptor_scans",
			evidence.provider_descriptor_scans);
		append("provider_descriptor_rows", evidence.provider_descriptor_rows);
		append("payload_tuples_formed", evidence.payload_tuples_formed);
		append("payload_tuples_inserted", evidence.payload_tuples_inserted);
		append("payload_tuples_reread", evidence.payload_tuples_reread);
		append("mapped_sink_calls", evidence.mapped_sink_calls);
		append("mapped_sink_bytes", evidence.mapped_sink_bytes);
		append("mapped_sink_flushes", evidence.mapped_sink_flushes);
		append("mapped_sink_coalescer_capacity_bytes",
			evidence.mapped_sink_coalescer_capacity_bytes);
		append("mapped_sink_pwrite_calls", evidence.mapped_sink_pwrite_calls);
		append("mapped_sink_short_writes", evidence.mapped_sink_short_writes);
		append("mapped_sink_eintr_retries", evidence.mapped_sink_eintr_retries);
		append("mapped_sink_preallocation_bytes",
			evidence.mapped_sink_preallocation_bytes);
		append("mapped_sink_preallocation_result",
			evidence.mapped_sink_preallocation_result);
		append("header_crc_bytes", evidence.header_crc_bytes);
		append("directory_crc_bytes", evidence.directory_crc_bytes);
		append("manifest_crc_bytes", evidence.manifest_crc_bytes);
		append("seal_aggregate_payload_crc_bytes",
			evidence.seal_aggregate_payload_crc_bytes);
		append("open_aggregate_payload_crc_bytes",
			evidence.open_aggregate_payload_crc_bytes);
		append("lazy_extent_cas_winners", evidence.lazy_extent_cas_winners);
		append("lazy_extent_waiters", evidence.lazy_extent_waiters);
		append("lazy_extent_executions", evidence.lazy_extent_executions);
		append("lazy_extent_reuses", evidence.lazy_extent_reuses);
		append("lazy_extent_crc_bytes", evidence.lazy_extent_crc_bytes);
		append("lazy_extent_state_bytes", evidence.lazy_extent_state_bytes);
		append("lazy_extent_invalid_receipts",
			evidence.lazy_extent_invalid_receipts);
		append("lazy_state_layout", evidence.lazy_state_layout);
		append("mapping_bytes", evidence.mapping_bytes);
		append("mapping_count", evidence.mapping_count);
		append("mapping_resourceowner_releases",
			evidence.mapping_resourceowner_releases);
		append("file_action_high_water", evidence.file_action_high_water);
		append("file_action_identity_mismatches",
			evidence.file_action_identity_mismatches);
		append("reaper_descriptors_examined",
			evidence.reaper_descriptors_examined);
		append("reaper_relation_rows_deleted",
			evidence.reaper_relation_rows_deleted);
		append("reaper_payload_bytes_deleted",
			evidence.reaper_payload_bytes_deleted);
		append("reaper_files_queued", evidence.reaper_files_queued);
		append("reaper_remaining", evidence.reaper_remaining);
		append("orphan_entries_examined", evidence.orphan_entries_examined);
		append("orphan_files_acted", evidence.orphan_files_acted);
		append("orphan_bytes_acted", evidence.orphan_bytes_acted);
		append("orphan_remaining", evidence.orphan_remaining);
		append("orphan_cursor_wraps", evidence.orphan_cursor_wraps);
		append("madvise_choice", evidence.madvise_choice);
			append("madvise_result", evidence.madvise_result);
		}
    if (include_bucket_direct_query) {
        const auto& evidence = operational.fragment_native_query_evidence;
        append("fragment_native_peak_query_group_offset_capacity_bytes",
            evidence.peak_query_group_offset_capacity_bytes);
        append("fragment_native_peak_query_membership_filter_capacity_bytes",
            evidence.peak_query_membership_filter_capacity_bytes);
        append("fragment_native_query_group_offset_allocations",
            evidence.query_group_offset_allocations);
        append("fragment_native_query_membership_filter_allocations",
            evidence.query_membership_filter_allocations);
    }
	if (include_compact_direct_one_decode) {
		const auto& evidence = operational.fragment_native_query_evidence;
		append("fragment_native_compact_direct_lookup_ns",
			evidence.compact_direct_lookup_ns);
		append("fragment_native_compact_direct_selection_shards",
			evidence.compact_direct_selection_shards);
		append("fragment_native_compact_direct_shard_query_layouts",
			evidence.compact_direct_shard_query_layouts);
		append("fragment_native_compact_direct_layout_shards_completed",
			evidence.compact_direct_layout_shards_completed);
		append("fragment_native_compact_direct_emission_shards_completed",
			evidence.compact_direct_emission_shards_completed);
		append("fragment_native_compact_direct_posting_visit_passes",
			evidence.compact_direct_posting_visit_passes);
		append("fragment_native_compact_direct_selected_postings",
			evidence.compact_direct_selected_postings);
		append("fragment_native_compact_direct_layout_postings_visited",
			evidence.compact_direct_layout_postings_visited);
		append("fragment_native_compact_direct_emission_postings_visited",
			evidence.compact_direct_emission_postings_visited);
		append("fragment_native_compact_direct_target_control_scan_bytes",
			evidence.compact_direct_target_control_scan_bytes);
		append("fragment_native_compact_direct_target_control_scan_passes",
			evidence.compact_direct_target_control_scan_passes);
		append("fragment_native_compact_direct_fallbacks",
			evidence.compact_direct_fallbacks);
		append(
			"fragment_native_compact_direct_traversal_rank_checkpoints_examined",
			evidence.compact_direct_traversal_rank_checkpoints_examined);
		append(
			"fragment_native_compact_direct_traversal_membership_words_examined",
			evidence.compact_direct_traversal_membership_words_examined);
		append("fragment_native_compact_direct_rejected_posting_reads",
			evidence.compact_direct_rejected_posting_reads);
	}
	result += '}';
    return result;
}

[[nodiscard]] std::string format5_reader_memory_evidence_json_v2(
        const alignment_exec::AlignmentExecutionSnapshot& snapshot) {
    return format5_reader_memory_evidence_json_versioned(
        snapshot, Format5ReaderMemoryEvidenceVersion::exact_array_layout);
}

[[nodiscard]] std::string format5_reader_memory_evidence_json_v3(
        const alignment_exec::AlignmentExecutionSnapshot& snapshot) {
    return format5_reader_memory_evidence_json_versioned(
        snapshot, Format5ReaderMemoryEvidenceVersion::fragment_native_query);
}

[[nodiscard]] std::string format5_reader_memory_evidence_json_v4(
        const alignment_exec::AlignmentExecutionSnapshot& snapshot) {
    return format5_reader_memory_evidence_json_versioned(
        snapshot, Format5ReaderMemoryEvidenceVersion::provider_lifecycle);
}

[[nodiscard]] std::string format5_reader_memory_evidence_json_v5(
        const alignment_exec::AlignmentExecutionSnapshot& snapshot) {
    return format5_reader_memory_evidence_json_versioned(
        snapshot, Format5ReaderMemoryEvidenceVersion::bucket_direct_query);
}

[[nodiscard]] std::string format5_reader_memory_evidence_json_v6(
        const alignment_exec::AlignmentExecutionSnapshot& snapshot) {
    return format5_reader_memory_evidence_json_versioned(
        snapshot,
        Format5ReaderMemoryEvidenceVersion::compact_direct_one_decode);
}

[[nodiscard]] std::string format6_reader_evidence_json(
		const alignment_exec::AlignmentExecutionSnapshot& snapshot)
{
	const auto& operational = snapshot.pipeline.operational;
	const auto& evidence = operational.format6_reader_evidence;
	std::string result =
		R"({"schema":"bioseq.format6_reader_evidence","version":1)";
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
	append("format_version", evidence.format_version);
	append("sessions", evidence.sessions);
	append("generation_id", evidence.generation_id);
	append_string(
		"source_identity",
		fixed_hex_bytes(evidence.source_identity));
	append_string(
		"source_content_crc64",
		fixed_hex_u64(evidence.source_content_crc64));
	append_string(
		"compatibility_digest",
		fixed_hex_bytes(evidence.compatibility_digest));
	append("directory_rows", evidence.directory_rows);
	append("directory_bytes", evidence.directory_bytes);
	append("control_rows", evidence.control_rows);
	append("control_bytes", evidence.control_bytes);
	append("posting_rows", evidence.posting_rows);
	append("posting_bytes", evidence.posting_bytes);
	append(
		"directory_row_crc_receipts",
		evidence.directory_row_crc_receipts);
	append(
		"directory_stream_crc_receipts",
		evidence.directory_stream_crc_receipts);
	append(
		"control_row_crc_receipts",
		evidence.control_row_crc_receipts);
	append(
		"control_block_crc_receipts",
		evidence.control_block_crc_receipts);
	append(
		"control_semantic_receipts",
		evidence.control_semantic_receipts);
	append(
		"posting_row_crc_receipts",
		evidence.posting_row_crc_receipts);
	append(
		"posting_ranges_delivered",
		evidence.posting_ranges_delivered);
	append(
		"posting_range_crc_receipts",
		evidence.posting_range_crc_receipts);
	append(
		"posting_semantic_receipts",
		evidence.posting_semantic_receipts);
	append(
		"authorized_posting_bytes",
		evidence.authorized_posting_bytes);
	append("query_count", evidence.query_count);
	append("selected_block_count", evidence.selected_block_count);
	append("accepted_hash_count", evidence.accepted_hash_count);
	append(
		"retained_position_count",
		evidence.retained_position_count);
	append(
		"directory_phase_peak_bytes",
		evidence.directory_phase_peak_bytes);
	append(
		"control_phase_peak_bytes",
		evidence.control_phase_peak_bytes);
	append(
		"posting_phase_peak_bytes",
		evidence.posting_phase_peak_bytes);
	append("peak_admitted_bytes", evidence.peak_admitted_bytes);
	append("retained_owner_bytes", evidence.retained_owner_bytes);
	append("admitted_bytes", evidence.admitted_bytes);
	append(
		"legacy_assembly_calls",
		operational.persisted_hash_build_calls);
	append(
		"legacy_globalize_calls",
		operational.persisted_legacy_globalize_calls);
	append(
		"legacy_rebase_calls",
		operational.persisted_legacy_rebase_calls);
	append(
		"recompute_calls",
		operational.recomputed_target_index_rows);
	append(
		"generic_source_fallback_calls",
		evidence.generic_source_fallback_calls);
	append("backend_fallbacks", operational.backend_fallbacks);
	append("failures", evidence.failures);
	result += R"(,"saturated":)";
	result += evidence.saturated ? "true" : "false";
	result += R"(,"operational_identity_conflict":)";
	result += operational.operational_identity_conflict
		? "true" : "false";
	result += R"(,"stats_saturated":)";
	result += operational.stats_saturated ? "true" : "false";
	result += '}';
	return result;
}

[[nodiscard]] std::string persisted_gpu_operation_evidence_json(
		const alignment_exec::AlignmentExecutionSnapshot& snapshot,
		const bool include_cache_qualification_v2)
{
	const auto& ledger = snapshot.persisted_gpu_operation_evidence;
	const auto& stats = snapshot.pipeline;
	const bool requires_evidence =
		stats.actual_backend
			== bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu
		&& stats.actual_target_index_source
			== bioseq::pipeline_stats::TargetIndexSource::persisted;
	const bool ledger_complete = ledger.complete();
	const bool pipeline_matches = requires_evidence
		? ledger.matches_pipeline(stats)
		: ledger.operation_count() == 0u && ledger.job_count() == 0u;
	const bool complete = ledger_complete && pipeline_matches;
	const bool saturated = ledger.saturated()
		|| stats.operational.stats_saturated;
	const auto* first_chunk = ledger.head();
	const bioseq::pipeline_stats::PersistedGpuCacheQualificationEvidence*
		cache_qualification = first_chunk == nullptr
		? nullptr
		: &first_chunk->operation().cache_qualification;
	bool cache_qualification_complete = cache_qualification != nullptr
		&& cache_qualification->accepted();
	for (const auto* chunk = first_chunk; chunk != nullptr;
		 chunk = chunk->next()) {
		cache_qualification_complete = cache_qualification_complete
			&& chunk->operation().cache_qualification
				== *cache_qualification;
	}
	const bool accepted = requires_evidence && snapshot.completed
		&& !snapshot.partial && snapshot.fully_drained && complete
		&& !saturated && ledger.dropped_operation_count() == 0u
		&& (!include_cache_qualification_v2
			|| cache_qualification_complete);

	std::string result =
		R"({"schema":"bioseq.persisted_gpu_operation_evidence","version":)";
	result += include_cache_qualification_v2 ? "2" : "1";
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
	append_bool("completed", snapshot.completed);
	append_bool("partial", snapshot.partial);
	append_bool("fully_drained", snapshot.fully_drained);
	append_bool("accepted", accepted);
	append_bool("complete", complete);
	append_bool("saturated", saturated);
	append_unsigned(
		"dropped_operation_count", ledger.dropped_operation_count());
	append_unsigned("operation_count", ledger.operation_count());
	append_unsigned(
		"accepted_operation_count",
		ledger.accepted() ? ledger.operation_count() : 0u);
	append_unsigned(
		"complete_operation_count",
		ledger_complete ? ledger.operation_count() : 0u);
	append_unsigned("job_count", ledger.job_count());
	append_unsigned(
		"accepted_job_count", ledger.accepted() ? ledger.job_count() : 0u);
	if (include_cache_qualification_v2) {
		result += R"(,"cache_qualification":)";
		if (cache_qualification == nullptr) {
			result += "null";
		} else {
			const auto& qualification = *cache_qualification;
			result += R"({"version":)";
			result += std::to_string(qualification.version);
			append_hex("identity", qualification.identity);
			append_bool("accepted", qualification.accepted());
			append_bool("saturated", qualification.saturated);
			append_unsigned("archive_count", qualification.archive_count);
			append_unsigned("archive_bytes", qualification.archive_bytes);
			append_hex("archive_checksum", qualification.archive_checksum);
			append_unsigned("receipt_count", qualification.receipt_count);
			append_unsigned("hash_passes", qualification.hash_passes);
			append_unsigned("hashed_bytes", qualification.hashed_bytes);
			append_unsigned(
				"private_files_created",
				qualification.private_files_created);
			append_unsigned(
				"private_files_unlinked",
				qualification.private_files_unlinked);
			append_unsigned(
				"live_private_paths", qualification.live_private_paths);
			append_unsigned(
				"pipeline_filesystem_hash_passes",
				qualification.pipeline_filesystem_hash_passes);
			append_unsigned(
				"pipeline_filesystem_hashed_bytes",
				qualification.pipeline_filesystem_hashed_bytes);
			append_unsigned(
				"helper_protocol_version",
				qualification.helper_protocol_version);
			append_text(
				"helper_build_identity",
				qualification.helper_build_identity_text());
			append_text(
				"helper_source", qualification.helper_source_text());
			append_unsigned(
				"helper_executable_size",
				qualification.helper_executable_size);
			result += R"(,"helper_executable_sha256":")";
			result += fixed_hex_bytes(
				qualification.helper_executable_sha256);
			result += '"';
			append_hex(
				"helper_metal_registry_id",
				qualification.helper_metal_registry_id);
			result += R"(,"device_uuid":")";
			result += fixed_hex_bytes(qualification.device_uuid);
			result += '"';
			append_unsigned("api_version", qualification.api_version);
			append_unsigned("driver_version", qualification.driver_version);
			append_unsigned("vendor_id", qualification.vendor_id);
			append_unsigned("device_id", qualification.device_id);
			result += R"(,"pipeline_cache_uuid":")";
			result += fixed_hex_bytes(qualification.pipeline_cache_uuid);
			result += '"';
			append_hex(
				"seed_receipt_identity",
				qualification.seed_receipt_identity);
			result += R"(,"seed_sha256":")";
			result += fixed_hex_bytes(qualification.seed_sha256);
			result += '"';
			append_unsigned("seed_size", qualification.seed_size);
			append_hex(
				"manifest_checksum", qualification.manifest_checksum);
			result += '}';
		}
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
		append_unsigned("proof_jobs", publication.proof_jobs);
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
		if (include_cache_qualification_v2) {
			append_hex(
				"cache_qualification_identity",
				operation.cache_qualification.identity);
		}
		append_unsigned("generation_id", operation.generation_id);
		append_hex("source_identity", operation.source_identity);
		append_hex("cold_load_identity", operation.cold_load_identity);
		result += R"(,"source":"persisted_shape_v5")";
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
			result += coordinate.valid ? "true" : "false";
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

std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence() {
	const auto& last_alignment_execution_snapshot =
		bioseq::pg::detail::last_alignment_execution_snapshot_ref();
	if (!last_alignment_execution_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		format5_reader_memory_evidence_json(snapshot)};
}


std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v2() {
	const auto& last_alignment_execution_snapshot =
		bioseq::pg::detail::last_alignment_execution_snapshot_ref();
	if (!last_alignment_execution_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		format5_reader_memory_evidence_json_v2(snapshot)};
}


std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v3() {
	const auto& last_alignment_execution_snapshot =
		bioseq::pg::detail::last_alignment_execution_snapshot_ref();
	if (!last_alignment_execution_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		format5_reader_memory_evidence_json_v3(snapshot)};
}


std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v4() {
	const auto& last_alignment_execution_snapshot =
		bioseq::pg::detail::last_alignment_execution_snapshot_ref();
	if (!last_alignment_execution_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		format5_reader_memory_evidence_json_v4(snapshot)};
}


std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v5() {
	const auto& last_alignment_execution_snapshot =
		bioseq::pg::detail::last_alignment_execution_snapshot_ref();
    if (!last_alignment_execution_snapshot.has_value()) {
        return std::nullopt;
    }
    const alignment_exec::AlignmentExecutionSnapshot& snapshot =
        *last_alignment_execution_snapshot;
    (void)validate_alignment_snapshot_environment(snapshot);
    return bioseq::pg_utils::sql_jsonb{
        format5_reader_memory_evidence_json_v5(snapshot)};
}


std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v6() {
	const auto& last_alignment_execution_snapshot =
		bioseq::pg::detail::last_alignment_execution_snapshot_ref();
    if (!last_alignment_execution_snapshot.has_value()) {
        return std::nullopt;
    }
    const alignment_exec::AlignmentExecutionSnapshot& snapshot =
        *last_alignment_execution_snapshot;
    (void)validate_alignment_snapshot_environment(snapshot);
    return bioseq::pg_utils::sql_jsonb{
        format5_reader_memory_evidence_json_v6(snapshot)};
}


std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format6_reader_evidence() {
	const auto& last_alignment_execution_snapshot =
		bioseq::pg::detail::last_alignment_execution_snapshot_ref();
	if (!last_alignment_execution_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		format6_reader_evidence_json(snapshot)};
}


std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_persisted_gpu_operation_evidence_v1() {
	const auto& last_completed_alignment_evidence_snapshot =
		bioseq::pg::detail::last_completed_alignment_evidence_snapshot_ref();
	if (!last_completed_alignment_evidence_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_completed_alignment_evidence_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		persisted_gpu_operation_evidence_json(snapshot, false)};
}


std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_persisted_gpu_operation_evidence_v2() {
	const auto& last_completed_alignment_evidence_snapshot =
		bioseq::pg::detail::last_completed_alignment_evidence_snapshot_ref();
	if (!last_completed_alignment_evidence_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_completed_alignment_evidence_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		persisted_gpu_operation_evidence_json(snapshot, true)};
}

} // namespace bioseq::pg::sql_routines
