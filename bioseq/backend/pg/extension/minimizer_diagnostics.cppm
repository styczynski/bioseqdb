/**
 * @file extension/minimizer_diagnostics.cppm
 * @brief Ready-minimizer inspection, probes, and integration fixture SQL routines.
 */
module;

#include <bioseq/pg_compat.h>
#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

module bioseq.postgres.extension;

import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.type;
import bioseq.postgres.extension.sql_rows;
import bioseq.postgres.support;
import bioseq.postgres.storage.indexing.admin;
import bioseq.postgres.storage.indexing.chunked_storage;

namespace {

using bioseq::pg::detail::fixed_hex_bytes;
using bioseq::pg::detail::fixed_hex_u64;

[[nodiscard]] std::string hash_major_minimizer_generation_inspection_json(
		const bioseq::storage::indexing::
			hash_major_minimizer_generation_inspection& inspection)
{
	std::string result =
			R"({"schema":"bioseq.hash_major_minimizer_generation_inspection","revision":1,"layout":"hash_major")";
	result.reserve(4'096);
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
	const auto bounded_name = []<std::size_t p_capacity>(
			const std::array<char, p_capacity>& value) {
		const auto end = std::find(value.begin(), value.end(), '\0');
		return std::string_view{
			value.data(),
			static_cast<std::size_t>(end - value.begin())};
	};
	const auto stream_kind_name = [](
			const bioseq::indexing::hash_major_minimizer_stream_kind kind)
			-> std::string_view {
		switch (kind) {
			case bioseq::indexing::hash_major_minimizer_stream_kind::directory:
				return "directory";
			case bioseq::indexing::hash_major_minimizer_stream_kind::controls:
				return "controls";
			case bioseq::indexing::hash_major_minimizer_stream_kind::postings:
				return "postings";
		}
		return "invalid";
	};

	append("database_oid", inspection.database_oid);
	append("main_relfilenode", inspection.main_relfilenode);
	append(
		"column_attno",
		static_cast<std::uint64_t>(inspection.column_attno));
	append("query_key", inspection.query_key);
	append_string("table_name", bounded_name(inspection.table_name));
	append_string("index_name", bounded_name(inspection.index_name));

	const auto& source = inspection.source;
	result += R"(,"source":{"sequence_rowno":)";
	result += std::to_string(source.sequence_rowno);
	result += R"(,"locator_revision":)";
	result += std::to_string(source.locator.payload_version);
	result += R"(,"bucket_idx":)";
	result += std::to_string(source.locator.bucket_index);
	result += R"(,"shard_idx":)";
	result += std::to_string(source.locator.shard_index);
	result += R"(,"num_fragments":)";
	result += std::to_string(source.locator.fragment_count);
	result += R"(,"total_bp_size":)";
	result += std::to_string(source.locator.logical_length);
	result += R"(,"first_sub_row_no":)";
	result += std::to_string(source.locator.first_fragment_row);
	result += R"(,"source_epoch":)";
	result += std::to_string(source.locator.source_epoch);
	result += R"(,"padding_letters":)";
	result += std::to_string(source.padding_letters);
	result += R"(,"alphabet_code":)";
	result += std::to_string(source.alphabet_code);
	result += R"(,"storage_unit_byte_width":)";
	result += std::to_string(source.storage_unit_byte_width);
	result += R"(,"source_content_crc64":")";
	result += fixed_hex_u64(inspection.source_content_crc64);
	result += R"(","source_identity":")";
	result += fixed_hex_bytes(inspection.source_identity);
	result += R"("})";

	const auto& head = inspection.head;
	result += R"(,"head":{"generation_id":)";
	result += std::to_string(head.generation_id);
	result += R"(,"query_key":)";
	result += std::to_string(head.query_key);
	result += R"(,"source_epoch":)";
	result += std::to_string(head.source_epoch);
	result += R"(,"source_identity":")";
	result += fixed_hex_bytes(head.source_identity);
	result += R"(","source_content_crc64":")";
	result += fixed_hex_u64(head.source_content_crc64);
	result += R"(","compatibility_digest":")";
	result += fixed_hex_bytes(head.storage_profile_digest);
	result += R"(","manifest_address_bound_row_crc64":")";
	result += fixed_hex_u64(head.manifest_address_bound_row_crc64);
	result += R"(","manifest_payload_crc64":")";
	result += fixed_hex_u64(head.manifest_payload_crc64);
	result += R"(","head_crc64":")";
	result += fixed_hex_u64(head.head_crc64);
	result += R"(","dense_seal_present":)";
	result += head.dense_seal.has_value() ? "true" : "false";
	result += '}';

	const auto& manifest = inspection.manifest;
	const auto& storage_profile = manifest.storage_profile;
	result += R"(,"manifest":{"generation_id":)";
	result += std::to_string(manifest.generation_id);
	result += R"(,"source_epoch":)";
	result += std::to_string(manifest.source_epoch);
	result += R"(,"source_identity":")";
	result += fixed_hex_bytes(manifest.source_identity);
	result += R"(","source_content_crc64":")";
	result += fixed_hex_u64(manifest.source_content_crc64);
	result += R"(","target_length":)";
	result += std::to_string(manifest.target_length);
	result += R"(,"unique_hash_count":)";
	result += std::to_string(manifest.unique_hash_count);
	result += R"(,"total_posting_count":)";
	result += std::to_string(manifest.total_posting_count);
	result += R"(,"compatibility_digest":")";
	result += fixed_hex_bytes(manifest.storage_profile_digest);
	result += R"(","authenticated_directory_crc64":")";
	result += fixed_hex_u64(manifest.authenticated_directory_crc64);
	result += R"(","generation_crc64":")";
	result += fixed_hex_u64(manifest.generation_crc64);
	result += R"(","total_control_bytes":)";
	result += std::to_string(manifest.total_control_bytes);
	result += R"(,"total_posting_bytes":)";
	result += std::to_string(manifest.total_posting_bytes);
	result += R"(,"directory_entry_count":)";
	result += std::to_string(manifest.directory_entry_count);
	result += R"(,"compatibility":{"hash_scheme_identity":)";
	result += std::to_string(storage_profile.hash_scheme_identity);
	result += R"(,"hash_scheme_revision":)";
	result += std::to_string(storage_profile.hash_scheme_revision);
	result += R"(,"alphabet_code":)";
	result += std::to_string(storage_profile.alphabet_code);
	result += R"(,"kmer_size":)";
	result += std::to_string(storage_profile.kmer_size);
	result += R"(,"window_size":)";
	result += std::to_string(storage_profile.window_size);
	result += R"(,"hash_width_bytes":)";
	result += std::to_string(storage_profile.hash_width_bytes);
	result += R"(,"stored_posting_width_bytes":)";
	result += std::to_string(
		storage_profile.stored_posting_width_bytes);
	result += R"(,"position_encoding_revision":)";
	result += std::to_string(
		storage_profile.position_encoding_revision);
	result += R"(,"hash_semantics_revision":)";
	result += std::to_string(storage_profile.hash_semantics_revision);
	result += R"(,"count_semantics_revision":)";
	result += std::to_string(storage_profile.count_semantics_revision);
	result += R"(,"producer_backend_identity":)";
	result += std::to_string(storage_profile.producer_backend_identity);
	result += R"(,"producer_backend_revision":)";
	result += std::to_string(storage_profile.producer_backend_revision);
	result += R"(,"controls_per_block":)";
	result += std::to_string(storage_profile.controls_per_block);
	result += R"(,"hash_seed":)";
	result += std::to_string(storage_profile.hash_seed);
	result += R"(}})";

	result += R"(,"metadata":{"head_rows":)";
	result += std::to_string(inspection.head_rows);
	result += R"(,"head_bytes":)";
	result += std::to_string(inspection.head_bytes);
	result += R"(,"manifest_rows":)";
	result += std::to_string(inspection.manifest_rows);
	result += R"(,"manifest_bytes":)";
	result += std::to_string(inspection.manifest_bytes);
	result += '}';

	result += R"(,"streams":[)";
	for (std::size_t index = 0; index < inspection.streams.size(); ++index) {
		if (index != 0u) result += ',';
		const auto& stream = inspection.streams[index];
		result += R"({"kind":")";
		result.append(stream_kind_name(stream.kind));
		result += R"(","element_count":)";
		result += std::to_string(stream.element_count);
		result += R"(,"logical_bytes":)";
		result += std::to_string(stream.logical_bytes);
		result += R"(,"row_count":)";
		result += std::to_string(stream.row_count);
		result += R"(,"stream_crc64":")";
		result += fixed_hex_u64(stream.stream_crc64);
		result += R"(","exact_catalog_rows":)";
		result += std::to_string(stream.exact_catalog_rows);
		result += R"(,"exact_catalog_bytes":)";
		result += std::to_string(stream.exact_catalog_bytes);
		result += R"(,"exact_catalog_stream_crc64":")";
		result += fixed_hex_u64(
			stream.exact_catalog_stream_crc64);
		result += R"("})";
	}
	result += "]}";
	return result;
}

[[nodiscard]] std::string hash_major_minimizer_parallel_scan_stats_json(
		const bioseq::storage::indexing::
			hash_major_minimizer_parallel_scan_stats& stats)
{
	std::string result =
		R"({"schema":"bioseq_internal.hash_major_minimizer_parallel_scan_probe","revision":1)";
	result.reserve(768);
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	append("requested_workers", stats.requested_workers);
	append("launched_workers", stats.launched_workers);
	append("generation_id", stats.generation_id);
	append("query_key", stats.query_key);
	append("expected_rows", stats.expected_rows);
	append("scanned_rows", stats.scanned_rows);
	append("expected_bytes", stats.expected_bytes);
	append("scanned_bytes", stats.scanned_bytes);
	append("duplicate_rows", stats.duplicate_rows);
	append("missing_rows", stats.missing_rows);
	append("corrupt_rows", stats.corrupt_rows);
	append("elapsed_ns", stats.elapsed_ns);
	result += R"(,"checksum_digest":")";
	result += fixed_hex_u64(stats.checksum_digest);
	result += R"(","receipt_digest":")";
	result += fixed_hex_u64(stats.receipt_digest);
	result += R"(","has_leader_participation":)";
	result += stats.has_leader_participation ? "true" : "false";
	result += '}';
	return result;
}

[[nodiscard]] std::string cold_index_selective_probe_json(
		const bioseq::storage::indexing::cold_index_preparation_stats& stats)
{
	std::string result =
		R"({"schema":"bioseq.cold_index_selective_probe","revision":2,"timing_semantics":"payload_inclusive_wall","decode_timing_available":false,"transfer_timing_available":false)";
	result.reserve(1024);
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	append("route_empty", stats.route_empty);
	append("route_selective", stats.route_selective);
	append("route_full", stats.route_full);
	append("payload_rows_requested", stats.payload_rows_requested);
	append("payload_rows_delivered", stats.payload_rows_delivered);
	append("payload_rows_scanned", stats.payload_rows_scanned);
	append("payload_rows_skipped", stats.payload_rows_skipped);
	append("payload_bytes_requested", stats.payload_bytes_requested);
	append("payload_bytes_delivered", stats.payload_bytes_delivered);
	append("payload_bytes_scanned", stats.payload_bytes_scanned);
	append("heap_tid_adjacent", stats.heap_tid_adjacent);
	append("heap_tid_forward_gap", stats.heap_tid_forward_gap);
	append("heap_tid_backward_jump", stats.heap_tid_backward_jump);
	append("extent_crc_bytes", stats.extent_crc_bytes);
	append("extent_crc_raw_bytes_hashed", stats.extent_crc_raw_bytes_hashed);
	append("extent_crc_combined_bytes", stats.extent_crc_combined_bytes);
	append("extent_crc_combine_rows", stats.extent_crc_combine_rows);
	append("row_crc_bytes", stats.row_crc_bytes);
	append("payload_ns", stats.payload_ns);
	result += R"(,"decode_ns":null,"transfer_ns":null)";
	result += R"(,"are_stats_saturated":)";
	result += stats.are_stats_saturated ? "true" : "false";
	result += '}';
	return result;
}

} // namespace

namespace bioseq::pg::sql_routines {

/**
 * @brief Validates a source-bound ready minimizer generation without reading
 * payload extents or enabling live generation reads.
 */
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::inspect_ready_minimizer_generation_row>
inspect_ready_minimizer_generation(
	Oid rel,
	std::string index_name,
	std::int64_t query_key) {
	if (query_key < 0) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"bioseq minimizer generation inspection query key is negative"});
	}
	const auto inspection = bioseq::storage::indexing::
		inspect_ready_minimizer_generation_impl(
			rel, index_name,
			static_cast<std::uint64_t>(query_key));
	const auto sql_bigint = [](std::uint64_t value, const char* field) {
		if (!bioseq::indexing::sql_bigint_can_represent(value)) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				{
					.message =
						"bioseq minimizer generation inspection field exceeds BIGINT",
					.detail = fmt::format("field={}", field),
				});
		}
		return static_cast<std::int64_t>(value);
	};
	const auto sql_smallint = [](std::uint64_t value, const char* field) {
		if (value > static_cast<std::uint64_t>(
				(std::numeric_limits<std::int16_t>::max)())) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				{
					.message =
						"bioseq minimizer generation inspection field exceeds SMALLINT",
					.detail = fmt::format("field={}", field),
				});
		}
		return static_cast<std::int16_t>(value);
	};
	const auto sql_integer = [](std::uint64_t value, const char* field) {
		if (value > static_cast<std::uint64_t>(
				(std::numeric_limits<std::int32_t>::max)())) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				{
					.message =
						"bioseq minimizer generation inspection field exceeds INTEGER",
					.detail = fmt::format("field={}", field),
				});
		}
		return static_cast<std::int32_t>(value);
	};
	std::vector<
		bioseq::postgres::sql_rows::inspect_ready_minimizer_generation_row> rows;
	rows.push_back({
		sql_bigint(inspection.identity.generation_id, "generation_id"),
		fixed_hex_u64(inspection.identity.source_identity),
		fixed_hex_u64(inspection.source.storage_contract_checksum),
		fixed_hex_u64(inspection.source_content_checksum),
		inspection.database_oid,
		inspection.main_relfilenode,
		inspection.column_attno,
		sql_bigint(inspection.source.sequence_rowno, "sequence_rowno"),
		sql_smallint(inspection.source.locator.payload_version, "locator_revision"),
		sql_smallint(inspection.source.locator.bucket_index, "bucket_idx"),
		sql_integer(inspection.source.locator.shard_index, "shard_idx"),
		sql_bigint(inspection.source.locator.fragment_count, "num_fragments"),
		sql_bigint(inspection.source.locator.logical_length, "total_bp_size"),
		sql_bigint(
			inspection.source.locator.first_fragment_row,
			"first_sub_row_no"),
		sql_integer(inspection.source.padding_letters, "padding_letters"),
		sql_smallint(inspection.source.alphabet_code, "alphabet_code"),
		sql_smallint(
			inspection.source.storage_unit_byte_width,
			"storage_unit_byte_width"),
		sql_smallint(inspection.manifest.kmer_size, "kmer_size"),
		sql_smallint(inspection.manifest.window_size, "window_size"),
		sql_integer(inspection.manifest.gpu_abi_revision, "gpu_abi_revision"),
		sql_smallint(
			inspection.manifest.segment_geometry_revision,
			"segment_geometry_revision"),
		sql_bigint(
			inspection.manifest.position_segment_span_bp,
			"position_segment_span_bp"),
		sql_integer(
			inspection.manifest.position_segment_count,
			"position_segment_count"),
	});
	return bioseq::postgres::support::table_rows<
		bioseq::postgres::sql_rows::inspect_ready_minimizer_generation_row>{
			std::move(rows)};
}


/**
 * @brief Validates a source-bound ready hash-major generation and verifies its
 * metadata, catalog rows, byte counts, and checksums.
 */
bioseq::postgres::support::jsonb_value
inspect_ready_hash_major_minimizer_generation(
	Oid rel,
	std::string index_name,
	std::int64_t query_key) {
	if (query_key < 0) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = "bioseq hash-major inspection query key is negative"});
	}
	const auto inspection = bioseq::storage::indexing::
		inspect_hash_major_minimizer_generation(
			rel, index_name,
			static_cast<std::uint64_t>(query_key));
	return bioseq::postgres::support::jsonb_value{
		hash_major_minimizer_generation_inspection_json(inspection)};
}


/**
 * @brief Validates a ready generation row, plain payload, and address checksum
 * using a serial scan when requested_workers is zero or a shared parallel heap
 * scan when it is positive.
 */
bioseq::postgres::support::jsonb_value hash_major_minimizer_parallel_scan_probe(
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int32_t requested_workers) {
	if (sequence_rowno < 0 || requested_workers < 0
		|| requested_workers > 64) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"bioseq hash-major parallel scan probe parameters are invalid"});
	}
	const auto stats = bioseq::storage::indexing::
		hash_major_minimizer_parallel_scan_probe_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno),
			static_cast<std::uint32_t>(requested_workers));
	return bioseq::postgres::support::jsonb_value{
		hash_major_minimizer_parallel_scan_stats_json(stats)};
}

} // namespace bioseq::pg::sql_routines
namespace {

template<class p_result>
[[nodiscard]] static p_result fixture_sql_integer(
		const std::uint64_t value,
		const std::string_view context,
		const std::string_view field) {
	static_assert(
		std::is_same_v<p_result, std::int32_t>
		|| std::is_same_v<p_result, std::int64_t>);
	if (value > static_cast<std::uint64_t>(
			(std::numeric_limits<p_result>::max)())) {
		constexpr std::string_view sql_type =
			std::is_same_v<p_result, std::int32_t>
				? "INTEGER" : "BIGINT";
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
			{
				.message = fmt::format(
					"{} field exceeds {}", context, sql_type),
				.detail = fmt::format("field={}", field),
			});
	}
	return static_cast<p_result>(value);
}

[[nodiscard]] static auto minimizer_fixture_backend(
		const std::string_view requested_backend) {
	return bioseq::storage::indexing::
		ready_minimizer_fixture_backend_from_name(requested_backend);
}

static void validate_minimizer_fixture_parameters(
		const std::int64_t sequence_rowno,
		const std::int16_t kmer_size,
		const std::int16_t window_size,
		const std::int16_t layout,
		const bool allow_grouped_soa) {
	const bool is_valid_layout =
		layout == 3 || layout == 4
		|| (allow_grouped_soa && layout == 5);
	if (sequence_rowno < 0 || kmer_size <= 0 || window_size <= 0
		|| !is_valid_layout) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = "minimizer fixture parameters are invalid"});
	}
}

} // namespace

namespace bioseq::pg::sql_routines {


/**
 * @brief Builds and publishes an authenticated minimizer generation for one
 * explicit sketch backend without opening a read session.
 */
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::build_minimizer_fixture_row>
build_minimizer_fixture(
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int16_t kmer_size,
	std::int16_t window_size,
	std::int16_t selected_layout,
	std::string requested_backend,
	std::int64_t position_segment_span_bp) {
	validate_minimizer_fixture_parameters(
		sequence_rowno, kmer_size, window_size,
		selected_layout, false);
	if (position_segment_span_bp < 0
		|| (position_segment_span_bp != 0 && selected_layout != 4)) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"minimizer fixture position segment span is invalid"});
	}
	const auto layout = static_cast<
		bioseq::indexing::minimizer_generation_layout>(
			static_cast<std::uint16_t>(selected_layout));
	const auto fixture =
		bioseq::storage::indexing::
			build_minimizer_generation_integration_fixture_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno),
			static_cast<std::uint16_t>(kmer_size),
			static_cast<std::uint16_t>(window_size),
			layout,
			minimizer_fixture_backend(requested_backend),
			static_cast<std::uint64_t>(position_segment_span_bp));
	constexpr std::string_view context = "minimizer build fixture";
	std::vector<bioseq::postgres::sql_rows::build_minimizer_fixture_row> rows;
	rows.push_back({
		fixture.table_name,
		static_cast<std::int16_t>(fixture.layout),
		fixture_sql_integer<std::int64_t>(fixture.generation_id, context, "generation_id"),
		fixed_hex_u64(fixture.source_identity),
		fixed_hex_u64(fixture.source_content_checksum),
		fixed_hex_u64(fixture.storage_contract_checksum),
		fixture_sql_integer<std::int64_t>(
			fixture.source_num_fragments, context, "source_num_fragments"),
		fixture_sql_integer<std::int64_t>(
			fixture.source_total_bp, context, "source_total_bp"),
		fixture_sql_integer<std::int64_t>(
			fixture.source_padding_letters, context,
			"source_padding_letters"),
		fixture_sql_integer<std::int64_t>(fixture.writer_rows, context, "writer_rows"),
		fixture_sql_integer<std::int64_t>(fixture.writer_batches, context, "writer_batches"),
		fixture_sql_integer<std::int64_t>(
			fixture.published_head_bytes, context, "published_head_bytes"),
		fixture_sql_integer<std::int64_t>(fixture.control_count, context, "control_count"),
		fixture_sql_integer<std::int64_t>(fixture.position_count, context, "position_count"),
		fixture_sql_integer<std::int64_t>(
			fixture.directory_entries, context, "directory_entries"),
		fixture_sql_integer<std::int64_t>(
			fixture.position_segment_count, context,
			"position_segment_count"),
		fixture_sql_integer<std::int64_t>(
			fixture.control_encoded_bytes, context,
			"control_encoded_bytes"),
		fixture_sql_integer<std::int64_t>(
			fixture.position_encoded_bytes, context,
			"position_encoded_bytes"),
		fixture_sql_integer<std::int64_t>(fixture.control_chunks, context, "control_chunks"),
		fixture_sql_integer<std::int64_t>(
			fixture.position_chunks, context, "position_chunks"),
	});
	return bioseq::postgres::support::table_rows<
		bioseq::postgres::sql_rows::build_minimizer_fixture_row>{
			std::move(rows)};
}


/**
 * @brief Opens an authenticated minimizer-generation read session and returns
 * payload validation, lookup, scan, and cache counters.
 */
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::read_minimizer_fixture_row>
read_minimizer_fixture(
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int16_t kmer_size,
	std::int16_t window_size,
	std::int16_t selected_layout,
	std::string requested_backend) {
	validate_minimizer_fixture_parameters(
		sequence_rowno, kmer_size, window_size,
		selected_layout, false);
	const auto layout = static_cast<
		bioseq::indexing::minimizer_generation_layout>(
			static_cast<std::uint16_t>(selected_layout));
	const auto fixture =
		bioseq::storage::indexing::
			read_minimizer_generation_integration_fixture_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno),
			static_cast<std::uint16_t>(kmer_size),
			static_cast<std::uint16_t>(window_size),
			layout,
			minimizer_fixture_backend(requested_backend));
	constexpr std::string_view context = "minimizer read fixture";
	const auto& stats = fixture.read_stats;
	std::vector<bioseq::postgres::sql_rows::read_minimizer_fixture_row> rows;
	rows.push_back({
		fixture.table_name,
		static_cast<std::int16_t>(fixture.layout),
		fixture_sql_integer<std::int64_t>(fixture.generation_id, context, "generation_id"),
		fixed_hex_u64(fixture.source_identity),
		fixed_hex_u64(fixture.source_content_checksum),
		fixed_hex_u64(fixture.storage_contract_checksum),
		fixture_sql_integer<std::int64_t>(
			fixture.source_num_fragments, context, "source_num_fragments"),
		fixture_sql_integer<std::int64_t>(
			fixture.source_total_bp, context, "source_total_bp"),
		fixture_sql_integer<std::int64_t>(
			fixture.source_padding_letters, context,
			"source_padding_letters"),
		fixture_sql_integer<std::int64_t>(
			fixture.validated_control_bytes, context,
			"validated_control_bytes"),
		fixture_sql_integer<std::int64_t>(
			fixture.validated_position_bytes, context,
			"validated_position_bytes"),
		fixed_hex_u64(fixture.validated_control_checksum),
		fixed_hex_u64(fixture.validated_position_checksum),
		fixture_sql_integer<std::int64_t>(fixture.lookup_count, context, "lookup_count"),
		fixture_sql_integer<std::int64_t>(fixture.lookup_bp, context, "lookup_bp"),
		fixture.lookup_reverse,
		fixture.positions_complete,
		fixture_sql_integer<std::int64_t>(stats.relation_opens, context, "relation_opens"),
		fixture_sql_integer<std::int64_t>(stats.index_scans, context, "index_scans"),
		fixture_sql_integer<std::int64_t>(stats.head_scans, context, "head_scans"),
		fixture_sql_integer<std::int64_t>(
			stats.manifest_directory_scans, context,
			"manifest_directory_scans"),
		fixture_sql_integer<std::int64_t>(
			stats.planned_request_groups, context,
			"planned_request_groups"),
		fixture_sql_integer<std::int64_t>(
			stats.executed_request_groups, context,
			"executed_request_groups"),
		fixture_sql_integer<std::int64_t>(
			stats.bulk_read_calls, context, "bulk_read_calls"),
		fixture_sql_integer<std::int64_t>(
			stats.range_scan_calls, context, "range_scan_calls"),
		fixture_sql_integer<std::int64_t>(
			stats.payload.wanted_extents, context,
			"payload_wanted_extents"),
		fixture_sql_integer<std::int64_t>(
			stats.payload.read_extents, context, "payload_read_extents"),
		fixture_sql_integer<std::int64_t>(
			stats.payload.wanted_chunks, context,
			"payload_wanted_chunks"),
		fixture_sql_integer<std::int64_t>(
			stats.payload.delivered_chunks, context,
			"payload_delivered_chunks"),
		fixture_sql_integer<std::int64_t>(
			stats.payload.delivered_bytes, context,
			"payload_delivered_bytes"),
		fixture_sql_integer<std::int64_t>(
			stats.payload_heap_rows_read, context,
			"payload_heap_rows_read"),
		fixture_sql_integer<std::int64_t>(
			stats.payload.skipped_rows, context, "payload_skipped_rows"),
		fixture_sql_integer<std::int64_t>(
			stats.toast_values_read, context, "toast_values_read"),
		fixture_sql_integer<std::int64_t>(
			stats.detoast_bytes, context, "detoast_bytes"),
		fixture_sql_integer<std::int64_t>(stats.cache_lookups, context, "cache_lookups"),
		bioseq::postgres::support::jsonb_value{
			cold_index_selective_probe_json(
				fixture.selective_probe_stats.cold)},
	});
	return bioseq::postgres::support::table_rows<
		bioseq::postgres::sql_rows::read_minimizer_fixture_row>{
			std::move(rows)};
}


/**
 * @brief Scans one complete minimizer generation through the requested sketch
 * backend and returns payload and scan counters without decoding payload bytes.
 */
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::scan_grouped_soa_minimizer_generation_test_row>
scan_grouped_soa_minimizer_generation_test(
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int16_t kmer_size,
	std::int16_t window_size,
	std::int16_t selected_layout,
	std::string requested_backend) {
	validate_minimizer_fixture_parameters(
		sequence_rowno, kmer_size, window_size,
		selected_layout, true);
	const auto fixture = bioseq::storage::indexing::
		read_minimizer_generation_full_payload_fixture_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno),
			static_cast<std::uint16_t>(kmer_size),
			static_cast<std::uint16_t>(window_size),
			static_cast<
				bioseq::indexing::minimizer_generation_layout>(
					static_cast<std::uint16_t>(selected_layout)),
			minimizer_fixture_backend(requested_backend));
	constexpr std::string_view context =
		"minimizer generation scan fixture";
	const auto& stats = fixture.read_stats;
	std::vector<
		bioseq::postgres::sql_rows::scan_grouped_soa_minimizer_generation_test_row>
		rows;
	rows.push_back({
		fixture.table_name,
		fixture.selected_backend_name,
		fixture_sql_integer<std::int64_t>(fixture.generation_id, context, "generation_id"),
		fixture_sql_integer<std::int64_t>(fixture.control_bytes, context, "control_bytes"),
		fixture_sql_integer<std::int64_t>(
			fixture.position_bytes, context, "position_bytes"),
		fixed_hex_u64(fixture.control_checksum),
		fixed_hex_u64(fixture.position_checksum),
		fixture_sql_integer<std::int64_t>(stats.relation_opens, context, "relation_opens"),
		fixture_sql_integer<std::int64_t>(stats.index_scans, context, "index_scans"),
		fixture_sql_integer<std::int64_t>(
			stats.range_scan_calls, context, "range_scan_calls"),
		fixture_sql_integer<std::int64_t>(
			stats.payload_heap_rows_read, context, "payload_heap_rows"),
		fixture_sql_integer<std::int64_t>(
			stats.payload.delivered_chunks, context, "delivered_chunks"),
		fixture_sql_integer<std::int64_t>(
			stats.payload.delivered_bytes, context, "delivered_bytes"),
		fixture_sql_integer<std::int64_t>(
			stats.full_generation.planned_extents, context,
			"planned_extents"),
		fixture_sql_integer<std::int64_t>(
			stats.full_generation.completed_extents, context,
			"completed_extents"),
		fixture_sql_integer<std::int64_t>(
			stats.full_generation.scan_calls, context, "scan_calls"),
	});
	return bioseq::postgres::support::table_rows<
		bioseq::postgres::sql_rows::scan_grouped_soa_minimizer_generation_test_row>{
			std::move(rows)};
}




/**
 * @brief Builds and publishes a ready PostgreSQL-backed minimizer-fragment
 * generation without changing active routing or metadata age.
 */
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::build_ready_minimizer_fragment_generation_row>
build_ready_minimizer_fragment_generation(
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::string backend) {
	if (sequence_rowno < 0) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"ready minimizer-fragment fixture row number is negative"});
	}
	if (index_name
			!= bioseq::indexing::k_minimizers_per_fragment_index_name) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"ready minimizer-fragment fixture requires "
				"minimizers_per_fragment"});
	}
	const auto fixture = bioseq::storage::indexing::
		build_ready_minimizer_generation_coordinator_fixture_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno), backend);
	const auto& publication = fixture.sealed_publication();
	const auto* execution = std::get_if<
		bioseq::indexing::minimizer_fragment_generation_execution_receipt>(
			&publication.execution());
	if (execution == nullptr) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_DATA_CORRUPTED,
			{.message =
				"ready minimizer-fragment fixture returned a non-fragment "
				"execution receipt"});
	}
	constexpr std::string_view context = "ready minimizer-fragment fixture";
	const auto are_bounded_stats_consistent =
		execution->packed_bounded_stats_present
		&& execution->packed_bounded_status
			== bioseq::indexing::packed_bounded_nthash_index_status::ok
		&& execution->packed_bounded_stats.is_internally_consistent(
			/*require_nonempty_vector_work=*/true);
	std::vector<
		bioseq::postgres::sql_rows::build_ready_minimizer_fragment_generation_row>
		rows;
	rows.push_back({
		fixture.table_name,
		fixture.column_attno,
		fixture_sql_integer<std::int64_t>(
			fixture.sequence_rowno, context, "sequence_rowno"),
		fixed_hex_u64(fixture.locator.source_epoch),
		fixture_sql_integer<std::int64_t>(
			fixture.locator.fragment_count, context,
			"source_num_fragments"),
		fixture_sql_integer<std::int64_t>(
			fixture.locator.logical_length, context, "source_total_bp"),
		fixture.selected_backend_name,
		static_cast<std::int16_t>(fixture.manifest.layout),
		publication.published(),
		fixture.head_manifest_verified,
		fixture_sql_integer<std::int64_t>(
			publication.generation_id(), context, "generation_id"),
		fixed_hex_u64(publication.source_identity()),
		fixed_hex_u64(publication.source_content_checksum()),
		fixed_hex_u64(publication.storage_contract_checksum()),
		fixture_sql_integer<std::int64_t>(
			fixture.manifest.target_length, context,
			"manifest_target_length"),
		static_cast<std::int16_t>(fixture.manifest.kmer_size),
		static_cast<std::int16_t>(fixture.manifest.window_size),
		fixture_sql_integer<std::int32_t>(
			fixture.manifest.minimizer_scheme_id, context,
			"minimizer_scheme_id"),
		fixture_sql_integer<std::int32_t>(
			fixture.manifest.minimizer_scheme_revision, context,
			"minimizer_scheme_revision"),
		fixture_sql_integer<std::int32_t>(
			fixture.manifest.gpu_abi_revision, context,
			"gpu_abi_revision"),
		fixture_sql_integer<std::int64_t>(
			fixture.manifest.position_segment_span_bp, context,
			"position_segment_span_bp"),
		fixture_sql_integer<std::int32_t>(
			fixture.manifest.position_segment_count, context,
			"position_segment_count"),
		fixture_sql_integer<std::int64_t>(
			publication.writer_rows(), context, "writer_rows"),
		fixture_sql_integer<std::int64_t>(
			publication.writer_batches(), context, "writer_batches"),
		fixture_sql_integer<std::int64_t>(
			publication.control_count(), context, "control_count"),
		fixture_sql_integer<std::int64_t>(
			publication.position_count(), context, "position_count"),
		fixture_sql_integer<std::int64_t>(
			execution->producer_scan_count, context,
			"producer_scan_count"),
		fixture_sql_integer<std::int64_t>(
			execution->geometry_retry_count, context,
			"geometry_retry_count"),
		fixture_sql_integer<std::int64_t>(
			execution->short_core_runs, context, "short_core_runs"),
		fixture_sql_integer<std::int64_t>(
			execution->scalar_core_runs, context, "scalar_core_runs"),
		fixture_sql_integer<std::int64_t>(
			execution->exact_simd_core_runs, context,
			"exact_simd_core_runs"),
		fixture_sql_integer<std::int64_t>(
			execution->bounded_nthash_core_runs, context,
			"bounded_nthash_core_runs"),
		fixture_sql_integer<std::int64_t>(
			execution->vulkan_core_runs, context, "vulkan_core_runs"),
		execution->is_accepted(),
		execution->exact_simd_stats_present,
		execution->exact_simd_stats_present
			&& execution->exact_simd_stats.is_internally_consistent(
				/*require_work=*/true),
		fixture_sql_integer<std::int64_t>(
			execution->exact_simd_stats.vector_batches, context,
			"exact_vector_batches"),
		fixture_sql_integer<std::int64_t>(
			execution->exact_simd_stats.scalar_backend_branches,
			context, "exact_scalar_backend_branches"),
		execution->packed_bounded_stats_present,
		std::string{
			bioseq::indexing::packed_bounded_nthash_index_status_name(
				execution->packed_bounded_status)},
		are_bounded_stats_consistent,
		fixture_sql_integer<std::int64_t>(
			execution->packed_bounded_stats.packed_decode_batches,
			context, "bounded_nthash_packed_decode_batches"),
		fixture_sql_integer<std::int64_t>(
			execution->packed_bounded_stats.fallback_calls, context,
			"bounded_nthash_fallback_calls"),
		fixture_sql_integer<std::int64_t>(
			execution->vulkan_stats.completed_roundtrips, context,
			"vulkan_completed_roundtrips"),
		fixture_sql_integer<std::int64_t>(
			execution->vulkan_stats.downloaded_minimizers, context,
			"vulkan_downloaded_minimizers"),
	});
	return bioseq::postgres::support::table_rows<
		bioseq::postgres::sql_rows::build_ready_minimizer_fragment_generation_row>{
			std::move(rows)};
}


/**
 * @brief Builds and publishes a ready PostgreSQL-backed minimizer-sequence
 * generation, releases its spill runs, and leaves active routing and metadata
 * age unchanged.
 */
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::build_ready_minimizer_sequence_generation_row>
build_ready_minimizer_sequence_generation(
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::string backend) {
	if (sequence_rowno < 0) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"ready minimizer-sequence fixture row number is negative"});
	}
	if (index_name
			!= bioseq::indexing::k_minimizers_per_sequence_index_name) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"ready minimizer-sequence fixture requires "
				"minimizers_per_sequence"});
	}
	const auto fixture = bioseq::storage::indexing::
		build_ready_minimizer_generation_coordinator_fixture_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno), backend);
	const auto& publication = fixture.sealed_publication();
	const auto* execution = std::get_if<
		bioseq::indexing::minimizer_sequence_generation_execution_receipt>(
			&publication.execution());
	if (execution == nullptr) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_DATA_CORRUPTED,
			{.message =
				"ready minimizer-sequence fixture returned a non-sequence "
				"execution receipt"});
	}
	constexpr std::string_view context = "ready minimizer-sequence fixture";
	const auto are_bounded_stats_consistent =
		execution->packed_bounded_stats_available
		&& execution->packed_bounded_status
			== bioseq::indexing::packed_bounded_nthash_index_status::ok
		&& execution->packed_bounded_stats.is_internally_consistent(
			/*require_nonempty_vector_work=*/true);
	std::vector<
		bioseq::postgres::sql_rows::build_ready_minimizer_sequence_generation_row>
		rows;
	rows.push_back({
		fixture.table_name,
		fixture.column_attno,
		fixture_sql_integer<std::int64_t>(
			fixture.sequence_rowno, context, "sequence_rowno"),
		fixed_hex_u64(fixture.locator.source_epoch),
		fixture_sql_integer<std::int64_t>(
			fixture.locator.fragment_count, context,
			"source_num_fragments"),
		fixture_sql_integer<std::int64_t>(
			fixture.locator.logical_length, context, "source_total_bp"),
		fixture.selected_backend_name,
		static_cast<std::int16_t>(fixture.manifest.layout),
		publication.published(),
		fixture.head_manifest_verified,
		fixture_sql_integer<std::int64_t>(
			publication.generation_id(), context, "generation_id"),
		fixed_hex_u64(publication.source_identity()),
		fixed_hex_u64(publication.source_content_checksum()),
		fixed_hex_u64(publication.storage_contract_checksum()),
		fixture_sql_integer<std::int64_t>(
			fixture.manifest.target_length, context,
			"manifest_target_length"),
		static_cast<std::int16_t>(fixture.manifest.kmer_size),
		static_cast<std::int16_t>(fixture.manifest.window_size),
		fixture_sql_integer<std::int32_t>(
			fixture.manifest.minimizer_scheme_id, context,
			"minimizer_scheme_id"),
		fixture_sql_integer<std::int32_t>(
			fixture.manifest.minimizer_scheme_revision, context,
			"minimizer_scheme_revision"),
		fixture_sql_integer<std::int32_t>(
			fixture.manifest.gpu_abi_revision, context,
			"gpu_abi_revision"),
		fixture_sql_integer<std::int64_t>(
			fixture.manifest.position_segment_span_bp, context,
			"position_segment_span_bp"),
		fixture_sql_integer<std::int32_t>(
			fixture.manifest.position_segment_count, context,
			"position_segment_count"),
		fixture_sql_integer<std::int64_t>(
			publication.writer_rows(), context, "writer_rows"),
		fixture_sql_integer<std::int64_t>(
			publication.writer_batches(), context, "writer_batches"),
		fixture_sql_integer<std::int64_t>(
			publication.control_count(), context, "control_count"),
		fixture_sql_integer<std::int64_t>(
			publication.position_count(), context, "position_count"),
		fixture_sql_integer<std::int64_t>(
			execution->spill_run_count, context, "spill_run_count"),
		fixture_sql_integer<std::int64_t>(
			execution->spilled_event_count, context,
			"spilled_event_count"),
		fixture_sql_integer<std::int64_t>(
			execution->spill_operation_epoch, context,
			"spill_operation_epoch"),
		fixture_sql_integer<std::int64_t>(
			execution->spill_storage_binding.generation_token, context,
			"spill_generation_token"),
		fixture_sql_integer<std::int64_t>(
			execution->spill_storage_binding.mutation_revision, context,
			"spill_mutation_revision"),
		fixture_sql_integer<std::int64_t>(
			execution->spill_qualification_scan_count, context,
			"spill_qualification_scan_count"),
		fixture_sql_integer<std::int64_t>(
			execution->spill_qualification_merge_count, context,
			"spill_qualification_merge_count"),
		fixture_sql_integer<std::int64_t>(
			execution->configured_core_span_bp, context,
			"configured_core_span_bp"),
		fixture_sql_integer<std::int64_t>(
			execution->peak_core_span_bp, context, "peak_core_span_bp"),
		fixture_sql_integer<std::int64_t>(
			execution->peak_core_events, context, "peak_core_events"),
		fixture_sql_integer<std::int64_t>(
			execution->short_core_runs, context, "short_core_runs"),
		fixture_sql_integer<std::int64_t>(
			execution->scalar_core_runs, context, "scalar_core_runs"),
		fixture_sql_integer<std::int64_t>(
			execution->exact_simd_core_runs, context,
			"exact_simd_core_runs"),
		fixture_sql_integer<std::int64_t>(
			execution->bounded_nthash_core_runs, context,
			"bounded_nthash_core_runs"),
		fixture_sql_integer<std::int64_t>(
			execution->vulkan_core_runs, context, "vulkan_core_runs"),
		execution->is_internally_consistent(),
		execution->exact_simd_stats_available,
		execution->exact_simd_stats_available
			&& execution->exact_simd_stats.is_internally_consistent(
				/*require_work=*/true),
		fixture_sql_integer<std::int64_t>(
			execution->exact_simd_stats.vector_batches, context,
			"exact_vector_batches"),
		fixture_sql_integer<std::int64_t>(
			execution->exact_simd_stats.scalar_backend_branches,
			context, "exact_scalar_backend_branches"),
		execution->packed_bounded_stats_available,
		std::string{
			bioseq::indexing::packed_bounded_nthash_index_status_name(
				execution->packed_bounded_status)},
		are_bounded_stats_consistent,
		fixture_sql_integer<std::int64_t>(
			execution->packed_bounded_stats.packed_decode_batches,
			context, "bounded_nthash_packed_decode_batches"),
		fixture_sql_integer<std::int64_t>(
			execution->packed_bounded_stats.fallback_calls, context,
			"bounded_nthash_fallback_calls"),
		fixture_sql_integer<std::int64_t>(
			execution->vulkan_stats.completed_roundtrips, context,
			"vulkan_completed_roundtrips"),
		fixture_sql_integer<std::int64_t>(
			execution->vulkan_stats.downloaded_minimizers, context,
			"vulkan_downloaded_minimizers"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_scan_count, context, "replay_scan_count"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_merge_pass_count, context,
			"replay_merge_pass_count"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_reroute_pass_count, context,
			"replay_reroute_pass_count"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_completed_scan_count, context,
			"replay_completed_scan_count"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_failed_scan_count, context,
			"replay_failed_scan_count"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_binding_checked_failed_scan_count,
			context, "replay_binding_checked_failed_scan_count"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_illegal_span_transition_count, context,
			"replay_illegal_span_transition_count"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_first_completed_event_count, context,
			"replay_first_completed_event_count"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_last_completed_event_count, context,
			"replay_last_completed_event_count"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_final_span_bp, context,
			"replay_final_span_bp"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_final_storage_binding.generation_token,
			context, "replay_final_generation_token"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_final_storage_binding.mutation_revision,
			context, "replay_final_mutation_revision"),
		fixture_sql_integer<std::int64_t>(
			execution->replay_visited_event_count, context,
			"replay_visited_event_count"),
		fixture_sql_integer<std::int64_t>(
			execution->manifest_position_count, context,
			"manifest_position_count"),
		fixture_sql_integer<std::int64_t>(
			execution->manifest_position_segment_span_bp, context,
			"manifest_position_segment_span_bp"),
		fixture_sql_integer<std::int64_t>(
			publication.minimizer_sequence_spill_generation_token(), context,
			"release_generation_token"),
		fixture_sql_integer<std::int64_t>(
			publication.minimizer_sequence_spill_mutation_before_release(), context,
			"release_mutation_before"),
		fixture_sql_integer<std::int64_t>(
			publication.minimizer_sequence_spill_mutation_after_release(), context,
			"release_mutation_after"),
		publication.minimizer_sequence_spill_resources_released(),
	});
	return bioseq::postgres::support::table_rows<
		bioseq::postgres::sql_rows::build_ready_minimizer_sequence_generation_row>{
			std::move(rows)};
}


} // namespace bioseq::pg::sql_routines
