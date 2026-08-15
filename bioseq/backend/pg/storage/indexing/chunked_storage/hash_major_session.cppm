/**
 * @file hash_major_session.cppm
 * @brief Snapshot-bound hash-major read session, parallel scan probe, and
 *        generation administration.
 *
 * Index chunks are raw BYTEA tuples in heap relations (heap_form_tuple /
 * CatalogTupleInsert + systable_beginscan), bypassing columnarisation
 * (Invariant C). The per-sequence layouts co-locate with the sequence's
 * `(bucket, shard)`; the table layout shards via `index_table_shard`.
 */
module;
#include <libassert/assert.hpp>
#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "common/relpath.h"
#include "commands/sequence.h"
#include "miscadmin.h"
#include "catalog/namespace.h"   // RelnameGetRelid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/tablespace.h"
#include "executor/instrument.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "port/atomics.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/relfilelocator.h"
#include "storage/shm_toc.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/xid8.h"
#include "port.h"
}

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <expected>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
export module bioseq.postgres.storage.indexing.chunked_storage:hash_major_session;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support;                       // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager;                 // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;             // sequence_locator
import bioseq.storage.size;            // sequence_size_t
import bioseq.indexing.type;           // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;        // the concepts
import bioseq.indexing.minimizer_hashmap;   // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image;       // explicit fixed-endian minimizer fragment image image ABI
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io;                // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.postgres.storage.scanner;      // PostgreSQL-backed lazy sequence access
import bioseq.storage.lazy_sequence;            // lazy_sequence
import bioseq.postgres.types.compact_sequence;  // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;               // compact_sequence_range
import bioseq.sequences.simd;          // canonical bounded base decoding
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
import :hash_major_writer;
export namespace bioseq::storage::indexing {

/// One hash-major read session owns the exact current-generation heap/primary-index
/// descriptors and borrows one already-registered executor MVCC snapshot.
/// Metadata discovery, source authorization, and every logical stream callback
/// therefore observe one relation incarnation and one snapshot.
struct hash_major_minimizer_catalog_stream_receipt {
    hash_major_minimizer_stream_kind kind =
        hash_major_minimizer_stream_kind::directory;
    std::uint64_t exact_catalog_rows = 0;
    std::uint64_t exact_catalog_bytes = 0;
    std::uint64_t exact_catalog_stream_crc64 = 0;
};

/// Test-only authorization returned by the hash-major parallel heap-scan experiment.
/// The production reader does not consult this object or switch to this path.
struct hash_major_minimizer_parallel_scan_stats {
    std::uint32_t requested_workers = 0;
    std::uint32_t launched_workers = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t query_key = 0;
    std::uint64_t expected_rows = 0;
    std::uint64_t scanned_rows = 0;
    std::uint64_t expected_bytes = 0;
    std::uint64_t scanned_bytes = 0;
    std::uint64_t duplicate_rows = 0;
    std::uint64_t missing_rows = 0;
    std::uint64_t corrupt_rows = 0;
    std::uint64_t elapsed_ns = 0;
    std::uint64_t checksum_digest = 0;
    std::uint64_t receipt_digest = 0;
    std::uint64_t image_bytes = 0;
    std::uint64_t image_build_ns = 0;
    std::uint64_t mmap_elapsed_ns = 0;
    std::uint64_t mmap_scanned_rows = 0;
    std::uint64_t mmap_scanned_bytes = 0;
    std::uint64_t mmap_checksum_digest = 0;
    std::uint64_t mmap_receipt_digest = 0;
    bool has_leader_participation = false;
    bool mmap_advice_applied = false;
};

namespace hash_major_minimizer_parallel_probe_detail {

inline constexpr std::uint64_t k_toc_magic =
    UINT64_C(0x4236534350524f42); // "B6SCPROB"
inline constexpr std::uint64_t k_header_toc_key =
    UINT64_C(0x4236534300000001);
inline constexpr std::uint64_t k_parallel_scan_toc_key =
    UINT64_C(0x4236534300000002);
inline constexpr std::uint64_t k_seen_bitset_toc_key =
    UINT64_C(0x4236534300000003);
inline constexpr std::uint32_t k_parallel_header_format_revision = 1u;

/// Pointer-free expected shape for one stream in shared scan state.
struct parallel_scan_stream {
    std::int64_t sub_key = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint64_t global_row_base = 0;
};

/// Pointer-free DSM state. Workers authenticate rows and publish only
/// commutative counters plus one exact expected-row bit per accepted tuple.
struct parallel_scan_header {
    std::uint64_t magic = 0;
    std::uint32_t format_revision = 0;
    std::uint32_t requested_workers = 0;
    Oid relation_oid = InvalidOid;
    RelFileLocator relation_locator{};
    std::uint64_t generation_id = 0;
    std::uint64_t query_key = 0;
    std::uint64_t expected_rows = 0;
    std::uint64_t expected_bytes = 0;
    std::uint64_t bitset_word_count = 0;
    std::array<parallel_scan_stream, 3> streams{};
    pg_atomic_uint64 scanned_rows{};
    pg_atomic_uint64 scanned_bytes{};
    pg_atomic_uint64 duplicate_rows{};
    pg_atomic_uint64 corrupt_rows{};
    pg_atomic_uint64 checksum_digest{};
    pg_atomic_uint64 receipt_digest{};
};

static_assert(std::is_trivially_destructible_v<parallel_scan_header>);
static_assert(std::is_standard_layout_v<parallel_scan_header>);

inline constexpr std::uint64_t k_image_magic =
    UINT64_C(0x42364636494d4147); // "B6F6IMAG"
inline constexpr std::uint32_t k_image_format_revision = 1u;
inline constexpr std::uint64_t k_image_section_alignment = 4096u;

/// Offset and extent metadata for one stream in the temporary image.
struct parallel_image_stream {
    std::int64_t sub_key = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint64_t global_row_base = 0;
    std::uint64_t payload_offset = 0;
};

/// Offset-only test image written to a PostgreSQL-owned temporary file.
struct parallel_image_header {
    std::uint64_t magic = 0;
    std::uint32_t format_revision = 0;
    std::uint32_t header_bytes = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t query_key = 0;
    std::uint64_t expected_rows = 0;
    std::uint64_t expected_bytes = 0;
    std::uint64_t receipts_offset = 0;
    std::uint64_t payload_offset = 0;
    std::uint64_t image_bytes = 0;
    std::array<parallel_image_stream, 3> streams{};
    std::uint64_t header_crc64 = 0;
};

static_assert(std::is_trivially_copyable_v<parallel_image_header>);
static_assert(std::is_standard_layout_v<parallel_image_header>);

/// Temporary-file destination used while materializing the probe image.
struct parallel_image_write_target {
    File file = -1;
    const parallel_image_header* header = nullptr;
};

/// Validation counters returned after reading the temporary image.
struct parallel_image_validation {
    std::uint64_t elapsed_ns = 0;
    std::uint64_t rows = 0;
    std::uint64_t bytes = 0;
    std::uint64_t checksum_digest = 0;
    std::uint64_t receipt_digest = 0;
    bool advice_applied = false;
};

/// Mix one receipt value into the probe's commutative digest.
[[nodiscard]] inline std::uint64_t mix64(
        std::uint64_t value) noexcept {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30u))
        * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27u))
        * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31u);
}

/// Round an image offset up to the fixed section alignment.
[[nodiscard]] inline std::uint64_t align_image_offset(
        std::uint64_t value) {
    if (value > (std::numeric_limits<std::uint64_t>::max)()
            - (k_image_section_alignment - 1u)) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major mmap image offset overflows");
    }
    return (value + k_image_section_alignment - 1u)
        / k_image_section_alignment * k_image_section_alignment;
}

/// Compute the header checksum with its checksum field cleared.
[[nodiscard]] inline std::uint64_t image_header_crc(
        const parallel_image_header& header) noexcept {
    parallel_image_header copy = header;
    copy.header_crc64 = 0;
    crc64_ecma182_accumulator crc{};
    crc.update(std::as_bytes(
        std::span{&copy, std::size_t{1}}));
    return crc.checksum();
}

[[noreturn]] inline void corrupt_row_pg_leaf(
        parallel_scan_header* header,
        const char* detail) {
    if (header != nullptr) {
        (void)pg_atomic_fetch_add_u64(
            &header->corrupt_rows, 1);
    }
    ereport(ERROR,
        (errcode(ERRCODE_DATA_CORRUPTED),
         errmsg("bioseq hash-major parallel scan probe rejected a row"),
         errdetail_internal("%s", detail)));
    pg_unreachable();
}

[[noreturn]] inline void duplicate_row_pg_leaf(
        parallel_scan_header* header,
        std::uint64_t global_row) {
    (void)pg_atomic_fetch_add_u64(
        &header->duplicate_rows, 1);
    ereport(ERROR,
        (errcode(ERRCODE_DATA_CORRUPTED),
         errmsg("bioseq hash-major parallel scan probe found a duplicate row"),
         errdetail_internal(
            "global_row=%llu",
            static_cast<unsigned long long>(global_row))));
    pg_unreachable();
}

/// Resolve a persisted sub-key to its shared stream slot.
[[nodiscard]] inline int stream_index_for_sub_key(
        const parallel_scan_header& header,
        std::int64_t sub_key) noexcept {
    for (std::size_t index = 0; index < header.streams.size();
         ++index) {
        if (header.streams[index].sub_key == sub_key) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

/// Authenticate every tuple claimed from one serial or parallel table scan.
inline void scan_table_desc_pg_leaf(
        Relation relation,
        TableScanDesc scan,
        parallel_scan_header* header,
        pg_atomic_uint64* seen_words) {
    if (relation == nullptr || scan == nullptr || header == nullptr
        || seen_words == nullptr
        || header->magic != k_toc_magic
        || header->format_revision != k_parallel_header_format_revision
        || RelationGetRelid(relation) != header->relation_oid
        || !RelFileLocatorEquals(
            relation->rd_locator, header->relation_locator)
        || RelationGetDescr(relation) == nullptr
        || RelationGetDescr(relation)->natts
            != static_cast<int>(
                k_minimizer_generation_index_attribute_descriptors.size())) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq hash-major parallel scan probe binding changed")));
    }

    TupleTableSlot* slot = table_slot_create(relation, nullptr);
    std::uint64_t local_rows = 0;
    std::uint64_t local_bytes = 0;
    std::uint64_t local_checksum_digest = 0;
    std::uint64_t local_receipt_digest = 0;
    std::uint64_t visited_rows = 0;

    while (table_scan_getnextslot(
            scan, ForwardScanDirection, slot)) {
        ++visited_rows;
        slot_getallattrs(slot);
        Datum* values = slot->tts_values;
        bool* nulls = slot->tts_isnull;

        // Filter on the four fixed identity columns before inspecting BYTEA.
        // Non-active generations and metadata rows are deliberately outside this
        // probe's corruption authority.
        if (!nulls[k_minimizer_generation_attribute_generation_id - 1]
            && !nulls[k_minimizer_generation_attribute_query_key - 1]
            && !nulls[k_minimizer_generation_attribute_position_segment - 1]
            && !nulls[k_minimizer_generation_attribute_sub_key - 1]
            && DatumGetInt64(values[k_minimizer_generation_attribute_generation_id - 1])
                == static_cast<std::int64_t>(
                    header->generation_id)
            && DatumGetInt64(values[k_minimizer_generation_attribute_query_key - 1])
                == static_cast<std::int64_t>(header->query_key)
            && DatumGetInt32(
                    values[k_minimizer_generation_attribute_position_segment - 1]) == 0) {
            const std::int64_t sub_key =
                DatumGetInt64(values[k_minimizer_generation_attribute_sub_key - 1]);
            const int stream_index =
                stream_index_for_sub_key(*header, sub_key);
            if (stream_index >= 0) {
                minimizer_generation_deformed_row deformed{};
                for (std::size_t index = 0;
                     index < deformed.values.size(); ++index) {
                    deformed.values[index] = values[index];
                    deformed.nulls[index] = nulls[index];
                }
                const minimizer_generation_projected_row projected =
                    project_minimizer_generation_deformed_row(deformed);
                if (projected.error != nullptr) {
                    corrupt_row_pg_leaf(header, projected.error);
                }
                const parallel_scan_stream& stream =
                    header->streams[
                        static_cast<std::size_t>(stream_index)];
                const std::uint64_t row_ordinal =
                    projected.address.chunk_no;
                if (projected.query_key != header->query_key
                    || projected.address.generation_id
                        != header->generation_id
                    || projected.address.position_segment != 0
                    || projected.address.sub_key != stream.sub_key
                    || row_ordinal >= stream.row_count
                    || row_ordinal
                        > (std::numeric_limits<std::uint64_t>::max)()
                            / k_hash_major_minimizer_row_payload_bytes) {
                    corrupt_row_pg_leaf(
                        header, "row identity or ordinal disagrees");
                }
                const std::uint64_t row_offset =
                    row_ordinal * k_hash_major_minimizer_row_payload_bytes;
                if (row_offset >= stream.logical_bytes) {
                    corrupt_row_pg_leaf(
                        header, "row offset exceeds logical stream");
                }
                const std::uint64_t remaining =
                    stream.logical_bytes - row_offset;
                const std::size_t expected_bytes =
                    static_cast<std::size_t>(
                        std::min<std::uint64_t>(
                            remaining,
                            k_hash_major_minimizer_row_payload_bytes));
                if (projected.bytes.size() != expected_bytes
                    || projected.chunk_bytes != expected_bytes) {
                    corrupt_row_pg_leaf(
                        header, "PLAIN row length disagrees");
                }
                const std::uint64_t expected_checksum =
                    bioseq::indexing::crc64_index_chunk_row(
                        projected.address.generation_id,
                        projected.address.position_segment,
                        projected.address.sub_key,
                        projected.address.chunk_no,
                        projected.chunk_bytes,
                        projected.bytes);
                if (projected.checksum != expected_checksum) {
                    corrupt_row_pg_leaf(
                        header,
                        "address-bound row CRC disagrees");
                }

                const std::uint64_t global_row =
                    stream.global_row_base + row_ordinal;
                if (global_row >= header->expected_rows) {
                    corrupt_row_pg_leaf(
                        header, "global row ordinal overflows");
                }
                const std::uint64_t word_ordinal = global_row / 64u;
                const std::uint64_t bit =
                    std::uint64_t{1} << (global_row % 64u);
                if (word_ordinal >= header->bitset_word_count) {
                    corrupt_row_pg_leaf(
                        header, "seen-row bitset is undersized");
                }
                const std::uint64_t previous =
                    pg_atomic_fetch_or_u64(
                        &seen_words[word_ordinal], bit);
                if ((previous & bit) != 0u) {
                    duplicate_row_pg_leaf(header, global_row);
                }

                ++local_rows;
                local_bytes += projected.bytes.size();
                local_checksum_digest +=
                    mix64(projected.checksum);
                local_receipt_digest += mix64(
                    projected.checksum
                    ^ mix64(projected.address.generation_id)
                    ^ mix64(projected.query_key)
                    ^ mix64(static_cast<std::uint64_t>(
                        projected.address.sub_key))
                    ^ mix64(row_ordinal)
                    ^ mix64(projected.chunk_bytes));
            }
        }

        ExecClearTuple(slot);
        if ((visited_rows & 1023u) == 0u) {
            if (!IsParallelWorker() && ParallelMessagePending) {
                HandleParallelMessages();
            }
            CHECK_FOR_INTERRUPTS();
        }
    }

    ExecDropSingleTupleTableSlot(slot);
    (void)pg_atomic_fetch_add_u64(
        &header->scanned_rows,
        static_cast<std::int64_t>(local_rows));
    (void)pg_atomic_fetch_add_u64(
        &header->scanned_bytes,
        static_cast<std::int64_t>(local_bytes));
    (void)pg_atomic_fetch_add_u64(
        &header->checksum_digest,
        std::bit_cast<std::int64_t>(local_checksum_digest));
    (void)pg_atomic_fetch_add_u64(
        &header->receipt_digest,
        std::bit_cast<std::int64_t>(local_receipt_digest));
}

/// Run the probe through a backend-local table scan.
inline void serial_scan_pg_leaf(
        Relation relation,
        Snapshot snapshot,
        parallel_scan_header* header,
        pg_atomic_uint64* seen_words) {
    TableScanDesc scan =
        table_beginscan(relation, snapshot, 0, nullptr);
    scan_table_desc_pg_leaf(
        relation, scan, header, seen_words);
    table_endscan(scan);
}

/// Join the shared block allocator as a leader or worker.
inline void parallel_scan_pg_leaf(
        Relation relation,
        ParallelTableScanDesc parallel_scan,
        parallel_scan_header* header,
        pg_atomic_uint64* seen_words) {
    TableScanDesc scan =
        table_beginscan_parallel(relation, parallel_scan);
    scan_table_desc_pg_leaf(
        relation, scan, header, seen_words);
    table_endscan(scan);
}

/// Bind shared scan state to the relation and decoded manifest.
inline void initialize_header(
        parallel_scan_header* header,
        Relation relation,
        const hash_major_minimizer_manifest& manifest,
        std::uint64_t query_key,
        std::uint32_t requested_workers) {
    if (header == nullptr || relation == nullptr
        || manifest.generation_id == 0u
        || !bioseq::indexing::sql_bigint_can_represent(
            manifest.generation_id)
        || !bioseq::indexing::sql_bigint_can_represent(query_key)) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe header is invalid");
    }
    std::memset(header, 0, sizeof(*header));
    header->magic = k_toc_magic;
    header->format_revision = k_parallel_header_format_revision;
    header->requested_workers = requested_workers;
    header->relation_oid = RelationGetRelid(relation);
    header->relation_locator = relation->rd_locator;
    header->generation_id = manifest.generation_id;
    header->query_key = query_key;

    std::uint64_t row_base = 0;
    std::uint64_t total_bytes = 0;
    for (std::size_t index = 0;
         index < header->streams.size(); ++index) {
        const hash_major_minimizer_stream_kind expected_kind =
            hash_major_minimizer_pg_detail::stream_kind(index);
        const auto& descriptor = manifest.streams[index];
        const std::uint64_t expected_rows =
            hash_major_minimizer_pg_detail::ceil_rows(
                descriptor.logical_bytes);
        if (descriptor.kind != expected_kind
            || descriptor.row_count != expected_rows
            || descriptor.row_count
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int32_t>::max)())
            || descriptor.row_count
                > (std::numeric_limits<std::uint64_t>::max)()
                    - row_base
            || descriptor.logical_bytes
                > (std::numeric_limits<std::uint64_t>::max)()
                    - total_bytes) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major parallel scan probe stream geometry is invalid");
        }
        header->streams[index] = parallel_scan_stream{
            .sub_key =
                hash_major_minimizer_pg_detail::stream_sub_key(expected_kind),
            .logical_bytes = descriptor.logical_bytes,
            .row_count = descriptor.row_count,
            .global_row_base = row_base,
        };
        row_base += descriptor.row_count;
        total_bytes += descriptor.logical_bytes;
    }
    header->expected_rows = row_base;
    header->expected_bytes = total_bytes;
    header->bitset_word_count =
        row_base / 64u + (row_base % 64u != 0u ? 1u : 0u);
    if (header->bitset_word_count == 0u
        || header->bitset_word_count
            > static_cast<std::uint64_t>(
                MaxAllocSize / sizeof(pg_atomic_uint64))) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe bitset geometry is invalid");
    }
    pg_atomic_init_u64(&header->scanned_rows, 0);
    pg_atomic_init_u64(&header->scanned_bytes, 0);
    pg_atomic_init_u64(&header->duplicate_rows, 0);
    pg_atomic_init_u64(&header->corrupt_rows, 0);
    pg_atomic_init_u64(&header->checksum_digest, 0);
    pg_atomic_init_u64(&header->receipt_digest, 0);
}

/// Initialize the exact-row receipt bitset.
inline void initialize_seen_words(
        pg_atomic_uint64* words,
        std::uint64_t word_count) {
    if (words == nullptr || word_count == 0u) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe bitset is invalid");
    }
    for (std::uint64_t index = 0; index < word_count; ++index) {
        pg_atomic_init_u64(&words[index], 0);
    }
}

[[nodiscard]] inline hash_major_minimizer_parallel_scan_stats
finalize_result(
        parallel_scan_header& header,
        pg_atomic_uint64* seen_words,
        std::uint32_t launched_workers,
        std::uint64_t elapsed_ns) {
    std::uint64_t seen_rows = 0;
    for (std::uint64_t index = 0;
         index < header.bitset_word_count; ++index) {
        std::uint64_t word =
            pg_atomic_read_u64(&seen_words[index]);
        if (index + 1u == header.bitset_word_count
            && header.expected_rows % 64u != 0u) {
            word &= (std::uint64_t{1}
                    << (header.expected_rows % 64u))
                - 1u;
        }
        seen_rows += std::popcount(word);
    }
    const std::uint64_t scanned_rows =
        pg_atomic_read_u64(&header.scanned_rows);
    const std::uint64_t scanned_bytes =
        pg_atomic_read_u64(&header.scanned_bytes);
    const std::uint64_t duplicates =
        pg_atomic_read_u64(&header.duplicate_rows);
    const std::uint64_t corrupt =
        pg_atomic_read_u64(&header.corrupt_rows);
    const std::uint64_t missing =
        seen_rows <= header.expected_rows
            ? header.expected_rows - seen_rows
            : header.expected_rows;
    if (duplicates != 0u || corrupt != 0u || missing != 0u
        || seen_rows != header.expected_rows
        || scanned_rows != header.expected_rows
        || scanned_bytes != header.expected_bytes) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "hash-major parallel scan probe receipt equation failed: "
            "expected_rows={} seen_rows={} scanned_rows={} "
            "expected_bytes={} scanned_bytes={} duplicates={} "
            "missing={} corrupt={}",
            header.expected_rows, seen_rows, scanned_rows,
            header.expected_bytes, scanned_bytes, duplicates,
            missing, corrupt));
    }
    return hash_major_minimizer_parallel_scan_stats{
        .requested_workers = header.requested_workers,
        .launched_workers = launched_workers,
        .generation_id = header.generation_id,
        .query_key = header.query_key,
        .expected_rows = header.expected_rows,
        .scanned_rows = scanned_rows,
        .expected_bytes = header.expected_bytes,
        .scanned_bytes = scanned_bytes,
        .duplicate_rows = duplicates,
        .missing_rows = missing,
        .corrupt_rows = corrupt,
        .elapsed_ns = elapsed_ns,
        .checksum_digest =
            pg_atomic_read_u64(&header.checksum_digest),
        .receipt_digest =
            pg_atomic_read_u64(&header.receipt_digest),
        .has_leader_participation = true,
    };
}

[[nodiscard]] inline hash_major_minimizer_parallel_scan_stats
run_probe(
        Relation relation,
        Snapshot snapshot,
        const hash_major_minimizer_manifest& manifest,
        std::uint64_t query_key,
        std::uint32_t requested_workers) {
    if (relation == nullptr || snapshot == nullptr
        || !IsMVCCSnapshot(snapshot)
        || snapshot->regd_count == 0) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe requires a registered MVCC snapshot");
    }

    if (requested_workers == 0u) {
        parallel_scan_header header{};
        initialize_header(
            &header, relation, manifest, query_key, 0u);
        const Size bitset_bytes = static_cast<Size>(
            header.bitset_word_count
            * sizeof(pg_atomic_uint64));
        auto* seen_words =
            static_cast<pg_atomic_uint64*>(
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [bitset_bytes] {
                        return palloc0(bitset_bytes);
                    }));
        initialize_seen_words(
            seen_words, header.bitset_word_count);
        instr_time begin{};
        instr_time end{};
        INSTR_TIME_SET_CURRENT(begin);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, snapshot, &header, seen_words] {
                serial_scan_pg_leaf(
                    relation, snapshot, &header, seen_words);
            });
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, begin);
        hash_major_minimizer_parallel_scan_stats result =
            finalize_result(
                header, seen_words, 0u,
                INSTR_TIME_GET_NANOSEC(end));
        bioseq::postgres::support::invoke_pg_as_cpp(
            [seen_words] { pfree(seen_words); });
        return result;
    }

    if (IsParallelWorker() || IsInParallelMode()
        || ParallelContextActive()) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe cannot nest a parallel context");
    }
    const Size parallel_scan_bytes =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, snapshot] {
                return table_parallelscan_estimate(
                    relation, snapshot);
            });
    if (parallel_scan_bytes == 0u) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe received an empty scan descriptor");
    }

    ParallelContext* context = nullptr;
    bool entered_parallel_mode = false;
    instr_time begin{};
    instr_time end{};
    INSTR_TIME_SET_CURRENT(begin);
    try {
        EnterParallelMode();
        entered_parallel_mode = true;
        context = bioseq::postgres::support::invoke_pg_as_cpp(
            [requested_workers] {
                return CreateParallelContext(
                    "bioseq",
                    "bioseq_hash_major_parallel_scan_probe_worker_main",
                    static_cast<int>(requested_workers));
            });
        if (context == nullptr) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major parallel scan probe could not create a parallel context");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [context, parallel_scan_bytes, &manifest] {
                shm_toc_estimate_chunk(
                    &context->estimator, sizeof(parallel_scan_header));
                shm_toc_estimate_chunk(
                    &context->estimator, parallel_scan_bytes);
                // The exact size becomes known only after the manifest-backed
                // header is initialized. Its upper bound is one atomic word
                // per 64 logical rows.
                std::uint64_t expected_rows = 0;
                for (const auto& descriptor : manifest.streams) {
                    if (descriptor.row_count
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - expected_rows) {
                        ereport(ERROR,
                            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                             errmsg("bioseq hash-major parallel scan row count overflows")));
                    }
                    expected_rows += descriptor.row_count;
                }
                const std::uint64_t word_count =
                    expected_rows / 64u
                    + (expected_rows % 64u != 0u ? 1u : 0u);
                if (word_count == 0u
                    || word_count
                        > static_cast<std::uint64_t>(
                            MaxAllocSize
                            / sizeof(pg_atomic_uint64))) {
                    ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bioseq hash-major parallel scan bitset is too large")));
                }
                shm_toc_estimate_chunk(
                    &context->estimator,
                    static_cast<Size>(
                        word_count
                        * sizeof(pg_atomic_uint64)));
                shm_toc_estimate_keys(&context->estimator, 3);
                InitializeParallelDSM(context);
            });
        if (context->toc == nullptr) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major parallel scan probe has no DSM table of contents");
        }

        auto* header = static_cast<parallel_scan_header*>(
            bioseq::postgres::support::invoke_pg_as_cpp(
                [context] {
                    return shm_toc_allocate(
                        context->toc, sizeof(parallel_scan_header));
                }));
        initialize_header(
            header, relation, manifest, query_key,
            requested_workers);
        const Size bitset_bytes = static_cast<Size>(
            header->bitset_word_count
            * sizeof(pg_atomic_uint64));
        auto* parallel_scan =
            static_cast<ParallelTableScanDesc>(
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [context, parallel_scan_bytes] {
                        return shm_toc_allocate(
                            context->toc,
                            parallel_scan_bytes);
                    }));
        auto* seen_words =
            static_cast<pg_atomic_uint64*>(
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [context, bitset_bytes] {
                        return shm_toc_allocate(
                            context->toc, bitset_bytes);
                    }));
        initialize_seen_words(
            seen_words, header->bitset_word_count);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, parallel_scan, snapshot, context, header, seen_words] {
                table_parallelscan_initialize(
                    relation, parallel_scan, snapshot);
                shm_toc_insert(
                    context->toc, k_header_toc_key, header);
                shm_toc_insert(
                    context->toc, k_parallel_scan_toc_key,
                    parallel_scan);
                shm_toc_insert(
                    context->toc, k_seen_bitset_toc_key,
                    seen_words);
                LaunchParallelWorkers(context);
                WaitForParallelWorkersToAttach(context);
            });
        const std::uint32_t launched_workers =
            static_cast<std::uint32_t>(
                std::max(context->nworkers_launched, 0));

        // The leader is an equal consumer of the shared block allocator.
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, parallel_scan, header, seen_words, context] {
                parallel_scan_pg_leaf(
                    relation, parallel_scan,
                    header, seen_words);
                WaitForParallelWorkersToFinish(context);
            });
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, begin);
        hash_major_minimizer_parallel_scan_stats result =
            finalize_result(
                *header, seen_words, launched_workers,
                INSTR_TIME_GET_NANOSEC(end));
        bioseq::postgres::support::invoke_pg_as_cpp(
            [context] { DestroyParallelContext(context); });
        context = nullptr;
        ExitParallelMode();
        entered_parallel_mode = false;
        return result;
    } catch (...) {
        if (context != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [context] { DestroyParallelContext(context); });
            context = nullptr;
        }
        if (entered_parallel_mode) {
            ExitParallelMode();
            entered_parallel_mode = false;
        }
        throw;
    }
}

} // namespace hash_major_minimizer_parallel_probe_detail

/// Snapshot-bound reader for one READY hash-major generation and its three streams.
class index_storage_hash_major_minimizer_read_session {
private:
    enum class operation_state : std::uint8_t {
        opened,
        head_absent,
        metadata_ready,
        source_authorized,
        finished,
        failed,
        moved,
    };

    std::string table_name_;
    std::string index_name_;
    Relation heap_relation_ = nullptr;
    Relation primary_index_relation_ = nullptr;
    SysScanDesc active_scan_ = nullptr;
    std::array<TupleTableSlot*, k_minimizer_generation_full_scan_pinned_batch_rows>
        row_batch_slots_{};
    std::size_t row_batch_slot_count_ = 0u;
    bool cached_boundary_row_valid_ = false;
    hash_major_minimizer_stream_kind cached_boundary_row_kind_ =
        hash_major_minimizer_stream_kind::directory;
    std::uint64_t cached_boundary_row_ordinal_ = 0u;
    std::size_t cached_boundary_slot_ordinal_ =
        k_minimizer_generation_full_scan_pinned_batch_rows;
    minimizer_generation_projected_row cached_boundary_row_{};
    Snapshot snapshot_ = nullptr;
    std::uint64_t query_key_ = 0u;
    Oid expected_tablespace_oid_ = InvalidOid;
    operation_state state_ = operation_state::opened;
    encoded_hash_major_minimizer_head encoded_head_{};
    encoded_hash_major_minimizer_manifest encoded_manifest_{};
    std::optional<hash_major_minimizer_head> head_{};
    std::optional<hash_major_minimizer_manifest> manifest_{};
    std::optional<expected_hash_major_minimizer_generation>
        expected_generation_{};
    std::array<std::byte, k_hash_major_minimizer_maximum_control_block_bytes>
        control_wave_scratch_{};
    minimizer_generation_read_stats stats_{};

    [[noreturn]] void fail_(std::string_view message) {
        state_ = operation_state::failed;
        minimizer_generation_storage_error_as_cpp(message);
    }

    void create_row_batch_slots_checked_() {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] {
                while (row_batch_slot_count_
                       < row_batch_slots_.size()) {
                    TupleTableSlot* slot =
                        table_slot_create(heap_relation_, nullptr);
                    Assert(TTS_IS_BUFFERTUPLE(slot));
                    row_batch_slots_[row_batch_slot_count_] = slot;
                    ++row_batch_slot_count_;
                }
            });
    }

    void invalidate_cached_boundary_row_() noexcept {
        cached_boundary_row_valid_ = false;
        cached_boundary_row_kind_ =
            hash_major_minimizer_stream_kind::directory;
        cached_boundary_row_ordinal_ = 0u;
        cached_boundary_slot_ordinal_ =
            row_batch_slots_.size();
        cached_boundary_row_ = {};
    }

    void clear_row_batch_slots_checked_() {
        if (row_batch_slot_count_ == 0u) return;
        invalidate_cached_boundary_row_();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] {
                for (std::size_t ordinal = 0u;
                     ordinal < row_batch_slot_count_; ++ordinal) {
                    TupleTableSlot* slot = row_batch_slots_[ordinal];
                    Assert(slot != nullptr);
                    ExecClearTuple(slot);
                }
            });
    }

    /// Retain only the final validated row of a selective scan. Consecutive
    /// scheduler waves commonly split inside one 8,064-byte catalog row; its
    /// pinned projection can serve the next wave before any slot is advanced.
    /// All other batch pins are released immediately.
    void clear_row_batch_slots_except_checked_(
            std::size_t retained_ordinal) {
        if (!cached_boundary_row_valid_
            || retained_ordinal >= row_batch_slot_count_) {
            fail_("hash-major boundary-row cache slot is invalid");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this, retained_ordinal] {
                for (std::size_t ordinal = 0u;
                     ordinal < row_batch_slot_count_; ++ordinal) {
                    if (ordinal == retained_ordinal) continue;
                    TupleTableSlot* slot = row_batch_slots_[ordinal];
                    Assert(slot != nullptr);
                    ExecClearTuple(slot);
                }
            });
    }

    void drop_row_batch_slots_checked_() {
        if (row_batch_slot_count_ == 0u) return;
        invalidate_cached_boundary_row_();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] {
                while (row_batch_slot_count_ != 0u) {
                    const std::size_t ordinal =
                        row_batch_slot_count_ - 1u;
                    TupleTableSlot* slot =
                        row_batch_slots_[ordinal];
                    row_batch_slots_[ordinal] = nullptr;
                    row_batch_slot_count_ = ordinal;
                    if (slot != nullptr) {
                        ExecDropSingleTupleTableSlot(slot);
                    }
                }
            });
    }

    void abandon_noexcept_() noexcept {
        invalidate_cached_boundary_row_();
        row_batch_slots_.fill(nullptr);
        row_batch_slot_count_ = 0u;
        active_scan_ = nullptr;
        primary_index_relation_ = nullptr;
        heap_relation_ = nullptr;
        snapshot_ = nullptr;
        expected_generation_.reset();
        manifest_.reset();
        head_.reset();
    }

    void close_relations_() {
        drop_row_batch_slots_checked_();
        if (active_scan_ != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;
        }
        if (primary_index_relation_ != nullptr) {
            Relation finished =
                std::exchange(primary_index_relation_, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished] {
                    index_close(finished, AccessShareLock);
                });
            stats_.cold.add(stats_.cold.lock_releases, 1u);
        }
        if (heap_relation_ != nullptr) {
            Relation finished =
                std::exchange(heap_relation_, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished] {
                    relation_close(finished, AccessShareLock);
                });
            stats_.cold.add(stats_.cold.lock_releases, 1u);
        }
        snapshot_ = nullptr;
    }

    [[nodiscard]] const
    bioseq::indexing::hash_major_minimizer_stream_descriptor&
    descriptor_(hash_major_minimizer_stream_kind kind) const {
        if (!manifest_ || !hash_major_minimizer_pg_detail::valid_stream_kind(kind)) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major stream descriptor requested before metadata");
        }
        const std::size_t index =
            hash_major_minimizer_pg_detail::stream_index(kind);
        const auto& descriptor = manifest_->streams[index];
        if (descriptor.kind != kind) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major stream descriptor ordering changed");
        }
        return descriptor;
    }

    void record_stream_row_(
            hash_major_minimizer_stream_kind kind,
            std::size_t bytes) {
        ++stats_.payload_heap_rows_read;
        stats_.payload_bytes_read += bytes;
        stats_.payload.heap_rows_read++;
        stats_.payload.delivered_chunks++;
        stats_.payload.delivered_bytes += bytes;
        stats_.cold.add(stats_.cold.payload_rows_scanned, 1u);
        stats_.cold.add(stats_.cold.payload_rows_delivered, 1u);
        stats_.cold.add(stats_.cold.payload_bytes_scanned, bytes);
        stats_.cold.add(stats_.cold.payload_bytes_delivered, bytes);
        stats_.cold.add(stats_.cold.row_crc_bytes, bytes);
        if (kind == hash_major_minimizer_stream_kind::directory) {
            ++stats_.directory_chunks_read;
            stats_.directory_bytes_read += bytes;
            ++stats_.hash_major.directory_row_crc_receipts;
        } else if (kind == hash_major_minimizer_stream_kind::controls) {
            ++stats_.controls.physical_heap_rows_read;
            stats_.controls.physical_bytes_read += bytes;
            ++stats_.controls.read.heap_rows_read;
            ++stats_.controls.read.delivered_chunks;
            stats_.controls.read.delivered_bytes += bytes;
            ++stats_.hash_major.control_row_crc_receipts;
        } else {
            ++stats_.positions.physical_heap_rows_read;
            stats_.positions.physical_bytes_read += bytes;
            ++stats_.positions.read.heap_rows_read;
            ++stats_.positions.read.delivered_chunks;
            stats_.positions.read.delivered_bytes += bytes;
            ++stats_.hash_major.posting_row_crc_receipts;
        }
    }

    template <class p_consumer>
    [[nodiscard]] bool scan_stream_rows_(
            hash_major_minimizer_stream_kind kind,
            std::uint64_t first_row,
            std::uint64_t last_row,
            p_consumer&& consume) {
        const auto& descriptor = descriptor_(kind);
        if (state_ != operation_state::source_authorized
            || active_scan_ != nullptr
            || first_row > last_row
            || last_row >= descriptor.row_count
            || last_row
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int32_t>::max)())) {
            return false;
        }
        std::uint64_t expected_row = first_row;
        bool accepted = true;

        // A bounded control/posting wave often starts in the same physical
        // row where the preceding wave ended. Reuse that already authenticated
        // pinned projection before opening another btree scan. If more rows
        // remain, releasing the cache also clears every prior batch slot before
        // the next scan advances them.
        if (cached_boundary_row_valid_) {
            if (cached_boundary_row_kind_ == kind
                && cached_boundary_row_ordinal_ == expected_row) {
                // The projection was address-bound CRC authenticated before
                // being retained, and its heap buffer has remained pinned.
                // Replaying the immutable span needs neither another tuple
                // deformation nor another 8-KiB row-CRC pass.
                accepted = consume(
                    expected_row, cached_boundary_row_.bytes);
                ++expected_row;
                if (!accepted || expected_row > last_row) {
                    return accepted
                        && expected_row == last_row + 1u;
                }
            }
            clear_row_batch_slots_checked_();
        }

        ScanKeyData keys[6];
        ScanKeyInit(&keys[0], k_minimizer_generation_index_generation_id,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                manifest_->generation_id)));
        ScanKeyInit(&keys[1], k_minimizer_generation_index_query_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2], k_minimizer_generation_index_sub_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                hash_major_minimizer_pg_detail::stream_sub_key(kind))));
        ScanKeyInit(&keys[3], k_minimizer_generation_index_position_segment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        ScanKeyInit(&keys[4], k_minimizer_generation_index_chunk_number,
            BTGreaterEqualStrategyNumber, F_INT4GE,
            Int32GetDatum(static_cast<std::int32_t>(expected_row)));
        ScanKeyInit(&keys[5], k_minimizer_generation_index_chunk_number,
            BTLessEqualStrategyNumber, F_INT4LE,
            Int32GetDatum(static_cast<std::int32_t>(last_row)));
        create_row_batch_slots_checked_();
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp(
            [this, &keys] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_,
                snapshot_, 6, keys);
        });
        ++stats_.index_scans;
        ++stats_.range_scan_calls;
        stats_.cold.add(stats_.cold.payload_range_scans, 1u);
        pinned_minimizer_generation_projected_row_batch batch{};
        validated_minimizer_generation_row_receipt_batch validated_receipts{};
        std::size_t batch_ordinal = 0u;
        while (accepted && expected_row <= last_row) {
            if (batch_ordinal == batch.size) {
                if (batch.eof) break;
                clear_validated_minimizer_generation_row_receipt_batch(
                    validated_receipts);
                batch =
                    next_pinned_minimizer_generation_projected_row_batch_or_error(
                        heap_relation_, active_scan_,
                        row_batch_slots_);
                batch_ordinal = 0u;
                if (batch.size == 0u) {
                    if (batch.eof) break;
                    fail_("hash-major batched stream scan made no progress");
                }
                authenticate_minimizer_generation_projected_row_batch(
                    batch, validated_receipts);
            }
            const pinned_minimizer_generation_projected_row& pinned =
                batch.rows[batch_ordinal];
            minimizer_generation_loaded_row row =
                load_authenticated_minimizer_generation_projected_row_or_error(
                    pinned.projected,
                    validated_receipts[batch_ordinal]);
            ++batch_ordinal;
            if (row.query_key != query_key_
                || row.address != index_chunk_address{
                    .generation_id = manifest_->generation_id,
                    .position_segment = 0,
                    .sub_key =
                        hash_major_minimizer_pg_detail::stream_sub_key(kind),
                    .chunk_no =
                        static_cast<std::uint32_t>(expected_row),
                }) {
                row.release();
                fail_("hash-major stream row identity/order disagrees");
            }
            const std::uint64_t row_offset =
                expected_row * k_hash_major_minimizer_row_payload_bytes;
            const std::uint64_t remaining =
                descriptor.logical_bytes - row_offset;
            const std::size_t expected_bytes =
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    remaining, k_hash_major_minimizer_row_payload_bytes));
            if (row.bytes.size() != expected_bytes
                || (expected_row + 1u < descriptor.row_count
                    && row.bytes.size()
                        != k_hash_major_minimizer_row_payload_bytes)) {
                row.release();
                fail_("hash-major stream physical row length disagrees");
            }
            record_stream_row_(kind, row.bytes.size());
            accepted = consume(expected_row, row.bytes);
            const bool retain_boundary_row =
                accepted && expected_row == last_row;
            row.release();
            if (retain_boundary_row) {
                cached_boundary_row_valid_ = true;
                cached_boundary_row_kind_ = kind;
                cached_boundary_row_ordinal_ = expected_row;
                cached_boundary_slot_ordinal_ =
                    batch_ordinal - 1u;
                cached_boundary_row_ = pinned.projected;
            }
            ++expected_row;
        }
        clear_validated_minimizer_generation_row_receipt_batch(
            validated_receipts);
        if (cached_boundary_row_valid_) {
            clear_row_batch_slots_except_checked_(
                cached_boundary_slot_ordinal_);
        } else {
            clear_row_batch_slots_checked_();
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        return accepted && expected_row == last_row + 1u;
    }

    [[nodiscard]] bool read_logical_bytes_(
            hash_major_minimizer_stream_kind kind,
            std::uint64_t offset,
            std::span<std::byte> output) {
        if (state_ != operation_state::source_authorized
            || !hash_major_minimizer_pg_detail::valid_stream_kind(kind)) {
            return false;
        }
        const auto& descriptor = descriptor_(kind);
        if (offset > descriptor.logical_bytes
            || output.size() > descriptor.logical_bytes - offset) {
            return false;
        }
        if (output.empty()) return true;
        const std::uint64_t end = offset + output.size();
        const std::uint64_t first_row =
            offset / k_hash_major_minimizer_row_payload_bytes;
        const std::uint64_t last_row =
            (end - 1u) / k_hash_major_minimizer_row_payload_bytes;
        std::size_t copied = 0u;
        const bool read = scan_stream_rows_(
            kind, first_row, last_row,
            [offset, end, output, &copied](
                    std::uint64_t row_ordinal,
                    std::span<const std::byte> row) {
                const std::uint64_t row_begin =
                    row_ordinal * k_hash_major_minimizer_row_payload_bytes;
                const std::uint64_t row_end = row_begin + row.size();
                const std::uint64_t copy_begin =
                    std::max(offset, row_begin);
                const std::uint64_t copy_end =
                    std::min(end, row_end);
                if (copy_begin >= copy_end) return true;
                const std::size_t bytes =
                    static_cast<std::size_t>(copy_end - copy_begin);
                std::copy_n(
                    row.begin()
                        + static_cast<std::ptrdiff_t>(
                            copy_begin - row_begin),
                    bytes,
                    output.begin()
                        + static_cast<std::ptrdiff_t>(copied));
                copied += bytes;
                return true;
            });
        return read && copied == output.size();
    }

    [[nodiscard]] bool read_control_wave_(
            std::span<const hash_major_minimizer_authorized_control_range>
                ranges,
            void* consumer_context,
            hash_major_minimizer_consume_control_block_fn consume) {
        if (state_ != operation_state::source_authorized
            || consume == nullptr
            || ranges.empty()
            || ranges.size()
                > bioseq::indexing::
                    k_hash_major_minimizer_control_read_wave_blocks
            || ranges.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            return false;
        }
        const auto& descriptor =
            descriptor_(hash_major_minimizer_stream_kind::controls);
        std::uint64_t previous_end = 0u;
        std::uint64_t previous_block = 0u;
        bool have_previous = false;
        for (const auto& range : ranges) {
            const std::uint64_t end =
                range.control_byte_offset + range.byte_count;
            if (range.byte_count == 0u
                || range.byte_count
                    > k_hash_major_minimizer_maximum_control_block_bytes
                || range.reserved_zero != 0u
                || end < range.control_byte_offset
                || end > descriptor.logical_bytes
                || (have_previous
                    && (range.control_byte_offset < previous_end
                        || range.control_block_ordinal
                            <= previous_block))) {
                return false;
            }
            previous_end = end;
            previous_block = range.control_block_ordinal;
            have_previous = true;
        }

        std::size_t group_begin = 0u;
        while (group_begin < ranges.size()) {
            const auto first_interval_end =
                ranges[group_begin].control_byte_offset
                + ranges[group_begin].byte_count;
            std::uint64_t group_last_row =
                (first_interval_end - 1u)
                / k_hash_major_minimizer_row_payload_bytes;
            std::size_t group_end = group_begin + 1u;
            while (group_end < ranges.size()) {
                const std::uint64_t next_first_row =
                    ranges[group_end].control_byte_offset
                    / k_hash_major_minimizer_row_payload_bytes;
                if (next_first_row > group_last_row + 1u) break;
                const std::uint64_t next_end =
                    ranges[group_end].control_byte_offset
                    + ranges[group_end].byte_count;
                group_last_row = std::max(
                    group_last_row,
                    (next_end - 1u)
                        / k_hash_major_minimizer_row_payload_bytes);
                ++group_end;
            }
            const std::uint64_t group_first_row =
                ranges[group_begin].control_byte_offset
                / k_hash_major_minimizer_row_payload_bytes;
            std::size_t cursor = group_begin;
            const bool delivered = scan_stream_rows_(
                hash_major_minimizer_stream_kind::controls,
                group_first_row,
                group_last_row,
                [this, ranges, consumer_context, consume, group_end,
                 &cursor](std::uint64_t row_ordinal,
                        std::span<const std::byte> row) {
                    const std::uint64_t row_begin =
                        row_ordinal
                        * k_hash_major_minimizer_row_payload_bytes;
                    const std::uint64_t row_end =
                        row_begin + row.size();
                    while (cursor < group_end) {
                        const auto& range = ranges[cursor];
                        const std::uint64_t range_begin =
                            range.control_byte_offset;
                        const std::uint64_t range_end =
                            range_begin + range.byte_count;
                        if (range_begin >= row_end) break;
                        const std::uint64_t copy_begin =
                            std::max(row_begin, range_begin);
                        const std::uint64_t copy_end =
                            std::min(row_end, range_end);
                        if (copy_begin < copy_end) {
                            std::copy_n(
                                row.begin()
                                    + static_cast<std::ptrdiff_t>(
                                        copy_begin - row_begin),
                                static_cast<std::size_t>(
                                    copy_end - copy_begin),
                                control_wave_scratch_.begin()
                                    + static_cast<std::ptrdiff_t>(
                                        copy_begin - range_begin));
                        }
                        if (row_end < range_end) break;
                        if (!consume(
                                consumer_context,
                                static_cast<std::uint32_t>(cursor),
                                std::span<const std::byte>{
                                    control_wave_scratch_}.first(
                                        range.byte_count))) {
                            return false;
                        }
                        ++cursor;
                    }
                    return true;
                });
            if (!delivered || cursor != group_end) return false;
            group_begin = group_end;
        }
        return true;
    }

    [[nodiscard]] bool read_posting_wave_(
            std::span<const hash_major_minimizer_authorized_posting_range>
                ranges,
            void* consumer_context,
            hash_major_minimizer_consume_posting_bytes_fn consume) {
        if (state_ != operation_state::source_authorized
            || consume == nullptr || ranges.empty()
            || ranges.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            return false;
        }
        const auto& descriptor =
            descriptor_(hash_major_minimizer_stream_kind::postings);
        std::uint64_t previous_end = 0u;
        bool have_previous = false;
        for (const auto& range : ranges) {
            const std::uint64_t end =
                range.posting_byte_offset + range.byte_count;
            if (range.byte_count == 0u
                || range.reserved_zero != 0u
                || range.request_count == 0u
                || end < range.posting_byte_offset
                || end > descriptor.logical_bytes
                || (have_previous
                    && range.posting_byte_offset < previous_end)) {
                return false;
            }
            previous_end = end;
            have_previous = true;
        }

        std::size_t group_begin = 0u;
        while (group_begin < ranges.size()) {
            const std::uint64_t first_end =
                ranges[group_begin].posting_byte_offset
                + ranges[group_begin].byte_count;
            std::uint64_t group_last_row =
                (first_end - 1u)
                / k_hash_major_minimizer_row_payload_bytes;
            std::size_t group_end = group_begin + 1u;
            while (group_end < ranges.size()) {
                const std::uint64_t next_first_row =
                    ranges[group_end].posting_byte_offset
                    / k_hash_major_minimizer_row_payload_bytes;
                if (next_first_row > group_last_row + 1u) break;
                const std::uint64_t next_end =
                    ranges[group_end].posting_byte_offset
                    + ranges[group_end].byte_count;
                group_last_row = std::max(
                    group_last_row,
                    (next_end - 1u)
                        / k_hash_major_minimizer_row_payload_bytes);
                ++group_end;
            }
            const std::uint64_t group_first_row =
                ranges[group_begin].posting_byte_offset
                / k_hash_major_minimizer_row_payload_bytes;
            std::size_t cursor = group_begin;
            const bool delivered = scan_stream_rows_(
                hash_major_minimizer_stream_kind::postings,
                group_first_row,
                group_last_row,
                [this, ranges, consumer_context, consume, group_end,
                 &cursor](std::uint64_t row_ordinal,
                        std::span<const std::byte> row) {
                    const std::uint64_t row_begin =
                        row_ordinal
                        * k_hash_major_minimizer_row_payload_bytes;
                    const std::uint64_t row_end =
                        row_begin + row.size();
                    while (cursor < group_end) {
                        const auto& range = ranges[cursor];
                        const std::uint64_t range_begin =
                            range.posting_byte_offset;
                        const std::uint64_t range_end =
                            range_begin + range.byte_count;
                        if (range_begin >= row_end) break;
                        const std::uint64_t copy_begin =
                            std::max(row_begin, range_begin);
                        const std::uint64_t copy_end =
                            std::min(row_end, range_end);
                        if (copy_begin < copy_end
                            && !consume(
                                consumer_context,
                                static_cast<std::uint32_t>(cursor),
                                copy_begin - range_begin,
                                row.subspan(
                                    static_cast<std::size_t>(
                                        copy_begin - row_begin),
                                    static_cast<std::size_t>(
                                        copy_end - copy_begin)))) {
                            return false;
                        }
                        if (row_end < range_end) break;
                        if (stats_.hash_major.posting_ranges_delivered
                                == (std::numeric_limits<
                                    std::uint64_t>::max)()
                            || range.byte_count
                                > (std::numeric_limits<
                                    std::uint64_t>::max)()
                                    - stats_.hash_major
                                        .authorized_posting_bytes) {
                            return false;
                        }
                        ++stats_.hash_major.posting_ranges_delivered;
                        stats_.hash_major.authorized_posting_bytes +=
                            range.byte_count;
                        ++cursor;
                    }
                    return true;
                });
            if (!delivered || cursor != group_end) return false;
            group_begin = group_end;
        }
        return true;
    }

    [[nodiscard]] static bool read_bytes_callback_(
            void* context,
            hash_major_minimizer_stream_kind kind,
            std::uint64_t offset,
            std::span<std::byte> output) {
        return static_cast<index_storage_hash_major_minimizer_read_session*>(
            context)->read_logical_bytes_(kind, offset, output);
    }

    [[nodiscard]] static std::uint64_t logical_size_callback_(
            void* context,
            hash_major_minimizer_stream_kind kind) {
        auto& self =
            *static_cast<index_storage_hash_major_minimizer_read_session*>(
                context);
        if (self.state_ != operation_state::source_authorized
            || !hash_major_minimizer_pg_detail::valid_stream_kind(kind)) {
            return 0u;
        }
        return self.descriptor_(kind).logical_bytes;
    }

    [[nodiscard]] static bool control_wave_callback_(
            void* context,
            std::span<const hash_major_minimizer_authorized_control_range>
                ranges,
            void* consumer_context,
            hash_major_minimizer_consume_control_block_fn consume) {
        return static_cast<index_storage_hash_major_minimizer_read_session*>(
            context)->read_control_wave_(
                ranges, consumer_context, consume);
    }

    [[nodiscard]] static bool posting_wave_callback_(
            void* context,
            std::span<const hash_major_minimizer_authorized_posting_range>
                ranges,
            void* consumer_context,
            hash_major_minimizer_consume_posting_bytes_fn consume) {
        return static_cast<index_storage_hash_major_minimizer_read_session*>(
            context)->read_posting_wave_(
                ranges, consumer_context, consume);
    }

public:
    index_storage_hash_major_minimizer_read_session() = delete;
    index_storage_hash_major_minimizer_read_session(
            std::string_view table_name,
            std::string_view classification_index_name,
            std::uint64_t query_key,
            Snapshot registered_snapshot,
            Oid expected_tablespace_oid)
        : table_name_(table_name),
          index_name_(classification_index_name),
          snapshot_(registered_snapshot),
          query_key_(query_key),
          expected_tablespace_oid_(expected_tablespace_oid) {
        try {
            if (table_name_.empty()
                || index_name_.empty()
                || snapshot_ == nullptr
                || !IsMVCCSnapshot(snapshot_)
                || snapshot_->regd_count == 0
                || !OidIsValid(expected_tablespace_oid_)
                || !bioseq::indexing::sql_bigint_can_represent(
                    query_key_)) {
                fail_("hash-major read binding is invalid");
            }
            const Oid relation_oid =
                bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return get_relname_relid(
                        table_name_.c_str(), bioseq_internal_ns());
                });
            if (!OidIsValid(relation_oid)) {
                throw minimizer_generation_relation_open_failure{
                    .inspection = {
                        .kind = minimizer_generation_relation_kind::missing,
                        .schema_error = minimizer_generation_schema_error::none,
                    },
                };
            }
            heap_relation_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_oid] {
                return bioseq::postgres::support::try_open_postgres_relation(
                    relation_oid, AccessShareLock);
            });
            if (heap_relation_ == nullptr) {
                throw minimizer_generation_relation_open_failure{
                    .inspection = {
                        .kind = minimizer_generation_relation_kind::missing,
                        .schema_error = minimizer_generation_schema_error::none,
                    },
                };
            }
            stats_.cold.add(stats_.cold.sidecar_heap_opens, 1u);
            stats_.cold.add(stats_.cold.lock_acquires, 1u);
            const bool exact_identity =
                RelationGetNamespace(heap_relation_)
                    == bioseq_internal_ns()
                && std::string_view{
                    RelationGetRelationName(heap_relation_)}
                    == table_name_;
            const Oid primary_oid =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [this] {
                        return RelationGetPrimaryKeyIndex(
                            heap_relation_);
                    });
            if (OidIsValid(primary_oid)) {
                primary_index_relation_ =
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [primary_oid] {
                            return index_open(
                                primary_oid, AccessShareLock);
                        });
                stats_.cold.add(
                    stats_.cold.sidecar_index_opens, 1u);
                stats_.cold.add(stats_.cold.lock_acquires, 1u);
            }
            const minimizer_generation_schema_inspection schema = exact_identity
                ? bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return inspect_bound_minimizer_generation_schema(
                        heap_relation_,
                        primary_index_relation_,
                        minimizer_generation_schema_expectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = table_name_,
                            .tablespace_oid =
                                expected_tablespace_oid_,
                        });
                })
                : minimizer_generation_schema_inspection{
                    .error = minimizer_generation_schema_error::not_heap,
                };
            stats_.cold.add(
                stats_.cold.sidecar_schema_validations, 1u);
            if (!schema) {
                if (primary_index_relation_ != nullptr) {
                    Relation finished = std::exchange(
                        primary_index_relation_, nullptr);
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [finished] {
                            index_close(
                                finished, AccessShareLock);
                        });
                }
                Relation finished =
                    std::exchange(heap_relation_, nullptr);
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [finished] {
                        relation_close(
                            finished, AccessShareLock);
                    });
                snapshot_ = nullptr;
                throw minimizer_generation_relation_open_failure{
                    .inspection = {
                        .kind = minimizer_generation_relation_kind::malformed,
                        .schema_error = schema.error,
                    },
                };
            }
            (void)verify_pg_inline_payload_layout(heap_relation_);
            stats_.relation_opens = 1u;
        } catch (const minimizer_generation_relation_open_failure&) {
            state_ = operation_state::failed;
            abandon_noexcept_();
            throw;
        } catch (...) {
            state_ = operation_state::failed;
            abandon_noexcept_();
            throw;
        }
    }
    index_storage_hash_major_minimizer_read_session(
        const index_storage_hash_major_minimizer_read_session&) = delete;
    index_storage_hash_major_minimizer_read_session& operator=(
        const index_storage_hash_major_minimizer_read_session&) = delete;
    index_storage_hash_major_minimizer_read_session(
            index_storage_hash_major_minimizer_read_session&& other) noexcept
        : table_name_(std::move(other.table_name_)),
          index_name_(std::move(other.index_name_)),
          heap_relation_(std::exchange(
              other.heap_relation_, nullptr)),
          primary_index_relation_(std::exchange(
              other.primary_index_relation_, nullptr)),
          active_scan_(std::exchange(other.active_scan_, nullptr)),
          row_batch_slots_(other.row_batch_slots_),
          row_batch_slot_count_(other.row_batch_slot_count_),
          cached_boundary_row_valid_(
              other.cached_boundary_row_valid_),
          cached_boundary_row_kind_(
              other.cached_boundary_row_kind_),
          cached_boundary_row_ordinal_(
              other.cached_boundary_row_ordinal_),
          cached_boundary_slot_ordinal_(
              other.cached_boundary_slot_ordinal_),
          cached_boundary_row_(other.cached_boundary_row_),
          snapshot_(std::exchange(other.snapshot_, nullptr)),
          query_key_(other.query_key_),
          expected_tablespace_oid_(
              other.expected_tablespace_oid_),
          state_(other.state_),
          encoded_head_(other.encoded_head_),
          encoded_manifest_(other.encoded_manifest_),
          head_(std::move(other.head_)),
          manifest_(std::move(other.manifest_)),
          expected_generation_(
              std::move(other.expected_generation_)),
          control_wave_scratch_(other.control_wave_scratch_),
          stats_(other.stats_) {
        other.invalidate_cached_boundary_row_();
        other.row_batch_slots_.fill(nullptr);
        other.row_batch_slot_count_ = 0u;
        other.state_ = operation_state::moved;
    }
    index_storage_hash_major_minimizer_read_session& operator=(
        index_storage_hash_major_minimizer_read_session&&) = delete;
    ~index_storage_hash_major_minimizer_read_session() {
        abandon_noexcept_();
    }

    /// Returns false only for an authenticated absence of the distinct
    /// hash-major head sentinel.  A present malformed head or manifest is a
    /// hard error and never enters the grouped-SoA fallback.
    [[nodiscard]] bool read_active_head_manifest_once() {
        if (state_ != operation_state::opened || active_scan_ != nullptr) {
            fail_("hash-major metadata was read more than once");
        }
        ScanKeyData head_keys[5];
        ScanKeyInit(&head_keys[0], k_minimizer_generation_index_generation_id,
            BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
        ScanKeyInit(&head_keys[1], k_minimizer_generation_index_query_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&head_keys[2], k_minimizer_generation_index_sub_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                k_hash_major_generation_head_sub_key)));
        ScanKeyInit(&head_keys[3], k_minimizer_generation_index_position_segment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
        ScanKeyInit(&head_keys[4], k_minimizer_generation_index_chunk_number,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp(
            [this, &head_keys] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_,
                snapshot_, 5, head_keys);
        });
        ++stats_.index_scans;
        ++stats_.head_scans;
        HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp([this] {
            return systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
        });
        if (!HeapTupleIsValid(tuple)) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;
            state_ = operation_state::head_absent;
            return false;
        }
        minimizer_generation_loaded_row head_row =
            load_minimizer_generation_row_or_error(heap_relation_, tuple);
        if (head_row.query_key != query_key_
            || head_row.address != index_chunk_address{
                .generation_id = 0u,
                .position_segment = -1,
                .sub_key = k_hash_major_generation_head_sub_key,
                .chunk_no = 0u,
            }
            || head_row.bytes.size()
                != k_hash_major_minimizer_head_bytes) {
            head_row.release();
            fail_("hash-major head row shape disagrees");
        }
        std::copy(
            head_row.bytes.begin(), head_row.bytes.end(),
            encoded_head_.begin());
        head_row.release();
        HeapTuple extra = bioseq::postgres::support::invoke_pg_as_cpp([this] {
            return systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
        });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        if (HeapTupleIsValid(extra)) {
            fail_("hash-major head primary key returned duplicates");
        }
        auto decoded_head =
            bioseq::indexing::decode_hash_major_minimizer_head(
                encoded_head_);
        if (!decoded_head
            || decoded_head->query_key != query_key_
            || decoded_head->generation_id == 0u
            || !bioseq::indexing::sql_bigint_can_represent(
                decoded_head->generation_id)) {
            fail_("hash-major head fixed wire is invalid");
        }
        head_.emplace(*decoded_head);
        if (head_->query_key != query_key_
            || head_->generation_id == 0u
            || !bioseq::indexing::sql_bigint_can_represent(
                head_->generation_id)) {
            fail_("hash-major head fixed wire is invalid");
        }
        stats_.head_rows_read = 1u;
        stats_.head_bytes_read = k_hash_major_minimizer_head_bytes;
        stats_.cold.add(
            stats_.cold.row_crc_bytes,
            k_hash_major_minimizer_head_bytes);

        ScanKeyData manifest_keys[5];
        ScanKeyInit(&manifest_keys[0], k_minimizer_generation_index_generation_id,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                head_->generation_id)));
        ScanKeyInit(&manifest_keys[1], k_minimizer_generation_index_query_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&manifest_keys[2], k_minimizer_generation_index_sub_key,
            BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
        ScanKeyInit(&manifest_keys[3], k_minimizer_generation_index_position_segment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        ScanKeyInit(&manifest_keys[4], k_minimizer_generation_index_chunk_number,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp(
            [this, &manifest_keys] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_,
                snapshot_, 5, manifest_keys);
        });
        ++stats_.index_scans;
        ++stats_.manifest_directory_scans;
        tuple = bioseq::postgres::support::invoke_pg_as_cpp([this] {
            return systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
        });
        if (!HeapTupleIsValid(tuple)) {
            fail_("hash-major manifest row is missing");
        }
        minimizer_generation_loaded_row manifest_row =
            load_minimizer_generation_row_or_error(heap_relation_, tuple);
        if (manifest_row.query_key != query_key_
            || manifest_row.address != index_chunk_address{
                .generation_id = head_->generation_id,
                .position_segment = 0,
                .sub_key = 0,
                .chunk_no = 0u,
            }
            || manifest_row.bytes.size()
                != k_hash_major_minimizer_manifest_bytes) {
            manifest_row.release();
            fail_("hash-major manifest row shape disagrees");
        }
        std::copy(
            manifest_row.bytes.begin(), manifest_row.bytes.end(),
            encoded_manifest_.begin());
        manifest_row.release();
        extra = bioseq::postgres::support::invoke_pg_as_cpp([this] {
            return systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
        });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        if (HeapTupleIsValid(extra)) {
            fail_("hash-major manifest primary key returned duplicates");
        }
        auto decoded_manifest =
            bioseq::indexing::decode_hash_major_minimizer_manifest(
                encoded_manifest_);
        if (!decoded_manifest
            || decoded_manifest->generation_id
                != head_->generation_id
            || decoded_manifest->source_epoch
                != head_->source_epoch
            || decoded_manifest->source_identity
                != head_->source_identity
            || decoded_manifest->source_content_crc64
                != head_->source_content_crc64
            || decoded_manifest->storage_profile_digest
                != head_->storage_profile_digest) {
            fail_("hash-major head/manifest authentication disagrees");
        }
        manifest_.emplace(*decoded_manifest);
        if (manifest_->generation_id != head_->generation_id
            || manifest_->source_epoch != head_->source_epoch
            || manifest_->source_identity != head_->source_identity
            || manifest_->source_content_crc64
                != head_->source_content_crc64
            || manifest_->storage_profile_digest
                != head_->storage_profile_digest
            || head_->manifest_payload_crc64
                != bioseq::indexing::crc64_ecma182(
                    encoded_manifest_)
            || head_->manifest_address_bound_row_crc64
                != bioseq::indexing::crc64_index_chunk_row(
                    head_->generation_id, 0, 0, 0u,
                    static_cast<std::uint32_t>(
                        encoded_manifest_.size()),
                    encoded_manifest_)
            || !bioseq::indexing::
                is_valid_hash_major_minimizer_storage_profile(
                    manifest_->storage_profile,
                    manifest_->target_length)
            || manifest_->streams[0].kind
                != hash_major_minimizer_stream_kind::directory
            || manifest_->streams[1].kind
                != hash_major_minimizer_stream_kind::controls
            || manifest_->streams[2].kind
                != hash_major_minimizer_stream_kind::postings) {
            fail_("hash-major head/manifest authentication disagrees");
        }
        std::uint64_t first_row = 0u;
        for (std::size_t index = 0u; index < 3u; ++index) {
            const auto& stream = manifest_->streams[index];
            if (stream.row_count
                    != hash_major_minimizer_pg_detail::ceil_rows(
                        stream.logical_bytes)
                || stream.first_row_ordinal != first_row
                || stream.row_count
                    > static_cast<std::uint64_t>(
                        (std::numeric_limits<std::int32_t>::max)())
                || stream.row_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - first_row) {
                fail_("hash-major stream row descriptor disagrees");
            }
            first_row += stream.row_count;
        }
        stats_.manifest_rows_read = 1u;
        stats_.manifest_bytes_read =
            k_hash_major_minimizer_manifest_bytes;
        stats_.metadata_heap_rows_read = 2u;
        stats_.metadata_bytes_read =
            k_hash_major_minimizer_head_bytes
            + k_hash_major_minimizer_manifest_bytes;
        stats_.cold.add(
            stats_.cold.row_crc_bytes,
            k_hash_major_minimizer_manifest_bytes);
        state_ = operation_state::metadata_ready;
        return true;
    }

    [[nodiscard]] std::span<const std::byte>
    encoded_head() const noexcept {
        return state_ == operation_state::metadata_ready
                || state_ == operation_state::source_authorized
            ? std::span<const std::byte>{encoded_head_}
            : std::span<const std::byte>{};
    }

    [[nodiscard]] std::span<const std::byte>
    encoded_manifest() const noexcept {
        return state_ == operation_state::metadata_ready
                || state_ == operation_state::source_authorized
            ? std::span<const std::byte>{encoded_manifest_}
            : std::span<const std::byte>{};
    }

    [[nodiscard]] const hash_major_minimizer_head& head() const {
        if (!head_) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major head requested before metadata");
        }
        return *head_;
    }

    [[nodiscard]] const hash_major_minimizer_manifest& manifest() const {
        if (!manifest_) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major manifest requested before metadata");
        }
        return *manifest_;
    }

    [[nodiscard]] const expected_hash_major_minimizer_generation&
    consume_source_authorization(minimizer_generation_catalog_source_authorization&& authorization) {
        try {
            if (state_ != operation_state::metadata_ready
                || !head_ || !manifest_
                || !authorization.valid_
                || !authorization.stats_.timed_equation_holds()
                || authorization.snapshot_ != snapshot_
                || authorization.database_oid_ != MyDatabaseId
                || authorization.tablespace_oid_ != expected_tablespace_oid_
                || authorization.index_name_ != index_name_
                || authorization.source_.sequence_rowno != query_key_
                || authorization.table_name_ != table_name_) {
                authorization.invalidate_();
                fail_("hash-major source authorization binding/work disagrees");
            }
            const auto source_identity =
                bioseq::indexing::hash_major_minimizer_source_identity(
                    query_key_,
                    authorization.source_.locator.source_epoch,
                    authorization.source_content_checksum_,
                    authorization.source_.locator.logical_length);
            if (authorization.source_.locator.source_epoch
                    != manifest_->source_epoch
                || authorization.source_content_checksum_
                    != manifest_->source_content_crc64
                || authorization.source_.locator.logical_length
                    != manifest_->target_length
                || authorization.source_.alphabet_code
                    != manifest_->storage_profile.alphabet_code
                || source_identity != manifest_->source_identity
                || source_identity != head_->source_identity) {
                authorization.invalidate_();
                fail_("hash-major source identity/content disagrees");
            }
            stats_.source_parent_rows_read =
                authorization.stats_.parent_rows_read;
            stats_.source_catalog_rows_read =
                authorization.stats_.catalog_rows_read;
            stats_.source_catalog_index_scans =
                authorization.stats_.catalog_index_scans;
            stats_.source_fragment_rows_read =
                authorization.stats_.fragment_rows_read;
            stats_.source_decoded_bp =
                authorization.stats_.decoded_bp;
            stats_.source_storage_incarnations_checked =
                authorization.stats_.storage_incarnations_checked;
            stats_.cold.merge(authorization.cold_stats_);
            expected_generation_.emplace(
                expected_hash_major_minimizer_generation{
                    .generation_id = head_->generation_id,
                    .query_key = query_key_,
                    .source_epoch = manifest_->source_epoch,
                    .source_content_crc64 =
                        manifest_->source_content_crc64,
                    .target_length = manifest_->target_length,
                    .storage_profile = manifest_->storage_profile,
                });
            authorization.invalidate_();
            state_ = operation_state::source_authorized;
            return *expected_generation_;
        } catch (...) {
            authorization.invalidate_();
            state_ = operation_state::failed;
            throw;
        }
    }

    [[nodiscard]] hash_major_minimizer_logical_stream_source_ref
    source_ref() noexcept {
        if (state_ != operation_state::source_authorized) return {};
        return {
            .context = this,
            .read_bytes = &read_bytes_callback_,
            .logical_size = &logical_size_callback_,
            .read_control_wave = &control_wave_callback_,
            .read_posting_wave = &posting_wave_callback_,
        };
    }

    /// Execute the isolated process-parallel transport experiment without
    /// changing the production selective-reader callbacks.
    [[nodiscard]] hash_major_minimizer_parallel_scan_stats
    run_parallel_scan_probe(std::uint32_t requested_workers) {
        if (state_ != operation_state::source_authorized
            || active_scan_ != nullptr
            || heap_relation_ == nullptr
            || !manifest_) {
            fail_(
                "hash-major parallel scan probe is out of order");
        }
        return hash_major_minimizer_parallel_probe_detail::run_probe(
            heap_relation_, snapshot_, *manifest_,
            query_key_, requested_workers);
    }

    /// Administrative/restart audit. Each stream is reread in primary-key
    /// order with one physical row resident at a time. The scan authenticates
    /// every address-bound row receipt and the exact concatenated logical CRC.
    [[nodiscard]] std::array<
        hash_major_minimizer_catalog_stream_receipt, 3>
    audit_complete_streams() {
        if (state_ != operation_state::source_authorized
            || active_scan_ != nullptr || !manifest_) {
            fail_("hash-major complete stream audit is out of order");
        }
        std::array<hash_major_minimizer_catalog_stream_receipt, 3>
            receipts{};
        for (std::size_t index = 0u; index < receipts.size(); ++index) {
            const hash_major_minimizer_stream_kind kind =
                hash_major_minimizer_pg_detail::stream_kind(index);
            const auto& descriptor = descriptor_(kind);
            crc64_ecma182_accumulator crc{};
            std::uint64_t rows = 0u;
            std::uint64_t bytes = 0u;
            bool read = true;
            if (descriptor.row_count != 0u) {
                read = scan_stream_rows_(
                    kind, 0u, descriptor.row_count - 1u,
                    [&rows, &bytes, &crc](std::uint64_t,
                            std::span<const std::byte> row) {
                        ++rows;
                        bytes += row.size();
                        crc.update(row);
                        return true;
                    });
            }
            receipts[index] = {
                .kind = kind,
                .exact_catalog_rows = rows,
                .exact_catalog_bytes = bytes,
                .exact_catalog_stream_crc64 = crc.checksum(),
            };
            if (!read
                || rows != descriptor.row_count
                || bytes != descriptor.logical_bytes
                || crc.checksum() != descriptor.stream_crc64) {
                fail_("hash-major complete stream audit disagrees");
            }
        }
        return receipts;
    }

    /// Checked close used by a route-inapplicable, self-authenticated F6 head
    /// before the independent F5 sentinel is probed.
    void close_metadata_read() {
        if (state_ != operation_state::metadata_ready
            || active_scan_ != nullptr) {
            fail_("hash-major metadata close is out of order");
        }
        state_ = operation_state::finished;
        close_relations_();
    }

    void close_head_absent() {
        if (state_ != operation_state::head_absent
            || active_scan_ != nullptr) {
            fail_("hash-major absent-head close is out of order");
        }
        state_ = operation_state::finished;
        close_relations_();
    }

    [[nodiscard]] minimizer_generation_read_stats finish() {
        if (state_ != operation_state::source_authorized
            || active_scan_ != nullptr
            || heap_relation_ == nullptr
            || primary_index_relation_ == nullptr) {
            fail_("hash-major read finished out of order");
        }
        stats_.total_heap_rows_read =
            stats_.metadata_heap_rows_read
            + stats_.payload_heap_rows_read;
        stats_.total_bytes_read =
            stats_.metadata_bytes_read
            + stats_.payload_bytes_read;
        stats_.cold.add(
            stats_.cold.payload_rows_requested,
            stats_.payload_heap_rows_read);
        stats_.cold.add(
            stats_.cold.payload_bytes_requested,
            stats_.payload_bytes_read);
        stats_.hash_major.layout_revision =
            bioseq::indexing::k_hash_major_minimizer_layout_revision;
        stats_.hash_major.sessions = 1u;
        stats_.hash_major.generation_id = head_->generation_id;
        stats_.hash_major.source_identity = head_->source_identity;
        stats_.hash_major.source_content_crc64 =
            head_->source_content_crc64;
        stats_.hash_major.storage_contract_digest =
            manifest_->storage_profile_digest;
        stats_.hash_major.directory_rows =
            stats_.directory_chunks_read;
        stats_.hash_major.directory_bytes =
            stats_.directory_bytes_read;
        stats_.hash_major.control_rows =
            stats_.controls.physical_heap_rows_read;
        stats_.hash_major.control_bytes =
            stats_.controls.physical_bytes_read;
        stats_.hash_major.posting_rows =
            stats_.positions.physical_heap_rows_read;
        stats_.hash_major.posting_bytes =
            stats_.positions.physical_bytes_read;
        state_ = operation_state::finished;
        close_relations_();
        return stats_;
    }
};

static_assert(!std::is_default_constructible_v<
    index_storage_hash_major_minimizer_read_session>);
static_assert(!std::is_copy_constructible_v<
    index_storage_hash_major_minimizer_read_session>);
static_assert(std::is_nothrow_move_constructible_v<
    index_storage_hash_major_minimizer_read_session>);

/// Normal open outcome with either an owned reader or classified absence.
struct hash_major_minimizer_read_session_open_result {
    std::optional<index_storage_hash_major_minimizer_read_session>
        session{};
    minimizer_generation_relation_inspection relation_failure{
        .kind = minimizer_generation_relation_kind::malformed,
        .schema_error = minimizer_generation_schema_error::none,
    };
    bool head_absent = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return session.has_value();
    }
};

[[nodiscard]] inline hash_major_minimizer_read_session_open_result
open_exact_hash_major_minimizer_read_session_pg(
        std::string_view table_name,
        std::string_view index_name,
        std::uint64_t query_key,
        const borrowed_executor_read_context& executor_read) {
    if (!executor_read.valid()) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major read requires a valid executor context");
    }
    hash_major_minimizer_read_session_open_result result{};
    try {
        result.session.emplace(
            table_name,
            index_name,
            query_key,
            executor_read.snapshot(),
            executor_read.tablespace_oid());
        if (!result.session->read_active_head_manifest_once()) {
            result.session->close_head_absent();
            result.session.reset();
            result.head_absent = true;
            result.relation_failure = {
                .kind =
                    minimizer_generation_relation_kind::current_exact,
                .schema_error = minimizer_generation_schema_error::none,
            };
        } else {
            result.relation_failure = {
                .kind =
                    minimizer_generation_relation_kind::current_exact,
                .schema_error = minimizer_generation_schema_error::none,
            };
        }
    } catch (const minimizer_generation_relation_open_failure& failure) {
        result.relation_failure = failure.inspection;
    }
    return result;
}

/// Read-only restart/audit receipt for the complete fixed hash-major metadata
/// surface. parallel_scan_stream descriptors carry the directory/control/posting counts,
/// byte lengths and CRCs; the manifest carries the generation CRC.
struct ready_hash_major_minimizer_metadata_identity {
    hash_major_minimizer_head head{};
    hash_major_minimizer_manifest manifest{};
    encoded_hash_major_minimizer_head encoded_head{};
    encoded_hash_major_minimizer_manifest encoded_manifest{};
    minimizer_generation_source_locator source{};
    std::uint64_t source_content_checksum = 0;
    minimizer_generation_catalog_source_stats source_stats{};
    std::array<hash_major_minimizer_catalog_stream_receipt, 3>
        exact_catalog_streams{};
};

[[nodiscard]] inline ready_hash_major_minimizer_metadata_identity
inspect_ready_hash_major_minimizer_metadata_pg(
        const minimizer_generation_administration_identity& identity,
        std::uint64_t query_key,
        Snapshot registered_snapshot,
        minimizer_generation_catalog_source_authorization&& authorization) {
    if (identity.database_oid != MyDatabaseId
        || identity.index_name.empty()
        || registered_snapshot == nullptr
        || !IsMVCCSnapshot(registered_snapshot)
        || registered_snapshot->regd_count == 0) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major metadata inspection input is invalid");
    }
    const std::string table_name{
        authorization.table_name_for_diagnostics()};
    if (table_name.empty()
        || (!identity.table_name.empty()
            && identity.table_name != table_name)) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major metadata inspection source/table binding is invalid");
    }
    const minimizer_generation_source_locator source =
        authorization.source_for_diagnostics();
    const std::uint64_t source_content_checksum =
        authorization.source_content_checksum_for_diagnostics();
    const minimizer_generation_catalog_source_stats source_stats =
        authorization.stats_for_diagnostics();
    index_storage_hash_major_minimizer_read_session session{
        table_name,
        identity.index_name,
        query_key,
        registered_snapshot,
        identity.tablespace_oid,
    };
    if (!session.read_active_head_manifest_once()) {
        session.close_head_absent();
        minimizer_generation_storage_error_as_cpp(
            "hash-major metadata inspection found no READY head");
    }
    (void)session.consume_source_authorization(std::move(authorization));
    ready_hash_major_minimizer_metadata_identity result{
        .head = session.head(),
        .manifest = session.manifest(),
        .source = source,
        .source_content_checksum = source_content_checksum,
        .source_stats = source_stats,
    };
    std::ranges::copy(
        session.encoded_head(), result.encoded_head.begin());
    std::ranges::copy(
        session.encoded_manifest(),
        result.encoded_manifest.begin());
    result.exact_catalog_streams =
        session.audit_complete_streams();
    (void)session.finish();
    return result;
}

[[nodiscard]] inline ready_hash_major_minimizer_metadata_identity
inspect_ready_hash_major_minimizer_metadata_pg(
        const minimizer_generation_administration_identity& identity,
        std::uint64_t query_key,
        Snapshot registered_snapshot) {
    minimizer_generation_catalog_source_authorization authorization =
        authorize_minimizer_generation_catalog_source_pg(
            identity, query_key, registered_snapshot);
    return inspect_ready_hash_major_minimizer_metadata_pg(
        identity, query_key, registered_snapshot, std::move(authorization));
}

/// Test-only transport probe. It authenticates the same READY head, manifest
/// and source binding as the production reader, then scans only the three
/// current-generation hash-major stream subkeys through either one serial heap
/// scan or one core ParallelContext/shared ParallelTableScanDesc.
[[nodiscard]] inline hash_major_minimizer_parallel_scan_stats
probe_ready_hash_major_minimizer_parallel_scan_pg(
        const minimizer_generation_administration_identity& identity,
        std::uint64_t query_key,
        Snapshot registered_snapshot,
        minimizer_generation_catalog_source_authorization&& authorization,
        std::uint32_t requested_workers) {
    if (identity.database_oid != MyDatabaseId
        || identity.index_name.empty()
        || registered_snapshot == nullptr
        || !IsMVCCSnapshot(registered_snapshot)
        || registered_snapshot->regd_count == 0) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe input is invalid");
    }
    const std::string table_name{
        authorization.table_name_for_diagnostics()};
    if (table_name.empty()) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe table binding is empty");
    }
    index_storage_hash_major_minimizer_read_session session{
        table_name,
        identity.index_name,
        query_key,
        registered_snapshot,
        identity.tablespace_oid,
    };
    if (!session.read_active_head_manifest_once()) {
        session.close_head_absent();
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe found no READY head");
    }
    (void)session.consume_source_authorization(std::move(authorization));
    hash_major_minimizer_parallel_scan_stats result =
        session.run_parallel_scan_probe(requested_workers);
    (void)session.finish();
    return result;
}

inline void hash_major_minimizer_parallel_scan_probe_worker_main_pg(
        dsm_segment* segment,
        shm_toc* toc) {
    using namespace hash_major_minimizer_parallel_probe_detail;
    if (segment == nullptr || toc == nullptr
        || !IsParallelWorker()) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq hash-major parallel scan worker binding is invalid")));
    }
    auto* header = static_cast<parallel_scan_header*>(
        shm_toc_lookup(toc, k_header_toc_key, false));
    auto* parallel_scan =
        static_cast<ParallelTableScanDesc>(
            shm_toc_lookup(toc, k_parallel_scan_toc_key, false));
    auto* seen_words = static_cast<pg_atomic_uint64*>(
        shm_toc_lookup(toc, k_seen_bitset_toc_key, false));
    if (header == nullptr || parallel_scan == nullptr
        || seen_words == nullptr
        || header->magic != k_toc_magic
        || header->format_revision != k_parallel_header_format_revision
        || header->relation_locator.dbOid != MyDatabaseId
        || GetActiveSnapshot() == nullptr
        || !IsMVCCSnapshot(GetActiveSnapshot())) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq hash-major parallel scan worker state is invalid")));
    }

    // ParallelWorkerMain joined the leader's lock group before loading this
    // symbol, so NoLock reuses the leader-held AccessShareLock and cannot
    // release it independently.
    Relation relation = table_open(
        header->relation_oid, NoLock);
    parallel_scan_pg_leaf(
        relation, parallel_scan, header, seen_words);
    table_close(relation, NoLock);
}

/// Result of one current-generation logical-index clear.
struct minimizer_generation_clear_result {
    std::uint64_t dropped_relation_count = 0;
    std::uint64_t deleted_metadata_age_rows = 0;
};

/// Parsed bucket and shard from one canonical generation sidecar name.
struct canonical_generation_sidecar_name {
    bool valid = false;
    std::uint8_t bucket_idx = 0;
    std::uint32_t shard_idx = 0;
};

/// Parse only the exact spelling emitted by `format_seqidx_name`: unsigned
/// canonical decimal bucket, one underscore, unsigned canonical decimal shard.
/// Leading zeroes, signs, trailing decorations, overflow, and extra separators
/// are rejected without allocation.
[[nodiscard]] inline canonical_generation_sidecar_name
parse_canonical_generation_sidecar_name(
        std::string_view relation_name,
        std::string_view prefix) noexcept {
    if (!relation_name.starts_with(prefix)) return {};
    const std::string_view suffix = relation_name.substr(prefix.size());
    const std::size_t separator = suffix.find('_');
    if (separator == std::string_view::npos || separator == 0
        || separator + 1 >= suffix.size()
        || suffix.find('_', separator + 1) != std::string_view::npos) {
        return {};
    }
    const std::string_view bucket_text = suffix.substr(0, separator);
    const std::string_view shard_text = suffix.substr(separator + 1);
    if ((bucket_text.size() > 1 && bucket_text.front() == '0')
        || (shard_text.size() > 1 && shard_text.front() == '0')) {
        return {};
    }
    const auto parse_decimal = [](std::string_view text,
            std::uint64_t maximum, std::uint64_t& result) noexcept {
        result = 0;
        for (const char character : text) {
            if (character < '0' || character > '9') return false;
            const std::uint64_t digit =
                static_cast<std::uint64_t>(character - '0');
            if (result > (maximum - digit) / 10u) return false;
            result = result * 10u + digit;
        }
        return true;
    };
    std::uint64_t bucket = 0;
    std::uint64_t shard = 0;
    if (!parse_decimal(bucket_text,
            (std::numeric_limits<std::uint8_t>::max)(), bucket)
        || !parse_decimal(shard_text,
            (std::numeric_limits<std::uint32_t>::max)(), shard)) {
        return {};
    }
    return canonical_generation_sidecar_name{
        .valid = true,
        .bucket_idx = static_cast<std::uint8_t>(bucket),
        .shard_idx = static_cast<std::uint32_t>(shard),
    };
}

/// Clear-only DDL executor. Every SPI transition is checked and the caller-owned
/// activity bit remains set if PostgreSQL jumps
/// out of SPI_execute so the enclosing PG_CATCH can finish the connection.
inline void run_checked_generation_clear_ddl(
        const char* ddl,
        bool& spi_active) {
    if (ddl == nullptr || *ddl == '\0' || spi_active) {
        minimizer_generation_storage_error("generation clear DDL state is invalid");
    }
    const int connect_rc = SPI_connect();
    if (connect_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq current-generation index: generation clear DDL SPI_connect failed"),
             errdetail("SPI rc=%d", connect_rc)));
    }
    spi_active = true;
    const int execute_rc = SPI_execute(
        ddl, /*read_only=*/false, /*tcount=*/0);
    const int finish_rc = SPI_finish();
    spi_active = finish_rc != SPI_OK_FINISH;
    if (execute_rc != SPI_OK_UTILITY || finish_rc != SPI_OK_FINISH) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq current-generation index: generation clear DDL failed"),
             errdetail("execute_rc=%d finish_rc=%d",
                execute_rc, finish_rc)));
    }
}


}  // namespace bioseq::storage::indexing
