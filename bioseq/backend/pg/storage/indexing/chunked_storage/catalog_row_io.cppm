/**
 * @file catalog_row_io.cppm
 * @brief Build index-generation tuples and read their payloads with checksum checks.
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
import bioseq.indexing.minimizer_fragment_image; // minimizer fragment byte layout
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;

import :catalog_contracts;
import :catalog_metadata;
import :catalog_source_authorization;
import :catalog_storage_descriptors;
export namespace bioseq::storage::indexing {
/// Copy and register a snapshot with the current command ID so a writer can
/// reread its new rows. Keep the source reader's snapshot  unchanged The caller
/// must unregister the returned copy.
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

/// Form a heap tuple from a validated generation row. The caller owns the
/// tuple and inserts it through a heap-tuple slot, usually with a batch of
/// other rows. This helper only builds the tuple.
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

/// Sizes checked to ensure an inline index payload and tuple fit on one PostgreSQL page.
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

/// Borrowed row payload wqth a checked address and checksum, valid during its callback.
struct minimizer_generation_loaded_row {
    std::uint64_t query_key = 0;
    index_chunk_address address{};
    std::uint32_t chunk_bytes = 0;
    std::uint64_t checksum = 0;
    // The payload is inline and uncompressed. These bytes borrow the scan slot
    // or batch storage, so this row does not own memory that needs pfree.
    std::span<const std::byte> bytes;
    std::optional<validated_minimizer_generation_row_receipt> validated_receipt;

    void release() noexcept {
        // Discard the checksum receipt before its borrowed tuple can be reused.
        validated_receipt.reset();
        bytes = {};
    }
};

/// The  eight fields read from a heap tuple. PostgreSQL error handling may
/// skip C++ destructors, so this value owns no resources.
struct minimizer_generation_deformed_row {
    HeapTuple tuple = nullptr;
    std::array<Datum, k_minimizer_generation_index_attribute_descriptors.size()> values{};
    std::array<bool, k_minimizer_generation_index_attribute_descriptors.size()> nulls{};
};

static_assert(std::is_trivially_copyable_v<minimizer_generation_deformed_row>);
static_assert(std::is_trivially_destructible_v<minimizer_generation_deformed_row>);

/// A fetched tuple's fields and a flag indicating whether the, scan found a row.
struct minimizer_generation_next_deformed_row {
    minimizer_generation_deformed_row row{};
    bool found = false;
};

static_assert(std::is_trivially_copyable_v<minimizer_generation_next_deformed_row>);
static_assert(std::is_trivially_destructible_v<minimizer_generation_next_deformed_row>);

/// Read all tuple fields together. Call inside invoke_pg_as_cpp(), or use
/// deform_minimizer_generation_row_or_error() to convert PostgreSQL errors
/// into C++ exceptions.
[[nodiscard]] inline minimizer_generation_deformed_row
deform_minimizer_generation_row_pg_leaf(Relation relation, HeapTuple tuple) {
    minimizer_generation_deformed_row deformed{.tuple = tuple};
    heap_deform_tuple(
        tuple, RelationGetDescr(relation), deformed.values.data(), deformed.nulls.data());
    return deformed;
}

/// Read a tuple's fields and translate PostgreSQL failures into C++ exceptions.
[[nodiscard]] inline minimizer_generation_deformed_row
deform_minimizer_generation_row_or_error(Relation relation, HeapTuple tuple) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [relation, tuple] { return deform_minimizer_generation_row_pg_leaf(relation, tuple); });
}

/// Fetch the next ordered-scan tuple and read its fields in one
/// invoke_pg_as_cpp() call. The caller checks checksums and runs callbacks
/// after it returns.
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

/// Borrowed row fields and an optional error message This value owns no
/// resources, so PostgreSQL error handling can safely skip  its destructor.
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

/// Check required fields and the inline payload length, then borrow its bytes.
/// Return an error message for malformed rows. Check the checksum separately.
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

/// Fetch several rows per invoke_pg_as_cpp() call. Bound the number of pinned
/// rows while their payloads stay in their buffer slots.
inline constexpr std::size_t k_minimizer_generation_full_scan_pinned_batch_rows = 128;

/// Borrowed row fields and the physical location of their pinned tuple.
struct pinned_minimizer_generation_projected_row {
    minimizer_generation_projected_row projected{};
    ItemPointerData tid{};
};

static_assert(std::is_trivially_copyable_v<pinned_minimizer_generation_projected_row>);
static_assert(std::is_trivially_destructible_v<pinned_minimizer_generation_projected_row>);

/// Rows fetched togeter. Each payload borrows its matching pinned slot.
/// Check checksums and run callbacks after invoke_pg_as_cpp() returns,
/// while those slots remain pinned.
struct pinned_minimizer_generation_projected_row_batch {
    std::array<pinned_minimizer_generation_projected_row,
               k_minimizer_generation_full_scan_pinned_batch_rows>
        rows{};
    std::size_t size = 0;
    bool eof = false;
};

static_assert(std::is_trivially_copyable_v<pinned_minimizer_generation_projected_row_batch>);
static_assert(std::is_trivially_destructible_v<pinned_minimizer_generation_projected_row_batch>);

/// Check for a concurrent transaction abort  when reading into caller-owned slots.
/// This repeats the check PostgreSQL normally performs in systable_getnext_ordered().
inline void handle_minimizer_generation_concurrent_abort_pg_leaf() {
    if (TransactionIdIsValid(CheckXidAlive) && !TransactionIdIsInProgress(CheckXidAlive) &&
        !TransactionIdDidCommit(CheckXidAlive)) {
        ereport(ERROR,
                (errcode(ERRCODE_TRANSACTION_ROLLBACK),
                 errmsg("transaction aborted during system catalog scan")));
    }
}

/// Fetch and decode a batch while keeping each tuple pinned in its slot.
/// Stop at the first malformed row so the caller reports errors in scan order.
/// Consume the batch before calling again, because the next call clears the slots.
[[nodiscard]] inline pinned_minimizer_generation_projected_row_batch
next_pinned_minimizer_generation_projected_row_batch_or_error(
    Relation relation,
    SysScanDesc scan,
    const std::array<TupleTableSlot*, k_minimizer_generation_full_scan_pinned_batch_rows>& slots) {
    pinned_minimizer_generation_projected_row_batch batch{};
    bioseq::postgres::support::invoke_pg_as_cpp([relation, scan, &slots, &batch] {
        Assert(scan != nullptr);
        Assert(scan->irel != nullptr);

        // The caller has finished the previous batch. Release its pins before
        // filling these slots with new rows.
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

/// Check a projected row's checksum and return a borrowed payload with, its receipt.
/// Call this  after invoke_pg_as_cpp() returns.
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

/// Smallest batch for which parallel checksum checks are considered
inline constexpr std::size_t k_minimizer_generation_parallel_row_authentication_min_rows = 32u;
/// Row count used to split parallel checksum work into smaller ranges.
inline constexpr std::size_t k_minimizer_generation_parallel_row_authentication_grain_rows = 8u;

/// Discard all checksum receipts before theio pinned tuple slots are reused.
inline void clear_validated_minimizer_generation_row_receipt_batch(
    validated_minimizer_generation_row_receipt_batch& receipts) noexcept {
    for (auto& receipt : receipts) {
        receipt.reset();
    }
}

/// Check the batch's row checksums, using worker threads when enough rows
/// and workers are available. Workers read pinned bytes and leave failed rows
/// without a receipt. All workers finish before this function returns.
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

/// Move a checked receipt into a loaded row after  confirming its address,
/// byte span, and checksum match. Reuse the earlier checksum calculation.
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

/// Check the tuple fields, inline payload length, and address-based checksum
/// before returning a borrowed payload to the caller.
[[nodiscard]] inline minimizer_generation_loaded_row
load_minimizer_generation_deformed_row_or_error(const minimizer_generation_deformed_row& deformed) {
    return load_minimizer_generation_projected_row_or_error(
        project_minimizer_generation_deformed_row(deformed));
}

/// Read and validate one tuple using a single PostgreSQL call for its fields.
[[nodiscard]] inline minimizer_generation_loaded_row
load_minimizer_generation_row_or_error(Relation relation, HeapTuple tuple) {
    const minimizer_generation_deformed_row deformed =
        deform_minimizer_generation_row_or_error(relation, tuple);
    return load_minimizer_generation_deformed_row_or_error(deformed);
}

} // namespace bioseq::storage::indexing
