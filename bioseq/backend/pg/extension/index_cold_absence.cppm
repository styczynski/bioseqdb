/**
 * @file index_cold_absence.cppm
 * @brief Implements the cold no-index storage audit SQL entry point.
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>
#include <string>

module bioseq.postgres.extension;

import bioseq.indexing.minimizer_hashmap;
import bioseq.postgres.extension.sql_rows;
import bioseq.postgres.storage.indexing.admin;
import bioseq.postgres.support;

namespace bioseq::pg::sql_routines {

/** @return Evidence that a cold no-index path has no persisted artifacts. */
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

} // namespace bioseq::pg::sql_routines
