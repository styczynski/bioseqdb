/**
 * @file
 * @brief Primary Bioseq PostgreSQL table module interface.
 */
export module bioseq.postgres.table;

/* Re-export only the lightweight declaration partition. Implementation
 * partitions remain behind their existing module boundaries. */
export import :api;
