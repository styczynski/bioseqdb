/**
 * @file fragment_storage_opener.cppm
 * @brief Declares the fragment-storage opener used only by typed scanner rows.
 */
module;

extern "C" {
#include "postgres_ext.h"

typedef struct MemoryContextData* MemoryContext;
}

#include <cstdint>
#include <string_view>

export module bioseq.postgres.scheduler.dispatch_processor.fragment_storage_opener;

import std;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.manager;
import bioseq.storage.layout;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** Construct the concrete persisted-index fragment handle for one scanner row. */
[[nodiscard]] bioseq::storage::indexing::fragment_index_storage
open_typed_dispatch_fragment_storage(Oid user_relid,
                                     Oid database_oid,
                                     Oid tablespace_oid,
                                     Oid main_relfilenode,
                                     std::int16_t column_attno,
                                     std::string_view index_name,
                                     const bioseq::storage::sequence_locator& locator,
                                     std::uint64_t sequence_rowno,
                                     bioseq::storage::seq_caches* caches,
                                     MemoryContext parent_mctx);

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
