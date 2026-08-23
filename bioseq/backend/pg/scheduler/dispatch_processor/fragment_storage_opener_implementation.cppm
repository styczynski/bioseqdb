/**
 * @file fragment_storage_opener_implementation.cppm
 * @brief Implements the typed scanner-row fragment-storage opener.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstdint>
#include <string_view>

module bioseq.postgres.scheduler.dispatch_processor.fragment_storage_opener;

import bioseq.postgres.storage.indexing.fragment_handle;

namespace bioseq::simd::typed_dispatch::typed_support_detail {

[[nodiscard]] bioseq::storage::indexing::fragment_index_storage
open_typed_dispatch_fragment_storage(const Oid user_relid,
                                     const Oid database_oid,
                                     const Oid tablespace_oid,
                                     const Oid main_relfilenode,
                                     const std::int16_t column_attno,
                                     const std::string_view index_name,
                                     const bioseq::storage::sequence_locator& locator,
                                     const std::uint64_t sequence_rowno,
                                     bioseq::storage::seq_caches* caches,
                                     MemoryContext parent_mctx) {
    return bioseq::storage::indexing::open_fragment_storage(user_relid,
                                                            database_oid,
                                                            tablespace_oid,
                                                            main_relfilenode,
                                                            column_attno,
                                                            index_name,
                                                            locator,
                                                            sequence_rowno,
                                                            caches,
                                                            parent_mctx);
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
