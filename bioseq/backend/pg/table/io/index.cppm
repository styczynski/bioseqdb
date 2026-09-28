/**
 * @file index.cppm
 * @brief Read and write Bioseq columnar tables.
 *
 * Exports the table reader, writer, chunk decoding and filtering helpers,
 * and the cache of decompressed columns.
 */
/** @brief Export the table I/O modules through one interface. */
export module bioseq.postgres.table.io;
export import :buffers;
export import :pruning;
export import :read_chunk;
export import :reader;
export import :writer;
export import :cache;
