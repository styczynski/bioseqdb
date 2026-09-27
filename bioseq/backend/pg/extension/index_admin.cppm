/**
 * @file index_admin.cppm
 * @brief SQL functions for index maintenance and storage tests.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/htup.h"
#include "access/tupdesc.h"
#include "catalog/pg_type_d.h"
#include "datatype/timestamp.h"
#include "executor/spi.h"
#include "storage/itemptr.h"
#include "utils/builtins.h"
#include "utils/errcodes.h"
#include "utils/lsyscache.h"
#include "varatt.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
extern "C" {
void bioseq_external_tableam_init(void);
void bioseq_external_scan_init(void);
}
#endif

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.postgres.extension;

import bioseq.common.logging;
import bioseq.postgres.extension.sql_rows;
import bioseq.postgres.support;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.admin;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.column.table_access;

using namespace bioseq::postgres::support;

namespace bioseq::pg::sql_routines {

/**
 * @brief Rebuilds a named sequence index and returns the number of indexed sequences.
 */
std::int64_t update_index(
	Oid rel,
	std::string kind) {
	return bioseq::storage::indexing::update_index_pg_checked(
        rel, kind.data(), kind.size());
}

/** @brief Return the latest index build's sorted-run count, including runs kept in memory. */
std::int64_t last_index_merge_run_count() {
	return static_cast<std::int64_t>(
        bioseq::storage::indexing::g_last_index_merge_run_count);
}

/** @brief Reset this backend's counters for selecting indexes during reads. */
void reset_active_read_policy_debug() {
	bioseq::storage::indexing::reset_active_read_policy_debug_state();
}

/** @return One row showing how index-read policies were chosen and used in this backend. */
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::active_read_policy_debug_row>
active_read_policy_debug() {
	const auto resolver = bioseq::storage::indexing::
		g_active_read_policy_resolution_counters;
	const auto dispatch = bioseq::storage::indexing::
		g_active_read_policy_dispatch_stats;
	const auto source = bioseq::storage::indexing::
		active_read_policy_resolution_source_name(
			bioseq::storage::indexing::
				g_last_active_read_policy_dispatch_source);
    std::vector<bioseq::postgres::sql_rows::active_read_policy_debug_row> rows;
    rows.push_back({
        .resolver_configured_rows =
            static_cast<std::int64_t>(resolver.configured_rows),
        .resolver_authoritative_defaults =
            static_cast<std::int64_t>(resolver.authoritative_defaults),
        .resolver_non_relation =
            static_cast<std::int64_t>(resolver.non_relation),
        .dispatch_configured_batches =
            static_cast<std::int64_t>(dispatch.configured_batches),
        .dispatch_authoritative_default_batches =
            static_cast<std::int64_t>(
                dispatch.authoritative_default_batches),
        .dispatch_non_relation_batches =
            static_cast<std::int64_t>(dispatch.non_relation_batches),
        .last_dispatch_source = std::string{source},
        .prohibited_file_constructor_entries =
            static_cast<std::int64_t>(bioseq::storage::indexing::
                g_prohibited_external_file_producer_entry_counters
                    .constructor_entries),
        .prohibited_file_writer_entries =
            static_cast<std::int64_t>(bioseq::storage::indexing::
                g_prohibited_external_file_producer_entry_counters
                    .writer_entries),
    });
    return bioseq::postgres::support::table_rows<
        bioseq::postgres::sql_rows::active_read_policy_debug_row>{
            std::move(rows)};
}

/** @return Physical storage-row count across shards of the named index. */
std::int64_t index_table_row_count(
	Oid rel,
	std::string name) {
	return bioseq::storage::indexing::
		index_table_row_count_impl(rel, name);
}

/**
 * @brief Invalidate a stored sequence for a missing-fragment test.
 * Validate the requested non-first fragment, then invalidate its source epoch
 * so readers reject the sequence. The fragment rows remain stored.
 */
void remove_sequence_fragment(
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int32_t fragment_ordinal) {
	if (sequence_rowno < 0 || fragment_ordinal <= 0) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = "source-fragment fixture parameters are invalid"});
	}
	bioseq::storage::indexing::remove_sequence_fragment_impl(
		source_table, index_name,
		static_cast<std::uint64_t>(sequence_rowno),
		static_cast<std::uint32_t>(fragment_ordinal));
}


/**
 * @brief Copy a donor index's predicate or expression into the target primary index.
 * Tests use this catalog change to check rejection of malformed index schemas.
 * The change belongs to the current transaction.
 */
void copy_primary_index_shape(
	Oid target_table,
	Oid donor_index,
	std::string shape) {
	if (shape != "partial" && shape != "expression") {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"index shape must be partial or expression"});
	}
	bioseq::storage::indexing::copy_primary_index_shape_pg(
		target_table, donor_index, shape == "partial");
}


/**
 * @brief Corrupt the saved source checksum while keeping index metadata consistent.
 * XOR the supplied mask into the checksum and update the head, manifest and
 * their row checksums. Tests can then check validation against the source sequence.
 */
void rewrite_source_checksum(
	Oid target_table,
	std::int64_t query_key,
	std::int64_t xor_mask) {
	if (query_key < 0 || xor_mask == 0) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = "checksum rewrite parameters are invalid"});
	}
	bioseq::storage::indexing::rewrite_source_checksum_pg(
		target_table,
		static_cast<std::uint64_t>(query_key),
		std::bit_cast<std::uint64_t>(xor_mask));
}


/**
 * @brief Run a read-session failure test against a stored minimizer generation.
 * The selected case checks invalid read order, incompatible metadata or
 * refused memory, and releases session resources on failure.
 */
void exercise_generation_session_failure(
	Oid source_table,
	std::string index_name,
	std::int64_t query_key,
	std::string failure_mode) {
	if (query_key < 0) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"generation session fixture query key is negative"});
	}
	bioseq::storage::indexing::exercise_minimizer_generation_session_failure_impl(
		source_table, index_name,
		static_cast<std::uint64_t>(query_key), failure_mode);
}


/**
 * @brief Exercise creation, clearing or rollback of minimizer-generation storage.
 * Return the relation OIDs before and after the operation, whether the handle
 * was rebound, and whether cached data remains. Missing OIDs become SQL NULL.
 */
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::generation_storage_state_row>
inspect_generation_storage_transition(
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::string operation) {
	if (sequence_rowno < 0) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"generation-storage fixture query key is negative"});
	}
	const auto state = bioseq::storage::indexing::
		exercise_minimizer_generation_storage_state_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno), operation);
	const auto optional_oid = [](const Oid value) -> std::optional<Oid> {
		if (OidIsValid(value)) {
			return value;
		}
		return std::nullopt;
	};
	std::vector<bioseq::postgres::sql_rows::generation_storage_state_row> rows;
	rows.push_back({
		optional_oid(state.before_oid),
		optional_oid(state.operation_oid),
		optional_oid(state.after_oid),
		state.same_handle_rebound,
		state.generation_schema_after,
		state.chunk_cache_hit,
		state.assembled_cache_hit,
	});
	return bioseq::postgres::support::table_rows<
		bioseq::postgres::sql_rows::generation_storage_state_row>{
			std::move(rows)};
}

/** @return The number of distinct subkeys for one index query key. */
std::int64_t index_distinct_sub_keys(
	Oid rel,
	std::string name,
	std::int64_t query_key) {
	return bioseq::storage::indexing::
		index_distinct_sub_keys_impl(rel, name, query_key);
}

/** @brief Drop all physical Bioseq sequence-index tables in the current database. */
void drop_all_index_tables() {
	bioseq::storage::indexing::drop_all_index_tables_impl();
}

/** @brief Set insert-time index maintenance for every sequence column in a table.
 * UPDATE_ON_INSERT enables it. Other policy strings select manual updates.
 * Reject unknown indexes and indexes that cannot be updated during insertion.
 */
void set_index_policy(
	Oid rel,
	std::string name,
	std::string policy) {
	const int status = bioseq::storage::indexing::
		set_index_policy_impl(rel, name, policy == "UPDATE_ON_INSERT");
	if (status == 1) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = fmt::format("unknown index '{}'", name)});
	}
	if (status == 2) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = fmt::format(
				"index '{}' cannot be UPDATE_ON_INSERT", name)});
	}
}

/** @brief Select one index for reads of every sequence column in the table.
 * Clear the previous choice. Indexes stored as table-wide chunks are rejected.
 */
void set_active_read_index(
	Oid rel,
	std::string name) {
	const int status = bioseq::storage::indexing::
		set_active_read_index_impl(rel, name);
	if (status == 1) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = fmt::format("unknown index '{}'", name)});
	}
	if (status == 2) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"a CHUNKED_PER_TABLE index cannot be the active read index"});
	}
}

/** @return Logical row number encoded in the ctid of a USING bioseq table.
 * Reject other table-access methods.
 */
std::int64_t column_row_number(
	Oid rel,
	bioseq::postgres::support::tuple_identifier ctid) {
	if (!relation_oid_uses_column_table_access_method(rel)) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_WRONG_OBJECT_TYPE,
			{.message = "relation is not a USING bioseq table"});
	}
	ItemPointerData tid{};
	ItemPointerSet(
		&tid,
		static_cast<BlockNumber>(ctid.block_number),
		static_cast<OffsetNumber>(ctid.offset_number));
	return static_cast<std::int64_t>(tid_to_row_number(tid));
}


/**
 * @brief Make the next selected inline-sequence operation raise a PostgreSQL error.
 * Tests can choose relation lookup, tuple read, detoasting or relation close.
 */
void arm_inline_sequence_pg_failure(std::string failure_point) {
	using failure_kind =
		bioseq::storage::indexing::inline_sequence_pg_leaf_failure;
	failure_kind failure = failure_kind::none;
	if (failure_point == "relation_lookup") {
		failure = failure_kind::relation_lookup;
	} else if (failure_point == "tuple_read") {
		failure = failure_kind::tuple_read;
	} else if (failure_point == "detoast") {
		failure = failure_kind::detoast;
	} else if (failure_point == "close") {
		failure = failure_kind::close;
	} else {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{
				.message =
					"unknown inline sequence PostgreSQL failure point",
				.detail = fmt::format(
					"failure_point={}", failure_point),
			});
	}
	bioseq::storage::indexing::
		arm_inline_sequence_pg_leaf_failure(failure);
}

/** @brief Insert or replace a physical index chunk for storage tests.
 * Prefer a shard already holding the query key, otherwise use the first shard.
 * Raise an error when the named index has no shard.
 */
void write_index_chunk(
	Oid rel,
	std::string name,
	std::int64_t query_key,
	std::int64_t sub_key,
	std::int32_t chunk_no,
	bioseq::postgres::support::bytea_value payload) {
	const auto payload_datum = bioseq::postgres::support::invoke_pg_as_cpp(
		[&payload] {
			const auto size = payload.bytes().size();
			auto* value = reinterpret_cast<bytea*>(
				palloc(VARHDRSZ + size));
			SET_VARSIZE(value, VARHDRSZ + size);
			if (size != 0) {
				std::memcpy(
					VARDATA(value), payload.bytes().data(), size);
			}
			return PointerGetDatum(value);
		});
	bool wrote = false;
	for (const std::string& prefix :
			bioseq::storage::indexing::seqidx_prefixes_for(rel, name)) {
		auto shards =
			bioseq::storage::indexing::shard_relnames_with_prefix(prefix);
		if (shards.empty()) {
			continue;
		}
		std::string target = shards.front();
		for (const std::string& shard : shards) {
			if (bioseq::storage::indexing::count_in_shard(
					shard,
					fmt::format(
						"WHERE query_key={}",
						static_cast<long long>(query_key))) > 0) {
				target = shard;
				break;
			}
		}
		bioseq::postgres::support::spi_session spi;
		if (spi.connect() != SPI_OK_CONNECT) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_CONNECTION_FAILURE,
				{.message = "could not connect to SPI"});
		}
		const std::string quoted_target =
			bioseq::postgres::support::invoke_pg_as_cpp([&target] {
				return std::string{quote_identifier(target.c_str())};
			});
		const std::string sql = fmt::format(
			"INSERT INTO bioseq_internal.{} "
			"(query_key, sub_key, chunk_no, chunk_bytes, blob) "
			"VALUES ($1,$2,$3,octet_length($4),$4) "
			"ON CONFLICT (query_key, sub_key, chunk_no) DO UPDATE "
			"SET chunk_bytes=EXCLUDED.chunk_bytes, blob=EXCLUDED.blob",
			quoted_target);
		Oid argument_types[4] = {
			INT8OID, INT8OID, INT4OID, BYTEAOID};
		Datum arguments[4] = {
			Int64GetDatum(query_key),
			Int64GetDatum(sub_key),
			Int32GetDatum(chunk_no),
			payload_datum,
		};
		const int status = bioseq::postgres::support::invoke_pg_as_cpp(
			[&sql, &argument_types, &arguments] {
				return SPI_execute_with_args(
					sql.c_str(), 4, argument_types, arguments,
					nullptr, false, 0);
			});
		if (status < 0) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{.message = "could not write index chunk"});
		}
		if (spi.finish() != SPI_OK_FINISH) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_CONNECTION_FAILURE,
				{.message = "could not finish SPI"});
		}
		wrote = true;
		break;
	}
	if (!wrote) {
		const std::string relation_name =
			bioseq::postgres::support::invoke_pg_as_cpp([rel] {
				const char* value = get_rel_name(rel);
				return value == nullptr
					? std::string{"<missing>"}
					: std::string{value};
			});
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_UNDEFINED_TABLE,
			{.message = fmt::format(
				"no seqidx shard for ({}, {})",
				relation_name, name)});
	}
}

/** @brief Delete the manifest chunks for an index query key from every shard.
 * Storage tests use this to leave payload data without its directory.
 */
void delete_index_manifest(
	Oid rel,
	std::string name,
	std::int64_t query_key) {
	const auto directory_sub_key =
		static_cast<std::int64_t>(bioseq::indexing::k_directory_sub_key);
	for (const std::string& prefix :
			bioseq::storage::indexing::seqidx_prefixes_for(rel, name)) {
		for (const std::string& shard :
				bioseq::storage::indexing::
					shard_relnames_with_prefix(prefix)) {
			bioseq::postgres::support::spi_session spi;
			if (spi.connect() != SPI_OK_CONNECT) {
				bioseq::postgres::support::raise_sql_error(
					ERRCODE_CONNECTION_FAILURE,
					{.message = "could not connect to SPI"});
			}
			const std::string quoted_shard =
				bioseq::postgres::support::invoke_pg_as_cpp([&shard] {
					return std::string{
						quote_identifier(shard.c_str())};
				});
			const std::string sql = fmt::format(
				"DELETE FROM bioseq_internal.{} "
				"WHERE query_key={} AND sub_key={}",
				quoted_shard,
				static_cast<long long>(query_key),
				static_cast<long long>(directory_sub_key));
			const int status = bioseq::postgres::support::invoke_pg_as_cpp(
				[&sql] {
					return SPI_execute(sql.c_str(), false, 0);
				});
			if (status < 0) {
				bioseq::postgres::support::raise_sql_error(
					ERRCODE_INTERNAL_ERROR,
					{.message = "could not delete index manifest"});
			}
			if (spi.finish() != SPI_OK_FINISH) {
				bioseq::postgres::support::raise_sql_error(
					ERRCODE_CONNECTION_FAILURE,
					{.message = "could not finish SPI"});
			}
		}
	}
}

} // namespace bioseq::pg::sql_routines
