/**
 * @file fragment_storage_opener_implementation.cppm
 * @brief Open, use, and close the fragment reader behind the scanner callback.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstdint>
#include <new>
#include <string_view>

module bioseq.postgres.scheduler.dispatch_processor.fragment_storage_opener;

import std;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.postgres.support;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.fragment_handle;

namespace bioseq::simd::typed_dispatch::typed_support_detail {

[[nodiscard]] std::size_t typed_dispatch_fragment_read_view::number_of_chunks(
    const bioseq::indexing::index_sub_key_t sub_key) const {
    auto& storage =
        *static_cast<bioseq::storage::indexing::fragment_index_storage*>(storage_);
    return storage.number_of_chunks(sub_key);
}

[[nodiscard]] std::size_t typed_dispatch_fragment_read_view::chunk_byte_size(
    const bioseq::indexing::index_sub_key_t sub_key, const std::size_t chunk_no) const {
    auto& storage =
        *static_cast<bioseq::storage::indexing::fragment_index_storage*>(storage_);
    return storage.chunk_byte_size(sub_key, chunk_no);
}

[[nodiscard]] bioseq::indexing::minimizer_hashmap_fragment_index
typed_dispatch_fragment_read_view::assemble_fragment_segment_for_query(
    const std::span<const bioseq::minimizers::minimap_sketch_data> query_minimizers,
    const std::size_t frag_lo,
    const std::size_t frag_hi,
    const bioseq::storage::sequence_size_t frag_bp,
    const bioseq::storage::sequence_size_t total_bp,
    const bioseq::storage::sequence_size_t clip_lo_bp,
    const bioseq::storage::sequence_size_t clip_hi_bp,
    bioseq::indexing::minimizer_assembly_scratch& scratch) {
    auto& storage =
        *static_cast<bioseq::storage::indexing::fragment_index_storage*>(storage_);
    return bioseq::indexing::minimizers_per_fragment_index<
        bioseq::storage::indexing::fragment_index_storage>::assemble_segment_for_query(
        storage,
        query_minimizers,
        frag_lo,
        frag_hi,
        frag_bp,
        total_bp,
        clip_lo_bp,
        clip_hi_bp,
        scratch);
}

[[nodiscard]] bioseq::indexing::minimizer_hashmap_fragment_index
typed_dispatch_fragment_read_view::assemble_sequence_segment_for_query(
    const std::span<const bioseq::minimizers::minimap_sketch_data> query_minimizers,
    const bioseq::storage::sequence_size_t target_len,
    const std::int32_t max_occ,
    const bioseq::storage::sequence_size_t clip_lo_bp,
    const bioseq::storage::sequence_size_t clip_hi_bp,
    bioseq::indexing::minimizer_assembly_scratch& scratch) {
    auto& storage =
        *static_cast<bioseq::storage::indexing::fragment_index_storage*>(storage_);
    return bioseq::indexing::direct_minimizer_bucket_index<
        bioseq::storage::indexing::fragment_index_storage>::assemble_for_query_segment(
        storage,
        query_minimizers,
        target_len,
        max_occ,
        clip_lo_bp,
        clip_hi_bp,
        scratch);
}

void
open_typed_dispatch_fragment_storage(const Oid user_relid,
                                     const Oid database_oid,
                                     const Oid tablespace_oid,
                                     const Oid main_relfilenode,
                                     const std::int16_t column_attno,
                                     const std::string_view index_name,
                                     const bioseq::storage::sequence_locator& locator,
                                     const std::uint64_t sequence_rowno,
                                     bioseq::storage::seq_caches* caches,
                                     MemoryContext parent_mctx,
                                     void* callback_context,
                                     const typed_dispatch_fragment_read_callback callback) {
    auto storage = bioseq::storage::indexing::open_fragment_storage(user_relid,
                                                                    database_oid,
                                                                    tablespace_oid,
                                                                    main_relfilenode,
                                                                    column_attno,
                                                                    index_name,
                                                                    locator,
                                                                    sequence_rowno,
                                                                    caches,
                                                                    parent_mctx);
    typed_dispatch_fragment_read_view view{std::addressof(storage)};
    try {
        callback(callback_context, view);
    } catch (const bioseq::postgres::support::postgres_error&) {
        storage.abandon_after_pg_error_noexcept();
        throw;
    } catch (...) {
        try {
            storage.close_checked();
        } catch (...) {
            storage.abandon_after_pg_error_noexcept();
        }
        throw;
    }
    try {
        storage.close_checked();
    } catch (...) {
        storage.abandon_after_pg_error_noexcept();
        throw;
    }
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
