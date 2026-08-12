/*-------------------------------------------------------------------------
 *
 * mod_index_admin_impl.cppm
 *
 * Ordinary implementation unit for index administration and storage fixture
 * routines declared by bioseq.postgres.extension.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "catalog/pg_collation_d.h"
#include "executor/executor.h"
#include "utils/timestamp.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
}

#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
extern "C" {
void bioseq_external_tableam_init(void);
void bioseq_external_scan_init(void);
}
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

module bioseq.postgres.extension;

import bioseq.common.logging;
import bioseq.postgres.column;
import bioseq.postgres.extension.alignment.sql_rows;
import bioseq.postgres.extension.sql_rows;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.postgres.support.row_codec_runtime_test_support;
#endif
import bioseq.postgres.support.sql_catalog_entities;
import bioseq.postgres.extension.cache_provider;
import bioseq.postgres.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import bioseq.postgres.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
import bioseq.postgres.sql.generated.core.storage_metadata_core.entity_handles;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.postgres.sql.generated.test_indexes.guc_registry;
#else
import bioseq.postgres.sql.generated.production.guc_registry;
#endif
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.vectorization.execution;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.vectorization.types.aggregates;
import bioseq.sequences;
import bioseq.postgres.storage.manager;
import bioseq.postgres.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.admin;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.scheduler.supervisor;
import bioseq.parallelism.mode;
import bioseq.common.seqan_adapter;
import bioseq.postgres.column.table_access;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.table;
import bioseq.postgres.table.sql_rows;

using namespace bioseq::postgres::support;

namespace bioseq::pg::sql_routines {

/**
 * @brief Lists registered indexes, policies, sizes, and ages for Bioseq sequence columns.
 *
 * The result combines configured policy, persisted shard size, and last
 * update time without requiring an index generation to exist.
 */
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::list_metadata_row>
list_metadata() {
	namespace bsi = bioseq::storage::indexing;
	namespace bidx = bioseq::indexing;
	std::vector<bioseq::postgres::sql_rows::list_metadata_row> result;

	const auto kinds = bsi::pg_supported_indexes::list_types();
	const auto allows = bsi::pg_supported_indexes::allows_on_insert_flags();

    bioseq::postgres::support::spi_session spi;
	if (spi.connect() != SPI_OK_CONNECT) {
        throw std::runtime_error{
            "bioseq.list_metadata could not connect to SPI"};
    }
	const auto execute_spi = [](const char* sql, const bool read_only) {
		return bioseq::postgres::support::invoke_pg_as_cpp(
			[sql, read_only] {
				return SPI_execute(sql, read_only, 0);
			});
	};
	const auto quote_literal_owned = [](const std::string_view value) {
		/*
		 * Keep every C++ owner outside the PG_TRY bridge.  The leaf callback
		 * captures only a stable raw pointer and returns PostgreSQL's owned
		 * buffer; copying and freeing happen after the bridge has returned.
		 */
		const std::string input{value};
		char* const quoted = bioseq::postgres::support::invoke_pg_as_cpp(
			[input_data = input.c_str()] {
				return quote_literal_cstr(input_data);
			});
		try {
			std::string result{quoted};
			bioseq::postgres::support::invoke_pg_as_cpp(
				[quoted] { pfree(quoted); });
			return result;
		} catch (...) {
			const std::exception_ptr original =
				std::current_exception();
			try {
				bioseq::postgres::support::invoke_pg_as_cpp(
					[quoted] { pfree(quoted); });
			} catch (...) {
				std::terminate();
			}
			std::rethrow_exception(original);
		}
	};

	/* 1. every user bioseq-AM relation (exclude internal namespaces / names). */
	std::vector<Oid> rels;
	if (execute_spi(
			"SELECT c.oid FROM pg_class c JOIN pg_am am ON c.relam=am.oid "
			"JOIN pg_namespace n ON c.relnamespace=n.oid "
			"WHERE am.amname='bioseq' AND c.relkind='r' "
			"AND n.nspname NOT IN ('bioseq_internal','pg_catalog','information_schema') "
			"AND n.nspname NOT LIKE 'pg\\_temp%'", true) == SPI_OK_SELECT) {
		for (std::uint64_t r = 0; r < SPI_processed; ++r) {
			auto rel_cell = bioseq::postgres::support::pg_spi_cell::get(
				SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1);
			if (auto rel_oid = rel_cell.optional_oid()) rels.push_back(*rel_oid);
		}
	}

	/* 2. build the VALUES source: (rel, attno, attname, idx_name, idx_type, def_uoi, prefix). */
	std::string values;
	for (Oid rel : rels) {
		const auto rid = bsi::index_relation_identity(rel);
		std::string colsql = fmt::format(
			"SELECT a.attnum, a.attname FROM pg_attribute a JOIN pg_type t ON a.atttypid=t.oid "
			"JOIN pg_namespace tn ON t.typnamespace=tn.oid WHERE a.attrelid={}::oid AND t.typname='seq' "
			"AND tn.nspname IN ('bioseq','bioseq_internal') AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",
			static_cast<unsigned>(rel));
		if (execute_spi(colsql.c_str(), true) != SPI_OK_SELECT) continue;
		std::vector<std::pair<std::int16_t, std::string>> cols;
		for (std::uint64_t c = 0; c < SPI_processed; ++c) {
			auto att_cell = bioseq::postgres::support::pg_spi_cell::get(
				SPI_tuptable->vals[c], SPI_tuptable->tupdesc, 1);
			const std::int16_t a = att_cell.optional<std::int16_t>().value_or(0);
			char* nm = bioseq::postgres::support::invoke_pg_as_cpp(
				[c] {
					return SPI_getvalue(
						SPI_tuptable->vals[c],
						SPI_tuptable->tupdesc, 2);
				});
			cols.emplace_back(
				a, nm == nullptr ? std::string{} : std::string{nm});
			if (nm != nullptr) {
				bioseq::postgres::support::invoke_pg_as_cpp(
					[nm] { pfree(nm); });
			}
		}
		for (auto& [attno, attname] : cols) {
			for (std::size_t i = 0; i < kinds.size(); ++i) {
				const std::string idx_name{kinds[i].first};
				const std::string idx_type{bidx::sequence_index_layout_name(kinds[i].second)};
				const bool def_uoi = bidx::is_default_update_on_insert_index(kinds[i].first) && allows[i];
				const std::string prefix = fmt::format("seqidx_{}_{:016x}_",
					bsi::short_index_for(kinds[i].first),
					bsi::identity_hash64(rid.db_oid, rid.tablespace_oid, rid.relfilenode, attno, kinds[i].first));
				if (!values.empty()) values += ",";
				values += fmt::format("({}::oid,{}::int2,{},{},{},{},{})",
					static_cast<unsigned>(rel), static_cast<int>(attno),
					quote_literal_owned(attname),
					quote_literal_owned(idx_name),
					quote_literal_owned(idx_type),
					def_uoi ? "true" : "false",
					quote_literal_owned(prefix));
			}
		}
	}

	if (!values.empty()) {
		std::string query = fmt::format(
			"WITH src(rel,attno,attname,idx_name,idx_type,def_uoi,prefix) AS (VALUES {}) "
			"SELECT src.rel::regclass, src.attno::int2, src.attname, src.idx_name, src.idx_type, "
			"  CASE WHEN p.update_on_insert IS NOT NULL "
			"       THEN (CASE WHEN p.update_on_insert THEN 'UPDATE_ON_INSERT' ELSE 'UPDATE_MANUALLY' END) "
			"       WHEN src.def_uoi THEN 'UPDATE_ON_INSERT' ELSE 'UPDATE_MANUALLY' END, "
			"  pg_size_pretty(COALESCE((SELECT SUM(pg_total_relation_size(ic.oid)) FROM pg_class ic "
			"     JOIN pg_namespace inx ON ic.relnamespace=inx.oid "
			"     WHERE inx.nspname='bioseq_internal' AND ic.relname LIKE src.prefix||'%'),0)::bigint), "
			"  (SELECT now()-m.last_updated FROM bioseq_internal.metadata_age_log m "
			"     WHERE m.user_relid=src.rel AND m.column_attno=src.attno AND m.index_name=src.idx_name LIMIT 1) "
			"FROM src LEFT JOIN bioseq_internal.index_policy p "
			"  ON p.user_relid=src.rel AND p.column_attno=src.attno AND p.index_name=src.idx_name "
			"ORDER BY src.rel, src.attno, src.idx_name", values);
		if (execute_spi(query.c_str(), true) == SPI_OK_SELECT) {
			for (std::uint64_t r = 0; r < SPI_processed; ++r) {
                HeapTuple tuple = SPI_tuptable->vals[r];
                TupleDesc descriptor = SPI_tuptable->tupdesc;
                const auto required_text =
                    [tuple, descriptor](int column) {
                        char* value =
							bioseq::postgres::support::invoke_pg_as_cpp(
								[tuple, descriptor, column] {
									return SPI_getvalue(
										tuple, descriptor, column);
								});
                        if (value == nullptr) {
                            throw std::runtime_error{
                                "bioseq.list_metadata produced a null "
                                "required text field"};
                        }
                        std::string owned{value};
						bioseq::postgres::support::invoke_pg_as_cpp(
							[value] { pfree(value); });
                        return owned;
                    };
                const auto table_name =
                    bioseq::postgres::support::pg_spi_cell::get(
                        tuple, descriptor, 1).optional_oid();
                const auto column_attno =
                    bioseq::postgres::support::pg_spi_cell::get(
                        tuple, descriptor, 2)
                        .optional<std::int16_t>();
                if (!table_name.has_value()
                    || !column_attno.has_value()) {
                    throw std::runtime_error{
                        "bioseq.list_metadata produced a null relation "
                        "identity"};
                }
                std::optional<bioseq::postgres::support::interval_value>
					time_since_update;
                const auto time_since_update_cell =
                    bioseq::postgres::support::pg_spi_cell::get(
                        tuple, descriptor, 8);
                if (!time_since_update_cell.is_null) {
                    const auto* interval =
                        reinterpret_cast<const Interval*>(
							DatumGetPointer(
								time_since_update_cell.value.raw()));
                    if (interval == nullptr) {
                        throw std::runtime_error{
                            "bioseq.list_metadata produced an invalid "
                            "interval"};
                    }
					time_since_update =
						bioseq::postgres::support::interval_value{
                        .microseconds = interval->time,
                        .days = interval->day,
                        .months = interval->month,
                    };
                }
                result.push_back({
                    .table_name = *table_name,
                    .column_attno = *column_attno,
                    .column_name = required_text(3),
                    .index_name = required_text(4),
                    .index_type = required_text(5),
                    .update_policy = required_text(6),
                    .storage_size = required_text(7),
                    .time_since_update = time_since_update,
                });
			}
		}
	}

	if (spi.finish() != SPI_OK_FINISH) {
        throw std::runtime_error{
            "bioseq.list_metadata could not finish SPI"};
    }
	return bioseq::postgres::support::table_rows<
        bioseq::postgres::sql_rows::list_metadata_row>{
            std::move(result)};
}

/**
 * @brief Rebuilds a named sequence index and returns the number of indexed sequences.
 */
std::int64_t update_index(
	Oid rel,
	std::string kind) {
	return bioseq::storage::indexing::update_index_pg_checked(
        rel, kind.data(), kind.size());
}

std::int64_t last_index_merge_run_count() {
	return static_cast<std::int64_t>(
        bioseq::storage::indexing::g_last_index_merge_run_count);
}


void reset_active_read_policy_debug() {
	bioseq::storage::indexing::reset_active_read_policy_debug_state();
}


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

std::int64_t index_table_row_count(
	Oid rel,
	std::string name) {
	return bioseq::storage::indexing::
		index_table_row_count_impl(rel, name);
}

bioseq::postgres::support::named_output<
	bioseq::postgres::sql_rows::inspect_cold_no_index_absence_row>
inspect_cold_no_index_absence(
	Oid rel,
	std::string name) {
	const auto audit =
		bioseq::storage::indexing::cold_no_index_absence_audit_impl(rel, name);
	const auto sql_bigint = [](const std::uint64_t value) {
		if (!bioseq::indexing::sql_bigint_can_represent(value)) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				{.message =
					"bioseq cold no-index audit value exceeds BIGINT"});
		}
		return static_cast<std::int64_t>(value);
	};
	return bioseq::postgres::support::named_output<
		bioseq::postgres::sql_rows::inspect_cold_no_index_absence_row>{
		bioseq::postgres::sql_rows::inspect_cold_no_index_absence_row{
			sql_bigint(audit.target_rows),
			audit.target_cardinality_metadata_only,
			sql_bigint(audit.source_parent_rows_read),
			sql_bigint(audit.source_fragment_rows_read),
			sql_bigint(audit.source_decoded_bp),
			sql_bigint(audit.physical_index_relations),
			sql_bigint(audit.persisted_index_rows),
			sql_bigint(audit.generation_objects),
		},
	};
}

/**
 * @brief Removes the selected non-first sequence fragment referenced by a
 * source locator.
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
 * @brief Copies selected primary-index predicate or expression metadata from a
 * donor index inside the current transaction.
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
 * @brief Applies the requested mask to matching source and generation checksums
 * while keeping their metadata consistent.
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
 * @brief Exercises the requested read-session failure mode and verifies permit
 * and resource cleanup.
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
 * @brief Runs the selected generation-storage transition and returns
 * relation-handle, rollback, and cache-invalidation state.
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

std::int64_t index_distinct_sub_keys(
	Oid rel,
	std::string name,
	std::int64_t query_key) {
	return bioseq::storage::indexing::
		index_distinct_sub_keys_impl(rel, name, query_key);
}

void drop_all_index_tables() {
	bioseq::storage::indexing::drop_all_index_tables_impl();
}

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
 * @brief Arms one PostgreSQL error at the selected inline-sequence lookup, read,
 * detoast, or close operation.
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
