/**
 * @file catalog_row_io.cppm
 * @brief Defines immutable generation-row formation, deformation, projection, and authentication.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/transam.h"
#include "executor/tuptable.h"
#include "storage/bufpage.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "varatt.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

export module bioseq.postgres.storage.indexing.chunked_storage:catalog_row_io;

import std;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.minimizer_fragment_image; // explicit fixed-endian minimizer fragment image image ABI
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;

import :catalog_contracts;
import :catalog_metadata;
import :catalog_source_authorization;
import :catalog_storage_descriptors;
export namespace bioseq::storage::indexing {
/// Register a private MVCC copy for rereading rows after CommandCounterIncrement.
/// Source readers may retain the caller's active snapshot. Refresh only the
/// copy, never their visibility boundary; the caller must unregister the result.
[[nodiscard]] inline Snapshot register_minimizer_reread_snapshot_pg(Snapshot source) {
    return bioseq::postgres::support::invoke_pg_as_cpp([source] {
        PushCopiedSnapshot(source);
        Snapshot volatile registered = nullptr;
        PG_TRY();
        {
            UpdateActiveSnapshotCommandId();
            registered = RegisterSnapshot(GetActiveSnapshot());
        }
        PG_CATCH();
        {
            PopActiveSnapshot();
            PG_RE_THROW();
        }
        PG_END_TRY();
        PopActiveSnapshot();
        return registered;
    });
}

/// Form one immutable current-generation payload/directory/manifest tuple. The caller
/// transfers the tuple into a TTSOpsHeapTuple slot and flushes a bounded group
/// with CatalogTuplesMultiInsertWithInfo, PostgreSQL 17's supported catalog
/// heap-multi-insert + index-maintenance API. This helper never inserts, probes
/// for, or replaces a row by itself.
[[nodiscard]] inline std::expected<HeapTuple, minimizer_generation_encoding_error>
form_minimizer_generation_row_pg(Relation relation,
                                 std::uint64_t query_key,
                                 const validated_minimizer_generation_row& validated_row,
                                 bool inject_failure = true) {
    const minimizer_generation_row_view& row = validated_row.row();
    if (relation == nullptr || !bioseq::indexing::sql_bigint_can_represent(query_key)) {
        return std::unexpected(minimizer_generation_encoding_error::generation_id);
    }
    bytea* blob = bioseq::postgres::support::invoke_pg_as_cpp(
        [&row] { return reinterpret_cast<bytea*>(palloc(VARHDRSZ + row.bytes.size())); });
    SET_VARSIZE(blob, VARHDRSZ + row.bytes.size());
    std::memcpy(VARDATA(blob), row.bytes.data(), row.bytes.size());
    bioseq::postgres::support::pg_datum_list<8> values{};
    values.set(k_minimizer_generation_attribute_query_key, static_cast<std::int64_t>(query_key));
    values.set(k_minimizer_generation_attribute_generation_id,
               static_cast<std::int64_t>(row.address.generation_id));
    values.set(k_minimizer_generation_attribute_position_segment, row.address.position_segment);
    values.set(k_minimizer_generation_attribute_sub_key,
               static_cast<std::int64_t>(row.address.sub_key));
    values.set(k_minimizer_generation_attribute_chunk_number,
               static_cast<std::int32_t>(row.address.chunk_no));
    values.set(k_minimizer_generation_attribute_chunk_bytes,
               static_cast<std::int32_t>(row.bytes.size()));
    values.set(k_minimizer_generation_attribute_checksum, checksum_to_pg_bigint(row.row_checksum));
    values.set_raw(k_minimizer_generation_attribute_blob,
                   bioseq::postgres::support::pg_datum::from_pointer(blob));
    HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
        [&values, relation] { return values.form_tuple(RelationGetDescr(relation)); });
    if (inject_failure) {
        inject_minimizer_generation_failure_as_cpp(
            minimizer_generation_failure_injection::generation_tuple_form, "generation_tuple_form");
    }
    bioseq::postgres::support::invoke_pg_as_cpp([blob] { pfree(blob); });
    return tuple;
}

/// Exact PostgreSQL ABI validation result for the S1 one-page PLAIN payload contract.
struct pg_inline_payload_layout {
    std::uint32_t block_bytes = 0;
    std::uint32_t max_heap_tuple_bytes = 0;
    std::uint32_t fixed_tuple_bytes = 0;
    std::uint32_t payload_bytes = 0;
    std::uint32_t alignment_multiple = 0;
    std::uint32_t full_tuple_bytes = 0;
};

[[nodiscard]] inline pg_inline_payload_layout verify_pg_inline_payload_layout(Relation relation) {
    static std::optional<pg_inline_payload_layout> cached;
    if (cached)
        return *cached;
    if (relation == nullptr || BLCKSZ != 8192 || RelationGetDescr(relation) == nullptr ||
        RelationGetDescr(relation)->natts !=
            static_cast<int>(k_minimizer_generation_index_attribute_descriptors.size())) {
        minimizer_generation_storage_error_as_cpp(
            "grouped-SoA storage requires BLCKSZ=8192 and the exact tuple descriptor");
    }
    constexpr std::uint32_t k_payload_bytes = bioseq::indexing::k_grouped_soa_extent_payload_bytes;
    constexpr std::uint32_t k_alignment_multiple = 96u;
    static_assert(k_payload_bytes % k_alignment_multiple == 0u);
    static_assert(k_payload_bytes % 16u == 0u);
    static_assert(k_payload_bytes % 12u == 0u);
    static_assert(k_payload_bytes % 8u == 0u);
    static_assert(k_payload_bytes % 4u == 0u);

    std::array<std::byte, k_payload_bytes> payload{};
    const index_chunk_address address{
        .generation_id = 1u,
        .position_segment = 0,
        .sub_key = 0,
        .chunk_no = 0u,
    };
    const minimizer_generation_row_view row{
        .address = address,
        .bytes = payload,
        .row_checksum = bioseq::indexing::crc64_index_chunk_row(address.generation_id,
                                                                address.position_segment,
                                                                address.sub_key,
                                                                address.chunk_no,
                                                                k_payload_bytes,
                                                                payload),
    };
    auto validated = validate_minimizer_generation_row_for_write(1u, row);
    if (!validated) {
        minimizer_generation_storage_error_as_cpp(
            "grouped-SoA representative row validation failed");
    }
    auto tuple =
        form_minimizer_generation_row_pg(relation, 1u, *validated, /*inject_failure=*/false);
    if (!tuple) {
        minimizer_generation_storage_error_as_cpp(
            "grouped-SoA representative tuple formation failed");
    }
    const std::uint32_t tuple_bytes = (*tuple)->t_len;
    heap_freetuple(*tuple);
    if (tuple_bytes < k_payload_bytes) {
        minimizer_generation_storage_error_as_cpp(
            "grouped-SoA representative tuple length underflow");
    }
    const std::uint32_t fixed_tuple_bytes = tuple_bytes - k_payload_bytes;
    const std::uint32_t max_payload_by_tuple =
        static_cast<std::uint32_t>(MaxHeapTupleSize) - fixed_tuple_bytes;
    const std::uint32_t largest_aligned_payload =
        max_payload_by_tuple / k_alignment_multiple * k_alignment_multiple;
    const std::uint32_t page_payload_capacity = BLCKSZ - SizeOfPageHeaderData;
    if (tuple_bytes != 8140u || tuple_bytes > static_cast<std::uint32_t>(MaxHeapTupleSize) ||
        MAXALIGN(tuple_bytes) + sizeof(ItemIdData) > page_payload_capacity ||
        largest_aligned_payload != k_payload_bytes) {
        minimizer_generation_storage_error_as_cpp(
            "grouped-SoA 8064-byte payload does not satisfy the one-page tuple equation");
    }
    cached = pg_inline_payload_layout{
        .block_bytes = BLCKSZ,
        .max_heap_tuple_bytes = MaxHeapTupleSize,
        .fixed_tuple_bytes = fixed_tuple_bytes,
        .payload_bytes = k_payload_bytes,
        .alignment_multiple = k_alignment_multiple,
        .full_tuple_bytes = tuple_bytes,
    };
    return *cached;
}

/// Callback-scoped payload row with address-bound checksum authority.
struct minimizer_generation_loaded_row {
    std::uint64_t query_key = 0;
    index_chunk_address address{};
    std::uint32_t chunk_bytes = 0;
    std::uint64_t checksum = 0;
    // The schema is frozen to PLAIN and the loader rejects every external or
    // compressed datum. Payload bytes either borrow the current scan slot or
    // one admitted batch; neither form needs detoast ownership or pfree.
    std::span<const std::byte> bytes;
    std::optional<validated_minimizer_generation_row_receipt> validated_receipt;

    void release() noexcept {
        // The receipt seals a callback-scoped span into blob storage. Destroy
        // that authority before the scan slot may be advanced.
        validated_receipt.reset();
        bytes = {};
    }
};

/// One deformation of the frozen eight-column heap tuple.  This object is
/// deliberately trivial: the hot full-generation scan can form it inside one
/// outer PostgreSQL error boundary without a longjmp crossing a C++ owner.
struct minimizer_generation_deformed_row {
    HeapTuple tuple = nullptr;
    std::array<Datum, k_minimizer_generation_index_attribute_descriptors.size()> values{};
    std::array<bool, k_minimizer_generation_index_attribute_descriptors.size()> nulls{};
};

static_assert(std::is_trivially_copyable_v<minimizer_generation_deformed_row>);
static_assert(std::is_trivially_destructible_v<minimizer_generation_deformed_row>);

/// Trivial optional row returned from one protected ordered-scan step.
struct minimizer_generation_next_deformed_row {
    minimizer_generation_deformed_row row{};
    bool found = false;
};

static_assert(std::is_trivially_copyable_v<minimizer_generation_next_deformed_row>);
static_assert(std::is_trivially_destructible_v<minimizer_generation_next_deformed_row>);

/// PostgreSQL leaf: deform all fixed attributes once instead of entering one
/// PG_TRY/sigsetjmp bridge per heap_getattr call.  Call either from an existing
/// invoke_pg_as_cpp boundary or through deform_minimizer_generation_row_or_error().
[[nodiscard]] inline minimizer_generation_deformed_row
deform_minimizer_generation_row_pg_leaf(Relation relation, HeapTuple tuple) {
    minimizer_generation_deformed_row deformed{.tuple = tuple};
    heap_deform_tuple(
        tuple, RelationGetDescr(relation), deformed.values.data(), deformed.nulls.data());
    return deformed;
}

/// Standalone deformation bridge used by metadata, publication validation,
/// and selective reads. The full-generation route instead combines its fetch
/// and deformation through next_minimizer_generation_row_or_error().
[[nodiscard]] inline minimizer_generation_deformed_row
deform_minimizer_generation_row_or_error(Relation relation, HeapTuple tuple) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [relation, tuple] { return deform_minimizer_generation_row_pg_leaf(relation, tuple); });
}

/// Fetch and deform one ordered-scan row under one PostgreSQL boundary. The
/// returned object is trivial. Authentication and arbitrary sinks run only
/// after PG_END_TRY has restored PostgreSQL's exception stack.
[[nodiscard]] inline minimizer_generation_next_deformed_row
next_minimizer_generation_row_or_error(Relation relation, SysScanDesc scan) {
    return bioseq::postgres::support::invoke_pg_as_cpp([relation, scan] {
        HeapTuple tuple = systable_getnext_ordered(scan, ForwardScanDirection);
        if (!HeapTupleIsValid(tuple))
            return minimizer_generation_next_deformed_row{};
        return minimizer_generation_next_deformed_row{
            .row = deform_minimizer_generation_row_pg_leaf(relation, tuple),
            .found = true,
        };
    });
}

/// Trivial borrowed projection used while a PostgreSQL sigsetjmp frame is
/// active.  It performs no allocation, owns no receipt, and reports semantic
/// failure as a static message for the C++ caller to raise after leaving the
/// PostgreSQL boundary.
struct minimizer_generation_projected_row {
    std::uint64_t query_key = 0;
    index_chunk_address address{};
    std::uint32_t chunk_bytes = 0;
    std::uint64_t checksum = 0;
    std::span<const std::byte> bytes{};
    const char* error = nullptr;
};

static_assert(std::is_trivially_copyable_v<minimizer_generation_projected_row>);
static_assert(std::is_trivially_destructible_v<minimizer_generation_projected_row>);

/// Decode the frozen eight-column projection and validate inline PLAIN/length
/// shape without constructing any C++ authority object.  Address-bound CRC
/// validation remains outside PostgreSQL error frames.
[[nodiscard]] inline minimizer_generation_projected_row project_minimizer_generation_deformed_row(
    const minimizer_generation_deformed_row& deformed) noexcept {
    const auto read_attribute = [&deformed](AttrNumber attribute_number) {
        return deformed.values[static_cast<std::size_t>(attribute_number - 1)];
    };
    const auto attribute_is_null = [&deformed](AttrNumber attribute_number) {
        return deformed.nulls[static_cast<std::size_t>(attribute_number - 1)];
    };
    const auto failed = [](const char* message) noexcept {
        return minimizer_generation_projected_row{.error = message};
    };
    const Datum query_key_datum = read_attribute(k_minimizer_generation_attribute_query_key);
    if (attribute_is_null(k_minimizer_generation_attribute_query_key)) {
        return failed("row query key is NULL");
    }
    const std::int64_t signed_query_key = DatumGetInt64(query_key_datum);
    if (signed_query_key < 0)
        return failed("row query key is negative");
    const Datum generation_datum = read_attribute(k_minimizer_generation_attribute_generation_id);
    if (attribute_is_null(k_minimizer_generation_attribute_generation_id)) {
        return failed("row generation ID is NULL");
    }
    const std::int64_t signed_generation_id = DatumGetInt64(generation_datum);
    if (signed_generation_id < 0) {
        return failed("row generation ID is negative");
    }
    const Datum position_datum = read_attribute(k_minimizer_generation_attribute_position_segment);
    if (attribute_is_null(k_minimizer_generation_attribute_position_segment)) {
        return failed("row position segment is NULL");
    }
    const std::int32_t position_segment = DatumGetInt32(position_datum);
    const Datum sub_key_datum = read_attribute(k_minimizer_generation_attribute_sub_key);
    if (attribute_is_null(k_minimizer_generation_attribute_sub_key)) {
        return failed("row sub-key is NULL");
    }
    const std::int64_t sub_key = DatumGetInt64(sub_key_datum);
    const Datum chunk_no_datum = read_attribute(k_minimizer_generation_attribute_chunk_number);
    if (attribute_is_null(k_minimizer_generation_attribute_chunk_number)) {
        return failed("row chunk number is NULL");
    }
    const std::int32_t signed_chunk_no = DatumGetInt32(chunk_no_datum);
    if (signed_chunk_no < 0)
        return failed("row chunk number is negative");
    const Datum chunk_bytes_datum = read_attribute(k_minimizer_generation_attribute_chunk_bytes);
    if (attribute_is_null(k_minimizer_generation_attribute_chunk_bytes)) {
        return failed("row byte count is NULL");
    }
    const std::int32_t signed_chunk_bytes = DatumGetInt32(chunk_bytes_datum);
    if (signed_chunk_bytes <= 0) {
        return failed("row byte count is non-positive");
    }
    const Datum checksum_datum = read_attribute(k_minimizer_generation_attribute_checksum);
    if (attribute_is_null(k_minimizer_generation_attribute_checksum)) {
        return failed("row checksum is NULL");
    }
    const std::int64_t signed_checksum = DatumGetInt64(checksum_datum);
    const Datum blob_datum = read_attribute(k_minimizer_generation_attribute_blob);
    if (attribute_is_null(k_minimizer_generation_attribute_blob))
        return failed("row blob is NULL");
    auto* original_blob = reinterpret_cast<varlena*>(DatumGetPointer(blob_datum));
    if (original_blob == nullptr)
        return failed("row blob pointer is NULL");
    if (VARATT_IS_EXTERNAL(original_blob) || VARATT_IS_COMPRESSED(original_blob)) {
        return failed("PLAIN row contains toasted/compressed data");
    }
    auto* inline_blob = reinterpret_cast<bytea*>(original_blob);
    const std::size_t payload_bytes = VARSIZE_ANY_EXHDR(inline_blob);
    if (payload_bytes != static_cast<std::size_t>(signed_chunk_bytes)) {
        return failed("row byte count disagrees with BYTEA length");
    }
    const index_chunk_address address{
        .generation_id = static_cast<std::uint64_t>(signed_generation_id),
        .position_segment = position_segment,
        .sub_key = sub_key,
        .chunk_no = static_cast<std::uint32_t>(signed_chunk_no),
    };
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(VARDATA_ANY(inline_blob)), payload_bytes};
    const std::uint64_t checksum = checksum_from_pg_bigint(signed_checksum);
    return minimizer_generation_projected_row{
        .query_key = static_cast<std::uint64_t>(signed_query_key),
        .address = address,
        .chunk_bytes = static_cast<std::uint32_t>(signed_chunk_bytes),
        .checksum = checksum,
        .bytes = bytes,
    };
}

/// Bound simultaneous heap-buffer pins while amortizing PostgreSQL's
/// PG_TRY/sigsetjmp bridge. At the measured 562,075-row production geometry,
/// K=128 reduces the hot-path boundary count to at most 4,392 without copying
/// or materializing the 4.46-GB payload image.
inline constexpr std::size_t k_minimizer_generation_full_scan_pinned_batch_rows = 128;

/// Projected row paired with the TID of its independently pinned tuple.
struct pinned_minimizer_generation_projected_row {
    minimizer_generation_projected_row projected{};
    ItemPointerData tid{};
};

static_assert(std::is_trivially_copyable_v<pinned_minimizer_generation_projected_row>);
static_assert(std::is_trivially_destructible_v<pinned_minimizer_generation_projected_row>);

/// Trivial output from one PostgreSQL boundary. Each projection borrows the
/// independently pinned BufferHeapTuple slot at the same ordinal. Receipts,
/// CRC work and arbitrary sinks are deliberately constructed only after the
/// boundary has been left.
struct pinned_minimizer_generation_projected_row_batch {
    std::array<pinned_minimizer_generation_projected_row,
               k_minimizer_generation_full_scan_pinned_batch_rows>
        rows{};
    std::size_t size = 0;
    bool eof = false;
};

static_assert(std::is_trivially_copyable_v<pinned_minimizer_generation_projected_row_batch>);
static_assert(std::is_trivially_destructible_v<pinned_minimizer_generation_projected_row_batch>);

/// Keep systable_getnext_ordered's concurrent-abort contract while using
/// caller-owned slots. PostgreSQL keeps this helper private to genam.c, so the
/// extension mirrors its PG17 implementation inside the same protected leaf.
inline void handle_minimizer_generation_concurrent_abort_pg_leaf() {
    if (TransactionIdIsValid(CheckXidAlive) && !TransactionIdIsInProgress(CheckXidAlive) &&
        !TransactionIdDidCommit(CheckXidAlive)) {
        ereport(ERROR,
                (errcode(ERRCODE_TRANSACTION_ROLLBACK),
                 errmsg("transaction aborted during system catalog scan")));
    }
}

/// Fetch, deform and project up to K rows under one PostgreSQL error boundary.
/// Every slot owns its buffer pin until the caller has consumed the complete
/// batch. A semantic projection error stops the fill at that row so its error
/// identity remains the first observable failure, matching single-row reads.
[[nodiscard]] inline pinned_minimizer_generation_projected_row_batch
next_pinned_minimizer_generation_projected_row_batch_or_error(
    Relation relation,
    SysScanDesc scan,
    const std::array<TupleTableSlot*, k_minimizer_generation_full_scan_pinned_batch_rows>& slots) {
    pinned_minimizer_generation_projected_row_batch batch{};
    bioseq::postgres::support::invoke_pg_as_cpp([relation, scan, &slots, &batch] {
        Assert(scan != nullptr);
        Assert(scan->irel != nullptr);

        // All receipts and callbacks from the preceding batch are gone
        // before this function is entered, so its buffer pins may now be
        // released in bulk before the slots are reused.
        for (TupleTableSlot* slot : slots) {
            Assert(slot != nullptr);
            ExecClearTuple(slot);
        }

        for (std::size_t ordinal = 0; ordinal < slots.size(); ++ordinal) {
            TupleTableSlot* slot = slots[ordinal];
            HeapTuple tuple = nullptr;
#if PG_VERSION_NUM >= 200000
            if (table_index_getnext_slot(scan->iscan, ForwardScanDirection, slot))
#else
            if (index_getnext_slot(scan->iscan, ForwardScanDirection, slot))
#endif
            {
                tuple = ExecFetchSlotHeapTuple(slot, false, nullptr);
            }
            if (HeapTupleIsValid(tuple) && scan->iscan->xs_recheck) {
                elog(ERROR, "system catalog scans with lossy index conditions are not implemented");
            }
            handle_minimizer_generation_concurrent_abort_pg_leaf();
            if (!HeapTupleIsValid(tuple)) {
                batch.eof = true;
                return;
            }

            pinned_minimizer_generation_projected_row& output = batch.rows[batch.size];
            output.tid = tuple->t_self;
            output.projected = project_minimizer_generation_deformed_row(
                deform_minimizer_generation_row_pg_leaf(relation, tuple));
            ++batch.size;
            if (output.projected.error != nullptr)
                return;
        }
    });
    return batch;
}

/// Validate the address-bound row checksum only after PostgreSQL deformation
/// has completed.  Callers outside a PG boundary may safely own the receipt
/// and invoke arbitrary C++ sinks.
[[nodiscard]] inline minimizer_generation_loaded_row
load_minimizer_generation_projected_row_or_error(
    const minimizer_generation_projected_row& projected) {
    if (projected.error != nullptr) {
        minimizer_generation_storage_error_as_cpp(projected.error);
    }
    auto validated_receipt =
        bioseq::indexing::validate_minimizer_generation_row_receipt(minimizer_generation_row_view{
            .address = projected.address,
            .bytes = projected.bytes,
            .row_checksum = projected.checksum,
        });
    if (!validated_receipt) {
        minimizer_generation_storage_error_as_cpp("row checksum disagrees");
    }
    return minimizer_generation_loaded_row{
        .query_key = projected.query_key,
        .address = projected.address,
        .chunk_bytes = projected.chunk_bytes,
        .checksum = projected.checksum,
        .bytes = projected.bytes,
        .validated_receipt = std::move(*validated_receipt),
    };
}

using validated_minimizer_generation_row_receipt_batch =
    std::array<std::optional<validated_minimizer_generation_row_receipt>,
               k_minimizer_generation_full_scan_pinned_batch_rows>;

/// Minimum batch size eligible for storage-neutral parallel authentication.
inline constexpr std::size_t k_minimizer_generation_parallel_row_authentication_min_rows = 32u;
/// Row grain used by parallel checksum authentication.
inline constexpr std::size_t k_minimizer_generation_parallel_row_authentication_grain_rows = 8u;

/// Destroy every row authority before its pinned slot can be advanced.
inline void clear_validated_minimizer_generation_row_receipt_batch(
    validated_minimizer_generation_row_receipt_batch& receipts) noexcept {
    for (auto& receipt : receipts) {
        receipt.reset();
    }
}

/// Authenticate one already-projected pinned batch. Only immutable payload
/// spans and storage-neutral CRC code enter TBB. No PostgreSQL API or callback
/// is invoked off the backend thread. The call joins before returning, so all
/// borrowed spans remain inside the lifetime of their pinned tuple slots.
inline void authenticate_minimizer_generation_projected_row_batch(
    const pinned_minimizer_generation_projected_row_batch& batch,
    validated_minimizer_generation_row_receipt_batch& receipts) {
    clear_validated_minimizer_generation_row_receipt_batch(receipts);
    const auto authenticate_range = [&batch, &receipts](std::size_t begin,
                                                        std::size_t end) noexcept {
        for (std::size_t ordinal = begin; ordinal < end; ++ordinal) {
            const minimizer_generation_projected_row& projected = batch.rows[ordinal].projected;
            if (projected.error != nullptr)
                continue;
            auto validated = bioseq::indexing::validate_minimizer_generation_row_receipt(
                minimizer_generation_row_view{
                    .address = projected.address,
                    .bytes = projected.bytes,
                    .row_checksum = projected.checksum,
                });
            if (validated) {
                receipts[ordinal].emplace(std::move(*validated));
            }
        }
    };

    auto& pool = bioseq::parallelism::global_pool::instance();
    const bool use_parallel_authentication =
        batch.size >= k_minimizer_generation_parallel_row_authentication_min_rows &&
        pool.is_active() && bioseq::parallelism::has_parallel_tasks(pool.get_mode()) &&
        pool.maximum_concurrency() > 1u;
    if (use_parallel_authentication) {
        pool.parallel_for(0u,
                          batch.size,
                          k_minimizer_generation_parallel_row_authentication_grain_rows,
                          bioseq::parallelism::range_body{authenticate_range});
    } else {
        authenticate_range(0u, batch.size);
    }
}

/// Transfer one pre-authenticated row authority into the callback-scoped
/// loaded-row form without repeating its address-bound CRC pass.
[[nodiscard]] inline minimizer_generation_loaded_row
load_authenticated_minimizer_generation_projected_row_or_error(
    const minimizer_generation_projected_row& projected,
    std::optional<validated_minimizer_generation_row_receipt>& validated_receipt) {
    if (projected.error != nullptr) {
        minimizer_generation_storage_error_as_cpp(projected.error);
    }
    if (!validated_receipt) {
        minimizer_generation_storage_error_as_cpp("row checksum disagrees");
    }
    bool exact_authority = false;
    const bool visited = bioseq::indexing::visit_validated_minimizer_generation_row(
        *validated_receipt,
        [&projected, &exact_authority](const index_chunk_address& address,
                                       std::span<const std::byte> bytes,
                                       std::uint64_t row_checksum) noexcept {
            exact_authority =
                address == projected.address && bytes.data() == projected.bytes.data() &&
                bytes.size() == projected.bytes.size() && row_checksum == projected.checksum;
        });
    if (!visited || !exact_authority) {
        minimizer_generation_storage_error_as_cpp("row checksum authority disagrees");
    }
    return minimizer_generation_loaded_row{
        .query_key = projected.query_key,
        .address = projected.address,
        .chunk_bytes = projected.chunk_bytes,
        .checksum = projected.checksum,
        .bytes = projected.bytes,
        .validated_receipt = std::move(*validated_receipt),
    };
}

/// Decode all eight non-NULL fields and validate PLAIN storage, byte count and
/// the address-bound row checksum before any caller observes the payload.
[[nodiscard]] inline minimizer_generation_loaded_row
load_minimizer_generation_deformed_row_or_error(const minimizer_generation_deformed_row& deformed) {
    return load_minimizer_generation_projected_row_or_error(
        project_minimizer_generation_deformed_row(deformed));
}

/// General single-row loader.  Its hot-path PostgreSQL work is one deformation
/// bridge regardless of the eight-column schema width.
[[nodiscard]] inline minimizer_generation_loaded_row
load_minimizer_generation_row_or_error(Relation relation, HeapTuple tuple) {
    const minimizer_generation_deformed_row deformed =
        deform_minimizer_generation_row_or_error(relation, tuple);
    return load_minimizer_generation_deformed_row_or_error(deformed);
}

} // namespace bioseq::storage::indexing
