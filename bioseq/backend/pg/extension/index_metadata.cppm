/**
 * @file index_metadata.cppm
 * @brief List available index types and stored-index information for sequence columns.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/htup.h"
#include "access/tupdesc.h"
#include "datatype/timestamp.h"
#include "executor/spi.h"
#include "utils/builtins.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

module bioseq.postgres.extension;

import bioseq.postgres.extension.sql_rows;
import bioseq.postgres.support;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.supported_indexes;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.index_relation_identity;

using namespace bioseq::postgres::support;

namespace bioseq::pg::sql_routines {

/**
 * @brief List every supported index type for each sequence column in a Bioseq table.
 * Include the update policy, total shard size and time  since the last update.
 * Missing storage gives zero size. A missing update-log entry gives no age.
 * Internal and temporary tables are excluded.
 */
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::list_metadata_row>
list_metadata() {
	namespace bsi = bioseq::storage::indexing;
	namespace bidx = bioseq::indexing;
	std::vector<bioseq::postgres::sql_rows::list_metadata_row> result;

	const auto index_types = bsi::pg_supported_indexes::list_types();
	const auto allows_on_insert =
		bsi::pg_supported_indexes::allows_on_insert_flags();

    bioseq::postgres::support::spi_session spi;
	if (spi.connect() != SPI_OK_CONNECT) {
        throw std::runtime_error{
            "bioseq.list_metadata could not connect to SPI"};
    }
	const auto execute_spi = [](const char* statement, const bool read_only) {
		return bioseq::postgres::support::invoke_pg_as_cpp(
			[statement, read_only] {
				return SPI_execute(statement, read_only, 0);
			});
	};
	const auto quote_literal_owned = [](const std::string_view value) {
		/* PostgreSQL errors can jump past C++ destructors. Keep the input string
		 * outside the callback and pass only its raw pointer Copy the quoted
		 * result after the call returns, then free PostgreSQL's buffer. */
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

	/* Find user tables that use the Bioseq table-acces method. */
	std::vector<Oid> relation_oids;
	if (execute_spi(
			"SELECT c.oid FROM pg_class c JOIN pg_am am ON c.relam=am.oid "
			"JOIN pg_namespace n ON c.relnamespace=n.oid "
			"WHERE am.amname='bioseq' AND c.relkind='r' "
			"AND n.nspname NOT IN ('bioseq_internal','pg_catalog','information_schema') "
			"AND n.nspname NOT LIKE 'pg\\_temp%'", true) == SPI_OK_SELECT) {
		for (std::uint64_t row_index = 0; row_index < SPI_processed;
			 ++row_index) {
			auto relation_cell = bioseq::postgres::support::pg_spi_cell::get(
				SPI_tuptable->vals[row_index], SPI_tuptable->tupdesc, 1);
			if (auto relation_oid = relation_cell.optional_oid()) {
				relation_oids.push_back(*relation_oid);
			}
		}
	}

	/* Pair each sequence column with, every supported index  type. These rows
	 * supply defaults when the policy or stored index does not exist yet. */
	std::string source_values;
	for (const Oid relation_oid : relation_oids) {
		const auto relation_identity = bsi::index_relation_identity(relation_oid);
		std::string column_query = fmt::format(
			"SELECT a.attnum, a.attname FROM pg_attribute a JOIN pg_type t ON a.atttypid=t.oid "
			"JOIN pg_namespace tn ON t.typnamespace=tn.oid WHERE a.attrelid={}::oid AND t.typname='seq' "
			"AND tn.nspname IN ('bioseq','bioseq_internal') AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",
			static_cast<unsigned>(relation_oid));
		if (execute_spi(column_query.c_str(), true) != SPI_OK_SELECT)
		{
			continue;
		}
		std::vector<std::pair<std::int16_t, std::string>> sequence_columns;
		for (std::uint64_t row_index = 0; row_index < SPI_processed;
			 ++row_index) {
			auto attribute_cell = bioseq::postgres::support::pg_spi_cell::get(
				SPI_tuptable->vals[row_index], SPI_tuptable->tupdesc, 1);
			const std::int16_t attribute_number =
				attribute_cell.optional<std::int16_t>().value_or(0);
			char* attribute_name_data =
				bioseq::postgres::support::invoke_pg_as_cpp(
				[row_index] {
					return SPI_getvalue(
						SPI_tuptable->vals[row_index],
						SPI_tuptable->tupdesc, 2);
				});
			sequence_columns.emplace_back(
				attribute_number,
				attribute_name_data == nullptr
					? std::string{}
					: std::string{attribute_name_data});
			if (attribute_name_data != nullptr) {
				bioseq::postgres::support::invoke_pg_as_cpp(
					[attribute_name_data] { pfree(attribute_name_data); });
			}
		}
		for (auto& [attribute_number, attribute_name] : sequence_columns) {
			for (std::size_t index_type_index = 0;
				 index_type_index < index_types.size(); ++index_type_index) {
				const auto& index_type_entry = index_types[index_type_index];
				const std::string index_name{index_type_entry.first};
				const std::string index_type{
					bidx::sequence_index_layout_name(index_type_entry.second)};
				const bool default_update_on_insert =
					bidx::is_default_update_on_insert_index(index_type_entry.first)
					&& allows_on_insert[index_type_index];
				const std::string relation_prefix = fmt::format(
					"seqidx_{}_{:016x}_",
					bsi::short_index_for(index_type_entry.first),
					bsi::identity_hash64(
						relation_identity.db_oid,
						relation_identity.tablespace_oid,
						relation_identity.relfilenode,
						attribute_number,
						index_type_entry.first));
				if (!source_values.empty()) source_values += ",";
				source_values += fmt::format(
					"({}::oid,{}::int2,{},{},{},{},{})",
					static_cast<unsigned>(relation_oid),
					static_cast<int>(attribute_number),
					quote_literal_owned(attribute_name),
					quote_literal_owned(index_name),
					quote_literal_owned(index_type),
					default_update_on_insert ? "true" : "false",
					quote_literal_owned(relation_prefix));
			}
		}
	}

	if (!source_values.empty()) {
		std::string metadata_query = fmt::format(
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
			"ORDER BY src.rel, src.attno, src.idx_name", source_values);
		if (execute_spi(metadata_query.c_str(), true) == SPI_OK_SELECT) {
			for (std::uint64_t row_index = 0; row_index < SPI_processed;
				 ++row_index) {
                HeapTuple tuple = SPI_tuptable->vals[row_index];
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

} // namespace bioseq::pg::sql_routines

