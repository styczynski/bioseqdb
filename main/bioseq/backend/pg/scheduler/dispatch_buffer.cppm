/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_buffer.cppm
 *
 * TupleDispatchBuffer accumulates TupleTableSlot copies produced during
 * a custom-scan execution phase so the scan node can later replay them in
 * order. The scan runs in two phases per query:
 *
 *   Writing phase (driven by submit_scan): for every source tuple that
 *   passed the WHERE qual, the scan copies the slot into the buffer and
 *   records the row's primary key and a detoasted copy of its bioseq.seq
 *   varlena. Projection does NOT happen here; `ecxt_scantuple` still
 *   references the original scan slot which is about to be overwritten
 *   by the next call.
 *
 *   Batch phase (run_batch_and_build_cursors): after the access method
 *   signals EOF, the buffer feeds its target varlenas (plus the mmsearch
 *   query varlena captured at BeginCustomScan) into
 *   BatchProcessor::process_sequences. That function dispatches on the
 *   column's (alphabet_code, storage_unit_byte_width) typmod to instantiate the right
 *   compact_sequence_range<Dst> specialisation, then invokes
 *   minimap_port(query, target) for every row and stores the returned
 *   MatchResults keyed by primary key. Rows whose PK is missing from the
 *   returned map are filtered; this is how WHERE mmsearch(...) evaluates
 *   to FALSE for those rows.
 *
 *   Reading phase (read_next_output): the custom scan iterates over
 *   output cursors, pointing `ecxt_scantuple` at the buffered scan slot
 *   and projecting via ExecProject. The placeholder Const* nodes
 *   injected by VarInjectorMutator are updated from the MatchResult
 *   before projection fires.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <hwy/highway.h>  // only for HWY_NAMESPACE when naming active-target scheduler types
#include "common.h"
extern "C" {
#include "executor/instrument.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// DOC0263 §11.3.2 / DOC0264 — the chunked-scanner dispatch path is always
// compiled (Phase C made the fragmented pipeline tree a permanent fixture).
// The `bioseq.test_force_chunked_scanner` GUC routes row sequences through a
// synthetic chunked scanner end-to-end (DOC0263 parity tests). The
// `bioseq.test_force_inmemory_scanner` GUC forces the materialise-then-
// in_memory baseline. Without either GUC the production path promotes locator
// values through the statement-owned source provider and runs from its
// canonical lazy facade.
export module bioseq.scheduler.dispatch_buffer;

import bioseq.common.mlogging;
import bioseq.pg_utils;
import bioseq.config;                               // DOC0322 §9.3 — bioseq::settings::index_async_persist, bioseq::settings::recompute_async_persist GUCs
import bioseq.indexing.type;                 // DOC0279 — DIRECTORY_SUB_KEY
import bioseq.storage.indexing_pg.handles;      // DOC0279 §8.7/§12.3 — lightweight storage handles + active index
import bioseq.storage.indexing_pg.chunked_storage;
import bioseq.storage.lazy;                  // allocation observer for route-aware sequence payload tracing
import bioseq.scheduler.dispatch_processor;   // fixed-shape BatchProcessor, heavy pipeline in its own module
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.sequences.minimap_port;
import bioseq.sequences.cigar;
import bioseq.sequences.pipe;
import bioseq.types.pg_compact_sequence;
import bioseq.instr;
import bioseq.storage.manager;                  // DOC0260 §9
import bioseq.storage.scanner;          // DOC0263 §11.3.2 — chunked test scanner — seq_locator, fetch_payload_bulk, seq_caches
import bioseq.storage.scanner_pg;       // statement-owned source provider for locator targets
import bioseq.parallelism.memory_admission;
import bioseq.scheduler.supervisor_pg;
import bioseq.column.tam;               // DOC0322 §7.4 — tid_to_row_number (leaf; replaces the former extern "C++" reach into the table AM)

export namespace bioseq::scheduler {

constexpr size_t kDispatchBatchCapacity = 8192;

/// Merge one PostgreSQL-local cold-index delta through the existing
/// PipelineOperationalStats saturating path. This function performs only a
/// field mapping; it cannot change backend/source/fallback proof.
inline void merge_cold_index_preparation_stats(
        ::bioseq::pipeline_stats::PipelineOperationalStats& destination,
        const bioseq::storage::indexing::ColdIndexPreparationStatsPg& source)
        noexcept {
    ::bioseq::pipeline_stats::PipelineOperationalStats delta{};
    delta.cold_index_target_relation_reuses = source.target_relation_reuses;
    delta.cold_index_target_relation_extra_opens =
        source.target_relation_extra_opens;
    delta.cold_index_parent_random_reads = source.parent_random_reads;
    delta.cold_index_policy_heap_opens = source.policy_heap_opens;
    delta.cold_index_policy_index_opens = source.policy_index_opens;
    delta.cold_index_source_heap_opens = source.source_heap_opens;
    delta.cold_index_source_index_opens = source.source_index_opens;
    delta.cold_index_fragment_heap_opens = source.fragment_heap_opens;
    delta.cold_index_sidecar_heap_opens = source.sidecar_heap_opens;
    delta.cold_index_sidecar_index_opens = source.sidecar_index_opens;
    delta.cold_index_policy_schema_validations =
        source.policy_schema_validations;
    delta.cold_index_source_schema_validations =
        source.source_schema_validations;
    delta.cold_index_sidecar_schema_validations =
        source.sidecar_schema_validations;
    delta.cold_index_head_scans = source.head_scans;
    delta.cold_index_directory_scans = source.directory_scans;
    delta.cold_index_payload_range_scans = source.payload_range_scans;
    delta.cold_index_lock_acquires = source.lock_acquires;
    delta.cold_index_lock_releases = source.lock_releases;
    delta.cold_index_snapshot_registers = source.snapshot_registers;
    delta.cold_index_route_empty = source.route_empty;
    delta.cold_index_route_selective = source.route_selective;
    delta.cold_index_route_full = source.route_full;
    delta.cold_index_route_candidate_rows = source.route_candidate_rows;
    delta.cold_index_route_full_rows = source.route_full_rows;
    delta.cold_index_route_candidate_bytes = source.route_candidate_bytes;
    delta.cold_index_route_full_bytes = source.route_full_bytes;
    delta.cold_index_route_selective_cost = source.route_selective_cost;
    delta.cold_index_route_full_cost = source.route_full_cost;
    delta.cold_index_memory_reroutes = source.memory_reroutes;
    delta.cold_index_payload_bytes_requested =
        source.payload_bytes_requested;
    delta.cold_index_payload_bytes_delivered =
        source.payload_bytes_delivered;
    delta.cold_index_payload_rows_requested = source.payload_rows_requested;
    delta.cold_index_payload_rows_delivered = source.payload_rows_delivered;
    delta.cold_index_payload_rows_scanned = source.payload_rows_scanned;
    delta.cold_index_payload_bytes_scanned = source.payload_bytes_scanned;
    delta.cold_index_payload_rows_skipped = source.payload_rows_skipped;
    delta.cold_index_heap_tid_adjacent = source.heap_tid_adjacent;
    delta.cold_index_heap_tid_forward_gap = source.heap_tid_forward_gap;
    delta.cold_index_heap_tid_backward_jump = source.heap_tid_backward_jump;
    delta.cold_index_extent_crc_bytes = source.extent_crc_bytes;
    delta.cold_index_extent_crc_raw_bytes_hashed =
        source.extent_crc_raw_bytes_hashed;
    delta.cold_index_extent_crc_combined_bytes =
        source.extent_crc_combined_bytes;
    delta.cold_index_extent_crc_combine_rows = source.extent_crc_combine_rows;
    delta.cold_index_row_crc_bytes = source.row_crc_bytes;
    delta.cold_index_tuple_form_bytes = source.tuple_form_bytes;
    delta.cold_index_toast_values = source.toast_values;
    delta.cold_index_detoast_bytes = source.detoast_bytes;
    delta.cold_index_policy_ns = source.policy_ns;
    delta.cold_index_source_ns = source.source_ns;
    delta.cold_index_bind_ns = source.bind_ns;
    delta.cold_index_head_ns = source.head_ns;
    delta.cold_index_directory_ns = source.directory_ns;
    delta.cold_index_payload_ns = source.payload_ns;
    delta.cold_index_decode_ns = source.decode_ns;
    delta.cold_index_transfer_ns = source.transfer_ns;
    delta.cold_index_close_ns = source.close_ns;
    delta.cold_index_shared_blks_hit = source.shared_blks_hit;
    delta.cold_index_shared_blks_read = source.shared_blks_read;
    delta.stats_saturated = source.stats_saturated;
    destination.merge(delta);
}

inline void map_format6_read_evidence(
		::bioseq::pipeline_stats::PipelineOperationalStats& destination,
		const bioseq::storage::indexing::MinimizerGenerationReadStats& source)
		noexcept {
	const auto& input = source.format6;
	auto& output = destination.format6_reader_evidence;
	output.format_version = input.format_version;
	output.sessions = input.sessions;
	output.identity_present = input.sessions != 0u;
	output.generation_id = input.generation_id;
	output.source_identity = input.source_identity;
	output.source_content_crc64 = input.source_content_crc64;
	output.compatibility_digest = input.compatibility_digest;
	output.directory_rows = input.directory_rows;
	output.directory_bytes = input.directory_bytes;
	output.control_rows = input.control_rows;
	output.control_bytes = input.control_bytes;
	output.posting_rows = input.posting_rows;
	output.posting_bytes = input.posting_bytes;
	output.directory_row_crc_receipts =
		input.directory_row_crc_receipts;
	output.directory_stream_crc_receipts =
		input.directory_stream_crc_receipts;
	output.control_row_crc_receipts =
		input.control_row_crc_receipts;
	output.control_block_crc_receipts =
		input.control_block_crc_receipts;
	output.control_semantic_receipts =
		input.control_semantic_receipts;
	output.posting_row_crc_receipts =
		input.posting_row_crc_receipts;
	output.posting_ranges_delivered =
		input.posting_ranges_delivered;
	output.posting_range_crc_receipts =
		input.posting_range_crc_receipts;
	output.posting_semantic_receipts =
		input.posting_semantic_receipts;
	output.authorized_posting_bytes =
		input.authorized_posting_bytes;
	output.query_count = input.query_count;
	output.selected_block_count = input.selected_block_count;
	output.accepted_hash_count = input.accepted_hash_count;
	output.retained_position_count = input.retained_position_count;
	output.directory_phase_peak_bytes =
		input.directory_phase_peak_bytes;
	output.control_phase_peak_bytes =
		input.control_phase_peak_bytes;
	output.posting_phase_peak_bytes =
		input.posting_phase_peak_bytes;
	output.peak_admitted_bytes = input.peak_admitted_bytes;
	output.owner_bytes_present = input.sessions != 0u;
	output.retained_owner_bytes = input.retained_owner_bytes;
	output.admitted_bytes = input.admitted_bytes;
	output.generic_source_fallback_calls =
		input.generic_source_fallback_calls;
	output.failures = input.failures;
	output.saturated = input.saturated;
}

/// Publish one authenticated generation-session read exactly once.  GPU and
/// CPU prepared targets use the same PostgreSQL reader, so their physical I/O
/// proof must be mapped identically instead of being inferred from backend
/// selection labels.
inline void merge_generation_read_stats(
		::bioseq::pipeline_stats::PipelineOperationalStats& destination,
		const bioseq::storage::indexing::MinimizerGenerationReadStats& source)
		noexcept {
	::bioseq::pipeline_stats::PipelineOperationalStats delta{};
	delta.persisted_generation_read_sessions = 1;
	delta.persisted_relation_opens = source.relation_opens;
	delta.persisted_index_scans = source.index_scans;
	delta.persisted_head_scans = source.head_scans;
	delta.persisted_head_rows_read = source.head_rows_read;
	delta.persisted_head_bytes_read = source.head_bytes_read;
	delta.persisted_manifest_directory_scans =
		source.manifest_directory_scans;
	delta.persisted_manifest_rows_read = source.manifest_rows_read;
	delta.persisted_manifest_bytes_read = source.manifest_bytes_read;
	delta.persisted_directory_chunks_read = source.directory_chunks_read;
	delta.persisted_directory_bytes_read = source.directory_bytes_read;
	delta.persisted_metadata_repeated_reads = source.metadata_repeated_reads;
	delta.persisted_metadata_heap_rows_read = source.metadata_heap_rows_read;
	delta.persisted_metadata_bytes_read = source.metadata_bytes_read;
	delta.persisted_payload_heap_rows_read = source.payload_heap_rows_read;
	delta.persisted_payload_bytes_read = source.payload_bytes_read;
	delta.persisted_heap_rows_read = source.total_heap_rows_read;
	delta.persisted_toast_values_read = source.toast_values_read;
	delta.persisted_detoast_bytes = source.detoast_bytes;
	delta.persisted_explicit_copy_bytes = source.explicit_copy_bytes;
	delta.persisted_wanted_subkeys = source.payload.wanted_subkeys;
	delta.persisted_read_subkeys = source.payload.read_subkeys;
	delta.persisted_wanted_extents = source.payload.wanted_extents;
	delta.persisted_wanted_chunks = source.payload.wanted_chunks;
	delta.persisted_skipped_rows = source.payload.skipped_rows;
	delta.persisted_repeated_chunk_reads = source.payload.repeated_chunk_reads;
	delta.persisted_extents_read = source.payload.read_extents;
	delta.persisted_chunks_read = source.payload.delivered_chunks;
	delta.persisted_bytes_read = source.payload.delivered_bytes;
	delta.persisted_bulk_read_calls = source.bulk_read_calls;
	delta.persisted_planned_request_groups = source.planned_request_groups;
	delta.persisted_executed_request_groups = source.executed_request_groups;
	delta.persisted_range_scan_calls = source.range_scan_calls;
	delta.persisted_skip_budget_rows = source.payload.skip_budget_rows;
	delta.persisted_source_parent_rows_read = source.source_parent_rows_read;
	delta.persisted_source_catalog_rows_read = source.source_catalog_rows_read;
	delta.persisted_source_catalog_index_scans =
		source.source_catalog_index_scans;
	delta.persisted_source_fragment_rows_read =
		source.source_fragment_rows_read;
	delta.persisted_source_decoded_bp = source.source_decoded_bp;
	map_format6_read_evidence(delta, source);
	destination.merge(delta);
	merge_cold_index_preparation_stats(destination, source.cold);
}

inline void install_active_read_policy_dispatch_proof(
		::bioseq::simd::BatchProcessor::ScannerPgContext& context,
		bioseq::storage::indexing::ActiveReadPolicyResolution resolution,
		const Oid user_relid,
		const Oid relfilenode,
		const std::int16_t attno) {
	std::uint64_t* counter = nullptr;
	switch (resolution.source) {
		case bioseq::storage::indexing::
				ActiveReadPolicyResolutionSource::configured_row:
			counter = &bioseq::storage::indexing::
				g_active_read_policy_dispatch_proof_counters
				.configured_batches;
			break;
		case bioseq::storage::indexing::
				ActiveReadPolicyResolutionSource::authoritative_no_row_default:
			counter = &bioseq::storage::indexing::
				g_active_read_policy_dispatch_proof_counters
				.authoritative_default_batches;
			break;
		case bioseq::storage::indexing::
				ActiveReadPolicyResolutionSource::non_relation:
			counter = &bioseq::storage::indexing::
				g_active_read_policy_dispatch_proof_counters
				.non_relation_batches;
			break;
		case bioseq::storage::indexing::
				ActiveReadPolicyResolutionSource::unresolved:
			break;
	}
	if (counter == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("active-read policy resolver returned no source proof")));
		pg_unreachable();
	}
	++*counter;
	context.active_index = std::move(resolution.index_name);
	context.active_index_policy_source = resolution.source;
	bioseq::storage::indexing::g_last_active_read_policy_dispatch_source =
		resolution.source;
	bioseq_log<DBG>(
		"[bioseq] dispatch active-read policy proof source={} relation={} relfilenode={} attno={} index={} source_dispatch_count={}",
		bioseq::storage::indexing::active_read_policy_resolution_source_name(
			resolution.source),
		static_cast<unsigned>(user_relid),
		static_cast<unsigned>(relfilenode), static_cast<int>(attno),
		context.active_index, static_cast<unsigned long long>(*counter));
}

[[nodiscard]] constexpr std::uint64_t extra_broadcast_query_ordinal(
        const bool per_row_query_mode,
        const std::size_t batch_size,
        const std::size_t extra_query_index) noexcept {
    return per_row_query_mode
        ? static_cast<std::uint64_t>(batch_size)
            + static_cast<std::uint64_t>(extra_query_index)
        : static_cast<std::uint64_t>(extra_query_index) + 1;
}
static_assert(extra_broadcast_query_ordinal(false, 2, 0) == 1);
static_assert(extra_broadcast_query_ordinal(true, 2, 0) == 2);
static_assert(
    std::uint64_t{1} != extra_broadcast_query_ordinal(true, 2, 0),
    "per-row primary row 1 and the first extra broadcast query must differ");

[[nodiscard]] constexpr bool dispatch_uses_per_row_query(
        const bool active,
        const AttrNumber raw_query_attno,
        const bool evaluated_query) noexcept {
    return active
        && (raw_query_attno != InvalidAttrNumber || evaluated_query);
}

static_assert(!dispatch_uses_per_row_query(
    false, InvalidAttrNumber, true));
static_assert(dispatch_uses_per_row_query(
    true, AttrNumber{1}, false));
static_assert(dispatch_uses_per_row_query(
    true, InvalidAttrNumber, true));

[[nodiscard]] constexpr bool evaluated_mmsearch_index_policy_valid(
        const bioseq::indexing::IndexReadRequirement requirement,
        const bool read_sequence_index) noexcept {
    return requirement
            == bioseq::indexing::IndexReadRequirement::require_absent
        && !read_sequence_index;
}

static_assert(evaluated_mmsearch_index_policy_valid(
    bioseq::indexing::IndexReadRequirement::require_absent, false));
static_assert(!evaluated_mmsearch_index_policy_valid(
    bioseq::indexing::IndexReadRequirement::allow, false));

enum class CursorBoundary : std::uint8_t {
    value,
    exact_eof,
    corrupt,
};

[[nodiscard]] constexpr CursorBoundary classify_output_cursor(
        const std::int64_t position,
        const std::size_t size) noexcept {
    if (position < 0) return CursorBoundary::corrupt;
    const auto unsigned_position = static_cast<std::uint64_t>(position);
    if (unsigned_position == size) return CursorBoundary::exact_eof;
    return unsigned_position < size
        ? CursorBoundary::value
        : CursorBoundary::corrupt;
}

[[nodiscard]] constexpr CursorBoundary classify_output_cursor(
        const std::size_t position,
        const std::size_t size) noexcept {
    if (position == size) return CursorBoundary::exact_eof;
    return position < size
        ? CursorBoundary::value
        : CursorBoundary::corrupt;
}

static_assert(classify_output_cursor(std::int64_t{0}, 1)
    == CursorBoundary::value);
static_assert(classify_output_cursor(std::int64_t{1}, 1)
    == CursorBoundary::exact_eof);
static_assert(classify_output_cursor(std::int64_t{-1}, 1)
    == CursorBoundary::corrupt);
static_assert(classify_output_cursor(std::int64_t{2}, 1)
    == CursorBoundary::corrupt);
static_assert(classify_output_cursor(std::size_t{0}, 0)
    == CursorBoundary::exact_eof);
static_assert(classify_output_cursor(std::size_t{2}, 1)
    == CursorBoundary::corrupt);

enum class SharedJoinTargetOutcomeTopology : std::uint8_t {
	invalid,
	cpu_per_pair,
	gpu_shared_target,
};

/* Requested-backend identity is an attestation result, not a dispatch-shape
 * discriminator. A deliberately unattested mixed CPU tuple still emits one
 * raw target outcome per target-query pair, whereas the complete Vulkan tuple
 * emits one already-shared target outcome. Derive that topology from the
 * immutable selected params and keep the backend identity only as a coherence
 * check. */
[[nodiscard]] constexpr SharedJoinTargetOutcomeTopology
classify_shared_join_target_outcome_topology(
		const bool any_vulkan_component_requested,
		const bool complete_vulkan_tuple_requested) noexcept {
	if (complete_vulkan_tuple_requested) {
		return any_vulkan_component_requested
			? SharedJoinTargetOutcomeTopology::gpu_shared_target
			: SharedJoinTargetOutcomeTopology::invalid;
	}
	return any_vulkan_component_requested
		? SharedJoinTargetOutcomeTopology::invalid
		: SharedJoinTargetOutcomeTopology::cpu_per_pair;
}
static_assert(classify_shared_join_target_outcome_topology(false, false)
	== SharedJoinTargetOutcomeTopology::cpu_per_pair);
static_assert(classify_shared_join_target_outcome_topology(true, true)
	== SharedJoinTargetOutcomeTopology::gpu_shared_target);
static_assert(classify_shared_join_target_outcome_topology(true, false)
	== SharedJoinTargetOutcomeTopology::invalid);
static_assert(classify_shared_join_target_outcome_topology(false, true)
	== SharedJoinTargetOutcomeTopology::invalid);

[[nodiscard]] constexpr bool shared_join_raw_target_shape_valid(
		const SharedJoinTargetOutcomeTopology topology,
		const ::bioseq::pipeline_stats::ExecutedBackend backend,
		const std::uint64_t query_count,
		const std::uint64_t attempted_jobs,
		const std::uint64_t raw_target_rows) noexcept {
	if (query_count == 0 || attempted_jobs != query_count) {
		return false;
	}
	std::uint64_t expected = 0;
	switch (topology) {
		case SharedJoinTargetOutcomeTopology::cpu_per_pair:
			if (backend == ::bioseq::pipeline_stats::ExecutedBackend::none
				|| backend
					== ::bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu) {
				return false;
			}
			/* mixed_invalid is the correct fail-closed identity for a
			 * heterogeneous CPU tuple such as scalar sketch/chaining plus
			 * Block Aligner. It remains unattested while retaining CPU
			 * per-pair target accounting. */
			expected = query_count;
			break;
		case SharedJoinTargetOutcomeTopology::gpu_shared_target:
			if (backend
					!= ::bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu) {
				return false;
			}
			expected = 1;
			break;
		case SharedJoinTargetOutcomeTopology::invalid:
			return false;
	}
	return raw_target_rows == expected;
}
static_assert(shared_join_raw_target_shape_valid(
	SharedJoinTargetOutcomeTopology::cpu_per_pair,
	::bioseq::pipeline_stats::ExecutedBackend::simd_cpu,
	16, 16, 16));
static_assert(shared_join_raw_target_shape_valid(
	SharedJoinTargetOutcomeTopology::cpu_per_pair,
	::bioseq::pipeline_stats::ExecutedBackend::mixed_invalid,
	16, 16, 16));
static_assert(shared_join_raw_target_shape_valid(
	SharedJoinTargetOutcomeTopology::gpu_shared_target,
	::bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu,
	16, 16, 1));
/* Later waves must still carry native raw target/source evidence.  Only the
 * BatchJoin owner suppresses their normalized publication. */
static_assert(shared_join_raw_target_shape_valid(
	SharedJoinTargetOutcomeTopology::cpu_per_pair,
	::bioseq::pipeline_stats::ExecutedBackend::simd_cpu,
	8192, 8192, 8192));
static_assert(!shared_join_raw_target_shape_valid(
	SharedJoinTargetOutcomeTopology::cpu_per_pair,
	::bioseq::pipeline_stats::ExecutedBackend::simd_cpu,
	1, 1, 0));
static_assert(!shared_join_raw_target_shape_valid(
	SharedJoinTargetOutcomeTopology::gpu_shared_target,
	::bioseq::pipeline_stats::ExecutedBackend::mixed_invalid,
	16, 16, 1));
static_assert(!shared_join_raw_target_shape_valid(
	SharedJoinTargetOutcomeTopology::cpu_per_pair,
	::bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu,
	16, 16, 16));
static_assert(!shared_join_raw_target_shape_valid(
	SharedJoinTargetOutcomeTopology::cpu_per_pair,
	::bioseq::pipeline_stats::ExecutedBackend::simd_cpu,
	16, 15, 16));
static_assert(!shared_join_raw_target_shape_valid(
	SharedJoinTargetOutcomeTopology::cpu_per_pair,
	::bioseq::pipeline_stats::ExecutedBackend::simd_cpu,
	16, 16, 15));

[[nodiscard]] constexpr bool shared_join_raw_source_shape_valid(
		const ::bioseq::pipeline_stats::TargetIndexSource requested,
		const ::bioseq::pipeline_stats::TargetIndexSource actual,
		const std::uint64_t raw_target_rows,
		const std::uint64_t persisted_rows,
		const std::uint64_t recomputed_rows,
		const std::uint64_t missing_rows) noexcept {
	if (raw_target_rows == 0
		|| requested == ::bioseq::pipeline_stats::TargetIndexSource::none
		|| requested
			== ::bioseq::pipeline_stats::TargetIndexSource::mixed_invalid
		|| actual == ::bioseq::pipeline_stats::TargetIndexSource::none
		|| actual
			== ::bioseq::pipeline_stats::TargetIndexSource::mixed_invalid) {
		return false;
	}
	if (actual == ::bioseq::pipeline_stats::TargetIndexSource::persisted) {
		/* A recompute-only request must never discover/use persisted payload. */
		return requested
				== ::bioseq::pipeline_stats::TargetIndexSource::persisted
			&& persisted_rows == raw_target_rows
			&& recomputed_rows == 0
			&& missing_rows == 0;
	}
	/* The only remaining concrete actual source is recomputed. */
	if (persisted_rows != 0 || recomputed_rows != raw_target_rows) {
		return false;
	}
	if (requested
		== ::bioseq::pipeline_stats::TargetIndexSource::recomputed) {
		return missing_rows == 0;
	}
	/* An allow-mode persisted request may truthfully miss and recompute, but
	 * the producer must report one missing outcome for every raw target. */
	return requested
			== ::bioseq::pipeline_stats::TargetIndexSource::persisted
		&& missing_rows == raw_target_rows;
}
static_assert(shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::recomputed,
	::bioseq::pipeline_stats::TargetIndexSource::recomputed,
	16, 0, 16, 0));
static_assert(shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	16, 16, 0, 0));
static_assert(shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	::bioseq::pipeline_stats::TargetIndexSource::recomputed,
	16, 0, 16, 16));
static_assert(!shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	::bioseq::pipeline_stats::TargetIndexSource::recomputed,
	16, 0, 16, 0));
static_assert(!shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::recomputed,
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	16, 16, 0, 0));
static_assert(!shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::mixed_invalid,
	::bioseq::pipeline_stats::TargetIndexSource::recomputed,
	16, 0, 16, 0));
static_assert(!shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	16, 15, 1, 0));

/*
 * The typed processor is a generic row-pair consumer: some target/index
 * outcome paths still report one operational observation per pair.  A
 * BioseqBatchJoin wave, by contrast, owns exactly one physical target and
 * repeats only its query side.  Collapse that generic evidence at the owner
 * boundary after first proving that it is internally consistent.  This is
 * deliberately not a permissive clamp: mixed target sources, missing source
 * outcomes, or counts outside the physical wave shape fail closed.
 */
inline void normalize_shared_join_target_outcomes(
		::bioseq::pipeline_stats::PipelineExecutionStats& stats,
        const bool record_physical_target,
		const std::size_t query_count,
		const SharedJoinTargetOutcomeTopology topology) {
	auto& operational = stats.operational;
    const std::uint64_t target_rows = operational.target_rows;
    const std::uint64_t persisted_rows = operational.persisted_index_rows;
    const std::uint64_t recomputed_rows =
        operational.recomputed_target_index_rows;
    const std::uint64_t missing_rows =
        operational.missing_persisted_index_rows;
    const std::uint64_t query_count_u64 =
        static_cast<std::uint64_t>(query_count);
	std::uint64_t expected_raw_target_rows = 0;
	switch (topology) {
		case SharedJoinTargetOutcomeTopology::cpu_per_pair:
			expected_raw_target_rows = query_count_u64;
			break;
		case SharedJoinTargetOutcomeTopology::gpu_shared_target:
			expected_raw_target_rows = 1;
			break;
		case SharedJoinTargetOutcomeTopology::invalid:
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqBatchJoin target outcomes have an invalid dispatch topology"),
				 errdetail("requested_backend=%.*s query_count=%zu",
					static_cast<int>(
						::bioseq::pipeline_stats::executed_backend_name(
							stats.requested_backend).size()),
					::bioseq::pipeline_stats::executed_backend_name(
						stats.requested_backend).data(),
					query_count)));
	}
    const bool source_mixed = persisted_rows != 0 && recomputed_rows != 0;
    const bool source_count_invalid =
        persisted_rows > query_count_u64
        || recomputed_rows > query_count_u64
        || persisted_rows + recomputed_rows != target_rows;
	const bool raw_target_shape_invalid =
		!shared_join_raw_target_shape_valid(
			topology,
			stats.requested_backend,
			query_count_u64,
			stats.attempted_alignment_jobs,
			target_rows);
	const bool raw_source_shape_invalid =
		!shared_join_raw_source_shape_valid(
			stats.requested_target_index_source,
			stats.actual_target_index_source,
			target_rows,
			persisted_rows,
			recomputed_rows,
			missing_rows);
    const bool shape_invalid = query_count == 0
		|| raw_target_shape_invalid
		|| raw_source_shape_invalid
        || missing_rows > recomputed_rows
		|| (record_physical_target && target_rows == 0);
    if (source_mixed || source_count_invalid || shape_invalid) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqBatchJoin target outcome accounting is inconsistent"),
             errdetail(
				"record_physical_target=%s query_count=%zu attempted_jobs=%llu "
				"requested_backend=%.*s dispatch_topology=%s "
				"expected_raw_target_rows=%llu "
				"requested_source=%.*s actual_source=%.*s "
				"target_rows=%llu persisted_rows=%llu recomputed_rows=%llu "
				"missing_rows=%llu",
                record_physical_target ? "true" : "false",
                query_count,
				static_cast<unsigned long long>(
					stats.attempted_alignment_jobs),
				static_cast<int>(
					::bioseq::pipeline_stats::executed_backend_name(
						stats.requested_backend).size()),
				::bioseq::pipeline_stats::executed_backend_name(
					stats.requested_backend).data(),
				topology
						== SharedJoinTargetOutcomeTopology::cpu_per_pair
					? "cpu_per_pair"
					: "gpu_shared_target",
				static_cast<unsigned long long>(expected_raw_target_rows),
				static_cast<int>(
					::bioseq::pipeline_stats::target_index_source_name(
						stats.requested_target_index_source).size()),
				::bioseq::pipeline_stats::target_index_source_name(
					stats.requested_target_index_source).data(),
				static_cast<int>(
					::bioseq::pipeline_stats::target_index_source_name(
						stats.actual_target_index_source).size()),
				::bioseq::pipeline_stats::target_index_source_name(
					stats.actual_target_index_source).data(),
                static_cast<unsigned long long>(target_rows),
                static_cast<unsigned long long>(persisted_rows),
                static_cast<unsigned long long>(recomputed_rows),
                static_cast<unsigned long long>(missing_rows))));
    }

    operational.target_rows = record_physical_target ? 1 : 0;
    operational.persisted_index_rows =
        record_physical_target && persisted_rows != 0 ? 1 : 0;
    operational.recomputed_target_index_rows =
        record_physical_target && recomputed_rows != 0 ? 1 : 0;
    operational.missing_persisted_index_rows =
        record_physical_target && missing_rows != 0 ? 1 : 0;
}

// Fixed upper bound on one physical dispatch. The custom scan rolls over at
// this boundary, drains every result, clears the completed physical batch and
// then resumes the same source scan. No source tuple may be truncated.
class TupleDispatchBuffer {
public:
    /*
     * Context captured at BeginCustomScan from cscan->scan.plan.qual /
     * targetlist. seq_attno is the 1-based attribute number of the Var
     * passed as the first argument of mmsearch(); primary_key_attno is
     * the 1-based attribute number of the Var passed as the argument of
     * any bioseq.<field>(...) projection function. alphabet_code /
     * storage_unit_byte_width hold the (alphabet, width) typmod shared by the column
     * and the query literal — they are validated to be equal at
     * BeginCustomScan time. query_varlena holds a full detoasted copy
     * of the mmsearch second-argument bioseq.seq Const, so submit_scan
     * never has to re-read from the plan tree.
     */
    struct MmsearchContext {
        AttrNumber seq_attno           = InvalidAttrNumber;
        AttrNumber primary_key_attno   = InvalidAttrNumber;
        std::string column_name;
        std::vector<char> query_varlena;  // full detoasted varlena bytes, 16-byte aligned by std::allocator
        uint8_t alphabet_code          = 0;
        uint8_t storage_unit_byte_width             = 0;
        uint8_t query_alphabet_code    = 0;
        uint8_t query_storage_unit_byte_width = 0;
        bool active                    = false;

        /*
         * Per-row-query attno. Set when the mmsearch second argument is a
         * Var of the SAME relation (e.g. `mmsearch(t1.seq, t1.seq)` for
         * self-alignment, or `mmsearch(t1.seq, t1.seq2)` for sibling-
         * column alignment). The planner does not parameterize same-rel
         * Vars (no Param injection happens) so the existing Const/Param
         * paths reject the qual; instead we extract the value of this
         * column at submit_scan time and store it per-row in
         * BufferedScanEntry::per_row_query_varlena. Run-batch then
         * dispatches process_sequences once per entry against that
         * row's own query — which is the only correct semantics for
         * per-row mmsearch (broadcasting one row's query to other rows
         * is exactly the bug §9 of REPORT_POSTGRES_QUERY_BUGFIXING_1.md
         * documents).
         *
         * `per_row_self_alignment` is the special case where the
         * arg1 Var has the SAME varattno as the arg0 Var (same column).
         * mmsearch(seq, seq) is then equivalent to "every row aligns
         * against itself" — guaranteed full-length match. We still
         * dispatch through the aligner so the produced MatchResult is
         * the byte-for-byte alignment that the user can inspect via
         * bioseq.cigar / target_start / etc.
         */
        AttrNumber per_row_query_attno      = InvalidAttrNumber;
        bool       per_row_self_alignment   = false;
        bool       per_row_evaluated        = false;

        /*
         * Typmod coalesce: when the column and the query have the same
         * alphabet but different widths (e.g. column is dna4/32 and
         * query is dna4/64), the bytes in the query header carry the
         * larger width. The bioseq pipeline templates on
         * (alphabet_code, storage_unit_byte_width) and dispatches via
         * bioseq::pg_types::seq::dispatch_instance, so the query and target MUST
         * agree on both.
         * `coalesced_query_varlena` holds the query bytes re-encoded
         * at the COLUMN's (alphabet, width). The non-coalesced
         * `query_varlena` is preserved for the EXPLAIN line so users
         * see what the original literal was; the dispatch helper
         * `effective_query_hdr()` returns the coalesced bytes when
         * available, and the original ones otherwise.
         */
        std::vector<char> coalesced_query_varlena;
        bool       coalesce_active          = false;
        [[nodiscard]] const bioseq::pg_types::seq* query_hdr() const {
            const std::vector<char>& bytes =
                coalesce_active ? coalesced_query_varlena : query_varlena;
            if (!active || bytes.empty()) {
                return nullptr;
            }
            return reinterpret_cast<const bioseq::pg_types::seq*>(bytes.data());
        }
    };

    /*
     * One buffered scan tuple. The slot copy holds the full base-relation
     * tuple so projection can resolve any Var reference at read time.
     * primary_key is the detoasted INT4 value of the PK column; it is
     * used as the map key by the batch processor. seq_varlena is a
     * detoasted copy of the mmsearch target column's bioseq.seq value;
     * it is passed through BatchProcessor::process_sequences as the
     * per-row target sequence. Copying rather than pointing into the
     * slot is mandatory: tts_values[] is reused across scan calls and
     * the TOAST-detoasted buffer lives in CurrentMemoryContext which
     * may reset between rows.
     */
    struct BufferedScanEntry {
        struct RetainedExpandedAttribute {
            AttrNumber attno = InvalidAttrNumber;
            Datum datum = Datum{0};
            pg_sequence_descriptor descriptor{};
        };
        TupleTableSlot* scan_slot_copy      = nullptr;
        int32_t         primary_key         = -1;
        bool            primary_key_is_null = true;
        int64_t         sequence_rowno      = -1;   /* derived from scan_slot->tts_tid; -1 when the TID was invalid */
        std::vector<char> seq_varlena;
        bool            seq_is_null         = true;
        bool            target_from_evaluated_expression = false;

        /*
         * Per-row mmsearch query, populated only when MmsearchContext's
         * `per_row_query_attno` is set (i.e. the qual is
         * `mmsearch(this_rel.seq_a, this_rel.seq_b)` where the second
         * arg is a Var of the same relation). Holds the detoasted
         * bioseq.seq varlena bytes for THIS row's `seq_b` column — so
         * each row aligns against its own query. The bytes are stored
         * in the column's typmod (no coalesce needed: arg1 must be a
         * column of the same relation, which means it shares the
         * column's alphabet/width via the same Var resolution path).
         */
        std::vector<char> per_row_query_varlena;
        bool            per_row_query_is_null = true;
        bool            query_from_evaluated_expression = false;

        /*
         * DOC0260 §9.1 — Phase A deferred-hydration path. Under
         * defer_seq_hydration the custom scan returns 40-byte locator stubs
         * instead of materialised payloads, so submit_scan records the
         * decoded locator here (leaving seq_varlena empty). Production
         * CPU/SIMD dispatch preserves that locator until the algorithm enters
         * the statement source-provider facade. The hydrated pointers remain
         * only for explicit flat baselines and Vulkan's packed-upload ABI.
         * The seq_varlena path is still the fast path for a small inline value.
         */
        std::optional<bioseq::storage::seq_locator> seq_loc;
        std::optional<bioseq::storage::seq_locator> per_row_query_loc;
        /*
         * A SliceProject result is an expanded piece-tree Datum, not a flat
         * bioseq::pg_types::seq.  Keep a tiny read-only wrapper in the batch arena instead
         * of asking PG_DETOAST_DATUM to invoke the EOH flatten callback while
         * the row is merely being admitted.  The copied wrapper retains every
         * source-domain edge and remains valid until clear_slots() resets the
         * arena after the output cursors have drained.
         */
        Datum seq_recipe_datum = Datum{0};
        Datum per_row_query_recipe_datum = Datum{0};
        pg_sequence_descriptor seq_recipe_descriptor{};
        pg_sequence_descriptor per_row_query_recipe_descriptor{};
        std::vector<RetainedExpandedAttribute> expanded_sequence_attributes{};
        const bioseq::pg_types::seq* hydrated_seq_hdr           = nullptr;
        const bioseq::pg_types::seq* hydrated_per_row_query_hdr = nullptr;

        [[nodiscard]] bool has_seq_recipe() const noexcept {
            return seq_recipe_datum != Datum{0};
        }

        [[nodiscard]] bool has_per_row_query_recipe() const noexcept {
            return per_row_query_recipe_datum != Datum{0};
        }

        [[nodiscard]] std::uint64_t seq_bp_size() const noexcept {
            if (const bioseq::pg_types::seq* header = seq_hdr(); header != nullptr) {
                return header->bp_size;
            }
            if (has_seq_recipe()) {
                return seq_recipe_descriptor.bp_length;
            }
            if (seq_loc.has_value()) {
                return seq_loc->total_bp_size;
            }
            return 0u;
        }

        [[nodiscard]] const bioseq::pg_types::seq* seq_hdr() const {
            if (hydrated_seq_hdr != nullptr) {
                return hydrated_seq_hdr;
            }
            if (seq_is_null || seq_varlena.empty()) {
                return nullptr;
            }
            return reinterpret_cast<const bioseq::pg_types::seq*>(seq_varlena.data());
        }

        [[nodiscard]] const bioseq::pg_types::seq* per_row_query_hdr() const {
            if (hydrated_per_row_query_hdr != nullptr) {
                return hydrated_per_row_query_hdr;
            }
            if (per_row_query_is_null || per_row_query_varlena.empty()) {
                return nullptr;
            }
            return reinterpret_cast<const bioseq::pg_types::seq*>(
                per_row_query_varlena.data());
        }
    };

    /*
     * A cursor into (buffered_entries, match_map). output_cursors is
     * built in insertion order so EXPLAIN output mirrors INSERT order.
     */
    struct OutputCursor {
        size_t scan_entry_index = 0;
        size_t match_index      = 0;
    };

    struct NextOutput {
        BufferedScanEntry* entry;
        std::uint64_t query_ordinal;
        const ::bioseq::simd::MatchResult* match;
    };

    struct JoinQueryRef {
        std::uint64_t ordinal = 0;
        TupleTableSlot* query_slot = nullptr;
    };

    struct JoinOutputCursor {
        std::size_t query_index = 0;
        std::size_t match_index = 0;
    };

private:
    ::bioseq::simd::BatchProcessor processor;
    MmsearchContext mmsearch_context;

    /*
     * Per-scan base Minimap2Params — captured by BioseqColumnScan_BeginCustomScan
     * from the plan (GUC snapshot + per-call aligner_options() overrides) and
     * pushed into BatchProcessor::process_sequences once per batch. Every
     * row inherits these values with size-heuristic tuning applied if the
     * user left the minimap2 defaults in place.
     */
    ::bioseq::simd::Minimap2Params base_params{};

    /*
     * Effective per-scan persisted-payload read gate. `allow` preserves the
     * captured bioseq.read_sequence_index value; the two explicit contracts
     * override it. Payload probes happen later in the typed scanner path,
     * never in submit_scan.
     */
    bool read_sequence_index_active = false;
    bioseq::indexing::IndexReadRequirement index_read_requirement =
        bioseq::indexing::IndexReadRequirement::allow;

    /*
     * The OID of the relation being scanned. Only used to key into
     * bioseq_internal.index_sidecar; set at configure time so submit_scan
     * doesn't need to re-read RelationGetRelid per tuple.
     */
    Oid user_relid = InvalidOid;

    /*
     * Execution statistics captured during the last batch. Used by
     * BioseqColumnScan_ExplainCustomScan so it can compute the "Bioseq
     * Recomputed" percentage from runtime truth instead of the
     * plan-time estimate.
     */
    ::bioseq::pipeline_stats::PipelineExecutionStats last_stats{};

    std::vector<BufferedScanEntry> buffered_entries;
    std::map<int32_t, std::vector<::bioseq::simd::MatchResult>> match_map;
    std::vector<OutputCursor> output_cursors;
    int output_position = -1;

    bool join_mode = false;
    AttrNumber join_target_storage_seq_attno = InvalidAttrNumber;
    AttrNumber join_target_slot_seq_resno = InvalidAttrNumber;
    AttrNumber join_target_slot_rowno_resno = InvalidAttrNumber;
    AttrNumber join_query_slot_seq_resno = InvalidAttrNumber;
    std::vector<JoinQueryRef> join_queries{};
    std::map<std::uint64_t,
        std::vector<::bioseq::simd::MatchResult>>
        join_match_map{};
    std::vector<JoinOutputCursor> join_output_cursors{};
    std::size_t join_output_position = 0;
    bool join_target_active = false;
    bool join_target_prepared = false;
    bool join_target_outcomes_recorded = false;
	bool join_prepared_union_outcomes_recorded = false;
	bool join_prepared_build_checksum_recorded = false;
	std::optional<bioseq::storage::indexing::ActiveReadPolicyResolution>
		join_active_read_policy{};
	bioseq::storage::indexing::ColdIndexPreparationStatsPg
		join_cold_policy_stats{};
	::bioseq::simd::BatchProcessor::CpuPreparedTarget
		join_cpu_prepared_target{};
	::bioseq::simd::BatchProcessor::GpuPreparedShapeV5Target
		join_gpu_prepared_shape_v5_target{};
    std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity> join_target_hdrs{};
    std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity> join_query_hdrs{};
    std::array<int32_t, kDispatchBatchCapacity> join_query_ids{};
    std::array<bioseq::pg_types::seq*, kDispatchBatchCapacity> join_query_detoast_copies{};
    std::vector<std::vector<char>> join_query_rewidth_varlenas{};
    std::unique_ptr<
        ::bioseq::simd::BatchProcessor::ScannerPgContext>
        join_scanner_pg_ctx{};
    // PG_TRY is sigsetjmp-based: the catch path must not read a cache pointer
    // automatic that was assigned inside the protected region.
    bioseq::storage::seq_caches* join_scanner_pg_cache = nullptr;
    std::map<int32_t,
        std::vector<::bioseq::simd::MatchResult>>
        join_result_scratch{};

    /*
     * Additional Const-only mmsearch quals that share the primary
     * mmsearch_context's seq column and (alphabet, width) typmod.
     * Each entry holds the detoasted bioseq.seq varlena bytes for one
     * extra query. After the primary batch runs, run_batch_and_build_cursors
     * dispatches process_sequences once per extra query and intersects
     * the per-entry match presence so output_cursors only emit rows
     * that match every mmsearch qual (logical AND).
     *
     * Only the FIRST mmsearch context drives projection placeholders
     * (bioseq.cigar / score / target_*); the extra queries are pure
     * filters. Param-driven (cross-table) extras and same-column-but-
     * different-typmod extras are not supported in this iteration —
     * they are detected and rejected at BeginCustomScan with a log
     * line so the user sees why the qual didn't apply.
     */
    std::vector<std::vector<char>> extra_query_varlenas;

    /*
     * DOC0260 §9 — per-bucket deferred-hydration machinery. The custom scan
     * runs with defer_seq_hydration=true, so submit_scan captures only
     * locators; run_batch_and_build_cursors groups the buffered locators by
     * bucket_idx and issues one fetch_payload_bulk per non-empty bucket
     * (each opens that bucket's shards once, scanning them row-number-sorted).
     * Hydrated payloads live in batch_payload_ctx (reset per batch); the
     * per-scan db/spc/relfilenode identity is captured by configure_params.
     */
    using SM = bioseq::storage::bioseq_default_storage_manager;
    Oid user_main_database_oid   = InvalidOid;
    Oid user_main_tablespace_oid = InvalidOid;
    Oid user_main_relfilenode    = InvalidOid;
    std::optional<bioseq::storage::indexing::BorrowedExecutorReadContextPg>
        executor_read_context{};
    // Borrowed from the owning executor state for active scan execution. The
    // owner keeps it registered until every batch/scanner is drained; buffer
    // destruction never dereferences it and never changes its registration.
    Snapshot scan_snapshot       = nullptr;
    MemoryContext executor_query_mctx = nullptr;
    // Borrowed from the BioseqScan provider. Every locator-backed scanner
    // target must use this exact source-domain route; this buffer never owns
    // or closes it.
    bioseq::storage::pg_sequence_source_provider* source_provider = nullptr;
    // Borrowed from the statement-root AlignmentExecutionCollector.
    bioseq::pipeline_stats::PersistedGpuOperationEvidenceLedger*
        persisted_gpu_operation_evidence_sink = nullptr;
    slice_async_owner_guard* slice_async_owner = nullptr;
    bioseq::storage::sequence_payload_allocation_observer
        mmsearch_target_payload_observer{};
    bioseq::storage::sequence_payload_allocation_observer
        mmsearch_query_payload_observer{};
    MemoryContext batch_payload_ctx = nullptr;
    /* Reused virtual serializer input for rows containing expanded recipe
     * Datums.  Keeping this handle in member storage makes the PG_TRY error
     * path deterministic: a longjmp may abandon the current copy, but cannot
     * leave an untracked automatic slot or poison the next submitted row. */
    TupleTableSlot* copy_staging_slot = nullptr;
    std::array<std::vector<SM::fetch_request>, SM::bucket_count>
        per_bucket_target_requests{};
    std::array<std::vector<SM::fetch_request>, SM::bucket_count>
        per_bucket_query_requests{};
    std::vector<SM::fetch_request> fetch_bulk_sort_scratch{};
    std::vector<bioseq::pg_types::seq*>          hydrated_target_payloads{};
    std::vector<bioseq::pg_types::seq*>          hydrated_query_payloads{};
    pg_sequence_read_session* active_wave_read_session = nullptr;
    // Member storage is intentional: PG_TRY/PG_CATCH uses setjmp/longjmp, so
    // a modified automatic optional would have an indeterminate catch value.
    std::optional<bioseq::parallelism::MemoryPermit> active_wave_permit{};
    /* §9.6 EXPLAIN counters. */
    std::uint64_t explain_bucket_touched_mask      = 0;
    std::uint64_t explain_storage_bulk_fetch_calls = 0;

    struct EvaluatedRowOperands {
        Datum target = Datum{0};
        bool target_is_null = true;
        bool has_query_override = false;
        Datum query = Datum{0};
        bool query_is_null = true;
    };

    void require_evaluated_mmsearch_index_policy_() const {
        if (!evaluated_mmsearch_index_policy_valid(
                index_read_requirement, read_sequence_index_active)) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("evaluated bioseq mmsearch target requires no persisted target index"),
                 errdetail("index policy must be require_absent and payload reads must be disabled")));
        }
    }

    static void require_registered_mvcc_snapshot_(Snapshot snapshot) {
        if (snapshot == nullptr
            || !IsMVCCSnapshot(snapshot)
            || snapshot->regd_count == 0) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch requires a registered MVCC executor snapshot")));
        }
    }

    [[nodiscard]] Datum retain_expanded_recipe_(Datum input) {
        if (batch_payload_ctx == nullptr) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch recipe arena is not initialized")));
        }
        return bioseq::pg_utils::invoke_pg_as_cpp(
            [this, input] {
                MemoryContext previous = MemoryContextSwitchTo(
                    batch_payload_ctx);
                try {
                    /* Force a copy even when the producer handed us its RW
                     * wrapper: that wrapper commonly belongs to the current
                     * per-tuple context.  The retained edge graph is small and
                     * source-backed; no sequence payload is copied here. */
                    const Datum read_only = MakeExpandedObjectReadOnly(
                        input, false, -1);
                    acquired_sequence acquired =
                        acquire_writable_pg_sequence(read_only);
                    const Datum retained = pg_sequence_ro_datum(
                        acquired.value);
                    MemoryContextSwitchTo(previous);
                    return retained;
                } catch (...) {
                    MemoryContextSwitchTo(previous);
                    throw;
                }
            });
    }

    static void require_recipe_descriptor_(
            const pg_sequence_descriptor& descriptor,
            const std::uint8_t alphabet_code,
            const std::uint8_t storage_unit_byte_width,
            const char* operand) {
        const bool expanded =
            descriptor.kind == pg_sequence_datum_kind::expanded_read_only
            || descriptor.kind == pg_sequence_datum_kind::expanded_read_write;
        if (!expanded || !descriptor.alphabet_bound
            || descriptor.alphabet_code != alphabet_code
            || descriptor.storage_unit_byte_width
                != storage_unit_byte_width) {
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq mmsearch %s recipe typmod changed after planning",
                    operand),
                 errdetail("expected=(%u,%u) actual=(%u,%u) bound=%s kind=%u",
                    static_cast<unsigned>(alphabet_code),
                    static_cast<unsigned>(storage_unit_byte_width),
                    static_cast<unsigned>(descriptor.alphabet_code),
                    static_cast<unsigned>(
                        descriptor.storage_unit_byte_width),
                    descriptor.alphabet_bound ? "true" : "false",
                    static_cast<unsigned>(descriptor.kind))));
        }
    }

    void capture_target_datum_(BufferedScanEntry& entry, Datum datum) {
        if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(datum))) {
            const pg_sequence_descriptor descriptor =
                classify_and_describe_sequence(datum);
            require_recipe_descriptor_(
                descriptor,
                mmsearch_context.alphabet_code,
                mmsearch_context.storage_unit_byte_width,
                "target");
            entry.seq_recipe_datum = retain_expanded_recipe_(datum);
            entry.seq_recipe_descriptor = descriptor;
            entry.seq_is_null = false;
            return;
        }

        auto source = bioseq::pg_utils::detoast_as<bioseq::pg_types::seq>(
            bioseq::pg_utils::pg_datum::from_raw(datum));
        bioseq::pg_types::seq* header = source.ptr;
        if (bioseq::storage::is_locator_stub(header)) {
            entry.seq_loc = bioseq::storage::decode_locator_from_stub(header);
        } else {
            if (header->alphabet_code != mmsearch_context.alphabet_code
                || header->storage_unit_byte_width
                    != mmsearch_context.storage_unit_byte_width) {
                source.free_if_copy();
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq mmsearch target payload typmod changed after planning")));
            }
            const size_t total_bytes = static_cast<size_t>(VARSIZE(header));
            entry.seq_varlena.assign(
                reinterpret_cast<const char*>(header),
                reinterpret_cast<const char*>(header) + total_bytes);
        }
        entry.seq_is_null = false;
        source.free_if_copy();
    }

    void capture_query_datum_(BufferedScanEntry& entry, Datum datum) {
        if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(datum))) {
            const pg_sequence_descriptor descriptor =
                classify_and_describe_sequence(datum);
            require_recipe_descriptor_(
                descriptor,
                mmsearch_context.query_alphabet_code,
                mmsearch_context.query_storage_unit_byte_width,
                "query");
            entry.per_row_query_recipe_datum =
                retain_expanded_recipe_(datum);
            entry.per_row_query_recipe_descriptor = descriptor;
            entry.per_row_query_is_null = false;
            return;
        }

        auto source = bioseq::pg_utils::detoast_as<bioseq::pg_types::seq>(
            bioseq::pg_utils::pg_datum::from_raw(datum));
        bioseq::pg_types::seq* header = source.ptr;
        if (bioseq::storage::is_locator_stub(header)) {
            entry.per_row_query_loc =
                bioseq::storage::decode_locator_from_stub(header);
            entry.per_row_query_is_null = false;
            source.free_if_copy();
            return;
        }
        if (header->alphabet_code != mmsearch_context.query_alphabet_code
            || header->storage_unit_byte_width
                != mmsearch_context.query_storage_unit_byte_width) {
            source.free_if_copy();
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq mmsearch query payload typmod changed after planning")));
        }
        const bioseq::pg_types::seq* dispatch_header = header;
        bioseq::pg_types::seq* rewidth = nullptr;
        if (header->storage_unit_byte_width
                != mmsearch_context.storage_unit_byte_width) {
            rewidth = ::bioseq::simd::active_target::
                stdc_rewidth_to_type_params(
                    *header,
                    compact_sequence_stdc_type_params{
                        mmsearch_context.alphabet_code,
                        mmsearch_context.storage_unit_byte_width},
                    pg_alloc);
            if (rewidth == nullptr) {
                source.free_if_copy();
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq mmsearch query rewidth rejected an incompatible alphabet")));
            }
            dispatch_header = rewidth;
        }
        const size_t total_bytes = static_cast<size_t>(
            VARSIZE(dispatch_header));
        entry.per_row_query_varlena.assign(
            reinterpret_cast<const char*>(dispatch_header),
            reinterpret_cast<const char*>(dispatch_header) + total_bytes);
        if (rewidth != nullptr) {
            pfree(rewidth);
        }
        entry.per_row_query_is_null = false;
        source.free_if_copy();
    }

    void retain_expanded_sequence_output_attributes_(
            BufferedScanEntry& entry,
            TupleTableSlot* slot) {
        if (slot == nullptr || slot->tts_tupleDescriptor == nullptr
            || mmsearch_context.seq_attno <= 0
            || mmsearch_context.seq_attno
                > slot->tts_tupleDescriptor->natts) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq dispatch output slot descriptor is invalid")));
        }
        const Oid sequence_type_oid = TupleDescAttr(
            slot->tts_tupleDescriptor,
            mmsearch_context.seq_attno - 1)->atttypid;
        entry.expanded_sequence_attributes.clear();
        for (int index = 0;
             index < slot->tts_tupleDescriptor->natts; ++index) {
            if (slot->tts_isnull[index]
                || TupleDescAttr(slot->tts_tupleDescriptor, index)->atttypid
                    != sequence_type_oid) {
                continue;
            }
            const Datum value = slot->tts_values[index];
            if (value == Datum{0}) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("non-NULL bioseq dispatch output has a null Datum"),
                     errdetail("attribute=%d", index + 1)));
            }
            if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(value))) {
                continue;
            }
            const AttrNumber attno = static_cast<AttrNumber>(index + 1);
            Datum retained = Datum{0};
            pg_sequence_descriptor descriptor{};
            if (attno == mmsearch_context.seq_attno
                && entry.has_seq_recipe()
                && !entry.target_from_evaluated_expression) {
                retained = entry.seq_recipe_datum;
                descriptor = entry.seq_recipe_descriptor;
            } else if (attno == mmsearch_context.per_row_query_attno
                && entry.has_per_row_query_recipe()
                && !entry.query_from_evaluated_expression) {
                retained = entry.per_row_query_recipe_datum;
                descriptor = entry.per_row_query_recipe_descriptor;
            } else {
                descriptor = classify_and_describe_sequence(value);
                const bool expanded = descriptor.kind
                        == pg_sequence_datum_kind::expanded_read_only
                    || descriptor.kind
                        == pg_sequence_datum_kind::expanded_read_write;
                if (!expanded) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("expanded bioseq output has invalid descriptor")));
                }
                retained = retain_expanded_recipe_(value);
            }
            entry.expanded_sequence_attributes.push_back(
                BufferedScanEntry::RetainedExpandedAttribute{
                    .attno = attno,
                    .datum = retained,
                    .descriptor = descriptor,
                });
        }
    }

public:
    explicit TupleDispatchBuffer(const size_t estimated_tuple_count) {
        buffered_entries.reserve(estimated_tuple_count * 2);
        output_cursors.reserve(estimated_tuple_count * 2);
    }

    /// Bind the sole statement-root owner used by every batch/node/rescan.
    void bind_persisted_gpu_operation_evidence_sink(
        bioseq::pipeline_stats::PersistedGpuOperationEvidenceLedger& sink) {
        if (persisted_gpu_operation_evidence_sink != nullptr
            && persisted_gpu_operation_evidence_sink != &sink) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq persisted GPU evidence sink changed during execution")));
        }
        persisted_gpu_operation_evidence_sink = &sink;
    }

    void bind_slice_async_owner(slice_async_owner_guard* owner) {
        if (owner == nullptr
            || !make_slice_async_pool_observer_noexcept(owner)) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq async owner is not accepting tracked jobs")));
        }
        if (slice_async_owner != nullptr && slice_async_owner != owner) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq async owner changed during dispatch")));
        }
        slice_async_owner = owner;
        mmsearch_target_payload_observer =
            make_slice_payload_allocation_observer_noexcept(
                owner->attachment, 0u,
                materialization_mechanism::packed_region,
                materialization_reason::mmsearch_target);
        mmsearch_query_payload_observer =
            make_slice_payload_allocation_observer_noexcept(
                owner->attachment, 0u,
                materialization_mechanism::packed_region,
                materialization_reason::mmsearch_query);
    }

    void bind_sequence_source_provider(
            bioseq::storage::pg_sequence_source_provider* provider) {
        if (provider == nullptr || !provider->open
            || provider->lease.registry == nullptr) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch source provider is not live")));
        }
        if (source_provider != nullptr && source_provider != provider) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch source provider changed during execution")));
        }
        source_provider = provider;
    }

    void clear_sequence_source_provider_binding() noexcept {
        source_provider = nullptr;
    }

    /**
     * Release a provenance binding discovered from one PerRowJoin batch.
     *
     * Leaf scan buffers are bound once by their owning SliceProject and must
     * retain that binding across ordinary clear_slots() calls.  A PerRowJoin
     * buffer is different: its owner is discovered from the returned operand
     * EOHs, and a parameterized/partitioned rescan may legitimately return a
     * different owning projector after the previous batch is fully drained.
     */
    void release_per_row_slice_async_owner_after_drain() {
        if (join_mode || !buffered_entries.empty() || !match_map.empty()
            || !output_cursors.empty() || output_position != -1
            || active_wave_read_session != nullptr
            || active_wave_permit.has_value()) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq per-row async owner was released before dispatch drained")));
        }
        slice_async_owner = nullptr;
        mmsearch_target_payload_observer = {};
        mmsearch_query_payload_observer = {};
    }

	/* BatchJoin discovers its owner from the retained query relation.  The
	 * binding spans every target in one executor pass, then is released only
	 * after reset_join_runtime_for_rescan() has drained all waves. */
	void release_join_slice_async_owner_after_drain() {
		if (!join_mode || join_target_active || join_target_prepared
			|| !join_queries.empty() || !join_match_map.empty()
			|| !join_output_cursors.empty()
			|| active_wave_read_session != nullptr
			|| active_wave_permit.has_value()) {
			ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("BioseqBatchJoin async owner was released before dispatch drained")));
		}
		slice_async_owner = nullptr;
		mmsearch_target_payload_observer = {};
		mmsearch_query_payload_observer = {};
	}

    /*
     * Install the per-scan aligner parameters. Called from
     * BioseqColumnScan_BeginCustomScan after the GUC snapshot + any per-call
     * aligner_options() overrides have been applied. Must be called before
     * the first submit_scan.
     */
    void configure_params(const ::bioseq::simd::Minimap2Params& params,
                          bioseq::indexing::IndexReadRequirement read_requirement,
                          bool read_sequence_index,
                          Oid scan_user_relid,
                          Oid scan_user_database_oid,
                          Oid scan_user_tablespace_oid,
                          Oid scan_user_main_relfilenode,
                          Relation executor_relation,
                          Snapshot executor_snapshot,
                          MemoryContext executor_query_context) {
        require_registered_mvcc_snapshot_(executor_snapshot);
        if (executor_query_context == nullptr
            || !MemoryContextIsValid(executor_query_context)) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch executor query context is invalid")));
        }
        if (executor_relation == nullptr
            || RelationGetRelid(executor_relation) != scan_user_relid
            || executor_relation->rd_locator.dbOid != scan_user_database_oid
            || executor_relation->rd_locator.spcOid != scan_user_tablespace_oid
            || static_cast<Oid>(executor_relation->rd_locator.relNumber)
                != scan_user_main_relfilenode) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch executor relation identity is invalid")));
        }
        base_params                  = params;
        index_read_requirement       = read_requirement;
        read_sequence_index_active   =
            read_requirement
                    == bioseq::indexing::IndexReadRequirement::require_absent
                ? false
                : read_requirement
                    == bioseq::indexing::IndexReadRequirement::
                        require_compatible_persisted
                ? true
                : read_sequence_index;
        if (mmsearch_context.per_row_evaluated) {
            require_evaluated_mmsearch_index_policy_();
        }
        user_relid                   = scan_user_relid;
        user_main_database_oid       = scan_user_database_oid;
        user_main_tablespace_oid     = scan_user_tablespace_oid;
        user_main_relfilenode        = scan_user_main_relfilenode;
        scan_snapshot                = executor_snapshot;
        executor_query_mctx          = executor_query_context;
        executor_read_context = bioseq::storage::indexing::
            BorrowedExecutorReadContextPg::bind_checked(
                executor_relation, executor_snapshot);
        if (!executor_read_context.has_value()) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch borrowed executor context is invalid")));
        }
        last_stats                   = ::bioseq::pipeline_stats::PipelineExecutionStats{};
        /*
         * DOC0260 §9.4 — the payload arena is parented on CurrentMemoryContext
         * at BeginCustomScan time (the executor per-query context), so the
         * bulk-fetched bioseq::pg_types::seq* survive the full scan, not just one tuple.
         */
        if (batch_payload_ctx == nullptr) {
            batch_payload_ctx = AllocSetContextCreate(executor_query_mctx,
                "TupleDispatchBuffer payload arena", ALLOCSET_DEFAULT_SIZES);
        }
        if (fetch_bulk_sort_scratch.size() < kDispatchBatchCapacity) {
            fetch_bulk_sort_scratch.resize(kDispatchBatchCapacity);
        }
    }

    // Parallel workers restore the leader's serialized scan snapshot only
    // after BeginCustomScan configured this buffer. Rebind the borrowed
    // pointer before the worker can dispatch any row; ownership remains with
    // BioseqScanState.
    void replace_executor_snapshot(Snapshot executor_snapshot) {
        require_registered_mvcc_snapshot_(executor_snapshot);
        Relation executor_relation = executor_read_context.has_value()
            ? executor_read_context->relation() : nullptr;
        executor_read_context = bioseq::storage::indexing::
            BorrowedExecutorReadContextPg::bind_checked(
                executor_relation, executor_snapshot);
        if (!executor_read_context.has_value()) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch could not rebind its borrowed executor snapshot")));
        }
        scan_snapshot = executor_snapshot;
    }

    [[nodiscard]] const ::bioseq::simd::Minimap2Params& base_aligner_params() const { return base_params; }
    [[nodiscard]] const ::bioseq::pipeline_stats::PipelineExecutionStats& execution_stats() const { return last_stats; }
    [[nodiscard]] bool reads_sequence_index() const { return read_sequence_index_active; }
    [[nodiscard]] AttrNumber target_sequence_attno() const noexcept {
        return mmsearch_context.seq_attno;
    }
    [[nodiscard]] AttrNumber per_row_query_sequence_attno() const noexcept {
        return mmsearch_context.per_row_query_attno;
    }

    /* DOC0260 §9.6 — per-scan storage-bucket EXPLAIN counters. */
    [[nodiscard]] std::uint64_t explain_bucket_touched_bits() const {
        return explain_bucket_touched_mask;
    }
    [[nodiscard]] std::uint64_t explain_bulk_fetch_calls() const {
        return explain_storage_bulk_fetch_calls;
    }

    /*
     * Record the mmsearch column / pk / query triple extracted from the
     * plan. Called once per scan from BioseqColumnScan_BeginCustomScan.
     * query_varlena_bytes owns the detoasted varlena — moved in.
     *
     * (alphabet_code, storage_unit_byte_width) is the COLUMN's typmod — the
     * dispatch typmod for the whole batch. The query bytes may be
     * carried at a different (alphabet, width); the caller is
     * responsible for re-encoding if a coalesce is needed. See
     * `apply_typmod_coalesce()` below.
     */
    void configure_mmsearch(AttrNumber seq_attno,
                            std::string column_name,
                            std::vector<char> query_varlena_bytes,
                            uint8_t alphabet_code,
                            uint8_t storage_unit_byte_width,
                            AttrNumber primary_key_attno) {
        mmsearch_context.seq_attno          = seq_attno;
        mmsearch_context.primary_key_attno  = primary_key_attno;
        mmsearch_context.column_name        = std::move(column_name);
        mmsearch_context.query_varlena      = std::move(query_varlena_bytes);
        mmsearch_context.alphabet_code      = alphabet_code;
        mmsearch_context.storage_unit_byte_width         = storage_unit_byte_width;
        mmsearch_context.query_alphabet_code = alphabet_code;
        mmsearch_context.query_storage_unit_byte_width =
            storage_unit_byte_width;
        mmsearch_context.coalesce_active    = false;
        mmsearch_context.coalesced_query_varlena.clear();
        mmsearch_context.per_row_query_attno = InvalidAttrNumber;
        mmsearch_context.per_row_self_alignment = false;
        mmsearch_context.per_row_evaluated   = false;
        mmsearch_context.active             = true;

        uint32_t query_bp_size = 0;
        if (const auto* hdr = mmsearch_context.query_hdr()) {
            query_bp_size = hdr->bp_size;
        }
        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer::configure_mmsearch() seq_attno={} pk_attno={} column='{}' typmod=({},{}) query_bp_size={}",
                static_cast<int>(seq_attno),
                static_cast<int>(primary_key_attno),
                mmsearch_context.column_name,
                static_cast<unsigned>(alphabet_code),
                static_cast<unsigned>(storage_unit_byte_width),
                query_bp_size);
    }

    /*
     * Configure the per-row-query mode. Called by
     * BioseqColumnScan_BeginCustomScan when the mmsearch second arg is a
     * Var of the SAME relation — i.e. each row aligns against its
     * own value of `per_row_query_attno`. No global query varlena
     * is captured up front; submit_scan extracts the per-row bytes
     * for every passing tuple, and run_batch dispatches once per
     * entry.
     *
     * `per_row_self_alignment` is purely informational here (used
     * only by EXPLAIN to surface "self-alignment" instead of
     * "per-row alignment"). Both modes go through the same
     * dispatch path; we never short-circuit the aligner.
     */
    void configure_mmsearch_per_row(AttrNumber seq_attno,
                                    std::string column_name,
                                    AttrNumber per_row_query_attno,
                                    bool per_row_self_alignment,
                                    uint8_t alphabet_code,
                                    uint8_t storage_unit_byte_width,
                                    uint8_t query_alphabet_code,
                                    uint8_t query_storage_unit_byte_width,
                                    AttrNumber primary_key_attno) {
        if (alphabet_code != query_alphabet_code) {
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq mmsearch requires operands with the same bound alphabet")));
        }
        mmsearch_context.seq_attno              = seq_attno;
        mmsearch_context.primary_key_attno      = primary_key_attno;
        mmsearch_context.column_name            = std::move(column_name);
        mmsearch_context.query_varlena.clear();
        mmsearch_context.coalesced_query_varlena.clear();
        mmsearch_context.coalesce_active        = false;
        mmsearch_context.alphabet_code          = alphabet_code;
        mmsearch_context.storage_unit_byte_width             = storage_unit_byte_width;
        mmsearch_context.query_alphabet_code = query_alphabet_code;
        mmsearch_context.query_storage_unit_byte_width =
            query_storage_unit_byte_width;
        mmsearch_context.per_row_query_attno    = per_row_query_attno;
        mmsearch_context.per_row_self_alignment = per_row_self_alignment;
        mmsearch_context.per_row_evaluated      = false;
        mmsearch_context.active                 = true;

        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer::configure_mmsearch_per_row() seq_attno={} per_row_query_attno={} self_alignment={} target_typmod=({},{}) query_typmod=({},{})",
                static_cast<int>(seq_attno),
                static_cast<int>(per_row_query_attno),
                per_row_self_alignment,
                static_cast<unsigned>(alphabet_code),
                static_cast<unsigned>(storage_unit_byte_width),
                static_cast<unsigned>(query_alphabet_code),
                static_cast<unsigned>(query_storage_unit_byte_width));
    }

    /* Evaluated target/query expressions are submitted explicitly for each
     * passing base row.  The physical seq_attno remains source metadata and
     * identifies the raw output attribute, but is never read as an operand by
     * this mode. */
    void configure_mmsearch_per_row_evaluated(
            const AttrNumber seq_attno,
            std::string column_name,
            const std::uint8_t alphabet_code,
            const std::uint8_t storage_unit_byte_width,
            const std::uint8_t query_alphabet_code,
            const std::uint8_t query_storage_unit_byte_width,
            const AttrNumber primary_key_attno) {
        if (alphabet_code != query_alphabet_code) {
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq mmsearch requires operands with the same bound alphabet")));
        }
        mmsearch_context.seq_attno = seq_attno;
        mmsearch_context.primary_key_attno = primary_key_attno;
        mmsearch_context.column_name = std::move(column_name);
        mmsearch_context.query_varlena.clear();
        mmsearch_context.coalesced_query_varlena.clear();
        mmsearch_context.coalesce_active = false;
        mmsearch_context.alphabet_code = alphabet_code;
        mmsearch_context.storage_unit_byte_width =
            storage_unit_byte_width;
        mmsearch_context.query_alphabet_code = query_alphabet_code;
        mmsearch_context.query_storage_unit_byte_width =
            query_storage_unit_byte_width;
        mmsearch_context.per_row_query_attno = InvalidAttrNumber;
        mmsearch_context.per_row_self_alignment = false;
        mmsearch_context.per_row_evaluated = true;
        mmsearch_context.active = true;
    }

    /* Configure the narrow two-child BioseqBatchJoin seam. */
    void configure_mmsearch_join(
            const Oid target_relation_oid,
            const AttrNumber target_storage_seq_attno,
            const AttrNumber target_slot_seq_resno,
            const AttrNumber target_slot_rowno_resno,
            const AttrNumber query_slot_seq_resno,
            std::string target_column_name,
            const std::uint8_t alphabet_code,
            const std::uint8_t storage_unit_byte_width,
            const std::uint8_t query_alphabet_code,
            const std::uint8_t query_storage_unit_byte_width) {
        if (!OidIsValid(target_relation_oid)
            || target_storage_seq_attno <= 0
            || target_slot_seq_resno <= 0
            || target_slot_rowno_resno <= 0
            || query_slot_seq_resno <= 0) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("BioseqBatchJoin received invalid relation/attribute metadata")));
        }
        if (alphabet_code != query_alphabet_code) {
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq mmsearch requires operands with the same bound alphabet")));
        }
        if (OidIsValid(user_relid) && user_relid != target_relation_oid) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("BioseqBatchJoin target relation disagrees with dispatch configuration")));
        }
        user_relid = target_relation_oid;
        join_mode = true;
        join_target_storage_seq_attno = target_storage_seq_attno;
        join_target_slot_seq_resno = target_slot_seq_resno;
        join_target_slot_rowno_resno = target_slot_rowno_resno;
        join_query_slot_seq_resno = query_slot_seq_resno;
        mmsearch_context = MmsearchContext{};
        mmsearch_context.seq_attno = target_slot_seq_resno;
        mmsearch_context.primary_key_attno = InvalidAttrNumber;
        mmsearch_context.column_name = std::move(target_column_name);
        mmsearch_context.alphabet_code = alphabet_code;
        mmsearch_context.storage_unit_byte_width = storage_unit_byte_width;
        mmsearch_context.query_alphabet_code = query_alphabet_code;
        mmsearch_context.query_storage_unit_byte_width =
            query_storage_unit_byte_width;
        mmsearch_context.active = true;
        reset_join_runtime_for_rescan();
    }

    /* Pin/copy one locator-passthrough target for all query batches. */
    void begin_join_target(TupleTableSlot* target_slot) {
        if (!join_mode || target_slot == nullptr || !join_queries.empty()) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("BioseqBatchJoin target lifecycle is invalid")));
        }
		join_active_read_policy.reset();
		join_cold_policy_stats = {};
        bool sequence_is_null = true;
        const Datum sequence_datum = slot_getattr(
            target_slot, join_target_slot_seq_resno, &sequence_is_null);
        if (sequence_is_null) {
            ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("BioseqBatchJoin locator target sequence is NULL")));
        }
        auto target_sequence = bioseq::pg_utils::detoast_as<bioseq::pg_types::seq>(
            bioseq::pg_utils::pg_datum::from_raw(sequence_datum));
        const bool is_locator =
            bioseq::storage::is_locator_stub(target_sequence.ptr);
        target_sequence.free_if_copy();
        if (!is_locator) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqBatchJoin target provider did not return a locator stub"),
                 errdetail("locator passthrough is mandatory; materialized target values are not accepted by this executor")));
        }
        clear_slots();
        submit_scan(target_slot);
        if (buffered_entries.size() != 1) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("BioseqBatchJoin failed to retain exactly one target")));
        }
        bool rowno_is_null = true;
        const Datum rowno_datum = slot_getattr(
            target_slot, join_target_slot_rowno_resno, &rowno_is_null);
        if (rowno_is_null) {
            ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("BioseqBatchJoin hidden target row number is NULL")));
        }
        const std::int64_t rowno = DatumGetInt64(rowno_datum);
        if (rowno < 0) {
            ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("BioseqBatchJoin hidden target row number is negative")));
        }
        buffered_entries.front().sequence_rowno = rowno;
        join_target_active = true;
        join_target_prepared = false;
        join_target_outcomes_recorded = false;
		join_prepared_union_outcomes_recorded = false;
		join_prepared_build_checksum_recorded = false;
		join_cpu_prepared_target =
			::bioseq::simd::BatchProcessor::CpuPreparedTarget{};
		join_cpu_prepared_target.target_rowno = rowno;
		join_gpu_prepared_shape_v5_target =
			::bioseq::simd::BatchProcessor::
				GpuPreparedShapeV5Target{};
		join_gpu_prepared_shape_v5_target.target_rowno = rowno;
    }

    void prepare_join_target_queries(std::span<const JoinQueryRef> all_queries) {
        if (!join_target_active || join_target_prepared) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("BioseqBatchJoin target preparation is out of order")));
        }
		if (join_active_read_policy.has_value()) {
			ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("BioseqBatchJoin active-read policy was already resolved")));
		}
		const std::uint64_t policy_begin_ns =
			bioseq::storage::indexing::cold_index_now_ns();
		const BufferUsage policy_buffer_begin = pgBufferUsage;
		if (!executor_read_context.has_value()
			|| !executor_read_context->valid()) {
			ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("BioseqBatchJoin borrowed executor context is unavailable")));
		}
		join_active_read_policy.emplace(
			bioseq::storage::indexing::resolve_active_read_index_outcome(
				*executor_read_context,
				static_cast<std::int16_t>(join_target_storage_seq_attno),
				&join_cold_policy_stats));
		bioseq::storage::indexing::add_cold_index_elapsed(
			join_cold_policy_stats,
			join_cold_policy_stats.policy_ns,
			policy_begin_ns);
		bioseq::storage::indexing::add_cold_index_buffer_usage(
			join_cold_policy_stats, policy_buffer_begin);
		const bool vulkan_requested =
			::bioseq::simd::pipe::vulkan_components_requested(
				base_params);
		std::vector<const bioseq::pg_types::seq*> query_headers;
		std::vector<Datum> query_recipe_datums;
		std::vector<pg_sequence_descriptor> query_recipe_descriptors;
		std::vector<std::uint64_t> query_ordinals;
		std::vector<bioseq::pg_types::seq*> detoast_copies;
		std::vector<std::vector<char>> rewidthed_query_varlenas(
			all_queries.size());
		query_headers.reserve(all_queries.size());
		query_recipe_datums.reserve(all_queries.size());
		query_recipe_descriptors.reserve(all_queries.size());
		query_ordinals.reserve(all_queries.size());
		detoast_copies.reserve(all_queries.size());
        std::uint64_t previous_ordinal = 0;
        bool first = true;
        for (const JoinQueryRef& query : all_queries) {
            if (query.query_slot == nullptr
                || query.ordinal
                    > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())
                || (!first && query.ordinal <= previous_ordinal)) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("BioseqBatchJoin query references are invalid or unordered")));
            }
            bool query_is_null = true;
            const Datum query_datum = slot_getattr(
                query.query_slot, join_query_slot_seq_resno, &query_is_null);
            if (query_is_null) {
                ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("BioseqBatchJoin query sequence is NULL")));
            }
			if (VARATT_IS_EXTERNAL_EXPANDED(
					DatumGetPointer(query_datum))) {
				const pg_sequence_descriptor descriptor =
					classify_and_describe_sequence(query_datum);
				require_recipe_descriptor_(
					descriptor,
					mmsearch_context.query_alphabet_code,
					mmsearch_context.query_storage_unit_byte_width,
					"query");
				query_headers.push_back(nullptr);
				query_recipe_datums.push_back(query_datum);
				query_recipe_descriptors.push_back(descriptor);
				query_ordinals.push_back(query.ordinal);
				previous_ordinal = query.ordinal;
				first = false;
				continue;
			}
			bioseq::pg_types::seq* query_header = reinterpret_cast<bioseq::pg_types::seq*>(
				PG_DETOAST_DATUM(query_datum));
			if (bioseq::storage::is_locator_stub(query_header)) {
				if (reinterpret_cast<void*>(query_header)
					!= reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
					pfree(query_header);
				}
				ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("BioseqBatchJoin query preparation received a locator stub")));
			}
			if (query_header->alphabet_code
					!= mmsearch_context.query_alphabet_code
				|| query_header->storage_unit_byte_width
					!= mmsearch_context.query_storage_unit_byte_width) {
				if (reinterpret_cast<void*>(query_header)
					!= reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
					pfree(query_header);
				}
				ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("BioseqBatchJoin query typmod changed during preparation")));
			}
			const bioseq::pg_types::seq* preparation_query_header = query_header;
			if (query_header->storage_unit_byte_width
					!= mmsearch_context.storage_unit_byte_width) {
				bioseq::pg_types::seq* rewidthed = ::bioseq::simd::active_target::
					stdc_rewidth_to_type_params(
						*query_header,
						compact_sequence_stdc_type_params{
							mmsearch_context.alphabet_code,
							mmsearch_context.storage_unit_byte_width},
						pg_alloc);
				if (rewidthed == nullptr) {
					if (reinterpret_cast<void*>(query_header)
						!= reinterpret_cast<void*>(
							DatumGetPointer(query_datum))) {
						pfree(query_header);
					}
					ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("BioseqBatchJoin query preparation rewidth rejected an incompatible alphabet")));
				}
				const std::size_t rewidthed_bytes =
					static_cast<std::size_t>(VARSIZE(rewidthed));
				auto& owned = rewidthed_query_varlenas[query_headers.size()];
				owned.assign(
					reinterpret_cast<const char*>(rewidthed),
					reinterpret_cast<const char*>(rewidthed)
						+ rewidthed_bytes);
				pfree(rewidthed);
				preparation_query_header =
					reinterpret_cast<const bioseq::pg_types::seq*>(owned.data());
			}
			query_headers.push_back(preparation_query_header);
			query_recipe_datums.push_back(Datum{0});
			query_recipe_descriptors.push_back(pg_sequence_descriptor{});
			query_ordinals.push_back(query.ordinal);
			if (reinterpret_cast<void*>(query_header)
				!= reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
				detoast_copies.push_back(query_header);
			}
            previous_ordinal = query.ordinal;
            first = false;
        }
		const BufferedScanEntry& target = buffered_entries.front();
		const std::uint64_t target_bp_size = target.seq_bp_size();
		if (read_sequence_index_active) {
			MemoryContext saved_context = CurrentMemoryContext;
			ErrorData* volatile pg_error = nullptr;
			std::exception_ptr cpp_error;
			PG_TRY();
			{
				try {
					if (!vulkan_requested
						&& std::ranges::any_of(
							query_recipe_datums,
							[](Datum datum) noexcept {
								return datum != Datum{0};
							})) {
						if (active_wave_read_session != nullptr) {
							ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("BioseqBatchJoin retained a stale preparation session")));
						}
						active_wave_read_session =
							::begin_pg_sequence_read_session(
								batch_payload_ctx);
					}
					if (vulkan_requested) {
						processor.prepare_gpu_persisted_shape_v5_target(
							user_main_database_oid,
							user_main_tablespace_oid,
							user_main_relfilenode,
							user_relid,
							static_cast<std::int16_t>(
								join_target_storage_seq_attno),
							join_active_read_policy->index_name,
							static_cast<std::uint64_t>(
								join_gpu_prepared_shape_v5_target.target_rowno),
							target_bp_size,
							query_headers.size(),
							mmsearch_context.alphabet_code,
							mmsearch_context.storage_unit_byte_width,
							base_params,
							*executor_read_context,
							join_gpu_prepared_shape_v5_target);
					} else {
						if (index_read_requirement
								== bioseq::indexing::IndexReadRequirement::
									require_compatible_persisted) {
							processor.prepare_cpu_persisted_native_target(
								user_main_database_oid,
								user_main_tablespace_oid,
								user_main_relfilenode,
								user_relid,
								static_cast<std::int16_t>(
									join_target_storage_seq_attno),
								join_active_read_policy->index_name,
								static_cast<std::uint64_t>(
									join_cpu_prepared_target.target_rowno),
								target_bp_size,
								mmsearch_context.alphabet_code,
								mmsearch_context.storage_unit_byte_width,
								query_headers,
								query_recipe_datums,
								query_recipe_descriptors,
								active_wave_read_session,
								std::addressof(
									mmsearch_query_payload_observer),
								query_ordinals,
								base_params,
								*executor_read_context,
								join_cpu_prepared_target);
						} else {
							processor.prepare_cpu_shared_query_unions(
								mmsearch_context.alphabet_code,
								mmsearch_context.storage_unit_byte_width,
								target_bp_size,
								query_headers,
								query_recipe_datums,
								query_recipe_descriptors,
								active_wave_read_session,
								std::addressof(
									mmsearch_query_payload_observer),
								query_ordinals,
								base_params,
								/*include_persisted_parameter_group=*/true,
								::bioseq::simd::BatchProcessor::
									CpuQueryUnionStorage::minimizer_sketch,
								join_cpu_prepared_target);
						}
					}
					if (active_wave_read_session != nullptr) {
						::finish_pg_sequence_read_session(
							active_wave_read_session, true);
						active_wave_read_session = nullptr;
					}
				} catch (...) {
					cpp_error = std::current_exception();
				}
			}
			PG_CATCH();
			{
				MemoryContextSwitchTo(saved_context);
				pg_error = CopyErrorData();
				FlushErrorState();
			}
			PG_END_TRY();
			if (pg_error != nullptr) {
				::abort_pg_sequence_read_session_noexcept(
					std::exchange(active_wave_read_session, nullptr));
				query_headers.clear();
				detoast_copies.clear();
				ErrorData* exact_error = pg_error;
				throw bioseq::pg_utils::PostgresErrorAsCpp{exact_error};
			}
			if (cpp_error != nullptr) {
				::abort_pg_sequence_read_session_noexcept(
					std::exchange(active_wave_read_session, nullptr));
				for (bioseq::pg_types::seq* copy : detoast_copies) {
					bioseq::pg_utils::invoke_pg_as_cpp(
						[copy] { pfree(copy); });
				}
				std::rethrow_exception(cpp_error);
			}
		} else {
			/* no_index starts without a retained prepared target.  Building one
			 * in this pre-measurement lifecycle would invalidate cold semantics;
			 * the selected CPU/SIMD/GPU route must construct its own transient
			 * target index after timing begins and may not cross-fallback. */
			join_cpu_prepared_target.target_bp_size = target_bp_size;
			join_cpu_prepared_target.query_count = query_headers.size();
			join_cpu_prepared_target.unions_ready = false;
			join_gpu_prepared_shape_v5_target.target_bp_size = target_bp_size;
			join_gpu_prepared_shape_v5_target.query_count = query_headers.size();
			bioseq_log<DBG>(
				"[bioseq] pg: BatchJoin no_index retains no prepared target; "
				"the selected backend must use its authentic transient index route");
		}
		for (bioseq::pg_types::seq* copy : detoast_copies) {
			bioseq::pg_utils::invoke_pg_as_cpp(
				[copy] { pfree(copy); });
		}
        join_target_prepared = true;
    }

    void set_join_query_batch(std::span<const JoinQueryRef> queries) {
        if (!join_target_prepared
            || join_output_position != join_output_cursors.size()) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("BioseqBatchJoin query batch lifecycle is invalid")));
        }
        join_output_cursors.clear();
        join_match_map.clear();
        if (queries.size() > kDispatchBatchCapacity) {
            ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("BioseqBatchJoin query batch exceeds dispatch capacity")));
        }
        join_queries.assign(queries.begin(), queries.end());
        join_output_position = 0;
    }

    /*
     * Re-encode the literal query under (target_alphabet,
     * target_width) so the BatchProcessor's static dispatch agrees
     * with the column's typmod. Caller passes the detoasted bytes
     * of the re-encoded value (typically built via the same
     * convert_from(cstr, …, pg_alloc) path used by seq_in / seq_cast).
     *
     * The ORIGINAL query_varlena is preserved (so EXPLAIN can
     * surface the literal the user wrote) and `query_hdr()`
     * returns the coalesced bytes whenever `coalesce_active` is
     * true.
     */
    void apply_typmod_coalesce(std::vector<char> coalesced_bytes) {
        mmsearch_context.coalesced_query_varlena = std::move(coalesced_bytes);
        mmsearch_context.coalesce_active = true;
        const bioseq::pg_types::seq* raw_query = mmsearch_context.query_varlena.empty()
            ? nullptr : reinterpret_cast<const bioseq::pg_types::seq*>(
                mmsearch_context.query_varlena.data());
        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer::apply_typmod_coalesce() raw=({},{}) coalesced=({},{}) raw_bytes={} coalesced_bytes={}",
                static_cast<unsigned>(raw_query == nullptr
                    ? mmsearch_context.alphabet_code
                    : raw_query->alphabet_code),
                static_cast<unsigned>(raw_query == nullptr
                    ? mmsearch_context.storage_unit_byte_width
                    : raw_query->storage_unit_byte_width),
                static_cast<unsigned>(mmsearch_context.alphabet_code),
                static_cast<unsigned>(mmsearch_context.storage_unit_byte_width),
                mmsearch_context.query_varlena.size(),
                mmsearch_context.coalesced_query_varlena.size());
    }

    [[nodiscard]] bool per_row_mode() const noexcept {
        return dispatch_uses_per_row_query(
            mmsearch_context.active,
            mmsearch_context.per_row_query_attno,
            mmsearch_context.per_row_evaluated);
    }
    [[nodiscard]] AttrNumber per_row_query_attno() const {
        return mmsearch_context.per_row_query_attno;
    }
    [[nodiscard]] AttrNumber query_source_attno() const noexcept {
        if (!per_row_mode()) {
            return InvalidAttrNumber;
        }
        return mmsearch_context.per_row_query_attno != InvalidAttrNumber
            ? mmsearch_context.per_row_query_attno
            : mmsearch_context.seq_attno;
    }
    [[nodiscard]] bool coalesce_active() const {
        return mmsearch_context.coalesce_active;
    }
    [[nodiscard]] bool per_row_self_alignment() const {
        return mmsearch_context.per_row_self_alignment;
    }
    [[nodiscard]] uint8_t mmsearch_alphabet_code() const {
        return mmsearch_context.alphabet_code;
    }
    [[nodiscard]] uint8_t mmsearch_storage_unit_byte_width() const {
        return mmsearch_context.storage_unit_byte_width;
    }

    /*
     * Reset the primary mmsearch context. Used by:
     *   - BeginCustomScan as a defensive reset before re-installing
     *     the per-query primary (extras get added in the same Begin
     *     pass; clearing them here is correct because BeginCustomScan
     *     re-populates everything from scratch).
     *   - ReScanCustomScan ONLY when the per-rescan Param-rebind
     *     fails (Param resolved to NULL / wrong paramkind). After §9.4
     *     enabled Param-primary + Const-extras, the extras must
     *     SURVIVE a temporary primary failure: a NL where outer row
     *     #1 has a NULL Param and outer row #2 has a non-NULL one
     *     should still apply the extras for row #2. The extras are
     *     registered ONCE in BeginCustomScan and never re-added on
     *     rescan, so clearing them here would silently lose them
     *     for every later rescan — the §9 review caught this.
     *
     * `clear_extras=true` is the historical full-reset semantics
     * (used by BeginCustomScan defensive init only). Default is
     * false so ReScanCustomScan and any other caller preserves the
     * extras list across rebinds.
     */
    void clear_mmsearch_context(bool clear_extras = false) {
        mmsearch_context = MmsearchContext{};
        if (clear_extras) {
            extra_query_varlenas.clear();
        }
    }

    /*
     * Append another Const-driven mmsearch query that shares the
     * primary context's column and typmod. The caller (BeginCustomScan)
     * is responsible for validating the column / typmod match before
     * calling — we only own ownership of the bytes here.
     *
     * The accepted shape is exactly one same-column same-typmod
     * mmsearch beyond the first. Any extra query with a different
     * column or typmod must be rejected by the caller; if added here
     * it would silently get dispatched against the first context's
     * (seq, alphabet, width) and produce nonsense matches.
     */
    void add_extra_const_query(std::vector<char> query_varlena_bytes) {
        const bioseq::pg_types::seq* hdr =
            reinterpret_cast<const bioseq::pg_types::seq*>(
                query_varlena_bytes.data());
        const uint32_t bp_size = (query_varlena_bytes.empty() || hdr == nullptr)
            ? 0u : hdr->bp_size;
        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer::add_extra_const_query() bp_size={} (extra index #{})",
                bp_size, extra_query_varlenas.size());
        extra_query_varlenas.push_back(std::move(query_varlena_bytes));
    }

    [[nodiscard]] size_t extra_query_count() const {
        return extra_query_varlenas.size();
    }

    /*
     * Writing-phase hook. Fired from Inner_CustomExecScan after ExecQual
     * passes, using econtext->ecxt_scantuple which still holds every
     * attribute of the base relation. Extracts the bioseq.seq column
     * named by configure_mmsearch (detoasting its varlena), the PK
     * column value, and copies the slot into a MinimalTuple-backed
     * clone so the data outlives the next ExecScanFetch call.
     */
    [[gnu::always_inline]] inline void submit_scan(TupleTableSlot* scan_slot) {
        submit_scan_impl_(scan_slot, nullptr);
    }

    void submit_scan_evaluated(
            TupleTableSlot* scan_slot,
            const Datum target,
            const bool target_is_null,
            const bool has_query_override,
            const Datum query,
            const bool query_is_null) {
        require_evaluated_mmsearch_index_policy_();
        if (has_query_override != per_row_mode()) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("evaluated bioseq mmsearch query submission disagrees with its configuration"),
                 errdetail("per_row_mode=%s has_query_override=%s",
                    per_row_mode() ? "true" : "false",
                    has_query_override ? "true" : "false")));
        }
        const EvaluatedRowOperands evaluated{
            .target = target,
            .target_is_null = target_is_null,
            .has_query_override = has_query_override,
            .query = query,
            .query_is_null = query_is_null,
        };
        submit_scan_impl_(scan_slot, std::addressof(evaluated));
    }

private:
    [[gnu::always_inline]] inline void submit_scan_impl_(
            TupleTableSlot* scan_slot,
            const EvaluatedRowOperands* evaluated) {
        if (!mmsearch_context.active) {
            bioseq_log<DBG>("[bioseq] pg: submit_scan called while mmsearch_context inactive; skipping");
            return;
        }
        if (scan_slot == nullptr) {
            bioseq_log<DBG>("[bioseq] pg: submit_scan received null scan_slot; skipping");
            return;
        }
        if (buffered_entries.size() >= kDispatchBatchCapacity) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq dispatch buffer capacity invariant was violated"),
                 errdetail("buffered=%zu capacity=%zu",
                    buffered_entries.size(), kDispatchBatchCapacity)));
        }

        BufferedScanEntry entry{};

        /*
         * Capture the scan slot's tts_tid (set by the bioseq columnar
         * access method in BioseqColumnScanNext) and invert it back to a
         * row number so downstream consumers can join against the
         * sidecar tables (keyed on (user_relid, sequence_rowno)). Uses
         * the canonical tid_to_row_number helper from access_method.cppm
         * so the encoding stays in lockstep with row_number_to_tid.
         */
        if (ItemPointerIsValid(&scan_slot->tts_tid)) {
            entry.sequence_rowno = static_cast<int64_t>(
                tid_to_row_number(scan_slot->tts_tid));
        }

        /* ExprState NULL is authoritative for an evaluated operand.  Do not
         * consult the physical seq attribute: it may be non-NULL while the
         * expression is NULL, or vice versa. */
        if (evaluated != nullptr) {
            entry.target_from_evaluated_expression = true;
            if (evaluated->target_is_null) {
                entry.seq_is_null = true;
            } else if (evaluated->target == Datum{0}) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("evaluated bioseq mmsearch target has a null Datum")));
            } else {
                capture_target_datum_(entry, evaluated->target);
            }
        } else if (mmsearch_context.seq_attno == InvalidAttrNumber) {
            entry.seq_is_null = true;
        } else {
            bool is_null = true;
            Datum seq_datum = slot_getattr(scan_slot, mmsearch_context.seq_attno, &is_null);
            if (is_null) {
                entry.seq_is_null = true;
            } else {
                /* Flat/TOAST values retain the established compact fast path;
                 * locators retain their decoded source identity; expanded
                 * SliceProject values retain only a stable piece-tree wrapper.
                 * In particular, never send an expanded Datum through
                 * PG_DETOAST_DATUM here: that would flatten the complete
                 * transformed value before algorithm admission. */
                capture_target_datum_(entry, seq_datum);
            }
        }

        /* Extract the primary-key column. */
        if (mmsearch_context.primary_key_attno == InvalidAttrNumber) {
            entry.primary_key_is_null = true;
        } else {
            bool pk_null = true;
            Datum pk_datum = slot_getattr(scan_slot, mmsearch_context.primary_key_attno, &pk_null);
            if (pk_null) {
                entry.primary_key_is_null = true;
            } else {
                entry.primary_key = DatumGetInt32(pk_datum);
                entry.primary_key_is_null = false;
            }
        }

        /*
         * Per-row-query mode: extract the value of the OTHER bioseq.seq
         * column referenced by the mmsearch second arg. For
         * self-alignment (`per_row_self_alignment` true and
         * per_row_query_attno == seq_attno) this is literally the same
         * column we just buffered — we still re-extract so the path is
         * uniform between sibling-column and self-column shapes.
         * Per-row-query bytes retain the query column's declared typmod in the
         * copied output slot. The private dispatch copy is rewidthed once to
         * the target shape when both operands bind the same alphabet.
         */
        if (evaluated != nullptr && evaluated->has_query_override) {
            entry.query_from_evaluated_expression = true;
            if (evaluated->query_is_null) {
                entry.per_row_query_is_null = true;
            } else if (evaluated->query == Datum{0}) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("evaluated bioseq mmsearch query has a null Datum")));
            } else {
                /* Evaluate and retain target/query independently.  In
                 * particular, a self-alignment may contain two different
                 * slices of the same physical base column and must not reuse
                 * the target recipe merely because the raw attnos match. */
                capture_query_datum_(entry, evaluated->query);
            }
        } else if (mmsearch_context.per_row_query_attno
                != InvalidAttrNumber) {
            bool query_null = true;
            Datum query_datum = slot_getattr(scan_slot,
                mmsearch_context.per_row_query_attno, &query_null);
            if (query_null) {
                entry.per_row_query_is_null = true;
            } else if (mmsearch_context.per_row_query_attno
                           == mmsearch_context.seq_attno
                       && entry.has_seq_recipe()) {
                /* A self-alignment needs one retained wrapper, not two
                 * independently retained source graphs. */
                require_recipe_descriptor_(
                    entry.seq_recipe_descriptor,
                    mmsearch_context.query_alphabet_code,
                    mmsearch_context.query_storage_unit_byte_width,
                    "query");
                entry.per_row_query_recipe_datum = entry.seq_recipe_datum;
                entry.per_row_query_recipe_descriptor =
                    entry.seq_recipe_descriptor;
                entry.per_row_query_is_null = false;
            } else {
                capture_query_datum_(entry, query_datum);
            }
        }

        /*
         * Force materialization of every attribute before cloning. The
         * columnar scan only populates tts_values[] for attributes in
         * attrNeededList; anything outside that set carries a stale
         * Datum. ExecCopySlotMinimalTuple serialises tts_values[] to a
         * MinimalTuple, and slot_getallattrs() guarantees every entry
         * is valid before serialisation.
         */
        slot_getallattrs(scan_slot);
        retain_expanded_sequence_output_attributes_(entry, scan_slot);

        /* ExecCopySlotMinimalTuple materializes expanded attributes.  Mask
         * only the retained recipe columns while serializing the rest of the
         * tuple, then reinstall the batch-owned wrappers in the copied slot.
         * The source slot is never mutated and can be reused by its child. */
        TupleTableSlot* copy_source = scan_slot;
        if (batch_payload_ctx == nullptr) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch tuple arena is not initialized")));
        }
        MemoryContext previous_copy_context = MemoryContextSwitchTo(
            batch_payload_ctx);
        if (!entry.expanded_sequence_attributes.empty()) {
            if (copy_staging_slot == nullptr) {
                copy_staging_slot = MakeSingleTupleTableSlot(
                    scan_slot->tts_tupleDescriptor, &TTSOpsVirtual);
            } else if (copy_staging_slot->tts_tupleDescriptor
                    != scan_slot->tts_tupleDescriptor) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq dispatch staging slot descriptor changed")));
            }
            ExecClearTuple(copy_staging_slot);
            const int natts = scan_slot->tts_tupleDescriptor->natts;
            std::memcpy(
                copy_staging_slot->tts_values, scan_slot->tts_values,
                static_cast<std::size_t>(natts) * sizeof(Datum));
            std::memcpy(
                copy_staging_slot->tts_isnull, scan_slot->tts_isnull,
                static_cast<std::size_t>(natts) * sizeof(bool));
            const auto mask_recipe = [this, natts](AttrNumber attno) {
                const int index = static_cast<int>(attno) - 1;
                if (index >= 0 && index < natts) {
                    copy_staging_slot->tts_values[index] = Datum{0};
                    copy_staging_slot->tts_isnull[index] = true;
                }
            };
            for (const auto& expanded :
                 entry.expanded_sequence_attributes) {
                mask_recipe(expanded.attno);
            }
            copy_staging_slot->tts_nvalid = natts;
            ExecStoreVirtualTuple(copy_staging_slot);
            copy_source = copy_staging_slot;
        }

        entry.scan_slot_copy = MakeSingleTupleTableSlot(
            scan_slot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
        ExecStoreMinimalTuple(ExecCopySlotMinimalTuple(copy_source),
                              entry.scan_slot_copy, /*shouldFree=*/true);
        MemoryContextSwitchTo(previous_copy_context);

        /*
         * Eagerly materialize all attributes of the copied slot into
         * tts_values[]/tts_isnull[]. Without this, a downstream
         * ExecBuildProjectionInfo + ExecProject sees tts_nvalid=0 and
         * still works via the deopt getsomeattrs callback — but during
         * debugging we observed cases where the path built against the
         * copied slot's tupdesc fell back to zero-valued reads. Forcing
         * materialization now removes that ambiguity for zero cost.
         */
        slot_getallattrs(entry.scan_slot_copy);
        bioseq::pg_utils::pg_slot_datums_view copied_values{
            entry.scan_slot_copy};
        for (const auto& expanded :
             entry.expanded_sequence_attributes) {
            copied_values.set_raw(
                expanded.attno,
                bioseq::pg_utils::pg_datum::from_raw(
                    expanded.datum));
        }

        /*
         * DOC0279 §11.3c — no per-row blob read here any more. The stored
         * index (if any) is ASSEMBLED at batch time from its chunked storage,
         * keyed by entry.sequence_rowno (the columnar rowno == index
         * query_key, already computed above), so submit_scan only needs the
         * rowno (carried into scanner_pg_ctx.target_rownos[]).
         */
        const std::uint64_t entry_seq_bp_size = entry.seq_bp_size();
        bioseq_log<DBG>("[bioseq] pg: submit_scan pk={} rowno={} seq_bp_size={} (null={})",
                entry.primary_key_is_null ? -1 : entry.primary_key,
                entry.sequence_rowno,
                entry_seq_bp_size,
                entry.seq_is_null);

        buffered_entries.push_back(std::move(entry));
    }

public:
    /* True exactly when the current physical buffer must be dispatched. */
    [[nodiscard]] bool ready_to_dispatch() const noexcept {
        return buffered_entries.size() == kDispatchBatchCapacity;
    }

    /*
     * DOC0260 §9.3 — per-bucket bulk hydration of one seq column. Groups the
     * buffered entries that captured a locator for `column_attno` by
     * bucket_idx and issues one SM::fetch_payload_bulk per non-empty bucket
     * (each opens that bucket's shards once and scans them row-number-sorted).
     * The hydrated payloads (palloc'd in batch_payload_ctx) are recorded on
     * the entry so seq_hdr()/per_row_query_hdr() return them, and written back
     * into the buffered output slot so a target list projecting the raw seq
     * column resolves the Var to the materialised payload, not the stub.
     */
    void hydrate_one_column(
            std::span<const size_t> rows,
            bioseq::storage::seq_caches* cache,
            std::int16_t column_attno,
            std::array<std::vector<SM::fetch_request>, SM::bucket_count>& per_bucket,
            std::vector<bioseq::pg_types::seq*>& out_payloads,
            bool is_query) {
        for (auto& v : per_bucket) {
            v.clear();
        }
        // DOC0274 — compute letters_per_unit + storage_unit_byte_width from
        // the batch's (alphabet, width) typmod so the MaxAllocSize gate below
        // works for ANY alphabet / storage_unit_byte_width pair (dna4/uint8,
        // dna5/uint64, aa20/uint32, …) — NOT just the 2-bits-per-letter dna4
        // case. The dispatch happens once per column (not per row).
        //
        // Explicit captures (no `[&]` capture-all) per DOC0264 R6 — the user-
        // visible rule on the hot path is no capture-all lambdas.
        std::size_t lpu_for_column          = 0;
        std::size_t bytes_per_unit_for_col  = 0;
        const std::uint8_t column_alphabet_code = is_query
            ? mmsearch_context.query_alphabet_code
            : mmsearch_context.alphabet_code;
        const std::uint8_t column_storage_unit_byte_width = is_query
            ? mmsearch_context.query_storage_unit_byte_width
            : mmsearch_context.storage_unit_byte_width;
        bioseq::pg_types::seq::dispatch_instance(
            column_alphabet_code,
            column_storage_unit_byte_width,
            [&lpu_for_column, &bytes_per_unit_for_col]<class S>() noexcept {
                using traits = typename compact_sequence_range<S>::traits;
                lpu_for_column         = traits::letters_per_unit;
                bytes_per_unit_for_col = sizeof(typename traits::storage_unit);
            });
        if (lpu_for_column == 0 || bytes_per_unit_for_col == 0) {
            bioseq_log<ERR, PG>(
                "hydrate_one_column: dispatch_instance returned zero lpu / "
                "bytes_per_unit for (alphabet={}, width={})",
                static_cast<unsigned>(column_alphabet_code),
                static_cast<unsigned>(column_storage_unit_byte_width));
        }
        const size_t batch_size = buffered_entries.size();
        bool any = false;
        for (const size_t i : rows) {
            if (i >= batch_size) {
                continue;
            }
            const auto& loc = is_query ? buffered_entries[i].per_row_query_loc
                                       : buffered_entries[i].seq_loc;
            if (!loc.has_value()) {
                continue;
            }
            // DOC0274 — Phase C / D-VARLENA bridge: skip hydration when the
            // assembled varlena would exceed MaxAllocSize. The scanner_pg
            // dispatch path reads such sequences fragment-by-fragment without
            // materialising. Projection of `SELECT seq` on these rows raises
            // bioseq_log<ERR, PG>() from access_method.cppm — that's the documented
            // Phase C limit until Phase D D-VARLENA lifts it.
            //
            // Size = ceil(total_bp / letters_per_unit) * bytes_per_unit + header,
            // where lpu and bytes_per_unit come from the column's (alphabet,
            // width) typmod — generic across all supported alphabets.
            const std::uint64_t total_units = lpu_for_column == 0
                ? std::uint64_t{0}
                : (static_cast<std::uint64_t>(loc->total_bp_size)
                       + lpu_for_column - 1)
                  / lpu_for_column;
            const std::uint64_t projected_payload_bytes =
                total_units * static_cast<std::uint64_t>(bytes_per_unit_for_col);
            const std::uint64_t projected_varlena =
                static_cast<std::uint64_t>(bioseq::pg_types::seq::header_bytes())
                + projected_payload_bytes;
            if (projected_varlena > static_cast<std::uint64_t>(MaxAllocSize)) {
                bioseq_log<DBG>("[bioseq] pg: hydrate_one_column row={} attno={} too large "
                        "({} bp, lpu={}, byte_width={} ⇒ {} bytes > MaxAllocSize); "
                        "deferring to scanner_pg",
                        i, static_cast<int>(column_attno),
                        loc->total_bp_size, lpu_for_column,
                        bytes_per_unit_for_col, projected_varlena);
                continue;
            }
            per_bucket[loc->bucket_idx].push_back(SM::fetch_request{i, *loc});
            any = true;
        }
        if (!any) {
            return;
        }
        out_payloads.assign(batch_size, nullptr);
        for (std::uint8_t b = 0; b < SM::bucket_count; ++b) {
            auto& reqs = per_bucket[b];
            if (reqs.empty()) {
                continue;
            }
            SM::fetch_payload_bulk(
                user_main_database_oid, user_main_tablespace_oid,
                user_main_relfilenode, column_attno, static_cast<std::int16_t>(b),
                std::span<const SM::fetch_request>{reqs.data(), reqs.size()},
                std::span<bioseq::pg_types::seq*>{out_payloads.data(), out_payloads.size()},
                std::span<SM::fetch_request>{fetch_bulk_sort_scratch.data(),
                                             fetch_bulk_sort_scratch.size()},
                scan_snapshot, batch_payload_ctx, cache);
            explain_bucket_touched_mask |= (std::uint64_t{1} << b);
            ++explain_storage_bulk_fetch_calls;
        }
        for (const size_t i : rows) {
            if (i >= batch_size) {
                continue;
            }
            if (out_payloads[i] == nullptr) {
                continue;
            }
            if (is_query) {
                bioseq::pg_types::seq* source_query = out_payloads[i];
                if (source_query->alphabet_code
                        != mmsearch_context.query_alphabet_code
                    || source_query->storage_unit_byte_width
                        != mmsearch_context.query_storage_unit_byte_width) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("bioseq mmsearch hydrated query typmod changed after planning")));
                }
                bioseq::pg_types::seq* dispatch_query = source_query;
                if (source_query->storage_unit_byte_width
                        != mmsearch_context.storage_unit_byte_width) {
                    MemoryContext old_context = MemoryContextSwitchTo(
                        batch_payload_ctx);
                    dispatch_query = ::bioseq::simd::active_target::
                        stdc_rewidth_to_type_params(
                            *source_query,
                            compact_sequence_stdc_type_params{
                                mmsearch_context.alphabet_code,
                                mmsearch_context.storage_unit_byte_width},
                            pg_alloc);
                    MemoryContextSwitchTo(old_context);
                    if (dispatch_query == nullptr) {
                        ereport(ERROR,
                            (errcode(ERRCODE_DATATYPE_MISMATCH),
                             errmsg("bioseq mmsearch hydrated query rewidth rejected an incompatible alphabet")));
                    }
                }
                buffered_entries[i].hydrated_per_row_query_hdr =
                    dispatch_query;
            } else {
                buffered_entries[i].hydrated_seq_hdr = out_payloads[i];
            }
	            TupleTableSlot* sslot = buffered_entries[i].scan_slot_copy;
                const bool preserve_raw_output = is_query
                    ? buffered_entries[i].query_from_evaluated_expression
                    : buffered_entries[i].target_from_evaluated_expression;
	            if (sslot != nullptr && !preserve_raw_output) {
	                const int idx = column_attno - 1;
	                if (idx >= 0 && idx < sslot->tts_tupleDescriptor->natts) {
	                    bioseq::pg_utils::pg_slot_datums_view{sslot}.set_raw(
	                        column_attno,
                        bioseq::pg_utils::pg_datum::from_pointer(out_payloads[i]));
	                }
	            }
        }
    }

    /* Integration-only Phase-B baselines. Production scanner dispatch must
     * not call this routine: it would stitch every locator into a flat varlena
     * before the source-domain facade had a chance to stream it. */
    void hydrate_locators_by_bucket(std::span<const size_t> rows) {
        const size_t batch_size = buffered_entries.size();
        bool any_locator = false;
        for (const size_t i : rows) {
            if (i >= batch_size) {
                continue;
            }
            if (buffered_entries[i].seq_loc.has_value()
                || buffered_entries[i].per_row_query_loc.has_value()) {
                any_locator = true;
                break;
            }
        }
        if (!any_locator) {
            return;
        }
        if (batch_payload_ctx == nullptr) {
            batch_payload_ctx = AllocSetContextCreate(executor_query_mctx,
                "TupleDispatchBuffer payload arena", ALLOCSET_DEFAULT_SIZES);
        }
        if (fetch_bulk_sort_scratch.size() < batch_size) {
            fetch_bulk_sort_scratch.resize(batch_size);
        }

        bioseq::storage::seq_caches* cache =
            bioseq::storage::alloc_seq_caches(batch_payload_ctx);
        cache->side                = bioseq::storage::seq_caches::Side::Read;
        cache->lock_mode           = AccessShareLock;
        cache->read_state_mctx     = batch_payload_ctx;
        cache->main_database_oid   = user_main_database_oid;
        cache->main_tablespace_oid = user_main_tablespace_oid;
        cache->main_relfilenode    = user_main_relfilenode;

        hydrate_one_column(rows, cache,
            static_cast<std::int16_t>(mmsearch_context.seq_attno),
            per_bucket_target_requests, hydrated_target_payloads,
            /*is_query=*/false);

        if (per_row_mode()) {
            hydrate_one_column(rows, cache,
                static_cast<std::int16_t>(query_source_attno()),
                per_bucket_query_requests, hydrated_query_payloads,
                /*is_query=*/true);
        }

        cache->close_all(/*commit=*/false);
        bioseq::storage::destroy_seq_caches(cache);
    }

    [[nodiscard]] bool run_join_query_batch_and_build_cursors() {
        if (!join_mode || !join_target_prepared || !join_target_active) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("BioseqBatchJoin query batch ran before target preparation")));
        }
        if (join_queries.empty()) {
            last_stats = ::bioseq::pipeline_stats::PipelineExecutionStats{};
            join_output_cursors.clear();
            join_output_position = 0;
            return false;
        }
        if (join_queries.size() > kDispatchBatchCapacity
            || buffered_entries.size() != 1) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("BioseqBatchJoin dispatch shape is invalid")));
        }

        bioseq::parallelism::pg::backend_sync();
        last_stats = ::bioseq::pipeline_stats::PipelineExecutionStats{};
        const std::size_t batch_size = join_queries.size();
        join_target_hdrs.fill(nullptr);
        join_query_hdrs.fill(nullptr);
        join_query_ids.fill(0);
        join_query_detoast_copies.fill(nullptr);
        join_query_rewidth_varlenas.clear();
        join_query_rewidth_varlenas.resize(batch_size);
        join_result_scratch.clear();
        if (!join_scanner_pg_ctx) {
            join_scanner_pg_ctx = std::make_unique<
                ::bioseq::simd::BatchProcessor::
                    ScannerPgContext>();
        }
        *join_scanner_pg_ctx =
            ::bioseq::simd::BatchProcessor::
                ScannerPgContext{};
		auto& scanner_pg_ctx = *join_scanner_pg_ctx;
        const auto ticket = bioseq::parallelism::pg::begin_batch(
            static_cast<std::int64_t>(batch_size));
		auto prepared_counter_totals = [this] {
			std::array<std::uint64_t, 5> totals{};
			for (const auto& group : join_cpu_prepared_target.groups) {
				totals[0] += group.target_index_builds;
				totals[1] += group.target_index_reuses;
				totals[2] += group.duplicate_build_attempts;
				totals[3] += group.persisted_reader_loads;
				totals[4] += group.persisted_reader_queries;
			}
			return totals;
		};
		const auto prepared_before = prepared_counter_totals();
        join_scanner_pg_cache = nullptr;
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* volatile original_error = nullptr;
        std::exception_ptr cpp_error;
        PG_TRY();
        {
          try {
            BufferedScanEntry& target = buffered_entries.front();
            for (std::size_t i = 0; i < batch_size; ++i) {
                const JoinQueryRef& query = join_queries[i];
                if (query.query_slot == nullptr
                    || query.ordinal
                        > static_cast<std::uint64_t>(
                            (std::numeric_limits<std::int32_t>::max)())) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("BioseqBatchJoin query reference is invalid")));
                }
                bool query_is_null = true;
                const Datum query_datum = slot_getattr(
                    query.query_slot, join_query_slot_seq_resno, &query_is_null);
                if (query_is_null) {
                    ereport(ERROR,
                        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                         errmsg("BioseqBatchJoin query sequence is NULL")));
                }
                if (VARATT_IS_EXTERNAL_EXPANDED(
                        DatumGetPointer(query_datum))) {
					const pg_sequence_descriptor descriptor =
						classify_and_describe_sequence(query_datum);
					require_recipe_descriptor_(
						descriptor,
						mmsearch_context.query_alphabet_code,
						mmsearch_context.query_storage_unit_byte_width,
						"query");
					join_query_hdrs[i] = nullptr;
					scanner_pg_ctx.query_recipe_datums[i] = query_datum;
					scanner_pg_ctx.query_recipe_descriptors[i] = descriptor;
					join_target_hdrs[i] = target.seq_hdr();
					scanner_pg_ctx.target_recipe_datums[i] =
						target.seq_recipe_datum;
					scanner_pg_ctx.target_recipe_descriptors[i] =
						target.seq_recipe_descriptor;
					join_query_ids[i] = static_cast<std::int32_t>(query.ordinal);
					continue;
				}
                bioseq::pg_types::seq* query_hdr = reinterpret_cast<bioseq::pg_types::seq*>(
                    PG_DETOAST_DATUM(query_datum));
                if (bioseq::storage::is_locator_stub(query_hdr)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("BioseqBatchJoin query child returned a locator stub"),
                         errhint("The bounded query child must materialize its compact sequence before join dispatch.")));
                }
                if (query_hdr->alphabet_code
                        != mmsearch_context.query_alphabet_code
                    || query_hdr->storage_unit_byte_width
                        != mmsearch_context.query_storage_unit_byte_width) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("BioseqBatchJoin query typmod changed after planning")));
                }
                if (reinterpret_cast<void*>(query_hdr)
                    != reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
                    join_query_detoast_copies[i] = query_hdr;
                }
                const bioseq::pg_types::seq* dispatch_query_hdr = query_hdr;
                if (query_hdr->storage_unit_byte_width
                        != mmsearch_context.storage_unit_byte_width) {
                    bioseq::pg_types::seq* rewidth = ::bioseq::simd::active_target::
                        stdc_rewidth_to_type_params(
                            *query_hdr,
                            compact_sequence_stdc_type_params{
                                mmsearch_context.alphabet_code,
                                mmsearch_context.storage_unit_byte_width},
                            pg_alloc);
                    if (rewidth == nullptr) {
                        ereport(ERROR,
                            (errcode(ERRCODE_DATATYPE_MISMATCH),
                             errmsg("BioseqBatchJoin query rewidth rejected an incompatible alphabet")));
                    }
                    const std::size_t rewidth_bytes =
                        static_cast<std::size_t>(VARSIZE(rewidth));
                    join_query_rewidth_varlenas[i].assign(
                        reinterpret_cast<const char*>(rewidth),
                        reinterpret_cast<const char*>(rewidth)
                            + rewidth_bytes);
                    pfree(rewidth);
                    dispatch_query_hdr = reinterpret_cast<const bioseq::pg_types::seq*>(
                        join_query_rewidth_varlenas[i].data());
                }
                join_query_hdrs[i] = dispatch_query_hdr;
                join_target_hdrs[i] = target.seq_hdr();
				scanner_pg_ctx.target_recipe_datums[i] =
					target.seq_recipe_datum;
				scanner_pg_ctx.target_recipe_descriptors[i] =
					target.seq_recipe_descriptor;
                join_query_ids[i] = static_cast<std::int32_t>(query.ordinal);
            }

            if (batch_payload_ctx == nullptr) {
                batch_payload_ctx = AllocSetContextCreate(
                    CurrentMemoryContext,
                    "TupleDispatchBuffer join payload arena",
                    ALLOCSET_DEFAULT_SIZES);
            }
            join_scanner_pg_cache = bioseq::storage::alloc_seq_caches(
                batch_payload_ctx);
            join_scanner_pg_cache->side =
                bioseq::storage::seq_caches::Side::Read;
            join_scanner_pg_cache->lock_mode = AccessShareLock;
            join_scanner_pg_cache->read_state_mctx = batch_payload_ctx;
            join_scanner_pg_cache->main_database_oid = user_main_database_oid;
            join_scanner_pg_cache->main_tablespace_oid = user_main_tablespace_oid;
            join_scanner_pg_cache->main_relfilenode = user_main_relfilenode;

            scanner_pg_ctx.async_job_observer =
                make_slice_async_pool_observer_noexcept(slice_async_owner);
            scanner_pg_ctx.mmsearch_target_payload_observer =
                mmsearch_target_payload_observer;
            scanner_pg_ctx.mmsearch_query_payload_observer =
                mmsearch_query_payload_observer;
            scanner_pg_ctx.db_oid = user_main_database_oid;
            scanner_pg_ctx.tablespace_oid = user_main_tablespace_oid;
            scanner_pg_ctx.main_relfilenode = user_main_relfilenode;
            scanner_pg_ctx.mmsearch_seq_attno =
                static_cast<std::int16_t>(join_target_storage_seq_attno);
			scanner_pg_ctx.mmsearch_query_attno =
				static_cast<std::int16_t>(join_query_slot_seq_resno);
			scanner_pg_ctx.query_alphabet_code =
				mmsearch_context.query_alphabet_code;
			scanner_pg_ctx.query_storage_unit_byte_width =
				mmsearch_context.query_storage_unit_byte_width;
            scanner_pg_ctx.caches = join_scanner_pg_cache;
            scanner_pg_ctx.parent_mctx = batch_payload_ctx;
            scanner_pg_ctx.snapshot = scan_snapshot;
            scanner_pg_ctx.source_provider = source_provider;
            scanner_pg_ctx.persisted_gpu_operation_evidence_sink =
                persisted_gpu_operation_evidence_sink;
            scanner_pg_ctx.user_relid = user_relid;
            scanner_pg_ctx.index_read_requirement = index_read_requirement;
            scanner_pg_ctx.read_index_active = read_sequence_index_active;
            scanner_pg_ctx.index_async_persist_active =
                bioseq::settings::index_async_persist;
            scanner_pg_ctx.recompute_async_persist_active =
                bioseq::settings::recompute_async_persist;
			if (!join_active_read_policy.has_value()) {
				ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("BioseqBatchJoin active-read policy proof is missing")));
			}
			install_active_read_policy_dispatch_proof(
				scanner_pg_ctx,
				*join_active_read_policy,
				user_relid, user_main_relfilenode,
				static_cast<std::int16_t>(join_target_storage_seq_attno));
            const bool record_physical_target =
                !join_target_outcomes_recorded;
            scanner_pg_ctx.explicit_query_ordinals = true;
			/* Ask the generic processor for its native raw evidence on every
			 * wave. CPU reports one target/source observation per pair; GPU
			 * reports its already-deduplicated logical target identity. The
			 * BatchJoin owner validates that backend-specific shape above and
			 * alone decides whether this wave publishes the physical target. */
            scanner_pg_ctx.shared_target_batch = false;
            scanner_pg_ctx.record_target_operational_outcomes = true;
			scanner_pg_ctx.cpu_prepared_target =
				join_cpu_prepared_target.unions_ready
					? &join_cpu_prepared_target : nullptr;
			scanner_pg_ctx.gpu_prepared_shape_v5_target =
				join_gpu_prepared_shape_v5_target.ready()
					? &join_gpu_prepared_shape_v5_target : nullptr;
            for (std::size_t i = 0; i < batch_size; ++i) {
                scanner_pg_ctx.target_locators[i] = target.seq_loc;
                scanner_pg_ctx.target_rownos[i] = target.sequence_rowno;
                scanner_pg_ctx.query_ordinals[i] = join_queries[i].ordinal;
            }
			const bool wave_has_recipe_or_locator =
				target.seq_loc.has_value() || target.has_seq_recipe()
				|| std::ranges::any_of(
					scanner_pg_ctx.query_recipe_datums.begin(),
					scanner_pg_ctx.query_recipe_datums.begin() + batch_size,
					[](Datum datum) noexcept { return datum != Datum{0}; });
			if (wave_has_recipe_or_locator) {
				if (active_wave_read_session != nullptr) {
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("BioseqBatchJoin retained a stale dispatch session")));
				}
				active_wave_read_session =
					::begin_pg_sequence_read_session(batch_payload_ctx);
				scanner_pg_ctx.read_session = active_wave_read_session;
			}

            join_result_scratch = processor.process_sequences(
                batch_size,
                mmsearch_context.alphabet_code,
                mmsearch_context.storage_unit_byte_width,
                nullptr,
                &join_query_hdrs,
                join_target_hdrs,
                join_query_ids,
                base_params,
                ::bioseq::simd::BatchInvocationRole::primary,
                0,
                last_stats,
                &scanner_pg_ctx);
			if (active_wave_read_session != nullptr) {
				::finish_pg_sequence_read_session(
					active_wave_read_session, true);
				active_wave_read_session = nullptr;
				scanner_pg_ctx.read_session = nullptr;
			}
			if (join_cpu_prepared_target.unions_ready
				&& !join_cpu_prepared_target.query_set_proof_ready()) {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("BioseqBatchJoin compact-direct query-set proof remained pending after dispatch")));
			}
			normalize_shared_join_target_outcomes(
				last_stats,
				record_physical_target,
				batch_size,
				classify_shared_join_target_outcome_topology(
					::bioseq::simd::pipe::
						vulkan_components_requested(base_params),
					::bioseq::simd::pipe::
						complete_all_gpu_tuple_selected(base_params)));
			const auto prepared_after = prepared_counter_totals();
			if (prepared_after[0] < prepared_before[0]
				|| prepared_after[1] < prepared_before[1]
				|| prepared_after[2] < prepared_before[2]
				|| prepared_after[3] < prepared_before[3]
				|| prepared_after[4] < prepared_before[4]
				|| join_cpu_prepared_target.physical_target_index_reads
					!= prepared_after[0] + prepared_after[3]) {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("BioseqBatchJoin prepared-target counters are inconsistent")));
			}
			auto& operational = last_stats.operational;
			if (!join_cpu_prepared_target
					.query_union_prepare_host_wall_published
				&& join_cpu_prepared_target
					.query_union_prepare_host_wall_ns != 0u) {
				const std::size_t query_sketch_stage =
					::bioseq::pipeline_stats::stage_index(
						::bioseq::pipeline_stats::PipelineStage::query_sketch);
				operational.stats_saturated =
					::bioseq::pipeline_stats::merge_counter(
						operational.timing[query_sketch_stage].host_wall_ns,
						join_cpu_prepared_target
							.query_union_prepare_host_wall_ns)
					|| operational.stats_saturated;
				join_cpu_prepared_target
					.query_union_prepare_host_wall_published = true;
			}
			::bioseq::pipeline_stats::PipelineOperationalStats prepared_delta{};
			prepared_delta.cpu_prepared_index_builds =
				prepared_after[0] - prepared_before[0];
			prepared_delta.cpu_prepared_index_reuses =
				prepared_after[1] - prepared_before[1];
			prepared_delta.cpu_prepared_duplicate_build_attempts =
				prepared_after[2] - prepared_before[2];
			operational.merge(prepared_delta);
			if (!join_cpu_prepared_target.persisted_read_stats_published
				&& !join_gpu_prepared_shape_v5_target.ready()) {
				merge_cold_index_preparation_stats(
					operational, join_cold_policy_stats);
				for (const auto& group : join_cpu_prepared_target.groups) {
					if (!group.persisted_grouped32_reader
						&& !group.persisted_shape_v5_reader
						&& !group.persisted_mps_shape_v5_reader
						&& !group.persisted_format6_reader) continue;
					const auto& read = group.persisted_read_stats;
					::bioseq::pipeline_stats::PipelineOperationalStats read_delta{};
					read_delta.persisted_generation_read_sessions = 1;
					read_delta.persisted_relation_opens = read.relation_opens;
					read_delta.persisted_index_scans = read.index_scans;
					read_delta.persisted_head_scans = read.head_scans;
					read_delta.persisted_head_rows_read = read.head_rows_read;
					read_delta.persisted_head_bytes_read = read.head_bytes_read;
					read_delta.persisted_manifest_directory_scans =
						read.manifest_directory_scans;
					read_delta.persisted_manifest_rows_read =
						read.manifest_rows_read;
					read_delta.persisted_manifest_bytes_read =
						read.manifest_bytes_read;
					read_delta.persisted_directory_chunks_read =
						read.directory_chunks_read;
					read_delta.persisted_directory_bytes_read =
						read.directory_bytes_read;
					read_delta.persisted_metadata_repeated_reads =
						read.metadata_repeated_reads;
					read_delta.persisted_metadata_heap_rows_read =
						read.metadata_heap_rows_read;
					read_delta.persisted_metadata_bytes_read =
						read.metadata_bytes_read;
					read_delta.persisted_payload_heap_rows_read =
						read.payload_heap_rows_read;
					read_delta.persisted_payload_bytes_read =
						read.payload_bytes_read;
					read_delta.persisted_heap_rows_read =
						read.total_heap_rows_read;
					read_delta.persisted_toast_values_read =
						read.toast_values_read;
					read_delta.persisted_detoast_bytes = read.detoast_bytes;
					read_delta.persisted_explicit_copy_bytes =
						read.explicit_copy_bytes;
					read_delta.persisted_wanted_subkeys =
						read.payload.wanted_subkeys;
					read_delta.persisted_read_subkeys =
						read.payload.read_subkeys;
					read_delta.persisted_wanted_extents =
						read.payload.wanted_extents;
					read_delta.persisted_wanted_chunks =
						read.payload.wanted_chunks;
					read_delta.persisted_skipped_rows =
						read.payload.skipped_rows;
					read_delta.persisted_repeated_chunk_reads =
						read.payload.repeated_chunk_reads;
					read_delta.persisted_extents_read =
						read.payload.read_extents;
					read_delta.persisted_chunks_read =
						read.payload.delivered_chunks;
					read_delta.persisted_bytes_read =
						read.payload.delivered_bytes;
					read_delta.persisted_bulk_read_calls = read.bulk_read_calls;
					read_delta.persisted_planned_request_groups =
						read.planned_request_groups;
					read_delta.persisted_executed_request_groups =
						read.executed_request_groups;
					read_delta.persisted_range_scan_calls = read.range_scan_calls;
					read_delta.persisted_skip_budget_rows =
						read.payload.skip_budget_rows;
					read_delta.persisted_source_parent_rows_read =
						read.source_parent_rows_read;
					read_delta.persisted_source_catalog_rows_read =
						read.source_catalog_rows_read;
					read_delta.persisted_source_catalog_index_scans =
						read.source_catalog_index_scans;
					read_delta.persisted_source_fragment_rows_read =
						read.source_fragment_rows_read;
					read_delta.persisted_source_decoded_bp =
						read.source_decoded_bp;
					map_format6_read_evidence(read_delta, read);
					const auto record_format5_memory_evidence =
						[&read_delta](const auto& format5_reader) {
						const auto memory_evidence =
							format5_reader.memory_evidence();
						const std::uint64_t exact_position_bytes =
							bioseq::parallelism::saturating_mul(
								memory_evidence.retained_position_count,
								sizeof(std::uint64_t));
						const bool exact_position_image =
							exact_position_bytes
								!= (std::numeric_limits<std::uint64_t>::max)()
							&& memory_evidence.retained_position_capacity
								== memory_evidence.retained_position_count
							&& memory_evidence.global_position_capacity_bytes
								== exact_position_bytes;
						const bool retained_layout =
							memory_evidence.retained_layout_equation_holds()
							&& memory_evidence.owned_capacity_bytes
								== format5_reader.admitted_bytes();
						read_delta.format5_reader_capacity_accounting_passes =
							memory_evidence.capacity_accounting_passes;
						read_delta.
							format5_reader_global_position_storage_preparations =
							memory_evidence.global_position_storage_preparations;
						read_delta.
							format5_reader_global_position_storage_allocations =
							memory_evidence.global_position_storage_allocations;
						read_delta.
							format5_reader_position_append_growth_reservations =
							memory_evidence.position_append_growth_reservations;
						read_delta.format5_reader_retained_position_count =
							memory_evidence.retained_position_count;
						read_delta.format5_reader_retained_position_capacity =
							memory_evidence.retained_position_capacity;
						read_delta.format5_reader_directory_capacity_bytes =
							memory_evidence.directory_capacity_bytes;
						read_delta.format5_reader_query_hash_capacity_bytes =
							memory_evidence.query_hash_capacity_bytes;
						read_delta.format5_reader_query_record_capacity_bytes =
							memory_evidence.query_record_capacity_bytes;
						read_delta.format5_reader_ordinal_route_capacity_bytes =
							memory_evidence.ordinal_route_capacity_bytes;
						read_delta.format5_reader_selected_extent_capacity_bytes =
							memory_evidence.selected_extent_capacity_bytes;
						read_delta.
							format5_reader_current_selected_control_capacity_bytes =
							memory_evidence.current_selected_control_capacity_bytes;
						read_delta.format5_reader_global_position_capacity_bytes =
							memory_evidence.global_position_capacity_bytes;
						read_delta.format5_reader_control_payload_capacity_bytes =
							memory_evidence.control_payload_capacity_bytes;
						read_delta.format5_reader_owned_capacity_bytes =
							memory_evidence.owned_capacity_bytes;
						read_delta.format5_reader_admitted_bytes =
							format5_reader.admitted_bytes();
						read_delta.format5_reader_exact_position_image_checks = 1;
						read_delta.format5_reader_exact_position_image_failures =
							exact_position_image ? 0 : 1;
						read_delta.format5_reader_retained_layout_checks = 1;
						read_delta.format5_reader_retained_layout_failures =
							retained_layout ? 0 : 1;
						read_delta.format5_reader_memory_evidence_saturated =
							memory_evidence.saturated
							|| exact_position_bytes
								== (std::numeric_limits<std::uint64_t>::max)();
					};
					if (group.persisted_shape_v5_reader) {
						record_format5_memory_evidence(
							*group.persisted_shape_v5_reader);
					} else if (group.persisted_mps_shape_v5_reader) {
						record_format5_memory_evidence(
							*group.persisted_mps_shape_v5_reader);
					}
					operational.merge(read_delta);
					merge_cold_index_preparation_stats(
						operational, read.cold);
				}
				join_cpu_prepared_target.persisted_read_stats_published = true;
			}
			if (join_gpu_prepared_shape_v5_target.ready()
				&& !join_gpu_prepared_shape_v5_target
					.persisted_read_stats_published) {
				merge_cold_index_preparation_stats(
					operational, join_cold_policy_stats);
				merge_generation_read_stats(
					operational,
					join_gpu_prepared_shape_v5_target.persisted_read_stats);
				::bioseq::pipeline_stats::PipelineOperationalStats gpu_load{};
				gpu_load.gpu_persisted_resident_uploads = 1;
				gpu_load.gpu_buffer_creates = 3;
				gpu_load.gpu_memory_allocations = 3;
				operational.merge(gpu_load);
				join_gpu_prepared_shape_v5_target
					.persisted_read_stats_published = true;
			}
			if (join_cpu_prepared_target.unions_ready
				&& !join_cpu_prepared_target.groups.empty()
				&& !join_prepared_union_outcomes_recorded) {
				operational.cpu_prepared_unique_keys =
					join_cpu_prepared_target.total_unique_keys;
				operational.cpu_prepared_key_set_checksum =
					join_cpu_prepared_target.key_set_checksum;
				operational.cpu_prepared_key_set_checksum_present = true;
				join_prepared_union_outcomes_recorded = true;
			}
			const bool every_group_built =
				!join_cpu_prepared_target.groups.empty()
				&& std::all_of(
					join_cpu_prepared_target.groups.begin(),
					join_cpu_prepared_target.groups.end(),
					[](const auto& group) noexcept {
						return (group.target_index != nullptr
								|| group.persisted_grouped32_reader.has_value()
								|| group.persisted_shape_v5_reader.has_value()
								|| group.persisted_mps_shape_v5_reader.has_value()
								|| group.persisted_format6_reader.has_value())
							&& group.build_set_checksum_present
							&& group.build_set_checksum
								== group.key_set_checksum;
					});
			if (every_group_built
				&& !join_prepared_build_checksum_recorded) {
				operational.cpu_prepared_build_set_checksum =
					join_cpu_prepared_target.key_set_checksum;
				operational.cpu_prepared_build_set_checksum_present = true;
				join_prepared_build_checksum_recorded = true;
			}
            join_scanner_pg_cache->close_all(/*commit=*/false);
            bioseq::storage::destroy_seq_caches(join_scanner_pg_cache);
            join_scanner_pg_cache = nullptr;
            for (std::size_t i = 0; i < batch_size; ++i) {
                if (join_query_detoast_copies[i] != nullptr) {
                    pfree(join_query_detoast_copies[i]);
                    join_query_detoast_copies[i] = nullptr;
                }
            }

            join_match_map.clear();
            join_output_cursors.clear();
            for (const auto& [result_key, matches] : join_result_scratch) {
                (void)matches;
                const bool known = std::any_of(
                    join_queries.begin(), join_queries.end(),
                    [result_key](const JoinQueryRef& query) noexcept {
                        return query.ordinal
                            == static_cast<std::uint64_t>(result_key);
                    });
                if (result_key < 0 || !known) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqBatchJoin processor returned an unknown query id"),
                         errdetail("query_id=%d", result_key)));
                }
            }
            for (std::size_t query_index = 0;
                 query_index < join_queries.size(); ++query_index) {
                const std::uint64_t ordinal = join_queries[query_index].ordinal;
                auto found = join_result_scratch.find(
                    static_cast<std::int32_t>(ordinal));
                if (found == join_result_scratch.end()) {
                    continue;
                }
                auto [inserted, did_insert] = join_match_map.emplace(
                    ordinal, std::move(found->second));
                if (!did_insert) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqBatchJoin produced duplicate query ordinals")));
                }
                for (std::size_t match_index = 0;
                     match_index < inserted->second.size(); ++match_index) {
                    const auto& match = inserted->second[match_index];
                    if (base_params.do_cigar
                        && (!match.has_cigar || match.cigar.empty())) {
                        continue;
                    }
                    join_output_cursors.push_back(
                        JoinOutputCursor{query_index, match_index});
                }
            }
            join_output_position = 0;
            join_result_scratch.clear();
            join_target_outcomes_recorded = true;
          } catch (...) {
            cpp_error = std::current_exception();
          }
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            original_error = CopyErrorData();
            FlushErrorState();
        }
        PG_END_TRY();
        if (original_error != nullptr) {
            abandon_join_runtime_after_pg_error_noexcept();
            bioseq::parallelism::pg::end_batch(ticket, false);
            ErrorData* exact_error = original_error;
            throw bioseq::pg_utils::PostgresErrorAsCpp{exact_error};
        }
        if (cpp_error != nullptr) {
            abandon_join_runtime_after_pg_error_noexcept();
            bioseq::parallelism::pg::end_batch(ticket, false);
            std::rethrow_exception(cpp_error);
        }
        bioseq::parallelism::pg::end_batch(ticket, true);
        return true;
    }

    void finish_join_target() {
        if (!join_mode) {
            return;
        }
		if (join_gpu_prepared_shape_v5_target.resident != nullptr) {
			const bool gpu_equation_valid =
				join_gpu_prepared_shape_v5_target.ready()
				&& join_gpu_prepared_shape_v5_target.query_count != 0
				&& join_gpu_prepared_shape_v5_target.dispatched_query_count
					<= join_gpu_prepared_shape_v5_target.query_count
				&& (join_gpu_prepared_shape_v5_target.dispatched_query_count == 0
					? join_gpu_prepared_shape_v5_target.dispatch_waves == 0
					: join_gpu_prepared_shape_v5_target.dispatch_waves != 0);
			if (!gpu_equation_valid) {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("BioseqBatchJoin GPU Format-5 target lifecycle equation failed"),
					 errdetail("prepared_queries=%llu dispatched_queries=%llu waves=%llu ready=%s",
						static_cast<unsigned long long>(
							join_gpu_prepared_shape_v5_target.query_count),
						static_cast<unsigned long long>(
							join_gpu_prepared_shape_v5_target
								.dispatched_query_count),
						static_cast<unsigned long long>(
							join_gpu_prepared_shape_v5_target.dispatch_waves),
						join_gpu_prepared_shape_v5_target.ready()
							? "true" : "false")));
			}
		}
		std::uint64_t builds = 0;
		std::uint64_t reuses = 0;
		std::uint64_t duplicate_build_attempts = 0;
		std::uint64_t built_groups = 0;
		std::uint64_t reader_loads = 0;
		std::uint64_t reader_queries = 0;
		for (const auto& group : join_cpu_prepared_target.groups) {
			builds += group.target_index_builds;
			reuses += group.target_index_reuses;
			duplicate_build_attempts += group.duplicate_build_attempts;
			built_groups += group.target_index != nullptr ? 1u : 0u;
			reader_loads += group.persisted_reader_loads;
			reader_queries += group.persisted_reader_queries;
		}
		const bool reader_mode = reader_loads != 0;
		const std::uint64_t processed_queries = reader_mode
			? reader_queries : builds + reuses;
		const bool fully_processed = processed_queries
			== join_cpu_prepared_target.query_count;
		if ((built_groups != 0 || reader_loads != 0)
			&& (builds != built_groups
				|| builds + reader_loads
					!= join_cpu_prepared_target.physical_target_index_reads
				|| processed_queries > join_cpu_prepared_target.query_count
				|| (reader_mode
					&& (reader_loads != 1 || builds != 0 || reuses != 0))
				|| duplicate_build_attempts != 0
				|| (fully_processed
					&& join_cpu_prepared_target.query_count != 0
					&& !join_prepared_build_checksum_recorded))) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqBatchJoin target-wide CPU preparation equation failed"),
				 errdetail("builds=%llu built_groups=%llu reads=%llu reuses=%llu queries=%llu duplicate_build_attempts=%llu checksum_published=%s",
					static_cast<unsigned long long>(builds),
					static_cast<unsigned long long>(built_groups),
					static_cast<unsigned long long>(
						join_cpu_prepared_target.physical_target_index_reads),
					static_cast<unsigned long long>(reuses),
					static_cast<unsigned long long>(
						join_cpu_prepared_target.query_count),
					static_cast<unsigned long long>(duplicate_build_attempts),
					join_prepared_build_checksum_recorded ? "true" : "false")));
		}
		if (!fully_processed) {
			bioseq_log<DBG>(
				"[bioseq] BatchJoin prepared target finalized partial: "
				"processed_queries={} prepared_queries={} reader_mode={}",
				static_cast<unsigned long long>(processed_queries),
				static_cast<unsigned long long>(
					join_cpu_prepared_target.query_count),
				reader_mode);
		}
        join_queries.clear();
        join_match_map.clear();
        join_output_cursors.clear();
        join_output_position = 0;
        join_target_active = false;
        join_target_prepared = false;
        join_target_outcomes_recorded = false;
		join_prepared_union_outcomes_recorded = false;
		join_prepared_build_checksum_recorded = false;
		join_active_read_policy.reset();
		join_cold_policy_stats = {};
		join_cpu_prepared_target =
			::bioseq::simd::BatchProcessor::CpuPreparedTarget{};
		join_gpu_prepared_shape_v5_target =
			::bioseq::simd::BatchProcessor::
				GpuPreparedShapeV5Target{};
        clear_slots();
    }

    /// Zero-PostgreSQL failure abandonment for the CustomScan executor seam.
    /// The transaction/query ResourceOwners reclaim tuple slots, detoast
    /// copies, readers and MemoryContexts. This method only severs those PG
    /// handles and destroys ordinary C++ containers/permits after a bridged
    /// ERROR, so cleanup cannot replace the original ErrorData.
    void abandon_join_runtime_after_pg_error_noexcept() noexcept {
        if (join_scanner_pg_cache != nullptr) {
            auto* abandoned = std::exchange(join_scanner_pg_cache, nullptr);
            for (auto& [key, entry] : abandoned->entries) {
                (void)key;
                entry.rel = nullptr;
                entry.write_state = nullptr;
                entry.read_state = nullptr;
                entry.read_snapshot_source = nullptr;
                entry.pending_bytes_delta = 0;
                entry.pending_total_payload_delta = 0;
            }
            abandoned->entries.clear();
            abandoned->~seq_caches();
        }
        join_query_detoast_copies.fill(nullptr);
        join_query_rewidth_varlenas.clear();
        join_scanner_pg_ctx.reset();
        join_result_scratch.clear();
        join_queries.clear();
        join_match_map.clear();
        join_output_cursors.clear();
        join_output_position = 0;
        join_target_hdrs.fill(nullptr);
        join_query_hdrs.fill(nullptr);
        join_query_ids.fill(0);
        join_target_active = false;
        join_target_prepared = false;
        join_target_outcomes_recorded = false;
        join_prepared_union_outcomes_recorded = false;
        join_prepared_build_checksum_recorded = false;
		join_active_read_policy.reset();
        join_cold_policy_stats = {};
        join_cpu_prepared_target =
            ::bioseq::simd::BatchProcessor::
                CpuPreparedTarget{};
		join_gpu_prepared_shape_v5_target =
			::bioseq::simd::BatchProcessor::
				GpuPreparedShapeV5Target{};
        for (auto& entry : buffered_entries) {
            entry.scan_slot_copy = nullptr;
        }
        buffered_entries.clear();
        match_map.clear();
        output_cursors.clear();
        output_position = -1;
        for (auto& requests : per_bucket_target_requests) requests.clear();
        for (auto& requests : per_bucket_query_requests) requests.clear();
        fetch_bulk_sort_scratch.clear();
        hydrated_target_payloads.clear();
        hydrated_query_payloads.clear();
        ::abort_pg_sequence_read_session_noexcept(
            std::exchange(active_wave_read_session, nullptr));
        active_wave_permit.reset();
		slice_async_owner = nullptr;
		mmsearch_target_payload_observer = {};
		mmsearch_query_payload_observer = {};
        copy_staging_slot = nullptr;
        batch_payload_ctx = nullptr;
    }

    void reset_join_runtime_for_rescan() {
        join_queries.clear();
        join_match_map.clear();
        join_output_cursors.clear();
        join_output_position = 0;
        join_target_active = false;
        join_target_prepared = false;
        join_target_outcomes_recorded = false;
		join_prepared_union_outcomes_recorded = false;
		join_prepared_build_checksum_recorded = false;
		join_active_read_policy.reset();
		join_cold_policy_stats = {};
		join_cpu_prepared_target =
			::bioseq::simd::BatchProcessor::CpuPreparedTarget{};
		join_gpu_prepared_shape_v5_target =
			::bioseq::simd::BatchProcessor::
				GpuPreparedShapeV5Target{};
        clear_slots();
        last_stats = ::bioseq::pipeline_stats::PipelineExecutionStats{};
    }

    /*
     * Batch-phase transition. Called when the access method returns
     * TupIsNull. Invokes BatchProcessor::process_sequences on the
     * buffered target varlenas plus the stashed query varlena; stashes
     * the returned MatchResult map; builds output_cursors in insertion
     * order (one per MatchResult per row, skipping rows whose PK is
     * absent from the map). The query/target type dispatch happens
     * inside process_sequences via bioseq::pg_types::seq::dispatch_instance.
     */
    [[nodiscard]] bool run_batch_and_build_cursors() {
        bioseq::parallelism::pg::backend_sync();
        if (buffered_entries.size() > kDispatchBatchCapacity) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq attempted to dispatch an oversized tuple buffer"),
                 errdetail("buffered=%zu capacity=%zu",
                    buffered_entries.size(), kDispatchBatchCapacity)));
        }
        const size_t batch_size = buffered_entries.size();
        /*
         * DOC0371 \u00a75.2: this value is one completed dispatch-buffer delta.
         * Waves and stacked extra queries below merge into it, but a later
         * ReScan/batch must never replay an earlier delta into the statement
         * collector.
         */
        last_stats = ::bioseq::pipeline_stats::PipelineExecutionStats{};
        const bool executed_nonempty_buffer =
            mmsearch_context.active && batch_size != 0;
        const std::int64_t scheduled_queries =
            static_cast<std::int64_t>(batch_size)
            * static_cast<std::int64_t>(1 + extra_query_varlenas.size());
        const auto ticket =
            bioseq::parallelism::pg::begin_batch(scheduled_queries);
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* volatile pg_error = nullptr;
        std::exception_ptr cpp_error;
        PG_TRY();
        {
            try {
                run_batch_and_build_cursors_impl();
            } catch (...) {
                MemoryContextSwitchTo(saved_context);
                cpp_error = std::current_exception();
            }
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            pg_error = CopyErrorData();
            FlushErrorState();
        }
        PG_END_TRY();
        if (pg_error != nullptr) {
            bioseq::parallelism::pg::end_batch(ticket, false);
            ErrorData* exact_error = pg_error;
            throw bioseq::pg_utils::PostgresErrorAsCpp{exact_error};
        }
        if (cpp_error != nullptr) {
            bioseq::parallelism::pg::end_batch(ticket, false);
            std::rethrow_exception(cpp_error);
        }
        bioseq::parallelism::pg::end_batch(ticket, true);
        return executed_nonempty_buffer;
    }

    void run_batch_and_build_cursors_impl() {
        /*
         * DOC0294 — hydrate in row-span waves. A small batch naturally becomes
         * one wave; larger batches split at the current index-memory cap so
         * the dispatch phase does not force materialisation of rows that are
         * not active in the current wave.
         */
        std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity> target_hdrs{};
        std::array<const bioseq::pg_types::seq*, kDispatchBatchCapacity> per_row_query_hdrs{};
        std::array<int32_t,                      kDispatchBatchCapacity> query_ids{};

        const size_t batch_size = buffered_entries.size();
        if (!mmsearch_context.active || batch_size == 0) {
            match_map.clear();
            output_cursors.clear();
            output_position = 0;
            return;
        }

        /*
         * We use the buffer entry index `i` as the BatchProcessor's
         * key rather than the row's primary_key column. The PK from
         * the user's table is only available when the targetlist
         * includes a bioseq.<projection>(pk) FuncExpr (which is what
         * BeginCustomScan walks to discover `primary_key_attno`).
         * For queries without such a projection — e.g. plain
         * `SELECT t.pk FROM t WHERE mmsearch(t.seq, ...)` or
         * cross-table mmsearch joins where the qual is the only
         * bioseq surface — primary_key_attno stays
         * InvalidAttrNumber and every row would map to query_id=-1,
         * letting the std::map key collapse all matches into one
         * bucket. Using the entry index as the key keeps each row
         * uniquely identifiable inside the batch regardless of
         * targetlist shape.
         */
        std::vector<size_t> all_rows(batch_size);
        std::iota(all_rows.begin(), all_rows.end(), size_t{0});
        const bool batch_has_recipe = std::ranges::any_of(
            buffered_entries,
            [](const BufferedScanEntry& entry) noexcept {
                return entry.has_seq_recipe()
                    || entry.has_per_row_query_recipe();
            });
        const bool forced_flat_baseline =
            (bioseq::settings::test_force_inmemory_scanner
                || bioseq::settings::test_force_chunked_scanner)
            && !batch_has_recipe;
        const bool vulkan_requested =
            ::bioseq::simd::pipe::
                vulkan_components_requested(base_params);
        if (vulkan_requested
            && !::bioseq::simd::pipe::
                complete_all_gpu_tuple_selected(base_params)) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("Vulkan aligner execution failed"),
                 errdetail("Vulkan complete-tuple admission: requested Vulkan components do not form the complete supported all-GPU tuple; generic CPU/SIMD biological fallback is forbidden"),
                 errhint("Selected Vulkan execution has no CPU fallback; fix the Vulkan runtime or select a non-Vulkan algorithm before dispatch.")));
        }
        /* Selected Vulkan consumes source-backed inputs through typed lazy
         * views and owner-bearing admitted_packed_region uploads. It must not
         * ask the tuple buffer to EOH-flatten locators or recipes. */
        const bool requires_flat_sequence_payloads = forced_flat_baseline;

        std::size_t lpu_for_hydration = 0;
        std::size_t bytes_per_unit_for_hydration = 0;
        bioseq::pg_types::seq::dispatch_instance(
            mmsearch_context.alphabet_code,
            mmsearch_context.storage_unit_byte_width,
            [&lpu_for_hydration, &bytes_per_unit_for_hydration]<class S>() noexcept {
                using traits = typename compact_sequence_range<S>::traits;
                lpu_for_hydration = traits::letters_per_unit;
                bytes_per_unit_for_hydration =
                    sizeof(typename traits::storage_unit);
            });
        std::size_t query_lpu_for_hydration = lpu_for_hydration;
        std::size_t query_bytes_per_unit_for_hydration =
            bytes_per_unit_for_hydration;
        if (per_row_mode()) {
            bioseq::pg_types::seq::dispatch_instance(
                mmsearch_context.query_alphabet_code,
                mmsearch_context.query_storage_unit_byte_width,
                [&query_lpu_for_hydration,
                 &query_bytes_per_unit_for_hydration]<class S>() noexcept {
                    using traits =
                        typename compact_sequence_range<S>::traits;
                    query_lpu_for_hydration = traits::letters_per_unit;
                    query_bytes_per_unit_for_hydration =
                        sizeof(typename traits::storage_unit);
                });
        }
        auto estimate_flat_sequence_bytes =
            [](const std::uint64_t bp_length,
               const std::size_t letters_per_unit,
               const std::size_t bytes_per_unit)
                -> std::uint64_t {
            if (letters_per_unit == 0 || bytes_per_unit == 0) {
                return 0;
            }
            const std::uint64_t total_units =
                bp_length / letters_per_unit
                + static_cast<std::uint64_t>(
                    bp_length % letters_per_unit != 0u);
            if (total_units
                > (std::numeric_limits<std::uint64_t>::max)()
                    / static_cast<std::uint64_t>(bytes_per_unit)) {
                return 0;
            }
            const std::uint64_t payload_bytes =
                total_units
                * static_cast<std::uint64_t>(bytes_per_unit);
            if (payload_bytes
                > (std::numeric_limits<std::uint64_t>::max)()
                    - static_cast<std::uint64_t>(bioseq::pg_types::seq::header_bytes())) {
                return 0;
            }
            const std::uint64_t varlena_bytes =
                static_cast<std::uint64_t>(bioseq::pg_types::seq::header_bytes())
                + payload_bytes;
            if (varlena_bytes > static_cast<std::uint64_t>(MaxAllocSize)) {
                return 0;
            }
            return varlena_bytes;
        };
        auto estimate_hydration_bytes =
            [this, requires_flat_sequence_payloads,
             lpu_for_hydration, bytes_per_unit_for_hydration,
             query_lpu_for_hydration,
             query_bytes_per_unit_for_hydration,
             &estimate_flat_sequence_bytes](size_t row) -> std::uint64_t {
            if (!requires_flat_sequence_payloads) {
                return 0u;
            }
            if (row >= buffered_entries.size()) {
                return 0;
            }
            const BufferedScanEntry& entry = buffered_entries[row];
            std::uint64_t bytes = 0;
            if (entry.seq_loc.has_value()) {
                const std::uint64_t target_bytes =
                    estimate_flat_sequence_bytes(
                    entry.seq_loc->total_bp_size,
                    lpu_for_hydration,
                    bytes_per_unit_for_hydration);
                bytes = target_bytes;
            }
            if (entry.per_row_query_loc.has_value()) {
                const std::uint64_t query_bytes =
                    estimate_flat_sequence_bytes(
                        entry.per_row_query_loc->total_bp_size,
                        query_lpu_for_hydration,
                        query_bytes_per_unit_for_hydration);
                if (query_bytes
                    > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
                    return (std::numeric_limits<std::uint64_t>::max)();
                }
                bytes += query_bytes;
                if (mmsearch_context.query_storage_unit_byte_width
                        != mmsearch_context.storage_unit_byte_width) {
                    const std::uint64_t rewidthed_query_bytes =
                        estimate_flat_sequence_bytes(
                            entry.per_row_query_loc->total_bp_size,
                            lpu_for_hydration,
                            bytes_per_unit_for_hydration);
                    if (rewidthed_query_bytes
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - bytes) {
                        return (std::numeric_limits<std::uint64_t>::max)();
                    }
                    bytes += rewidthed_query_bytes;
                }
            }
            return bytes;
        };
        auto refresh_row_arrays =
            [this, &target_hdrs, &per_row_query_hdrs, &query_ids](std::span<const size_t> rows) {
            for (const size_t i : rows) {
                if (i >= buffered_entries.size() || i >= kDispatchBatchCapacity) {
                    continue;
                }
                const BufferedScanEntry& entry = buffered_entries[i];
                target_hdrs[i] = entry.seq_hdr();  // nullptr for NULL seq
                per_row_query_hdrs[i] = entry.per_row_query_hdr();
                query_ids[i] = static_cast<int32_t>(i);
            }
        };

        std::vector<std::pair<size_t, size_t>> waves;
        const std::uint64_t budget =
            bioseq::parallelism::MemoryAdmissionController::instance()
                .allocatable_after_reservations();
        std::uint64_t wave_bytes = 0;
        size_t wave_begin = 0;
        for (size_t i = 0; i < batch_size; ++i) {
            const std::uint64_t row_bytes = estimate_hydration_bytes(i);
            if (requires_flat_sequence_payloads
                && row_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()) {
                ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("selected dispatch cannot materialize sequence recipe"),
                     errdetail("row=%zu requires an invalid or oversized flat payload",
                        i)));
            }
            const bool split =
                budget != (std::numeric_limits<std::uint64_t>::max)()
                && wave_bytes != 0
                && row_bytes > 0
                && row_bytes > budget - std::min(wave_bytes, budget);
            if (split) {
                waves.emplace_back(wave_begin, i);
                wave_begin = i;
                wave_bytes = 0;
            }
            if (row_bytes > (std::numeric_limits<std::uint64_t>::max)() - wave_bytes) {
                wave_bytes = (std::numeric_limits<std::uint64_t>::max)();
            } else {
                wave_bytes += row_bytes;
            }
        }
        if (batch_size > 0) {
            waves.emplace_back(wave_begin, batch_size);
        }

        const bioseq::pg_types::seq* query_hdr = mmsearch_context.query_hdr();
        const uint32_t query_bp_size = query_hdr != nullptr ? query_hdr->bp_size : 0;
        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer calling BatchProcessor::process_sequences batch_size={} typmod=({},{}) query_bp_size={} read_index={} per_row_mode={}",
                batch_size,
                static_cast<unsigned>(mmsearch_context.alphabet_code),
                static_cast<unsigned>(mmsearch_context.storage_unit_byte_width),
                query_bp_size,
                read_sequence_index_active,
                per_row_mode());

        // DOC0264 §9.2 / DOC0274 — wire scanner_pg into the dispatch loop.
        // Allocates a fresh seq_caches for scanner_pg's PG-backed reads.
        // Lives in batch_payload_ctx so palloc'd state survives the call.
        if (batch_payload_ctx == nullptr) {
            batch_payload_ctx = AllocSetContextCreate(executor_query_mctx,
                "TupleDispatchBuffer payload arena", ALLOCSET_DEFAULT_SIZES);
        }
        bioseq::storage::seq_caches* scanner_pg_cache =
            bioseq::storage::alloc_seq_caches(batch_payload_ctx);
        scanner_pg_cache->side                = bioseq::storage::seq_caches::Side::Read;
        scanner_pg_cache->lock_mode           = AccessShareLock;
        scanner_pg_cache->read_state_mctx     = batch_payload_ctx;
        scanner_pg_cache->main_database_oid   = user_main_database_oid;
        scanner_pg_cache->main_tablespace_oid = user_main_tablespace_oid;
        scanner_pg_cache->main_relfilenode    = user_main_relfilenode;

        typename ::bioseq::simd::BatchProcessor::ScannerPgContext scanner_pg_ctx{};
        scanner_pg_ctx.async_job_observer =
            make_slice_async_pool_observer_noexcept(slice_async_owner);
        scanner_pg_ctx.mmsearch_target_payload_observer =
            mmsearch_target_payload_observer;
        scanner_pg_ctx.mmsearch_query_payload_observer =
            mmsearch_query_payload_observer;
        scanner_pg_ctx.db_oid             = user_main_database_oid;
        scanner_pg_ctx.tablespace_oid     = user_main_tablespace_oid;
        scanner_pg_ctx.main_relfilenode   = user_main_relfilenode;
        scanner_pg_ctx.mmsearch_seq_attno =
            static_cast<std::int16_t>(mmsearch_context.seq_attno);
        scanner_pg_ctx.mmsearch_query_attno =
            static_cast<std::int16_t>(query_source_attno());
        scanner_pg_ctx.query_alphabet_code =
            mmsearch_context.query_alphabet_code;
        scanner_pg_ctx.query_storage_unit_byte_width =
            mmsearch_context.query_storage_unit_byte_width;
        scanner_pg_ctx.caches             = scanner_pg_cache;
        scanner_pg_ctx.parent_mctx        = batch_payload_ctx;
        scanner_pg_ctx.snapshot           = scan_snapshot;
        scanner_pg_ctx.source_provider    = source_provider;
        scanner_pg_ctx.persisted_gpu_operation_evidence_sink =
            persisted_gpu_operation_evidence_sink;
        // Populate per-row locators for the target column. The production
        // scanner_pg path uses these. Last-fragment padding is recomputed
        // inside process_sequences where the template parameter S (and thus
        // its letters_per_unit) is available.
        for (size_t i = 0; i < batch_size; ++i) {
            const auto& seq_loc_opt = buffered_entries[i].seq_loc;
            if (seq_loc_opt.has_value()) {
                scanner_pg_ctx.target_locators[i] = *seq_loc_opt;
            }
            const auto& query_loc_opt =
                buffered_entries[i].per_row_query_loc;
            if (query_loc_opt.has_value()) {
                scanner_pg_ctx.query_locators[i] = *query_loc_opt;
            }
            if (buffered_entries[i].has_seq_recipe()) {
                scanner_pg_ctx.target_recipe_datums[i] =
                    buffered_entries[i].seq_recipe_datum;
                scanner_pg_ctx.target_recipe_descriptors[i] =
                    buffered_entries[i].seq_recipe_descriptor;
            }
            if (buffered_entries[i].has_per_row_query_recipe()) {
                scanner_pg_ctx.query_recipe_datums[i] =
                    buffered_entries[i].per_row_query_recipe_datum;
                scanner_pg_ctx.query_recipe_descriptors[i] =
                    buffered_entries[i].per_row_query_recipe_descriptor;
            }
            // DOC0279 §11.3c — surface the per-row columnar rowno (the index
            // query_key) so the stored per-fragment index can be assembled.
            scanner_pg_ctx.target_rownos[i] = buffered_entries[i].sequence_rowno;
        }
        scanner_pg_ctx.user_relid        = user_relid;
        scanner_pg_ctx.index_read_requirement = index_read_requirement;
        scanner_pg_ctx.read_index_active = read_sequence_index_active;
        scanner_pg_ctx.index_async_persist_active = bioseq::settings::index_async_persist;
        scanner_pg_ctx.recompute_async_persist_active =
            bioseq::settings::recompute_async_persist;
		// DOC0279 §11.3/§12.3 — resolve the target column's active minimizer
		// policy ONCE per batch. Stored-index reads use it when
		// read_index_active is true; no-index recompute also uses it to select
		// the transient MPF/MPS index shape without reading stored chunks.
		// The active-policy catalog row selects the transient MPF/MPS shape
		// even when payload reads are forbidden. Resolving policy metadata is
		// not a persisted target-index payload probe and must not mutate the
		// existing no-index algorithm contract.
		install_active_read_policy_dispatch_proof(
			scanner_pg_ctx,
			bioseq::storage::indexing::resolve_active_read_index_outcome(
				*executor_read_context,
				static_cast<std::int16_t>(mmsearch_context.seq_attno)),
			user_relid, user_main_relfilenode,
			static_cast<std::int16_t>(mmsearch_context.seq_attno));
        auto reset_dispatch_arrays =
            [&target_hdrs, &per_row_query_hdrs, &query_ids] {
            target_hdrs.fill(nullptr);
            per_row_query_hdrs.fill(nullptr);
            query_ids.fill(-1);
        };
        auto reset_scanner_wave =
            [&scanner_pg_ctx] {
            scanner_pg_ctx.target_locators.fill(std::nullopt);
            scanner_pg_ctx.query_locators.fill(std::nullopt);
            scanner_pg_ctx.target_recipe_datums.fill(Datum{0});
            scanner_pg_ctx.query_recipe_datums.fill(Datum{0});
            scanner_pg_ctx.target_recipe_descriptors.fill(
                pg_sequence_descriptor{});
            scanner_pg_ctx.query_recipe_descriptors.fill(
                pg_sequence_descriptor{});
            scanner_pg_ctx.target_rownos.fill(-1);
            scanner_pg_ctx.target_index_sources.fill(
                ::bioseq::pipeline_stats::TargetIndexSource::none);
        };
        auto install_scanner_wave =
            [this, &scanner_pg_ctx](std::span<const size_t> rows) {
            for (const size_t row : rows) {
                if (row >= buffered_entries.size() || row >= kDispatchBatchCapacity) {
                    continue;
                }
                const auto& seq_loc_opt = buffered_entries[row].seq_loc;
                if (seq_loc_opt.has_value()) {
                    scanner_pg_ctx.target_locators[row] = *seq_loc_opt;
                }
                const auto& query_loc_opt =
                    buffered_entries[row].per_row_query_loc;
                if (query_loc_opt.has_value()) {
                    scanner_pg_ctx.query_locators[row] = *query_loc_opt;
                }
                if (buffered_entries[row].has_seq_recipe()) {
                    scanner_pg_ctx.target_recipe_datums[row] =
                        buffered_entries[row].seq_recipe_datum;
                    scanner_pg_ctx.target_recipe_descriptors[row] =
                        buffered_entries[row].seq_recipe_descriptor;
                }
                if (buffered_entries[row].has_per_row_query_recipe()) {
                    scanner_pg_ctx.query_recipe_datums[row] =
                        buffered_entries[row].per_row_query_recipe_datum;
                    scanner_pg_ctx.query_recipe_descriptors[row] =
                        buffered_entries[row].per_row_query_recipe_descriptor;
                }
                scanner_pg_ctx.target_rownos[row] = buffered_entries[row].sequence_rowno;
            }
        };
        auto wave_estimated_bytes =
            [&estimate_hydration_bytes](std::span<const size_t> rows) -> std::uint64_t {
            std::uint64_t bytes = 0;
            for (const size_t row : rows) {
                const std::uint64_t row_bytes = estimate_hydration_bytes(row);
                if (row_bytes > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
                    return (std::numeric_limits<std::uint64_t>::max)();
                }
                bytes += row_bytes;
            }
            return bytes;
        };
        auto wave_has_source_facade =
            [this](std::span<const size_t> rows) noexcept {
                for (const size_t row : rows) {
                    if (row >= buffered_entries.size()) continue;
                    const auto& entry = buffered_entries[row];
                    if (entry.has_seq_recipe()
                        || entry.has_per_row_query_recipe()
                        || entry.seq_loc.has_value()
                        || entry.per_row_query_loc.has_value()) {
                        return true;
                    }
                }
                return false;
            };
        auto merge_stats =
            [this](const ::bioseq::pipeline_stats::PipelineExecutionStats& wave_stats) {
            last_stats.merge(wave_stats);
        };

        match_map.clear();
        // Pointer to the context.
        auto* scanner_pg_arg = &scanner_pg_ctx;
        ::abort_pg_sequence_read_session_noexcept(
            std::exchange(active_wave_read_session, nullptr));
        active_wave_permit.reset();
        if (copy_staging_slot != nullptr) {
            ExecDropSingleTupleTableSlot(copy_staging_slot);
            copy_staging_slot = nullptr;
        }
        const auto clear_active_wave_member_handles_noexcept =
            [this]() noexcept {
                for (auto& entry : buffered_entries) {
                    entry.hydrated_seq_hdr = nullptr;
                    entry.hydrated_per_row_query_hdr = nullptr;
                }
                hydrated_target_payloads.clear();
                hydrated_query_payloads.clear();
            };
        const auto release_active_wave_after_error_noexcept = [&]() noexcept {
            scanner_pg_ctx.read_session = nullptr;
            ::abort_pg_sequence_read_session_noexcept(
                std::exchange(active_wave_read_session, nullptr));
            clear_active_wave_member_handles_noexcept();
            target_hdrs.fill(nullptr);
            per_row_query_hdrs.fill(nullptr);
            active_wave_permit.reset();
        };
        const auto release_active_wave_checked = [&]() {
            if (active_wave_read_session != nullptr) {
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq dispatch wave session was not finished")));
            }
            clear_active_wave_member_handles_noexcept();
            target_hdrs.fill(nullptr);
            per_row_query_hdrs.fill(nullptr);
            active_wave_permit.reset();
        };
        const auto abandon_scanner_cache_noexcept = [&]() noexcept {
            release_active_wave_after_error_noexcept();
            if (scanner_pg_cache == nullptr) {
                return;
            }
            for (auto& [key, entry] : scanner_pg_cache->entries) {
                (void)key;
                entry.rel = nullptr;
                entry.write_state = nullptr;
                entry.read_state = nullptr;
                entry.read_snapshot_source = nullptr;
                entry.pending_bytes_delta = 0;
                entry.pending_total_payload_delta = 0;
            }
            scanner_pg_cache->entries.clear();
            scanner_pg_cache->~seq_caches();
            scanner_pg_cache = nullptr;
        };
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* volatile pg_error = nullptr;
        std::exception_ptr cpp_error;

        PG_TRY();
        {
          try {
            for (const auto& wave : waves) {
                const std::span<const size_t> rows{
                    all_rows.data() + wave.first, wave.second - wave.first};
                const std::uint64_t estimated_wave_bytes = wave_estimated_bytes(rows);
                active_wave_permit =
                    bioseq::parallelism::MemoryAdmissionController::instance()
                        .try_reserve(estimated_wave_bytes);
                const bool hydrate_wave =
                    estimated_wave_bytes == 0
                    || active_wave_permit.has_value();
                if (!hydrate_wave) {
                    bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer wave [{}, {}) was not admitted for {} bytes; target locators stream through scanner_pg",
                            wave.first, wave.second,
                            static_cast<unsigned long long>(estimated_wave_bytes));
                }
                reset_dispatch_arrays();
                reset_scanner_wave();
                if (hydrate_wave) {
                    if (forced_flat_baseline) {
                        hydrate_locators_by_bucket(rows);
                    }
                }
                refresh_row_arrays(rows);
                install_scanner_wave(rows);
                if (wave_has_source_facade(rows)) {
                    if (active_wave_read_session != nullptr) {
                        ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("bioseq dispatch retained a stale read session")));
                    }
                    active_wave_read_session =
                        ::begin_pg_sequence_read_session(batch_payload_ctx);
                    scanner_pg_ctx.read_session =
                        active_wave_read_session;
                }

                ::bioseq::pipeline_stats::PipelineExecutionStats wave_stats{};
                std::map<int32_t, std::vector<::bioseq::simd::MatchResult>> wave_match_map;
                if (per_row_mode()) {
                    wave_match_map = processor.process_sequences(
                        batch_size,
                        mmsearch_context.alphabet_code,
                        mmsearch_context.storage_unit_byte_width,
                        nullptr,
                        &per_row_query_hdrs,
                        target_hdrs, query_ids,
                        base_params,
                        ::bioseq::simd::BatchInvocationRole::primary,
                        0,
                        wave_stats,
                        scanner_pg_arg);
                } else {
                    wave_match_map = processor.process_sequences(
                        batch_size,
                        mmsearch_context.alphabet_code,
                        mmsearch_context.storage_unit_byte_width,
                        query_hdr,
                        nullptr,
                        target_hdrs, query_ids,
                        base_params,
                        ::bioseq::simd::BatchInvocationRole::primary,
                        0,
                        wave_stats,
                        scanner_pg_arg);
                }
                merge_stats(wave_stats);

                /*
                 * For every extra mmsearch qual the caller registered we run
                 * an independent process_sequences pass over the same active
                 * wave, then logically AND the matches into the wave map.
                 */
                for (size_t extra_idx = 0;
                     extra_idx < extra_query_varlenas.size(); ++extra_idx) {
                    const std::vector<char>& extra_bytes =
                        extra_query_varlenas[extra_idx];
                    if (extra_bytes.empty()) {
                        continue;
                    }
                    const bioseq::pg_types::seq* extra_hdr =
                        reinterpret_cast<const bioseq::pg_types::seq*>(extra_bytes.data());
                    ::bioseq::pipeline_stats::PipelineExecutionStats extra_stats{};
                    auto extra_match_map =
                        processor.process_sequences(
                            batch_size,
                            mmsearch_context.alphabet_code,
                            mmsearch_context.storage_unit_byte_width,
                            extra_hdr,
                            nullptr,
                            target_hdrs, query_ids,
                            base_params,
                            ::bioseq::simd::BatchInvocationRole::extra_query,
                            extra_broadcast_query_ordinal(
                                per_row_mode(), batch_size, extra_idx),
                            extra_stats,
                            scanner_pg_arg);
                    merge_stats(extra_stats);
                    bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer extra-query #{} wave [{}, {}) produced matches for {} entries",
                            extra_idx, wave.first, wave.second,
                            extra_match_map.size());
                    for (auto it = wave_match_map.begin();
                         it != wave_match_map.end();) {
                        if (extra_match_map.find(it->first)
                            == extra_match_map.end()) {
                            it = wave_match_map.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }

                for (auto& [entry_id, matches] : wave_match_map) {
                    match_map[entry_id] = std::move(matches);
                }
                if (active_wave_read_session != nullptr) {
                    ::finish_pg_sequence_read_session(
                        active_wave_read_session, true);
                    active_wave_read_session = nullptr;
                    scanner_pg_ctx.read_session = nullptr;
                }
                release_active_wave_checked();
            }

            // DOC0274 — release scanner_pg's per-batch seq_caches now that all
            // process_sequences calls have returned. close_all releases the
            // sub-table relations and their BioseqColumnReadStates;
            // destroy_seq_caches returns the slab to the parent context.
            scanner_pg_cache->close_all(/*commit=*/false);
            bioseq::storage::destroy_seq_caches(scanner_pg_cache);
            scanner_pg_cache = nullptr;
          } catch (...) {
            MemoryContextSwitchTo(saved_context);
            cpp_error = std::current_exception();
          }
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            pg_error = CopyErrorData();
            FlushErrorState();
        }
        PG_END_TRY();
        if (pg_error != nullptr) {
            abandon_scanner_cache_noexcept();
            ErrorData* exact_error = pg_error;
            throw bioseq::pg_utils::PostgresErrorAsCpp{exact_error};
        }
        if (cpp_error != nullptr) {
            abandon_scanner_cache_noexcept();
            std::rethrow_exception(cpp_error);
        }

        output_cursors.clear();
        for (size_t i = 0; i < buffered_entries.size(); ++i) {
            const BufferedScanEntry& entry = buffered_entries[i];
            /*
             * Match map is keyed on the entry index (see comment in
             * the loop above that built `query_ids[i] = i`). Skip
             * entries whose index is not in the map — those rows had
             * no usable target sequence (NULL seq column) or the
             * aligner produced zero matches; either way `WHERE
             * mmsearch(...)` evaluates to FALSE for them.
             */
            auto hit = match_map.find(static_cast<int32_t>(i));
            if (hit == match_map.end()) {
                bioseq_log<DBG>("[bioseq] pg: batch miss for entry index={}; WHERE mmsearch evaluates to FALSE",
                        i);
                continue;
            }
            /*
             * Filter degenerate matches. The aligner can return a
             * MatchResult that has a chain pinned in the seed pass
             * but no usable base alignment (`has_cigar == false` or
             * `cigar` empty). When the user requested a CIGAR via
             * `bioseq.cigar(...)`, an empty/no-cigar match is the
             * fuzzy-search equivalent of "no real hit": surfacing it
             * as a row would mean WHERE mmsearch(...) acts more like
             * a tautology than a filter, which contradicts the
             * substring-match semantics users expect of mmsearch.
             *
             * Skipping these here filters them out at output-cursor
             * build time; the row never reaches the projection
             * pipeline, so its bioseq.cigar / bioseq.target_start /
             * etc. would have come back as the placeholder Const's
             * default-NULL state — exactly the kind of "row but
             * empty fields" output the user reported as undesired.
             *
             * When `do_cigar` is false (the user only projects e.g.
             * bioseq.target_start / bioseq.score), we don't have a
             * cigar to inspect; in that case we keep every match
             * the chainer surfaced because the chain itself is
             * meaningful (passed `min_seed_count` / `min_chain_score`).
             */
            for (size_t m = 0; m < hit->second.size(); ++m) {
                const ::bioseq::simd::MatchResult& match_candidate = hit->second[m];
                if (base_params.do_cigar
                    && (!match_candidate.has_cigar
                        || match_candidate.cigar.empty())) {
                    bioseq_log<DBG>("[bioseq] pg: batch skip: degenerate match for entry index={} cursor m={} (do_cigar=true but cigar empty)",
                            i, m);
                    continue;
                }
                output_cursors.push_back({ i, m });
            }
        }
        output_position = 0;
        bioseq_log<DBG>("[bioseq] pg: batch produced {} output tuples from {} scan tuples",
                output_cursors.size(), buffered_entries.size());
    }

    /*
     * Reading-phase iterator. nullopt means only genuine cursor exhaustion.
     * A cursor whose entry or match no longer exists is internal corruption,
     * not EOF, and therefore raises ERROR fail closed.
     */
    [[nodiscard]] std::optional<NextOutput> read_next_output() {
        if (join_mode) {
            const CursorBoundary boundary = classify_output_cursor(
                join_output_position, join_output_cursors.size());
            if (boundary == CursorBoundary::exact_eof) {
                return std::nullopt;
            }
            if (boundary == CursorBoundary::corrupt) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqBatchJoin output cursor position is corrupt"),
                     errdetail("position=%zu size=%zu",
                        join_output_position, join_output_cursors.size())));
            }
            const JoinOutputCursor cursor =
                join_output_cursors[join_output_position++];
            if (buffered_entries.size() != 1
                || cursor.query_index >= join_queries.size()) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqBatchJoin output cursor references missing input")));
            }
            const std::uint64_t ordinal =
                join_queries[cursor.query_index].ordinal;
            auto found = join_match_map.find(ordinal);
            if (found == join_match_map.end()
                || cursor.match_index >= found->second.size()) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqBatchJoin output cursor references a missing match")));
            }
            return NextOutput{
                &buffered_entries.front(),
                ordinal,
                &found->second[cursor.match_index],
            };
        }
        const CursorBoundary boundary = classify_output_cursor(
            static_cast<std::int64_t>(output_position), output_cursors.size());
        if (boundary == CursorBoundary::exact_eof) {
            return std::nullopt;
        }
        if (boundary == CursorBoundary::corrupt) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq output cursor position is corrupt"),
                 errdetail("position=%d size=%zu",
                    output_position, output_cursors.size())));
        }
        const OutputCursor& cursor = output_cursors[output_position];
        output_position++;
        if (cursor.scan_entry_index >= buffered_entries.size()) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq output cursor references a missing scan entry"),
                 errdetail("entry=%zu buffered=%zu",
                    cursor.scan_entry_index, buffered_entries.size())));
        }
        BufferedScanEntry& entry = buffered_entries[cursor.scan_entry_index];
        auto hit = match_map.find(static_cast<int32_t>(cursor.scan_entry_index));
        if (hit == match_map.end() || cursor.match_index >= hit->second.size()) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq output cursor references a missing alignment match"),
                 errdetail("entry=%zu match=%zu available=%zu",
                    cursor.scan_entry_index,
                    cursor.match_index,
                    hit == match_map.end() ? std::size_t{0} : hit->second.size())));
        }
        return NextOutput{ &entry, 0, &hit->second[cursor.match_index] };
    }

    /*
     * True once run_batch_and_build_cursors has been called for the
     * current scan. Inner_CustomExecScan uses this to switch from the
     * writing-phase access-method loop to the reading-phase drain loop.
     */
    [[nodiscard]] constexpr bool has_finished_writing() const {
        return output_position >= 0;
    }

    /*
     * True when BeginCustomScan found a WHERE mmsearch(...) qual and
     * installed its (seq_attno, query_varlena, typmod) context. When
     * false the custom scan must behave like a pass-through scan:
     * fetch rows, project them directly via the (VarInjectorMutator-
     * rewritten) target list, and return them without buffering. The
     * buffering + batch pipeline is only meaningful when there is a
     * WHERE mmsearch(...) to drive it. See SPEC 2 §6.8 for the
     * historical gap this accessor closes.
     */
    [[nodiscard]] constexpr bool mmsearch_active() const {
        return mmsearch_context.active;
    }

    [[nodiscard]] size_t buffered_count() const {
        return buffered_entries.size();
    }

    [[nodiscard]] size_t output_count() const {
        return output_cursors.size();
    }

    /*
     * Reset writing/reading state so a new scan can start. Frees the
     * MinimalTuple slots allocated in submit_scan; leaves
     * mmsearch_context untouched so callers can reuse configuration.
     */
    [[gnu::always_inline]] inline void clear_slots() {
        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer::clear_slots() entered; freeing {} slots, {} outputs",
                buffered_entries.size(), output_cursors.size());
        for (auto& entry : buffered_entries) {
            if (entry.scan_slot_copy != nullptr) {
                ExecDropSingleTupleTableSlot(entry.scan_slot_copy);
                entry.scan_slot_copy = nullptr;
            }
        }
        buffered_entries.clear();
        match_map.clear();
        output_cursors.clear();
        output_position = -1;
        ::abort_pg_sequence_read_session_noexcept(
            std::exchange(active_wave_read_session, nullptr));
        active_wave_permit.reset();
        if (copy_staging_slot != nullptr) {
            ExecDropSingleTupleTableSlot(copy_staging_slot);
            copy_staging_slot = nullptr;
        }
        /*
         * DOC0260 §9.5 — the bulk-fetched payloads (and the per-row query
         * payloads) live in batch_payload_ctx and are valid until exactly
         * here: clear_slots() runs after the batch's output has been drained
         * and before the next buffering cycle (e.g. a Nested Loop rescan), so
         * resetting now reclaims the previous batch's payload arena without
         * dangling any hydrated_seq_hdr the just-cleared entries referenced.
         */
        if (batch_payload_ctx != nullptr) {
            MemoryContextReset(batch_payload_ctx);
        }
    }

    /*
     * Total-reset shorthand: drop every buffered slot, the
     * mmsearch primary AND every registered Const-extra. Equivalent
     * to "as if the buffer had never been configured". This is
     * stricter than `clear_mmsearch_context()` (which preserves
     * extras across rescans by default — see §9.4 / §9-review fix).
     * Kept for symmetry with the pre-§9 API; not used by any live
     * caller today, but the explicit `true` makes the contract
     * loud rather than relying on the default's semantics.
     */
    [[gnu::always_inline]] inline void clear() {
        clear_slots();
        clear_mmsearch_context(/*clear_extras=*/true);
    }
};

static_assert(noexcept(
    std::declval<TupleDispatchBuffer&>()
        .abandon_join_runtime_after_pg_error_noexcept()));

} // namespace bioseq::scheduler
