/**
 * @file fragment_handle.cppm
 * @brief Opens the PostgreSQL storage handle used by fragment index readers.
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

/// Concrete PostgreSQL storage owner used by fragment-index readers and writers.
using fragment_storage_handle = fragment_index_storage;

/**
 * @brief Creates the child memory context owned by an index handle.
 *
 * @param[in] parent Parent context, or null to use the current context.
 * @return A newly allocated PostgreSQL memory context that the handle must
 * close or abandon before destruction.
 */
[[nodiscard]] MemoryContext make_handle_ctx(MemoryContext parent, const char* tag);

/**
 * @brief Opens a fragment-scoped index storage handle for one locator.
 *
 * @param[in] user_relid User relation whose index metadata is addressed.
 * @param[in] db Database OID in the physical storage identity.
 * @param[in] spc Tablespace OID in the physical storage identity.
 * @param[in] relfn Main relation relfilenode in the storage identity.
 * @param[in] attno One-based PostgreSQL column number.
 * @param[in] index_name Stable index policy name borrowed for the handle lifetime.
 * @param[in] loc Locator that selects the fragment bucket and shard.
 * @param[in] sequence_rowno Logical sequence row number used as the query key.
 * @param[in] caches Borrowed relation cache state, which must outlive the handle.
 * @param[in] mctx Parent for the handle-owned child memory context.
 * @return A handle that requires checked close on success or explicit
 * abandonment after a PostgreSQL error.
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

/** @brief Opens a sequence-scoped index storage handle for one locator. */
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

/** @brief Opens a table-scoped index storage handle. */
[[nodiscard]] table_index_storage open_table_storage_handle(Oid user_relid,
                                                            Oid db,
                                                            Oid spc,
                                                            Oid relfn,
                                                            std::int16_t attno,
                                                            std::string_view index_name,
                                                            seq_caches* caches,
                                                            MemoryContext mctx);

} // namespace bioseq::storage::indexing
