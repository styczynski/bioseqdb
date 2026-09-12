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
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.type;
import bioseq.postgres.storage.manager;
import bioseq.storage.layout;
import bioseq.storage.size;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** Scope-bound view of one concrete persisted-index fragment reader. */
class typed_dispatch_fragment_read_view;

/** Synchronous callback invoked while the concrete fragment reader remains alive. */
using typed_dispatch_fragment_read_callback =
    void (*)(void* context, typed_dispatch_fragment_read_view& storage);

/**
 * Open one concrete persisted-index fragment reader, invoke @p callback once,
 * and close or abandon the reader before returning. The view must not escape
 * the synchronous callback.
 */
void
open_typed_dispatch_fragment_storage(Oid user_relid,
                                     Oid database_oid,
                                     Oid tablespace_oid,
                                     Oid main_relfilenode,
                                     std::int16_t column_attno,
                                     std::string_view index_name,
                                     const bioseq::storage::sequence_locator& locator,
                                     std::uint64_t sequence_rowno,
                                     bioseq::storage::seq_caches* caches,
                                     MemoryContext parent_mctx,
                                     void* callback_context,
                                     typed_dispatch_fragment_read_callback callback);

/**
 * Narrow non-owning facade for the coarse persisted-index operations used by a
 * typed scanner row. The opener retains all concrete lifetime and cleanup authority.
 */
class typed_dispatch_fragment_read_view final {
  public:
    typed_dispatch_fragment_read_view(const typed_dispatch_fragment_read_view&) = delete;
    typed_dispatch_fragment_read_view&
    operator=(const typed_dispatch_fragment_read_view&) = delete;
    typed_dispatch_fragment_read_view(typed_dispatch_fragment_read_view&&) = delete;
    typed_dispatch_fragment_read_view& operator=(typed_dispatch_fragment_read_view&&) = delete;

    [[nodiscard]] std::size_t
    number_of_chunks(bioseq::indexing::index_sub_key_t sub_key) const;

    [[nodiscard]] std::size_t
    chunk_byte_size(bioseq::indexing::index_sub_key_t sub_key, std::size_t chunk_no) const;

    [[nodiscard]] bioseq::indexing::minimizer_hashmap_fragment_index
    assemble_fragment_segment_for_query(
        std::span<const bioseq::minimizers::minimap_sketch_data> query_minimizers,
        std::size_t frag_lo,
        std::size_t frag_hi,
        bioseq::storage::sequence_size_t frag_bp,
        bioseq::storage::sequence_size_t total_bp,
        bioseq::storage::sequence_size_t clip_lo_bp,
        bioseq::storage::sequence_size_t clip_hi_bp,
        bioseq::indexing::minimizer_assembly_scratch& scratch);

    [[nodiscard]] bioseq::indexing::minimizer_hashmap_fragment_index
    assemble_sequence_segment_for_query(
        std::span<const bioseq::minimizers::minimap_sketch_data> query_minimizers,
        bioseq::storage::sequence_size_t target_len,
        std::int32_t max_occ,
        bioseq::storage::sequence_size_t clip_lo_bp,
        bioseq::storage::sequence_size_t clip_hi_bp,
        bioseq::indexing::minimizer_assembly_scratch& scratch);

  private:
    explicit typed_dispatch_fragment_read_view(void* storage) noexcept : storage_(storage) {}

    void* storage_ = nullptr;

    friend void open_typed_dispatch_fragment_storage(
        Oid user_relid,
        Oid database_oid,
        Oid tablespace_oid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        std::string_view index_name,
        const bioseq::storage::sequence_locator& locator,
        std::uint64_t sequence_rowno,
        bioseq::storage::seq_caches* caches,
        MemoryContext parent_mctx,
        void* callback_context,
        typed_dispatch_fragment_read_callback callback);
};

static_assert(sizeof(typed_dispatch_fragment_read_view) == sizeof(void*));
static_assert(!std::is_copy_constructible_v<typed_dispatch_fragment_read_view>);
static_assert(!std::is_copy_assignable_v<typed_dispatch_fragment_read_view>);
static_assert(!std::is_move_constructible_v<typed_dispatch_fragment_read_view>);
static_assert(!std::is_move_assignable_v<typed_dispatch_fragment_read_view>);

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
