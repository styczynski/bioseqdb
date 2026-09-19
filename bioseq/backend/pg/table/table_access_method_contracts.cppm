/**
 * @file
 * @brief Declares the Bioseq table access-method identity without implementation fanout.
 */
module;
struct TableAmRoutine;

export module bioseq.postgres.table:table_access_method_contracts;

/** @brief Return the Bioseq PostgreSQL table-access callback table. */
export const TableAmRoutine* column_table_access_method(void);
