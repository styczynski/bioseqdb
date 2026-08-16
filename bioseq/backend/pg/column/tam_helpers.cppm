/**
 * @file tam_helpers.cppm
 * @brief Cycle-free table-access-method identity helpers.
 *
 * Leaf module for the small Bioseq table-AM helper functions that several
 * modules (the scheduler dispatch buffer, the *_pg storage-indexing
 * modules, the extension entry module, and the custom-scan partition)
 * need to reach without importing the full bioseq_table module — which
 * would otherwise create an import cycle.
 *
 * The TableAmRoutine identity used by relation_oid_uses_column_table_access_method is supplied
 * by the table-AM initializer. Comparing the registered routine pointer keeps
 * this leaf independent from the full table-access implementation.
 */
module;
#include <bioseq/pg_compat.h>

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


/// @brief Encodes a column row number as a PostgreSQL item pointer.
/// @param[in] row_number Valid one-based column row number.
/// @return PostgreSQL item pointer containing the row identity.
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

/// @brief Records the installed Bioseq table-access routine identity.
/// @param[in] routine Process-lifetime routine whose address identifies Bioseq relations.
export void
register_column_table_access_method(const TableAmRoutine* routine) noexcept
{
	registered_column_table_access_method = routine;
}

/** @brief Tests an already-open relation for the Bioseq table-access method.
 *
 * Callers retain their existing relcache pin and relation lock. Reusing that
 * exact handle is essential during dependency deletion: reopening an OID from
 * PostgreSQL's precomputed DROP list can fail after an earlier recursive drop
 * has already removed the catalog row.
 *
 * @param[in] relation Borrowed relation handle, or `nullptr`.
 * @return `true` when the handle uses the registered Bioseq routine.
 */
export bool
relation_uses_column_table_access_method(Relation relation) noexcept
{
	return relation != nullptr
		&& registered_column_table_access_method != nullptr
		&& relation->rd_tableam == registered_column_table_access_method;
}


/// @brief Opens an OID and tests it for the Bioseq table-access method.
/// @param[in] relation_oid Relation to inspect without retaining a lock or relcache pin.
/// @return False for invalid OIDs or relations using another access method.
export bool
relation_oid_uses_column_table_access_method(Oid relation_oid)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: relation_oid_uses_column_table_access_method() entered");
	if (!OidIsValid(relation_oid))
	{
		return false;
	}

	/*
	 * Lock relation to prevent it from being dropped &
	 * avoid race conditions.
	 */
	Relation relation = bioseq::postgres::support::open_postgres_relation(
		relation_oid, AccessShareLock);
	const bool result = relation_uses_column_table_access_method(relation);
	relation_close(relation, NoLock);

	return result;
}
