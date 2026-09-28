/**
 * @file tam_helpers.cppm
 * @brief Identify Bioseq tables and convert logical row numbers to PostgreSQL TIDs.
 *
 * The table-access initializer supplies its routine pointer Other modules
 * can compare that pointer without importing the full table implementation,
 * avoiding circular imports.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "access/tableam.h"
#include "storage/itemptr.h"
#include "storage/lockdefs.h"
#include "utils/errcodes.h"
#include "utils/rel.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

export module bioseq.postgres.column.table_access;

import bioseq.postgres.storage.physical;
import bioseq.postgres.support;
import bioseq.common.logging;

/// @brief Rejects row numbers that cannot be represented by a PostgreSQL TID.
/// @param[in] row_number One-based column row number to validate.
/// @note Reports a PostgreSQL error when the value is reserved or exceeds TID capacity.
static void
validate_row_number(uint64_t row_number)
{
	if (row_number == invalid_column_row_number)
	{
		bioseq_report<logging_level::error, logging_target::postgres>().with_sql_error_code(ERRCODE_INTERNAL_ERROR)
			.with_message("unexpected row number for columnar table");
	}
	else if (row_number > maximum_column_row_number)
	{
		bioseq_report<logging_level::error, logging_target::postgres>().with_sql_error_code(ERRCODE_FEATURE_NOT_SUPPORTED)
			.with_message("columnar tables can't have row numbers greater than {}",
				 static_cast<uint64_t>(maximum_column_row_number))
			.with_hint("Consider using VACUUM FULL for your table");
	}
}


/// @brief Encode a logical column row number in a PostgreSQL tuple id (TID).
/// @param[in] row_number One-based row number within the supported range.
/// @return Item pointer identifying the logical  row, independent of its data page.
export ItemPointerData
row_number_to_tid(uint64_t row_number)
{
	validate_row_number(row_number);

	ItemPointerData tid = { 0 };
	ItemPointerSetBlockNumber(&tid, row_number / item_pointer_row_capacity);
	ItemPointerSetOffsetNumber(&tid, row_number % item_pointer_row_capacity +
							   FirstOffsetNumber);
	return tid;
}


/// @brief Decodes a PostgreSQL item pointer into a column row number.
/// @param[in] tid Item pointer produced by row_number_to_tid.
/// @return One-based column row number.
export uint64_t
tid_to_row_number(ItemPointerData tid)
{
	uint64_t row_number = ItemPointerGetBlockNumber(&tid) * item_pointer_row_capacity +
					   ItemPointerGetOffsetNumber(&tid) - FirstOffsetNumber;

	validate_row_number(row_number);

	return row_number;
}


// Registered once during table-AM initialization and compared by identity.
static const TableAmRoutine* registered_column_table_access_method = nullptr;

/// @brief Save the routine pointer used to recognize Bioseq columnar tables.
/// @param[in] routine Table-access routine tnat remains alive for this process.
export void
register_column_table_access_method(const TableAmRoutine* routine) noexcept
{
	registered_column_table_access_method = routine;
}

/** @brief Check an open relation for the Bioseq table-access method.
 *
 * Reuse the caller's handle during DROP. An earlier recursive drop may have
 * removed the catalog row, making a second lookup by OID fail.
 *
 * @param[in] relation Borrowed relation handle, or nullptr.
 * @return true if the relation uses the registered Bioseq routine.
 */
export bool
relation_uses_column_table_access_method(Relation relation) noexcept
{
	return relation != nullptr
		&& registered_column_table_access_method != nullptr
		&& relation->rd_tableam == registered_column_table_access_method;
}


/// @brief Open a relation and check for  the Bioseq table-access method.
/// @param[in] relation_oid Relation OID. InvalidOid returns false.
/// @return true if the relation uses Bioseq. Release the relation handle,
/// keeping AccessShareLock until transaction end. A missing relation raises an error.
export bool
relation_oid_uses_column_table_access_method(Oid relation_oid)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: relation_oid_uses_column_table_access_method() entered");
	if (!OidIsValid(relation_oid))
	{
		return false;
	}

	/* Keep the relation from being dropped during this check. */
	Relation relation = bioseq::postgres::support::open_postgres_relation(
		relation_oid, AccessShareLock);
	const bool result = relation_uses_column_table_access_method(relation);
	relation_close(relation, NoLock);

	return result;
}
