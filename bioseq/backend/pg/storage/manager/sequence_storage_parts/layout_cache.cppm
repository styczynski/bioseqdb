/**
 * @file layout_cache.cppm
 * @brief Locator layout, cache ownership and storage declarations.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"
#include "utils/resowner.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/indexing.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/extension.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/vacuum.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
#include "utils/tuplestore.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.storage.manager:sequence_storage_layout_cache;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.storage;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.table.io;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.indexing.storage;
import bioseq.common.seqan_adapter;  // ceil_log2 used in letters_per_unit_for
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
export import bioseq.postgres.storage.manager.locator_stub;
// The PG-free bucket math and sequence_locator value live in their own
// module so the unit-test TU can link them without the Postgres backend.
// Re-exported here so existing `import bioseq_storage` consumers are unchanged.
import bioseq.storage.layout;
import :sequence_storage_catalog;

export namespace bioseq::storage {

// Current locator stub representation. A 56-byte bioseq::pg_types::seq-shaped
// varlena lives in the main columnar relation in place of the full sequence
// payload. No alternate on-disk representation is accepted.
// ---------------------------------------------------------------------------

// `sequence_locator_payload_version` and the 40-byte `sequence_locator` value
// live in the PG-free `bioseq.storage.layout` module so the unit-test TU can
// link them. They are re-exported above, so every use below resolves unchanged.

/// Check whether a sequence varlena carries the canonical locator width marker.
/// @param hdr Borrowed varlena pointer, which may be null.
/// @return Whether `hdr` is non-null and has the locator marker.
[[nodiscard]] [[gnu::always_inline]]
inline bool is_locator_stub(const bioseq::pg_types::seq* hdr) noexcept {
    return hdr != nullptr
        && is_sequence_locator_stub_width(
            static_cast<std::uint8_t>(hdr->storage_unit_byte_width));
}

/// Remove the locator marker and return the logical packed-unit byte width.
/// @param hdr Borrowed non-null sequence header.
/// @return Packed storage-unit width in bytes.
[[nodiscard]] [[gnu::always_inline]]
inline std::uint8_t logical_storage_unit_byte_width(const bioseq::pg_types::seq* hdr) noexcept {
    return static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(hdr->storage_unit_byte_width) & 0x0Fu);
}

/// Decode and validate the canonical locator payload from one stub.
/// @param hdr Borrowed sequence header containing a locator stub.
/// @return Decoded locator value.
/// @throws PostgreSQL ERROR if the marker, varlena size, or wire payload is invalid.
[[nodiscard]] [[gnu::always_inline]]
inline sequence_locator decode_locator_from_stub(const bioseq::pg_types::seq* hdr) {
    if (!is_locator_stub(hdr)) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: decode_locator_from_stub called on a "
               "non-stub value (sub_w={}). Caller must guard with "
               "is_locator_stub(hdr) first.",
               static_cast<unsigned>(hdr->storage_unit_byte_width));
    }
    if (bioseq::postgres::support::varlena_4byte_size(hdr) != k_locator_stub_varlena_bytes) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: locator stub size mismatch (got {}, expected {})",
               static_cast<unsigned long long>(bioseq::postgres::support::varlena_4byte_size(hdr)),
               static_cast<unsigned long long>(k_locator_stub_varlena_bytes));
    }
    const auto payload = std::span<std::byte const>{
        reinterpret_cast<std::byte const*>(hdr->data), sizeof(sequence_locator)};
    const auto decoded = decode_sequence_locator_payload(payload);
    if (!decoded.has_value()) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: locator payload is not canonical revision-3 data");
    }
    return *decoded;
}

/* Raw locator consumers that hydrate a contiguous bioseq::pg_types::seq must reject every
 * narrowing and row-range violation before opening a shard, doing allocation
 * arithmetic, or converting catalog-width fields. */
/// Validate that one locator can be safely flattened and addressed in SQL.
/// @param loc Locator whose lengths and row range are checked.
/// @throws PostgreSQL ERROR when any value exceeds its SQL or allocation domain
/// or the fragment geometry is inconsistent.
[[gnu::always_inline]]
inline void validate_locator_flat_materialization_length(
    const sequence_locator& loc)
{
    using error = sequence_locator_validation_error;
    switch (validate_sequence_locator(loc)) {
        case error::none:
            return;
        case error::logical_length_exceeds_sql_bigint:
            ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq locator logical length exceeds bigint")));
        case error::logical_length_exceeds_flat_storage:
            ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq sequence is too long to flatten")));
        case error::invalid_payload_metadata:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator wire metadata is invalid")));
        case error::shard_index_exceeds_catalog:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator shard exceeds the catalog domain")));
        case error::first_fragment_row_exceeds_catalog:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator first fragment row exceeds bigint")));
        case error::inconsistent_fragment_geometry:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator fragment geometry is inconsistent"),
                 errdetail("logical length=%llu fragments=%u first row=%llu",
                     static_cast<unsigned long long>(loc.logical_length),
                     loc.fragment_count,
                     static_cast<unsigned long long>(loc.first_fragment_row))));
        case error::fragment_row_range_exceeds_catalog:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator fragment row range exceeds bigint")));
    }
    pg_unreachable();
}

/// Validate one fragment varlena frame against its expected physical size.
/// @param raw_bytes Observed varlena size in bytes.
/// @param expected_bytes Required varlena size in bytes.
/// @param row_no Physical fragment row number used for diagnostics.
/// @throws PostgreSQL ERROR when the frame is short or has the wrong size.
[[gnu::always_inline]]
inline void validate_sequence_fragment_frame(
    Size raw_bytes, Size expected_bytes, std::uint64_t row_no)
{
    using error = fragment_frame_error;
    switch (validate_fragment_frame(
        static_cast<std::uint64_t>(raw_bytes),
        static_cast<std::uint64_t>(bioseq::pg_types::seq::header_bytes()),
        static_cast<std::uint64_t>(expected_bytes))) {
        case error::none:
            return;
        case error::shorter_than_header:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq fragment row is shorter than its sequence header"),
                 errdetail("fragment row=%llu bytes=%zu header bytes=%zu",
                     static_cast<unsigned long long>(row_no), raw_bytes,
                     bioseq::pg_types::seq::header_bytes())));
        case error::size_mismatch:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq fragment row has an invalid physical size"),
                 errdetail("fragment row=%llu bytes=%zu expected bytes=%zu",
                     static_cast<unsigned long long>(row_no), raw_bytes,
                     expected_bytes)));
    }
    pg_unreachable();
}

/// Validate one fragment's packed type, logical length and padding.
/// @param raw Borrowed fragment header.
/// @param expected_alphabet Required alphabet code.
/// @param expected_width Required packed-unit width in bytes.
/// @param expected_bp_size Required logical length in letters.
/// @param expected_padding Required unused-letter count.
/// @param row_no Physical fragment row number used for diagnostics.
/// @throws PostgreSQL ERROR when persisted metadata differs from the locator geometry.
[[gnu::always_inline]]
inline void validate_sequence_fragment_metadata(
    const bioseq::pg_types::seq* raw,
    std::uint8_t expected_alphabet,
    std::uint8_t expected_width,
    std::uint64_t expected_bp_size,
    std::uint64_t expected_padding,
    std::uint64_t row_no)
{
    using error = fragment_metadata_error;
    switch (validate_fragment_metadata(
        static_cast<std::uint8_t>(raw->alphabet_code),
        static_cast<std::uint8_t>(raw->storage_unit_byte_width),
        static_cast<std::uint64_t>(raw->bp_size),
        static_cast<std::uint64_t>(raw->padding_letters),
        expected_alphabet, expected_width, expected_bp_size,
        expected_padding)) {
        case error::none:
            return;
        case error::type_mismatch:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq fragment row type does not match its sequence"),
                 errdetail("fragment row=%llu alphabet=%u width=%u expected alphabet=%u width=%u",
                     static_cast<unsigned long long>(row_no),
                     static_cast<unsigned>(raw->alphabet_code),
                     static_cast<unsigned>(raw->storage_unit_byte_width),
                     static_cast<unsigned>(expected_alphabet),
                     static_cast<unsigned>(expected_width))));
        case error::logical_length_mismatch:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq fragment row has an invalid logical length"),
                 errdetail("fragment row=%llu length=%llu expected length=%llu",
                     static_cast<unsigned long long>(row_no),
                     static_cast<unsigned long long>(raw->bp_size),
                     static_cast<unsigned long long>(expected_bp_size))));
        case error::padding_mismatch:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq fragment row has invalid padding"),
                 errdetail("fragment row=%llu padding=%u expected padding=%llu",
                     static_cast<unsigned long long>(row_no),
                     static_cast<unsigned>(raw->padding_letters),
                     static_cast<unsigned long long>(expected_padding))));
    }
    pg_unreachable();
}

// ---------------------------------------------------------------------------
// Bucket math. The consteval threshold builder, sequence-size bit-width
// calculation, branchless lookup, and sequence_bucket_index live in the
// PG-free `bioseq.storage.layout` module re-exported above. The template below
// delegates to that authority.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Per-scan / per-batch relation cache. Owned by column_scan_descriptor,
// index_fetch_state, column_write_state and tuple_dispatch_buffer.
// Always allocated separately (`palloc(sizeof(seq_caches)) + placement-new`)
// because std::map's non-standard-layout would otherwise spoil the surrounding
// POD struct's std::is_standard_layout_v guarantee.
// ---------------------------------------------------------------------------

/// Per-owner cache of fragment shard relations and column read or write state.
struct seq_caches {
    /// One cached shard relation with mutually exclusive read or write state.
    struct entry {
        /// Cache-owned open relation reference.
        Relation rel = nullptr;
        /// Column writer owned by this entry when the cache is write-side.
        column_write_state* write_state = nullptr;
        /// Column reader owned by this entry when the cache is read-side.
        column_read_state*  read_state  = nullptr;
        // Exact caller-owned snapshot used to create read_state. Registered
        // MVCC snapshots and permanent SnapshotAny may be retained; shorter-
        // lived special snapshots are served by a per-call reader instead.
        Snapshot read_snapshot_source = nullptr;
        /// Unpublished physical bytes accumulated by the writer.
        sequence_size_t pending_bytes_delta = 0;
        /// Unpublished logical-payload bytes accumulated by the writer.
        sequence_size_t pending_total_payload_delta = 0;
    };

    /// Sorted-vector map providing deterministic cache iteration and cleanup.
    struct entry_map {
        /// Packed cache key paired with its owned entry.
        using value_type = std::pair<std::uint64_t, entry>;
        /// Contiguous storage that preserves ascending key order.
        using storage_type = std::vector<value_type>;
        /// Mutable entry iterator.
        using iterator = storage_type::iterator;
        /// Immutable entry iterator.
        using const_iterator = storage_type::const_iterator;

        /// @return Iterator to the first cached entry.
        [[nodiscard]] iterator begin() noexcept { return entries_.begin(); }
        /// @return Past-the-end iterator for mutable traversal.
        [[nodiscard]] iterator end() noexcept { return entries_.end(); }
        /// @return Iterator to the first cached entry.
        [[nodiscard]] const_iterator begin() const noexcept { return entries_.begin(); }
        /// @return Past-the-end iterator for immutable traversal.
        [[nodiscard]] const_iterator end() const noexcept { return entries_.end(); }
        /// @return Number of cached shards.
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        /// Remove entries after their PostgreSQL resources have been closed.
        void clear() noexcept { entries_.clear(); }

        /// Find one mutable cache entry by its packed shard key.
        /// @return Matching iterator, or `end()` when absent.
        [[nodiscard]] iterator find(std::uint64_t key) noexcept {
            auto it = lower_bound(key);
            return it != entries_.end() && it->first == key ? it : entries_.end();
        }

        /// Find one immutable cache entry by its packed shard key.
        /// @return Matching iterator, or `end()` when absent.
        [[nodiscard]] const_iterator find(std::uint64_t key) const noexcept {
            auto it = lower_bound(key);
            return it != entries_.end() && it->first == key ? it : entries_.end();
        }

        /// Return or insert one cache entry while preserving key order.
        /// @return Mutable entry associated with `key`.
        entry& operator[](std::uint64_t key) {
            auto it = lower_bound(key);
            if (it == entries_.end() || it->first != key) {
                it = entries_.emplace(it, key, entry{});
            }
            return it->second;
        }

    private:
        /// Compare a stored key with a lookup key.
        struct less_by_key {
            /// @return Whether the stored key is less than `rhs`.
            [[nodiscard]] constexpr bool operator()(const value_type& lhs,
                                                   std::uint64_t rhs) const noexcept {
                return lhs.first < rhs;
            }
        };

        /// @return First mutable entry whose key is not less than `key`.
        [[nodiscard]] iterator lower_bound(std::uint64_t key) noexcept {
            return std::lower_bound(
                entries_.begin(), entries_.end(), key, less_by_key{});
        }

        /// @return First immutable entry whose key is not less than `key`.
        [[nodiscard]] const_iterator lower_bound(std::uint64_t key) const noexcept {
            return std::lower_bound(
                entries_.begin(), entries_.end(), key, less_by_key{});
        }

        storage_type entries_{};
    };

    // A sorted-vector map avoids per-node allocation, preserves deterministic
    // allocation, deterministic sorted iteration order for close_all, and
    // cache-friendly for the typical <50-entry working set. The hot paths only
    // do find / operator[] / clear, and never hold an `entry&` across an insert.
    /// Entries keyed by packed column, bucket, and shard identity.
    entry_map entries{};
    /// Relation lock mode used consistently by this cache.
    LOCKMODE lock_mode = AccessShareLock;

    /// Exclusive read-side or write-side cache ownership mode.
    enum class access_side { read, write };
    access_side side = access_side::read;

    /// Memory context owning cached reader state.
    MemoryContext read_state_mctx = nullptr;

    // Optional explicit owner for read-side caches whose lifetime is longer
    // than the executor scan that first opened them.  Ordinary scan caches
    // leave this null and continue to use CurrentResourceOwner.  Locator
    // source domains set it to their transaction-owned child so a retained
    // expanded datum can finish reading after the provider scan has ended.
    ResourceOwner read_resource_owner = nullptr;

    /// Write-side shard capacity snapshot in bytes.
    sequence_size_t cap_bytes_snapshot = std::numeric_limits<sequence_size_t>::max();

    Oid main_database_oid   = InvalidOid;
    Oid main_tablespace_oid = InvalidOid;
    Oid main_relfilenode    = InvalidOid;

    /// Pack a sequence column, bucket and shard into one deterministic key.
    /// @return Unsigned key containing the three fixed-width fields.
    [[nodiscard]] [[gnu::always_inline]]
    static constexpr std::uint64_t key_for(std::int16_t column_attno,
                                           std::int16_t bucket_idx,
                                           std::int32_t shard_idx) noexcept {
        return (static_cast<std::uint64_t>(
                    static_cast<std::uint16_t>(column_attno)) << 48)
             | (static_cast<std::uint64_t>(
                    static_cast<std::uint16_t>(bucket_idx))   << 32)
             |  static_cast<std::uint64_t>(
                    static_cast<std::uint32_t>(shard_idx));
    }

    /// Close all cached state through the commit or abort cleanup path.
    /// @param commit Whether pending writer metadata should be committed.
    void close_all(bool commit) noexcept;
};

// alloc_seq_caches / destroy_seq_caches own the placement-new dance so the
// dependent consumers (column_scan_descriptor, column_write_state, ...)
// never see a half-constructed map. The header type stays
// reinterpret_cast-safe via void* opaque pointers in the consumer structs.
//
// destroy_seq_caches is NOT inline because write_state_management.cppm
// forward-declares it to avoid `import bioseq_storage` (which would
// create a module cycle with bioseq_table). Out-of-line gives it external
// linkage so the linker resolves the forward declaration.
/// Allocate and construct one cache set inside a PostgreSQL memory context.
/// @param mctx Memory context that owns the returned allocation.
/// @return Constructed cache set owned by `mctx`.
[[nodiscard]] inline seq_caches* alloc_seq_caches(MemoryContext mctx) {
    bioseq::postgres::support::memory_context_scope ctx_guard{mctx};
    void* raw = palloc0(sizeof(seq_caches));
    return ::new (raw) seq_caches{};
}

/// Destroy a cache set after all owned PostgreSQL state has been closed.
/// @param caches Cache previously constructed by `alloc_seq_caches`.
void destroy_seq_caches(seq_caches* caches) noexcept;

/// Close every cache entry without destroying its enclosing allocation.
/// @param caches Cache whose owned PostgreSQL state is closed.
/// @param commit Whether pending writer metadata should be committed.
[[gnu::always_inline]] inline void close_seq_caches(seq_caches& caches,
                                                    bool commit) noexcept {
    caches.close_all(commit);
}

/// Close and destroy one separately allocated cache set.
/// @param caches Cache to close and destroy, or null.
/// @param commit Whether pending writer metadata should be committed.
void close_and_destroy_seq_caches(seq_caches* caches, bool commit) noexcept;

// Public alias for the per-fragment storage-unit count
// (detail::k_fragment_storage_units below aliases this so the two never drift).
// `bioseq.load`'s streaming sink uses it to size fragment payloads.
/// Packed storage-unit capacity of one physical sequence fragment.
inline constexpr std::size_t k_sequence_fragment_storage_units =
    std::size_t{1024};

// Flush every open sub-table write state owned by `caches` so an
// in-statement index scan (e.g. the batched update_index `bioseq.load` runs
// after streaming) sees the just-written fragment rows. Unlike `close_all`,
// this only flushes pending columnar writes (it does NOT pop/close the write
// state); the central xact-commit teardown still owns lifecycle.
/// Flush cached write states without transferring their cleanup ownership.
/// @param caches Caller-owned write cache, or null for no work.
/// @param current_sub_xid Subtransaction that owns the writer state.
void flush_seq_cache_write_states(seq_caches* caches,
                                  SubTransactionId current_sub_xid);

/// Bounded streaming fragment writer for one sequence. Accepts
/// column-native packed bytes, writes full fragment rows (each
/// `k_sequence_fragment_storage_units` storage units) as its internal buffer fills,
/// retains only the final partial fragment until `finish`, and returns the
/// current `sequence_locator`. Peak extra memory is one ≤8 KiB fragment buffer + one
/// `bioseq::pg_types::seq` fragment row, so a >MaxAllocSize record never materialises as one
/// buffer. Writes to one shard. A single sequence exceeding the sub-table cap
/// raises ERROR (transaction rollback cleans the partial rows).
struct sequence_stream_writer {
    /// Initialize one writer and reserve its immutable source epoch.
    void begin(Oid user_relid, Oid db_oid, Oid spc_oid,
               RelFileNumber main_relfilenode, std::uint64_t main_storage_id,
               AttrNumber seq_attno, TupleDesc parent_td,
               std::uint8_t alphabet_code, std::uint8_t byte_width,
               sequence_size_t cap_bytes, seq_caches* caches);
    /// Append packed bytes and emit every complete fragment immediately.
    void append_packed_bytes(std::span<const std::uint8_t> bytes,
                             sequence_size_t bp_count);
    /// Emit the final fragment and return sole source-publication authority.
    [[nodiscard]] pending_sequence_source finish(sequence_size_t total_bp,
                                               std::uint16_t last_pad);
    /// Abandon writer-local state after PostgreSQL owns rollback cleanup.
    void abort();

private:
    /// Resolve and open the single shard selected for this sequence.
    void open_shard();
    /// Write one complete or terminal partial physical fragment row.
    void write_fragment(const std::uint8_t* payload, std::size_t payload_bytes,
                        sequence_size_t bp, std::uint16_t pad);

    Oid                 user_relid_ = InvalidOid;
    Oid                 db_oid_ = InvalidOid;
    Oid                 spc_oid_ = InvalidOid;
    RelFileNumber       main_relfilenode_{};
    std::uint64_t       main_storage_id_ = 0;
    std::int16_t        column_attno_ = 0;
    TupleDesc           parent_td_ = nullptr;
    std::uint8_t        alphabet_code_ = 0;
    std::uint8_t        byte_width_ = 0;
    sequence_size_t     cap_bytes_ = 0;
    seq_caches*         caches_ = nullptr;

    std::int16_t        bucket_idx_ = 0;
    std::int32_t        shard_idx_ = 0;
    std::uint64_t       cache_key_ = 0;
    Relation            shard_ = nullptr;
    column_write_state* write_state_ = nullptr;

    std::size_t         frag_payload_bytes_ = 0;  // k_sequence_fragment_storage_units * byte_width
    std::size_t         frag_varlena_bytes_ = 0;  // header + payload
    sequence_size_t     frag_bp_ = 0;             // k_sequence_fragment_storage_units * letters_per_unit
    std::vector<std::uint8_t> buffer_;            // < frag_payload_bytes_ leftover
    std::uint64_t       first_sub_row_no_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t       last_sub_row_no_  = std::numeric_limits<std::uint64_t>::max();
    std::uint32_t       num_fragments_ = 0;
    sequence_size_t     total_varlena_bytes_ = 0;
    std::uint64_t       source_checksum_value_ = 0;
    std::uint64_t       source_epoch_ = 0;
    bool                active_ = false;
};

}  // namespace bioseq::storage

// Export this block so importers, notably
// access_method.cppm) can name `sequence_storage_manager`,
// `bioseq_default_storage_manager`, the sidecar I/O helpers, and
// `read_max_sub_table_cap_bytes`. Without the `export` keyword on this
// re-opening of `namespace bioseq::storage`, the declarations land in the
// module purview but are not exported, so the names are present in the
// translation unit yet refused at import (clang's "declaration must be
// imported from module 'bioseq_storage' before it is required").
export namespace bioseq::storage {

// ---------------------------------------------------------------------------
// Catalog access shims. These call into storage_metadata which owns the
// bioseq_internal.seq_sub_table catalog table.
// ---------------------------------------------------------------------------

/// Compact-sequence type parameters resolved from one parent column typmod.
struct sequence_column_type_parameters {
    /// Registered logical alphabet name.
    std::string alphabet_name;
    /// Packed storage-unit width in bits.
    int storage_unit_bit_width = 0;
};

/// Resolve and validate the compact-sequence typmod at one attribute number.
/// @param parent_tupdesc Borrowed parent tuple descriptor.
/// @param column_attno One-based attribute number.
/// @return Logical alphabet and packed-unit bit width.
/// @throws PostgreSQL ERROR when the attribute or typmod is invalid.
[[nodiscard]] sequence_column_type_parameters
column_typmod_at(TupleDesc parent_tupdesc, std::int16_t column_attno);

/// Return the relation-persistence keyword inherited by a sequence shard.
/// @return Static SQL keyword describing the parent relation persistence.
[[nodiscard]] const char*
persistence_keyword_for_relfilenode(Oid main_database_oid,
                                    Oid main_tablespace_oid,
                                    Oid main_relfilenode);

/// Format the canonical physical sequence-shard relation name.
/// @return Unqualified relation name derived from the physical identity.
[[nodiscard]] std::string
format_sub_table_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                      std::int16_t column_attno, std::int16_t bucket_idx,
                      std::int32_t shard_idx);

/// Format the canonical sidecar relation name paired with a sequence shard.
/// @return Unqualified relation name derived from the physical identity.
[[nodiscard]] std::string
format_sidecar_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                    std::int16_t column_attno, std::int16_t bucket_idx,
                    std::int32_t shard_idx);

/// Stateless shard manager for one sequence-length bucket geometry.
/// @tparam p_length_type Unsigned type used for configured sequence lengths.
/// @tparam p_bucket_minimum_length_bits Smallest configured bucket exponent.
/// @tparam p_bucket_maximum_length_bits Largest configured bucket exponent.
template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
struct sequence_storage_manager {
    /// Configured logical-length type.
    using len_t = p_length_type;
    static constexpr std::size_t bucket_min_len_bits =
        p_bucket_minimum_length_bits;
    static constexpr std::size_t bucket_max_len_bits =
        p_bucket_maximum_length_bits;
    static constexpr std::size_t total_len_bits      = sizeof(len_t) * 8u;
    static constexpr auto table_seq_buckets_len_thresholds =
        make_sequence_bucket_bit_thresholds<
            len_t, bucket_min_len_bits, bucket_max_len_bits>();
    static constexpr std::size_t bucket_count =
        table_seq_buckets_len_thresholds.size();

    static_assert(bucket_count <= std::numeric_limits<std::uint8_t>::max(),
                  "sequence_locator::bucket_index is uint8_t; raise the locator "
                  "wire revision before configuring more than 255 buckets");

    // Delegate to the PG-free template so production and
    // the unit-test TU resolve to identical bucketing code (the LUT itself is
    // sequence_bucket_lookup in bioseq.storage.layout).
    /// Select the configured sequence-length bucket for one logical length.
    /// @param seq_size Logical sequence length in letters.
    /// @return Zero-based configured bucket index.
    [[nodiscard]] [[gnu::always_inline]] static constexpr std::size_t
    sequence_bucket_index(sequence_size_t seq_size) noexcept {
        return bioseq::storage::sequence_bucket_index<
            len_t, bucket_min_len_bits, bucket_max_len_bits>(seq_size);
    }

    // Shard lifecycle / write path.
    //
    // `insert_sequence_streamed` writes one fragment per
    // sub-table row from a per-fragment producer callback. The producer
    // receives a palloc'd `bioseq::pg_types::seq*` whose header is pre-filled with
    // alphabet/width, and fills `data[]` + `bp_size` + `padding_letters`
    // for fragment `n` (last fragment uses `is_last_fragment=true`). The
    // per-fragment MemoryContext is reset between fragments to bound peak
    // RSS. Returns an invalid-to-ready publication token carrying the
    // revision-3 locator and its PostgreSQL-owned source epoch.
    //
    // `insert_payload_as_fragments` is the convenience wrapper that the
    // existing single-varlena code paths (TAM rewrite-to-stubs, CLUSTER)
    // use: it slices an already-materialised `bioseq::pg_types::seq*` into fragments
    // via `insert_sequence_streamed`.
    /// Stream producer-filled fragments into one shard and return publication authority.
    /// @tparam p_fragment_producer Callable that fills one destination fragment.
    template<class p_fragment_producer>
    [[nodiscard]] static pending_sequence_source
    insert_sequence_streamed(Oid user_relid,
                             std::uint64_t main_storage_id,
                             Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                             std::int16_t column_attno,
                             TupleDesc parent_tupdesc,
                             std::uint8_t logical_alphabet_code,
                             std::uint8_t logical_storage_unit_byte_width,
                             sequence_size_t total_bp_size,
                             std::uint16_t last_fragment_padding_letters,
                             sequence_size_t current_cap_bytes,
                             seq_caches* cache,
                             p_fragment_producer&& producer);

    /// Split one resident sequence payload through the streamed write path.
    [[nodiscard]] static pending_sequence_source
    insert_payload_as_fragments(Oid user_relid,
                                std::uint64_t main_storage_id,
                                Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                std::int16_t column_attno,
                                TupleDesc parent_tupdesc,
                                const bioseq::pg_types::seq* original_hdr,
                                sequence_size_t current_cap_bytes,
                                seq_caches* cache);

    // Read path: single sequence — stitches all fragments into a single
    // contiguous palloc'd `bioseq::pg_types::seq*` in `out_mctx`. Used by the
    // bounded hydration path; it raises a PostgreSQL error if the stitched
    // varlena would exceed MaxAllocSize. Streaming readers should
    // use the source-backed facade adapter directly.
    /// Materialize every validated locator fragment into one flat varlena.
    [[nodiscard]] static bioseq::pg_types::seq*
    fetch_payload(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const sequence_locator& loc,
                  Snapshot snapshot,
                  MemoryContext out_mctx, seq_caches* cache);

    // Read path: bulk per-bucket.
    /// Caller output slot paired with one locator for sorted bulk reads.
    struct fetch_request {
        /// Destination index in the caller-provided output span.
        std::size_t out_slot = 0;
        /// Locator to materialize into `out_slot`.
        sequence_locator locator{};
    };

    /// Fetch one bucket's locators in physical row order into caller slots.
    static void
    fetch_payload_bulk(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                       std::int16_t column_attno, std::int16_t bucket_idx,
                       std::span<const fetch_request> requests,
                       std::span<bioseq::pg_types::seq*>            out_payloads,
                       std::span<fetch_request>       sort_scratch,
                       Snapshot                       snapshot,
                       MemoryContext                  out_mctx,
                       seq_caches*                    cache);

    // Cascade on DROP TABLE / TRUNCATE / CLUSTER swap.
    // ProcessUtility calls this read-only preflight before PostgreSQL enters
    // relation deletion.  OAT_DROP repeats the same validation immediately
    // before child cleanup, closing the preflight-to-delete race.
    /// Validate every catalog contract required before cascading a main drop.
    static void validate_drop_catalog_contracts();

    /// Drop every sequence shard and source record owned by one main relation.
    static void
    drop_sub_tables_for_main(
        Oid db_oid, Oid spc_oid, Oid main_relfilenode,
        bool allow_missing_during_extension_teardown = false);
};

/// Default sequence-storage bucket geometry used by PostgreSQL integration.
using bioseq_default_storage_manager =
    sequence_storage_manager<sequence_size_t, /*min=*/8, /*max=*/42>;

// Exported entry point so the source-backed adapter
// can resolve a sub-table relation via the
// per-scan `seq_caches`. Implementation lives below in the .cppm body.
/// Open a locator's shard through the caller-owned relation cache.
/// @param lock_mode PostgreSQL relation lock acquired for a new cache entry.
/// @param cache Caller-owned cache retaining the relation reference and lock.
/// @return Open relation reference owned by `cache`.
/// @throws PostgreSQL ERROR when the shard cannot be resolved or opened.
[[nodiscard]] Relation
open_sub_table_for_locator(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                           std::int16_t column_attno, const sequence_locator& loc,
                           LOCKMODE lock_mode, seq_caches* cache);

/// True only when `relid` is the sub-table relation of an installed
/// seq_sub_table catalog row.  VACUUM and mutation guards use catalog
/// registration rather than trusting the relation-name convention.
/// @return Whether the catalog contains the relation identity.
[[nodiscard]] bool is_registered_sequence_fragment_relation(
    Oid relid,
    bool allow_missing_during_extension_teardown = false);

// ---------------------------------------------------------------------------
// Sidecar I/O. Per-shard heap relations co-sharded with sub-tables.
// ---------------------------------------------------------------------------

/// Insert one locator's sidecar blob into its co-sharded relation.
void
sidecar_write_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                   std::int16_t column_attno, const sequence_locator& loc,
                   std::span<const std::uint8_t> blob, seq_caches* cache);

/// Read one locator's sidecar blob when present.
/// @return Owning byte vector, or no value when no blob can be found.
[[nodiscard]] std::optional<std::vector<char>>
sidecar_read_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const sequence_locator& loc,
                  seq_caches* cache);

/// Read one bucket's sidecars in sorted physical-row order.
void
sidecar_read_blob_bulk(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, std::int16_t bucket_idx,
    std::span<const sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request> requests,
    std::span<std::optional<std::vector<char>>> out_blobs,
    std::span<sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request> sort_scratch,
    MemoryContext out_mctx,
    seq_caches* cache);

// ---------------------------------------------------------------------------
// Catalog reaper and cascade-drop entry point.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// TAM hot-path helper for write-state cap-snapshot read.
// ---------------------------------------------------------------------------

/// Read the current maximum physical sequence-shard payload cap.
/// @return Capacity in bytes.
[[nodiscard]] sequence_size_t read_max_sub_table_cap_bytes();

}  // namespace bioseq::storage


namespace bioseq::storage::detail {

// Per-fragment storage-unit count. The class member from
// `resident_sequence_scanner<T>::fragment_storage_units` is 1024. This aliases
// the exported `k_sequence_fragment_storage_units` so the
// public constant and this hot-path detail constant can never drift.
inline constexpr std::size_t k_fragment_storage_units =
    k_sequence_fragment_storage_units;

// Look-up bits_per_char via the supported-alphabet pack so a future alphabet
// addition does not silently produce wrong fragment_bp values.
/// Resolve packed bits per letter for one registered alphabet code.
/// @param alpha Registered alphabet code.
/// @return Bits per logical letter, or zero when the code is unsupported.
[[nodiscard]] inline std::size_t bits_per_char_for_alphabet_code(
    bioseq::pg_types::seq::alphabet_code_t alpha) noexcept {
    std::size_t bits_per_char = 0;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(alpha,
        [&bits_per_char]<class p_alphabet>() {
            bits_per_char = static_cast<std::size_t>(
                seqan3::detail::ceil_log2(
                    static_cast<std::size_t>(
                        seqan3::alphabet_size<p_alphabet>)));
        });
    return bits_per_char;
}

// letters_per_unit = (storage_unit_byte_width * 8) / bits_per_char.
/// Resolve logical letters per packed storage unit.
/// @param alpha Registered alphabet code.
/// @param logical_byte_width Packed-unit width in bytes.
/// @return Letters per unit, or zero when the alphabet is unsupported.
[[nodiscard]] inline std::size_t letters_per_unit_for(
    bioseq::pg_types::seq::alphabet_code_t alpha, std::uint8_t logical_byte_width) noexcept {
    const std::size_t bpc = bits_per_char_for_alphabet_code(alpha);
    return bpc > 0
        ? (static_cast<std::size_t>(logical_byte_width) * 8u) / bpc
        : 0u;
}

}  // namespace bioseq::storage::detail
