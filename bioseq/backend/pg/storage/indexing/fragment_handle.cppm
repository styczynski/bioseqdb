/**
 * @file fragment_handle.cppm
 * @brief Create PostgreSQL storage handles for fragment, sequence and table indexes.
 */
module;

extern "C" {
#include "postgres.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

export module bioseq.postgres.storage.indexing.fragment_handle;

import std;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.manager;
import bioseq.storage.layout;

export namespace bioseq::storage::indexing {

/// PostgreSQL storage handle for reading and writing fragment indexes.
using fragment_storage_handle = fragment_index_storage;

/**
 * @brief Allocate a child memory context for an index handle.
 * @param[in] parent Parent context, or null to use CurrentMemoryContext.
 * @return The new context. Its owner must delete it or leave cleanup to PostgreSQL after an error.
 */
[[nodiscard]] MemoryContext make_handle_ctx(MemoryContext parent, const char* tag);

/**
 * @brief Create a handle for one sequence's fragment index.
 *
 * The handle owns a child memory context, so call close_checked() when finished,
 * or abandon_after_pg_error_noexcept() after a PostgreSQL error, and keep its
 * borrowed index name and caches alive until then.
 *
 * @param[in] user_relid User table OID.
 * @param[in] db Database OID.
 * @param[in] spc Tablespace OID.
 * @param[in] relfn Main table file number.
 * @param[in] attno One-based sequence column number.
 * @param[in] index_name Index policy name.
 * @param[in] loc Sequence locator selecting the bucket and shard.
 * @param[in] sequence_rowno Sequence row number used to find its index rows.
 * @param[in] caches Borrowed relation caches.
 * @param[in] mctx Parent memory context, or null for CurrentMemoryContext.
 * @return A handle ready to read or write index chunks.
 */
[[nodiscard]] fragment_storage_handle open_fragment_storage(Oid user_relid,
                                                            Oid db,
                                                            Oid spc,
                                                            Oid relfn,
                                                            std::int16_t attno,
                                                            std::string_view index_name,
                                                            const sequence_locator& loc,
                                                            std::uint64_t sequence_rowno,
                                                            seq_caches* caches,
                                                            MemoryContext mctx);

/** @brief Create a handle for the index of one sequence identified by its locator and row number. */
[[nodiscard]] sequence_index_storage open_sequence_storage_handle(
    Oid user_relid,
    Oid db,
    Oid spc,
    Oid relfn,
    std::int16_t attno,
    std::string_view index_name,
    const sequence_locator& loc,
    std::uint64_t sequence_rowno,
    seq_caches* caches,
    MemoryContext mctx);

/** @brief Create a handle for an index covering the sequence column of a table. */
[[nodiscard]] table_index_storage open_table_storage_handle(Oid user_relid,
                                                            Oid db,
                                                            Oid spc,
                                                            Oid relfn,
                                                            std::int16_t attno,
                                                            std::string_view index_name,
                                                            seq_caches* caches,
                                                            MemoryContext mctx);

} // namespace bioseq::storage::indexing
