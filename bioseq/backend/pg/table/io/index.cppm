/**
 * @file index.cppm
 * @brief Aggregate interface for PostgreSQL-backed Bioseq table I/O.
 *
 * @details Re-exports chunk buffers, predicate pruning, decoding, reader and
 * writer lifecycles, and the backend-local chunk cache. Callers must observe
 * the PostgreSQL memory-context and relation lifetimes of selected operations.
 */
/** @brief Stable module symbol re-exporting all table-I/O partitions. */
export module bioseq.postgres.table.io;
export import :buffers;
export import :pruning;
export import :read_chunk;
export import :reader;
export import :writer;
export import :cache;
