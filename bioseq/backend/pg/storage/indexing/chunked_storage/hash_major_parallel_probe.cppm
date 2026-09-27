/**
 * @file hash_major_parallel_probe.cppm
 * @brief Defines the authenticated serial and parallel hash-major transport probe.
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
export module bioseq.postgres.storage.indexing.hash_major_parallel_probe;
import std;
export import bioseq.postgres.storage.indexing.hash_major_parallel_probe_contract;
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
import bioseq.postgres.storage.indexing.chunked_storage;
export namespace bioseq::storage::indexing {
namespace hash_major_minimizer_parallel_probe_detail {

inline constexpr std::uint64_t k_toc_magic = std::uint64_t{0x4236534350524f42}; // "B6SCPROB"
inline constexpr std::uint64_t k_header_toc_key = std::uint64_t{0x4236534300000001};
inline constexpr std::uint64_t k_parallel_scan_toc_key = std::uint64_t{0x4236534300000002};
inline constexpr std::uint64_t k_seen_bitset_toc_key = std::uint64_t{0x4236534300000003};
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

inline constexpr std::uint64_t k_image_magic = std::uint64_t{0x42364636494d4147}; // "B6F6IMAG"
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
[[nodiscard]] inline std::uint64_t mix64(std::uint64_t value) noexcept {
    value += std::uint64_t{0x9e3779b97f4a7c15};
    value = (value ^ (value >> 30u)) * std::uint64_t{0xbf58476d1ce4e5b9};
    value = (value ^ (value >> 27u)) * std::uint64_t{0x94d049bb133111eb};
    return value ^ (value >> 31u);
}

/// Round an image offset up to the fixed section alignment.
[[nodiscard]] inline std::uint64_t align_image_offset(std::uint64_t value) {
    if (value > (std::numeric_limits<std::uint64_t>::max)() - (k_image_section_alignment - 1u)) {
        minimizer_generation_storage_error_as_cpp("hash-major mmap image offset overflows");
    }
    return (value + k_image_section_alignment - 1u) / k_image_section_alignment *
           k_image_section_alignment;
}

/// Compute the header checksum with its checksum field cleared.
[[nodiscard]] inline std::uint64_t image_header_crc(const parallel_image_header& header) noexcept {
    parallel_image_header copy = header;
    copy.header_crc64 = 0;
    crc64_ecma182_accumulator crc{};
    crc.update(std::as_bytes(std::span{&copy, std::size_t{1}}));
    return crc.checksum();
}

[[noreturn]] inline void corrupt_row_pg_leaf(parallel_scan_header* header, const char* detail) {
    if (header != nullptr) {
        (void)pg_atomic_fetch_add_u64(&header->corrupt_rows, 1);
    }
    ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq hash-major parallel scan probe rejected a row"),
             errdetail_internal("%s", detail)));
    pg_unreachable();
}

[[noreturn]] inline void duplicate_row_pg_leaf(parallel_scan_header* header,
                                               std::uint64_t global_row) {
    (void)pg_atomic_fetch_add_u64(&header->duplicate_rows, 1);
    ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq hash-major parallel scan probe found a duplicate row"),
             errdetail_internal("global_row=%llu", static_cast<unsigned long long>(global_row))));
    pg_unreachable();
}

/// Resolve a persisted sub-key to its shared stream slot.
[[nodiscard]] inline int stream_index_for_sub_key(const parallel_scan_header& header,
                                                  std::int64_t sub_key) noexcept {
    for (std::size_t index = 0; index < header.streams.size(); ++index) {
        if (header.streams[index].sub_key == sub_key) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

/// Authenticate every tuple claimed from one serial or parallel table scan.
inline void scan_table_desc_pg_leaf(Relation relation,
                                    TableScanDesc scan,
                                    parallel_scan_header* header,
                                    pg_atomic_uint64* seen_words) {
    if (relation == nullptr || scan == nullptr || header == nullptr || seen_words == nullptr ||
        header->magic != k_toc_magic ||
        header->format_revision != k_parallel_header_format_revision ||
        RelationGetRelid(relation) != header->relation_oid ||
        !RelFileLocatorEquals(relation->rd_locator, header->relation_locator) ||
        RelationGetDescr(relation) == nullptr ||
        RelationGetDescr(relation)->natts !=
            static_cast<int>(k_minimizer_generation_index_attribute_descriptors.size())) {
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

    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        ++visited_rows;
        slot_getallattrs(slot);
        Datum* values = slot->tts_values;
        bool* nulls = slot->tts_isnull;

        // Filter on the four fixed identity columns before inspecting BYTEA.
        // Non-active generations and metadata rows are deliberately outside this
        // probe's corruption authority.
        if (!nulls[k_minimizer_generation_attribute_generation_id - 1] &&
            !nulls[k_minimizer_generation_attribute_query_key - 1] &&
            !nulls[k_minimizer_generation_attribute_position_segment - 1] &&
            !nulls[k_minimizer_generation_attribute_sub_key - 1] &&
            DatumGetInt64(values[k_minimizer_generation_attribute_generation_id - 1]) ==
                static_cast<std::int64_t>(header->generation_id) &&
            DatumGetInt64(values[k_minimizer_generation_attribute_query_key - 1]) ==
                static_cast<std::int64_t>(header->query_key) &&
            DatumGetInt32(values[k_minimizer_generation_attribute_position_segment - 1]) == 0) {
            const std::int64_t sub_key =
                DatumGetInt64(values[k_minimizer_generation_attribute_sub_key - 1]);
            const int stream_index = stream_index_for_sub_key(*header, sub_key);
            if (stream_index >= 0) {
                minimizer_generation_deformed_row deformed{};
                for (std::size_t index = 0; index < deformed.values.size(); ++index) {
                    deformed.values[index] = values[index];
                    deformed.nulls[index] = nulls[index];
                }
                const minimizer_generation_projected_row projected =
                    project_minimizer_generation_deformed_row(deformed);
                if (projected.error != nullptr) {
                    corrupt_row_pg_leaf(header, projected.error);
                }
                const parallel_scan_stream& stream =
                    header->streams[static_cast<std::size_t>(stream_index)];
                const std::uint64_t row_ordinal = projected.address.chunk_no;
                if (projected.query_key != header->query_key ||
                    projected.address.generation_id != header->generation_id ||
                    projected.address.position_segment != 0 ||
                    projected.address.sub_key != stream.sub_key ||
                    row_ordinal >= stream.row_count ||
                    row_ordinal > (std::numeric_limits<std::uint64_t>::max)() /
                                      k_hash_major_minimizer_row_payload_bytes) {
                    corrupt_row_pg_leaf(header, "row identity or ordinal disagrees");
                }
                const std::uint64_t row_offset =
                    row_ordinal * k_hash_major_minimizer_row_payload_bytes;
                if (row_offset >= stream.logical_bytes) {
                    corrupt_row_pg_leaf(header, "row offset exceeds logical stream");
                }
                const std::uint64_t remaining = stream.logical_bytes - row_offset;
                const std::size_t expected_bytes = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, k_hash_major_minimizer_row_payload_bytes));
                if (projected.bytes.size() != expected_bytes ||
                    projected.chunk_bytes != expected_bytes) {
                    corrupt_row_pg_leaf(header, "PLAIN row length disagrees");
                }
                const std::uint64_t expected_checksum =
                    bioseq::indexing::crc64_index_chunk_row(projected.address.generation_id,
                                                            projected.address.position_segment,
                                                            projected.address.sub_key,
                                                            projected.address.chunk_no,
                                                            projected.chunk_bytes,
                                                            projected.bytes);
                if (projected.checksum != expected_checksum) {
                    corrupt_row_pg_leaf(header, "address-bound row CRC disagrees");
                }

                const std::uint64_t global_row = stream.global_row_base + row_ordinal;
                if (global_row >= header->expected_rows) {
                    corrupt_row_pg_leaf(header, "global row ordinal overflows");
                }
                const std::uint64_t word_ordinal = global_row / 64u;
                const std::uint64_t bit = std::uint64_t{1} << (global_row % 64u);
                if (word_ordinal >= header->bitset_word_count) {
                    corrupt_row_pg_leaf(header, "seen-row bitset is undersized");
                }
                const std::uint64_t previous =
                    pg_atomic_fetch_or_u64(&seen_words[word_ordinal], bit);
                if ((previous & bit) != 0u) {
                    duplicate_row_pg_leaf(header, global_row);
                }

                ++local_rows;
                local_bytes += projected.bytes.size();
                local_checksum_digest += mix64(projected.checksum);
                local_receipt_digest +=
                    mix64(projected.checksum ^ mix64(projected.address.generation_id) ^
                          mix64(projected.query_key) ^
                          mix64(static_cast<std::uint64_t>(projected.address.sub_key)) ^
                          mix64(row_ordinal) ^ mix64(projected.chunk_bytes));
            }
        }

        ExecClearTuple(slot);
        if ((visited_rows & 1023u) == 0u) {
            if (!IsParallelWorker() && ParallelMessagePending) {
#if PG_VERSION_NUM >= PG_VERSION_18
                ProcessParallelMessages();
#else
                HandleParallelMessages();
#endif
            }
            CHECK_FOR_INTERRUPTS();
        }
    }

    ExecDropSingleTupleTableSlot(slot);
    (void)pg_atomic_fetch_add_u64(&header->scanned_rows, static_cast<std::int64_t>(local_rows));
    (void)pg_atomic_fetch_add_u64(&header->scanned_bytes, static_cast<std::int64_t>(local_bytes));
    (void)pg_atomic_fetch_add_u64(&header->checksum_digest,
                                  std::bit_cast<std::int64_t>(local_checksum_digest));
    (void)pg_atomic_fetch_add_u64(&header->receipt_digest,
                                  std::bit_cast<std::int64_t>(local_receipt_digest));
}

/// Run the probe through a backend-local table scan.
inline void serial_scan_pg_leaf(Relation relation,
                                Snapshot snapshot,
                                parallel_scan_header* header,
                                pg_atomic_uint64* seen_words) {
    TableScanDesc scan = table_beginscan(relation, snapshot, 0, nullptr
#if PG_VERSION_NUM >= 190000
                                       , SO_NONE
#endif
    );
    scan_table_desc_pg_leaf(relation, scan, header, seen_words);
    table_endscan(scan);
}

/// Join the shared block allocator as a leader or worker.
inline void parallel_scan_pg_leaf(Relation relation,
                                  ParallelTableScanDesc parallel_scan,
                                  parallel_scan_header* header,
                                  pg_atomic_uint64* seen_words) {
    TableScanDesc scan = table_beginscan_parallel(relation, parallel_scan
#if PG_VERSION_NUM >= 190000
                                                , SO_NONE
#endif
    );
    scan_table_desc_pg_leaf(relation, scan, header, seen_words);
    table_endscan(scan);
}

/// Bind shared scan state to the relation and decoded manifest.
inline void initialize_header(parallel_scan_header* header,
                              Relation relation,
                              const hash_major_minimizer_manifest& manifest,
                              std::uint64_t query_key,
                              std::uint32_t requested_workers) {
    if (header == nullptr || relation == nullptr || manifest.generation_id == 0u ||
        !bioseq::indexing::sql_bigint_can_represent(manifest.generation_id) ||
        !bioseq::indexing::sql_bigint_can_represent(query_key)) {
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
    for (std::size_t index = 0; index < header->streams.size(); ++index) {
        const hash_major_minimizer_stream_kind expected_kind =
            hash_major_minimizer_pg_detail::stream_kind(index);
        const auto& descriptor = manifest.streams[index];
        const std::uint64_t expected_rows =
            hash_major_minimizer_pg_detail::ceil_rows(descriptor.logical_bytes);
        if (descriptor.kind != expected_kind || descriptor.row_count != expected_rows ||
            descriptor.row_count >
                static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) ||
            descriptor.row_count > (std::numeric_limits<std::uint64_t>::max)() - row_base ||
            descriptor.logical_bytes > (std::numeric_limits<std::uint64_t>::max)() - total_bytes) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major parallel scan probe stream geometry is invalid");
        }
        header->streams[index] = parallel_scan_stream{
            .sub_key = hash_major_minimizer_pg_detail::stream_sub_key(expected_kind),
            .logical_bytes = descriptor.logical_bytes,
            .row_count = descriptor.row_count,
            .global_row_base = row_base,
        };
        row_base += descriptor.row_count;
        total_bytes += descriptor.logical_bytes;
    }
    header->expected_rows = row_base;
    header->expected_bytes = total_bytes;
    header->bitset_word_count = row_base / 64u + (row_base % 64u != 0u ? 1u : 0u);
    if (header->bitset_word_count == 0u ||
        header->bitset_word_count >
            static_cast<std::uint64_t>(MaxAllocSize / sizeof(pg_atomic_uint64))) {
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
inline void initialize_seen_words(pg_atomic_uint64* words, std::uint64_t word_count) {
    if (words == nullptr || word_count == 0u) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe bitset is invalid");
    }
    for (std::uint64_t index = 0; index < word_count; ++index) {
        pg_atomic_init_u64(&words[index], 0);
    }
}

[[nodiscard]] inline hash_major_minimizer_parallel_scan_stats
finalize_result(parallel_scan_header& header,
                pg_atomic_uint64* seen_words,
                std::uint32_t launched_workers,
                std::uint64_t elapsed_ns) {
    std::uint64_t seen_rows = 0;
    for (std::uint64_t index = 0; index < header.bitset_word_count; ++index) {
        std::uint64_t word = pg_atomic_read_u64(&seen_words[index]);
        if (index + 1u == header.bitset_word_count && header.expected_rows % 64u != 0u) {
            word &= (std::uint64_t{1} << (header.expected_rows % 64u)) - 1u;
        }
        seen_rows += std::popcount(word);
    }
    const std::uint64_t scanned_rows = pg_atomic_read_u64(&header.scanned_rows);
    const std::uint64_t scanned_bytes = pg_atomic_read_u64(&header.scanned_bytes);
    const std::uint64_t duplicates = pg_atomic_read_u64(&header.duplicate_rows);
    const std::uint64_t corrupt = pg_atomic_read_u64(&header.corrupt_rows);
    const std::uint64_t missing =
        seen_rows <= header.expected_rows ? header.expected_rows - seen_rows : header.expected_rows;
    if (duplicates != 0u || corrupt != 0u || missing != 0u || seen_rows != header.expected_rows ||
        scanned_rows != header.expected_rows || scanned_bytes != header.expected_bytes) {
        minimizer_generation_storage_error_as_cpp(
            fmt::format("hash-major parallel scan probe receipt equation failed: "
                        "expected_rows={} seen_rows={} scanned_rows={} "
                        "expected_bytes={} scanned_bytes={} duplicates={} "
                        "missing={} corrupt={}",
                        header.expected_rows,
                        seen_rows,
                        scanned_rows,
                        header.expected_bytes,
                        scanned_bytes,
                        duplicates,
                        missing,
                        corrupt));
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
        .checksum_digest = pg_atomic_read_u64(&header.checksum_digest),
        .receipt_digest = pg_atomic_read_u64(&header.receipt_digest),
        .has_leader_participation = true,
    };
}

[[nodiscard]] inline hash_major_minimizer_parallel_scan_stats
run_probe(Relation relation,
          Snapshot snapshot,
          const hash_major_minimizer_manifest& manifest,
          std::uint64_t query_key,
          std::uint32_t requested_workers) {
    if (relation == nullptr || snapshot == nullptr || !IsMVCCSnapshot(snapshot) ||
        snapshot->regd_count == 0) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe requires a registered MVCC snapshot");
    }

    if (requested_workers == 0u) {
        parallel_scan_header header{};
        initialize_header(&header, relation, manifest, query_key, 0u);
        const Size bitset_bytes =
            static_cast<Size>(header.bitset_word_count * sizeof(pg_atomic_uint64));
        auto* seen_words =
            static_cast<pg_atomic_uint64*>(bioseq::postgres::support::invoke_pg_as_cpp(
                [bitset_bytes] { return palloc0(bitset_bytes); }));
        initialize_seen_words(seen_words, header.bitset_word_count);
        instr_time begin{};
        instr_time end{};
        INSTR_TIME_SET_CURRENT(begin);
        bioseq::postgres::support::invoke_pg_as_cpp([relation, snapshot, &header, seen_words] {
            serial_scan_pg_leaf(relation, snapshot, &header, seen_words);
        });
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, begin);
        hash_major_minimizer_parallel_scan_stats result =
            finalize_result(header, seen_words, 0u, INSTR_TIME_GET_NANOSEC(end));
        bioseq::postgres::support::invoke_pg_as_cpp([seen_words] { pfree(seen_words); });
        return result;
    }

    if (IsParallelWorker() || IsInParallelMode() || ParallelContextActive()) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe cannot nest a parallel context");
    }
    const Size parallel_scan_bytes = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation, snapshot] { return table_parallelscan_estimate(relation, snapshot); });
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
        context = bioseq::postgres::support::invoke_pg_as_cpp([requested_workers] {
            return CreateParallelContext("bioseq",
                                         "bioseq_hash_major_parallel_scan_probe_worker_main",
                                         static_cast<int>(requested_workers));
        });
        if (context == nullptr) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major parallel scan probe could not create a parallel context");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([context, parallel_scan_bytes, &manifest] {
            shm_toc_estimate_chunk(&context->estimator, sizeof(parallel_scan_header));
            shm_toc_estimate_chunk(&context->estimator, parallel_scan_bytes);
            // The exact size becomes known only after the manifest-backed
            // header is initialized. Its upper bound is one atomic word
            // per 64 logical rows.
            std::uint64_t expected_rows = 0;
            for (const auto& descriptor : manifest.streams) {
                if (descriptor.row_count >
                    (std::numeric_limits<std::uint64_t>::max)() - expected_rows) {
                    ereport(ERROR,
                            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                             errmsg("bioseq hash-major parallel scan row count overflows")));
                }
                expected_rows += descriptor.row_count;
            }
            const std::uint64_t word_count =
                expected_rows / 64u + (expected_rows % 64u != 0u ? 1u : 0u);
            if (word_count == 0u ||
                word_count > static_cast<std::uint64_t>(MaxAllocSize / sizeof(pg_atomic_uint64))) {
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bioseq hash-major parallel scan bitset is too large")));
            }
            shm_toc_estimate_chunk(&context->estimator,
                                   static_cast<Size>(word_count * sizeof(pg_atomic_uint64)));
            shm_toc_estimate_keys(&context->estimator, 3);
            InitializeParallelDSM(context);
        });
        if (context->toc == nullptr) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major parallel scan probe has no DSM table of contents");
        }

        auto* header = static_cast<parallel_scan_header*>(
            bioseq::postgres::support::invoke_pg_as_cpp([context] {
                return shm_toc_allocate(context->toc, sizeof(parallel_scan_header));
            }));
        initialize_header(header, relation, manifest, query_key, requested_workers);
        const Size bitset_bytes =
            static_cast<Size>(header->bitset_word_count * sizeof(pg_atomic_uint64));
        auto* parallel_scan = static_cast<ParallelTableScanDesc>(
            bioseq::postgres::support::invoke_pg_as_cpp([context, parallel_scan_bytes] {
                return shm_toc_allocate(context->toc, parallel_scan_bytes);
            }));
        auto* seen_words =
            static_cast<pg_atomic_uint64*>(bioseq::postgres::support::invoke_pg_as_cpp(
                [context, bitset_bytes] { return shm_toc_allocate(context->toc, bitset_bytes); }));
        initialize_seen_words(seen_words, header->bitset_word_count);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, parallel_scan, snapshot, context, header, seen_words] {
                table_parallelscan_initialize(relation, parallel_scan, snapshot);
                shm_toc_insert(context->toc, k_header_toc_key, header);
                shm_toc_insert(context->toc, k_parallel_scan_toc_key, parallel_scan);
                shm_toc_insert(context->toc, k_seen_bitset_toc_key, seen_words);
                LaunchParallelWorkers(context);
                WaitForParallelWorkersToAttach(context);
            });
        const std::uint32_t launched_workers =
            static_cast<std::uint32_t>(std::max(context->nworkers_launched, 0));

        // The leader is an equal consumer of the shared block allocator.
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, parallel_scan, header, seen_words, context] {
                parallel_scan_pg_leaf(relation, parallel_scan, header, seen_words);
                WaitForParallelWorkersToFinish(context);
            });
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, begin);
        hash_major_minimizer_parallel_scan_stats result =
            finalize_result(*header, seen_words, launched_workers, INSTR_TIME_GET_NANOSEC(end));
        bioseq::postgres::support::invoke_pg_as_cpp([context] { DestroyParallelContext(context); });
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

} // namespace bioseq::storage::indexing
