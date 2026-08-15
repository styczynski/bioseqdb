/**
 * @file alignment_stats.cppm
 * @brief Alignment execution statistics formatting and SQL row materialization.
 */
module;

#include <bioseq/pg_compat.h>
#include "fmt/format.h"

extern "C" {
#include "utils/builtins.h"
#include "utils/jsonb.h"
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module bioseq.postgres.extension;

import bioseq.postgres.extension.alignment.sql_rows;
import bioseq.pipeline.stats;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.table.alignment_execution;

namespace bioseq::pg::detail {

/** @brief Formats an unsigned value as 16 lower-case hexadecimal digits. */
[[nodiscard]] auto fixed_hex_u64(std::uint64_t value) -> std::string {
    constexpr auto digits = std::to_array("0123456789abcdef");
    static_assert(digits.size() == 17u);
    static_assert(sizeof(digits) == sizeof("0123456789abcdef"));
    static_assert(alignof(decltype(digits)) == alignof(char));
    std::string result(16, '0');
	for (std::size_t i = 0; i < result.size(); ++i) {
		result[result.size() - i - 1] = digits[value & 0x0fU];
		value >>= 4U;
	}
	return result;
}

} // namespace bioseq::pg::detail

namespace {

namespace alignment_exec = bioseq::postgres::table::alignment_execution;
using bioseq::pg::detail::fixed_hex_bytes;
using bioseq::pg::detail::fixed_hex_u64;

inline constexpr int alignment_stats_column_count = 151;

[[nodiscard]] auto
cold_index_preparation_json(const bioseq::pipeline_stats::pipeline_operational_stats& stats)
    -> std::string {
    /*
	 * Keep the cold-read diagnostic surface self-contained
	 * without permanently widening the already-large SRF by one column per
	 * counter.  Values are saturated before reaching this boundary, so every
	 * JSON number remains representable as a SQL bigint. Head/directory/range
	 * scan counts and TOAST/detoast counts remain in their pre-existing scalar
	 * SRF columns; duplicating those five values here would create two public
	 * names for the same measurement. are_stats_saturated is repeated deliberately
	 * so a consumer can validate this object without consulting another column.
	 * payload_ns is one inclusive wall interval from the first payload scan to
	 * final payload validation, including callbacks. Internal decode/transfer
	 * counters remain zero, while their public JSON values are null and their
	 * availability flags are false: measuring them at tuple/chunk granularity
	 * would add O(rows) clock overhead to the benchmarked path.
	 */
	std::string result =
		R"({"schema":"bioseq.cold_index_preparation","revision":1,"timing_semantics":"payload_inclusive_wall","decode_timing_available":false,"transfer_timing_available":false)";
	result.reserve(2048);
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};

	append("target_relation_reuses",
		stats.cold_index_target_relation_reuses);
	append("target_relation_extra_opens",
		stats.cold_index_target_relation_extra_opens);
	append("parent_random_reads", stats.cold_index_parent_random_reads);
	append("policy_heap_opens", stats.cold_index_policy_heap_opens);
	append("policy_index_opens", stats.cold_index_policy_index_opens);
	append("source_heap_opens", stats.cold_index_source_heap_opens);
	append("source_index_opens", stats.cold_index_source_index_opens);
	append("fragment_heap_opens", stats.cold_index_fragment_heap_opens);
	append("sidecar_heap_opens", stats.cold_index_sidecar_heap_opens);
	append("sidecar_index_opens", stats.cold_index_sidecar_index_opens);
	append("policy_schema_validations",
		stats.cold_index_policy_schema_validations);
	append("source_schema_validations",
		stats.cold_index_source_schema_validations);
	append("sidecar_schema_validations",
		stats.cold_index_sidecar_schema_validations);
	append("lock_acquires", stats.cold_index_lock_acquires);
	append("lock_releases", stats.cold_index_lock_releases);
	append("snapshot_registers", stats.cold_index_snapshot_registers);
	append("route_empty", stats.cold_index_route_empty);
	append("route_selective", stats.cold_index_route_selective);
	append("route_full", stats.cold_index_route_full);
	append("route_candidate_rows", stats.cold_index_route_candidate_rows);
	append("route_full_rows", stats.cold_index_route_full_rows);
	append("route_candidate_bytes", stats.cold_index_route_candidate_bytes);
	append("route_full_bytes", stats.cold_index_route_full_bytes);
	append("route_selective_cost", stats.cold_index_route_selective_cost);
	append("route_full_cost", stats.cold_index_route_full_cost);
	append("memory_reroutes", stats.cold_index_memory_reroutes);
	append("payload_bytes_requested",
		stats.cold_index_payload_bytes_requested);
	append("payload_bytes_delivered",
		stats.cold_index_payload_bytes_delivered);
	append("payload_rows_requested",
		stats.cold_index_payload_rows_requested);
	append("payload_rows_delivered",
		stats.cold_index_payload_rows_delivered);
	append("payload_rows_scanned", stats.cold_index_payload_rows_scanned);
	append("payload_bytes_scanned", stats.cold_index_payload_bytes_scanned);
	append("payload_rows_skipped", stats.cold_index_payload_rows_skipped);
	append("heap_tid_adjacent", stats.cold_index_heap_tid_adjacent);
	append("heap_tid_forward_gap", stats.cold_index_heap_tid_forward_gap);
	append("heap_tid_backward_jump", stats.cold_index_heap_tid_backward_jump);
	append("extent_crc_bytes", stats.cold_index_extent_crc_bytes);
	append("extent_crc_raw_bytes_hashed",
		stats.cold_index_extent_crc_raw_bytes_hashed);
	append("extent_crc_combined_bytes",
		stats.cold_index_extent_crc_combined_bytes);
	append("extent_crc_combine_rows",
		stats.cold_index_extent_crc_combine_rows);
	append("row_crc_bytes", stats.cold_index_row_crc_bytes);
	append("tuple_form_bytes", stats.cold_index_tuple_form_bytes);
	append("policy_ns", stats.cold_index_policy_ns);
	append("source_ns", stats.cold_index_source_ns);
	append("bind_ns", stats.cold_index_bind_ns);
	append("head_ns", stats.cold_index_head_ns);
	append("directory_ns", stats.cold_index_directory_ns);
	append("payload_ns", stats.cold_index_payload_ns);
	result += R"(,"decode_ns":null,"transfer_ns":null)";
	append("close_ns", stats.cold_index_close_ns);
	append("shared_blks_hit", stats.cold_index_shared_blks_hit);
	append("shared_blks_read", stats.cold_index_shared_blks_read);
	result += R"(,"are_stats_saturated":)";
	result += stats.are_stats_saturated ? "true" : "false";
	result += '}';
	return result;
}

[[nodiscard]] auto
simd_execution_stats_json(const bioseq::pipeline_stats::pipeline_execution_stats& stats)
    -> std::string {
    /*
	 * One compact object carries the exact SIMD execution counters without
	 * widening the scalar row for each new counter. Every value comes from the
	 * immutable alignment snapshot copied before the SRF emits its stage rows.
	 */
	const auto& operational = stats.operational;
	std::string result =
		R"({"schema":"bioseq.simd_execution_stats","revision":2)";
	result.reserve(1152);
	result += R"(,"requested_policy":")";
	result.append(bioseq::pipeline_stats::simd_chaining_policy_identity_name(
		stats.requested_simd_chaining_policy));
	result += R"(","observed_policy":")";
	result.append(bioseq::pipeline_stats::simd_chaining_policy_identity_name(
		stats.observed_simd_chaining_policy));
	result += '"';
	result += R"(,"requested_alignment_algorithm":")";
	result.append(
		bioseq::pipeline_stats::simd_alignment_algorithm_identity_name(
			stats.requested_simd_alignment_algorithm));
	result += R"(","observed_alignment_algorithm":")";
	result.append(
		bioseq::pipeline_stats::simd_alignment_algorithm_identity_name(
			stats.observed_simd_alignment_algorithm));
	result += '"';
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	append("nthash_vector_batches", operational.nthash_vector_batches);
	append(
		"nthash_resident_vector_stream_kernel_calls",
		operational.nthash_resident_vector_stream_kernel_calls);
	append(
		"simd_chaining_exact_policy_calls",
		operational.simd_chaining_exact_policy_calls);
	append(
		"simd_chaining_bounded_policy_calls",
		operational.simd_chaining_bounded_policy_calls);
	append(
		"simd_chaining_bounded_score_batches",
		operational.simd_chaining_bounded_score_batches);
	append(
		"simd_chaining_bounded_cutoff_stops",
		operational.simd_chaining_bounded_cutoff_stops);
	append(
		"simd_chaining_bounded_candidates_skipped",
		operational.simd_chaining_bounded_candidates_skipped);
	append(
		"simd_chaining_policy_rejections",
		operational.simd_chaining_policy_rejections);
	result += R"(,"are_simd_chaining_policy_stats_saturated":)";
	result += operational.are_simd_chaining_policy_stats_saturated
		? "true" : "false";
	const auto& block = operational.block_aligner_stats;
	result += R"(,"block_aligner_execution_stats":{"available":)";
	result += block.is_available ? "true" : "false";
	result += R"(,"consistent":)";
	result += block.is_accepted ? "true" : "false";
	const auto append_block = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	append_block("executed_calls", block.executed_calls);
	append_block("nonempty_calls", block.nonempty_calls);
	append_block(
		"nonempty_calls_with_simd", block.nonempty_calls_with_simd);
	append_block("simd_dp_vector_batches", block.simd_dp_vector_batches);
	append_block("simd_dp_cells", block.simd_dp_cells);
	append_block("forbidden_ksw2_calls", block.forbidden_ksw2_calls);
	append_block(
		"forbidden_scalar_biological_fallbacks",
		block.forbidden_scalar_biological_fallbacks);
	result += R"(,"saturated":)";
	result += block.is_saturated ? "true" : "false";
	result += '}';
	result += '}';
	return result;
}

template<int p_column_count>
struct alignment_stats_row_builder {
	bioseq::postgres::support::pg_datum_list<p_column_count> row{};
	int next_attnum = 1;

	void null_value() noexcept
	{
		row.set_null(next_attnum++);
	}

	void boolean(bool value) noexcept
	{
		row.set(next_attnum++, value);
	}

	void oid(Oid value) noexcept
	{
		row.set_oid(next_attnum++, value);
	}

	void integer(std::int32_t value) noexcept
	{
		row.set(next_attnum++, value);
	}

	void unsigned_integer(std::uint32_t value)
	{
		if (value > bioseq::pipeline_stats::k_sql_integer_value_max) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				{.message =
					"bioseq alignment statistic exceeds SQL integer"});
		}
		integer(static_cast<std::int32_t>(value));
	}

	void signed_bigint(std::int64_t value) noexcept
	{
		row.set(next_attnum++, value);
	}

	void bigint(std::uint64_t value)
	{
		if (value > bioseq::pipeline_stats::k_sql_bigint_value_max) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				{.message =
					"bioseq alignment statistic exceeds SQL bigint"});
		}
		signed_bigint(static_cast<std::int64_t>(value));
	}

	void text(std::string_view value)
	{
		auto* datum = bioseq::postgres::support::invoke_pg_as_cpp(
			[value] {
				return cstring_to_text_with_len(
					value.data(), static_cast<int>(value.size()));
			});
		row.set_raw(
			next_attnum++,
			bioseq::postgres::support::pg_datum::from_pointer(datum));
	}

	void jsonb(std::string_view encoded)
	{
		const std::string terminated{encoded};
		const Datum datum = bioseq::postgres::support::invoke_pg_as_cpp(
			[&terminated] {
				return DirectFunctionCall1(
					jsonb_in, CStringGetDatum(terminated.c_str()));
			});
		row.set_raw(
			next_attnum++,
			bioseq::postgres::support::pg_datum::from_raw(datum));
	}

	void hex_u64(std::uint64_t value)
	{
		const std::string encoded = fixed_hex_u64(value);
		text(encoded);
	}

	void finish() const
	{
		if (next_attnum != p_column_count + 1) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{
					.message =
						"bioseq alignment result mapping is incomplete",
					.detail = fmt::format(
						"mapped={} expected={}",
						next_attnum - 1, p_column_count),
				});
		}
	}
};

} // namespace

namespace bioseq::pg::detail {

[[nodiscard]] auto validate_alignment_snapshot_environment(
    const bioseq::postgres::table::alignment_execution::alignment_execution_snapshot& snapshot)
    -> bioseq::pipeline_stats::executed_backend {
    const auto& stats = snapshot.pipeline;
	bioseq::pipeline_stats::executed_backend environment_backend =
		stats.actual_backend;
	if (!bioseq::pipeline_stats::is_concrete_backend(environment_backend)) {
		/*
		 * A statement whose alignment child is genuinely empty still publishes
		 * one row per pipeline stage. Both backend identities and the environment remain
		 * exactly unset and every job counter is zero.  Do not force a concrete
		 * backend onto this shape (which would launder an empty statement into
		 * backend acceptance), but do allow the SRF to materialise its twenty
		 * zero-work rows.
		 */
		const bool exact_zero_work_snapshot =
			environment_backend
				== bioseq::pipeline_stats::executed_backend::none
			&& stats.requested_backend
				== bioseq::pipeline_stats::executed_backend::none
			&& stats.attempted_alignment_jobs == 0
			&& stats.fully_attested_alignment_jobs == 0
			&& stats.unattested_alignment_jobs == 0
			&& stats.operational.target_rows == 0
			&& bioseq::pipeline_stats::execution_environment_snapshot::is_unset(
				stats.environment);
		if (exact_zero_work_snapshot) {
			return bioseq::pipeline_stats::executed_backend::none;
		}
		const bool honest_diagnostic =
			bioseq::pipeline_stats::pipeline_execution_stats::
				is_diagnostic_environment_valid(stats);
		if (!honest_diagnostic) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_DATA_EXCEPTION,
				{.message =
					"bioseq alignment snapshot has no valid environment backend"});
		}
		return environment_backend;
	}
	if (!bioseq::pipeline_stats::is_environment_valid_for_backend(
			stats.environment, environment_backend)) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_DATA_EXCEPTION,
			{
				.message =
					"bioseq alignment snapshot contains an invalid execution environment",
				.detail = fmt::format(
					"requested_backend={} actual_backend={} "
					"workers={} gpu_present={}",
					bioseq::pipeline_stats::executed_backend_name(
						stats.requested_backend),
					bioseq::pipeline_stats::executed_backend_name(
						stats.actual_backend),
					stats.environment.effective_cpu_workers,
					stats.environment.is_gpu_environment_available),
			});
	}
	return environment_backend;
}

} // namespace bioseq::pg::detail

namespace {

[[nodiscard]] auto
make_alignment_stats_row(const alignment_exec::alignment_execution_snapshot& snapshot,
                         bioseq::pipeline_stats::pipeline_stage stage)
    -> bioseq::postgres::support::pg_datum_list<alignment_stats_column_count> {
    alignment_stats_row_builder<alignment_stats_column_count> out{};
	const auto& stats = snapshot.pipeline;
	const auto& environment = stats.environment;
	const auto& operational = stats.operational;
	const auto stage_no = bioseq::pipeline_stats::stage_index(stage);
	const auto& coverage = stats.stages.coverage[stage_no];
	const auto& timing = operational.timing[stage_no];

	out.signed_bigint(snapshot.execution_id);                         // 1
	out.integer(snapshot.backend_pid);                               // 2
	out.oid(snapshot.database_oid);                                  // 3
	out.unsigned_integer(environment.effective_cpu_workers);          // 4
	if (environment.is_gpu_environment_available) {
		const std::string uuid = fixed_hex_bytes(
			environment.gpu_device_uuid);
		out.text(uuid);                                                // 5
		if (bioseq::pipeline_stats::is_canonical_nonempty_driver_identity(
				environment.gpu_driver_identity)) {
			std::size_t driver_size = 0;
			while (driver_size < environment.gpu_driver_identity.size()
				&& environment.gpu_driver_identity[driver_size] != '\0') {
				++driver_size;
			}
			out.text(std::string_view{
				environment.gpu_driver_identity.data(), driver_size});
			} else {
				bioseq::postgres::support::raise_sql_error(
					ERRCODE_DATA_EXCEPTION,
					{.message =
						"bioseq GPU driver identity is invalid"});
			}                                                           // 6
		out.text(bioseq::pipeline_stats::pipeline_cache_policy_name(
			environment.gpu_pipeline_cache_policy));                   // 7
		const std::string seed_sha = fixed_hex_bytes(
			environment.gpu_pipeline_cache_seed_sha256);
		out.text(seed_sha);                                           // 8
		out.bigint(environment.gpu_pipeline_cache_seed_size);          // 9
		out.hex_u64(environment.gpu_pipeline_cache_manifest_checksum); // 10
		switch (environment.gpu_pipeline_cache_hit) {
			case bioseq::pipeline_stats::optional_observation_bool::false_value:
				out.boolean(false);
				break;
			case bioseq::pipeline_stats::optional_observation_bool::true_value:
				out.boolean(true);
				break;
			default:
				bioseq::postgres::support::raise_sql_error(
					ERRCODE_DATA_EXCEPTION,
					{.message =
						"bioseq GPU pipeline-cache hit observation is invalid"});
		}                                                           // 11
	} else {
		for (int i = 0; i < 7; ++i) out.null_value();                // 5-11
	}
	out.integer(snapshot.scan_node_count);                           // 12
	out.integer(snapshot.nested_alignment_executor_count);           // 13
	out.boolean(snapshot.is_completed);                              // 14
	out.boolean(snapshot.is_partial);                                // 15
	out.boolean(snapshot.is_fully_drained);                          // 16
	out.text(bioseq::pipeline_stats::executed_backend_name(
		stats.requested_backend));                                    // 17
	out.text(bioseq::pipeline_stats::executed_backend_name(
		stats.actual_backend));                                       // 18
	out.text(bioseq::pipeline_stats::target_index_source_name(
		stats.requested_target_index_source));                         // 19
	out.text(bioseq::pipeline_stats::target_index_source_name(
		stats.actual_target_index_source));                            // 20
	out.bigint(stats.persisted_generation_id);                       // 21
	out.hex_u64(stats.persisted_source_identity);                    // 22
	out.boolean(stats.has_mixed_persisted_identity);                     // 23
	out.text(bioseq::pipeline_stats::pipeline_stage_name(stage));    // 24
	out.bigint(coverage.eligible_jobs);                              // 25
	out.bigint(coverage.executed_jobs[
		bioseq::pipeline_stats::engine_index(
			bioseq::pipeline_stats::execution_engine::host_io)]);        // 26
	out.bigint(coverage.executed_jobs[
		bioseq::pipeline_stats::engine_index(
			bioseq::pipeline_stats::execution_engine::scalar_cpu)]);     // 27
	out.bigint(coverage.executed_jobs[
		bioseq::pipeline_stats::engine_index(
			bioseq::pipeline_stats::execution_engine::simd_cpu)]);       // 28
	out.bigint(coverage.executed_jobs[
		bioseq::pipeline_stats::engine_index(
			bioseq::pipeline_stats::execution_engine::vulkan_gpu)]);     // 29
	out.bigint(coverage.fallback_jobs);                              // 30
	out.bigint(timing.host_wall_ns);                                 // 31
	out.bigint(timing.gpu_device_ns);                                // 32
	out.bigint(operational.target_rows);                             // 33
	out.bigint(stats.attempted_alignment_jobs);                      // 34
	out.bigint(stats.fully_attested_alignment_jobs);                 // 35
	out.bigint(stats.unattested_alignment_jobs);                     // 36
	if (stats.first_unattested_target_row
		== bioseq::pipeline_stats::k_unset_job_ordinal) out.null_value();
	else out.bigint(stats.first_unattested_target_row);              // 37
	if (stats.first_unattested_query_ordinal
		== bioseq::pipeline_stats::k_unset_job_ordinal) out.null_value();
	else out.bigint(stats.first_unattested_query_ordinal);           // 38
	out.bigint(operational.cpu_prepared_index_builds);               // 39
	out.bigint(operational.cpu_prepared_index_reuses);               // 40
	out.bigint(operational.cpu_prepared_unique_keys);                // 41
	out.bigint(operational.cpu_prepared_duplicate_build_attempts);   // 42
	if (operational.cpu_prepared_key_set_checksum_available)
		out.hex_u64(operational.cpu_prepared_key_set_checksum);
	else out.null_value();                                           // 43
	if (operational.cpu_prepared_build_set_checksum_available)
		out.hex_u64(operational.cpu_prepared_build_set_checksum);
	else out.null_value();                                           // 44
	out.boolean(operational.has_operational_identity_conflict);      // 45
	out.bigint(operational.persisted_index_rows);                    // 46
	out.bigint(operational.missing_persisted_index_rows);            // 47
	out.bigint(operational.recomputed_target_index_rows);            // 48
	out.bigint(operational.persisted_generation_read_sessions);      // 49
	out.bigint(operational.persisted_relation_opens);                // 50
	out.bigint(operational.persisted_index_scans);                   // 51
	out.bigint(operational.persisted_head_scans);                    // 52
	out.bigint(operational.persisted_head_rows_read);                // 53
	out.bigint(operational.persisted_head_bytes_read);               // 54
	out.bigint(operational.persisted_manifest_directory_scans);      // 55
	out.bigint(operational.persisted_manifest_rows_read);            // 56
	out.bigint(operational.persisted_manifest_bytes_read);           // 57
	out.bigint(operational.persisted_directory_chunks_read);         // 58
	out.bigint(operational.persisted_directory_bytes_read);          // 59
	out.bigint(operational.persisted_metadata_repeated_reads);       // 60
	out.bigint(operational.persisted_metadata_heap_rows_read);       // 61
	out.bigint(operational.persisted_metadata_bytes_read);           // 62
	out.bigint(operational.persisted_payload_heap_rows_read);        // 63
	out.bigint(operational.persisted_payload_bytes_read);            // 64
	out.bigint(operational.persisted_heap_rows_read);                // 65
	out.bigint(operational.persisted_toast_values_read);             // 66
	out.bigint(operational.persisted_detoast_bytes);                 // 67
	out.bigint(operational.persisted_explicit_copy_bytes);           // 68
	out.bigint(operational.persisted_wanted_subkeys);                // 69
	out.bigint(operational.persisted_read_subkeys);                  // 70
	out.bigint(operational.persisted_wanted_extents);                // 71
	out.bigint(operational.persisted_wanted_chunks);                 // 72
	out.bigint(operational.persisted_skipped_rows);                  // 73
	out.bigint(operational.persisted_repeated_chunk_reads);          // 74
	out.bigint(operational.persisted_extents_read);                  // 75
	out.bigint(operational.persisted_chunks_read);                   // 76
	out.bigint(operational.persisted_bytes_read);                    // 77
	out.bigint(operational.persisted_bulk_read_calls);               // 78
	out.bigint(operational.persisted_planned_request_groups);        // 79
	out.bigint(operational.persisted_executed_request_groups);       // 80
	out.bigint(operational.persisted_range_scan_calls);              // 81
	out.bigint(operational.persisted_skip_budget_rows);              // 82
	out.bigint(operational.persisted_hash_build_calls);              // 83
	out.bigint(operational.persisted_assembly_ns);                   // 84
	out.bigint(operational.pair_sort_calls);                         // 85
	out.bigint(operational.host_pair_materialization_calls);         // 86
	out.bigint(operational.host_biological_hash_filter_calls);       // 87
	out.bigint(operational.host_query_hash_readbacks);               // 88
	out.bigint(operational.host_query_symbol_packing_calls);         // 89
	out.bigint(operational.host_target_symbol_packing_calls);        // 90
	out.bigint(operational.host_alignment_job_classification_calls); // 91
	out.bigint(operational.host_anchor_concatenation_calls);         // 92
	out.bigint(operational.host_cigar_construction_calls);           // 93
	out.bigint(operational.full_segment_hydrations);                 // 94
	out.bigint(operational.nthash_vector_batches);                   // 95
	out.bigint(operational.nthash_scalar_backend_branches);          // 96
	out.bigint(operational.nthash_scalar_tail_symbols);              // 97
	out.bigint(operational.nthash_masked_tail_lanes);                // 98
	out.bigint(operational.simd_chain_vector_batches);               // 99
	out.bigint(operational.simd_chain_backtrack_vector_batches);     // 100
	out.bigint(operational.simd_chain_scalar_backend_branches);      // 101
	out.bigint(operational.simd_chain_backtrack_scalar_branches);    // 102
	out.bigint(operational.cpu_seed_positions_rejected_outside_collection); // 103
	out.bigint(operational.gpu_partition_select_dispatches);         // 104
	out.bigint(operational.gpu_partition_anchor_merge_dispatches);   // 105
	out.bigint(operational.gpu_index_build_candidates);              // 106
	out.bigint(operational.gpu_index_build_canonical_emitted);       // 107
	out.bigint(operational.gpu_index_build_overlap_duplicates_rejected); // 108
	out.bigint(operational.gpu_index_build_noncanonical_extras_rejected); // 109
	out.bigint(operational.gpu_index_build_surviving_extras);        // 110
	out.bigint(operational.gpu_persisted_resident_uploads);          // 111
	out.bigint(operational.gpu_persisted_resident_cache_hits);       // 112
	out.bigint(operational.gpu_overlap_anchor_duplicates);           // 113
	out.bigint(operational.gpu_seed_dispatches);                     // 114
	out.bigint(operational.gpu_chain_dispatches);                    // 115
	out.bigint(operational.gpu_chain_backtrack_dispatches);          // 116
	out.bigint(operational.gpu_extend_dispatches);                   // 117
	out.bigint(operational.gpu_target_window_request_dispatches);    // 118
	out.bigint(operational.gpu_target_window_logical_units_requested); // 119
	out.bigint(operational.host_target_window_requests_consumed);    // 120
	out.bigint(operational.host_target_window_physical_units_read);  // 121
	out.bigint(operational.host_target_window_remap_units_filled);   // 122
	out.bigint(operational.host_target_window_packed_bytes_read);    // 123
	out.bigint(operational.gpu_runtime_init_ns);                     // 124
	out.bigint(operational.gpu_pipeline_cache_loads);                // 125
	out.bigint(operational.gpu_pipeline_cache_seed_bytes);           // 126
	out.bigint(operational.gpu_pipeline_cache_load_ns);              // 127
	out.bigint(operational.gpu_pipeline_creation_ns);                // 128
	out.bigint(operational.gpu_submit_cpu_ns);                       // 129
	out.bigint(operational.gpu_wait_cpu_ns);                         // 130
	out.bigint(operational.gpu_ordinal_readback_cpu_ns);             // 131
	out.bigint(operational.gpu_result_readback_cpu_ns);              // 132
	out.bigint(operational.gpu_device_critical_path_ns);             // 133
	out.bigint(operational.gpu_timestamp_query_failures);            // 134
	out.bigint(operational.gpu_queue_submits);                       // 135
	out.bigint(operational.gpu_host_waits);                          // 136
	out.bigint(operational.gpu_buffer_creates);                      // 137
	out.bigint(operational.gpu_memory_allocations);                  // 138
	out.bigint(operational.gpu_pipeline_creates);                    // 139
	out.bigint(operational.backend_declines);                        // 140
	out.bigint(operational.backend_fallbacks);                       // 141
	out.boolean(operational.are_stats_saturated);                        // 142
	out.bigint(operational.persisted_source_parent_rows_read);       // 143
	out.bigint(operational.persisted_source_catalog_rows_read);      // 144
	out.bigint(operational.persisted_source_catalog_index_scans);    // 145
	out.bigint(operational.persisted_source_fragment_rows_read);     // 146
	out.bigint(operational.persisted_source_decoded_bp);             // 147
	out.bigint(operational.persisted_pair_index_conversion_calls);        // 148
	out.bigint(operational.persisted_segment_rebase_calls);           // 149
	out.jsonb(cold_index_preparation_json(operational));             // 150
	out.jsonb(simd_execution_stats_json(stats));                     // 151
	out.finish();
	return out.row;
}

template<class p_value>
struct alignment_optional_traits {
	static constexpr bool is_optional = false;
};

template<class p_value>
struct alignment_optional_traits<std::optional<p_value>> {
	static constexpr bool is_optional = true;
	using contained_type = p_value;
};

template <class p_value, std::size_t p_column_count>
[[nodiscard]] auto
alignment_stats_field(const bioseq::postgres::support::pg_datum_list<p_column_count>& raw,
                      const int attnum) -> p_value {
    if constexpr (alignment_optional_traits<p_value>::is_optional) {
		if (raw.is_null(attnum)) {
			return std::nullopt;
		}
		using contained_type =
			typename alignment_optional_traits<p_value>::contained_type;
		return p_value{alignment_stats_field<contained_type>(raw, attnum)};
	} else {
		if (raw.is_null(attnum)) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_NULL_VALUE_NOT_ALLOWED,
				{
					.message =
						"alignment result contains a null required field",
					.detail = fmt::format("column={}", attnum),
				});
		}
		const Datum datum = raw.at(attnum).raw();
		if constexpr (std::is_same_v<p_value, bool>) {
			return DatumGetBool(datum);
		} else if constexpr (std::is_same_v<p_value, std::int32_t>) {
			return DatumGetInt32(datum);
		} else if constexpr (std::is_same_v<p_value, std::int64_t>) {
			return DatumGetInt64(datum);
		} else if constexpr (std::is_same_v<p_value, Oid>) {
			return DatumGetObjectId(datum);
		} else if constexpr (std::is_same_v<p_value, std::string>) {
			const auto* value =
				reinterpret_cast<const text*>(DatumGetPointer(datum));
			if (value == nullptr) {
				bioseq::postgres::support::raise_sql_error(
					ERRCODE_DATA_EXCEPTION,
					{.message =
						"alignment result contains invalid text"});
			}
			return std::string{
				VARDATA_ANY(value),
				static_cast<std::size_t>(VARSIZE_ANY_EXHDR(value))};
		} else if constexpr (
				std::is_same_v<p_value, bioseq::postgres::support::jsonb_value>) {
			char* encoded = bioseq::postgres::support::invoke_pg_as_cpp(
				[datum] {
					return DatumGetCString(
						DirectFunctionCall1(jsonb_out, datum));
				});
			std::string owned{encoded};
			bioseq::postgres::support::invoke_pg_as_cpp(
				[encoded] { pfree(encoded); });
			return bioseq::postgres::support::jsonb_value{std::move(owned)};
		} else {
			static_assert(
				std::is_same_v<p_value, void>,
				"unsupported alignment SQL result field type");
		}
	}
}

#define BIOSEQ_ALIGNMENT_COMMON_FIELDS(raw) \
	.execution_id = alignment_stats_field<std::int64_t>(raw, 1), \
	.backend_pid = alignment_stats_field<std::int32_t>(raw, 2), \
	.database_oid = alignment_stats_field<Oid>(raw, 3), \
	.effective_cpu_workers = alignment_stats_field<std::int32_t>(raw, 4), \
	.gpu_device_uuid = alignment_stats_field<std::optional<std::string>>(raw, 5), \
	.gpu_driver = alignment_stats_field<std::optional<std::string>>(raw, 6), \
	.gpu_pipeline_cache_policy = alignment_stats_field<std::optional<std::string>>(raw, 7), \
	.gpu_pipeline_cache_seed_sha256 = alignment_stats_field<std::optional<std::string>>(raw, 8), \
	.gpu_pipeline_cache_seed_size = alignment_stats_field<std::optional<std::int64_t>>(raw, 9), \
	.gpu_pipeline_cache_manifest_checksum = alignment_stats_field<std::optional<std::string>>(raw, 10), \
	.gpu_pipeline_cache_hit = alignment_stats_field<std::optional<bool>>(raw, 11), \
	.scan_node_count = alignment_stats_field<std::int32_t>(raw, 12), \
	.nested_alignment_executor_count = alignment_stats_field<std::int32_t>(raw, 13), \
	.is_completed = alignment_stats_field<bool>(raw, 14), \
	.is_partial = alignment_stats_field<bool>(raw, 15), \
	.is_fully_drained = alignment_stats_field<bool>(raw, 16), \
	.requested_backend = alignment_stats_field<std::string>(raw, 17), \
	.actual_backend = alignment_stats_field<std::string>(raw, 18), \
	.requested_target_index_source = alignment_stats_field<std::string>(raw, 19), \
	.actual_target_index_source = alignment_stats_field<std::string>(raw, 20), \
	.persisted_generation_id = alignment_stats_field<std::int64_t>(raw, 21), \
	.persisted_source_identity = alignment_stats_field<std::string>(raw, 22), \
	.has_mixed_persisted_identity = alignment_stats_field<bool>(raw, 23), \
	.stage = alignment_stats_field<std::string>(raw, 24), \
	.eligible_jobs = alignment_stats_field<std::int64_t>(raw, 25), \
	.executed_host_io_jobs = alignment_stats_field<std::int64_t>(raw, 26), \
	.executed_scalar_cpu_jobs = alignment_stats_field<std::int64_t>(raw, 27), \
	.executed_simd_cpu_jobs = alignment_stats_field<std::int64_t>(raw, 28), \
	.executed_vulkan_gpu_jobs = alignment_stats_field<std::int64_t>(raw, 29), \
	.fallback_jobs = alignment_stats_field<std::int64_t>(raw, 30), \
	.stage_host_wall_ns = alignment_stats_field<std::int64_t>(raw, 31), \
	.stage_gpu_device_ns = alignment_stats_field<std::int64_t>(raw, 32), \
	.target_rows = alignment_stats_field<std::int64_t>(raw, 33), \
	.attempted_alignment_jobs = alignment_stats_field<std::int64_t>(raw, 34), \
	.fully_attested_alignment_jobs = alignment_stats_field<std::int64_t>(raw, 35), \
	.unattested_alignment_jobs = alignment_stats_field<std::int64_t>(raw, 36), \
	.first_unattested_target_row = alignment_stats_field<std::optional<std::int64_t>>(raw, 37), \
	.first_unattested_query_ordinal = alignment_stats_field<std::optional<std::int64_t>>(raw, 38), \
	.cpu_prepared_index_builds = alignment_stats_field<std::int64_t>(raw, 39), \
	.cpu_prepared_index_reuses = alignment_stats_field<std::int64_t>(raw, 40), \
	.cpu_prepared_unique_keys = alignment_stats_field<std::int64_t>(raw, 41), \
	.cpu_prepared_duplicate_build_attempts = alignment_stats_field<std::int64_t>(raw, 42), \
	.cpu_prepared_key_set_checksum = alignment_stats_field<std::optional<std::string>>(raw, 43), \
	.cpu_prepared_build_set_checksum = alignment_stats_field<std::optional<std::string>>(raw, 44), \
	.has_operational_identity_conflict = alignment_stats_field<bool>(raw, 45), \
	.persisted_index_rows = alignment_stats_field<std::int64_t>(raw, 46), \
	.missing_persisted_index_rows = alignment_stats_field<std::int64_t>(raw, 47), \
	.recomputed_target_index_rows = alignment_stats_field<std::int64_t>(raw, 48), \
	.persisted_generation_read_sessions = alignment_stats_field<std::int64_t>(raw, 49), \
	.persisted_relation_opens = alignment_stats_field<std::int64_t>(raw, 50), \
	.persisted_index_scans = alignment_stats_field<std::int64_t>(raw, 51), \
	.persisted_head_scans = alignment_stats_field<std::int64_t>(raw, 52), \
	.persisted_head_rows_read = alignment_stats_field<std::int64_t>(raw, 53), \
	.persisted_head_bytes_read = alignment_stats_field<std::int64_t>(raw, 54), \
	.persisted_manifest_directory_scans = alignment_stats_field<std::int64_t>(raw, 55), \
	.persisted_manifest_rows_read = alignment_stats_field<std::int64_t>(raw, 56), \
	.persisted_manifest_bytes_read = alignment_stats_field<std::int64_t>(raw, 57), \
	.persisted_directory_chunks_read = alignment_stats_field<std::int64_t>(raw, 58), \
	.persisted_directory_bytes_read = alignment_stats_field<std::int64_t>(raw, 59), \
	.persisted_metadata_repeated_reads = alignment_stats_field<std::int64_t>(raw, 60), \
	.persisted_metadata_heap_rows_read = alignment_stats_field<std::int64_t>(raw, 61), \
	.persisted_metadata_bytes_read = alignment_stats_field<std::int64_t>(raw, 62), \
	.persisted_payload_heap_rows_read = alignment_stats_field<std::int64_t>(raw, 63), \
	.persisted_payload_bytes_read = alignment_stats_field<std::int64_t>(raw, 64), \
	.persisted_heap_rows_read = alignment_stats_field<std::int64_t>(raw, 65), \
	.persisted_toast_values_read = alignment_stats_field<std::int64_t>(raw, 66), \
	.persisted_detoast_bytes = alignment_stats_field<std::int64_t>(raw, 67), \
	.persisted_explicit_copy_bytes = alignment_stats_field<std::int64_t>(raw, 68), \
	.persisted_wanted_subkeys = alignment_stats_field<std::int64_t>(raw, 69), \
	.persisted_read_subkeys = alignment_stats_field<std::int64_t>(raw, 70), \
	.persisted_wanted_extents = alignment_stats_field<std::int64_t>(raw, 71), \
	.persisted_wanted_chunks = alignment_stats_field<std::int64_t>(raw, 72), \
	.persisted_skipped_rows = alignment_stats_field<std::int64_t>(raw, 73), \
	.persisted_repeated_chunk_reads = alignment_stats_field<std::int64_t>(raw, 74), \
	.persisted_extents_read = alignment_stats_field<std::int64_t>(raw, 75), \
	.persisted_chunks_read = alignment_stats_field<std::int64_t>(raw, 76), \
	.persisted_bytes_read = alignment_stats_field<std::int64_t>(raw, 77), \
	.persisted_bulk_read_calls = alignment_stats_field<std::int64_t>(raw, 78), \
	.persisted_planned_request_groups = alignment_stats_field<std::int64_t>(raw, 79), \
	.persisted_executed_request_groups = alignment_stats_field<std::int64_t>(raw, 80), \
	.persisted_range_scan_calls = alignment_stats_field<std::int64_t>(raw, 81), \
	.persisted_skip_budget_rows = alignment_stats_field<std::int64_t>(raw, 82), \
	.persisted_hash_build_calls = alignment_stats_field<std::int64_t>(raw, 83), \
	.persisted_assembly_ns = alignment_stats_field<std::int64_t>(raw, 84), \
	.pair_sort_calls = alignment_stats_field<std::int64_t>(raw, 85), \
	.host_pair_materialization_calls = alignment_stats_field<std::int64_t>(raw, 86), \
	.host_biological_hash_filter_calls = alignment_stats_field<std::int64_t>(raw, 87), \
	.host_query_hash_readbacks = alignment_stats_field<std::int64_t>(raw, 88), \
	.host_query_symbol_packing_calls = alignment_stats_field<std::int64_t>(raw, 89), \
	.host_target_symbol_packing_calls = alignment_stats_field<std::int64_t>(raw, 90), \
	.host_alignment_job_classification_calls = alignment_stats_field<std::int64_t>(raw, 91), \
	.host_anchor_concatenation_calls = alignment_stats_field<std::int64_t>(raw, 92), \
	.host_cigar_construction_calls = alignment_stats_field<std::int64_t>(raw, 93), \
	.full_segment_hydrations = alignment_stats_field<std::int64_t>(raw, 94), \
	.nthash_vector_batches = alignment_stats_field<std::int64_t>(raw, 95), \
	.nthash_scalar_backend_branches = alignment_stats_field<std::int64_t>(raw, 96), \
	.nthash_scalar_tail_symbols = alignment_stats_field<std::int64_t>(raw, 97), \
	.nthash_masked_tail_lanes = alignment_stats_field<std::int64_t>(raw, 98), \
	.simd_chain_vector_batches = alignment_stats_field<std::int64_t>(raw, 99), \
	.simd_chain_backtrack_vector_batches = alignment_stats_field<std::int64_t>(raw, 100), \
	.simd_chain_scalar_backend_branches = alignment_stats_field<std::int64_t>(raw, 101), \
	.simd_chain_backtrack_scalar_branches = alignment_stats_field<std::int64_t>(raw, 102), \
	.cpu_seed_positions_rejected_outside_collection = alignment_stats_field<std::int64_t>(raw, 103), \
	.gpu_partition_select_dispatches = alignment_stats_field<std::int64_t>(raw, 104), \
	.gpu_partition_anchor_merge_dispatches = alignment_stats_field<std::int64_t>(raw, 105), \
	.gpu_index_build_candidates = alignment_stats_field<std::int64_t>(raw, 106), \
	.gpu_index_build_canonical_emitted = alignment_stats_field<std::int64_t>(raw, 107), \
	.gpu_index_build_overlap_duplicates_rejected = alignment_stats_field<std::int64_t>(raw, 108), \
	.gpu_index_build_noncanonical_extras_rejected = alignment_stats_field<std::int64_t>(raw, 109), \
	.gpu_index_build_surviving_extras = alignment_stats_field<std::int64_t>(raw, 110), \
	.gpu_persisted_resident_uploads = alignment_stats_field<std::int64_t>(raw, 111), \
	.gpu_persisted_resident_cache_hits = alignment_stats_field<std::int64_t>(raw, 112), \
	.gpu_overlap_anchor_duplicates = alignment_stats_field<std::int64_t>(raw, 113), \
	.gpu_seed_dispatches = alignment_stats_field<std::int64_t>(raw, 114), \
	.gpu_chain_dispatches = alignment_stats_field<std::int64_t>(raw, 115), \
	.gpu_chain_backtrack_dispatches = alignment_stats_field<std::int64_t>(raw, 116), \
	.gpu_extend_dispatches = alignment_stats_field<std::int64_t>(raw, 117), \
	.gpu_target_window_request_dispatches = alignment_stats_field<std::int64_t>(raw, 118), \
	.gpu_target_window_logical_units_requested = alignment_stats_field<std::int64_t>(raw, 119), \
	.host_target_window_requests_consumed = alignment_stats_field<std::int64_t>(raw, 120), \
	.host_target_window_physical_units_read = alignment_stats_field<std::int64_t>(raw, 121), \
	.host_target_window_remap_units_filled = alignment_stats_field<std::int64_t>(raw, 122), \
	.host_target_window_packed_bytes_read = alignment_stats_field<std::int64_t>(raw, 123), \
	.gpu_runtime_init_ns = alignment_stats_field<std::int64_t>(raw, 124), \
	.gpu_pipeline_cache_loads = alignment_stats_field<std::int64_t>(raw, 125), \
	.gpu_pipeline_cache_seed_bytes = alignment_stats_field<std::int64_t>(raw, 126), \
	.gpu_pipeline_cache_load_ns = alignment_stats_field<std::int64_t>(raw, 127), \
	.gpu_pipeline_creation_ns = alignment_stats_field<std::int64_t>(raw, 128), \
	.gpu_submit_cpu_ns = alignment_stats_field<std::int64_t>(raw, 129), \
	.gpu_wait_cpu_ns = alignment_stats_field<std::int64_t>(raw, 130), \
	.gpu_ordinal_readback_cpu_ns = alignment_stats_field<std::int64_t>(raw, 131), \
	.gpu_result_readback_cpu_ns = alignment_stats_field<std::int64_t>(raw, 132), \
	.gpu_device_critical_path_ns = alignment_stats_field<std::int64_t>(raw, 133), \
	.gpu_timestamp_query_failures = alignment_stats_field<std::int64_t>(raw, 134), \
	.gpu_queue_submits = alignment_stats_field<std::int64_t>(raw, 135), \
	.gpu_host_waits = alignment_stats_field<std::int64_t>(raw, 136), \
	.gpu_buffer_creates = alignment_stats_field<std::int64_t>(raw, 137), \
	.gpu_memory_allocations = alignment_stats_field<std::int64_t>(raw, 138), \
	.gpu_pipeline_creates = alignment_stats_field<std::int64_t>(raw, 139), \
	.backend_declines = alignment_stats_field<std::int64_t>(raw, 140), \
	.backend_fallbacks = alignment_stats_field<std::int64_t>(raw, 141), \
	.are_stats_saturated = alignment_stats_field<bool>(raw, 142), \
	.persisted_source_parent_rows_read = alignment_stats_field<std::int64_t>(raw, 143), \
	.persisted_source_catalog_rows_read = alignment_stats_field<std::int64_t>(raw, 144), \
	.persisted_source_catalog_index_scans = alignment_stats_field<std::int64_t>(raw, 145), \
	.persisted_source_fragment_rows_read = alignment_stats_field<std::int64_t>(raw, 146), \
	.persisted_source_decoded_bp = alignment_stats_field<std::int64_t>(raw, 147), \
	.persisted_pair_index_conversion_calls = alignment_stats_field<std::int64_t>(raw, 148), \
	.persisted_segment_rebase_calls = alignment_stats_field<std::int64_t>(raw, 149), \
	.cold_index_preparation = alignment_stats_field<bioseq::postgres::support::jsonb_value>(raw, 150),

[[nodiscard]] auto
make_owned_alignment_stats_row(const alignment_exec::alignment_execution_snapshot& snapshot,
                               const bioseq::pipeline_stats::pipeline_stage stage)
    -> bioseq::postgres::sql_rows::alignment_execution_stats_row {
    auto raw = make_alignment_stats_row(snapshot, stage);
	return bioseq::postgres::sql_rows::alignment_execution_stats_row{
		BIOSEQ_ALIGNMENT_COMMON_FIELDS(raw)
		.simd_execution_stats =
			alignment_stats_field<bioseq::postgres::support::jsonb_value>(
				raw, 151),
	};
}

#undef BIOSEQ_ALIGNMENT_COMMON_FIELDS

} // namespace

namespace bioseq::pg::sql_routines {

using bioseq::pg::detail::validate_alignment_snapshot_environment;

bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::alignment_execution_stats_row>
last_alignment_execution_stats() {
	const auto& last_alignment_execution_snapshot =
		bioseq::pg::detail::last_alignment_execution_snapshot_ref();
	std::vector<
		bioseq::postgres::sql_rows::alignment_execution_stats_row> rows;
	if (last_alignment_execution_snapshot.has_value()) {
		const auto& snapshot = *last_alignment_execution_snapshot;
		static_cast<void>(validate_alignment_snapshot_environment(snapshot));
		rows.reserve(bioseq::pipeline_stats::k_pipeline_stage_count);
		for (std::size_t stage_no = 0;
				stage_no < bioseq::pipeline_stats::k_pipeline_stage_count;
				++stage_no) {
			rows.push_back(
				make_owned_alignment_stats_row(
						snapshot,
						static_cast<
							bioseq::pipeline_stats::pipeline_stage>(
								stage_no)));
		}
	}
	return bioseq::postgres::support::table_rows<
		bioseq::postgres::sql_rows::alignment_execution_stats_row>{
			std::move(rows)};
}

} // namespace bioseq::pg::sql_routines
