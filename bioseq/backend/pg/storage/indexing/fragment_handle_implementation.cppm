/**
 * @file fragment_handle_implementation.cppm
 * @brief Create index storage handles and their child memory contexts.
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/memutils.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

module bioseq.postgres.storage.indexing.fragment_handle;

import std;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.manager;

namespace bioseq::storage::indexing {

[[nodiscard]] MemoryContext make_handle_ctx(MemoryContext parent, const char* /*tag*/) {
    const MemoryContext owning_parent = parent ? parent : CurrentMemoryContext;
    return bioseq::postgres::support::invoke_pg_as_cpp([owning_parent] {
        return AllocSetContextCreate(owning_parent, "bioseq_index_handle", ALLOCSET_SMALL_SIZES);
    });
}

[[nodiscard]] fragment_storage_handle open_fragment_storage(Oid user_relid,
                                                            Oid db,
                                                            Oid spc,
                                                            Oid relfn,
                                                            std::int16_t attno,
                                                            std::string_view index_name,
                                                            const sequence_locator& loc,
                                                            std::uint64_t sequence_rowno,
                                                            seq_caches* caches,
                                                            MemoryContext mctx) {
    // The handle owns this child memory context. Call close_checked() on success
    // or abandon_after_pg_error_noexcept() after a PostgreSQL error. Its destructor
    // checks that the context pointer has been cleared.
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
    MemoryContext mctx) {
    sequence_index_storage storage;
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
    storage.own_mctx_ = make_handle_ctx(mctx, "idx_sequence_handle");
    return storage;
}

[[nodiscard]] table_index_storage open_table_storage_handle(Oid user_relid,
                                                            Oid db,
                                                            Oid spc,
                                                            Oid relfn,
                                                            std::int16_t attno,
                                                            std::string_view index_name,
                                                            seq_caches* caches,
                                                            MemoryContext mctx) {
    table_index_storage storage;
    storage.db_oid_ = db;
    storage.tablespace_oid_ = spc;
    storage.main_relfilenode_ = relfn;
    storage.user_relid_ = user_relid;
    storage.read_caches_ = caches;
    storage.column_attno_ = attno;
    storage.index_name_ = index_name;
    storage.own_mctx_ = make_handle_ctx(mctx, "idx_table_handle");
    return storage;
}

} // namespace bioseq::storage::indexing
