/**
 * @file hash_major_writer.cppm
 * @brief Bounded hash-major generation writer and atomic head publication.
 *
 * Index chunks are raw BYTEA tuples in heap relations (heap_form_tuple /
 * CatalogTupleInsert + systable_beginscan), bypassing columnarisation
 * (Invariant C). The per-sequence layouts co-locate with the sequence's
 * `(bucket, shard)`. The table layout shards via `index_table_shard`.
 */
module;
#include <bioseq/pg_compat.h>
#include <libassert/assert.hpp>

extern "C" {
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h" // RelnameGetRelid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "executor/instrument.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/relfilelocator.h"
#include "storage/shm_toc.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/xid8.h"

#include "common/relpath.h"
#include "miscadmin.h"
#include "port.h"
#include "port/atomics.h"
}

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
export module bioseq.postgres.storage.indexing.chunked_storage:hash_major_writer;
import std;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;           // sequence_locator
import bioseq.storage.size;             // sequence_size_t
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;           // the concepts
import bioseq.indexing.minimizer_hashmap; // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image; // explicit fixed-endian minimizer fragment image image ABI
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.postgres.storage.scanner;        // PostgreSQL-backed lazy sequence access
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // canonical bounded base decoding
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.instrumentation.clock;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import :catalog;
import :catalog_row_io;
export namespace bioseq::storage::indexing {

// =========================================================================
//  hash-major PostgreSQL storage adapter
// =========================================================================

namespace hash_major_minimizer_pg_detail {

[[nodiscard]] constexpr std::size_t stream_index(hash_major_minimizer_stream_kind kind) noexcept {
    return static_cast<std::size_t>(kind) - 1u;
}

[[nodiscard]] constexpr hash_major_minimizer_stream_kind stream_kind(std::size_t index) noexcept {
    return static_cast<hash_major_minimizer_stream_kind>(index + 1u);
}

[[nodiscard]] constexpr bool valid_stream_kind(hash_major_minimizer_stream_kind kind) noexcept {
    return kind == hash_major_minimizer_stream_kind::directory ||
           kind == hash_major_minimizer_stream_kind::controls ||
           kind == hash_major_minimizer_stream_kind::postings;
}

[[nodiscard]] constexpr index_sub_key_t
stream_sub_key(hash_major_minimizer_stream_kind kind) noexcept {
    return static_cast<index_sub_key_t>(static_cast<std::uint16_t>(kind));
}

[[nodiscard]] constexpr std::uint64_t ceil_rows(std::uint64_t bytes) noexcept {
    return bytes / k_hash_major_minimizer_row_payload_bytes +
           (bytes % k_hash_major_minimizer_row_payload_bytes != 0u ? 1u : 0u);
}

/// Create one ResourceOwner-managed temporary stream spool.
[[nodiscard]] inline BufFile* create_spool() {
    BufFile* file =
        bioseq::postgres::support::invoke_pg_as_cpp([] { return BufFileCreateTemp(false); });
    if (file == nullptr) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major temporary logical stream could not be created");
    }
    return file;
}

/// Append a non-empty logical stream fragment to a spool.
inline void append_spool(BufFile* file, std::span<const std::byte> bytes) {
    if (file == nullptr || bytes.empty()) {
        minimizer_generation_storage_error_as_cpp("hash-major logical stream append is invalid");
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [file, bytes] { BufFileWrite(file, bytes.data(), bytes.size()); });
}

/// Rewind a completed spool for exact replay.
inline void rewind_spool(BufFile* file) {
    if (file == nullptr || bioseq::postgres::support::invoke_pg_as_cpp(
                               [file] { return BufFileSeek(file, 0, 0, SEEK_SET); }) != 0) {
        minimizer_generation_storage_error_as_cpp("hash-major logical stream rewind failed");
    }
}

/// Read the requested bytes or raise a PostgreSQL storage error.
inline void read_spool_exact(BufFile* file, std::span<std::byte> bytes) {
    if (file == nullptr || bytes.empty()) {
        minimizer_generation_storage_error_as_cpp("hash-major logical stream read is invalid");
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [file, bytes] { BufFileReadExact(file, bytes.data(), bytes.size()); });
}

/// Close a spool and clear its caller-owned handle.
inline void close_spool(BufFile*& file) {
    BufFile* closing = std::exchange(file, nullptr);
    if (closing != nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp([closing] { BufFileClose(closing); });
    }
}

/// Forget a spool after PostgreSQL ERROR leaves cleanup to ResourceOwner.
inline void abandon_spool(BufFile*& file) noexcept {
    // PostgreSQL ResourceOwner owns a file whose leaf raised ERROR.
    file = nullptr;
}

/// Form one authenticated hash-major stream tuple with an explicit checksum.
[[nodiscard]] inline HeapTuple form_row(Relation relation,
                                        std::uint64_t query_key,
                                        const index_chunk_address& address,
                                        std::span<const std::byte> bytes,
                                        std::uint64_t checksum) {
    if (relation == nullptr || bytes.empty() ||
        bytes.size() > k_hash_major_minimizer_row_payload_bytes ||
        bytes.size() > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()) ||
        !bioseq::indexing::sql_bigint_can_represent(query_key) ||
        !bioseq::indexing::sql_bigint_can_represent(address.generation_id) ||
        !bioseq::indexing::sql_integer_can_represent(address.chunk_no) ||
        checksum !=
            bioseq::indexing::crc64_index_chunk_row(address.generation_id,
                                                    address.position_segment,
                                                    address.sub_key,
                                                    address.chunk_no,
                                                    static_cast<std::uint32_t>(bytes.size()),
                                                    bytes)) {
        minimizer_generation_storage_error_as_cpp("hash-major row envelope is invalid");
    }
    bytea* blob = bioseq::postgres::support::invoke_pg_as_cpp(
        [bytes] { return reinterpret_cast<bytea*>(palloc(VARHDRSZ + bytes.size())); });
    SET_VARSIZE(blob, VARHDRSZ + bytes.size());
    std::memcpy(VARDATA(blob), bytes.data(), bytes.size());
    bioseq::postgres::support::pg_datum_list<8> values{};
    values.set(k_minimizer_generation_attribute_query_key, static_cast<std::int64_t>(query_key));
    values.set(k_minimizer_generation_attribute_generation_id,
               static_cast<std::int64_t>(address.generation_id));
    values.set(k_minimizer_generation_attribute_position_segment, address.position_segment);
    values.set(k_minimizer_generation_attribute_sub_key,
               static_cast<std::int64_t>(address.sub_key));
    values.set(k_minimizer_generation_attribute_chunk_number,
               static_cast<std::int32_t>(address.chunk_no));
    values.set(k_minimizer_generation_attribute_chunk_bytes,
               static_cast<std::int32_t>(bytes.size()));
    values.set(k_minimizer_generation_attribute_checksum, checksum_to_pg_bigint(checksum));
    values.set_raw(k_minimizer_generation_attribute_blob,
                   bioseq::postgres::support::pg_datum::from_pointer(blob));
    HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
        [&values, relation] { return values.form_tuple(RelationGetDescr(relation)); });
    bioseq::postgres::support::invoke_pg_as_cpp([blob] { pfree(blob); });
    return tuple;
}

/// Form one authenticated hash-major stream tuple and derive its row checksum.
[[nodiscard]] inline HeapTuple form_row(Relation relation,
                                        std::uint64_t query_key,
                                        const index_chunk_address& address,
                                        std::span<const std::byte> bytes) {
    return form_row(
        relation,
        query_key,
        address,
        bytes,
        bioseq::indexing::crc64_index_chunk_row(address.generation_id,
                                                address.position_segment,
                                                address.sub_key,
                                                address.chunk_no,
                                                static_cast<std::uint32_t>(bytes.size()),
                                                bytes));
}

} // namespace hash_major_minimizer_pg_detail

class ready_hash_major_minimizer_token;

/// Receipt for a byte-exact head publication and any generation it replaced.
struct ready_hash_major_minimizer_published_head_receipt {
    std::string table_name;
    std::uint64_t query_key = 0u;
    hash_major_minimizer_head head{};
    encoded_hash_major_minimizer_head head_bytes{};
    std::optional<std::uint64_t> replaced_generation_id{};
};

/// Publish the head authorized by a completed hash-major writer token.
[[nodiscard]] inline ready_hash_major_minimizer_published_head_receipt
publish_ready_hash_major_minimizer_head_pg(ready_hash_major_minimizer_token&& token);

/// Move-only authorization that the exact unpublished hash-major generation was
/// authenticated from PostgreSQL rows and bound to the authoritative source.
class ready_hash_major_minimizer_token {
  private:
    minimizer_generation_administration_identity identity_{};
    Oid sidecar_relation_oid_ = InvalidOid;
    RelFileLocator sidecar_locator_{};
    std::uint64_t advisory_lock_key_ = 0u;
    std::uint64_t query_key_ = 0u;
    std::uint64_t generation_id_ = 0u;
    TransactionId transaction_id_ = InvalidTransactionId;
    hash_major_minimizer_head head_{};
    encoded_hash_major_minimizer_head head_bytes_{};
    std::uint64_t head_row_checksum_ = 0u;
    bool valid_ = false;

    ready_hash_major_minimizer_token(minimizer_generation_administration_identity identity,
                                     Oid sidecar_relation_oid,
                                     RelFileLocator sidecar_locator,
                                     std::uint64_t advisory_lock_key,
                                     std::uint64_t query_key,
                                     std::uint64_t generation_id,
                                     TransactionId transaction_id,
                                     const hash_major_minimizer_head& head,
                                     const encoded_hash_major_minimizer_head& head_bytes,
                                     std::uint64_t head_row_checksum)
        : identity_(std::move(identity)), sidecar_relation_oid_(sidecar_relation_oid),
          sidecar_locator_(sidecar_locator), advisory_lock_key_(advisory_lock_key),
          query_key_(query_key), generation_id_(generation_id), transaction_id_(transaction_id),
          head_(head), head_bytes_(head_bytes), head_row_checksum_(head_row_checksum),
          valid_(true) {}

    void invalidate_() noexcept {
        identity_.release_owned_strings();
        sidecar_relation_oid_ = InvalidOid;
        sidecar_locator_ = {};
        advisory_lock_key_ = 0u;
        query_key_ = 0u;
        generation_id_ = 0u;
        transaction_id_ = InvalidTransactionId;
        head_ = {};
        head_bytes_.fill(std::byte{0});
        head_row_checksum_ = 0u;
        valid_ = false;
    }

    friend class index_storage_hash_major_minimizer_writer;
    friend ready_hash_major_minimizer_published_head_receipt
    publish_ready_hash_major_minimizer_head_pg(ready_hash_major_minimizer_token&& token);

  public:
    ready_hash_major_minimizer_token() = delete;
    ready_hash_major_minimizer_token(const ready_hash_major_minimizer_token&) = delete;
    ready_hash_major_minimizer_token& operator=(const ready_hash_major_minimizer_token&) = delete;
    ready_hash_major_minimizer_token(ready_hash_major_minimizer_token&& other) noexcept
        : identity_(std::move(other.identity_)), sidecar_relation_oid_(other.sidecar_relation_oid_),
          sidecar_locator_(other.sidecar_locator_), advisory_lock_key_(other.advisory_lock_key_),
          query_key_(other.query_key_), generation_id_(other.generation_id_),
          transaction_id_(other.transaction_id_), head_(other.head_),
          head_bytes_(other.head_bytes_), head_row_checksum_(other.head_row_checksum_),
          valid_(other.valid_) {
        other.invalidate_();
    }
    ready_hash_major_minimizer_token& operator=(ready_hash_major_minimizer_token&&) = delete;
    ~ready_hash_major_minimizer_token() = default;
};

static_assert(!std::is_default_constructible_v<ready_hash_major_minimizer_token>);
static_assert(!std::is_copy_constructible_v<ready_hash_major_minimizer_token>);
static_assert(std::is_nothrow_move_constructible_v<ready_hash_major_minimizer_token>);

inline constexpr std::uint64_t k_hash_major_minimizer_publication_slice_bytes = std::uint64_t{8}
                                                                                << 20u;

/// PostgreSQL-owned bounded sink for the storage-neutral hash-major encoder.
/// The callback path only appends to three ResourceOwner-managed BufFiles.
/// Sealing replays exact 8,064-byte rows in postings -> controls -> directory
/// order, writes the manifest, and rereads the complete unpublished generation
/// before a private publication token can be produced.
class index_storage_hash_major_minimizer_writer {
  private:
    enum class operation_state : std::uint8_t {
        accepting,
        sealed,
        failed,
        moved,
    };

    static constexpr std::uint64_t k_batch_rows = 1'024u;

    minimizer_generation_administration_identity identity_{};
    Relation relation_ = nullptr;
    CatalogIndexState catalog_indexes_ = nullptr;
    Snapshot validation_snapshot_ = nullptr;
    Relation validation_primary_index_ = nullptr;
    SysScanDesc validation_scan_ = nullptr;
    std::array<BufFile*, 3> stream_spools_{};
    std::uint64_t advisory_lock_key_ = 0u;
    std::uint64_t query_key_ = 0u;
    std::uint64_t generation_id_ = 0u;
    std::array<std::uint64_t, 3> logical_bytes_{};
    std::array<crc64_ecma182_accumulator, 3> logical_crc_{};
    bioseq::parallelism::memory_permit publication_permit_{};
    std::vector<TupleTableSlot*> pending_slots_{};
    std::uint64_t pending_tuple_bytes_ = 0u;
    std::array<std::byte, k_hash_major_minimizer_row_payload_bytes> row_buffer_{};
    operation_state state_ = operation_state::accepting;

    [[noreturn]] void fail_(std::string_view message) {
        state_ = operation_state::failed;
        minimizer_generation_storage_error_as_cpp(message);
    }

    void abandon_noexcept_() noexcept {
        relation_ = nullptr;
        catalog_indexes_ = nullptr;
        validation_snapshot_ = nullptr;
        validation_primary_index_ = nullptr;
        validation_scan_ = nullptr;
        for (BufFile*& file : stream_spools_) {
            hash_major_minimizer_pg_detail::abandon_spool(file);
        }
        pending_slots_.clear();
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        pending_tuple_bytes_ = 0u;
        publication_permit_.release();
    }

    void drop_pending_slots_() {
        for (TupleTableSlot* slot : pending_slots_) {
            if (slot != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [slot] { ExecDropSingleTupleTableSlot(slot); });
            }
        }
        pending_slots_.clear();
        pending_tuple_bytes_ = 0u;
    }

    void flush_batch_() {
        if (pending_slots_.empty()) {
            if (pending_tuple_bytes_ != 0u) {
                fail_("hash-major tuple batch accounting disagrees");
            }
            return;
        }
        if (relation_ == nullptr || catalog_indexes_ == nullptr ||
            pending_slots_.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            fail_("hash-major tuple batch state is invalid");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([this] {
            CatalogTuplesMultiInsertWithInfo(relation_,
                                             pending_slots_.data(),
                                             static_cast<int>(pending_slots_.size()),
                                             catalog_indexes_);
        });
        drop_pending_slots_();
    }

    void queue_row_(const index_chunk_address& address, std::span<const std::byte> bytes) {
        HeapTuple tuple =
            hash_major_minimizer_pg_detail::form_row(relation_, query_key_, address, bytes);
        const std::uint64_t tuple_bytes = static_cast<std::uint64_t>(tuple->t_len) +
                                          sizeof(TupleTableSlot*) + sizeof(HeapTupleData);
        const auto decision = bioseq::indexing::plan_index_tuple_batch_append(
            pending_slots_.size(),
            pending_tuple_bytes_,
            tuple_bytes,
            k_batch_rows,
            k_hash_major_minimizer_publication_slice_bytes);
        if (!decision.valid) {
            heap_freetuple(tuple);
            fail_("hash-major row exceeds the bounded publication batch");
        }
        if (decision.flush_before)
            flush_batch_();
        TupleTableSlot* slot = bioseq::postgres::support::invoke_pg_as_cpp([this] {
            return MakeSingleTupleTableSlot(RelationGetDescr(relation_), &TTSOpsHeapTuple);
        });
        try {
            pending_slots_.push_back(slot);
        } catch (...) {
            heap_freetuple(tuple);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [slot] { ExecDropSingleTupleTableSlot(slot); });
            throw;
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [slot, tuple] { ExecStoreHeapTuple(tuple, slot, true); });
        if (tuple_bytes > (std::numeric_limits<std::uint64_t>::max)() - pending_tuple_bytes_) {
            fail_("hash-major publication byte counter overflow");
        }
        pending_tuple_bytes_ += tuple_bytes;
        if (decision.flush_after)
            flush_batch_();
    }

    void
    replay_stream_(hash_major_minimizer_stream_kind kind,
                   const bioseq::indexing::hash_major_minimizer_stream_descriptor& descriptor) {
        using hash_major_minimizer_pg_detail::ceil_rows;
        using hash_major_minimizer_pg_detail::stream_index;
        const std::size_t index = stream_index(kind);
        if (descriptor.kind != kind || descriptor.logical_bytes != logical_bytes_[index] ||
            descriptor.stream_crc64 != logical_crc_[index].checksum() ||
            descriptor.row_count != ceil_rows(descriptor.logical_bytes) ||
            descriptor.row_count >
                static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
            fail_("hash-major stream descriptor disagrees with encoder output");
        }
        hash_major_minimizer_pg_detail::rewind_spool(stream_spools_[index]);
        std::uint64_t remaining = descriptor.logical_bytes;
        for (std::uint64_t row = 0u; row < descriptor.row_count; ++row) {
            const std::size_t bytes = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, k_hash_major_minimizer_row_payload_bytes));
            if (bytes == 0u || (row + 1u < descriptor.row_count &&
                                bytes != k_hash_major_minimizer_row_payload_bytes)) {
                fail_("hash-major physical stream row equation disagrees");
            }
            auto payload = std::span<std::byte>{row_buffer_}.first(bytes);
            hash_major_minimizer_pg_detail::read_spool_exact(stream_spools_[index], payload);
            queue_row_(
                index_chunk_address{
                    .generation_id = generation_id_,
                    .position_segment = 0,
                    .sub_key = hash_major_minimizer_pg_detail::stream_sub_key(kind),
                    .chunk_no = static_cast<std::uint32_t>(row),
                },
                payload);
            remaining -= bytes;
        }
        if (remaining != 0u) {
            fail_("hash-major physical stream replay was incomplete");
        }
    }

    [[nodiscard]] minimizer_generation_loaded_row
    read_exact_row_(const index_chunk_address& expected_address) {
        ScanKeyData keys[5];
        ScanKeyInit(&keys[0],
                    k_minimizer_generation_index_generation_id,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(expected_address.generation_id)));
        ScanKeyInit(&keys[1],
                    k_minimizer_generation_index_query_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2],
                    k_minimizer_generation_index_sub_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(expected_address.sub_key)));
        ScanKeyInit(&keys[3],
                    k_minimizer_generation_index_position_segment,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(expected_address.position_segment));
        ScanKeyInit(&keys[4],
                    k_minimizer_generation_index_chunk_number,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(static_cast<std::int32_t>(expected_address.chunk_no)));
        validation_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([this, &keys] {
            return systable_beginscan_ordered(
                relation_, validation_primary_index_, validation_snapshot_, 5, keys);
        });
        HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { return systable_getnext_ordered(validation_scan_, ForwardScanDirection); });
        if (!HeapTupleIsValid(tuple)) {
            fail_("hash-major validation row is missing");
        }
        minimizer_generation_loaded_row row =
            load_minimizer_generation_row_or_error(relation_, tuple);
        HeapTuple extra = bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { return systable_getnext_ordered(validation_scan_, ForwardScanDirection); });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { systable_endscan_ordered(validation_scan_); });
        validation_scan_ = nullptr;
        if (HeapTupleIsValid(extra) || row.query_key != query_key_ ||
            row.address != expected_address) {
            row.release();
            fail_("hash-major validation row identity is ambiguous");
        }
        return row;
    }

    void
    reread_stream_(hash_major_minimizer_stream_kind kind,
                   const bioseq::indexing::hash_major_minimizer_stream_descriptor& descriptor) {
        ScanKeyData keys[4];
        ScanKeyInit(&keys[0],
                    k_minimizer_generation_index_generation_id,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
        ScanKeyInit(&keys[1],
                    k_minimizer_generation_index_query_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2],
                    k_minimizer_generation_index_sub_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(
                        hash_major_minimizer_pg_detail::stream_sub_key(kind))));
        ScanKeyInit(&keys[3],
                    k_minimizer_generation_index_position_segment,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(0));
        validation_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([this, &keys] {
            return systable_beginscan_ordered(
                relation_, validation_primary_index_, validation_snapshot_, 4, keys);
        });
        crc64_ecma182_accumulator crc;
        std::uint64_t bytes_read = 0u;
        std::uint64_t ordinal = 0u;
        for (HeapTuple tuple; HeapTupleIsValid(
                 tuple = bioseq::postgres::support::invoke_pg_as_cpp([this] {
                     return systable_getnext_ordered(validation_scan_, ForwardScanDirection);
                 }));) {
            minimizer_generation_loaded_row row =
                load_minimizer_generation_row_or_error(relation_, tuple);
            const index_chunk_address expected_address{
                .generation_id = generation_id_,
                .position_segment = 0,
                .sub_key = hash_major_minimizer_pg_detail::stream_sub_key(kind),
                .chunk_no = static_cast<std::uint32_t>(ordinal),
            };
            const std::uint64_t remaining = descriptor.logical_bytes - bytes_read;
            const std::size_t expected_bytes = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, k_hash_major_minimizer_row_payload_bytes));
            if (ordinal >= descriptor.row_count || row.query_key != query_key_ ||
                row.address != expected_address || row.bytes.size() != expected_bytes) {
                row.release();
                fail_("hash-major validation stream row length disagrees");
            }
            crc.update(row.bytes);
            bytes_read += row.bytes.size();
            ++ordinal;
            row.release();
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { systable_endscan_ordered(validation_scan_); });
        validation_scan_ = nullptr;
        if (ordinal != descriptor.row_count || bytes_read != descriptor.logical_bytes ||
            crc.checksum() != descriptor.stream_crc64) {
            fail_("hash-major validation stream aggregate disagrees");
        }
    }

    [[nodiscard]] static bool append_callback_(void* context,
                                               hash_major_minimizer_stream_kind kind,
                                               std::span<const std::byte> bytes) {
        return static_cast<index_storage_hash_major_minimizer_writer*>(context)
            ->append_logical_bytes(kind, bytes);
    }

  public:
    index_storage_hash_major_minimizer_writer() = delete;
    /// Bind a new unpublished generation, acquire its transaction lock, and
    /// allocate the bounded PostgreSQL spool set.
    index_storage_hash_major_minimizer_writer(minimizer_generation_administration_identity identity,
                                              std::uint64_t query_key,
                                              std::uint64_t generation_id)
        : identity_(std::move(identity)), query_key_(query_key), generation_id_(generation_id) {
        try {
            if (identity_.database_oid != MyDatabaseId || !OidIsValid(identity_.tablespace_oid) ||
                !OidIsValid(identity_.main_relfilenode) || !OidIsValid(identity_.user_relid) ||
                identity_.column_attno <= 0 || identity_.index_name.empty() ||
                identity_.table_name.empty() || generation_id_ == 0u ||
                !bioseq::indexing::sql_bigint_can_represent(query_key_) ||
                !bioseq::indexing::sql_bigint_can_represent(generation_id_)) {
                fail_("hash-major writer identity is invalid");
            }
            auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                k_hash_major_minimizer_publication_slice_bytes);
            if (!permit) {
                fail_("hash-major publication memory admission was denied");
            }
            publication_permit_ = std::move(*permit);
            pending_slots_.reserve(k_batch_rows);
            advisory_lock_key_ = index_advisory_xact_lock_key(identity_.database_oid,
                                                              identity_.tablespace_oid,
                                                              identity_.main_relfilenode,
                                                              identity_.column_attno,
                                                              identity_.index_name);
            bioseq::postgres::support::invoke_pg_as_cpp([this] {
                acquire_index_advisory_xact_lock(identity_.database_oid,
                                                 identity_.tablespace_oid,
                                                 identity_.main_relfilenode,
                                                 identity_.column_attno,
                                                 identity_.index_name);
            });
            const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp([this] {
                return get_relname_relid(identity_.table_name.c_str(), bioseq_internal_ns());
            });
            relation_ = OidIsValid(relation_oid)
                            ? bioseq::postgres::support::invoke_pg_as_cpp([relation_oid] {
                                  return bioseq::postgres::support::try_open_postgres_relation(
                                      relation_oid, RowExclusiveLock);
                              })
                            : nullptr;
            if (relation_ == nullptr) {
                fail_("hash-major generation relation is missing");
            }
            const minimizer_generation_schema_inspection schema =
                bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return inspect_minimizer_generation_schema(
                        relation_,
                        nullptr,
                        minimizer_generation_schema_expectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = identity_.table_name,
                            .tablespace_oid = identity_.tablespace_oid,
                        });
                });
            if (!schema) {
                fail_(fmt::format("hash-major generation relation schema mismatch: {}",
                                  minimizer_generation_schema_error_name(schema.error)));
            }
            (void)verify_pg_inline_payload_layout(relation_);
            catalog_indexes_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { return CatalogOpenIndexes(relation_); });
            if (catalog_indexes_ == nullptr) {
                fail_("hash-major CatalogOpenIndexes returned NULL");
            }
            for (BufFile*& file : stream_spools_) {
                file = hash_major_minimizer_pg_detail::create_spool();
            }
        } catch (...) {
            state_ = operation_state::failed;
            abandon_noexcept_();
            throw;
        }
    }
    index_storage_hash_major_minimizer_writer(const index_storage_hash_major_minimizer_writer&) =
        delete;
    index_storage_hash_major_minimizer_writer&
    operator=(const index_storage_hash_major_minimizer_writer&) = delete;
    index_storage_hash_major_minimizer_writer(
        index_storage_hash_major_minimizer_writer&& other) noexcept
        : identity_(std::move(other.identity_)), relation_(std::exchange(other.relation_, nullptr)),
          catalog_indexes_(std::exchange(other.catalog_indexes_, nullptr)),
          validation_snapshot_(std::exchange(other.validation_snapshot_, nullptr)),
          validation_primary_index_(std::exchange(other.validation_primary_index_, nullptr)),
          validation_scan_(std::exchange(other.validation_scan_, nullptr)),
          stream_spools_(other.stream_spools_), advisory_lock_key_(other.advisory_lock_key_),
          query_key_(other.query_key_), generation_id_(other.generation_id_),
          logical_bytes_(other.logical_bytes_), logical_crc_(other.logical_crc_),
          publication_permit_(std::move(other.publication_permit_)),
          pending_slots_(std::move(other.pending_slots_)),
          pending_tuple_bytes_(other.pending_tuple_bytes_), row_buffer_(other.row_buffer_),
          state_(other.state_) {
        other.stream_spools_.fill(nullptr);
        other.pending_tuple_bytes_ = 0u;
        other.state_ = operation_state::moved;
    }
    index_storage_hash_major_minimizer_writer&
    operator=(index_storage_hash_major_minimizer_writer&&) = delete;
    ~index_storage_hash_major_minimizer_writer() {
        abandon_noexcept_();
    }

    /// Append bytes to one logical stream while updating its exact CRC receipt.
    [[nodiscard]] bool append_logical_bytes(hash_major_minimizer_stream_kind kind,
                                            std::span<const std::byte> bytes) {
        if (state_ != operation_state::accepting ||
            !hash_major_minimizer_pg_detail::valid_stream_kind(kind) || bytes.empty()) {
            return false;
        }
        const std::size_t index = hash_major_minimizer_pg_detail::stream_index(kind);
        if (bytes.size() > (std::numeric_limits<std::uint64_t>::max)() - logical_bytes_[index]) {
            return false;
        }
        hash_major_minimizer_pg_detail::append_spool(stream_spools_[index], bytes);
        logical_crc_[index].update(bytes);
        logical_bytes_[index] += bytes.size();
        return true;
    }

    /// Return the non-owning callback bridge consumed by the common encoder.
    [[nodiscard]] hash_major_minimizer_logical_stream_sink_ref logical_stream_sink() noexcept {
        return {
            .context = this,
            .append_bytes = &append_callback_,
        };
    }

    /// Replay, reread, and authenticate all streams before issuing publication
    /// authority for the exact generation.
    [[nodiscard]] ready_hash_major_minimizer_token
    seal_and_validate_ready(const hash_major_minimizer_encoded_metadata& metadata,
                            minimizer_generation_source_authorization source) {
        try {
            if (state_ != operation_state::accepting) {
                fail_("hash-major writer was sealed more than once");
            }
            auto decoded_head =
                bioseq::indexing::decode_hash_major_minimizer_head(metadata.encoded_head);
            auto decoded_manifest =
                bioseq::indexing::decode_hash_major_minimizer_manifest(metadata.encoded_manifest);
            if (!decoded_head || !decoded_manifest || *decoded_head != metadata.head ||
                *decoded_manifest != metadata.manifest ||
                metadata.head.generation_id != generation_id_ ||
                metadata.head.query_key != query_key_ ||
                metadata.manifest.generation_id != generation_id_ ||
                metadata.head.dense_seal.has_value() ||
                metadata.manifest.streams[0].kind != hash_major_minimizer_stream_kind::directory ||
                metadata.manifest.streams[1].kind != hash_major_minimizer_stream_kind::controls ||
                metadata.manifest.streams[2].kind != hash_major_minimizer_stream_kind::postings) {
                fail_("hash-major encoded metadata disagrees");
            }
            const TransactionId transaction_id = bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return GetCurrentTransactionIdIfAny(); });
            if (!source.valid_ || source.snapshot_ == nullptr ||
                !IsMVCCSnapshot(source.snapshot_) || source.snapshot_->regd_count == 0 ||
                !TransactionIdIsValid(transaction_id) || source.transaction_id_ != transaction_id ||
                source.database_oid_ != identity_.database_oid ||
                source.tablespace_oid_ != identity_.tablespace_oid ||
                source.main_relfilenode_ != identity_.main_relfilenode ||
                source.user_relid_ != identity_.user_relid ||
                source.column_attno_ != identity_.column_attno ||
                source.index_name_ != identity_.index_name ||
                source.table_name_ != identity_.table_name ||
                source.source_.sequence_rowno != query_key_) {
                source.invalidate_();
                fail_("hash-major source authorization is not bound to this writer transaction");
            }
            for (std::size_t index = 0u; index < 3u; ++index) {
                const auto& stream = metadata.manifest.streams[index];
                if (stream.logical_bytes != logical_bytes_[index] ||
                    stream.stream_crc64 != logical_crc_[index].checksum() ||
                    stream.row_count !=
                        hash_major_minimizer_pg_detail::ceil_rows(stream.logical_bytes)) {
                    fail_("hash-major logical stream receipt disagrees");
                }
            }

            // Authority is persisted only after all postings, then controls,
            // then the directory.  The primary-key layout remains unchanged;
            // the head is still absent, so none of these rows is visible as a
            // READY generation during replay.
            replay_stream_(hash_major_minimizer_stream_kind::postings,
                           metadata.manifest.streams[2]);
            replay_stream_(hash_major_minimizer_stream_kind::controls,
                           metadata.manifest.streams[1]);
            replay_stream_(hash_major_minimizer_stream_kind::directory,
                           metadata.manifest.streams[0]);
            queue_row_(
                index_chunk_address{
                    .generation_id = generation_id_,
                    .position_segment = 0,
                    .sub_key = 0,
                    .chunk_no = 0u,
                },
                metadata.encoded_manifest);
            flush_batch_();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { CatalogCloseIndexes(catalog_indexes_); });
            catalog_indexes_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
            if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return ActiveSnapshotSet(); })) {
                fail_("hash-major validation requires an active snapshot");
            }
            Snapshot active_snapshot =
                bioseq::postgres::support::invoke_pg_as_cpp([] { return GetActiveSnapshot(); });
            if (!IsMVCCSnapshot(active_snapshot)) {
                fail_("hash-major validation snapshot is not MVCC");
            }
            bioseq::postgres::support::invoke_pg_as_cpp([] { UpdateActiveSnapshotCommandId(); });
            validation_snapshot_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [active_snapshot] { return RegisterSnapshot(active_snapshot); });
            const Oid primary_index_oid = bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { return RelationGetPrimaryKeyIndex(relation_); });
            validation_primary_index_ =
                OidIsValid(primary_index_oid)
                    ? bioseq::postgres::support::invoke_pg_as_cpp([primary_index_oid] {
                          return index_open(primary_index_oid, AccessShareLock);
                      })
                    : nullptr;
            if (validation_primary_index_ == nullptr) {
                fail_("hash-major validation primary index is missing");
            }

            minimizer_generation_loaded_row manifest_row = read_exact_row_(index_chunk_address{
                .generation_id = generation_id_,
                .position_segment = 0,
                .sub_key = 0,
                .chunk_no = 0u,
            });
            if (manifest_row.bytes.size() != k_hash_major_minimizer_manifest_bytes ||
                !std::ranges::equal(manifest_row.bytes, metadata.encoded_manifest)) {
                manifest_row.release();
                fail_("hash-major validation manifest disagrees");
            }
            manifest_row.release();
            reread_stream_(hash_major_minimizer_stream_kind::directory,
                           metadata.manifest.streams[0]);
            reread_stream_(hash_major_minimizer_stream_kind::controls,
                           metadata.manifest.streams[1]);
            reread_stream_(hash_major_minimizer_stream_kind::postings,
                           metadata.manifest.streams[2]);

            const auto expected_source = bioseq::indexing::hash_major_minimizer_source_identity(
                query_key_,
                source.source_.locator.source_epoch,
                source.source_content_checksum_,
                source.source_.locator.logical_length);
            if (!source.valid_ || source.source_.sequence_rowno != query_key_ ||
                source.table_name_ != identity_.table_name ||
                source.source_.locator.source_epoch != metadata.manifest.source_epoch ||
                source.source_content_checksum_ != metadata.manifest.source_content_crc64 ||
                source.source_.locator.logical_length != metadata.manifest.target_length ||
                source.source_.alphabet_code != metadata.manifest.storage_profile.alphabet_code ||
                expected_source != metadata.manifest.source_identity ||
                expected_source != metadata.head.source_identity) {
                source.invalidate_();
                fail_("hash-major validation source authorization disagrees");
            }
            source.invalidate_();

            Relation finished_index = std::exchange(validation_primary_index_, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished_index] { index_close(finished_index, AccessShareLock); });
            Snapshot finished_snapshot = std::exchange(validation_snapshot_, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished_snapshot] { UnregisterSnapshot(finished_snapshot); });
            for (BufFile*& file : stream_spools_) {
                hash_major_minimizer_pg_detail::close_spool(file);
            }
            publication_permit_.release();

            const Oid validated_relation_oid = RelationGetRelid(relation_);
            const RelFileLocator validated_locator = relation_->rd_locator;
            Relation finished_relation = std::exchange(relation_, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished_relation] { relation_close(finished_relation, RowExclusiveLock); });
            const std::uint64_t head_row_checksum = bioseq::indexing::crc64_index_chunk_row(
                0u,
                -1,
                k_hash_major_generation_head_sub_key,
                0u,
                static_cast<std::uint32_t>(metadata.encoded_head.size()),
                metadata.encoded_head);
            state_ = operation_state::sealed;
            minimizer_generation_administration_identity validated_identity = std::move(identity_);
            return ready_hash_major_minimizer_token{
                std::move(validated_identity),
                validated_relation_oid,
                validated_locator,
                advisory_lock_key_,
                query_key_,
                generation_id_,
                transaction_id,
                metadata.head,
                metadata.encoded_head,
                head_row_checksum,
            };
        } catch (...) {
            state_ = operation_state::failed;
            abandon_noexcept_();
            throw;
        }
    }
};

static_assert(!std::is_default_constructible_v<index_storage_hash_major_minimizer_writer>);
static_assert(!std::is_copy_constructible_v<index_storage_hash_major_minimizer_writer>);
static_assert(std::is_nothrow_move_constructible_v<index_storage_hash_major_minimizer_writer>);

[[nodiscard]] inline ready_hash_major_minimizer_published_head_receipt
publish_ready_hash_major_minimizer_head_pg(ready_hash_major_minimizer_token&& token) {
    Relation relation = nullptr;
    Relation primary_index = nullptr;
    SysScanDesc scan = nullptr;
    HeapTuple replacement = nullptr;
    try {
        if (!token.valid_ || token.identity_.database_oid != MyDatabaseId ||
            token.generation_id_ == 0u || token.head_.generation_id != token.generation_id_ ||
            token.head_.query_key != token.query_key_ ||
            token.transaction_id_ != bioseq::postgres::support::invoke_pg_as_cpp(
                                         [] { return GetCurrentTransactionIdIfAny(); }) ||
            token.advisory_lock_key_ !=
                index_advisory_xact_lock_key(token.identity_.database_oid,
                                             token.identity_.tablespace_oid,
                                             token.identity_.main_relfilenode,
                                             token.identity_.column_attno,
                                             token.identity_.index_name) ||
            token.head_row_checksum_ != bioseq::indexing::crc64_index_chunk_row(
                                            0u,
                                            -1,
                                            k_hash_major_generation_head_sub_key,
                                            0u,
                                            static_cast<std::uint32_t>(token.head_bytes_.size()),
                                            token.head_bytes_)) {
            minimizer_generation_storage_error_as_cpp("hash-major publication token is invalid");
        }
        auto decoded_head = bioseq::indexing::decode_hash_major_minimizer_head(token.head_bytes_);
        if (!decoded_head || *decoded_head != token.head_) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major publication token head disagrees");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([&token] {
            acquire_index_advisory_xact_lock(token.identity_.database_oid,
                                             token.identity_.tablespace_oid,
                                             token.identity_.main_relfilenode,
                                             token.identity_.column_attno,
                                             token.identity_.index_name);
        });
        relation = bioseq::postgres::support::invoke_pg_as_cpp([&token] {
            return bioseq::postgres::support::try_open_postgres_relation(
                token.sidecar_relation_oid_, RowExclusiveLock);
        });
        if (relation == nullptr || RelationGetNamespace(relation) != bioseq_internal_ns() ||
            std::string_view{RelationGetRelationName(relation)} != token.identity_.table_name ||
            relation->rd_locator.spcOid != token.sidecar_locator_.spcOid ||
            relation->rd_locator.dbOid != token.sidecar_locator_.dbOid ||
            relation->rd_locator.relNumber != token.sidecar_locator_.relNumber) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major publication relation identity changed");
        }
        const minimizer_generation_schema_inspection schema =
            bioseq::postgres::support::invoke_pg_as_cpp([relation, &token] {
                return inspect_minimizer_generation_schema(
                    relation,
                    nullptr,
                    minimizer_generation_schema_expectation{
                        .namespace_oid = bioseq_internal_ns(),
                        .relation_name = token.identity_.table_name,
                        .tablespace_oid = token.identity_.tablespace_oid,
                    });
            });
        if (!schema) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major publication relation schema changed");
        }
        primary_index = bioseq::postgres::support::invoke_pg_as_cpp(
            [&schema] { return index_open(schema.primary_index_oid, AccessShareLock); });
        replacement = hash_major_minimizer_pg_detail::form_row(
            relation,
            token.query_key_,
            index_chunk_address{
                .generation_id = 0u,
                .position_segment = -1,
                .sub_key = k_hash_major_generation_head_sub_key,
                .chunk_no = 0u,
            },
            token.head_bytes_,
            token.head_row_checksum_);

        ScanKeyData keys[5];
        ScanKeyInit(&keys[0],
                    k_minimizer_generation_index_generation_id,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(0));
        ScanKeyInit(&keys[1],
                    k_minimizer_generation_index_query_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(token.query_key_)));
        ScanKeyInit(&keys[2],
                    k_minimizer_generation_index_sub_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(k_hash_major_generation_head_sub_key)));
        ScanKeyInit(&keys[3],
                    k_minimizer_generation_index_position_segment,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(-1));
        ScanKeyInit(&keys[4],
                    k_minimizer_generation_index_chunk_number,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(0));
        scan = bioseq::postgres::support::invoke_pg_as_cpp([relation, primary_index, &keys] {
            return systable_beginscan_ordered(relation, primary_index, SnapshotSelf, 5, keys);
        });
        HeapTuple previous_tuple = bioseq::postgres::support::invoke_pg_as_cpp(
            [scan] { return systable_getnext_ordered(scan, ForwardScanDirection); });
        std::optional<std::uint64_t> replaced_generation_id;
        ItemPointerData previous_tid{};
        bool replace = HeapTupleIsValid(previous_tuple);
        if (replace) {
            previous_tid = previous_tuple->t_self;
            minimizer_generation_loaded_row previous =
                load_minimizer_generation_row_or_error(relation, previous_tuple);
            if (previous.query_key != token.query_key_ ||
                previous.address !=
                    index_chunk_address{
                        .generation_id = 0u,
                        .position_segment = -1,
                        .sub_key = k_hash_major_generation_head_sub_key,
                        .chunk_no = 0u,
                    } ||
                previous.bytes.size() != k_hash_major_minimizer_head_bytes) {
                previous.release();
                minimizer_generation_storage_error_as_cpp(
                    "previous hash-major head row is malformed");
            }
            auto previous_head = bioseq::indexing::decode_hash_major_minimizer_head(previous.bytes);
            previous.release();
            if (!previous_head || previous_head->generation_id == 0u ||
                previous_head->generation_id == token.generation_id_) {
                minimizer_generation_storage_error_as_cpp(
                    "previous hash-major head generation is invalid");
            }
            replaced_generation_id = previous_head->generation_id;
        }
        HeapTuple extra = bioseq::postgres::support::invoke_pg_as_cpp(
            [scan] { return systable_getnext_ordered(scan, ForwardScanDirection); });
        bioseq::postgres::support::invoke_pg_as_cpp([scan] { systable_endscan_ordered(scan); });
        scan = nullptr;
        if (HeapTupleIsValid(extra)) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major head primary key returned duplicates");
        }
        if (replace) {
            bioseq::postgres::support::invoke_pg_as_cpp([relation, &previous_tid, replacement] {
                CatalogTupleUpdate(relation, &previous_tid, replacement);
            });
        } else {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation, replacement] { CatalogTupleInsert(relation, replacement); });
        }
        heap_freetuple(replacement);
        replacement = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });

        // systable_beginscan_ordered() rewrites heap attribute numbers in
        // place to index ordinals. Reinitialize before reusing this array;
        // passing the already-remapped keys through a second time produces
        // the non-monotone ordinal sequence 2,1,4,3,5 for this primary key.
        ScanKeyInit(&keys[0],
                    k_minimizer_generation_index_generation_id,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(0));
        ScanKeyInit(&keys[1],
                    k_minimizer_generation_index_query_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(token.query_key_)));
        ScanKeyInit(&keys[2],
                    k_minimizer_generation_index_sub_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(k_hash_major_generation_head_sub_key)));
        ScanKeyInit(&keys[3],
                    k_minimizer_generation_index_position_segment,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(-1));
        ScanKeyInit(&keys[4],
                    k_minimizer_generation_index_chunk_number,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(0));
        scan = bioseq::postgres::support::invoke_pg_as_cpp([relation, primary_index, &keys] {
            return systable_beginscan_ordered(relation, primary_index, SnapshotSelf, 5, keys);
        });
        HeapTuple published_tuple = bioseq::postgres::support::invoke_pg_as_cpp(
            [scan] { return systable_getnext_ordered(scan, ForwardScanDirection); });
        if (!HeapTupleIsValid(published_tuple)) {
            minimizer_generation_storage_error_as_cpp("published hash-major head is missing");
        }
        minimizer_generation_loaded_row published =
            load_minimizer_generation_row_or_error(relation, published_tuple);
        if (published.query_key != token.query_key_ ||
            published.address !=
                index_chunk_address{
                    .generation_id = 0u,
                    .position_segment = -1,
                    .sub_key = k_hash_major_generation_head_sub_key,
                    .chunk_no = 0u,
                } ||
            !std::ranges::equal(published.bytes, token.head_bytes_)) {
            published.release();
            minimizer_generation_storage_error_as_cpp(
                "published hash-major head differs from its token");
        }
        published.release();
        HeapTuple published_extra = bioseq::postgres::support::invoke_pg_as_cpp(
            [scan] { return systable_getnext_ordered(scan, ForwardScanDirection); });
        bioseq::postgres::support::invoke_pg_as_cpp([scan] { systable_endscan_ordered(scan); });
        scan = nullptr;
        if (HeapTupleIsValid(published_extra)) {
            minimizer_generation_storage_error_as_cpp("published hash-major head is not unique");
        }
        Relation finished_index = std::exchange(primary_index, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_index] { index_close(finished_index, AccessShareLock); });
        Relation finished_relation = std::exchange(relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_relation] { relation_close(finished_relation, RowExclusiveLock); });
        ready_hash_major_minimizer_published_head_receipt receipt{
            .table_name = token.identity_.table_name,
            .query_key = token.query_key_,
            .head = token.head_,
            .head_bytes = token.head_bytes_,
            .replaced_generation_id = replaced_generation_id,
        };
        token.invalidate_();
        return receipt;
    } catch (...) {
        // Do not invoke PostgreSQL cleanup while propagating a translated
        // ERROR. The current memory context and ResourceOwner retain every
        // outstanding tuple/scan/relation object until statement abort.
        replacement = nullptr;
        scan = nullptr;
        primary_index = nullptr;
        relation = nullptr;
        token.invalidate_();
        throw;
    }
}

} // namespace bioseq::storage::indexing
