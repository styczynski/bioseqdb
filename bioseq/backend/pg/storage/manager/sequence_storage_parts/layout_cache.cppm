/**
 * @file layout_cache.cppm
 * @brief Sequence locators, cached shard readers and writers, and fragment storage APIs.
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
import bioseq.common.seqan_adapter;  // ceil_log2 for calculating bits per symbol
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
export import bioseq.postgres.storage.manager.locator_stub;
// Bucket calculations and sequence_locator are shared with tests that run
// without PostgreSQL.
import bioseq.storage.layout;
import :sequence_storage_catalog;

export namespace bioseq::storage {

// The main table stores a 56-byte sequence locator in each sequence column.
// Its 40-byte payload identifies the fragment rows holding the sequence.

// The locator payload format is defined in bioseq.storage.layout.

/// Check the header  marker that identifies a stored sequence locator.
/// @param hdr Sequence header, or null.
/// @return True when the header has the locator marker.
[[nodiscard]] [[gnu::always_inline]]
inline bool is_locator_stub(const bioseq::pg_types::seq* hdr) noexcept {
    return hdr != nullptr
        && is_sequence_locator_stub_width(
            static_cast<std::uint8_t>(hdr->storage_unit_byte_width));
}

/// Read the packed storage-unit width without the, locator flag.
/// @param hdr Non-null sequence header.
/// @return Width in bytes.
[[nodiscard]] [[gnu::always_inline]]
inline std::uint8_t logical_storage_unit_byte_width(const bioseq::pg_types::seq* hdr) noexcept {
    return static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(hdr->storage_unit_byte_width) & 0x0Fu);
}

/// Read a locator after checking its marker, size and payload format.
/// @param hdr Non-null sequence header containing a locator.
/// @return Decoded locator.
/// @throws PostgreSQL ERROR if the header or payload is invalid.
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
               "bioseq storage: locator payload is not canonical revision-4 data");
    }
    return *decoded;
}

/* Check locator lengths and row numbers before opening a shard or allocating
 * a cmplete  sequence. These values must fit both PostgreSQL catalog fields
 * and the allocation size. */
/// Check locator lengths and fragment-row ranges before materialization.
/// @param loc Locator to check.
/// @throws PostgreSQL ERROR for invalid metadata, oversized lengths or row
/// numbers, or an inconsistent fragment count and row range.
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
        case error::invalid_encoded_payload_geometry:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator encoded payload geometry is inconsistent")));
        case error::fragment_row_range_exceeds_catalog:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator fragment row range exceeds bigint")));
    }
    pg_unreachable();
}

/// Check that a stored fragment has a complete header and the expected size
/// @param raw_bytes Stored value size in bytes.
/// @param expected_bytes Required value size in bytes.
/// @param row_no Fragment row number for error messages.
/// @throws PostgreSQL ERROR if the size is wrong.
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

/// Check a fragment against  the sequence type and expected fragment length.
/// @param raw Fragment header to check.
/// @param expected_alphabet Required alphabet code.
/// @param expected_width Required storage-unit width in bytes.
/// @param expected_bp_size Required length in symbols.
/// @param expected_padding Unused symbol positions in the final storage unit.
/// @param row_no Fragment row number for error messages.
/// @throws PostgreSQL ERROR if the type, length or padding differs.
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

// Sequence-length bucket calculations are shared with bioseq.storage.layout.

// Each scan, index fetch, writer or dispatch buffer keeps its own shard cache.
// Allocate the cache separatly so its C++ containers do not change the layout
// of PostgreSQL state structs that store  an opaque pointer to it.

/// Keep opened fragment tables and their readers or writers for one owner.
struct seq_caches {
    /// An opened fragment table and the reader, or writer using it.
    struct entry {
        /// Cache-owned open relation reference.
        Relation rel = nullptr;
        /// Column writer borrowed from the transaction write-state cache.
        column_write_state* write_state = nullptr;
        /// Column reader owned by this entry when the cache is read-side.
        column_read_state*  read_state  = nullptr;
        // Snapshot supplied when read_state was opened. Registered MVCC snapshots
        // and SnapshotAny can be kept here. Other snapshots use a reader that closes
        // before the fetch returns.
        Snapshot read_snapshot_source = nullptr;
        /// Fragment-row bytes waiting to be added to shard metadata.
        sequence_size_t pending_bytes_delta = 0;
        /// Payload-byte counter accumulated with pending fragment writes.
        sequence_size_t pending_total_payload_delta = 0;
    };

    /// Keep shard entries in a sorted vector for lookup and ordered cleanup
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

        /// Find an entry by its shard  key.
        /// @return Matching iterator, or `end()` if absent.
        [[nodiscard]] iterator find(std::uint64_t key) noexcept {
            auto it = lower_bound(key);
            return it != entries_.end() && it->first == key ? it : entries_.end();
        }

        /// Find an entry by its shard key without changing the cache.
        /// @return Matching iterator, or `end()` if absent.
        [[nodiscard]] const_iterator find(std::uint64_t key) const noexcept {
            auto it = lower_bound(key);
            return it != entries_.end() && it->first == key ? it : entries_.end();
        }

        /// Get the entry for `key`, inserting an empty entry if needed.
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

    // A sorted vector keeps small shard caches together in memory and avoids
    // allocating a node for each entry. Insertion can invalidate references,
    // so callers must look up entries again after inserting.
    /// Entries keyed by packed column, bucket, and shard identity.
    entry_map entries{};
    /// Relation lock mode used consistently by this cache.
    LOCKMODE lock_mode = AccessShareLock;

    /// Choose whether this cche serves reads or writes.
    enum class access_side { read, write };
    access_side side = access_side::read;

    /// Memory context owning cached reader state.
    MemoryContext read_state_mctx = nullptr;

    // Set this when cached readers must survive the scan that opened them.
    // A sequence kept after that scan can  finish reading under its transaction
    // resource owner. Null uses CurrentResourceOwner.
    ResourceOwner read_resource_owner = nullptr;

    /// Shard size limit captured when the write cache is created, in bytes.
    sequence_size_t cap_bytes_snapshot = std::numeric_limits<sequence_size_t>::max();

    Oid main_database_oid   = InvalidOid;
    Oid main_tablespace_oid = InvalidOid;
    Oid main_relfilenode    = InvalidOid;

    /// Combine column, bucket and shard numbers into the cache lookup key.
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

    /// Close cached readers and relations, then clear the cache.
    /// @param commit Apply pending shard-byte counts before closing.
    void close_all(bool commit) noexcept;
};

// Construct and destroy the cache explicitly because PostgreSQL allocates
// its memory with palloc. Consumers keep an opaque pointer to it.
//
// destroy_seq_caches has an out-of-line definition so write-state,  cleanup can
// call it through a forward declaration without creating a module import cycle
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

/// Close cached readers and relations while keeping the cache allocation.
/// @param caches Cache to close.
/// @param commit Apply pending shard-byte counts beore closing.
[[gnu::always_inline]] inline void close_seq_caches(seq_caches& caches,
                                                    bool commit) noexcept {
    caches.close_all(commit);
}

/// Close and free the cache and its reader state.
/// @param caches Cache to destroy, or null.
/// @param commit Apply pending shard-byte counts before closing.
void close_and_destroy_seq_caches(seq_caches* caches, bool commit) noexcept;

// bioseq.load uses this count to size fragment buffers. The internal storage
// constant aliases it so writers and readers use the same fragment size.
/// Packed storage-unit capacity of one physical sequence fragment.
inline constexpr std::size_t k_sequence_fragment_storage_units =
    std::size_t{1024};

/** Return fixed physical payload bytes for a sequence's persisted storage width. */
[[nodiscard]] constexpr std::size_t sequence_fragment_payload_byte_capacity(std::size_t storage_width) noexcept {
    return k_sequence_fragment_storage_units * storage_width;
}

/** Verify zero fill outside a complete encoded segment after frame-size validation. */
[[nodiscard]] inline bool sequence_encoded_fragment_fill_is_zero(
        const bioseq::pg_types::seq& fragment, std::size_t payload_bytes) noexcept {
    if (fragment.bp_size > payload_bytes) return false;
    const auto* bytes = reinterpret_cast<const std::byte*>(fragment.data);
    return std::all_of(bytes + fragment.bp_size, bytes + payload_bytes,
        [](std::byte value) { return value == std::byte{}; });
}

/** Verify source codec, padding and physical fragment count without reading payload bytes. */
[[nodiscard]] inline bool source_codec_geometry_is_valid(
        const bioseq::pg_types::seq& header, const sequence_locator& locator) {
    const auto width = logical_storage_unit_byte_width(&header);
    if (validate_sequence_locator(locator) != sequence_locator_validation_error::none ||
        !bioseq::valid_cigar_space(header.space) ||
        !is_supported_alphabet_code(header.alphabet_code) ||
        !pgseq_supported_storage_unit_byte_widths::contains_runtime(width) ||
        (pgseq_is_cigar_alphabet(header.alphabet_code) && width != sizeof(bioseq::cigar_run))) return false;
    bool valid = false;
    bioseq::pg_types::seq::dispatch_instance(header.alphabet_code,
        logical_storage_unit_byte_width(&header),
        [&valid, &header, &locator]<class Instance>() {
            using traits = typename compact_sequence_range<Instance>::traits;
            if constexpr (traits::is_run_encoded) {
                constexpr std::uint64_t capacity = sequence_fragment_payload_byte_capacity(sizeof(typename Instance::storage_unit));
                const auto extent = locator.encoded_payload_bytes;
                valid = header.encoding == bioseq::sequence_encoding::cigar_rle &&
                    header.padding_letters == 0u &&
                    ((locator.logical_length == 0u) == (extent == 0u)) &&
                    locator.fragment_count == extent / capacity + (extent % capacity != 0u ? 1u : 0u);
            } else {
                constexpr std::uint64_t letters_per_unit = traits::letters_per_unit;
                const auto units = locator.logical_length / letters_per_unit +
                    (locator.logical_length % letters_per_unit != 0u ? 1u : 0u);
                valid = header.encoding == bioseq::sequence_encoding::rank_packed &&
                    header.space == bioseq::cigar_space::compatibility &&
                    locator.encoded_payload_bytes == 0u &&
                    header.padding_letters == units * letters_per_unit - locator.logical_length &&
                    locator.fragment_count == units / k_sequence_fragment_storage_units +
                        (units % k_sequence_fragment_storage_units != 0u ? 1u : 0u);
            }
        });
    return valid;
}

// Flush fragment writes before an index build reads them in  the same
// statement. The transaction write-state cache still owns the writers.
/// Make pending fragment rows available to reads in the same statement.
/// @param caches Write cache, or null.
/// @param current_sub_xid Subtransaction owning the writers.
void flush_seq_cache_write_states(seq_caches* caches,
                                  SubTransactionId current_sub_xid);

/// Write a sequence as fragments while its final length is still unknown.
///
/// Input is already packed for the destination column. Completed fragments
/// go to one shard, and finish() writes the last fragment with its padding.
/// append_packed_bytes() copies each input span before draining full fragments,
/// so callers should submit bounded chunks to  limit temporary memory.
/// A sequence exceeding the shard size limit raises an error. PostgreSQL
/// rollback removes any ragment rows already written.
struct sequence_stream_writer {
    /// Start a sequence write and allocate its source version number
    void begin(Oid user_relid, Oid db_oid, Oid spc_oid,
               RelFileNumber main_relfilenode, std::uint64_t main_storage_id,
               AttrNumber seq_attno, TupleDesc parent_td,
               std::uint8_t alphabet_code, std::uint8_t byte_width,
               sequence_size_t cap_bytes, seq_caches* caches);
    /// Append packed bytes, keeping the last fragment for finish().
    void append_packed_bytes(std::span<const std::uint8_t> bytes,
                             sequence_size_t bp_count);
    /// Write the last fragment and return the token used to publish its locator.
    [[nodiscard]] pending_sequence_source finish(sequence_size_t total_bp,
                                               std::uint16_t last_pad);
    /// Clear buffered input and local state after a failed write.
    void abort();

private:
    /// Open the shard that will hold every  fragment, of this sequence.
    void open_shard();
    /// Store one fragment and record its row number.
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

    std::size_t         frag_payload_bytes_ = 0;  // Bytes reserved for the packed fragment payload
    std::size_t         frag_varlena_bytes_ = 0;  // Fragment payload plus its sequence header
    sequence_size_t     frag_bp_ = 0;             // Symbols in a full fragment
    std::vector<std::uint8_t> buffer_;            // Packed bytes waiting for the next fragment write
    std::uint64_t       first_sub_row_no_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t       last_sub_row_no_  = std::numeric_limits<std::uint64_t>::max();
    std::uint32_t       num_fragments_ = 0;
    sequence_size_t     total_varlena_bytes_ = 0;
    std::uint64_t       source_checksum_value_ = 0;
    std::uint64_t       source_epoch_ = 0;
    bool                active_ = false;
};

}  // namespace bioseq::storage

// Export the storage manager and helpers used by table access methods.
export namespace bioseq::storage {

// Helpers for the sequence shard catalog.

/// Alphabet and packed-unit width declared for a sequence column.
struct sequence_column_type_parameters {
    /// Alphabet name used in the column type.
    std::string alphabet_name;
    /// Packed storage-unit width in bits.
    int storage_unit_bit_width = 0;
};

/// Read  the alphabet and storage width declared for a sequence column.
/// @param parent_tupdesc Parent tazle column descriptions.
/// @param column_attno One-based column number.
/// @return Alphabet name and storage-unit width in bits.
/// @throws A translated PostgreSQL error if the column or type modifier is invalid.
[[nodiscard]] sequence_column_type_parameters
column_typmod_at(TupleDesc parent_tupdesc, std::int16_t column_attno);

/// Choose the shard CREATE TABLE modifier from its parent.
/// @return `UNLOGGED`, `TEMPORARY`, or an empty string for a permanent or missing parent
[[nodiscard]] const char*
persistence_keyword_for_relfilenode(Oid main_database_oid,
                                    Oid main_tablespace_oid,
                                    Oid main_relfilenode);

/// Build  a fragment table name from its parent, column, bucket and shard.
/// @return Table name without the schema.
[[nodiscard]] std::string
format_sub_table_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                      std::int16_t column_attno, std::int16_t bucket_idx,
                      std::int32_t shard_idx);

/// Build the name of the sidecar table paired with a fragment shard.
/// @return Table name without the schema.
[[nodiscard]] std::string
format_sidecar_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                    std::int16_t column_attno, std::int16_t bucket_idx,
                    std::int32_t shard_idx);

/// Store sequences in length buckets, each containing one or more shards.
/// @tparam p_length_type Unsigned type used for sequence lengths.
/// @tparam p_bucket_minimum_length_bits Smallest bucket exponent.
/// @tparam p_bucket_maximum_length_bits Largest bucket exponent.
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

    // Use the same bucket calculation in PostgreSQL and standalone tests.
    /// Choose a length bucket for a sequence.
    /// @param seq_size Sequence length in symbols.
    /// @return Zero-based bucket  number.
    [[nodiscard]] [[gnu::always_inline]] static constexpr std::size_t
    sequence_bucket_index(sequence_size_t seq_size) noexcept {
        return bioseq::storage::sequence_bucket_index<
            len_t, bucket_min_len_bits, bucket_max_len_bits>(seq_size);
    }

    // insert_sequence_streamed calls the producer once per, fragment. The
    // destination already has its alphabet and storage width. The producer fills
    // data[], bp_size and padding_letters, using the last-fragment flag to handle
    // the seqence end. Temporary fragment allocations are cleared between calls.
    //
    // The returned token holds the locator and an unpublished source version.
    // The caller publishes it after storing the parent row.
    // insert_payload_as_fragments uses this path for an already loaded sequence.
    /// Write producer-filled fragments to one shard and  return their unpublished locator.
    /// @tparam p_fragment_producer Callback that fills each destination fragment
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
                             p_fragment_producer&& producer,
                             bioseq::sequence_encoding encoding = bioseq::sequence_encoding::rank_packed,
                             bioseq::cigar_space space = bioseq::cigar_space::compatibility,
                             std::uint32_t encoded_payload_bytes = 0u);

    /// Split an already loaded sequence into stored fragments.
    [[nodiscard]] static pending_sequence_source
    insert_payload_as_fragments(Oid user_relid,
                                std::uint64_t main_storage_id,
                                Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                std::int16_t column_attno,
                                TupleDesc parent_tupdesc,
                                const bioseq::pg_types::seq* original_hdr,
                                sequence_size_t current_cap_bytes,
                                seq_caches* cache);

    // Read all fragments into one palloc allocation in out_mctx. This raises an
    // error if the complete value exceeds MaxAllocSize. Use the sequence scanner
    // to read large sequences a fragment at a time.
    /// Read and check every fragment, then return one complete sequence value.
    [[nodiscard]] static bioseq::pg_types::seq*
    fetch_payload(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const sequence_locator& loc,
                  Snapshot snapshot,
                  MemoryContext out_mctx, seq_caches* cache);

    // Batched sequence reads.
    /// A locator and the output slot where  its sequence should be returned.
    struct fetch_request {
        /// Destination index in the caller-provided output span.
        std::size_t out_slot = 0;
        /// Locator of the sequence to read.
        sequence_locator locator{};
    };

    /// Sort requests by shard and fragment row, then fil their output slots.
    static void
    fetch_payload_bulk(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                       std::int16_t column_attno, std::int16_t bucket_idx,
                       std::span<const fetch_request> requests,
                       std::span<bioseq::pg_types::seq*>            out_payloads,
                       std::span<fetch_request>       sort_scratch,
                       Snapshot                       snapshot,
                       MemoryContext                  out_mctx,
                       seq_caches*                    cache);

    // Check the catalogs before DROP TABLE, TRUNCATE or a table rewrite removes
    // child tables. The drop hook checks again immediately before deleting them.
    /// Check and lock the catalogs needed to remove a parent table and its shards.
    static void validate_drop_catalog_contracts();

    /// Drop, a parent table's fragment tables, sidecars and source-version records.
    static void
    drop_sub_tables_for_main(
        Oid db_oid, Oid spc_oid, Oid main_relfilenode,
        bool allow_missing_during_extension_teardown = false);
};

/// Length buckets used by the PostgreSQL sequence store.
using bioseq_default_storage_manager =
    sequence_storage_manager<sequence_size_t, /*min=*/8, /*max=*/42>;

// Sequence scanners use this helper to reuse opened shard relations.
/// Open the fragment  table named by a locator, reusing a cached relation.
/// @param lock_mode Lock to acquire when opening the table.
/// @param cache Optional cache that keeps the relation and lock until cleanup.
/// @return Open fragment relation Without a cache, the caller must close it
/// with the same lock mode.
/// @throws PostgreSQL ERROR or a translated error if the shard cannot be opened.
[[nodiscard]] Relation
open_sub_table_for_locator(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                           std::int16_t column_attno, const sequence_locator& loc,
                           LOCKMODE lock_mode, seq_caches* cache);

/// Check whether the shard catalog registers this relation as a fragment table.
/// VACUUM and checks that restrict writes use this because table names can change.
/// @return True when the relation is  registered.
[[nodiscard]] bool is_registered_sequence_fragment_relation(
    Oid relid,
    bool allow_missing_during_extension_teardown = false);

// Each fragment shard has a companion heap table for sidecar blobs.

/// Store sidecar bytes under the locator's first fragmnt row number.
void
sidecar_write_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                   std::int16_t column_attno, const sequence_locator& loc,
                   std::span<const std::uint8_t> blob, seq_caches* cache);

/// Read a copy of a locator's sidecar bytes.
/// @return Bytes, or no value if the table, row or blob is missing.
[[nodiscard]] std::optional<std::vector<char>>
sidecar_read_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const sequence_locator& loc,
                  seq_caches* cache);

/// Sort sidecar requests by shard and row, then fill their output slots.
void
sidecar_read_blob_bulk(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, std::int16_t bucket_idx,
    std::span<const sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request> requests,
    std::span<std::optional<std::vector<char>>> out_blobs,
    std::span<sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request> sort_scratch,
    MemoryContext out_mctx,
    seq_caches* cache);



// Shard size setting used when a writer starts.

/// Read the configured shard size limit.
/// @return Bytes, or zero to disable the size limit.
[[nodiscard]] sequence_size_t read_max_sub_table_cap_bytes();

}  // namespace bioseq::storage


namespace bioseq::storage::detail {

// Share the fragment size used by the public writer API.
inline constexpr std::size_t k_fragment_storage_units =
    k_sequence_fragment_storage_units;

// Derive symbol  size from the registered alphabet so new alphabets use
// the correct fragment length.
/// Find the packed bit width of one symbol.
/// @param alpha Registered alphabet code.
/// @return Bits per symbol, or zero for an unsupported code
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
/// Find how many symbols fit in one packed storage unit.
/// @param alpha Registered alphabet, code.
/// @param logical_byte_width Storage-unit width in bytes.
/// @return Symbols per unit, or zero for an unsupported alphabet.
[[nodiscard]] inline std::size_t letters_per_unit_for(
    bioseq::pg_types::seq::alphabet_code_t alpha, std::uint8_t logical_byte_width) noexcept {
    const std::size_t bpc = bits_per_char_for_alphabet_code(alpha);
    return bpc > 0
        ? (static_cast<std::size_t>(logical_byte_width) * 8u) / bpc
        : 0u;
}

}  // namespace bioseq::storage::detail
