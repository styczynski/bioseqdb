/**
 * @file
 * Declare the Bioseq table access method for callers that only need its identity.
 */
module;
struct TableAmRoutine;

export module bioseq.postgres.table:table_access_method_contracts;

/** @brief Return the Bioseq PostgreSQL table-access callback table. */
export const TableAmRoutine* column_table_access_method(void);
