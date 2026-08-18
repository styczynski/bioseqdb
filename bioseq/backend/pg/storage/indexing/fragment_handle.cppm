/**
 * @file fragment_handle.cppm
 * @brief Opens the PostgreSQL storage handle used by fragment index readers.
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/memutils.h"
}

export module bioseq.postgres.storage.indexing.fragment_handle;

import std;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.manager;
import bioseq.postgres.storage.indexing.chunked_storage;

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
[[nodiscard]] inline MemoryContext make_handle_ctx(MemoryContext parent, const char* /*tag*/) {
    const MemoryContext owning_parent = parent ? parent : CurrentMemoryContext;
    return bioseq::postgres::support::invoke_pg_as_cpp([owning_parent] {
        return AllocSetContextCreate(owning_parent, "bioseq_index_handle", ALLOCSET_SMALL_SIZES);
    });
}

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
[[nodiscard]] inline fragment_storage_handle open_fragment_storage(Oid user_relid,
                                                                   Oid db,
                                                                   Oid spc,
                                                                   Oid relfn,
                                                                   std::int16_t attno,
                                                                   std::string_view index_name,
                                                                   const sequence_locator& loc,
                                                                   std::uint64_t sequence_rowno,
                                                                   seq_caches* caches,
                                                                   MemoryContext mctx) {
    // The returned handle owns an open child MemoryContext. Normal callers
    // must invoke close_checked(); failure paths must invoke
    // abandon_after_pg_error_noexcept() before destruction. The destructor is
    // deliberately zero-PostgreSQL and asserts this ownership transition.
    fragment_storage_handle storage;
    storage.db_oid_ = db;
    storage.tablespace_oid_ = spc;
    storage.main_relfilenode_ = relfn;
    storage.user_relid_ = user_relid;
    storage.read_caches_ = caches;
    storage.column_attno_ = attno;
    storage.index_name_ = index_name;
    storage.bucket_idx_ = static_cast<std::uint8_t>(loc.bucket_index);
    storage.shard_idx_ = loc.shard_index;
    storage.query_key_ = sequence_rowno;
    storage.own_mctx_ = make_handle_ctx(mctx, "idx_fragment_handle");
    storage.run_buffer_override_bytes_ =
        bioseq::settings::index_build::run_buffer_bytes > 0
            ? static_cast<std::size_t>(bioseq::settings::index_build::run_buffer_bytes)
            : 0;
    return storage;
}

} // namespace bioseq::storage::indexing
