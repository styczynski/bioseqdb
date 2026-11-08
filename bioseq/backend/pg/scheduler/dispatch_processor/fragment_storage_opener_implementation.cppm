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

import bioseq.indexing.hash_major_minimizer_index;
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

namespace {
/**
 * @brief Recognize a valid native-generation table unsupported by this direct reader.
 * Check the exact relation identity and existing generation schema contract without
 * reading payload rows. Missing, direct, or malformed tables remain subject to the
 * ordinary fragment reader's checks; only a valid native layout permits recompute.
 */
[[nodiscard]] bool uses_native_generation_storage(
    const bioseq::storage::indexing::fragment_index_storage& storage) {
    const Oid relation_oid = storage.resolve_relid_(false);
    if (!OidIsValid(relation_oid)) {
        return false;
    }
    const std::string relation_name = storage.this_table_name();
    const Oid namespace_oid = bioseq::storage::indexing::bioseq_internal_ns();
    const Oid tablespace_oid = storage.tablespace_oid_;
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_oid, namespace_oid, tablespace_oid, &relation_name] {
            Relation relation = bioseq::postgres::support::try_open_postgres_relation(
                relation_oid, AccessShareLock);
            if (relation == nullptr) {
                return false;
            }
            const auto schema =
                bioseq::storage::indexing::inspect_minimizer_generation_schema(
                    relation,
                    nullptr,
                    {.namespace_oid = namespace_oid,
                     .relation_name = relation_name,
                     .tablespace_oid = tablespace_oid});
            relation_close(relation, AccessShareLock);
            return static_cast<bool>(schema);
        });
}

/** Reopen the concrete reader only inside its owning PG implementation boundary. */
[[nodiscard]] bioseq::indexing::minimizer_hashmap_fragment_index
assemble_fragment_for_canonical_query(
    void* storage_owner,
    const bioseq::indexing::canonical_query_hash_view query_minimizers,
    const std::size_t frag_lo,
    const std::size_t frag_hi,
    const bioseq::storage::sequence_size_t frag_bp,
    const bioseq::storage::sequence_size_t total_bp,
    const bioseq::storage::sequence_size_t clip_lo_bp,
    const bioseq::storage::sequence_size_t clip_hi_bp,
    bioseq::indexing::minimizer_assembly_scratch& scratch) {
    auto& storage =
        *static_cast<bioseq::storage::indexing::fragment_index_storage*>(storage_owner);
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

/** Invoke the shared sequence-bucket assembly with native-width query hashes. */
[[nodiscard]] bioseq::indexing::minimizer_hashmap_fragment_index
assemble_sequence_for_canonical_query(
    void* storage_owner,
    const bioseq::indexing::canonical_query_hash_view query_minimizers,
    const bioseq::storage::sequence_size_t target_len,
    const std::int32_t max_occ,
    const bioseq::storage::sequence_size_t clip_lo_bp,
    const bioseq::storage::sequence_size_t clip_hi_bp,
    bioseq::indexing::minimizer_assembly_scratch& scratch) {
    auto& storage =
        *static_cast<bioseq::storage::indexing::fragment_index_storage*>(storage_owner);
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

} // namespace

const typed_dispatch_fragment_read_view::canonical_query_methods&
typed_dispatch_fragment_read_view::canonical_query_methods_() {
    static const canonical_query_methods methods{
        .fragment = assemble_fragment_for_canonical_query,
        .sequence = assemble_sequence_for_canonical_query,
    };
    return methods;
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
        // Optional CPU chunk reads have no native-generation owner. Leave the
        // caller unassembled so its existing admitted recompute path and source
        // statistics remain authoritative. Required native readers do not use
        // this opener, and malformed schemas still reach the strict chunk reader.
        if (!uses_native_generation_storage(storage)) {
            callback(callback_context, view);
        }
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
