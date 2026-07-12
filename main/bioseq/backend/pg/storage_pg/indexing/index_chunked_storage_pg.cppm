/**
 * @file index_chunked_storage_pg.cppm
 * @brief DOC0279 §8 — the concrete Postgres storage implementations that satisfy
 *        the `is_index_chunked_storage_for<...>` concepts.
 *
 * Index chunks are raw BYTEA tuples in heap relations (heap_form_tuple /
 * CatalogTupleInsert + systable_beginscan), bypassing columnarisation
 * (Invariant C). The per-sequence layouts co-locate with the sequence's
 * `(bucket, shard)`; the table layout shards via `index_table_shard`.
 */
module;
#include "common.h"

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/heapam.h"
#include "catalog/indexing.h"
#include "commands/sequence.h"
#include "catalog/namespace.h"   // RelnameGetRelid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "utils/lsyscache.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
}

#include <array>
#include <atomic>
#include <bit>
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
export module bioseq.storage.indexing_pg.chunked_storage;
import bioseq.common.mlogging;
import bioseq.pg_utils;                       // try_safe_relation_open, find_primary_key_oid
import bioseq.storage.manager;                 // seq_caches, seq_locator, format helpers
import bioseq.storage.bucket_math;             // seq_locator
import bioseq.storage.size;            // sequence_size_t
import bioseq.indexing.type;           // SequenceIndexType, index_sub_key_t, MAX_INDEX_CHUNK_SIZE_BYTES, sentinels
import bioseq.indexing.storage;        // the concepts
import bioseq.indexing.minimizer_hashmap;   // SerializedHeader/Bucket validation
import bioseq.table.io;                // BioseqColumnBeginRead/ReadNextRow/EndRead/ReadState
import bioseq.storage.scanner_pg;      // with_lazy_postgres, make_scanner_pg_in_memory (for_each_sequence)
import bioseq.storage.lazy;            // lazy_compact_sequence
import bioseq.types.pg_compact_sequence;  // pgseq_t, dispatch_instance
import bioseq.sequences;               // compact_sequence_range
import bioseq.sequences.simd;          // canonical bounded base decoding for source proof
import bioseq.parallelism.memory_admission;
import bioseq.storage.indexing_pg.residency;
import bioseq.storage.indexing_pg.sorted_runs;
import bioseq.instr;
import bioseq.column.tam;                    // DOC0322 §7.4 — IsBioseqColumnTableAmTable (leaf)
import bioseq.storage.metadata;                     // DOC0322 §7.4 — BioseqColumnTableRowCount (moved to storage_metadata:core)
import bioseq.storage.physical;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::storage::indexing {

using bioseq::indexing::SequenceIndexType;
using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES;
using bioseq::indexing::TEMP_RUN_BASE;
using bioseq::indexing::POSITION_EXTENT_BASE;
using bioseq::indexing::DIRECTORY_SUB_KEY;
using bioseq::indexing::GENERATION_HEAD_SUB_KEY;
using bioseq::indexing::MinimizerHashmapFragmentIndex;
using bioseq::indexing::MinimizerGenerationHead;
using bioseq::indexing::MinimizerGenerationManifest;
using bioseq::indexing::MinimizerGenerationRowView;
using bioseq::indexing::MinimizerGenerationWireError;
using bioseq::indexing::MinimizerGenerationPayloadProfile;
using bioseq::indexing::ExpectedMinimizerCompatibility;
using bioseq::indexing::EncodedMinimizerGeneration;
using bioseq::indexing::MinimizerBucketDirEntry;
using bioseq::indexing::DirView;
using bioseq::indexing::IndexChunkAddress;
using bioseq::indexing::IndexChunkCachePolicy;
using bioseq::indexing::IndexChunkReadStats;
using bioseq::indexing::IndexSubKeyRange;
using bioseq::indexing::MpsSegmentSpillStorageBinding;
using bioseq::indexing::derive_minimizer_position_extent_sub_key;
using bioseq::indexing::project_minimizer_extent_sub_key;
using bioseq::indexing::IndexSubKeyRequest;
using bioseq::indexing::Crc64Ecma182;
using bioseq::indexing::MINIMIZERS_PER_FRAGMENT_INDEX_NAME;
using bioseq::indexing::MINIMIZERS_PER_SEQUENCE_INDEX_NAME;
using bioseq::indexing::SYMBOL_COUNTS_INDEX_NAME;

// pg-side mirror checks (§5): the macros ARE in scope here.
static_assert(bioseq::indexing::kVarlenaHeaderBytes == VARHDRSZ, "VARHDRSZ mirror drift");
static_assert(bioseq::indexing::kPgMaxAllocSize == MaxAllocSize, "MaxAllocSize mirror drift");

// Backend-local per-sequence build observability (§8.8/DOC0299). Defined here
// (storage) so merge/release can set it; admin zeroes it at build start and
// mod.cppm reads it through SQL in the same backend.
struct LastMpsBuildStats {
    std::uint64_t backend_pid = 0;
    std::uint64_t db_oid = 0;
    std::uint64_t relfilenode = 0;
    std::uint64_t sequence_rowno = 0;
    std::uint64_t total_run_count = 0;
    std::uint64_t resident_runs = 0;
    std::uint64_t file_runs = 0;
    std::uint64_t resident_run_bytes = 0;
    std::uint64_t resident_peak_bytes = 0;
    std::uint64_t spilled_run_bytes = 0;
    std::uint64_t postgres_temp_file_bytes = 0;
    std::uint64_t postgres_temp_file_count = 0;
    std::uint64_t merge_reader_buffer_bytes = 0;
    std::uint64_t merge_pass_count = 0;
    std::uint64_t spill_count_from_pressure = 0;
    std::uint64_t resident_demotions = 0;
    std::uint64_t position_spill_bytes = 0;
    std::uint64_t position_spill_count = 0;
};

inline std::uint64_t g_last_index_merge_run_count = 0;
inline LastMpsBuildStats g_last_mps_build_stats{};

inline void reset_last_mps_build_stats() noexcept {
    g_last_index_merge_run_count = 0;
    g_last_mps_build_stats = {};
}

enum class Rev3PgFailureInjection : std::uint8_t {
    none,
    mps_spill,
    mps_merge,
    generation_row_write,
};

#ifdef BIOSEQ_TEST_INDEXES
inline Rev3PgFailureInjection g_rev3_pg_failure_injection =
    Rev3PgFailureInjection::none;

inline void arm_rev3_pg_failure_injection(
        Rev3PgFailureInjection point) noexcept {
    g_rev3_pg_failure_injection = point;
}

inline void inject_rev3_pg_failure_as_cpp(
        Rev3PgFailureInjection point,
        const char* point_name) {
    if (g_rev3_pg_failure_injection != point) {
        return;
    }
    // Consume before ERROR so a fresh statement in the same backend proves
    // recovery instead of repeatedly tripping the test hook.
    g_rev3_pg_failure_injection = Rev3PgFailureInjection::none;
    bioseq::pg_utils::invoke_pg_as_cpp([&] {
        ereport(ERROR,
            (errcode(ERRCODE_IO_ERROR),
             errmsg("injected revision-3 PostgreSQL failure"),
             errdetail("failure_point=%s", point_name)));
    });
}
#else
inline void arm_rev3_pg_failure_injection(
        Rev3PgFailureInjection) noexcept {}
inline void inject_rev3_pg_failure_as_cpp(
        Rev3PgFailureInjection,
        const char*) {}
#endif

// Pinned attribute numbers (§8.1) for the CHUNKED_PER_FRAGMENT table.
inline constexpr AttrNumber kAttQueryKey   = 1;
inline constexpr AttrNumber kAttSubKey     = 2;
inline constexpr AttrNumber kAttChunkNo    = 3;
inline constexpr AttrNumber kAttChunkBytes = 4;
inline constexpr AttrNumber kAttBlob       = 5;

// Revision-3 lives in this module as an inactive, fail-closed surface until the
// destructive schema cutover is made atomically with every live writer/reader.
// The legacy five-column constants above deliberately remain unchanged.
inline constexpr AttrNumber kRev3AttQueryKey       = 1;
inline constexpr AttrNumber kRev3AttGenerationId   = 2;
inline constexpr AttrNumber kRev3AttPositionSegment = 3;
inline constexpr AttrNumber kRev3AttSubKey         = 4;
inline constexpr AttrNumber kRev3AttChunkNo        = 5;
inline constexpr AttrNumber kRev3AttChunkBytes     = 6;
inline constexpr AttrNumber kRev3AttChecksum       = 7;
inline constexpr AttrNumber kRev3AttBlob           = 8;

struct Rev3IndexAttributeDescriptor {
    AttrNumber attno = InvalidAttrNumber;
    std::string_view name;
    Oid type_oid = InvalidOid;
};

inline constexpr std::array<Rev3IndexAttributeDescriptor, 8>
kRev3IndexAttributeDescriptors{{
    {kRev3AttQueryKey, "query_key", INT8OID},
    {kRev3AttGenerationId, "generation_id", INT8OID},
    {kRev3AttPositionSegment, "position_segment", INT4OID},
    {kRev3AttSubKey, "sub_key", INT8OID},
    {kRev3AttChunkNo, "chunk_no", INT4OID},
    {kRev3AttChunkBytes, "chunk_bytes", INT4OID},
    {kRev3AttChecksum, "checksum", INT8OID},
    {kRev3AttBlob, "blob", BYTEAOID},
}};

inline constexpr std::array<AttrNumber, 5> kRev3IndexPrimaryKeyAttributes{
    kRev3AttQueryKey,
    kRev3AttGenerationId,
    kRev3AttPositionSegment,
    kRev3AttSubKey,
    kRev3AttChunkNo,
};

static_assert(kRev3IndexAttributeDescriptors.front().attno == 1);
static_assert(kRev3IndexAttributeDescriptors.back().attno == 8);
static_assert(kRev3IndexPrimaryKeyAttributes.size() == 5);

/// Preserve all CRC-64 bits in PostgreSQL's signed BIGINT container.
[[nodiscard]] constexpr std::int64_t checksum_to_pg_bigint(
        std::uint64_t checksum) noexcept {
    return std::bit_cast<std::int64_t>(checksum);
}

/// Recover the exact CRC-64 word from PostgreSQL's signed BIGINT container.
[[nodiscard]] constexpr std::uint64_t checksum_from_pg_bigint(
        std::int64_t checksum) noexcept {
    return std::bit_cast<std::uint64_t>(checksum);
}

static_assert(checksum_from_pg_bigint(checksum_to_pg_bigint(0)) == 0);
static_assert(checksum_from_pg_bigint(checksum_to_pg_bigint(
    (std::numeric_limits<std::uint64_t>::max)()))
    == (std::numeric_limits<std::uint64_t>::max)());
static_assert(checksum_to_pg_bigint(std::uint64_t{1} << 63u)
    == (std::numeric_limits<std::int64_t>::min)());

enum class Rev3IndexSchemaError : std::uint8_t {
    none,
    not_heap,
    attribute_count,
    attribute_number,
    attribute_dropped,
    attribute_name,
    attribute_type,
    attribute_nullable,
    blob_not_plain,
    primary_index_count,
    primary_index_not_unique,
    primary_index_not_immediate,
    primary_index_not_ready,
    primary_index_key_count,
    primary_index_key_order,
    primary_index_not_btree,
    primary_index_partial,
    primary_index_expression,
    primary_index_opclass,
    primary_index_collation,
    primary_index_options,
};

struct Rev3IndexSchemaInspection {
    Rev3IndexSchemaError error = Rev3IndexSchemaError::none;
    Oid primary_index_oid = InvalidOid;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == Rev3IndexSchemaError::none
            && OidIsValid(primary_index_oid);
    }
};

[[noreturn]] inline void rev3_storage_error(std::string_view message);

[[nodiscard]] constexpr std::string_view rev3_index_schema_error_name(
        Rev3IndexSchemaError error) noexcept {
    switch (error) {
        case Rev3IndexSchemaError::none: return "none";
        case Rev3IndexSchemaError::not_heap: return "relation is not heap AM";
        case Rev3IndexSchemaError::attribute_count: return "attribute count";
        case Rev3IndexSchemaError::attribute_number: return "attribute number";
        case Rev3IndexSchemaError::attribute_dropped: return "dropped attribute";
        case Rev3IndexSchemaError::attribute_name: return "attribute name";
        case Rev3IndexSchemaError::attribute_type: return "attribute type";
        case Rev3IndexSchemaError::attribute_nullable: return "nullable attribute";
        case Rev3IndexSchemaError::blob_not_plain: return "blob storage is not PLAIN";
        case Rev3IndexSchemaError::primary_index_count: return "primary index count";
        case Rev3IndexSchemaError::primary_index_not_unique: return "primary index is not unique";
        case Rev3IndexSchemaError::primary_index_not_immediate: return "primary index is deferred";
        case Rev3IndexSchemaError::primary_index_not_ready: return "primary index is not ready/valid/live";
        case Rev3IndexSchemaError::primary_index_key_count: return "primary index key count";
        case Rev3IndexSchemaError::primary_index_key_order: return "primary index key order";
        case Rev3IndexSchemaError::primary_index_not_btree: return "primary index is not btree";
        case Rev3IndexSchemaError::primary_index_partial: return "primary index is partial";
        case Rev3IndexSchemaError::primary_index_expression: return "primary index has expressions";
        case Rev3IndexSchemaError::primary_index_opclass: return "primary index opclass";
        case Rev3IndexSchemaError::primary_index_collation: return "primary index collation";
        case Rev3IndexSchemaError::primary_index_options: return "primary index ordering/opclass options";
    }
    return "unknown";
}

/// Inspect the complete revision-3 relation shape without accepting attno
/// shims, included PK columns, nullable fields, or a toastable blob.
[[nodiscard]] inline Rev3IndexSchemaInspection inspect_rev3_index_schema(
        Relation relation) {
    if (relation == nullptr || relation->rd_rel->relam != HEAP_TABLE_AM_OID) {
        return {.error = Rev3IndexSchemaError::not_heap};
    }
    TupleDesc tuple_desc = RelationGetDescr(relation);
    if (tuple_desc == nullptr
        || tuple_desc->natts
            != static_cast<int>(kRev3IndexAttributeDescriptors.size())) {
        return {.error = Rev3IndexSchemaError::attribute_count};
    }
    for (std::size_t index = 0;
         index < kRev3IndexAttributeDescriptors.size(); ++index) {
        const Rev3IndexAttributeDescriptor& expected =
            kRev3IndexAttributeDescriptors[index];
        Form_pg_attribute attribute = TupleDescAttr(
            tuple_desc, static_cast<int>(index));
        if (attribute->attnum != expected.attno) {
            return {.error = Rev3IndexSchemaError::attribute_number};
        }
        if (attribute->attisdropped) {
            return {.error = Rev3IndexSchemaError::attribute_dropped};
        }
        if (std::string_view{NameStr(attribute->attname)} != expected.name) {
            return {.error = Rev3IndexSchemaError::attribute_name};
        }
        if (attribute->atttypid != expected.type_oid) {
            return {.error = Rev3IndexSchemaError::attribute_type};
        }
        if (!attribute->attnotnull) {
            return {.error = Rev3IndexSchemaError::attribute_nullable};
        }
        if (expected.attno == kRev3AttBlob
            && attribute->attstorage != TYPSTORAGE_PLAIN) {
            return {.error = Rev3IndexSchemaError::blob_not_plain};
        }
    }

    Oid primary_index_oid = InvalidOid;
    std::uint32_t primary_index_count = 0;
    Rev3IndexSchemaError primary_error = Rev3IndexSchemaError::none;
    List* index_list = RelationGetIndexList(relation);
    ListCell* cell = nullptr;
    foreach (cell, index_list) {
        const Oid index_oid = lfirst_oid(cell);
        Relation index_relation = index_open(index_oid, AccessShareLock);
        if (index_relation->rd_index != nullptr
            && index_relation->rd_index->indisprimary) {
            ++primary_index_count;
            primary_index_oid = index_oid;
            const Form_pg_index index_form = index_relation->rd_index;
            if (!index_form->indisunique) {
                primary_error = Rev3IndexSchemaError::primary_index_not_unique;
            } else if (!index_form->indimmediate) {
                primary_error = Rev3IndexSchemaError::primary_index_not_immediate;
            } else if (!index_form->indisvalid || !index_form->indisready
                       || !index_form->indislive) {
                primary_error = Rev3IndexSchemaError::primary_index_not_ready;
            } else if (index_form->indnatts
                           != static_cast<std::int16_t>(
                               kRev3IndexPrimaryKeyAttributes.size())
                       || index_form->indnkeyatts
                           != static_cast<std::int16_t>(
                               kRev3IndexPrimaryKeyAttributes.size())) {
                primary_error = Rev3IndexSchemaError::primary_index_key_count;
            } else if (index_relation->rd_rel->relam != BTREE_AM_OID) {
                primary_error = Rev3IndexSchemaError::primary_index_not_btree;
            } else if (RelationGetIndexPredicate(index_relation) != NIL) {
                primary_error = Rev3IndexSchemaError::primary_index_partial;
            } else if (RelationGetIndexExpressions(index_relation) != NIL) {
                primary_error = Rev3IndexSchemaError::primary_index_expression;
            } else {
                constexpr std::array<Oid, 5> expected_opclasses{
                    INT8_BTREE_OPS_OID,
                    INT8_BTREE_OPS_OID,
                    INT4_BTREE_OPS_OID,
                    INT8_BTREE_OPS_OID,
                    INT4_BTREE_OPS_OID,
                };
                for (std::size_t key = 0;
                     key < kRev3IndexPrimaryKeyAttributes.size(); ++key) {
                    if (index_form->indkey.values[key]
                        != kRev3IndexPrimaryKeyAttributes[key]) {
                        primary_error = Rev3IndexSchemaError::primary_index_key_order;
                        break;
                    }
                    if (get_index_column_opclass(
                            index_oid, static_cast<int>(key + 1u))
                        != expected_opclasses[key]) {
                        primary_error = Rev3IndexSchemaError::primary_index_opclass;
                        break;
                    }
                    if (index_relation->rd_indcollation == nullptr
                        || OidIsValid(index_relation->rd_indcollation[key])) {
                        primary_error = Rev3IndexSchemaError::primary_index_collation;
                        break;
                    }
                    if (index_relation->rd_indoption == nullptr
                        || index_relation->rd_indoption[key] != 0
                        || (index_relation->rd_opcoptions != nullptr
                            && index_relation->rd_opcoptions[key] != nullptr)) {
                        primary_error = Rev3IndexSchemaError::primary_index_options;
                        break;
                    }
                }
            }
        }
        index_close(index_relation, AccessShareLock);
    }
    list_free(index_list);
    if (primary_index_count != 1) {
        return {.error = Rev3IndexSchemaError::primary_index_count};
    }
    if (primary_error != Rev3IndexSchemaError::none) {
        return {.error = primary_error};
    }
    return {
        .error = Rev3IndexSchemaError::none,
        .primary_index_oid = primary_index_oid,
    };
}

/// Integration-only catalog corruption seam. It copies a real parsed
/// predicate or expression tree from a donor index onto the otherwise-valid
/// primary index row. Tests execute it inside a transaction and roll back,
/// proving PG17's varlena `indpred`/`indexprs` path rather than fabricating a
/// C++ Relation object.
inline void rev3_test_copy_primary_index_shape_pg(
        Oid table_oid,
        Oid donor_index_oid,
        bool copy_predicate) {
    if (!superuser()) {
        rev3_storage_error("rev3 index-shape corruption requires superuser");
    }
    Relation target = try_safe_relation_open(table_oid, AccessShareLock);
    if (target == nullptr) {
        rev3_storage_error("rev3 index-shape target relation is missing");
    }
    const Rev3IndexSchemaInspection schema = inspect_rev3_index_schema(target);
    relation_close(target, AccessShareLock);
    if (!schema) {
        rev3_storage_error("rev3 index-shape target schema is not initially valid");
    }
    Relation donor = try_safe_relation_open(donor_index_oid, AccessShareLock);
    if (donor == nullptr || donor->rd_index == nullptr
        || donor->rd_index->indrelid != table_oid
        || (copy_predicate
            ? RelationGetIndexPredicate(donor) == NIL
            : RelationGetIndexExpressions(donor) == NIL)) {
        if (donor != nullptr) relation_close(donor, AccessShareLock);
        rev3_storage_error("rev3 index-shape donor does not carry the requested shape");
    }
    relation_close(donor, AccessShareLock);

    Relation catalog = table_open(IndexRelationId, RowExclusiveLock);
    HeapTuple primary_tuple = SearchSysCacheCopy1(
        INDEXRELID, ObjectIdGetDatum(schema.primary_index_oid));
    HeapTuple donor_tuple = SearchSysCache1(
        INDEXRELID, ObjectIdGetDatum(donor_index_oid));
    if (!HeapTupleIsValid(primary_tuple) || !HeapTupleIsValid(donor_tuple)) {
        if (HeapTupleIsValid(primary_tuple)) heap_freetuple(primary_tuple);
        if (HeapTupleIsValid(donor_tuple)) ReleaseSysCache(donor_tuple);
        table_close(catalog, RowExclusiveLock);
        rev3_storage_error("rev3 index-shape catalog tuple is missing");
    }
    const AttrNumber shape_attno = copy_predicate
        ? Anum_pg_index_indpred : Anum_pg_index_indexprs;
    bool donor_is_null = false;
    const Datum donor_shape = heap_getattr(
        donor_tuple, shape_attno, RelationGetDescr(catalog), &donor_is_null);
    if (donor_is_null) {
        heap_freetuple(primary_tuple);
        ReleaseSysCache(donor_tuple);
        table_close(catalog, RowExclusiveLock);
        rev3_storage_error("rev3 index-shape donor catalog value is NULL");
    }
    std::array<Datum, Natts_pg_index> values{};
    std::array<bool, Natts_pg_index> nulls{};
    std::array<bool, Natts_pg_index> replaces{};
    values[shape_attno - 1] = datumCopy(donor_shape, false, -1);
    replaces[shape_attno - 1] = true;
    HeapTuple modified = heap_modify_tuple(
        primary_tuple, RelationGetDescr(catalog),
        values.data(), nulls.data(), replaces.data());
    CatalogTupleUpdate(catalog, &primary_tuple->t_self, modified);
    heap_freetuple(modified);
    pfree(DatumGetPointer(values[shape_attno - 1]));
    heap_freetuple(primary_tuple);
    ReleaseSysCache(donor_tuple);
    table_close(catalog, RowExclusiveLock);
    CommandCounterIncrement();
    CacheInvalidateRelcacheByRelid(table_oid);
    CacheInvalidateRelcacheByRelid(schema.primary_index_oid);
}

// ---- naming helpers ------------------------------------------------------

/// Compile-time-ish abbreviation for an index name (keeps the physical name
/// inside NAMEDATALEN). Unknown names fall back to a stable short hash.
[[nodiscard]] inline std::string short_index_for(std::string_view name) {
    if (name == MINIMIZERS_PER_FRAGMENT_INDEX_NAME) return "mpf";
    if (name == MINIMIZERS_PER_SEQUENCE_INDEX_NAME) return "mps";
    if (name == SYMBOL_COUNTS_INDEX_NAME)           return "sc";
    if (name == "test_table_rowcount")     return "ttr";
    // generic fallback: first 4 chars + length, still deterministic
    std::string out = "ix";
    for (char c : name) { if (out.size() >= 6) break; if ((c>='a'&&c<='z')||(c>='0'&&c<='9')) out.push_back(c); }
    return out;
}

/// Stable 64-bit FNV-1a over the logical identity tuple, hex-rendered.
[[nodiscard]] inline std::uint64_t identity_hash64(Oid db, Oid spc, Oid relfn,
                                                   std::int16_t attno, std::string_view name) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v){ for (int i=0;i<8;++i){ h ^= (v & 0xff); h *= 1099511628211ull; v >>= 8; } };
    mix(db); mix(spc); mix(relfn); mix((std::uint64_t)(std::uint16_t)attno);
    for (char c : name) { h ^= (std::uint8_t)c; h *= 1099511628211ull; }
    return h;
}

[[nodiscard]] inline std::string format_seqidx_name(std::string_view name, Oid db, Oid spc, Oid relfn,
        std::int16_t attno, std::uint8_t bucket, std::uint32_t shard) {
    std::string out = fmt::format("seqidx_{}_{:016x}_{}_{}", short_index_for(name),
                                  identity_hash64(db, spc, relfn, attno, name),
                                  (unsigned)bucket, (unsigned)shard);
    if (out.size() >= NAMEDATALEN) bioseq_log<ERR, PG>( "bioseq index: derived name too long ({}): {}", out.size(), out);
    return out;
}
[[nodiscard]] inline std::string format_seqidxtab_name(std::string_view name, Oid db, Oid spc, Oid relfn,
        std::int16_t attno, std::int32_t shard) {
    std::string out = fmt::format("seqidxtab_{}_{:016x}_{}", short_index_for(name),
                                  identity_hash64(db, spc, relfn, attno, name), shard);
    if (out.size() >= NAMEDATALEN) bioseq_log<ERR, PG>( "bioseq index: derived name too long ({}): {}", out.size(), out);
    return out;
}

[[nodiscard]] inline Oid bioseq_internal_ns() { return get_namespace_oid("bioseq_internal", false); }

/// Run a DDL string inside an internal sub-transaction (matches
/// sequence_storage_manager's lazy-CREATE pattern). Returns nothing; throws on error.
inline void run_internal_ddl(const char* ddl) {
    MemoryContext saved_ctx   = CurrentMemoryContext;
    ResourceOwner saved_owner = CurrentResourceOwner;
    BeginInternalSubTransaction("bioseq index DDL");
    MemoryContextSwitchTo(saved_ctx);
    PG_TRY();
    {
        if (SPI_connect() != SPI_OK_CONNECT) bioseq_log<ERR, PG>( "bioseq index: SPI_connect failed");
        const int rc = SPI_execute(ddl, /*read_only=*/false, /*tcount=*/0);
        if (rc != SPI_OK_UTILITY && rc != SPI_OK_SELECT)
            bioseq_log<ERR, PG>( "bioseq index: DDL failed (SPI rc={})", rc);
        SPI_finish();
        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(saved_ctx);
        CurrentResourceOwner = saved_owner;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_ctx);
        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(saved_ctx);
        CurrentResourceOwner = saved_owner;
        PG_RE_THROW();
    }
    PG_END_TRY();
}

/// Persistence keyword inherited from the user relation (UNLOGGED if parent is).
[[nodiscard]] inline const char* persistence_kw_for(Oid user_relid) {
    char persist = 'p';
    Relation r = try_safe_relation_open(user_relid, AccessShareLock);
    if (r != nullptr) { persist = r->rd_rel->relpersistence; relation_close(r, AccessShareLock); }
    return persist == 'u' ? "UNLOGGED" : "";
}

[[noreturn]] inline void rev3_storage_error(std::string_view message) {
    ereport(ERROR,
        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
         errmsg("bioseq revision-3 index: %.*s",
            static_cast<int>(message.size()), message.data())));
    pg_unreachable();
}

[[noreturn]] inline void rev3_storage_error_as_cpp(
        std::string_view message) {
    bioseq::pg_utils::invoke_pg_as_cpp(
        [&] { rev3_storage_error(message); });
    std::terminate();
}

/// Stable administration-lock key shared by the legacy administrator and the
/// future revision-3 reset/publication path. The hash is intentionally byte-for-
/// byte identical to the pre-existing admin helper.
[[nodiscard]] inline std::uint64_t index_advisory_xact_lock_key(
        Oid db,
        Oid tablespace,
        Oid relfilenode,
        std::int16_t attno,
        std::string_view index_name) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash ^= 0xB10D2790u;
    const auto mix_word = [&hash](std::uint64_t value) noexcept {
        for (std::uint32_t byte = 0; byte < 8; ++byte) {
            hash ^= value & 0xffu;
            hash *= 1099511628211ULL;
            value >>= 8u;
        }
    };
    mix_word(db);
    mix_word(tablespace);
    mix_word(relfilenode);
    mix_word(static_cast<std::uint16_t>(attno));
    for (char byte : index_name) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// Acquire the one existing transaction-scoped logical-index administration
/// lock. Read sessions deliberately never call this helper.
inline void acquire_index_advisory_xact_lock(
        Oid db,
        Oid tablespace,
        Oid relfilenode,
        std::int16_t attno,
        std::string_view index_name) {
    const std::int64_t signed_key = std::bit_cast<std::int64_t>(
        index_advisory_xact_lock_key(
            db, tablespace, relfilenode, attno, index_name));
    DirectFunctionCall1(pg_advisory_xact_lock_int8, Int64GetDatum(signed_key));
}

inline constexpr std::string_view kMinimizerGenerationSequenceName =
    "minimizer_generation_id_seq";

enum class MinimizerGenerationSequenceError : std::uint8_t {
    none,
    missing,
    wrong_namespace,
    wrong_relkind,
    wrong_persistence,
    missing_catalog_row,
    wrong_type,
    wrong_bounds,
    cycling,
};

[[nodiscard]] constexpr std::string_view
minimizer_generation_sequence_error_name(
        MinimizerGenerationSequenceError error) noexcept {
    switch (error) {
        case MinimizerGenerationSequenceError::none: return "none";
        case MinimizerGenerationSequenceError::missing: return "missing";
        case MinimizerGenerationSequenceError::wrong_namespace: return "wrong namespace";
        case MinimizerGenerationSequenceError::wrong_relkind: return "not a sequence";
        case MinimizerGenerationSequenceError::wrong_persistence: return "not permanent";
        case MinimizerGenerationSequenceError::missing_catalog_row: return "missing pg_sequence row";
        case MinimizerGenerationSequenceError::wrong_type: return "not BIGINT";
        case MinimizerGenerationSequenceError::wrong_bounds: return "wrong positive non-wrapping bounds";
        case MinimizerGenerationSequenceError::cycling: return "cycles";
    }
    return "unknown";
}

struct MinimizerGenerationSequenceInspection {
    MinimizerGenerationSequenceError error =
        MinimizerGenerationSequenceError::none;
    Oid sequence_oid = InvalidOid;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == MinimizerGenerationSequenceError::none
            && OidIsValid(sequence_oid);
    }
};

/// Validate the SQL installation object's exact non-wrapping BIGINT contract.
[[nodiscard]] inline MinimizerGenerationSequenceInspection
inspect_minimizer_generation_sequence_contract() {
    const Oid internal_namespace = bioseq_internal_ns();
    const Oid sequence_oid = get_relname_relid(
        kMinimizerGenerationSequenceName.data(), internal_namespace);
    if (!OidIsValid(sequence_oid)) {
        return {.error = MinimizerGenerationSequenceError::missing};
    }
    Relation sequence_relation = try_safe_relation_open(
        sequence_oid, AccessShareLock);
    if (sequence_relation == nullptr) {
        return {.error = MinimizerGenerationSequenceError::missing};
    }
    MinimizerGenerationSequenceError error =
        MinimizerGenerationSequenceError::none;
    if (RelationGetNamespace(sequence_relation) != internal_namespace) {
        error = MinimizerGenerationSequenceError::wrong_namespace;
    } else if (sequence_relation->rd_rel->relkind != RELKIND_SEQUENCE) {
        error = MinimizerGenerationSequenceError::wrong_relkind;
    } else if (sequence_relation->rd_rel->relpersistence
               != RELPERSISTENCE_PERMANENT) {
        error = MinimizerGenerationSequenceError::wrong_persistence;
    }
    relation_close(sequence_relation, AccessShareLock);
    if (error != MinimizerGenerationSequenceError::none) {
        return {.error = error};
    }
    HeapTuple sequence_tuple = SearchSysCache1(
        SEQRELID, ObjectIdGetDatum(sequence_oid));
    if (!HeapTupleIsValid(sequence_tuple)) {
        return {.error =
            MinimizerGenerationSequenceError::missing_catalog_row};
    }
    const Form_pg_sequence sequence =
        reinterpret_cast<Form_pg_sequence>(GETSTRUCT(sequence_tuple));
    if (sequence->seqtypid != INT8OID) {
        error = MinimizerGenerationSequenceError::wrong_type;
    } else if (sequence->seqincrement != 1
               || sequence->seqmin != 1
               || sequence->seqmax
                    != (std::numeric_limits<std::int64_t>::max)()
               || sequence->seqstart != 1
               || sequence->seqcache != 1) {
        error = MinimizerGenerationSequenceError::wrong_bounds;
    } else if (sequence->seqcycle) {
        error = MinimizerGenerationSequenceError::cycling;
    }
    ReleaseSysCache(sequence_tuple);
    return {.error = error, .sequence_oid =
        error == MinimizerGenerationSequenceError::none
            ? sequence_oid : InvalidOid};
}

/// Allocate a positive signed-SQL generation ID. This helper never creates the
/// sequence; installation/cutover owns that atomic DDL step.
[[nodiscard]] inline std::uint64_t allocate_minimizer_generation_id_pg() {
    const MinimizerGenerationSequenceInspection sequence =
        inspect_minimizer_generation_sequence_contract();
    if (!sequence) {
        rev3_storage_error(fmt::format(
            "owned generation sequence contract mismatch: {}",
            minimizer_generation_sequence_error_name(sequence.error)));
    }
    const std::int64_t generation_id = nextval_internal(
        sequence.sequence_oid, false);
    if (generation_id <= 0) {
        rev3_storage_error("generation sequence exhausted its positive BIGINT range");
    }
    return static_cast<std::uint64_t>(generation_id);
}

/// Validate one revision-3 address and its row checksum before a PostgreSQL
/// tuple is formed. The active-head row is never accepted by the immutable
/// generation sink and has a separate atomic publication helper below.
[[nodiscard]] inline std::expected<void, MinimizerGenerationWireError>
validate_rev3_generation_row_for_write(
        std::uint64_t expected_generation_id,
        const MinimizerGenerationRowView& row) noexcept {
    if (expected_generation_id == 0
        || !bioseq::indexing::sql_bigint_can_represent(
            expected_generation_id)
        || row.address.generation_id != expected_generation_id) {
        return std::unexpected(MinimizerGenerationWireError::generation_id);
    }
    if (row.bytes.empty()
        || row.bytes.size()
            > static_cast<std::size_t>(
                (std::numeric_limits<std::int32_t>::max)())
        || !bioseq::indexing::sql_integer_can_represent(
            row.address.chunk_no)) {
        return std::unexpected(MinimizerGenerationWireError::byte_count);
    }
    if (row.address.position_segment == -1) {
        if (row.address.sub_key != DIRECTORY_SUB_KEY) {
            return std::unexpected(MinimizerGenerationWireError::sub_key);
        }
        if ((row.address.chunk_no == 0
                && row.bytes.size()
                    != MinimizerGenerationManifest::encoded_bytes)
            || (row.address.chunk_no != 0
                && row.bytes.size()
                    > bioseq::indexing::kMinimizerExtentPayloadBytes)) {
            return std::unexpected(MinimizerGenerationWireError::byte_count);
        }
    } else if (row.address.position_segment < 0) {
        return std::unexpected(MinimizerGenerationWireError::position_segment);
    } else if (row.address.sub_key < 0
               || row.address.sub_key >= TEMP_RUN_BASE
               || row.bytes.size()
                    > bioseq::indexing::kMinimizerExtentPayloadBytes) {
        return std::unexpected(MinimizerGenerationWireError::sub_key);
    }
    const std::uint64_t expected_checksum =
        bioseq::indexing::crc64_index_chunk_row(
            row.address.generation_id,
            row.address.position_segment,
            row.address.sub_key,
            row.address.chunk_no,
            static_cast<std::uint32_t>(row.bytes.size()),
            row.bytes);
    if (row.row_checksum != expected_checksum) {
        return std::unexpected(MinimizerGenerationWireError::checksum);
    }
    return {};
}

struct Rev3IndexAdministrationIdentity {
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    Oid user_relid = InvalidOid;
    std::int16_t column_attno = 0;
    std::string index_name;
    std::string table_name;

    void release_owned_strings() noexcept {
        std::string{}.swap(index_name);
        std::string{}.swap(table_name);
    }
};

/// Immutable identity of the exact visible source row used by revision-3.
/// `compatibility_checksum` is filled only after the independently decoded
/// manifest has been checked against the source row and active compatibility
/// axes; no head/manifest digest is copied into this object.
struct Rev3AuthoritativeSourceLocator {
    std::uint64_t sequence_rowno = 0;
    seq_locator locator{};
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t padding_letters = 0;
    std::uint64_t compatibility_checksum = 0;

    [[nodiscard]] bool operator==(
            const Rev3AuthoritativeSourceLocator& other) const noexcept {
        return sequence_rowno == other.sequence_rowno
            && locator.version == other.locator.version
            && locator.bucket_idx == other.locator.bucket_idx
            && locator._pad_2 == other.locator._pad_2
            && locator.shard_idx == other.locator.shard_idx
            && locator.num_fragments == other.locator.num_fragments
            && locator._pad_4 == other.locator._pad_4
            && locator.total_bp_size == other.locator.total_bp_size
            && locator.first_sub_row_no == other.locator.first_sub_row_no
            && locator.source_epoch == other.locator.source_epoch
            && alphabet_code == other.alphabet_code
            && storage_unit_byte_width == other.storage_unit_byte_width
            && padding_letters == other.padding_letters
            && compatibility_checksum == other.compatibility_checksum;
    }
};

[[nodiscard]] constexpr bool rev3_same_seq_locator(
        const seq_locator& left,
        const seq_locator& right) noexcept {
    return left.version == right.version
        && left.bucket_idx == right.bucket_idx
        && left._pad_2 == right._pad_2
        && left.shard_idx == right.shard_idx
        && left.num_fragments == right.num_fragments
        && left._pad_4 == right._pad_4
        && left.total_bp_size == right.total_bp_size
        && left.first_sub_row_no == right.first_sub_row_no
        && left.source_epoch == right.source_epoch;
}

/// Canonical locator-derived identity. Base content is deliberately a second,
/// independently streamed proof so a same-length mutation cannot be hidden by
/// a self-consistent rewrite of the head and manifest.
[[nodiscard]] constexpr std::uint64_t rev3_source_identity(
        Oid /*main_relfilenode*/,
        const Rev3AuthoritativeSourceLocator& source) noexcept {
    return source.locator.source_epoch;
}

class IndexStorageFragmentGenerationRowSinkPg;
class IndexStorageFragmentGenerationReadSessionPg;

/// Unforgeable PostgreSQL-owned proof derived from the exact visible main-row
/// datum and, for a locator stub, every referenced fragment row under one
/// registered MVCC snapshot. Only the derivation helper can construct it. The
/// relation/query/snapshot binding prevents replay into another read session;
/// consumption is move-only and invalidates the token.
class Rev3AuthoritativeSourceProof {
private:
    Rev3AuthoritativeSourceLocator source_{};
    std::uint64_t source_content_checksum_ = 0;
    Oid database_oid_ = InvalidOid;
    Oid tablespace_oid_ = InvalidOid;
    Oid main_relfilenode_ = InvalidOid;
    Oid user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string index_name_{};
    std::string table_name_{};
    Snapshot snapshot_ = nullptr;
    bool valid_ = false;

    Rev3AuthoritativeSourceProof() = default;

    void invalidate_() noexcept {
        source_ = {};
        source_content_checksum_ = 0;
        database_oid_ = InvalidOid;
        tablespace_oid_ = InvalidOid;
        main_relfilenode_ = InvalidOid;
        user_relid_ = InvalidOid;
        column_attno_ = 0;
        std::string{}.swap(index_name_);
        std::string{}.swap(table_name_);
        snapshot_ = nullptr;
        valid_ = false;
    }

    friend Rev3AuthoritativeSourceProof
    derive_rev3_authoritative_source_proof_pg(
        const Rev3IndexAdministrationIdentity&,
        std::uint64_t,
        Snapshot);
    friend class IndexStorageFragmentGenerationRowSinkPg;
    friend class IndexStorageFragmentGenerationReadSessionPg;

public:
    Rev3AuthoritativeSourceProof(
        const Rev3AuthoritativeSourceProof&) = delete;
    Rev3AuthoritativeSourceProof& operator=(
        const Rev3AuthoritativeSourceProof&) = delete;
    Rev3AuthoritativeSourceProof(
            Rev3AuthoritativeSourceProof&& other) noexcept
        : source_(other.source_),
          source_content_checksum_(other.source_content_checksum_),
          database_oid_(other.database_oid_),
          tablespace_oid_(other.tablespace_oid_),
          main_relfilenode_(other.main_relfilenode_),
          user_relid_(other.user_relid_),
          column_attno_(other.column_attno_),
          index_name_(std::move(other.index_name_)),
          table_name_(std::move(other.table_name_)),
          snapshot_(other.snapshot_),
          valid_(other.valid_) {
        other.invalidate_();
    }
    Rev3AuthoritativeSourceProof& operator=(
            Rev3AuthoritativeSourceProof&& other) noexcept {
        if (this != &other) {
            invalidate_();
            source_ = other.source_;
            source_content_checksum_ = other.source_content_checksum_;
            database_oid_ = other.database_oid_;
            tablespace_oid_ = other.tablespace_oid_;
            main_relfilenode_ = other.main_relfilenode_;
            user_relid_ = other.user_relid_;
            column_attno_ = other.column_attno_;
            index_name_ = std::move(other.index_name_);
            table_name_ = std::move(other.table_name_);
            snapshot_ = other.snapshot_;
            valid_ = other.valid_;
            other.invalidate_();
        }
        return *this;
    }
    ~Rev3AuthoritativeSourceProof() = default;

    [[nodiscard]] const Rev3AuthoritativeSourceLocator&
    source_for_diagnostics() const noexcept {
        return source_;
    }

    [[nodiscard]] std::uint64_t
    source_content_checksum_for_diagnostics() const noexcept {
        return source_content_checksum_;
    }
};

static_assert(!std::is_default_constructible_v<
    Rev3AuthoritativeSourceProof>);
static_assert(!std::is_copy_constructible_v<
    Rev3AuthoritativeSourceProof>);
static_assert(std::is_nothrow_move_constructible_v<
    Rev3AuthoritativeSourceProof>);

[[nodiscard]] inline Rev3AuthoritativeSourceProof
derive_rev3_authoritative_source_proof_pg(
        const Rev3IndexAdministrationIdentity& identity,
        std::uint64_t sequence_rowno,
        Snapshot registered_snapshot) {
    struct ProofState {
        Relation source_relation = nullptr;
        BioseqColumnReadState* source_read = nullptr;
        BioseqColumnReadState* fragment_read = nullptr;
        seq_caches* fragment_caches = nullptr;
        MemoryContext work_context = nullptr;
        MemoryContext fragment_context = nullptr;
    };
    auto* state = new ProofState{};
    Rev3AuthoritativeSourceProof proof{};
    MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        if (identity.database_oid != MyDatabaseId
            || !OidIsValid(identity.user_relid)
            || !OidIsValid(identity.tablespace_oid)
            || !OidIsValid(identity.main_relfilenode)
            || identity.column_attno <= 0
            || identity.index_name.empty()
            || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)
            || registered_snapshot == nullptr
            || !IsMVCCSnapshot(registered_snapshot)
            || registered_snapshot->regd_count == 0) {
            rev3_storage_error("authoritative source proof input is invalid");
        }
        state->work_context = AllocSetContextCreate(
            CurrentMemoryContext, "bioseq rev3 authoritative source proof",
            ALLOCSET_DEFAULT_SIZES);
        state->fragment_context = AllocSetContextCreate(
            state->work_context, "bioseq rev3 source proof fragment",
            ALLOCSET_DEFAULT_SIZES);
        state->source_relation = try_safe_relation_open(
            identity.user_relid, AccessShareLock);
        if (state->source_relation == nullptr
            || RelationGetRelid(state->source_relation) != identity.user_relid
            || state->source_relation->rd_locator.dbOid != identity.database_oid
            || static_cast<Oid>(state->source_relation->rd_locator.relNumber)
                != identity.main_relfilenode) {
            rev3_storage_error("authoritative source relation is missing or changed");
        }
        const Oid source_tablespace =
            OidIsValid(state->source_relation->rd_rel->reltablespace)
            ? state->source_relation->rd_rel->reltablespace
            : MyDatabaseTableSpace;
        TupleDesc source_desc = RelationGetDescr(state->source_relation);
        if (source_tablespace != identity.tablespace_oid
            || identity.column_attno > source_desc->natts
            || TupleDescAttr(source_desc, identity.column_attno - 1)->attisdropped) {
            rev3_storage_error("authoritative source relation/column identity changed");
        }

        MemoryContext previous_context = MemoryContextSwitchTo(
            state->work_context);
        List* projected = list_make1_int(identity.column_attno);
        Datum* values = static_cast<Datum*>(palloc0(
            sizeof(Datum) * static_cast<std::size_t>(source_desc->natts)));
        bool* nulls = static_cast<bool*>(palloc0(
            sizeof(bool) * static_cast<std::size_t>(source_desc->natts)));
        state->source_read = BioseqColumnBeginRead(
            state->source_relation, source_desc, projected, NIL,
            state->work_context, registered_snapshot,
            /*randomAccess=*/true, nullptr);
        const bool row_visible = BioseqColumnReadRowByRowNumber(
            state->source_read, sequence_rowno, values, nulls);
        if (!row_visible || nulls[identity.column_attno - 1]) {
            MemoryContextSwitchTo(previous_context);
            rev3_storage_error("authoritative source row is not visible");
        }
        pgseq_t* source_header = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(
            values[identity.column_attno - 1]));
        const std::uint8_t alphabet_code = static_cast<std::uint8_t>(
            source_header->alphabet_code);
        const std::uint8_t storage_width =
            logical_storage_unit_byte_width(source_header);
        const std::uint16_t padding_letters = source_header->padding_letters;
        if (storage_width == 0) {
            MemoryContextSwitchTo(previous_context);
            rev3_storage_error("authoritative source storage width is zero");
        }

        if (!is_locator_stub(source_header)) {
            MemoryContextSwitchTo(previous_context);
            rev3_storage_error(
                "authoritative revision-3 source is not an epoch-bearing locator stub");
        }
        const seq_locator locator = decode_locator_from_stub(source_header);
        if (locator.version != kLocatorVersion
            || locator._pad_2 != 0 || locator._pad_4 != 0
            || locator.source_epoch == 0) {
            MemoryContextSwitchTo(previous_context);
            rev3_storage_error("authoritative source locator is invalid");
        }
        proof.source_ = Rev3AuthoritativeSourceLocator{
            .sequence_rowno = sequence_rowno,
            .locator = locator,
            .alphabet_code = alphabet_code,
            .storage_unit_byte_width = storage_width,
            .padding_letters = padding_letters,
            .compatibility_checksum = 0,
        };

        const auto source_catalog =
            SequenceSourceVersionManager::lookup(
                locator.source_epoch, registered_snapshot);
        const std::uint64_t main_storage_id =
            BioseqColumnStorageGetStorageId(state->source_relation, false);
        if (!source_catalog
            || source_catalog->source_epoch != locator.source_epoch
            || source_catalog->user_relid != identity.user_relid
            || source_catalog->database_oid != identity.database_oid
            || source_catalog->tablespace_oid != identity.tablespace_oid
            || source_catalog->main_relfilenode != identity.main_relfilenode
            || source_catalog->main_storage_id != main_storage_id
            || source_catalog->column_attno != identity.column_attno
            || !rev3_same_seq_locator(source_catalog->locator, locator)
            || source_catalog->alphabet_code != alphabet_code
            || source_catalog->storage_unit_byte_width != storage_width
            || source_catalog->padding_letters != padding_letters) {
            MemoryContextSwitchTo(previous_context);
            rev3_storage_error(
                "authoritative source epoch/catalog binding disagrees");
        }

        Crc64Ecma182 content_checksum{};
        std::uint64_t decoded_bp = 0;
        const auto consume_visible_fragment = [&content_checksum, &decoded_bp,
                alphabet_code, storage_width](const pgseq_t* fragment) {
            if (fragment == nullptr || is_locator_stub(fragment)
                || static_cast<std::uint8_t>(fragment->alphabet_code)
                    != alphabet_code
                || logical_storage_unit_byte_width(fragment) != storage_width) {
                rev3_storage_error("authoritative source fragment header disagrees");
            }
            fragment->convert_to([&content_checksum, &decoded_bp]<class S>(
                    const compact_sequence_range<S>& range) {
                const std::string decoded =
                    ::bioseq::simd::active_target::decode_sequence_simd<S>(range);
                if (decoded.size() != range.bp_size()
                    || decoded.size()
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - decoded_bp) {
                    rev3_storage_error("authoritative source decoded length is invalid");
                }
                content_checksum.update(std::as_bytes(
                    std::span<const char>{decoded.data(), decoded.size()}));
                decoded_bp += decoded.size();
            });
        };

        if (!is_locator_stub(source_header)) {
            consume_visible_fragment(source_header);
        } else if (locator.num_fragments != 0) {
            state->fragment_caches = alloc_seq_caches(state->work_context);
            state->fragment_caches->side = seq_caches::Side::Read;
            state->fragment_caches->lock_mode = AccessShareLock;
            state->fragment_caches->read_state_mctx = state->work_context;
            state->fragment_caches->main_database_oid = identity.database_oid;
            state->fragment_caches->main_tablespace_oid = identity.tablespace_oid;
            state->fragment_caches->main_relfilenode = identity.main_relfilenode;
            Relation fragment_relation = open_sub_table_for_locator(
                identity.database_oid, identity.tablespace_oid,
                identity.main_relfilenode, identity.column_attno,
                locator, AccessShareLock, state->fragment_caches);
            if (RelationGetRelid(fragment_relation)
                    != source_catalog->fragment_relid
                || BioseqColumnStorageGetStorageId(
                    fragment_relation, false)
                    != source_catalog->fragment_storage_id) {
                MemoryContextSwitchTo(previous_context);
                rev3_storage_error(
                    "authoritative source fragment storage incarnation changed");
            }
            TupleDesc fragment_desc = RelationGetDescr(fragment_relation);
            if (fragment_desc->natts != 1
                || TupleDescAttr(fragment_desc, 0)->attisdropped) {
                MemoryContextSwitchTo(previous_context);
                rev3_storage_error("authoritative source fragment schema disagrees");
            }
            state->fragment_read = BioseqColumnBeginRead(
                fragment_relation, fragment_desc, list_make1_int(1), NIL,
                state->work_context, registered_snapshot,
                /*randomAccess=*/true, nullptr);
            for (std::uint32_t fragment_no = 0;
                 fragment_no < locator.num_fragments; ++fragment_no) {
                CHECK_FOR_INTERRUPTS();
                MemoryContextReset(state->fragment_context);
                Datum fragment_values[1]{0};
                bool fragment_nulls[1]{true};
                const std::uint64_t fragment_rowno =
                    locator.first_sub_row_no + fragment_no;
                if (fragment_rowno < locator.first_sub_row_no
                    || !BioseqColumnReadRowByRowNumber(
                        state->fragment_read, fragment_rowno,
                        fragment_values, fragment_nulls)
                    || fragment_nulls[0]) {
                    MemoryContextSwitchTo(previous_context);
                    rev3_storage_error("authoritative source fragment row is missing");
                }
                MemoryContextSwitchTo(state->fragment_context);
                pgseq_t* fragment = reinterpret_cast<pgseq_t*>(
                    PG_DETOAST_DATUM(fragment_values[0]));
                if ((fragment_no + 1u != locator.num_fragments
                        && fragment->padding_letters != 0)
                    || (fragment_no + 1u == locator.num_fragments
                        && fragment->padding_letters != padding_letters)) {
                    MemoryContextSwitchTo(previous_context);
                    rev3_storage_error("authoritative source fragment padding disagrees");
                }
                consume_visible_fragment(fragment);
                MemoryContextSwitchTo(state->work_context);
            }
        }
        if (decoded_bp != locator.total_bp_size) {
            MemoryContextSwitchTo(previous_context);
            rev3_storage_error("authoritative source decoded length disagrees with locator");
        }
        if (content_checksum.checksum()
                != source_catalog->source_content_checksum) {
            MemoryContextSwitchTo(previous_context);
            rev3_storage_error(
                "authoritative source decoded checksum disagrees with its epoch catalog");
        }
        proof.source_content_checksum_ =
            source_catalog->source_content_checksum;
        proof.database_oid_ = identity.database_oid;
        proof.tablespace_oid_ = identity.tablespace_oid;
        proof.main_relfilenode_ = identity.main_relfilenode;
        proof.user_relid_ = identity.user_relid;
        proof.column_attno_ = identity.column_attno;
        proof.index_name_ = identity.index_name;
        proof.table_name_ = format_seqidx_name(
            identity.index_name, identity.database_oid,
            identity.tablespace_oid, identity.main_relfilenode,
            identity.column_attno, proof.source_.locator.bucket_idx,
            proof.source_.locator.shard_idx);
        proof.snapshot_ = registered_snapshot;
        proof.valid_ = true;

        if (state->fragment_read != nullptr) {
            BioseqColumnEndRead(state->fragment_read);
            state->fragment_read = nullptr;
        }
        if (state->fragment_caches != nullptr) {
            close_and_destroy_seq_caches(
                state->fragment_caches, /*commit=*/false);
            state->fragment_caches = nullptr;
        }
        BioseqColumnEndRead(state->source_read);
        state->source_read = nullptr;
        MemoryContextSwitchTo(previous_context);
        Relation finished_source = std::exchange(
            state->source_relation, nullptr);
        relation_close(finished_source, AccessShareLock);
        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
        state->fragment_context = nullptr;
        MemoryContextDelete(finished_context);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        if (state->fragment_read != nullptr) {
            BioseqColumnReadState* cleanup = std::exchange(
                state->fragment_read, nullptr);
            PG_TRY(); { BioseqColumnEndRead(cleanup); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        if (state->fragment_caches != nullptr) {
            seq_caches* cleanup = std::exchange(
                state->fragment_caches, nullptr);
            PG_TRY(); { close_and_destroy_seq_caches(cleanup, false); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        if (state->source_read != nullptr) {
            BioseqColumnReadState* cleanup = std::exchange(
                state->source_read, nullptr);
            PG_TRY(); { BioseqColumnEndRead(cleanup); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        if (state->source_relation != nullptr) {
            Relation cleanup = std::exchange(state->source_relation, nullptr);
            PG_TRY(); { relation_close(cleanup, AccessShareLock); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        if (state->work_context != nullptr) {
            MemoryContext cleanup = std::exchange(
                state->work_context, nullptr);
            state->fragment_context = nullptr;
            MemoryContextDelete(cleanup);
        }
        proof.invalidate_();
        delete state;
        ReThrowError(saved_error);
    }
    PG_END_TRY();
    delete state;
    return proof;
}

/// Fixed work equation for cold, timed source admission. These counters travel
/// inside the move-only proof and are rechecked by the read session, so a later
/// accidental source scan cannot be benchmark-accepted as an O(1) load.
struct Rev3CatalogSourceProofCounters {
    std::uint64_t parent_rows_read = 0;
    std::uint64_t catalog_rows_read = 0;
    std::uint64_t catalog_index_scans = 0;
    std::uint64_t fragment_rows_read = 0;
    std::uint64_t decoded_bp = 0;
    std::uint64_t storage_incarnations_checked = 0;

    [[nodiscard]] bool timed_equation_holds() const noexcept {
        return parent_rows_read == 1
            && catalog_rows_read == 1
            && catalog_index_scans == 1
            && fragment_rows_read == 0
            && decoded_bp == 0
            && storage_incarnations_checked >= 1
            && storage_incarnations_checked <= 2;
    }
};

/// O(1), PostgreSQL-owned source proof for a cold reader. It validates one
/// visible parent stub and one source-epoch catalog tuple, plus constant-size
/// relation storage-incarnation metadata. It never opens a source fragment
/// read state and never decodes bases.
class Rev3CatalogSourceProof {
private:
    Rev3AuthoritativeSourceLocator source_{};
    std::uint64_t source_content_checksum_ = 0;
    Rev3CatalogSourceProofCounters counters_{};
    Oid database_oid_ = InvalidOid;
    Oid tablespace_oid_ = InvalidOid;
    Oid main_relfilenode_ = InvalidOid;
    Oid user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string index_name_{};
    std::string table_name_{};
    Snapshot snapshot_ = nullptr;
    bool valid_ = false;

    Rev3CatalogSourceProof() = default;

    void invalidate_() noexcept {
        source_ = {};
        source_content_checksum_ = 0;
        counters_ = {};
        database_oid_ = InvalidOid;
        tablespace_oid_ = InvalidOid;
        main_relfilenode_ = InvalidOid;
        user_relid_ = InvalidOid;
        column_attno_ = 0;
        std::string{}.swap(index_name_);
        std::string{}.swap(table_name_);
        snapshot_ = nullptr;
        valid_ = false;
    }

    friend Rev3CatalogSourceProof derive_rev3_catalog_source_proof_pg(
        const Rev3IndexAdministrationIdentity&, std::uint64_t, Snapshot);
    friend class IndexStorageFragmentGenerationReadSessionPg;

public:
    Rev3CatalogSourceProof(const Rev3CatalogSourceProof&) = delete;
    Rev3CatalogSourceProof& operator=(const Rev3CatalogSourceProof&) = delete;
    Rev3CatalogSourceProof(Rev3CatalogSourceProof&& other) noexcept
        : source_(other.source_),
          source_content_checksum_(other.source_content_checksum_),
          counters_(other.counters_),
          database_oid_(other.database_oid_),
          tablespace_oid_(other.tablespace_oid_),
          main_relfilenode_(other.main_relfilenode_),
          user_relid_(other.user_relid_),
          column_attno_(other.column_attno_),
          index_name_(std::move(other.index_name_)),
          table_name_(std::move(other.table_name_)),
          snapshot_(other.snapshot_), valid_(other.valid_) {
        other.invalidate_();
    }
    Rev3CatalogSourceProof& operator=(
            Rev3CatalogSourceProof&& other) noexcept {
        if (this != &other) {
            invalidate_();
            source_ = other.source_;
            source_content_checksum_ = other.source_content_checksum_;
            counters_ = other.counters_;
            database_oid_ = other.database_oid_;
            tablespace_oid_ = other.tablespace_oid_;
            main_relfilenode_ = other.main_relfilenode_;
            user_relid_ = other.user_relid_;
            column_attno_ = other.column_attno_;
            index_name_ = std::move(other.index_name_);
            table_name_ = std::move(other.table_name_);
            snapshot_ = other.snapshot_;
            valid_ = other.valid_;
            other.invalidate_();
        }
        return *this;
    }
    ~Rev3CatalogSourceProof() = default;

    [[nodiscard]] const Rev3AuthoritativeSourceLocator&
    source_for_diagnostics() const noexcept { return source_; }
    [[nodiscard]] std::uint64_t
    source_content_checksum_for_diagnostics() const noexcept {
        return source_content_checksum_;
    }
    [[nodiscard]] const Rev3CatalogSourceProofCounters&
    counters_for_diagnostics() const noexcept { return counters_; }
};

static_assert(!std::is_default_constructible_v<Rev3CatalogSourceProof>);
static_assert(!std::is_copy_constructible_v<Rev3CatalogSourceProof>);
static_assert(std::is_nothrow_move_constructible_v<Rev3CatalogSourceProof>);

[[nodiscard]] inline Rev3CatalogSourceProof
derive_rev3_catalog_source_proof_pg(
        const Rev3IndexAdministrationIdentity& identity,
        std::uint64_t sequence_rowno,
        Snapshot registered_snapshot) {
    struct ProofState {
        Relation source_relation = nullptr;
        Relation fragment_relation = nullptr;
        BioseqColumnReadState* source_read = nullptr;
        MemoryContext work_context = nullptr;
    };
    auto* state = new ProofState{};
    Rev3CatalogSourceProof proof{};
    MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        if (identity.database_oid != MyDatabaseId
            || !OidIsValid(identity.user_relid)
            || !OidIsValid(identity.tablespace_oid)
            || !OidIsValid(identity.main_relfilenode)
            || identity.column_attno <= 0
            || identity.index_name.empty()
            || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)
            || registered_snapshot == nullptr
            || !IsMVCCSnapshot(registered_snapshot)
            || registered_snapshot->regd_count == 0) {
            rev3_storage_error("catalog source proof input is invalid");
        }
        state->work_context = AllocSetContextCreate(
            CurrentMemoryContext, "bioseq rev3 catalog source proof",
            ALLOCSET_DEFAULT_SIZES);
        state->source_relation = try_safe_relation_open(
            identity.user_relid, AccessShareLock);
        if (state->source_relation == nullptr
            || RelationGetRelid(state->source_relation) != identity.user_relid
            || state->source_relation->rd_locator.dbOid != identity.database_oid
            || static_cast<Oid>(state->source_relation->rd_locator.relNumber)
                != identity.main_relfilenode) {
            rev3_storage_error("catalog source relation is missing or changed");
        }
        const Oid source_tablespace =
            OidIsValid(state->source_relation->rd_rel->reltablespace)
            ? state->source_relation->rd_rel->reltablespace
            : MyDatabaseTableSpace;
        TupleDesc source_desc = RelationGetDescr(state->source_relation);
        if (source_tablespace != identity.tablespace_oid
            || identity.column_attno > source_desc->natts
            || TupleDescAttr(source_desc, identity.column_attno - 1)->attisdropped) {
            rev3_storage_error("catalog source relation/column identity changed");
        }

        MemoryContext previous_context = MemoryContextSwitchTo(
            state->work_context);
        List* projected = list_make1_int(identity.column_attno);
        Datum* values = static_cast<Datum*>(palloc0(
            sizeof(Datum) * static_cast<std::size_t>(source_desc->natts)));
        bool* nulls = static_cast<bool*>(palloc0(
            sizeof(bool) * static_cast<std::size_t>(source_desc->natts)));
        state->source_read = BioseqColumnBeginRead(
            state->source_relation, source_desc, projected, NIL,
            state->work_context, registered_snapshot,
            /*randomAccess=*/true, nullptr);
        const bool row_visible = BioseqColumnReadRowByRowNumber(
            state->source_read, sequence_rowno, values, nulls);
        if (!row_visible || nulls[identity.column_attno - 1]) {
            MemoryContextSwitchTo(previous_context);
            rev3_storage_error("catalog source row is not visible");
        }
        ++proof.counters_.parent_rows_read;
        pgseq_t* source_header = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(
            values[identity.column_attno - 1]));
        if (!is_locator_stub(source_header)) {
            MemoryContextSwitchTo(previous_context);
            rev3_storage_error(
                "catalog source is not an epoch-bearing locator stub");
        }
        const seq_locator locator = decode_locator_from_stub(source_header);
        const std::uint8_t alphabet_code = static_cast<std::uint8_t>(
            source_header->alphabet_code);
        const std::uint8_t storage_width =
            logical_storage_unit_byte_width(source_header);
        const std::uint16_t padding_letters = source_header->padding_letters;
        if (locator.version != kLocatorVersion || locator.source_epoch == 0
            || locator._pad_2 != 0 || locator._pad_4 != 0
            || storage_width == 0) {
            MemoryContextSwitchTo(previous_context);
            rev3_storage_error("catalog source locator is invalid");
        }

        bool exact_primary_index_scan = false;
        const auto catalog = SequenceSourceVersionManager::lookup(
            locator.source_epoch, registered_snapshot,
            /*require_ready=*/true, &exact_primary_index_scan);
        if (exact_primary_index_scan) {
            ++proof.counters_.catalog_index_scans;
        }
        if (catalog) ++proof.counters_.catalog_rows_read;
        const std::uint64_t main_storage_id =
            BioseqColumnStorageGetStorageId(state->source_relation, false);
        ++proof.counters_.storage_incarnations_checked;
        if (!catalog
            || catalog->source_epoch != locator.source_epoch
            || catalog->user_relid != identity.user_relid
            || catalog->database_oid != identity.database_oid
            || catalog->tablespace_oid != identity.tablespace_oid
            || catalog->main_relfilenode != identity.main_relfilenode
            || catalog->main_storage_id != main_storage_id
            || catalog->column_attno != identity.column_attno
            || !rev3_same_seq_locator(catalog->locator, locator)
            || catalog->alphabet_code != alphabet_code
            || catalog->storage_unit_byte_width != storage_width
            || catalog->padding_letters != padding_letters) {
            MemoryContextSwitchTo(previous_context);
            rev3_storage_error("catalog source epoch binding disagrees");
        }
        if (locator.num_fragments == 0) {
            if (OidIsValid(catalog->fragment_relid)
                || catalog->fragment_storage_id != 0
                || locator.first_sub_row_no != 0) {
                MemoryContextSwitchTo(previous_context);
                rev3_storage_error("empty catalog source sentinel disagrees");
            }
        } else {
            state->fragment_relation = try_safe_relation_open(
                catalog->fragment_relid, AccessShareLock);
            const std::string expected_fragment_name = format_sub_table_name(
                identity.database_oid, identity.tablespace_oid,
                identity.main_relfilenode, identity.column_attno,
                static_cast<std::int16_t>(locator.bucket_idx),
                static_cast<std::int32_t>(locator.shard_idx));
            if (state->fragment_relation == nullptr
                || RelationGetNamespace(state->fragment_relation)
                    != bioseq_internal_ns()
                || expected_fragment_name
                    != RelationGetRelationName(state->fragment_relation)
                || BioseqColumnStorageGetStorageId(
                    state->fragment_relation, false)
                    != catalog->fragment_storage_id) {
                MemoryContextSwitchTo(previous_context);
                rev3_storage_error(
                    "catalog source fragment storage incarnation changed");
            }
            ++proof.counters_.storage_incarnations_checked;
        }

        proof.source_ = Rev3AuthoritativeSourceLocator{
            .sequence_rowno = sequence_rowno,
            .locator = locator,
            .alphabet_code = alphabet_code,
            .storage_unit_byte_width = storage_width,
            .padding_letters = padding_letters,
            .compatibility_checksum = 0,
        };
        proof.source_content_checksum_ = catalog->source_content_checksum;
        proof.database_oid_ = identity.database_oid;
        proof.tablespace_oid_ = identity.tablespace_oid;
        proof.main_relfilenode_ = identity.main_relfilenode;
        proof.user_relid_ = identity.user_relid;
        proof.column_attno_ = identity.column_attno;
        proof.index_name_ = identity.index_name;
        proof.table_name_ = format_seqidx_name(
            identity.index_name, identity.database_oid,
            identity.tablespace_oid, identity.main_relfilenode,
            identity.column_attno, locator.bucket_idx, locator.shard_idx);
        proof.snapshot_ = registered_snapshot;
        proof.valid_ = proof.counters_.timed_equation_holds();
        if (!proof.valid_) {
            MemoryContextSwitchTo(previous_context);
            rev3_storage_error("catalog source proof work equation disagrees");
        }

        if (state->fragment_relation != nullptr) {
            Relation finished = std::exchange(
                state->fragment_relation, nullptr);
            relation_close(finished, AccessShareLock);
        }
        BioseqColumnEndRead(state->source_read);
        state->source_read = nullptr;
        MemoryContextSwitchTo(previous_context);
        Relation finished_source = std::exchange(
            state->source_relation, nullptr);
        relation_close(finished_source, AccessShareLock);
        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
        MemoryContextDelete(finished_context);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        if (state->source_read != nullptr) {
            BioseqColumnReadState* cleanup = std::exchange(
                state->source_read, nullptr);
            PG_TRY(); { BioseqColumnEndRead(cleanup); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        if (state->fragment_relation != nullptr) {
            Relation cleanup = std::exchange(
                state->fragment_relation, nullptr);
            PG_TRY(); { relation_close(cleanup, AccessShareLock); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        if (state->source_relation != nullptr) {
            Relation cleanup = std::exchange(state->source_relation, nullptr);
            PG_TRY(); { relation_close(cleanup, AccessShareLock); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        if (state->work_context != nullptr) {
            MemoryContext cleanup = std::exchange(
                state->work_context, nullptr);
            MemoryContextDelete(cleanup);
        }
        proof.invalidate_();
        delete state;
        ReThrowError(saved_error);
    }
    PG_END_TRY();
    delete state;
    return proof;
}

class IndexStorageFragmentGenerationRowSinkPg;
class ReadyGenerationToken;
inline void publish_ready_minimizer_generation_head_pg(
    ReadyGenerationToken&& token);

/// Move-only proof that one exact relation/source/generation was completely
/// reread under an own-write-visible registered snapshot. Construction is
/// private to the generation sink's full validation pass.
class ReadyGenerationToken {
private:
    Rev3IndexAdministrationIdentity identity_{};
    Oid sidecar_relation_oid_ = InvalidOid;
    RelFileLocator sidecar_locator_{};
    std::uint64_t advisory_lock_key_ = 0;
    std::uint64_t query_key_ = 0;
    std::uint64_t generation_id_ = 0;
    std::uint64_t source_identity_ = 0;
    std::uint64_t source_content_checksum_ = 0;
    std::uint64_t compatibility_checksum_ = 0;
    TransactionId transaction_id_ = InvalidTransactionId;
    std::array<std::byte, MinimizerGenerationHead::encoded_bytes>
        head_bytes_{};
    std::uint64_t head_row_checksum_ = 0;
    bool valid_ = false;

    ReadyGenerationToken(
            Rev3IndexAdministrationIdentity identity,
            Oid sidecar_relation_oid,
            RelFileLocator sidecar_locator,
            std::uint64_t advisory_lock_key,
            std::uint64_t query_key,
            std::uint64_t generation_id,
            std::uint64_t source_identity,
            std::uint64_t source_content_checksum,
            std::uint64_t compatibility_checksum,
            std::span<const std::byte> head_bytes,
            std::uint64_t head_row_checksum)
        : identity_(std::move(identity)),
          sidecar_relation_oid_(sidecar_relation_oid),
          sidecar_locator_(sidecar_locator),
          advisory_lock_key_(advisory_lock_key),
          query_key_(query_key),
          generation_id_(generation_id),
          source_identity_(source_identity),
          source_content_checksum_(source_content_checksum),
          compatibility_checksum_(compatibility_checksum),
          transaction_id_(GetCurrentTransactionId()),
          head_row_checksum_(head_row_checksum),
          valid_(true) {
        std::copy(head_bytes.begin(), head_bytes.end(), head_bytes_.begin());
    }

    void invalidate() noexcept {
        identity_.release_owned_strings();
        sidecar_relation_oid_ = InvalidOid;
        sidecar_locator_ = {};
        advisory_lock_key_ = 0;
        query_key_ = 0;
        generation_id_ = 0;
        source_identity_ = 0;
        source_content_checksum_ = 0;
        compatibility_checksum_ = 0;
        transaction_id_ = InvalidTransactionId;
        head_bytes_.fill(std::byte{0});
        head_row_checksum_ = 0;
        valid_ = false;
    }

    friend class IndexStorageFragmentGenerationRowSinkPg;
    friend void publish_ready_minimizer_generation_head_pg(
        ReadyGenerationToken&& token);

public:
    ReadyGenerationToken() = delete;
    ReadyGenerationToken(const ReadyGenerationToken&) = delete;
    ReadyGenerationToken& operator=(const ReadyGenerationToken&) = delete;
    ReadyGenerationToken(ReadyGenerationToken&& other) noexcept
        : identity_(std::move(other.identity_)),
          sidecar_relation_oid_(other.sidecar_relation_oid_),
          sidecar_locator_(other.sidecar_locator_),
          advisory_lock_key_(other.advisory_lock_key_),
          query_key_(other.query_key_),
          generation_id_(other.generation_id_),
          source_identity_(other.source_identity_),
          source_content_checksum_(other.source_content_checksum_),
          compatibility_checksum_(other.compatibility_checksum_),
          transaction_id_(other.transaction_id_),
          head_bytes_(other.head_bytes_),
          head_row_checksum_(other.head_row_checksum_),
          valid_(other.valid_) {
        other.invalidate();
    }
    ReadyGenerationToken& operator=(ReadyGenerationToken&& other) noexcept {
        if (this != &other) {
            invalidate();
            identity_ = std::move(other.identity_);
            sidecar_relation_oid_ = other.sidecar_relation_oid_;
            sidecar_locator_ = other.sidecar_locator_;
            advisory_lock_key_ = other.advisory_lock_key_;
            query_key_ = other.query_key_;
            generation_id_ = other.generation_id_;
            source_identity_ = other.source_identity_;
            source_content_checksum_ = other.source_content_checksum_;
            compatibility_checksum_ = other.compatibility_checksum_;
            transaction_id_ = other.transaction_id_;
            head_bytes_ = other.head_bytes_;
            head_row_checksum_ = other.head_row_checksum_;
            valid_ = other.valid_;
            other.invalidate();
        }
        return *this;
    }
    ~ReadyGenerationToken() = default;
};

static_assert(!std::is_default_constructible_v<ReadyGenerationToken>);
static_assert(!std::is_copy_constructible_v<ReadyGenerationToken>);
static_assert(!std::is_copy_assignable_v<ReadyGenerationToken>);
static_assert(std::is_nothrow_move_constructible_v<ReadyGenerationToken>);
static_assert(std::is_nothrow_move_assignable_v<ReadyGenerationToken>);

/// Form one immutable revision-3 payload/directory/manifest tuple. The caller
/// transfers the tuple into a TTSOpsHeapTuple slot and flushes a bounded group
/// with CatalogTuplesMultiInsertWithInfo, PostgreSQL 17's supported catalog
/// heap-multi-insert + index-maintenance API. This helper never inserts, probes
/// for, or replaces a row by itself.
[[nodiscard]] inline std::expected<HeapTuple, MinimizerGenerationWireError>
form_rev3_generation_row_pg(
        Relation relation,
        std::uint64_t query_key,
        std::uint64_t expected_generation_id,
        const MinimizerGenerationRowView& row) {
    if (relation == nullptr
        || !bioseq::indexing::sql_bigint_can_represent(query_key)) {
        return std::unexpected(MinimizerGenerationWireError::generation_id);
    }
    if (auto valid = validate_rev3_generation_row_for_write(
            expected_generation_id, row); !valid) {
        return std::unexpected(valid.error());
    }
    bytea* blob = bioseq::pg_utils::invoke_pg_as_cpp([&] {
        return reinterpret_cast<bytea*>(
            palloc(VARHDRSZ + row.bytes.size()));
    });
    SET_VARSIZE(blob, VARHDRSZ + row.bytes.size());
    std::memcpy(VARDATA(blob), row.bytes.data(), row.bytes.size());
    bioseq::pg_utils::pg_datum_list<8> values{};
    values.set(kRev3AttQueryKey, static_cast<std::int64_t>(query_key));
    values.set(kRev3AttGenerationId,
        static_cast<std::int64_t>(row.address.generation_id));
    values.set(kRev3AttPositionSegment, row.address.position_segment);
    values.set(kRev3AttSubKey,
        static_cast<std::int64_t>(row.address.sub_key));
    values.set(kRev3AttChunkNo,
        static_cast<std::int32_t>(row.address.chunk_no));
    values.set(kRev3AttChunkBytes,
        static_cast<std::int32_t>(row.bytes.size()));
    values.set(kRev3AttChecksum,
        checksum_to_pg_bigint(row.row_checksum));
    values.set_raw(kRev3AttBlob,
        bioseq::pg_utils::pg_datum::from_pointer(blob));
    HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp(
        [&] { return values.form_tuple(RelationGetDescr(relation)); });
    pfree(blob);
    return tuple;
}

struct Rev3LoadedPgRow {
    std::uint64_t query_key = 0;
    IndexChunkAddress address{};
    std::uint32_t chunk_bytes = 0;
    std::uint64_t checksum = 0;
    bioseq::pg_utils::pg_detoasted_ptr<bytea> blob{};
    std::span<const std::byte> bytes;

    void release() noexcept {
        blob.free_if_copy();
        bytes = {};
    }
};

/// Decode all eight non-NULL fields and validate PLAIN storage, byte count and
/// the address-bound row checksum before any caller observes the payload.
[[nodiscard]] inline Rev3LoadedPgRow load_rev3_pg_row_or_error(
        Relation relation,
        HeapTuple tuple) {
    TupleDesc tuple_desc = RelationGetDescr(relation);
    bool is_null = false;
    const Datum query_key_datum = heap_getattr(
        tuple, kRev3AttQueryKey, tuple_desc, &is_null);
    if (is_null) rev3_storage_error("row query key is NULL");
    const std::int64_t signed_query_key = DatumGetInt64(query_key_datum);
    if (signed_query_key < 0) rev3_storage_error("row query key is negative");
    const Datum generation_datum = heap_getattr(
        tuple, kRev3AttGenerationId, tuple_desc, &is_null);
    if (is_null) rev3_storage_error("row generation ID is NULL");
    const std::int64_t signed_generation_id = DatumGetInt64(generation_datum);
    if (signed_generation_id < 0) {
        rev3_storage_error("row generation ID is negative");
    }
    const Datum position_datum = heap_getattr(
        tuple, kRev3AttPositionSegment, tuple_desc, &is_null);
    if (is_null) rev3_storage_error("row position segment is NULL");
    const std::int32_t position_segment = DatumGetInt32(position_datum);
    const Datum sub_key_datum = heap_getattr(
        tuple, kRev3AttSubKey, tuple_desc, &is_null);
    if (is_null) rev3_storage_error("row sub-key is NULL");
    const std::int64_t sub_key = DatumGetInt64(sub_key_datum);
    const Datum chunk_no_datum = heap_getattr(
        tuple, kRev3AttChunkNo, tuple_desc, &is_null);
    if (is_null) rev3_storage_error("row chunk number is NULL");
    const std::int32_t signed_chunk_no = DatumGetInt32(chunk_no_datum);
    if (signed_chunk_no < 0) rev3_storage_error("row chunk number is negative");
    const Datum chunk_bytes_datum = heap_getattr(
        tuple, kRev3AttChunkBytes, tuple_desc, &is_null);
    if (is_null) rev3_storage_error("row byte count is NULL");
    const std::int32_t signed_chunk_bytes = DatumGetInt32(chunk_bytes_datum);
    if (signed_chunk_bytes <= 0) {
        rev3_storage_error("row byte count is non-positive");
    }
    const Datum checksum_datum = heap_getattr(
        tuple, kRev3AttChecksum, tuple_desc, &is_null);
    if (is_null) rev3_storage_error("row checksum is NULL");
    const std::int64_t signed_checksum = DatumGetInt64(checksum_datum);
    const Datum blob_datum = heap_getattr(
        tuple, kRev3AttBlob, tuple_desc, &is_null);
    if (is_null) rev3_storage_error("row blob is NULL");
    auto* original_blob = reinterpret_cast<varlena*>(
        DatumGetPointer(blob_datum));
    if (VARATT_IS_EXTERNAL(original_blob)
        || VARATT_IS_COMPRESSED(original_blob)) {
        rev3_storage_error("PLAIN row contains toasted/compressed data");
    }
    auto detoasted = bioseq::pg_utils::detoast_packed_as<bytea>(
        bioseq::pg_utils::pg_datum::from_raw(blob_datum));
    if (detoasted.is_copy) {
        detoasted.free_if_copy();
        rev3_storage_error("PLAIN row required detoasting");
    }
    const std::size_t payload_bytes = VARSIZE_ANY_EXHDR(detoasted.ptr);
    if (payload_bytes != static_cast<std::size_t>(signed_chunk_bytes)) {
        detoasted.free_if_copy();
        rev3_storage_error("row byte count disagrees with BYTEA length");
    }
    const IndexChunkAddress address{
        .generation_id = static_cast<std::uint64_t>(signed_generation_id),
        .position_segment = position_segment,
        .sub_key = sub_key,
        .chunk_no = static_cast<std::uint32_t>(signed_chunk_no),
    };
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(VARDATA_ANY(detoasted.ptr)),
        payload_bytes};
    const std::uint64_t checksum = checksum_from_pg_bigint(signed_checksum);
    if (checksum != bioseq::indexing::crc64_index_chunk_row(
            address.generation_id,
            address.position_segment,
            address.sub_key,
            address.chunk_no,
            static_cast<std::uint32_t>(payload_bytes),
            bytes)) {
        detoasted.free_if_copy();
        rev3_storage_error("row checksum disagrees");
    }
    return Rev3LoadedPgRow{
        .query_key = static_cast<std::uint64_t>(signed_query_key),
        .address = address,
        .chunk_bytes = static_cast<std::uint32_t>(signed_chunk_bytes),
        .checksum = checksum,
        .blob = std::move(detoasted),
        .bytes = bytes,
    };
}

/// Relation-owning sink factory target used directly by the existing common
/// two-pass generation encoder. It cannot publish the head and therefore cannot
/// make an incompletely reread generation visible.
class IndexStorageFragmentGenerationRowSinkPg {
private:
    enum class WriteStage : std::uint8_t {
        payload,
        directory,
        manifest,
        tokenized,
        failed,
        moved,
    };

    static constexpr std::uint64_t kWriteBatchRows = 4096;
    static constexpr std::uint64_t kWriteBatchBytes =
        MAX_CATALOG_MULTI_INSERT_BYTES;

    Rev3IndexAdministrationIdentity identity_{};
    Relation relation_ = nullptr;
    CatalogIndexState catalog_indexes_ = nullptr;
    Relation validation_primary_index_ = nullptr;
    Snapshot validation_snapshot_ = nullptr;
    SysScanDesc validation_scan_ = nullptr;
    std::uint64_t advisory_lock_key_ = 0;
    std::uint64_t query_key_ = 0;
    std::uint64_t generation_id_ = 0;
    std::uint64_t row_count_ = 0;
    std::uint64_t write_batch_row_limit_ = kWriteBatchRows;
    std::uint64_t catalog_multi_insert_batches_ = 0;
    std::vector<TupleTableSlot*> pending_slots_;
    std::uint64_t rows_in_batch_ = 0;
    std::uint64_t bytes_in_batch_ = 0;
    std::uint64_t payload_rows_written_ = 0;
    std::uint64_t payload_bytes_written_ = 0;
    Crc64Ecma182 payload_row_checksums_{};
    std::uint32_t next_directory_chunk_ = 1;
    std::uint64_t directory_bytes_written_ = 0;
    Crc64Ecma182 directory_checksum_{};
    std::optional<IndexChunkAddress> previous_payload_address_;
    std::array<std::byte, MinimizerGenerationManifest::encoded_bytes>
        written_manifest_bytes_{};
    std::optional<Rev3AuthoritativeSourceProof> validation_source_proof_;
    WriteStage stage_ = WriteStage::payload;
    bool pg_poisoned_ = false;

    [[nodiscard]] static bool checked_add_(
            std::uint64_t& value,
            std::uint64_t increment) noexcept {
        if (increment
            > (std::numeric_limits<std::uint64_t>::max)() - value) {
            return false;
        }
        value += increment;
        return true;
    }

    void drop_pending_slots_() noexcept {
        for (TupleTableSlot* slot : pending_slots_) {
            if (slot != nullptr) {
                ExecDropSingleTupleTableSlot(slot);
            }
        }
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        rows_in_batch_ = 0;
        bytes_in_batch_ = 0;
    }

    void drop_pending_slots_as_cpp_() {
        for (TupleTableSlot* slot : pending_slots_) {
            if (slot != nullptr) {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { ExecDropSingleTupleTableSlot(slot); });
            }
        }
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        rows_in_batch_ = 0;
        bytes_in_batch_ = 0;
    }

    void release_() noexcept {
        validation_source_proof_.reset();
        drop_pending_slots_();
        if (validation_scan_ != nullptr) {
            systable_endscan_ordered(validation_scan_);
            validation_scan_ = nullptr;
        }
        if (validation_primary_index_ != nullptr) {
            index_close(validation_primary_index_, AccessShareLock);
            validation_primary_index_ = nullptr;
        }
        if (validation_snapshot_ != nullptr) {
            UnregisterSnapshot(validation_snapshot_);
            validation_snapshot_ = nullptr;
        }
        if (catalog_indexes_ != nullptr) {
            CatalogCloseIndexes(catalog_indexes_);
            catalog_indexes_ = nullptr;
        }
        if (relation_ != nullptr) {
            relation_close(relation_, RowExclusiveLock);
            relation_ = nullptr;
        }
    }

    void fail_cleanup_() noexcept {
        stage_ = WriteStage::failed;
        release_();
        identity_.release_owned_strings();
    }

    void abandon_after_pg_error_() noexcept {
        stage_ = WriteStage::failed;
        pg_poisoned_ = true;
        validation_source_proof_.reset();
        // Do not call any PostgreSQL cleanup routine while propagating an
        // ERROR converted at a leaf. Slots, scans, snapshots, catalog state
        // and relation locks remain registered with PostgreSQL's current
        // ResourceOwner and are reclaimed by statement/transaction abort.
        pending_slots_.clear();
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        rows_in_batch_ = 0;
        bytes_in_batch_ = 0;
        validation_scan_ = nullptr;
        validation_primary_index_ = nullptr;
        validation_snapshot_ = nullptr;
        catalog_indexes_ = nullptr;
        relation_ = nullptr;
        identity_.release_owned_strings();
    }

    [[noreturn]] void rethrow_after_cleanup_(
            MemoryContext saved_context) noexcept {
        MemoryContextSwitchTo(saved_context);
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        while (!pending_slots_.empty()) {
            TupleTableSlot* cleanup_slot = pending_slots_.back();
            pending_slots_.pop_back();
            if (cleanup_slot == nullptr) continue;
            PG_TRY();
            {
                ExecDropSingleTupleTableSlot(cleanup_slot);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        rows_in_batch_ = 0;
        bytes_in_batch_ = 0;
        validation_source_proof_.reset();
        if (validation_scan_ != nullptr) {
            SysScanDesc cleanup_scan = std::exchange(
                validation_scan_, nullptr);
            PG_TRY();
            {
                systable_endscan_ordered(cleanup_scan);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        if (validation_primary_index_ != nullptr) {
            Relation cleanup_index = std::exchange(
                validation_primary_index_, nullptr);
            PG_TRY();
            {
                index_close(cleanup_index, AccessShareLock);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        if (validation_snapshot_ != nullptr) {
            Snapshot cleanup_snapshot = std::exchange(
                validation_snapshot_, nullptr);
            PG_TRY();
            {
                UnregisterSnapshot(cleanup_snapshot);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        if (catalog_indexes_ != nullptr) {
            CatalogIndexState cleanup_indexes = std::exchange(
                catalog_indexes_, nullptr);
            PG_TRY();
            {
                CatalogCloseIndexes(cleanup_indexes);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        if (relation_ != nullptr) {
            Relation cleanup_relation = std::exchange(relation_, nullptr);
            PG_TRY();
            {
                relation_close(cleanup_relation, RowExclusiveLock);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        stage_ = WriteStage::failed;
        identity_.release_owned_strings();
        ReThrowError(saved_error);
    }

    void flush_bounded_batch_() {
        if (pending_slots_.empty()) {
            if (rows_in_batch_ != 0 || bytes_in_batch_ != 0) {
                rev3_storage_error("generation write batch counters disagree");
            }
            return;
        }
        if (relation_ == nullptr || catalog_indexes_ == nullptr
            || pending_slots_.size() != rows_in_batch_
            || pending_slots_.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<int>::max)())) {
            rev3_storage_error("generation write batch state is invalid");
        }
        CatalogTuplesMultiInsertWithInfo(
            relation_, pending_slots_.data(),
            static_cast<int>(pending_slots_.size()), catalog_indexes_);
        if (!checked_add_(catalog_multi_insert_batches_, 1)) {
            rev3_storage_error("generation write batch counter overflow");
        }
        drop_pending_slots_();
    }

    void flush_bounded_batch_as_cpp_() {
        if (pending_slots_.empty()) {
            if (rows_in_batch_ != 0 || bytes_in_batch_ != 0) {
                rev3_storage_error_as_cpp(
                    "generation write batch counters disagree");
            }
            return;
        }
        if (relation_ == nullptr || catalog_indexes_ == nullptr
            || pending_slots_.size() != rows_in_batch_
            || pending_slots_.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<int>::max)())) {
            rev3_storage_error_as_cpp(
                "generation write batch state is invalid");
        }
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            CatalogTuplesMultiInsertWithInfo(
                relation_, pending_slots_.data(),
                static_cast<int>(pending_slots_.size()), catalog_indexes_);
        });
        if (!checked_add_(catalog_multi_insert_batches_, 1)) {
            rev3_storage_error_as_cpp(
                "generation write batch counter overflow");
        }
        drop_pending_slots_as_cpp_();
    }

    [[nodiscard]] std::optional<MinimizerGenerationWireError>
    validate_next_row_(const MinimizerGenerationRowView& row) const noexcept {
        if (stage_ == WriteStage::manifest
            || stage_ == WriteStage::tokenized
            || stage_ == WriteStage::failed
            || stage_ == WriteStage::moved) {
            return MinimizerGenerationWireError::row_order;
        }
        const bool manifest = row.address.position_segment == -1
            && row.address.sub_key == DIRECTORY_SUB_KEY
            && row.address.chunk_no == 0;
        const bool directory = row.address.position_segment == -1
            && row.address.sub_key == DIRECTORY_SUB_KEY
            && row.address.chunk_no > 0;
        if (manifest) {
            if (row.address.chunk_no != 0
                || row.bytes.size()
                    != MinimizerGenerationManifest::encoded_bytes) {
                return MinimizerGenerationWireError::row_order;
            }
            return std::nullopt;
        }
        if (directory) {
            if (stage_ != WriteStage::payload
                && stage_ != WriteStage::directory) {
                return MinimizerGenerationWireError::row_order;
            }
            if (row.address.chunk_no != next_directory_chunk_) {
                return MinimizerGenerationWireError::chunk_range;
            }
            return std::nullopt;
        }
        if (stage_ != WriteStage::payload
            || row.address.position_segment < 0
            || row.address.sub_key < 0
            || row.address.sub_key >= TEMP_RUN_BASE) {
            return MinimizerGenerationWireError::row_order;
        }
        const bool position_extent =
            row.address.sub_key >= POSITION_EXTENT_BASE;
        const index_sub_key_t logical_sub_key = position_extent
            ? row.address.sub_key - POSITION_EXTENT_BASE
            : row.address.sub_key;
        if (logical_sub_key < 0
            || logical_sub_key >= POSITION_EXTENT_BASE) {
            return MinimizerGenerationWireError::sub_key;
        }
        if (previous_payload_address_) {
            const bool previous_position_extent =
                previous_payload_address_->sub_key >= POSITION_EXTENT_BASE;
            const index_sub_key_t previous_logical_sub_key =
                previous_position_extent
                ? previous_payload_address_->sub_key - POSITION_EXTENT_BASE
                : previous_payload_address_->sub_key;
            const auto previous = std::tuple{
                previous_payload_address_->position_segment,
                previous_logical_sub_key,
                previous_position_extent,
                previous_payload_address_->chunk_no};
            const auto current = std::tuple{
                row.address.position_segment,
                logical_sub_key,
                position_extent,
                row.address.chunk_no};
            if (!(previous < current)) {
                return MinimizerGenerationWireError::row_order;
            }
        }
        return std::nullopt;
    }

    void record_written_row_(
            const MinimizerGenerationRowView& row,
            std::uint64_t admitted_batch_bytes) {
        const bool manifest = row.address.position_segment == -1
            && row.address.sub_key == DIRECTORY_SUB_KEY
            && row.address.chunk_no == 0;
        const bool directory = row.address.position_segment == -1
            && row.address.sub_key == DIRECTORY_SUB_KEY
            && row.address.chunk_no > 0;
        if (manifest) {
            std::copy(row.bytes.begin(), row.bytes.end(),
                written_manifest_bytes_.begin());
            stage_ = WriteStage::manifest;
        } else if (directory) {
            stage_ = WriteStage::directory;
            ++next_directory_chunk_;
            directory_checksum_.update(row.bytes);
            if (!checked_add_(directory_bytes_written_, row.bytes.size())) {
                rev3_storage_error_as_cpp(
                    "directory byte counter overflow");
            }
        } else {
            previous_payload_address_ = row.address;
            payload_row_checksums_.update_little_endian(row.row_checksum);
            if (!checked_add_(payload_rows_written_, 1)
                || !checked_add_(payload_bytes_written_, row.bytes.size())) {
                rev3_storage_error_as_cpp(
                    "payload write counter overflow");
            }
        }
        if (!checked_add_(row_count_, 1)
            || !checked_add_(rows_in_batch_, 1)
            || !checked_add_(bytes_in_batch_, admitted_batch_bytes)) {
            rev3_storage_error_as_cpp(
                "generation write counter overflow");
        }
    }

public:
    IndexStorageFragmentGenerationRowSinkPg() = delete;
    IndexStorageFragmentGenerationRowSinkPg(
            Rev3IndexAdministrationIdentity identity,
            std::uint64_t query_key,
            std::uint64_t generation_id,
            std::uint64_t write_batch_row_limit = kWriteBatchRows)
        : identity_(std::move(identity)),
          query_key_(query_key), generation_id_(generation_id),
          write_batch_row_limit_(write_batch_row_limit) {
        try {
            if (!bioseq::indexing::sql_bigint_can_represent(query_key)
                || generation_id == 0
                || !bioseq::indexing::sql_bigint_can_represent(generation_id)
                || identity_.database_oid != MyDatabaseId
                || !OidIsValid(identity_.user_relid)
                || identity_.column_attno <= 0
                || identity_.index_name.empty()
                || identity_.table_name.empty()
                || write_batch_row_limit_ == 0
                || write_batch_row_limit_ > kWriteBatchRows) {
                rev3_storage_error_as_cpp(
                    "generation sink identity/key is invalid");
            }
            advisory_lock_key_ = index_advisory_xact_lock_key(
                identity_.database_oid,
                identity_.tablespace_oid,
                identity_.main_relfilenode,
                identity_.column_attno,
                identity_.index_name);
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                acquire_index_advisory_xact_lock(
                    identity_.database_oid,
                    identity_.tablespace_oid,
                    identity_.main_relfilenode,
                    identity_.column_attno,
                    identity_.index_name);
            });
            const Oid relation_oid = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return get_relname_relid(
                    identity_.table_name.c_str(), bioseq_internal_ns());
            });
            relation_ = OidIsValid(relation_oid)
                ? bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return try_safe_relation_open(
                        relation_oid, RowExclusiveLock);
                })
                : nullptr;
            if (relation_ == nullptr
                || RelationGetNamespace(relation_) != bioseq_internal_ns()
                || std::string_view{RelationGetRelationName(relation_)}
                    != identity_.table_name) {
                rev3_storage_error_as_cpp(
                    "revision-3 generation relation is missing or renamed");
            }
            const Rev3IndexSchemaInspection schema =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { return inspect_rev3_index_schema(relation_); });
            if (!schema) {
                rev3_storage_error_as_cpp(fmt::format(
                    "generation relation schema mismatch: {}",
                    rev3_index_schema_error_name(schema.error)));
            }
            catalog_indexes_ = bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { return CatalogOpenIndexes(relation_); });
            if (catalog_indexes_ == nullptr) {
                rev3_storage_error_as_cpp(
                    "CatalogOpenIndexes returned NULL");
            }
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            abandon_after_pg_error_();
            throw;
        }
    }
    IndexStorageFragmentGenerationRowSinkPg(
        const IndexStorageFragmentGenerationRowSinkPg&) = delete;
    IndexStorageFragmentGenerationRowSinkPg& operator=(
        const IndexStorageFragmentGenerationRowSinkPg&) = delete;
    IndexStorageFragmentGenerationRowSinkPg(
            IndexStorageFragmentGenerationRowSinkPg&& other) noexcept
        : identity_(std::move(other.identity_)),
          relation_(std::exchange(other.relation_, nullptr)),
          catalog_indexes_(std::exchange(other.catalog_indexes_, nullptr)),
          validation_primary_index_(std::exchange(
              other.validation_primary_index_, nullptr)),
          validation_snapshot_(std::exchange(
              other.validation_snapshot_, nullptr)),
          validation_scan_(std::exchange(other.validation_scan_, nullptr)),
          advisory_lock_key_(other.advisory_lock_key_),
          query_key_(other.query_key_),
          generation_id_(other.generation_id_),
          row_count_(other.row_count_),
          write_batch_row_limit_(other.write_batch_row_limit_),
          catalog_multi_insert_batches_(
              other.catalog_multi_insert_batches_),
          pending_slots_(std::move(other.pending_slots_)),
          rows_in_batch_(other.rows_in_batch_),
          bytes_in_batch_(other.bytes_in_batch_),
          payload_rows_written_(other.payload_rows_written_),
          payload_bytes_written_(other.payload_bytes_written_),
          payload_row_checksums_(other.payload_row_checksums_),
          next_directory_chunk_(other.next_directory_chunk_),
          directory_bytes_written_(other.directory_bytes_written_),
          directory_checksum_(other.directory_checksum_),
          previous_payload_address_(other.previous_payload_address_),
          written_manifest_bytes_(other.written_manifest_bytes_),
          validation_source_proof_(std::move(
              other.validation_source_proof_)),
          stage_(other.stage_),
          pg_poisoned_(other.pg_poisoned_) {
        other.stage_ = WriteStage::moved;
        other.pg_poisoned_ = true;
    }
    IndexStorageFragmentGenerationRowSinkPg& operator=(
            IndexStorageFragmentGenerationRowSinkPg&& other) noexcept {
        if (this != &other) {
            release_();
            relation_ = std::exchange(other.relation_, nullptr);
            catalog_indexes_ = std::exchange(other.catalog_indexes_, nullptr);
            validation_primary_index_ = std::exchange(
                other.validation_primary_index_, nullptr);
            validation_snapshot_ = std::exchange(
                other.validation_snapshot_, nullptr);
            validation_scan_ = std::exchange(
                other.validation_scan_, nullptr);
            identity_ = std::move(other.identity_);
            advisory_lock_key_ = other.advisory_lock_key_;
            query_key_ = other.query_key_;
            generation_id_ = other.generation_id_;
            row_count_ = other.row_count_;
            write_batch_row_limit_ = other.write_batch_row_limit_;
            catalog_multi_insert_batches_ =
                other.catalog_multi_insert_batches_;
            pending_slots_ = std::move(other.pending_slots_);
            rows_in_batch_ = other.rows_in_batch_;
            bytes_in_batch_ = other.bytes_in_batch_;
            payload_rows_written_ = other.payload_rows_written_;
            payload_bytes_written_ = other.payload_bytes_written_;
            payload_row_checksums_ = other.payload_row_checksums_;
            next_directory_chunk_ = other.next_directory_chunk_;
            directory_bytes_written_ = other.directory_bytes_written_;
            directory_checksum_ = other.directory_checksum_;
            previous_payload_address_ = other.previous_payload_address_;
            written_manifest_bytes_ = other.written_manifest_bytes_;
            validation_source_proof_ = std::move(
                other.validation_source_proof_);
            stage_ = other.stage_;
            pg_poisoned_ = other.pg_poisoned_;
            other.stage_ = WriteStage::moved;
            other.pg_poisoned_ = true;
        }
        return *this;
    }
    ~IndexStorageFragmentGenerationRowSinkPg() { release_(); }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    write(const MinimizerGenerationRowView& row) {
        if (pg_poisoned_) {
            rev3_storage_error_as_cpp(
                "poisoned generation row sink reused after PostgreSQL error");
        }
        if (auto order_error = validate_next_row_(row)) {
            fail_cleanup_();
            return std::unexpected(*order_error);
        }
        if (auto valid = validate_rev3_generation_row_for_write(
                generation_id_, row); !valid) {
            fail_cleanup_();
            return valid;
        }
        try {
            constexpr std::uint64_t kTupleAndSlotOverheadBudget = 1024;
            if (row.bytes.size()
                > (std::numeric_limits<std::uint64_t>::max)()
                    - kTupleAndSlotOverheadBudget) {
                rev3_storage_error_as_cpp(
                    "generation write batch byte estimate overflow");
            }
            const std::uint64_t estimated_batch_bytes =
                static_cast<std::uint64_t>(row.bytes.size())
                + kTupleAndSlotOverheadBudget;
            const auto batch_decision =
                bioseq::indexing::plan_revision3_tuple_batch_append(
                    pending_slots_.size(), bytes_in_batch_,
                    estimated_batch_bytes,
                    write_batch_row_limit_, kWriteBatchBytes);
            if (!batch_decision.valid) {
                rev3_storage_error_as_cpp(
                    "generation write row exceeds bounded catalog batch");
            }
            if (batch_decision.flush_before) {
                flush_bounded_batch_as_cpp_();
            }

            TupleTableSlot* slot = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return MakeSingleTupleTableSlot(
                    RelationGetDescr(relation_), &TTSOpsHeapTuple);
            });
            try {
                pending_slots_.push_back(slot);
            } catch (...) {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { ExecDropSingleTupleTableSlot(slot); });
                throw;
            }
            auto tuple = form_rev3_generation_row_pg(
                relation_, query_key_, generation_id_, row);
            if (!tuple) {
                pending_slots_.pop_back();
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { ExecDropSingleTupleTableSlot(slot); });
                fail_cleanup_();
                return std::unexpected(tuple.error());
            }
            const std::uint64_t actual_batch_bytes =
                static_cast<std::uint64_t>((*tuple)->t_len)
                + sizeof(TupleTableSlot*) + sizeof(HeapTupleData);
            if (actual_batch_bytes > estimated_batch_bytes) {
                heap_freetuple(*tuple);
                pending_slots_.pop_back();
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { ExecDropSingleTupleTableSlot(slot); });
                rev3_storage_error_as_cpp(
                    "generation write tuple exceeded its bounded estimate");
            }
            inject_rev3_pg_failure_as_cpp(
                Rev3PgFailureInjection::generation_row_write,
                "generation_row_write");
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { ExecStoreHeapTuple(*tuple, slot, true); });
            record_written_row_(row, actual_batch_bytes);
            if (rows_in_batch_ >= write_batch_row_limit_
                || bytes_in_batch_ >= kWriteBatchBytes) {
                flush_bounded_batch_as_cpp_();
            }
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            abandon_after_pg_error_();
            throw;
        }
        return {};
    }

    [[nodiscard]] std::uint64_t row_count() const noexcept {
        return row_count_;
    }
    [[nodiscard]] std::uint64_t catalog_multi_insert_batches() const noexcept {
        return catalog_multi_insert_batches_;
    }
    [[nodiscard]] ReadyGenerationToken validate_ready_generation(
            const EncodedMinimizerGeneration& generation) {
        Oid proven_relation_oid = InvalidOid;
        RelFileLocator proven_locator{};
        std::array<std::byte, MinimizerGenerationHead::encoded_bytes>
            proven_head_bytes{};
        std::uint64_t proven_head_checksum = 0;
        std::uint64_t proven_source_identity = 0;
        std::uint64_t proven_source_content_checksum = 0;
        std::uint64_t proven_compatibility_checksum = 0;

        MemoryContext saved_context = CurrentMemoryContext;
        PG_TRY();
        {
            if (stage_ != WriteStage::manifest
                || relation_ == nullptr || catalog_indexes_ == nullptr
                || generation.head.generation_id != generation_id_
                || generation.manifest.generation_id != generation_id_) {
                rev3_storage_error("READY validation identity/stage disagrees");
            }
            if (auto metadata =
                    bioseq::indexing::validate_minimizer_generation_metadata(
                        generation.head,
                        generation.manifest,
                        generation.directory);
                !metadata) {
                rev3_storage_error("READY validation metadata disagrees");
            }
            const MinimizerGenerationRowView head_row{
                .address = generation.unpublished_head_row.address,
                .bytes = generation.unpublished_head_row.bytes(),
                .row_checksum = generation.unpublished_head_row.row_checksum,
            };
            if (head_row.address != IndexChunkAddress{
                    .generation_id = 0,
                    .position_segment = -1,
                    .sub_key = GENERATION_HEAD_SUB_KEY,
                    .chunk_no = 0,
                }
                || head_row.bytes.size() != proven_head_bytes.size()
                || head_row.row_checksum
                    != bioseq::indexing::crc64_index_chunk_row(
                        0, -1, GENERATION_HEAD_SUB_KEY, 0,
                        static_cast<std::uint32_t>(head_row.bytes.size()),
                        head_row.bytes)) {
                rev3_storage_error("READY validation head row is malformed");
            }
            std::copy(head_row.bytes.begin(), head_row.bytes.end(),
                proven_head_bytes.begin());
            proven_head_checksum = head_row.row_checksum;

            std::uint64_t expected_payload_rows = 0;
            std::uint64_t expected_payload_bytes = 0;
            std::optional<std::pair<std::int32_t, index_sub_key_t>>
                previous_storage_key;
            for (const MinimizerBucketDirEntry& entry
                 : generation.directory.entries()) {
                const std::pair storage_key{
                    entry.position_segment, entry.sub_key};
                if (previous_storage_key
                    && !(*previous_storage_key < storage_key)) {
                    rev3_storage_error(
                        "directory is not in physical PK storage order");
                }
                previous_storage_key = storage_key;
                if (!checked_add_(expected_payload_rows,
                        static_cast<std::uint64_t>(entry.control_chunk_count)
                            + entry.position_chunk_count)
                    || !checked_add_(expected_payload_bytes,
                        entry.control_encoded_bytes)
                    || !checked_add_(expected_payload_bytes,
                        entry.position_encoded_bytes)) {
                    rev3_storage_error("READY expected payload counters overflow");
                }
            }
            std::uint64_t expected_total_rows = expected_payload_rows;
            if (!checked_add_(expected_total_rows,
                    generation.manifest.directory_chunk_count)
                || !checked_add_(expected_total_rows, 1)) {
                rev3_storage_error("READY expected row counter overflow");
            }
            if (row_count_ != expected_total_rows
                || payload_rows_written_ != expected_payload_rows
                || payload_bytes_written_ != expected_payload_bytes
                || payload_row_checksums_.checksum()
                    != generation.manifest.payload_checksum
                || next_directory_chunk_
                    != generation.manifest.directory_chunk_count + 1u
                || directory_bytes_written_
                    != generation.manifest.directory_bytes
                || directory_checksum_.checksum()
                    != generation.manifest.directory_checksum
                || !std::ranges::equal(
                    written_manifest_bytes_,
                    generation.manifest_row.bytes())) {
                rev3_storage_error("sink write order/count/checksum disagrees");
            }

            flush_bounded_batch_();
            CatalogCloseIndexes(catalog_indexes_);
            catalog_indexes_ = nullptr;
            CommandCounterIncrement();
            if (!ActiveSnapshotSet()) {
                rev3_storage_error(
                    "READY reread requires an active MVCC snapshot");
            }
            Snapshot active_snapshot = GetActiveSnapshot();
            if (!IsMVCCSnapshot(active_snapshot)) {
                rev3_storage_error(
                    "READY reread active snapshot is not MVCC");
            }
            UpdateActiveSnapshotCommandId();
            if (active_snapshot->curcid != GetCurrentCommandId(false)) {
                rev3_storage_error(
                    "READY reread active command ID was not refreshed");
            }
            validation_snapshot_ = RegisterSnapshot(active_snapshot);
            if (validation_snapshot_ == nullptr
                || validation_snapshot_->regd_count == 0) {
                rev3_storage_error("READY reread snapshot was not registered");
            }

            validation_source_proof_.emplace(
                derive_rev3_authoritative_source_proof_pg(
                    identity_, query_key_, validation_snapshot_));
            Rev3AuthoritativeSourceProof& source_proof =
                *validation_source_proof_;
            if (!source_proof.valid_
                || source_proof.snapshot_ != validation_snapshot_
                || source_proof.database_oid_ != identity_.database_oid
                || source_proof.tablespace_oid_ != identity_.tablespace_oid
                || source_proof.main_relfilenode_
                    != identity_.main_relfilenode
                || source_proof.user_relid_ != identity_.user_relid
                || source_proof.column_attno_ != identity_.column_attno
                || source_proof.index_name_ != identity_.index_name
                || source_proof.table_name_ != identity_.table_name
                || source_proof.source_.sequence_rowno != query_key_) {
                rev3_storage_error(
                    "READY authoritative source proof binding disagrees");
            }
            const std::uint64_t compatibility_checksum =
                bioseq::indexing::minimizer_generation_compatibility_checksum(
                    generation.manifest);
            if (generation.manifest.alphabet_code
                    != source_proof.source_.alphabet_code
                || generation.manifest.storage_unit_byte_width
                    != source_proof.source_.storage_unit_byte_width
                || generation.manifest.target_length
                    != source_proof.source_.locator.total_bp_size
                || generation.head.compatibility_checksum
                    != compatibility_checksum
                || generation.head.source_content_checksum
                    != source_proof.source_content_checksum_
                || generation.manifest.source_content_checksum
                    != source_proof.source_content_checksum_) {
                rev3_storage_error(
                    "READY authoritative source content/compatibility disagrees");
            }
            source_proof.source_.compatibility_checksum =
                compatibility_checksum;
            const std::uint64_t required_source_identity =
                rev3_source_identity(
                    identity_.main_relfilenode, source_proof.source_);
            if (required_source_identity == 0
                || generation.head.source_identity
                    != required_source_identity
                || generation.manifest.source_identity
                    != required_source_identity) {
                rev3_storage_error(
                    "READY authoritative source identity disagrees");
            }
            proven_source_identity = required_source_identity;
            proven_source_content_checksum =
                source_proof.source_content_checksum_;
            proven_compatibility_checksum = compatibility_checksum;
            source_proof.invalidate_();
            validation_source_proof_.reset();

            const Rev3IndexSchemaInspection schema =
                inspect_rev3_index_schema(relation_);
            if (!schema) {
                rev3_storage_error("READY relation schema changed");
            }
            proven_relation_oid = RelationGetRelid(relation_);
            proven_locator = relation_->rd_locator;
            validation_primary_index_ = index_open(
                schema.primary_index_oid, AccessShareLock);

            ScanKeyData metadata_keys[4];
            ScanKeyInit(&metadata_keys[0], kRev3AttQueryKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&metadata_keys[1], kRev3AttGenerationId,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
            ScanKeyInit(&metadata_keys[2], kRev3AttPositionSegment,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
            ScanKeyInit(&metadata_keys[3], kRev3AttSubKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(DIRECTORY_SUB_KEY)));
            validation_scan_ = systable_beginscan_ordered(
                relation_, validation_primary_index_, validation_snapshot_,
                4, metadata_keys);
            HeapTuple tuple = systable_getnext_ordered(
                validation_scan_, ForwardScanDirection);
            if (!HeapTupleIsValid(tuple)) {
                rev3_storage_error("READY manifest row is missing");
            }
            Rev3LoadedPgRow manifest_row =
                load_rev3_pg_row_or_error(relation_, tuple);
            if (manifest_row.query_key != query_key_
                || manifest_row.address != generation.manifest_row.address
                || manifest_row.checksum
                    != generation.manifest_row.row_checksum
                || !std::ranges::equal(
                    manifest_row.bytes, generation.manifest_row.bytes())) {
                manifest_row.release();
                rev3_storage_error("READY manifest reread disagrees");
            }
            manifest_row.release();

            Crc64Ecma182 reread_directory_checksum;
            std::uint64_t reread_directory_bytes = 0;
            const std::span<const std::byte> expected_directory =
                generation.directory.encoded_bytes();
            for (std::uint32_t chunk_no = 1;
                 chunk_no <= generation.manifest.directory_chunk_count;
                 ++chunk_no) {
                tuple = systable_getnext_ordered(
                    validation_scan_, ForwardScanDirection);
                if (!HeapTupleIsValid(tuple)) {
                    rev3_storage_error("READY directory chunk is missing");
                }
                Rev3LoadedPgRow directory_row =
                    load_rev3_pg_row_or_error(relation_, tuple);
                const std::uint64_t offset64 =
                    static_cast<std::uint64_t>(chunk_no - 1u)
                    * bioseq::indexing::kMinimizerExtentPayloadBytes;
                const std::size_t offset = static_cast<std::size_t>(offset64);
                const std::size_t count = std::min<std::size_t>(
                    bioseq::indexing::kMinimizerExtentPayloadBytes,
                    expected_directory.size() - offset);
                const auto expected_bytes = expected_directory.subspan(
                    offset, count);
                if (directory_row.query_key != query_key_
                    || directory_row.address != IndexChunkAddress{
                        .generation_id = generation_id_,
                        .position_segment = -1,
                        .sub_key = DIRECTORY_SUB_KEY,
                        .chunk_no = chunk_no,
                    }
                    || !std::ranges::equal(
                        directory_row.bytes, expected_bytes)) {
                    directory_row.release();
                    rev3_storage_error("READY directory reread disagrees");
                }
                reread_directory_checksum.update(directory_row.bytes);
                if (!checked_add_(reread_directory_bytes,
                        directory_row.bytes.size())) {
                    directory_row.release();
                    rev3_storage_error("READY directory reread bytes overflow");
                }
                directory_row.release();
            }
            if (HeapTupleIsValid(systable_getnext_ordered(
                    validation_scan_, ForwardScanDirection))) {
                rev3_storage_error("READY directory reread has extra rows");
            }
            systable_endscan_ordered(validation_scan_);
            validation_scan_ = nullptr;
            if (reread_directory_bytes
                    != generation.manifest.directory_bytes
                || reread_directory_checksum.checksum()
                    != generation.manifest.directory_checksum) {
                rev3_storage_error(
                    "READY directory aggregate reread disagrees");
            }

            ScanKeyData payload_keys[3];
            ScanKeyInit(&payload_keys[0], kRev3AttQueryKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&payload_keys[1], kRev3AttGenerationId,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
            ScanKeyInit(&payload_keys[2], kRev3AttPositionSegment,
                BTGreaterEqualStrategyNumber, F_INT4GE, Int32GetDatum(0));
            validation_scan_ = systable_beginscan_ordered(
                relation_, validation_primary_index_, validation_snapshot_,
                3, payload_keys);
            const auto directory_entries = generation.directory.entries();
            std::size_t segment_begin_ordinal = 0;
            std::size_t segment_end_ordinal = 0;
            std::size_t entry_ordinal = 0;
            std::uint32_t entry_chunk = 0;
            bool position_extent = false;
            std::uint64_t reread_payload_rows = 0;
            std::uint64_t reread_payload_bytes = 0;
            Crc64Ecma182 extent_checksum;
            const auto find_segment_end = [directory_entries](
                    std::size_t begin) noexcept {
                if (begin >= directory_entries.size()) {
                    return directory_entries.size();
                }
                const std::int32_t segment =
                    directory_entries[begin].position_segment;
                std::size_t end = begin + 1u;
                while (end < directory_entries.size()
                       && directory_entries[end].position_segment == segment) {
                    ++end;
                }
                return end;
            };
            segment_end_ordinal = find_segment_end(0);
            for (tuple = systable_getnext_ordered(
                     validation_scan_, ForwardScanDirection);
                 HeapTupleIsValid(tuple);
                 tuple = systable_getnext_ordered(
                     validation_scan_, ForwardScanDirection)) {
                if (entry_ordinal >= directory_entries.size()) {
                    rev3_storage_error("READY payload reread has extra rows");
                }
                const MinimizerBucketDirEntry& entry =
                    directory_entries[entry_ordinal];
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    rev3_storage_error(
                        "READY directory position sub-key is invalid");
                }
                const std::uint32_t extent_chunk_count = position_extent
                    ? entry.position_chunk_count : entry.control_chunk_count;
                const std::uint64_t extent_bytes = position_extent
                    ? entry.position_encoded_bytes : entry.control_encoded_bytes;
                const std::uint64_t offset =
                    static_cast<std::uint64_t>(entry_chunk)
                    * bioseq::indexing::kMinimizerExtentPayloadBytes;
                const std::uint64_t expected_bytes =
                    std::min<std::uint64_t>(
                        bioseq::indexing::kMinimizerExtentPayloadBytes,
                        extent_bytes - offset);
                Rev3LoadedPgRow payload_row =
                    load_rev3_pg_row_or_error(relation_, tuple);
                if (payload_row.query_key != query_key_
                    || payload_row.address != IndexChunkAddress{
                        .generation_id = generation_id_,
                        .position_segment = entry.position_segment,
                        .sub_key = position_extent
                            ? *position_sub_key : entry.sub_key,
                        .chunk_no = entry_chunk,
                    }
                    || payload_row.bytes.size() != expected_bytes) {
                    payload_row.release();
                    rev3_storage_error("READY payload row/address/size disagrees");
                }
                extent_checksum.update(payload_row.bytes);
                if (!checked_add_(reread_payload_rows, 1)
                    || !checked_add_(reread_payload_bytes,
                        payload_row.bytes.size())) {
                    payload_row.release();
                    rev3_storage_error("READY payload reread counters overflow");
                }
                payload_row.release();
                ++entry_chunk;
                if (entry_chunk == extent_chunk_count) {
                    const std::uint64_t expected_extent_checksum =
                        position_extent
                        ? entry.position_checksum : entry.control_checksum;
                    if (extent_checksum.checksum()
                        != expected_extent_checksum) {
                        rev3_storage_error(position_extent
                            ? "READY position extent checksum disagrees"
                            : "READY control extent checksum disagrees");
                    }
                    extent_checksum = {};
                    entry_chunk = 0;
                    ++entry_ordinal;
                    if (entry_ordinal == segment_end_ordinal) {
                        if (!position_extent) {
                            position_extent = true;
                            entry_ordinal = segment_begin_ordinal;
                        } else {
                            position_extent = false;
                            segment_begin_ordinal = segment_end_ordinal;
                            entry_ordinal = segment_begin_ordinal;
                            segment_end_ordinal = find_segment_end(
                                segment_begin_ordinal);
                        }
                    }
                }
            }
            systable_endscan_ordered(validation_scan_);
            validation_scan_ = nullptr;
            // The manifest payload checksum is over canonical encoder/write
            // order (control then position per partition). The physical PK
            // order is now control namespace then position namespace per
            // segment, so this reread proves every address-bound row and each
            // complete extent checksum instead of recomputing that
            // order-sensitive aggregate. The sink checked it before reread.
            if (segment_begin_ordinal != directory_entries.size()
                || entry_ordinal != directory_entries.size()
                || entry_chunk != 0
                || position_extent
                || reread_payload_rows != expected_payload_rows
                || reread_payload_bytes != expected_payload_bytes
                || reread_payload_bytes != generation.manifest.payload_bytes) {
                rev3_storage_error(
                    "READY complete payload aggregate reread disagrees");
            }

            index_close(validation_primary_index_, AccessShareLock);
            validation_primary_index_ = nullptr;
            UnregisterSnapshot(validation_snapshot_);
            validation_snapshot_ = nullptr;
            relation_close(relation_, RowExclusiveLock);
            relation_ = nullptr;
        }
        PG_CATCH();
        {
            rethrow_after_cleanup_(saved_context);
        }
        PG_END_TRY();

        stage_ = WriteStage::tokenized;
        Rev3IndexAdministrationIdentity proven_identity =
            std::move(identity_);
        return ReadyGenerationToken{
            std::move(proven_identity),
            proven_relation_oid,
            proven_locator,
            advisory_lock_key_,
            query_key_,
            generation_id_,
            proven_source_identity,
            proven_source_content_checksum,
            proven_compatibility_checksum,
            proven_head_bytes,
            proven_head_checksum};
    }
};

static_assert(bioseq::indexing::MinimizerGenerationRowSink<
    IndexStorageFragmentGenerationRowSinkPg>);
static_assert(!std::is_default_constructible_v<
    IndexStorageFragmentGenerationRowSinkPg>);
static_assert(!std::is_copy_constructible_v<
    IndexStorageFragmentGenerationRowSinkPg>);
static_assert(std::is_nothrow_move_constructible_v<
    IndexStorageFragmentGenerationRowSinkPg>);

/// Consume the private complete-reread proof and publish its head atomically.
/// There is no public overload that accepts an arbitrary relation/generation.
inline void publish_ready_minimizer_generation_head_pg(
        ReadyGenerationToken&& token) {
    struct PublicationState {
        Relation relation = nullptr;
        bytea* blob = nullptr;
        bool spi_connected = false;
        std::string sql;
    };
    auto* publication = new PublicationState{};
    std::array<std::byte, MinimizerGenerationHead::encoded_bytes>
        returned_head_bytes{};
    bool returned_valid = false;

    MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        if (!token.valid_
            || !bioseq::indexing::sql_bigint_can_represent(token.query_key_)
            || token.generation_id_ == 0
            || token.identity_.database_oid != MyDatabaseId
            || !TransactionIdIsValid(token.transaction_id_)
            || token.transaction_id_ != GetCurrentTransactionIdIfAny()
            || token.advisory_lock_key_
                != index_advisory_xact_lock_key(
                    token.identity_.database_oid,
                    token.identity_.tablespace_oid,
                    token.identity_.main_relfilenode,
                    token.identity_.column_attno,
                    token.identity_.index_name)) {
            rev3_storage_error("READY publication token is invalid");
        }
        acquire_index_advisory_xact_lock(
            token.identity_.database_oid,
            token.identity_.tablespace_oid,
            token.identity_.main_relfilenode,
            token.identity_.column_attno,
            token.identity_.index_name);
        publication->relation = try_safe_relation_open(
            token.sidecar_relation_oid_, RowExclusiveLock);
        if (publication->relation == nullptr
            || RelationGetNamespace(publication->relation)
                != bioseq_internal_ns()
            || std::string_view{
                RelationGetRelationName(publication->relation)}
                != token.identity_.table_name
            || publication->relation->rd_locator.spcOid
                != token.sidecar_locator_.spcOid
            || publication->relation->rd_locator.dbOid
                != token.sidecar_locator_.dbOid
            || publication->relation->rd_locator.relNumber
                != token.sidecar_locator_.relNumber) {
            rev3_storage_error(
                "READY publication relation identity changed after reread");
        }
        const Rev3IndexSchemaInspection schema =
            inspect_rev3_index_schema(publication->relation);
        if (!schema) {
            rev3_storage_error("READY publication schema changed after reread");
        }
        auto decoded_token_head =
            bioseq::indexing::decode_minimizer_generation_head(
                token.head_bytes_);
        if (!decoded_token_head
            || decoded_token_head->generation_id != token.generation_id_
            || decoded_token_head->source_identity != token.source_identity_
            || decoded_token_head->source_content_checksum
                != token.source_content_checksum_
            || decoded_token_head->compatibility_checksum
                != token.compatibility_checksum_
            || token.head_row_checksum_
                != bioseq::indexing::crc64_index_chunk_row(
                    0, -1, GENERATION_HEAD_SUB_KEY, 0,
                    static_cast<std::uint32_t>(token.head_bytes_.size()),
                    token.head_bytes_)) {
            rev3_storage_error("READY publication token head disagrees");
        }

        publication->blob = reinterpret_cast<bytea*>(
            palloc(VARHDRSZ + token.head_bytes_.size()));
        SET_VARSIZE(publication->blob, VARHDRSZ + token.head_bytes_.size());
        std::memcpy(VARDATA(publication->blob), token.head_bytes_.data(),
            token.head_bytes_.size());
    std::array<Oid, 8> argument_types{
        INT8OID, INT8OID, INT4OID, INT8OID,
        INT4OID, INT4OID, INT8OID, BYTEAOID,
    };
    std::array<Datum, 8> arguments{
        Int64GetDatum(static_cast<std::int64_t>(token.query_key_)),
        Int64GetDatum(0),
        Int32GetDatum(-1),
        Int64GetDatum(static_cast<std::int64_t>(GENERATION_HEAD_SUB_KEY)),
        Int32GetDatum(0),
        Int32GetDatum(static_cast<std::int32_t>(token.head_bytes_.size())),
        Int64GetDatum(checksum_to_pg_bigint(token.head_row_checksum_)),
        PointerGetDatum(publication->blob),
    };
    publication->sql = fmt::format(
        "INSERT INTO bioseq_internal.{} "
        "(query_key,generation_id,position_segment,sub_key,chunk_no,"
        "chunk_bytes,checksum,blob) VALUES "
        "($1,$2,$3,$4,$5,$6,$7,$8) "
        "ON CONFLICT "
        "(query_key,generation_id,position_segment,sub_key,chunk_no) "
        "DO UPDATE SET blob=EXCLUDED.blob, "
        "chunk_bytes=EXCLUDED.chunk_bytes, checksum=EXCLUDED.checksum "
        "RETURNING blob",
        quote_identifier(token.identity_.table_name.c_str()));
    if (SPI_connect() != SPI_OK_CONNECT) {
        rev3_storage_error("SPI_connect failed during READY publication");
    }
    publication->spi_connected = true;
    const int result = SPI_execute_with_args(
        publication->sql.c_str(),
        static_cast<int>(argument_types.size()),
        argument_types.data(),
        arguments.data(),
        nullptr,
        false,
        1);
    if (result == SPI_OK_INSERT_RETURNING && SPI_processed == 1
        && SPI_tuptable != nullptr) {
        bool is_null = false;
        const Datum returned_blob = SPI_getbinval(
            SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &is_null);
        if (!is_null) {
            auto detoasted = bioseq::pg_utils::detoast_packed_as<bytea>(
                bioseq::pg_utils::pg_datum::from_raw(returned_blob));
            const std::size_t bytes = VARSIZE_ANY_EXHDR(detoasted.ptr);
            if (bytes == returned_head_bytes.size()) {
                std::memcpy(returned_head_bytes.data(),
                    VARDATA_ANY(detoasted.ptr), bytes);
                returned_valid = true;
            }
            detoasted.free_if_copy();
        }
    }
        if (SPI_finish() != SPI_OK_FINISH) {
            rev3_storage_error("SPI_finish failed during READY publication");
        }
        publication->spi_connected = false;
        bytea* finished_blob = std::exchange(publication->blob, nullptr);
        pfree(finished_blob);
        Relation finished_relation = std::exchange(
            publication->relation, nullptr);
        relation_close(finished_relation, RowExclusiveLock);
        auto returned_head =
            bioseq::indexing::decode_minimizer_generation_head(
                returned_head_bytes);
        if (!returned_valid || !returned_head
            || !bioseq::indexing::revision3_fixed_wire_returned_exactly(
                token.head_bytes_, returned_head_bytes)
            || *returned_head != *decoded_token_head) {
            rev3_storage_error(
                "READY publication did not return the byte-exact proven head");
        }
        token.invalidate();
        std::string{}.swap(publication->sql);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        if (publication->spi_connected) {
            publication->spi_connected = false;
            PG_TRY();
            {
                (void)SPI_finish();
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        if (publication->blob != nullptr) {
            bytea* cleanup_blob = std::exchange(
                publication->blob, nullptr);
            pfree(cleanup_blob);
        }
        if (publication->relation != nullptr) {
            Relation cleanup_relation = std::exchange(
                publication->relation, nullptr);
            PG_TRY();
            {
                relation_close(cleanup_relation, RowExclusiveLock);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        token.invalidate();
        delete publication;
        ReThrowError(saved_error);
    }
    PG_END_TRY();
    delete publication;
}

struct MinimizerGenerationReadMetadata {
    MinimizerGenerationHead head{};
    MinimizerGenerationManifest manifest{};
    DirView directory{};
};

/// One caller-declared admission group. A group is one exact ordered PG range
/// scan; every request must match one complete directory control/position
/// extent, and every skipped row must be predeclared by that directory.
struct MinimizerGenerationPayloadReadGroup {
    IndexSubKeyRange range{};
    std::vector<IndexSubKeyRequest> requests;
};

struct MinimizerGenerationPayloadPhaseReadStats {
    std::uint64_t plan_calls = 0;
    std::uint64_t planned_request_groups = 0;
    std::uint64_t executed_request_groups = 0;
    std::uint64_t range_scan_calls = 0;
    std::uint64_t physical_heap_rows_read = 0;
    std::uint64_t physical_bytes_read = 0;
    std::uint64_t control_authorization_checks = 0;
    IndexChunkReadStats read{};
};

struct MinimizerGenerationReadStats {
    std::uint64_t relation_opens = 0;
    std::uint64_t index_scans = 0;
    std::uint64_t head_scans = 0;
    std::uint64_t head_rows_read = 0;
    std::uint64_t head_bytes_read = 0;
    std::uint64_t manifest_directory_scans = 0;
    std::uint64_t manifest_rows_read = 0;
    std::uint64_t manifest_bytes_read = 0;
    std::uint64_t directory_chunks_read = 0;
    std::uint64_t directory_bytes_read = 0;
    std::uint64_t metadata_repeated_reads = 0;
    std::uint64_t metadata_heap_rows_read = 0;
    std::uint64_t metadata_bytes_read = 0;
    std::uint64_t payload_heap_rows_read = 0;
    std::uint64_t payload_bytes_read = 0;
    std::uint64_t total_heap_rows_read = 0;
    std::uint64_t total_bytes_read = 0;
    std::uint64_t toast_values_read = 0;
    std::uint64_t detoast_bytes = 0;
    std::uint64_t explicit_copy_bytes = 0;
    std::uint64_t bulk_read_calls = 0;
    std::uint64_t planned_request_groups = 0;
    std::uint64_t executed_request_groups = 0;
    std::uint64_t range_scan_calls = 0;
    std::uint64_t cache_lookups = 0;
    std::uint64_t cache_populations = 0;
    std::uint64_t source_parent_rows_read = 0;
    std::uint64_t source_catalog_rows_read = 0;
    std::uint64_t source_catalog_index_scans = 0;
    std::uint64_t source_fragment_rows_read = 0;
    std::uint64_t source_decoded_bp = 0;
    std::uint64_t source_storage_incarnations_checked = 0;
    IndexChunkReadStats payload{};
    MinimizerGenerationPayloadPhaseReadStats controls{};
    MinimizerGenerationPayloadPhaseReadStats positions{};
};

enum class Rev3SnapshotRegistration : std::uint8_t {
    register_copy,
    borrow_registered,
};

/// Inactive revision-3 target session. It binds one fresh heap OID, its exact
/// five-key primary index, one executor-owned data snapshot, and one query key
/// for the complete head -> metadata -> controls -> positions -> finish
/// lifecycle. Both payload phases retain the same open relations and snapshot.
class IndexStorageFragmentGenerationReadSessionPg {
private:
    enum class State : std::uint8_t {
        opened,
        head_read,
        metadata_unattested,
        metadata_ready,
        controls_planned,
        controls_complete,
        positions_planned,
        payload_complete,
        finished,
        failed,
        moved,
    };

    enum class PayloadPhase : std::uint8_t {
        controls,
        positions,
    };

    using LoadedRow = Rev3LoadedPgRow;

    struct PlannedGroup {
        IndexSubKeyRange range{};
        std::vector<IndexSubKeyRequest> requests;
        std::size_t first_directory_ordinal = 0;
        std::size_t end_directory_ordinal = 0;
        std::uint64_t skip_budget_rows = 0;
        bool position_extent = false;
        bool executed = false;
    };

    struct RuntimeRequest {
        IndexSubKeyRequest request{};
        std::uint64_t end_chunk_no = 0;
        std::uint64_t next_chunk_no = 0;
        std::uint64_t total_bytes = 0;
        Crc64Ecma182 checksum{};
    };

    std::string table_name_;
    Relation heap_relation_ = nullptr;
    Relation primary_index_relation_ = nullptr;
    SysScanDesc active_scan_ = nullptr;
    Snapshot snapshot_ = nullptr;
    bool snapshot_registered_by_session_ = false;
    std::uint64_t query_key_ = 0;
    ExpectedMinimizerCompatibility expected_{};
    MinimizerGenerationPayloadProfile expected_profile_{};
    State state_ = State::opened;
    // Permits precede every allocation they cover, so reverse member
    // destruction cannot return admitted bytes while an owned view/buffer is
    // still alive.
    bioseq::parallelism::MemoryPermit metadata_permit_;
    bioseq::parallelism::MemoryPermit plan_permit_;
    bioseq::parallelism::MemoryPermit runtime_permit_;
    bioseq::parallelism::MemoryPermit control_authorization_permit_;
    std::optional<MinimizerGenerationHead> head_;
    std::optional<MinimizerGenerationReadMetadata> metadata_;
    std::vector<std::byte> directory_bytes_scratch_;
    std::vector<PlannedGroup> planned_groups_;
    std::vector<RuntimeRequest> runtime_requests_;
    std::vector<std::uint32_t> authorized_position_ordinals_;
    std::size_t next_group_ = 0;
    MinimizerGenerationReadStats stats_{};

    [[noreturn]] void fail_(std::string_view message) {
        state_ = State::failed;
        rev3_storage_error(message);
    }

    void checked_add_(
            std::uint64_t& value,
            std::uint64_t increment,
            std::string_view message) {
        if (increment
            > (std::numeric_limits<std::uint64_t>::max)() - value) {
            fail_(message);
        }
        value += increment;
    }

    void release_owned_state_() noexcept {
        // Free/reset every covered allocation before returning admission.
        std::vector<std::byte>{}.swap(directory_bytes_scratch_);
        std::vector<PlannedGroup>{}.swap(planned_groups_);
        std::vector<RuntimeRequest>{}.swap(runtime_requests_);
        std::vector<std::uint32_t>{}.swap(authorized_position_ordinals_);
        metadata_.reset();
        head_.reset();
        runtime_permit_.release();
        plan_permit_.release();
        control_authorization_permit_.release();
        metadata_permit_.release();
    }

    void release_phase_plan_() noexcept {
        std::vector<PlannedGroup>{}.swap(planned_groups_);
        next_group_ = 0;
        plan_permit_.release();
    }

    void release_control_authorization_() noexcept {
        std::vector<std::uint32_t>{}.swap(authorized_position_ordinals_);
        control_authorization_permit_.release();
    }

    void checked_add_read_stats_(
            IndexChunkReadStats& destination,
            const IndexChunkReadStats& increment) {
        checked_add_(destination.relation_opens, increment.relation_opens,
            "payload relation-open counter overflow");
        checked_add_(destination.index_scans, increment.index_scans,
            "payload index-scan counter overflow");
        checked_add_(destination.requested_ranges, increment.requested_ranges,
            "payload requested-range counter overflow");
        checked_add_(destination.heap_rows_read, increment.heap_rows_read,
            "payload heap-row counter overflow");
        checked_add_(destination.toast_values_read, increment.toast_values_read,
            "payload toast-value counter overflow");
        checked_add_(destination.detoast_bytes, increment.detoast_bytes,
            "payload detoast-byte counter overflow");
        checked_add_(destination.explicit_copy_bytes,
            increment.explicit_copy_bytes,
            "payload explicit-copy counter overflow");
        checked_add_(destination.wanted_subkeys, increment.wanted_subkeys,
            "payload wanted-sub-key counter overflow");
        checked_add_(destination.read_subkeys, increment.read_subkeys,
            "payload read-sub-key counter overflow");
        checked_add_(destination.wanted_extents, increment.wanted_extents,
            "payload wanted-extent counter overflow");
        checked_add_(destination.read_extents, increment.read_extents,
            "payload read-extent counter overflow");
        checked_add_(destination.wanted_chunks, increment.wanted_chunks,
            "payload wanted-chunk counter overflow");
        checked_add_(destination.skipped_rows, increment.skipped_rows,
            "payload skipped-row counter overflow");
        checked_add_(destination.skip_budget_rows,
            increment.skip_budget_rows,
            "payload skip-budget counter overflow");
        checked_add_(destination.repeated_chunk_reads,
            increment.repeated_chunk_reads,
            "payload repeated-read counter overflow");
        checked_add_(destination.delivered_chunks,
            increment.delivered_chunks,
            "payload delivered-chunk counter overflow");
        checked_add_(destination.delivered_bytes, increment.delivered_bytes,
            "payload delivered-byte counter overflow");
    }

    [[nodiscard]] static constexpr bool counter_is_sum_(
            std::uint64_t total,
            std::uint64_t left,
            std::uint64_t right) noexcept {
        return total >= left && total - left == right;
    }

    [[nodiscard]] static constexpr bool read_stats_are_sum_(
            const IndexChunkReadStats& total,
            const IndexChunkReadStats& left,
            const IndexChunkReadStats& right) noexcept {
        return counter_is_sum_(total.relation_opens,
                   left.relation_opens, right.relation_opens)
            && counter_is_sum_(total.index_scans,
                   left.index_scans, right.index_scans)
            && counter_is_sum_(total.requested_ranges,
                   left.requested_ranges, right.requested_ranges)
            && counter_is_sum_(total.heap_rows_read,
                   left.heap_rows_read, right.heap_rows_read)
            && counter_is_sum_(total.toast_values_read,
                   left.toast_values_read, right.toast_values_read)
            && counter_is_sum_(total.detoast_bytes,
                   left.detoast_bytes, right.detoast_bytes)
            && counter_is_sum_(total.explicit_copy_bytes,
                   left.explicit_copy_bytes, right.explicit_copy_bytes)
            && counter_is_sum_(total.wanted_subkeys,
                   left.wanted_subkeys, right.wanted_subkeys)
            && counter_is_sum_(total.read_subkeys,
                   left.read_subkeys, right.read_subkeys)
            && counter_is_sum_(total.wanted_extents,
                   left.wanted_extents, right.wanted_extents)
            && counter_is_sum_(total.read_extents,
                   left.read_extents, right.read_extents)
            && counter_is_sum_(total.wanted_chunks,
                   left.wanted_chunks, right.wanted_chunks)
            && counter_is_sum_(total.skipped_rows,
                   left.skipped_rows, right.skipped_rows)
            && counter_is_sum_(total.skip_budget_rows,
                   left.skip_budget_rows, right.skip_budget_rows)
            && counter_is_sum_(total.repeated_chunk_reads,
                   left.repeated_chunk_reads, right.repeated_chunk_reads)
            && counter_is_sum_(total.delivered_chunks,
                   left.delivered_chunks, right.delivered_chunks)
            && counter_is_sum_(total.delivered_bytes,
                   left.delivered_bytes, right.delivered_bytes);
    }

    [[nodiscard]] static constexpr bool phase_equations_hold_(
            const MinimizerGenerationPayloadPhaseReadStats& phase) noexcept {
        return phase.plan_calls == 1
            && phase.planned_request_groups
                == phase.executed_request_groups
            && phase.executed_request_groups == phase.range_scan_calls
            && phase.read.relation_opens == 0
            && phase.read.index_scans == phase.range_scan_calls
            && phase.read.requested_ranges == phase.range_scan_calls
            && phase.read.heap_rows_read == phase.physical_heap_rows_read
            && phase.read.heap_rows_read
                == phase.read.delivered_chunks + phase.read.skipped_rows
            && phase.read.toast_values_read == 0
            && phase.read.detoast_bytes == 0
            && phase.read.explicit_copy_bytes == 0
            && phase.read.wanted_subkeys == phase.read.read_subkeys
            && phase.read.wanted_extents == phase.read.read_extents
            && phase.read.wanted_chunks == phase.read.delivered_chunks
            && phase.read.skipped_rows == phase.read.skip_budget_rows
            && phase.read.repeated_chunk_reads == 0
            && phase.physical_bytes_read >= phase.read.delivered_bytes;
    }

    void close_relations_() noexcept {
        if (active_scan_ != nullptr) {
            systable_endscan_ordered(active_scan_);
            active_scan_ = nullptr;
        }
        if (primary_index_relation_ != nullptr) {
            index_close(primary_index_relation_, AccessShareLock);
            primary_index_relation_ = nullptr;
        }
        if (heap_relation_ != nullptr) {
            relation_close(heap_relation_, AccessShareLock);
            heap_relation_ = nullptr;
        }
        if (snapshot_registered_by_session_ && snapshot_ != nullptr) {
            UnregisterSnapshot(snapshot_);
        }
        snapshot_ = nullptr;
        snapshot_registered_by_session_ = false;
        release_owned_state_();
    }

    /// PostgreSQL ERROR is a longjmp, so C++ destructors cannot be the cleanup
    /// boundary. Detach every backend resource before attempting its release,
    /// swallow cleanup-time ERRORs independently, and rethrow the original.
    [[noreturn]] void rethrow_after_cleanup_(
            MemoryContext saved_context) noexcept {
        MemoryContextSwitchTo(saved_context);
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        if (active_scan_ != nullptr) {
            SysScanDesc cleanup_scan = std::exchange(active_scan_, nullptr);
            PG_TRY();
            {
                systable_endscan_ordered(cleanup_scan);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        if (primary_index_relation_ != nullptr) {
            Relation cleanup_index = std::exchange(
                primary_index_relation_, nullptr);
            PG_TRY();
            {
                index_close(cleanup_index, AccessShareLock);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        if (heap_relation_ != nullptr) {
            Relation cleanup_heap = std::exchange(heap_relation_, nullptr);
            PG_TRY();
            {
                relation_close(cleanup_heap, AccessShareLock);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        if (snapshot_registered_by_session_ && snapshot_ != nullptr) {
            Snapshot cleanup_snapshot = std::exchange(snapshot_, nullptr);
            snapshot_registered_by_session_ = false;
            PG_TRY();
            {
                UnregisterSnapshot(cleanup_snapshot);
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        snapshot_ = nullptr;
        snapshot_registered_by_session_ = false;
        release_owned_state_();
        std::string{}.swap(table_name_);
        ReThrowError(saved_error);
    }

    struct ProjectedExtentRange {
        index_sub_key_t logical_begin = 0;
        index_sub_key_t logical_end = 0;
        bool position_extent = false;
    };

    /// Project one physical range onto the logical/control directory key
    /// namespace. A range may cover controls or positions, never cross the
    /// namespace boundary.
    [[nodiscard]] static std::optional<ProjectedExtentRange>
    project_extent_range_(const IndexSubKeyRange& range) noexcept {
        if (range.begin < 0 || range.begin >= range.end
            || range.end > TEMP_RUN_BASE) {
            return std::nullopt;
        }
        if (range.end <= POSITION_EXTENT_BASE) {
            return ProjectedExtentRange{
                .logical_begin = range.begin,
                .logical_end = range.end,
                .position_extent = false,
            };
        }
        if (range.begin >= POSITION_EXTENT_BASE) {
            return ProjectedExtentRange{
                .logical_begin = range.begin - POSITION_EXTENT_BASE,
                .logical_end = range.end - POSITION_EXTENT_BASE,
                .position_extent = true,
            };
        }
        return std::nullopt;
    }

    [[nodiscard]] static bool request_matches_extent_(
            const IndexSubKeyRequest& request,
            const MinimizerBucketDirEntry& entry,
            bool position_extent) noexcept {
        const auto position_sub_key =
            derive_minimizer_position_extent_sub_key(entry.sub_key);
        if (!position_sub_key) {
            return false;
        }
        const bool control = !position_extent
            && request.sub_key == entry.sub_key
            && request.first_chunk_no
                == entry.control_first_chunk
            && request.expected_chunk_count == entry.control_chunk_count
            && request.expected_total_bytes == entry.control_encoded_bytes
            && request.expected_checksum == entry.control_checksum;
        const bool positions = position_extent
            && request.sub_key == *position_sub_key
            && request.first_chunk_no
                == entry.position_first_chunk
            && request.expected_chunk_count == entry.position_chunk_count
            && request.expected_total_bytes == entry.position_encoded_bytes
            && request.expected_checksum == entry.position_checksum;
        return control || positions;
    }

public:
    IndexStorageFragmentGenerationReadSessionPg() = delete;
    IndexStorageFragmentGenerationReadSessionPg(
            std::string_view table_name,
            std::uint64_t query_key,
            Snapshot executor_snapshot,
            ExpectedMinimizerCompatibility expected,
            Rev3SnapshotRegistration snapshot_registration =
                Rev3SnapshotRegistration::register_copy)
        : table_name_(table_name),
          snapshot_(executor_snapshot),
          query_key_(query_key),
          expected_(expected) {
        MemoryContext saved_context = CurrentMemoryContext;
        PG_TRY();
        {
            if (snapshot_ == nullptr
                || !IsMVCCSnapshot(snapshot_)
                || snapshot_->regd_count == 0) {
                rev3_storage_error(
                    "generation read requires a registered executor MVCC snapshot");
            }
            if (!bioseq::indexing::sql_bigint_can_represent(query_key_)) {
                rev3_storage_error(
                    "generation read query key exceeds signed SQL range");
            }
            const bioseq::indexing::MinimizerSchemeIdentity expected_scheme{
                .id = expected_.minimizer_scheme_id,
                .version = expected_.minimizer_scheme_version,
            };
            const auto expected_profile =
                bioseq::indexing::minimizer_generation_payload_profile(
                    expected_.wire_format,
                    expected_scheme,
                    expected_.kmer_size);
            const bool supported_storage_width =
                expected_.storage_unit_byte_width == 1u
                || expected_.storage_unit_byte_width == 2u
                || expected_.storage_unit_byte_width == 4u
                || expected_.storage_unit_byte_width == 8u;
            if (!bioseq::indexing::
                    is_supported_minimizer_generation_wire_format(
                        expected_.wire_format)
                || !expected_profile
                || expected_profile->wire_format != expected_.wire_format
                || expected_profile->minimizer_scheme != expected_scheme
                || expected_profile->kmer_size != expected_.kmer_size
                || !supported_storage_width
                || expected_.window_size == 0
                || expected_.gpu_abi_version
                    != expected_profile->gpu_abi_version) {
                rev3_storage_error(
                    "generation read expected minimizer compatibility is invalid");
            }
            expected_profile_ = *expected_profile;
            if (snapshot_registration
                    == Rev3SnapshotRegistration::register_copy) {
                snapshot_ = RegisterSnapshot(snapshot_);
                snapshot_registered_by_session_ = true;
            }
            const Oid relation_oid = get_relname_relid(
                table_name_.c_str(), bioseq_internal_ns());
            heap_relation_ = OidIsValid(relation_oid)
                ? try_safe_relation_open(relation_oid, AccessShareLock)
                : nullptr;
            if (heap_relation_ == nullptr
                || RelationGetNamespace(heap_relation_) != bioseq_internal_ns()
                || std::string_view{RelationGetRelationName(heap_relation_)}
                    != table_name_) {
                rev3_storage_error(
                    "revision-3 generation relation is missing or renamed");
            }
            const Rev3IndexSchemaInspection schema =
                inspect_rev3_index_schema(heap_relation_);
            if (!schema) {
                rev3_storage_error(fmt::format(
                    "generation relation schema mismatch: {}",
                    rev3_index_schema_error_name(schema.error)));
            }
            primary_index_relation_ = index_open(
                schema.primary_index_oid, AccessShareLock);
            stats_.relation_opens = 1;
        }
        PG_CATCH();
        {
            state_ = State::failed;
            rethrow_after_cleanup_(saved_context);
        }
        PG_END_TRY();
    }
    IndexStorageFragmentGenerationReadSessionPg(
        const IndexStorageFragmentGenerationReadSessionPg&) = delete;
    IndexStorageFragmentGenerationReadSessionPg& operator=(
        const IndexStorageFragmentGenerationReadSessionPg&) = delete;
    IndexStorageFragmentGenerationReadSessionPg(
            IndexStorageFragmentGenerationReadSessionPg&& other) noexcept
        : table_name_(std::move(other.table_name_)),
          heap_relation_(std::exchange(other.heap_relation_, nullptr)),
          primary_index_relation_(std::exchange(
              other.primary_index_relation_, nullptr)),
          active_scan_(std::exchange(other.active_scan_, nullptr)),
          snapshot_(std::exchange(other.snapshot_, nullptr)),
          snapshot_registered_by_session_(std::exchange(
              other.snapshot_registered_by_session_, false)),
          query_key_(other.query_key_),
          expected_(other.expected_),
          expected_profile_(other.expected_profile_),
          state_(other.state_),
          metadata_permit_(std::move(other.metadata_permit_)),
          plan_permit_(std::move(other.plan_permit_)),
          runtime_permit_(std::move(other.runtime_permit_)),
          control_authorization_permit_(std::move(
              other.control_authorization_permit_)),
          head_(std::move(other.head_)),
          metadata_(std::move(other.metadata_)),
          directory_bytes_scratch_(std::move(
              other.directory_bytes_scratch_)),
          planned_groups_(std::move(other.planned_groups_)),
          runtime_requests_(std::move(other.runtime_requests_)),
          authorized_position_ordinals_(std::move(
              other.authorized_position_ordinals_)),
          next_group_(other.next_group_),
          stats_(other.stats_) {
        other.state_ = State::moved;
    }
    IndexStorageFragmentGenerationReadSessionPg& operator=(
            IndexStorageFragmentGenerationReadSessionPg&& other) noexcept {
        if (this != &other) {
            close_relations_();
            table_name_ = std::move(other.table_name_);
            heap_relation_ = std::exchange(other.heap_relation_, nullptr);
            primary_index_relation_ = std::exchange(
                other.primary_index_relation_, nullptr);
            active_scan_ = std::exchange(other.active_scan_, nullptr);
            snapshot_ = std::exchange(other.snapshot_, nullptr);
            snapshot_registered_by_session_ = std::exchange(
                other.snapshot_registered_by_session_, false);
            query_key_ = other.query_key_;
            expected_ = other.expected_;
            expected_profile_ = other.expected_profile_;
            state_ = other.state_;
            metadata_permit_ = std::move(other.metadata_permit_);
            plan_permit_ = std::move(other.plan_permit_);
            runtime_permit_ = std::move(other.runtime_permit_);
            control_authorization_permit_ = std::move(
                other.control_authorization_permit_);
            head_ = std::move(other.head_);
            metadata_ = std::move(other.metadata_);
            directory_bytes_scratch_ = std::move(
                other.directory_bytes_scratch_);
            planned_groups_ = std::move(other.planned_groups_);
            runtime_requests_ = std::move(other.runtime_requests_);
            authorized_position_ordinals_ = std::move(
                other.authorized_position_ordinals_);
            next_group_ = other.next_group_;
            stats_ = other.stats_;
            other.state_ = State::moved;
        }
        return *this;
    }
    ~IndexStorageFragmentGenerationReadSessionPg() { close_relations_(); }

    [[nodiscard]] MinimizerGenerationHead read_active_head_once() {
        MinimizerGenerationHead result{};
        MemoryContext saved_context = CurrentMemoryContext;
        PG_TRY();
        {
            if (state_ != State::opened) {
                checked_add_(stats_.metadata_repeated_reads, 1,
                    "repeated metadata counter overflow");
                fail_("active generation head was read more than once or out of order");
            }
            ScanKeyData keys[5];
            ScanKeyInit(&keys[0], kRev3AttQueryKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&keys[1], kRev3AttGenerationId,
                BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
            ScanKeyInit(&keys[2], kRev3AttPositionSegment,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
            ScanKeyInit(&keys[3], kRev3AttSubKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(
                    GENERATION_HEAD_SUB_KEY)));
            ScanKeyInit(&keys[4], kRev3AttChunkNo,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
            active_scan_ = systable_beginscan_ordered(
                heap_relation_, primary_index_relation_, snapshot_, 5, keys);
            HeapTuple tuple = systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
            if (!HeapTupleIsValid(tuple)) {
                fail_("active generation head is missing");
            }
            LoadedRow row = load_rev3_pg_row_or_error(
                heap_relation_, tuple);
            std::array<std::byte, MinimizerGenerationHead::encoded_bytes> bytes{};
            if (row.query_key != query_key_
                || row.address != IndexChunkAddress{
                    .generation_id = 0,
                    .position_segment = -1,
                    .sub_key = GENERATION_HEAD_SUB_KEY,
                    .chunk_no = 0,
                }
                || row.bytes.size() != bytes.size()) {
                row.release();
                fail_("active generation head row shape disagrees");
            }
            std::copy(row.bytes.begin(), row.bytes.end(), bytes.begin());
            row.release();
            if (HeapTupleIsValid(systable_getnext_ordered(
                    active_scan_, ForwardScanDirection))) {
                fail_("active generation head scan returned duplicate rows");
            }
            systable_endscan_ordered(active_scan_);
            active_scan_ = nullptr;
            auto decoded =
                bioseq::indexing::decode_minimizer_generation_head(bytes);
            if (!decoded || !bioseq::indexing::sql_bigint_can_represent(
                    decoded->generation_id)) {
                fail_("active generation head fixed wire is invalid");
            }
            if (decoded->wire_format != expected_profile_.wire_format) {
                fail_(
                    "active generation head wire format disagrees with reader");
            }
            result = *decoded;
            head_ = result;
            state_ = State::head_read;
            stats_.head_scans = 1;
            stats_.head_rows_read = 1;
            stats_.head_bytes_read = MinimizerGenerationHead::encoded_bytes;
            stats_.index_scans = 1;
            stats_.explicit_copy_bytes = MinimizerGenerationHead::encoded_bytes;
        }
        PG_CATCH();
        {
            state_ = State::failed;
            rethrow_after_cleanup_(saved_context);
        }
        PG_END_TRY();
        return result;
    }

    [[nodiscard]] const MinimizerGenerationReadMetadata&
    read_manifest_directory_once(const MinimizerGenerationHead& expected_head) {
        MemoryContext saved_context = CurrentMemoryContext;
        PG_TRY();
        {
        if (state_ != State::head_read || !head_
            || expected_head != *head_) {
            checked_add_(stats_.metadata_repeated_reads, 1,
                "repeated metadata counter overflow");
            fail_("manifest/directory read repeated, early, or changed head identity");
        }
        ScanKeyData keys[4];
        ScanKeyInit(&keys[0], kRev3AttQueryKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[1], kRev3AttGenerationId,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                expected_head.generation_id)));
        ScanKeyInit(&keys[2], kRev3AttPositionSegment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
        ScanKeyInit(&keys[3], kRev3AttSubKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(DIRECTORY_SUB_KEY)));
        active_scan_ = systable_beginscan_ordered(
            heap_relation_, primary_index_relation_, snapshot_, 4, keys);
        HeapTuple tuple = systable_getnext_ordered(
            active_scan_, ForwardScanDirection);
        if (!HeapTupleIsValid(tuple)) {
            fail_("READY generation manifest is missing");
        }
        LoadedRow manifest_row = load_rev3_pg_row_or_error(
            heap_relation_, tuple);
        std::array<std::byte, MinimizerGenerationManifest::encoded_bytes>
            manifest_bytes{};
        if (manifest_row.query_key != query_key_
            || manifest_row.address != IndexChunkAddress{
                .generation_id = expected_head.generation_id,
                .position_segment = -1,
                .sub_key = DIRECTORY_SUB_KEY,
                .chunk_no = 0,
            }
            || manifest_row.bytes.size() != manifest_bytes.size()) {
            manifest_row.release();
            fail_("READY generation manifest row shape disagrees");
        }
        std::copy(manifest_row.bytes.begin(), manifest_row.bytes.end(),
            manifest_bytes.begin());
        manifest_row.release();
        auto manifest = bioseq::indexing::decode_minimizer_generation_manifest(
            manifest_bytes);
        if (!manifest
            || manifest->generation_id != expected_head.generation_id
            || manifest->source_identity != expected_head.source_identity
            || manifest->directory_bytes
                > (std::numeric_limits<std::size_t>::max)()) {
            fail_("READY generation manifest fixed wire disagrees");
        }
        if (!bioseq::indexing::validate_minimizer_generation_compatibility(
                *manifest, expected_)) {
            fail_("READY generation minimizer compatibility disagrees");
        }
        const std::uint64_t expected_directory_chunks =
            manifest->directory_bytes
                / bioseq::indexing::kMinimizerExtentPayloadBytes
            + (manifest->directory_bytes
                    % bioseq::indexing::kMinimizerExtentPayloadBytes
                != 0 ? 1u : 0u);
        if (expected_directory_chunks != manifest->directory_chunk_count) {
            fail_("READY manifest directory chunk equation disagrees");
        }
        const std::uint64_t entry_count = manifest->directory_bytes
            / MinimizerBucketDirEntry::encoded_bytes;
        std::uint64_t admitted_bytes = manifest->directory_bytes;
        if (manifest->directory_bytes
                > (std::numeric_limits<std::uint64_t>::max)()
                    - manifest->directory_bytes
            || entry_count
                > (std::numeric_limits<std::uint64_t>::max)()
                    / sizeof(MinimizerBucketDirEntry)) {
            fail_("READY metadata admission size overflow");
        }
        admitted_bytes += manifest->directory_bytes;
        std::uint64_t entry_storage =
            entry_count * sizeof(MinimizerBucketDirEntry);
        if (entry_storage
            > (std::numeric_limits<std::uint64_t>::max)()
                - admitted_bytes) {
            fail_("READY metadata admission size overflow");
        }
        admitted_bytes += entry_storage;
        auto metadata_permit =
            bioseq::parallelism::MemoryAdmissionController::instance()
                .try_reserve(admitted_bytes);
        if (!metadata_permit) {
            fail_("READY metadata memory admission was denied");
        }
        metadata_permit_ = std::move(*metadata_permit);
        directory_bytes_scratch_.clear();
        directory_bytes_scratch_.reserve(
            static_cast<std::size_t>(manifest->directory_bytes));
        for (std::uint32_t chunk_no = 1;
             chunk_no <= manifest->directory_chunk_count; ++chunk_no) {
            tuple = systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
            if (!HeapTupleIsValid(tuple)) {
                fail_("READY generation directory has missing chunks");
            }
            LoadedRow directory_row = load_rev3_pg_row_or_error(
                heap_relation_, tuple);
            const std::uint64_t offset =
                static_cast<std::uint64_t>(chunk_no - 1u)
                * bioseq::indexing::kMinimizerExtentPayloadBytes;
            const std::uint64_t expected_bytes = std::min<std::uint64_t>(
                bioseq::indexing::kMinimizerExtentPayloadBytes,
                manifest->directory_bytes - offset);
            if (directory_row.query_key != query_key_
                || directory_row.address != IndexChunkAddress{
                    .generation_id = expected_head.generation_id,
                    .position_segment = -1,
                    .sub_key = DIRECTORY_SUB_KEY,
                    .chunk_no = chunk_no,
                }
                || directory_row.bytes.size() != expected_bytes) {
                directory_row.release();
                fail_("READY generation directory row shape disagrees");
            }
            directory_bytes_scratch_.insert(directory_bytes_scratch_.end(),
                directory_row.bytes.begin(), directory_row.bytes.end());
            directory_row.release();
        }
        if (HeapTupleIsValid(systable_getnext_ordered(
                active_scan_, ForwardScanDirection))) {
            fail_("READY generation directory scan returned extra rows");
        }
        systable_endscan_ordered(active_scan_);
        active_scan_ = nullptr;
        if (directory_bytes_scratch_.size() != manifest->directory_bytes
            || bioseq::indexing::crc64_ecma182(
                std::span<const std::byte>{directory_bytes_scratch_})
                != manifest->directory_checksum) {
            fail_("READY generation directory aggregate checksum disagrees");
        }
        auto directory = DirView::decode(
            directory_bytes_scratch_, expected_profile_);
        if (!directory) {
            fail_("READY generation directory fixed wire is invalid");
        }
        metadata_.emplace(MinimizerGenerationReadMetadata{
            .head = expected_head,
            .manifest = *manifest,
            .directory = std::move(*directory),
        });
        std::vector<std::byte>{}.swap(directory_bytes_scratch_);
        if (auto metadata = bioseq::indexing::validate_minimizer_generation_metadata(
                expected_head, metadata_->manifest, metadata_->directory);
            !metadata) {
            fail_("READY generation metadata equations disagree");
        }
        std::optional<std::pair<std::int32_t, index_sub_key_t>>
            previous_storage_key;
        for (const MinimizerBucketDirEntry& entry
             : metadata_->directory.entries()) {
            const std::pair storage_key{
                entry.position_segment, entry.sub_key};
            if (previous_storage_key
                && !(*previous_storage_key < storage_key)) {
                fail_("READY directory is not in physical storage order");
            }
            previous_storage_key = storage_key;
        }
        metadata_permit_.shrink_to(
            manifest->directory_bytes + entry_storage);
        state_ = State::metadata_unattested;
        stats_.manifest_directory_scans = 1;
        stats_.manifest_rows_read = 1;
        stats_.manifest_bytes_read =
            MinimizerGenerationManifest::encoded_bytes;
        stats_.directory_chunks_read = manifest->directory_chunk_count;
        stats_.directory_bytes_read = manifest->directory_bytes;
        stats_.metadata_heap_rows_read = 2u
            + manifest->directory_chunk_count;
        stats_.metadata_bytes_read = MinimizerGenerationHead::encoded_bytes
            + MinimizerGenerationManifest::encoded_bytes
            + manifest->directory_bytes;
        checked_add_(stats_.index_scans, 1,
            "metadata index scan count overflow");
        checked_add_(stats_.explicit_copy_bytes,
            MinimizerGenerationManifest::encoded_bytes,
            "metadata copy byte count overflow");
        checked_add_(stats_.explicit_copy_bytes,
            manifest->directory_bytes,
            "directory copy byte count overflow");
        }
        PG_CATCH();
        {
            state_ = State::failed;
            rethrow_after_cleanup_(saved_context);
        }
        PG_END_TRY();
        return *metadata_;
    }

    /// Bind metadata to an independently reopened source row. This is the only
    /// transition to metadata_ready: source identity alone is insufficient.
    void validate_full_source_attestation(
            Rev3AuthoritativeSourceProof&& proof) {
        MemoryContext saved_context = CurrentMemoryContext;
        PG_TRY();
        {
            if (state_ != State::metadata_unattested
                || !head_ || !metadata_) {
                proof.invalidate_();
                fail_("READY source proof was consumed out of order");
            }
            if (!proof.valid_
                || proof.snapshot_ != snapshot_
                || proof.database_oid_ != MyDatabaseId
                || proof.source_.sequence_rowno != query_key_
                || proof.table_name_ != table_name_) {
                proof.invalidate_();
                fail_("READY source proof relation/query/snapshot binding disagrees");
            }
            const std::uint64_t compatibility_checksum =
                bioseq::indexing::minimizer_generation_compatibility_checksum(
                    metadata_->manifest);
            proof.source_.compatibility_checksum = compatibility_checksum;
            const std::uint64_t source_identity = rev3_source_identity(
                proof.main_relfilenode_, proof.source_);
            const std::uint64_t source_content_checksum =
                proof.source_content_checksum_;
            if (source_identity == 0
                || metadata_->manifest.alphabet_code
                    != proof.source_.alphabet_code
                || metadata_->manifest.storage_unit_byte_width
                    != proof.source_.storage_unit_byte_width
                || metadata_->manifest.target_length
                    != proof.source_.locator.total_bp_size
                || head_->source_identity != source_identity
                || metadata_->manifest.source_identity != source_identity
                || head_->source_content_checksum
                    != source_content_checksum
                || metadata_->manifest.source_content_checksum
                    != source_content_checksum
                || head_->compatibility_checksum
                    != compatibility_checksum
                || bioseq::indexing::minimizer_generation_compatibility_checksum(
                    metadata_->manifest) != compatibility_checksum) {
                proof.invalidate_();
                fail_("READY metadata/source attestation disagrees");
            }
            proof.invalidate_();
            state_ = State::metadata_ready;
        }
        PG_CATCH();
        {
            proof.invalidate_();
            state_ = State::failed;
            rethrow_after_cleanup_(saved_context);
        }
        PG_END_TRY();
    }

    /// Timed cold-reader transition. Only the constant-work catalog proof is
    /// accepted here; the full decoded proof has a separate, explicitly named
    /// untimed writer/admin transition above.
    void validate_source_attestation(Rev3CatalogSourceProof&& proof) {
        MemoryContext saved_context = CurrentMemoryContext;
        PG_TRY();
        {
            if (state_ != State::metadata_unattested
                || !head_ || !metadata_) {
                proof.invalidate_();
                fail_("READY catalog source proof was consumed out of order");
            }
            if (!proof.valid_
                || !proof.counters_.timed_equation_holds()
                || proof.snapshot_ != snapshot_
                || proof.database_oid_ != MyDatabaseId
                || proof.source_.sequence_rowno != query_key_
                || proof.table_name_ != table_name_) {
                proof.invalidate_();
                fail_("READY catalog source proof binding/work equation disagrees");
            }
            const std::uint64_t compatibility_checksum =
                bioseq::indexing::minimizer_generation_compatibility_checksum(
                    metadata_->manifest);
            proof.source_.compatibility_checksum = compatibility_checksum;
            const std::uint64_t source_identity = rev3_source_identity(
                proof.main_relfilenode_, proof.source_);
            const std::uint64_t source_content_checksum =
                proof.source_content_checksum_;
            if (source_identity == 0
                || metadata_->manifest.alphabet_code
                    != proof.source_.alphabet_code
                || metadata_->manifest.storage_unit_byte_width
                    != proof.source_.storage_unit_byte_width
                || metadata_->manifest.target_length
                    != proof.source_.locator.total_bp_size
                || head_->source_identity != source_identity
                || metadata_->manifest.source_identity != source_identity
                || head_->source_content_checksum
                    != source_content_checksum
                || metadata_->manifest.source_content_checksum
                    != source_content_checksum
                || head_->compatibility_checksum
                    != compatibility_checksum) {
                proof.invalidate_();
                fail_("READY metadata/catalog source attestation disagrees");
            }
            stats_.source_parent_rows_read = proof.counters_.parent_rows_read;
            stats_.source_catalog_rows_read = proof.counters_.catalog_rows_read;
            stats_.source_catalog_index_scans =
                proof.counters_.catalog_index_scans;
            stats_.source_fragment_rows_read =
                proof.counters_.fragment_rows_read;
            stats_.source_decoded_bp = proof.counters_.decoded_bp;
            stats_.source_storage_incarnations_checked =
                proof.counters_.storage_incarnations_checked;
            proof.invalidate_();
            state_ = State::metadata_ready;
        }
        PG_CATCH();
        {
            proof.invalidate_();
            state_ = State::failed;
            rethrow_after_cleanup_(saved_context);
        }
        PG_END_TRY();
    }

private:
    void plan_phase_groups_(
            PayloadPhase phase,
            std::span<const MinimizerGenerationPayloadReadGroup> groups) {
        MemoryContext saved_context = CurrentMemoryContext;
        PG_TRY();
        {
            const bool position_phase = phase == PayloadPhase::positions;
            const State required_state = position_phase
                ? State::controls_complete : State::metadata_ready;
            if (state_ != required_state || !metadata_) {
                fail_(position_phase
                    ? "position payload requests were planned before control payload completed or more than once"
                    : "control payload requests were planned before metadata or more than once");
            }
            if (!planned_groups_.empty() || next_group_ != 0 || plan_permit_
                || !runtime_requests_.empty() || runtime_permit_
                || active_scan_ != nullptr) {
                fail_("payload phase planning found leaked prior-phase state");
            }
            if (!position_phase
                && (!authorized_position_ordinals_.empty()
                    || control_authorization_permit_)) {
                fail_("control payload authorization state was already populated");
            }
            std::uint64_t request_count = 0;
            for (const MinimizerGenerationPayloadReadGroup& group : groups) {
                checked_add_(request_count, group.requests.size(),
                    "payload plan request count overflow");
            }
            std::uint64_t plan_bytes = 0;
            if (groups.size()
                    > (std::numeric_limits<std::uint64_t>::max)()
                        / sizeof(PlannedGroup)
                || request_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        / sizeof(IndexSubKeyRequest)
                || (!position_phase
                    && request_count
                        > (std::numeric_limits<std::uint64_t>::max)()
                            / sizeof(std::uint32_t))) {
                fail_("payload plan admission size overflow");
            }
            plan_bytes = groups.size() * sizeof(PlannedGroup);
            checked_add_(plan_bytes,
                request_count * sizeof(IndexSubKeyRequest),
                "payload plan admission size overflow");
            if (!position_phase && request_count != 0) {
                const std::uint64_t authorization_bytes =
                    request_count * sizeof(std::uint32_t);
                auto authorization_permit =
                    bioseq::parallelism::MemoryAdmissionController::instance()
                        .try_reserve(authorization_bytes);
                if (!authorization_permit) {
                    fail_("control payload authorization memory admission was denied");
                }
                control_authorization_permit_ =
                    std::move(*authorization_permit);
                authorized_position_ordinals_.clear();
                authorized_position_ordinals_.reserve(
                    static_cast<std::size_t>(request_count));
            }
            if (plan_bytes != 0) {
                auto plan_permit =
                    bioseq::parallelism::MemoryAdmissionController::instance()
                        .try_reserve(plan_bytes);
                if (!plan_permit) {
                    fail_("payload plan memory admission was denied");
                }
                plan_permit_ = std::move(*plan_permit);
            }
            planned_groups_.clear();
            planned_groups_.reserve(groups.size());
            std::optional<IndexSubKeyRange> previous_range;
            const auto entries = metadata_->directory.entries();
            std::uint64_t authorization_checks = 0;
            for (const MinimizerGenerationPayloadReadGroup& input : groups) {
                const IndexSubKeyRange& range = input.range;
                const auto projected_range = project_extent_range_(range);
                if (range.generation_id != metadata_->manifest.generation_id
                    || range.position_segment < 0
                    || !projected_range
                    || projected_range->position_extent != position_phase
                    || input.requests.empty()) {
                    fail_(position_phase
                        ? "position payload group range is empty, unqualified, or outside the position namespace"
                        : "control payload group range is empty, unqualified, or outside the control namespace");
                }
                if (previous_range) {
                    const auto previous_key = std::tuple{
                        previous_range->generation_id,
                        previous_range->position_segment,
                        previous_range->begin};
                    const auto current_key = std::tuple{
                        range.generation_id,
                        range.position_segment,
                        range.begin};
                    if (!(previous_key < current_key)
                        || (previous_range->generation_id
                                == range.generation_id
                            && previous_range->position_segment
                                == range.position_segment
                            && range.begin < previous_range->end)) {
                        fail_("payload group ranges are not sorted and disjoint");
                    }
                }
                previous_range = range;
                using DirectoryKey =
                    std::pair<std::int32_t, index_sub_key_t>;
                const auto entry_less_than_key = [](
                        const MinimizerBucketDirEntry& entry,
                        const DirectoryKey& key) noexcept {
                    return DirectoryKey{
                        entry.position_segment, entry.sub_key} < key;
                };
                const auto first_entry = std::lower_bound(
                    entries.begin(), entries.end(),
                    DirectoryKey{range.position_segment,
                        projected_range->logical_begin},
                    entry_less_than_key);
                const auto end_entry = std::lower_bound(
                    first_entry, entries.end(),
                    DirectoryKey{range.position_segment,
                        projected_range->logical_end},
                    entry_less_than_key);
                const std::size_t first_ordinal = static_cast<std::size_t>(
                    first_entry - entries.begin());
                const std::size_t end_ordinal = static_cast<std::size_t>(
                    end_entry - entries.begin());
                if (first_ordinal == end_ordinal) {
                    fail_("payload group range covers no directory entry");
                }
                std::size_t request_entry_cursor = first_ordinal;
                std::uint64_t wanted_chunks = 0;
                std::optional<std::tuple<index_sub_key_t, std::uint32_t>>
                    previous_request;
                std::uint64_t previous_request_end = 0;
                for (const IndexSubKeyRequest& request : input.requests) {
                    const auto request_key = std::tuple{
                        request.sub_key, request.first_chunk_no};
                    if (previous_request && !(*previous_request < request_key)) {
                        fail_("payload requests are not strictly sorted");
                    }
                    if (previous_request
                        && std::get<0>(*previous_request) == request.sub_key
                        && request.first_chunk_no < previous_request_end) {
                        fail_("payload request intervals overlap");
                    }
                    previous_request = request_key;
                    previous_request_end =
                        static_cast<std::uint64_t>(request.first_chunk_no)
                        + request.expected_chunk_count;
                    const auto logical_request_sub_key =
                        project_minimizer_extent_sub_key(
                            request.sub_key,
                            projected_range->position_extent);
                    if (request.generation_id != range.generation_id
                        || request.position_segment != range.position_segment
                        || request.sub_key < range.begin
                        || request.sub_key >= range.end
                        || !logical_request_sub_key
                        || request.expected_chunk_count == 0
                        || previous_request_end == 0
                        || previous_request_end - 1u
                            > static_cast<std::uint64_t>(
                                (std::numeric_limits<std::int32_t>::max)())) {
                        fail_("payload request is outside its qualified range");
                    }
                    while (request_entry_cursor < end_ordinal
                           && entries[request_entry_cursor].sub_key
                                < *logical_request_sub_key) {
                        ++request_entry_cursor;
                    }
                    if (request_entry_cursor == end_ordinal
                        || entries[request_entry_cursor].sub_key
                            != *logical_request_sub_key
                        || !request_matches_extent_(
                            request, entries[request_entry_cursor],
                            projected_range->position_extent)) {
                        fail_("payload request does not equal one directory extent");
                    }
                    const std::uint32_t directory_ordinal =
                        entries[request_entry_cursor].partition_id;
                    if (!position_phase) {
                        if (!authorized_position_ordinals_.empty()
                            && authorized_position_ordinals_.back()
                                >= directory_ordinal) {
                            fail_("control payload directory authorizations are not strictly sorted");
                        }
                        authorized_position_ordinals_.push_back(
                            directory_ordinal);
                    } else {
                        if (!std::binary_search(
                                authorized_position_ordinals_.begin(),
                                authorized_position_ordinals_.end(),
                                directory_ordinal)) {
                            fail_("position payload request has no delivered control extent");
                        }
                        checked_add_(authorization_checks, 1,
                            "position control-authorization counter overflow");
                    }
                    checked_add_(wanted_chunks,
                        request.expected_chunk_count,
                        "payload wanted chunk count overflow");
                }
                std::uint64_t declared_chunks = 0;
                for (std::size_t ordinal = first_ordinal;
                     ordinal < end_ordinal; ++ordinal) {
                    checked_add_(declared_chunks,
                        projected_range->position_extent
                            ? entries[ordinal].position_chunk_count
                            : entries[ordinal].control_chunk_count,
                        "payload declared chunk count overflow");
                }
                if (declared_chunks < wanted_chunks) {
                    fail_("payload skip budget is smaller than requested chunks");
                }
                planned_groups_.push_back(PlannedGroup{
                    .range = range,
                    .requests = input.requests,
                    .first_directory_ordinal = first_ordinal,
                    .end_directory_ordinal = end_ordinal,
                    .skip_budget_rows = declared_chunks - wanted_chunks,
                    .position_extent = projected_range->position_extent,
                });
            }
            auto& phase_stats = position_phase
                ? stats_.positions : stats_.controls;
            checked_add_(phase_stats.plan_calls, 1,
                "payload phase plan-call counter overflow");
            checked_add_(phase_stats.planned_request_groups,
                planned_groups_.size(),
                "payload phase planned-group counter overflow");
            checked_add_(phase_stats.control_authorization_checks,
                authorization_checks,
                "position control-authorization counter overflow");
            checked_add_(stats_.bulk_read_calls, 1,
                "payload plan-call counter overflow");
            checked_add_(stats_.planned_request_groups,
                planned_groups_.size(),
                "payload planned-group counter overflow");
            next_group_ = 0;
            if (position_phase) {
                release_control_authorization_();
                state_ = planned_groups_.empty()
                    ? State::payload_complete : State::positions_planned;
            } else {
                state_ = planned_groups_.empty()
                    ? State::controls_complete : State::controls_planned;
            }
        }
        PG_CATCH();
        {
            state_ = State::failed;
            rethrow_after_cleanup_(saved_context);
        }
        PG_END_TRY();
    }

public:
    void plan_control_groups(
            std::span<const MinimizerGenerationPayloadReadGroup> groups) {
        plan_phase_groups_(PayloadPhase::controls, groups);
    }

    void plan_position_groups(
            std::span<const MinimizerGenerationPayloadReadGroup> groups) {
        plan_phase_groups_(PayloadPhase::positions, groups);
    }

    template <class Sink>
    requires bioseq::indexing::index_chunk_reader_fn<Sink>
    [[nodiscard]] IndexChunkReadStats read_chunks_in_range(
            std::size_t group_index,
            IndexChunkCachePolicy cache_policy,
            Sink&& sink) {
        IndexChunkReadStats local{};
        MemoryContext saved_context = CurrentMemoryContext;
        PG_TRY();
        {
            const bool position_phase = state_ == State::positions_planned;
            if ((state_ != State::controls_planned && !position_phase)
                || group_index != next_group_
                || group_index >= planned_groups_.size()) {
                checked_add_(stats_.payload.repeated_chunk_reads, 1,
                    "payload repeated-read counter overflow");
                fail_("payload group was repeated, reordered, or not planned");
            }
            if (cache_policy != IndexChunkCachePolicy::bypass_one_shot) {
                fail_("attested generation session requires bypass_one_shot");
            }
            PlannedGroup& group = planned_groups_[group_index];
            if (group.position_extent != position_phase
                || group.executed || active_scan_ != nullptr
                || !runtime_requests_.empty() || runtime_permit_) {
                checked_add_(stats_.payload.repeated_chunk_reads, 1,
                    "payload repeated-read counter overflow");
                fail_("payload group was executed more than once or session state leaked");
            }
            if (group.requests.size()
                > (std::numeric_limits<std::uint64_t>::max)()
                    / sizeof(RuntimeRequest)) {
                fail_("payload runtime admission size overflow");
            }
            const std::uint64_t runtime_bytes =
                group.requests.size() * sizeof(RuntimeRequest);
            auto runtime_permit =
                bioseq::parallelism::MemoryAdmissionController::instance()
                    .try_reserve(runtime_bytes);
            if (!runtime_permit) {
                fail_("payload runtime memory admission was denied");
            }
            runtime_permit_ = std::move(*runtime_permit);
            runtime_requests_.clear();
            runtime_requests_.reserve(group.requests.size());
            local.index_scans = 1;
            local.requested_ranges = 1;
            local.wanted_extents = group.requests.size();
            local.skip_budget_rows = group.skip_budget_rows;
            std::optional<index_sub_key_t> previous_subkey;
            for (const IndexSubKeyRequest& request : group.requests) {
                runtime_requests_.push_back(RuntimeRequest{
                    .request = request,
                    .end_chunk_no = static_cast<std::uint64_t>(
                        request.first_chunk_no) + request.expected_chunk_count,
                    .next_chunk_no = request.first_chunk_no,
                });
                if (!previous_subkey || *previous_subkey != request.sub_key) {
                    checked_add_(local.wanted_subkeys, 1,
                        "payload wanted sub-key count overflow");
                    previous_subkey = request.sub_key;
                }
                checked_add_(local.wanted_chunks,
                    request.expected_chunk_count,
                    "payload local wanted chunk count overflow");
            }

            ScanKeyData keys[5];
            ScanKeyInit(&keys[0], kRev3AttQueryKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&keys[1], kRev3AttGenerationId,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(
                    group.range.generation_id)));
            ScanKeyInit(&keys[2], kRev3AttPositionSegment,
                BTEqualStrategyNumber, F_INT4EQ,
                Int32GetDatum(group.range.position_segment));
            ScanKeyInit(&keys[3], kRev3AttSubKey,
                BTGreaterEqualStrategyNumber, F_INT8GE,
                Int64GetDatum(static_cast<std::int64_t>(group.range.begin)));
            ScanKeyInit(&keys[4], kRev3AttSubKey,
                BTLessStrategyNumber, F_INT8LT,
                Int64GetDatum(static_cast<std::int64_t>(group.range.end)));
            active_scan_ = systable_beginscan_ordered(
                heap_relation_, primary_index_relation_, snapshot_, 5, keys);

            const auto entries = metadata_->directory.entries();
            std::size_t directory_ordinal = group.first_directory_ordinal;
            std::uint64_t expected_chunk_no = 0;
            std::size_t request_cursor = 0;
            std::uint64_t physical_payload_bytes = 0;
            for (HeapTuple tuple;
                 (tuple = systable_getnext_ordered(
                      active_scan_, ForwardScanDirection)) != nullptr;) {
                LoadedRow row = load_rev3_pg_row_or_error(
                    heap_relation_, tuple);
                checked_add_(local.heap_rows_read, 1,
                    "payload heap row count overflow");
                checked_add_(physical_payload_bytes, row.bytes.size(),
                    "payload physical byte count overflow");
                if (row.query_key != query_key_
                    || row.address.generation_id
                        != group.range.generation_id
                    || row.address.position_segment
                        != group.range.position_segment
                    || row.address.sub_key < group.range.begin
                    || row.address.sub_key >= group.range.end) {
                    row.release();
                    fail_("payload scan returned a row outside its qualified range");
                }

                if (directory_ordinal >= group.end_directory_ordinal) {
                    row.release();
                    fail_("payload scan returned more rows than its directory range");
                }
                const MinimizerBucketDirEntry& entry =
                    entries[directory_ordinal];
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    row.release();
                    fail_("payload directory position sub-key is invalid");
                }
                const index_sub_key_t expected_sub_key =
                    group.position_extent
                    ? *position_sub_key : entry.sub_key;
                const std::uint64_t entry_chunk_count = group.position_extent
                    ? entry.position_chunk_count : entry.control_chunk_count;
                if (row.address.sub_key != expected_sub_key
                    || row.address.chunk_no != expected_chunk_no
                    || expected_chunk_no >= entry_chunk_count) {
                    row.release();
                    fail_("payload physical rows do not exactly equal directory order");
                }

                while (request_cursor < runtime_requests_.size()) {
                    RuntimeRequest& request =
                        runtime_requests_[request_cursor];
                    const bool row_after_request =
                        row.address.sub_key > request.request.sub_key
                        || (row.address.sub_key == request.request.sub_key
                            && static_cast<std::uint64_t>(
                                row.address.chunk_no) >= request.end_chunk_no);
                    if (!row_after_request) {
                        break;
                    }
                    if (request.next_chunk_no != request.end_chunk_no
                        || request.total_bytes
                            != request.request.expected_total_bytes
                        || request.checksum.checksum()
                            != request.request.expected_checksum) {
                        row.release();
                        fail_("payload extent aggregate count/bytes/checksum disagrees");
                    }
                    ++request_cursor;
                }

                RuntimeRequest* matched = nullptr;
                if (request_cursor < runtime_requests_.size()) {
                    RuntimeRequest& request =
                        runtime_requests_[request_cursor];
                    if (row.address.sub_key == request.request.sub_key
                        && row.address.chunk_no
                            >= request.request.first_chunk_no
                        && static_cast<std::uint64_t>(row.address.chunk_no)
                            < request.end_chunk_no) {
                        matched = &request;
                    }
                }
                if (matched == nullptr) {
                    checked_add_(local.skipped_rows, 1,
                        "payload skipped row count overflow");
                } else {
                    if (matched->next_chunk_no != row.address.chunk_no) {
                        row.release();
                        fail_("payload extent is missing, duplicate, or non-contiguous");
                    }
                    checked_add_(matched->total_bytes, row.bytes.size(),
                        "payload extent byte count overflow");
                    matched->checksum.update(row.bytes);
                    sink(row.address,
                        reinterpret_cast<const char*>(row.bytes.data()),
                        row.bytes.size());
                    ++matched->next_chunk_no;
                    checked_add_(local.delivered_chunks, 1,
                        "payload delivered chunk count overflow");
                    checked_add_(local.delivered_bytes, row.bytes.size(),
                        "payload delivered byte count overflow");
                }
                row.release();
                ++expected_chunk_no;
                if (expected_chunk_no == entry_chunk_count) {
                    ++directory_ordinal;
                    expected_chunk_no = 0;
                }
            }
            systable_endscan_ordered(active_scan_);
            active_scan_ = nullptr;

            if (directory_ordinal != group.end_directory_ordinal
                || expected_chunk_no != 0) {
                fail_("payload scan ended before its complete directory range");
            }
            for (; request_cursor < runtime_requests_.size(); ++request_cursor) {
                const RuntimeRequest& request =
                    runtime_requests_[request_cursor];
                if (request.next_chunk_no != request.end_chunk_no
                    || request.total_bytes
                        != request.request.expected_total_bytes
                    || request.checksum.checksum()
                        != request.request.expected_checksum) {
                    fail_("payload extent aggregate count/bytes/checksum disagrees");
                }
            }
            local.read_subkeys = local.wanted_subkeys;
            local.read_extents = local.wanted_extents;
            if (local.delivered_chunks != local.wanted_chunks
                || local.heap_rows_read
                    != local.delivered_chunks + local.skipped_rows
                || local.skipped_rows != local.skip_budget_rows) {
                fail_("payload proof-counter equation disagrees");
            }

            group.executed = true;
            ++next_group_;
            const bool phase_complete = next_group_ == planned_groups_.size();
            auto& phase_stats = position_phase
                ? stats_.positions : stats_.controls;
            checked_add_(phase_stats.executed_request_groups, 1,
                "payload phase executed-group counter overflow");
            checked_add_(phase_stats.range_scan_calls, 1,
                "payload phase range-scan counter overflow");
            checked_add_(phase_stats.physical_heap_rows_read,
                local.heap_rows_read,
                "payload phase physical-row counter overflow");
            checked_add_(phase_stats.physical_bytes_read,
                physical_payload_bytes,
                "payload phase physical-byte counter overflow");
            checked_add_read_stats_(phase_stats.read, local);
            checked_add_(stats_.executed_request_groups, 1,
                "executed request group count overflow");
            checked_add_(stats_.range_scan_calls, 1,
                "payload range scan count overflow");
            checked_add_(stats_.index_scans, 1,
                "total index scan count overflow");
            checked_add_read_stats_(stats_.payload, local);
            stats_.payload_heap_rows_read = stats_.payload.heap_rows_read;
            checked_add_(stats_.payload_bytes_read, physical_payload_bytes,
                "payload physical byte count overflow");
            std::vector<RuntimeRequest>{}.swap(runtime_requests_);
            runtime_permit_.release();
            if (phase_complete) {
                release_phase_plan_();
                state_ = position_phase
                    ? State::payload_complete : State::controls_complete;
            }
        }
        PG_CATCH();
        {
            state_ = State::failed;
            rethrow_after_cleanup_(saved_context);
        }
        PG_END_TRY();
        return local;
    }

    [[nodiscard]] MinimizerGenerationReadStats finish() {
        MinimizerGenerationReadStats result{};
        MemoryContext saved_context = CurrentMemoryContext;
        PG_TRY();
        {
            if (state_ != State::payload_complete
                || next_group_ != 0 || !planned_groups_.empty()
                || !runtime_requests_.empty() || plan_permit_
                || runtime_permit_ || control_authorization_permit_
                || !authorized_position_ordinals_.empty()
                || active_scan_ != nullptr || heap_relation_ == nullptr
                || primary_index_relation_ == nullptr || snapshot_ == nullptr) {
                fail_("generation read finished before position payload completed or with leaked phase state");
            }
            std::uint64_t expected_metadata_rows = 0;
            checked_add_(expected_metadata_rows, stats_.head_rows_read,
                "metadata proof row equation overflow");
            checked_add_(expected_metadata_rows, stats_.manifest_rows_read,
                "metadata proof row equation overflow");
            checked_add_(expected_metadata_rows,
                stats_.directory_chunks_read,
                "metadata proof row equation overflow");
            std::uint64_t expected_metadata_bytes = 0;
            checked_add_(expected_metadata_bytes, stats_.head_bytes_read,
                "metadata proof byte equation overflow");
            checked_add_(expected_metadata_bytes, stats_.manifest_bytes_read,
                "metadata proof byte equation overflow");
            checked_add_(expected_metadata_bytes, stats_.directory_bytes_read,
                "metadata proof byte equation overflow");
            std::uint64_t expected_index_scans = 0;
            checked_add_(expected_index_scans, stats_.head_scans,
                "index scan proof equation overflow");
            checked_add_(expected_index_scans,
                stats_.manifest_directory_scans,
                "index scan proof equation overflow");
            checked_add_(expected_index_scans, stats_.range_scan_calls,
                "index scan proof equation overflow");
            std::uint64_t expected_payload_rows = 0;
            checked_add_(expected_payload_rows,
                stats_.payload.delivered_chunks,
                "payload proof row equation overflow");
            checked_add_(expected_payload_rows, stats_.payload.skipped_rows,
                "payload proof row equation overflow");
            if (stats_.relation_opens != 1
                || stats_.head_scans != 1
                || stats_.head_rows_read != 1
                || stats_.head_bytes_read
                    != MinimizerGenerationHead::encoded_bytes
                || stats_.manifest_directory_scans != 1
                || stats_.manifest_rows_read != 1
                || stats_.manifest_bytes_read
                    != MinimizerGenerationManifest::encoded_bytes
                || stats_.metadata_heap_rows_read != expected_metadata_rows
                || stats_.metadata_bytes_read != expected_metadata_bytes
                || !phase_equations_hold_(stats_.controls)
                || !phase_equations_hold_(stats_.positions)
                || stats_.controls.control_authorization_checks != 0
                || stats_.positions.control_authorization_checks
                    != stats_.positions.read.wanted_extents
                || stats_.positions.read.wanted_extents
                    > stats_.controls.read.wanted_extents
                || !counter_is_sum_(stats_.planned_request_groups,
                    stats_.controls.planned_request_groups,
                    stats_.positions.planned_request_groups)
                || !counter_is_sum_(stats_.executed_request_groups,
                    stats_.controls.executed_request_groups,
                    stats_.positions.executed_request_groups)
                || !counter_is_sum_(stats_.range_scan_calls,
                    stats_.controls.range_scan_calls,
                    stats_.positions.range_scan_calls)
                || !counter_is_sum_(stats_.payload_heap_rows_read,
                    stats_.controls.physical_heap_rows_read,
                    stats_.positions.physical_heap_rows_read)
                || !counter_is_sum_(stats_.payload_bytes_read,
                    stats_.controls.physical_bytes_read,
                    stats_.positions.physical_bytes_read)
                || !read_stats_are_sum_(stats_.payload,
                    stats_.controls.read, stats_.positions.read)
                || stats_.planned_request_groups
                    != stats_.executed_request_groups
                || stats_.bulk_read_calls != 2
                || stats_.range_scan_calls
                    != stats_.executed_request_groups
                || stats_.payload.index_scans
                    != stats_.executed_request_groups
                || stats_.payload.requested_ranges
                    != stats_.executed_request_groups
                || stats_.index_scans != expected_index_scans
                || stats_.payload.heap_rows_read != expected_payload_rows
                || stats_.payload_heap_rows_read
                    != stats_.payload.heap_rows_read
                || stats_.payload_bytes_read
                    < stats_.payload.delivered_bytes
                || stats_.payload.skipped_rows
                    != stats_.payload.skip_budget_rows
                || stats_.payload.wanted_subkeys
                    != stats_.payload.read_subkeys
                || stats_.payload.wanted_extents
                    != stats_.payload.read_extents
                || stats_.payload.wanted_chunks
                    != stats_.payload.delivered_chunks
                || stats_.cache_lookups != 0
                || stats_.cache_populations != 0
                || stats_.toast_values_read != 0
                || stats_.detoast_bytes != 0
                || (stats_.source_catalog_rows_read != 0
                    && (stats_.source_parent_rows_read != 1
                        || stats_.source_catalog_rows_read != 1
                        || stats_.source_catalog_index_scans != 1
                        || stats_.source_fragment_rows_read != 0
                        || stats_.source_decoded_bp != 0
                        || stats_.source_storage_incarnations_checked < 1
                        || stats_.source_storage_incarnations_checked > 2))) {
                fail_("generation read final proof equations disagree");
            }
            stats_.total_heap_rows_read = 0;
            checked_add_(stats_.total_heap_rows_read,
                stats_.metadata_heap_rows_read,
                "total heap row proof equation overflow");
            checked_add_(stats_.total_heap_rows_read,
                stats_.payload_heap_rows_read,
                "total heap row proof equation overflow");
            stats_.total_bytes_read = 0;
            checked_add_(stats_.total_bytes_read,
                stats_.metadata_bytes_read,
                "total byte proof equation overflow");
            checked_add_(stats_.total_bytes_read,
                stats_.payload_bytes_read,
                "total byte proof equation overflow");
            state_ = State::finished;
            result = stats_;
            close_relations_();
        }
        PG_CATCH();
        {
            state_ = State::failed;
            rethrow_after_cleanup_(saved_context);
        }
        PG_END_TRY();
        return result;
    }
};

static_assert(!std::is_default_constructible_v<
    IndexStorageFragmentGenerationReadSessionPg>);
static_assert(!std::is_copy_constructible_v<
    IndexStorageFragmentGenerationReadSessionPg>);
static_assert(std::is_nothrow_move_constructible_v<
    IndexStorageFragmentGenerationReadSessionPg>);

namespace rev3_compile_contract_detail {
struct QualifiedReadSink {
    void operator()(
        const IndexChunkAddress&, const char*, std::size_t) const noexcept {}
};
}  // namespace rev3_compile_contract_detail

template IndexChunkReadStats
IndexStorageFragmentGenerationReadSessionPg::read_chunks_in_range<
    rev3_compile_contract_detail::QualifiedReadSink>(
        std::size_t,
        IndexChunkCachePolicy,
        rev3_compile_contract_detail::QualifiedReadSink&&);

/// Integration-only self-consistent metadata corruption. Both manifest and
/// head source-content fields, the head's manifest checksum, and both physical
/// row CRCs are rewritten together. Only an independent source-row decode can
/// detect the corruption.
inline void rev3_test_rewrite_source_checksum_pg(
        Oid table_oid,
        std::uint64_t query_key,
        std::uint64_t xor_mask) {
    if (!superuser() || xor_mask == 0
        || !bioseq::indexing::sql_bigint_can_represent(query_key)) {
        rev3_storage_error("rev3 checksum corruption input is invalid");
    }
    Relation relation = try_safe_relation_open(table_oid, AccessShareLock);
    if (relation == nullptr
        || RelationGetNamespace(relation) != bioseq_internal_ns()) {
        if (relation != nullptr) relation_close(relation, AccessShareLock);
        rev3_storage_error("rev3 checksum corruption relation is invalid");
    }
    const Rev3IndexSchemaInspection schema = inspect_rev3_index_schema(relation);
    const std::string table_name{RelationGetRelationName(relation)};
    relation_close(relation, AccessShareLock);
    if (!schema) {
        rev3_storage_error("rev3 checksum corruption schema is invalid");
    }

    Snapshot registered_snapshot = RegisterSnapshot(GetActiveSnapshot());
    MinimizerGenerationHead head{};
    MinimizerGenerationManifest manifest{};
    {
        constexpr ExpectedMinimizerCompatibility fixture_expected{
            .wire_format = bioseq::indexing::MinimizerGenerationWireFormat::
                sorted_dense_global64,
            .alphabet_code = 0,
            .storage_unit_byte_width = 8,
            .kmer_size = 15,
            .window_size = 10,
            .gpu_abi_version =
                bioseq::indexing::kMinimizerSortedDenseGpuAbiVersion,
            .minimizer_scheme_id =
                bioseq::indexing::kLegacyMinimapV1SchemeId,
            .minimizer_scheme_version =
                bioseq::indexing::kLegacyMinimapV1SchemeVersion,
        };
        IndexStorageFragmentGenerationReadSessionPg session{
            table_name, query_key, registered_snapshot,
            fixture_expected,
            Rev3SnapshotRegistration::borrow_registered};
        head = session.read_active_head_once();
        manifest = session.read_manifest_directory_once(head).manifest;
    }
    UnregisterSnapshot(registered_snapshot);
    manifest.source_content_checksum ^= xor_mask;
    head.source_content_checksum = manifest.source_content_checksum;
    const auto manifest_bytes =
        bioseq::indexing::encode_minimizer_generation_manifest(manifest);
    if (!manifest_bytes) {
        rev3_storage_error("rev3 checksum corruption could not encode manifest");
    }
    head.manifest_checksum = bioseq::indexing::crc64_ecma182(
        std::span<const std::byte>{*manifest_bytes});
    const auto head_bytes =
        bioseq::indexing::encode_minimizer_generation_head(head);
    if (!head_bytes) {
        rev3_storage_error("rev3 checksum corruption could not encode head");
    }
    const std::uint64_t manifest_row_checksum =
        bioseq::indexing::crc64_index_chunk_row(
            head.generation_id, -1, DIRECTORY_SUB_KEY, 0,
            static_cast<std::uint32_t>(manifest_bytes->size()),
            std::span<const std::byte>{*manifest_bytes});
    const std::uint64_t head_row_checksum =
        bioseq::indexing::crc64_index_chunk_row(
            0, -1, GENERATION_HEAD_SUB_KEY, 0,
            static_cast<std::uint32_t>(head_bytes->size()),
            std::span<const std::byte>{*head_bytes});

    const auto make_bytea = [](std::span<const std::byte> bytes) {
        bytea* value = reinterpret_cast<bytea*>(palloc(VARHDRSZ + bytes.size()));
        SET_VARSIZE(value, VARHDRSZ + bytes.size());
        std::memcpy(VARDATA(value), bytes.data(), bytes.size());
        return value;
    };
    if (SPI_connect() != SPI_OK_CONNECT) {
        rev3_storage_error("rev3 checksum corruption SPI_connect failed");
    }
    const std::string sql = fmt::format(
        "UPDATE bioseq_internal.{} SET blob=$1,chunk_bytes=$2,checksum=$3 "
        "WHERE query_key=$4 AND generation_id=$5 AND position_segment=-1 "
        "AND sub_key=$6 AND chunk_no=0",
        quote_identifier(table_name.c_str()));
    std::array<Oid, 6> argument_types{
        BYTEAOID, INT4OID, INT8OID, INT8OID, INT8OID, INT8OID};
    const auto update_row = [&](std::span<const std::byte> bytes,
            std::uint64_t row_checksum,
            std::uint64_t generation_id,
            index_sub_key_t sub_key) {
        bytea* blob = make_bytea(bytes);
        std::array<Datum, 6> arguments{
            PointerGetDatum(blob),
            Int32GetDatum(static_cast<std::int32_t>(bytes.size())),
            Int64GetDatum(checksum_to_pg_bigint(row_checksum)),
            Int64GetDatum(static_cast<std::int64_t>(query_key)),
            Int64GetDatum(static_cast<std::int64_t>(generation_id)),
            Int64GetDatum(static_cast<std::int64_t>(sub_key)),
        };
        const int status = SPI_execute_with_args(
            sql.c_str(), static_cast<int>(argument_types.size()),
            argument_types.data(), arguments.data(), nullptr, false, 1);
        pfree(blob);
        if (status != SPI_OK_UPDATE || SPI_processed != 1) {
            rev3_storage_error("rev3 checksum corruption UPDATE disagrees");
        }
    };
    update_row(std::span<const std::byte>{*manifest_bytes},
        manifest_row_checksum, head.generation_id, DIRECTORY_SUB_KEY);
    update_row(std::span<const std::byte>{*head_bytes},
        head_row_checksum, 0, GENERATION_HEAD_SUB_KEY);
    SPI_finish();
    CommandCounterIncrement();
}

/// Setup-only identity carried across the mandatory E2E restart. The timed
/// backend must obtain its own identity from statement attestation instead of
/// invoking this pre-restart inspection seam.
struct ReadyMinimizerGenerationIdentity {
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t compatibility_checksum = 0;

    [[nodiscard]] bool operator==(
        const ReadyMinimizerGenerationIdentity&) const = default;
};

/// Read-only, PostgreSQL-owned pre-restart audit for one exact relation row,
/// column and logical minimizer index. It validates the source relfilenode and
/// attribute, the exact eight-column sidecar/PK, the address-bound row CRCs,
/// fixed head/manifest wire, every directory chunk, and all cross-object
/// metadata equations under one registered MVCC snapshot. It intentionally
/// does not activate ordinal 2 or expose a standalone SQL symbol.
[[nodiscard]] inline ReadyMinimizerGenerationIdentity
inspect_ready_minimizer_generation_identity_pg(
        const Rev3IndexAdministrationIdentity& identity,
        std::uint64_t sequence_rowno,
        Snapshot executor_snapshot,
        ExpectedMinimizerCompatibility expected) {
    struct SourceInspectionState {
        Snapshot registered_snapshot = nullptr;
        MemoryContext work_context = nullptr;
        void* session_storage = nullptr;
        IndexStorageFragmentGenerationReadSessionPg* session = nullptr;
        bool session_constructed = false;
        std::optional<Rev3CatalogSourceProof> source_proof;
    };
    auto* source = new SourceInspectionState{};
    ReadyMinimizerGenerationIdentity result{};
    MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        if (identity.database_oid != MyDatabaseId
            || !OidIsValid(identity.user_relid)
            || identity.column_attno <= 0
            || identity.index_name.empty()
            || identity.table_name.empty()
            || !bioseq::indexing::sql_bigint_can_represent(
                sequence_rowno)
            || executor_snapshot == nullptr
            || !IsMVCCSnapshot(executor_snapshot)
            || executor_snapshot->regd_count == 0) {
            rev3_storage_error(
                "generation identity inspection input is invalid");
        }
        source->registered_snapshot = executor_snapshot;
        source->source_proof.emplace(
            derive_rev3_catalog_source_proof_pg(
                identity, sequence_rowno,
                source->registered_snapshot));
        const Rev3AuthoritativeSourceLocator& proven_source =
            source->source_proof->source_for_diagnostics();
        source->work_context = AllocSetContextCreate(
            CurrentMemoryContext, "bioseq rev3 source inspection",
            ALLOCSET_DEFAULT_SIZES);

        source->session_storage = MemoryContextAlloc(
            source->work_context,
            sizeof(IndexStorageFragmentGenerationReadSessionPg));
        source->session = ::new (source->session_storage)
            IndexStorageFragmentGenerationReadSessionPg{
                identity.table_name,
                sequence_rowno,
                source->registered_snapshot,
                expected,
                Rev3SnapshotRegistration::borrow_registered};
        source->session_constructed = true;
        const MinimizerGenerationHead head =
            source->session->read_active_head_once();
        const MinimizerGenerationReadMetadata& metadata =
            source->session->read_manifest_directory_once(head);
        if (metadata.manifest.alphabet_code
                != proven_source.alphabet_code
            || metadata.manifest.storage_unit_byte_width
                != proven_source.storage_unit_byte_width
            || metadata.manifest.target_length
                != proven_source.locator.total_bp_size) {
            rev3_storage_error(
                "generation identity manifest/source row disagrees");
        }
        const std::uint64_t proven_source_content_checksum =
            source->source_proof
                ->source_content_checksum_for_diagnostics();
        const std::uint64_t proven_compatibility_checksum =
            bioseq::indexing::minimizer_generation_compatibility_checksum(
                metadata.manifest);
        source->session->validate_source_attestation(
            std::move(*source->source_proof));
        source->source_proof.reset();
        result = ReadyMinimizerGenerationIdentity{
            .generation_id = metadata.head.generation_id,
            .source_identity = metadata.head.source_identity,
            .source_content_checksum = proven_source_content_checksum,
            .compatibility_checksum = proven_compatibility_checksum,
        };
        source->session->plan_control_groups(
            std::span<const MinimizerGenerationPayloadReadGroup>{});
        source->session->plan_position_groups(
            std::span<const MinimizerGenerationPayloadReadGroup>{});
        const MinimizerGenerationReadStats read_stats =
            source->session->finish();
        if (read_stats.source_parent_rows_read != 1
            || read_stats.source_catalog_rows_read != 1
            || read_stats.source_catalog_index_scans != 1
            || read_stats.source_fragment_rows_read != 0
            || read_stats.source_decoded_bp != 0) {
            rev3_storage_error(
                "generation identity timed source work equation disagrees");
        }
        source->session->~IndexStorageFragmentGenerationReadSessionPg();
        source->session = nullptr;
        source->session_constructed = false;
        source->session_storage = nullptr;

        source->registered_snapshot = nullptr;
        MemoryContext finished_context = std::exchange(
            source->work_context, nullptr);
        MemoryContextDelete(finished_context);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        if (source->session_constructed && source->session != nullptr) {
            auto* cleanup_session = source->session;
            source->session = nullptr;
            source->session_constructed = false;
            PG_TRY();
            {
                cleanup_session->~IndexStorageFragmentGenerationReadSessionPg();
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        source->session_storage = nullptr;
        source->registered_snapshot = nullptr;
        if (source->work_context != nullptr) {
            MemoryContext cleanup_context = std::exchange(
                source->work_context, nullptr);
            MemoryContextDelete(cleanup_context);
        }
        delete source;
        ReThrowError(saved_error);
    }
    PG_END_TRY();
    delete source;
    return result;
}

namespace rev3_pg_compile_contract_detail {
using Publisher = void (*)(ReadyGenerationToken&&);
using Inspector = ReadyMinimizerGenerationIdentity (*)(
    const Rev3IndexAdministrationIdentity&,
    std::uint64_t,
    Snapshot,
    ExpectedMinimizerCompatibility);

static_assert(std::same_as<
    decltype(&publish_ready_minimizer_generation_head_pg), Publisher>);
static_assert(std::same_as<
    decltype(&inspect_ready_minimizer_generation_identity_pg), Inspector>);
static_assert(requires(
    IndexStorageFragmentGenerationRowSinkPg& writer,
    const EncodedMinimizerGeneration& generation,
    const MinimizerGenerationRowView& row) {
    { writer.write(row) }
        -> std::same_as<std::expected<void, MinimizerGenerationWireError>>;
    { writer.validate_ready_generation(generation) }
        -> std::same_as<ReadyGenerationToken>;
});
static_assert(requires(
    IndexStorageFragmentGenerationReadSessionPg& session,
    const MinimizerGenerationHead& head,
    std::span<const MinimizerGenerationPayloadReadGroup> groups) {
    { session.read_active_head_once() }
        -> std::same_as<MinimizerGenerationHead>;
    { session.read_manifest_directory_once(head) }
        -> std::same_as<const MinimizerGenerationReadMetadata&>;
    { session.plan_control_groups(groups) };
    { session.plan_position_groups(groups) };
    { session.finish() } -> std::same_as<MinimizerGenerationReadStats>;
});

constexpr Rev3AuthoritativeSourceLocator kSourceIdentityGolden{
    .sequence_rowno = 17,
    .locator = seq_locator{
        .version = kLocatorVersion,
        .bucket_idx = 9,
        ._pad_2 = 0,
        .shard_idx = 4,
        .num_fragments = 3,
        ._pad_4 = 0,
        .total_bp_size = 12'345,
        .first_sub_row_no = 77,
        .source_epoch = 101,
    },
    .alphabet_code = 1,
    .storage_unit_byte_width = 8,
    .padding_letters = 3,
    .compatibility_checksum = 0x0102030405060708ULL,
};
constexpr auto kSourceIdentityDifferentEpoch = [] {
    auto source = kSourceIdentityGolden;
    ++source.locator.source_epoch;
    return source;
}();
static_assert(rev3_source_identity(42, kSourceIdentityGolden) == 101);
static_assert(rev3_source_identity(42, kSourceIdentityGolden)
    != rev3_source_identity(42, kSourceIdentityDifferentEpoch));
static_assert(rev3_source_identity(42, kSourceIdentityGolden)
    == rev3_source_identity(43, kSourceIdentityGolden));
}  // namespace rev3_pg_compile_contract_detail

/// Result of one inactive revision-3/4 logical-index clear.  The operation is
/// deliberately separate from the legacy `clear()` entry point until every
/// live reader and writer is switched atomically to the generation format.
struct MinimizerGenerationClearResult {
    std::uint64_t dropped_relation_count = 0;
    std::uint64_t deleted_metadata_age_rows = 0;
};

struct CanonicalGenerationSidecarName {
    bool valid = false;
    std::uint8_t bucket_idx = 0;
    std::uint32_t shard_idx = 0;
};

/// Parse only the exact spelling emitted by `format_seqidx_name`: unsigned
/// canonical decimal bucket, one underscore, unsigned canonical decimal shard.
/// Leading zeroes, signs, trailing decorations, overflow, and extra separators
/// are rejected without allocation.
[[nodiscard]] inline CanonicalGenerationSidecarName
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
    return CanonicalGenerationSidecarName{
        .valid = true,
        .bucket_idx = static_cast<std::uint8_t>(bucket),
        .shard_idx = static_cast<std::uint32_t>(shard),
    };
}

/// Clear-only DDL executor.  Unlike the legacy helper, every SPI transition is
/// checked and the caller-owned activity bit remains set if PostgreSQL jumps
/// out of SPI_execute so the enclosing PG_CATCH can finish the connection.
inline void run_checked_generation_clear_ddl(
        const char* ddl,
        bool& spi_active) {
    if (ddl == nullptr || *ddl == '\0' || spi_active) {
        rev3_storage_error("generation clear DDL state is invalid");
    }
    const int connect_rc = SPI_connect();
    if (connect_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq revision-3 index: generation clear DDL SPI_connect failed"),
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
             errmsg("bioseq revision-3 index: generation clear DDL failed"),
             errdetail("execute_rc=%d finish_rc=%d",
                execute_rc, finish_rc)));
    }
}

// =========================================================================
//  IndexStorageFragmentPg  —  CHUNKED_PER_FRAGMENT
// =========================================================================
struct IndexStorageFragmentPg {
    using GenerationReadSession =
        IndexStorageFragmentGenerationReadSessionPg;
    using GenerationRowSink =
        IndexStorageFragmentGenerationRowSinkPg;

    Oid           db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid, user_relid_ = InvalidOid;
    seq_caches*   read_caches_ = nullptr;
    std::int16_t  column_attno_ = 0;
    std::string_view index_name_{};
    std::uint8_t  bucket_idx_ = 0;
    std::uint32_t shard_idx_ = 0;
    std::uint64_t query_key_ = 0;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid   index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr;
    mutable std::size_t   decoded_units_ = 0;
    mutable std::size_t   decoded_bytes_ = 0;
    mutable index_sub_key_t decoded_sub_ = DIRECTORY_SUB_KEY;
    mutable std::size_t   decoded_chunk_ = std::numeric_limits<std::size_t>::max();
    struct CachedChunk {
        // Admission outlives the bytes it covers (reverse member destruction).
        bioseq::parallelism::MemoryPermit permit;
        index_sub_key_t sub_key = DIRECTORY_SUB_KEY;
        std::size_t chunk_no = std::numeric_limits<std::size_t>::max();
        std::size_t payload_bytes = 0;
        std::uint64_t last_used = 0;
        std::vector<char> bytes;
    };
    mutable std::vector<CachedChunk> chunk_cache_;
    mutable std::uint64_t chunk_cache_clock_ = 0;
    mutable std::size_t chunk_cache_bytes_ = 0;
    static constexpr std::size_t kChunkCacheMaxEntries = 8;
    static constexpr std::size_t kChunkCacheMaxBytes = 256ULL << 20;
    std::int64_t* scratch_ = nullptr;
    std::size_t   scratch_units_ = 0;
    std::size_t   run_buffer_override_bytes_ = 0;
    // per-sub_key byte plan recorded by resize/resize_chunks, consumed by write_to_each.
    std::map<index_sub_key_t, std::vector<std::size_t>> chunk_payload_bytes_;
    std::optional<SortedPairRunStore> sorted_runs_;

private:
    // Authority-bearing MPS spill state is intentionally not part of the
    // otherwise duck-typed storage surface.  Callers may observe its immutable
    // binding and mutate it only through the narrow logical-event APIs below.
    std::map<std::uint32_t, SortedPairRunStore> segment_sorted_runs_;
    inline static std::atomic<std::uint64_t>
        next_segment_spill_generation_token_{1u};
    MpsSegmentSpillStorageBinding segment_spill_binding_{};
    bool segment_spill_pg_poisoned_ = false;
    void record_segment_spill_mutation_() noexcept;

public:

    IndexStorageFragmentPg() = default;
    IndexStorageFragmentPg(const IndexStorageFragmentPg&) = delete;
    IndexStorageFragmentPg& operator=(const IndexStorageFragmentPg&) = delete;
    IndexStorageFragmentPg(IndexStorageFragmentPg&& o) noexcept { steal_(std::move(o)); }
    IndexStorageFragmentPg& operator=(IndexStorageFragmentPg&& o) noexcept {
        if (this != &o) { release_(); steal_(std::move(o)); } return *this; }
    ~IndexStorageFragmentPg() { release_(); }

    void steal_(IndexStorageFragmentPg&& o) noexcept {
        db_oid_=o.db_oid_; tablespace_oid_=o.tablespace_oid_; main_relfilenode_=o.main_relfilenode_; user_relid_=o.user_relid_;
        read_caches_=o.read_caches_; column_attno_=o.column_attno_; index_name_=o.index_name_;
        bucket_idx_=o.bucket_idx_; shard_idx_=o.shard_idx_; query_key_=o.query_key_;
        own_mctx_=o.own_mctx_; index_relid_=o.index_relid_;
        decoded_=o.decoded_; decoded_units_=o.decoded_units_; decoded_bytes_=o.decoded_bytes_;
        decoded_sub_=o.decoded_sub_; decoded_chunk_=o.decoded_chunk_;
        chunk_cache_=std::move(o.chunk_cache_);
        chunk_cache_clock_=o.chunk_cache_clock_;
        chunk_cache_bytes_=o.chunk_cache_bytes_;
        scratch_=o.scratch_; scratch_units_=o.scratch_units_;
        run_buffer_override_bytes_=o.run_buffer_override_bytes_;
        chunk_payload_bytes_=std::move(o.chunk_payload_bytes_);
        sorted_runs_=std::move(o.sorted_runs_);
        segment_sorted_runs_=std::move(o.segment_sorted_runs_);
        segment_spill_binding_=o.segment_spill_binding_;
        segment_spill_pg_poisoned_=o.segment_spill_pg_poisoned_;
        o.own_mctx_=nullptr; o.decoded_=nullptr; o.scratch_=nullptr; o.index_relid_=InvalidOid;
        o.decoded_units_=0; o.decoded_bytes_=0; o.scratch_units_=0;
        o.chunk_cache_clock_=0; o.chunk_cache_bytes_=0;
        o.segment_spill_binding_={};
        o.segment_spill_pg_poisoned_=true;
    }
    void release_resident_runs_noexcept() noexcept {
        if (sorted_runs_.has_value()) {
            sorted_runs_->release_resident_runs_noexcept();
        }
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_resident_runs_noexcept();
        }
        // SortedPairRunStore::release_resident_runs_noexcept drops logical
        // memory-only runs; unlike its internal memory-to-BufFile compaction,
        // this is not representation-preserving and must invalidate replay.
        record_segment_spill_mutation_();
    }
    void release_() noexcept {
        release_runs_noexcept();
        chunk_cache_.clear();
        chunk_cache_bytes_ = 0;
        if (own_mctx_ != nullptr) {
            MemoryContextDelete(own_mctx_);
            own_mctx_=nullptr;
            decoded_=nullptr;
            scratch_=nullptr;
        }
    }

    [[noreturn]] void index_error(std::string_view msg) const { bioseq_log<ERR, PG>( "bioseq index: {}", msg); abort(); }

    // ---- validators (translate to ereport on the pg side) ----
    [[nodiscard]] MinimizerHashmapFragmentIndex::SerializedHeader validate_minimizer_blob_or_error(
            const char* p, std::size_t bytes, std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
        using H = MinimizerHashmapFragmentIndex::SerializedHeader;
        using B = MinimizerHashmapFragmentIndex::SerializedBucket;
        if (bytes < sizeof(H)) index_error("minimizer blob truncated header");
        H h{}; std::memcpy(&h, p, sizeof(H));
        if (h.magic != MinimizerHashmapFragmentIndex::kSerializeMagic || h.version != MinimizerHashmapFragmentIndex::kSerializeVersion)
            index_error("minimizer blob magic/version");
        const std::size_t expect = sizeof(H) + sizeof(B)*h.bucket_count + sizeof(std::uint64_t)*h.total_positions;
        if (bytes != expect) index_error("minimizer blob size mismatch");
        if (expected_bucket_count && h.bucket_count != *expected_bucket_count) index_error("minimizer blob bucket_count mismatch");
        return h;
    }
    void validate_minimizer_directory_or_error(const char* p, std::size_t bytes) const {
        if (bytes < 40) index_error("directory truncated");
        std::uint32_t magic{}, version{}; std::memcpy(&magic, p, 4); std::memcpy(&version, p+4, 4);
        if (magic != 0xB1000002u || version != 2u) index_error("directory magic/version");
    }
    void validate_minimizer_chunk_metadata_or_error(index_sub_key_t sk, std::size_t chunk_no,
            std::size_t min_bytes, std::size_t /*alignment*/) const {
        if (chunk_byte_size(sk, chunk_no) < min_bytes) index_error("chunk metadata below minimum size");
    }

    // ---- relation resolution ----
    [[nodiscard]] std::string this_table_name() const {
        return format_seqidx_name(index_name_, db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, bucket_idx_, shard_idx_);
    }

    /// Remove every backend-local object derived from this logical minimizer
    /// sidecar.  Relcache invalidation supplies a physical sidecar OID, whereas
    /// the cache keys use the owning user relation, so schema replacement must
    /// evict by the complete logical identity explicitly.
    void evict_generation_storage_residency_() {
        chunk_cache_.clear();
        chunk_cache_bytes_ = 0;
        chunk_cache_clock_ = 0;
        decoded_sub_ = DIRECTORY_SUB_KEY;
        decoded_chunk_ = (std::numeric_limits<std::size_t>::max)();
        decoded_bytes_ = 0;
        evict_index_residency(
            db_oid_, user_relid_, tablespace_oid_, main_relfilenode_,
            column_attno_,
            index_name_hash(db_oid_, tablespace_oid_, main_relfilenode_,
                column_attno_, index_name_));
    }

    /// Ensure the same-name minimizer sidecar has the one canonical generation
    /// schema.  A legacy five-column relation is replaced transactionally under
    /// AccessExclusiveLock; there is no ALTER migration, compatibility table,
    /// attno shim, or payload copy.  Exact relations retain only AccessShareLock
    /// to the end of the transaction, which blocks DDL while allowing readers.
    [[nodiscard]] Oid ensure_minimizer_generation_storage() {
        if (index_name_ != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
            && index_name_ != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
            rev3_storage_error(
                "generation storage is only valid for minimizer indexes");
        }
        if (db_oid_ != MyDatabaseId || !OidIsValid(user_relid_)
            || !OidIsValid(tablespace_oid_)
            || !OidIsValid(main_relfilenode_) || column_attno_ <= 0) {
            rev3_storage_error("generation storage identity is invalid");
        }
        acquire_index_advisory_xact_lock(
            db_oid_, tablespace_oid_, main_relfilenode_, column_attno_,
            index_name_);

        const std::string relation_name = this_table_name();
        const Oid internal_namespace = bioseq_internal_ns();
        index_relid_ = InvalidOid;
        Oid relation_oid = get_relname_relid(
            relation_name.c_str(), internal_namespace);

        if (OidIsValid(relation_oid)) {
            Relation relation = try_safe_relation_open(
                relation_oid, AccessShareLock);
            if (relation == nullptr
                || RelationGetNamespace(relation) != internal_namespace
                || std::string_view{RelationGetRelationName(relation)}
                    != relation_name) {
                if (relation != nullptr) {
                    relation_close(relation, AccessShareLock);
                }
                rev3_storage_error(
                    "generation relation disappeared during schema inspection");
            }
            const Rev3IndexSchemaInspection schema =
                inspect_rev3_index_schema(relation);
            if (schema) {
                // Keep the AccessShare relation lock until transaction end so
                // external DDL cannot replace the proven tuple descriptor
                // before the row sink opens it.
                evict_generation_storage_residency_();
                relation_close(relation, NoLock);
                return relation_oid;
            }

            // Retain the first AccessShare lock while upgrading.  Once the
            // exclusive lock is held, resolve the name and inspect again so a
            // concurrent external DDL attempt can never redirect the DROP.
            relation_close(relation, NoLock);
            relation = try_safe_relation_open(
                relation_oid, AccessExclusiveLock);
            if (relation == nullptr
                || get_relname_relid(relation_name.c_str(), internal_namespace)
                    != relation_oid
                || RelationGetNamespace(relation) != internal_namespace
                || std::string_view{RelationGetRelationName(relation)}
                    != relation_name) {
                if (relation != nullptr) {
                    relation_close(relation, AccessExclusiveLock);
                }
                rev3_storage_error(
                    "generation relation identity changed while acquiring reset lock");
            }
            const Rev3IndexSchemaInspection locked_schema =
                inspect_rev3_index_schema(relation);
            if (locked_schema) {
                evict_generation_storage_residency_();
                relation_close(relation, NoLock);
                return relation_oid;
            }

            const Oid old_primary_index = find_primary_key_oid(relation);
            evict_generation_storage_residency_();
            CacheInvalidateRelcacheByRelid(relation_oid);
            if (OidIsValid(old_primary_index)) {
                CacheInvalidateRelcacheByRelid(old_primary_index);
            }
            // Retain AccessExclusiveLock after closing the relcache reference;
            // DROP executes in the same outer transaction and rollback restores
            // the complete old catalog/heap state.
            relation_close(relation, NoLock);
            run_internal_ddl(fmt::format(
                "DROP TABLE bioseq_internal.{}",
                quote_identifier(relation_name.c_str())).c_str());
            CommandCounterIncrement();
            index_relid_ = InvalidOid;
            relation_oid = InvalidOid;
        }

        const char* persistence = persistence_kw_for(user_relid_);
        run_internal_ddl(fmt::format(
            "CREATE {} TABLE bioseq_internal.{} ("
            "query_key BIGINT NOT NULL, generation_id BIGINT NOT NULL, "
            "position_segment INTEGER NOT NULL, sub_key BIGINT NOT NULL, "
            "chunk_no INTEGER NOT NULL, chunk_bytes INTEGER NOT NULL, "
            "checksum BIGINT NOT NULL, blob BYTEA NOT NULL, "
            "PRIMARY KEY "
            "(query_key,generation_id,position_segment,sub_key,chunk_no)) "
            "USING heap",
            persistence, quote_identifier(relation_name.c_str())).c_str());
        run_internal_ddl(fmt::format(
            "ALTER TABLE bioseq_internal.{} "
            "ALTER COLUMN blob SET STORAGE PLAIN",
            quote_identifier(relation_name.c_str())).c_str());
        CommandCounterIncrement();

        relation_oid = get_relname_relid(
            relation_name.c_str(), internal_namespace);
        Relation created = OidIsValid(relation_oid)
            ? try_safe_relation_open(relation_oid, AccessShareLock)
            : nullptr;
        if (created == nullptr
            || RelationGetNamespace(created) != internal_namespace
            || std::string_view{RelationGetRelationName(created)}
                != relation_name) {
            if (created != nullptr) {
                relation_close(created, AccessShareLock);
            }
            rev3_storage_error(
                "canonical generation relation was not created");
        }
        const Rev3IndexSchemaInspection created_schema =
            inspect_rev3_index_schema(created);
        if (!created_schema) {
            relation_close(created, AccessShareLock);
            rev3_storage_error(fmt::format(
                "created generation relation schema mismatch: {}",
                rev3_index_schema_error_name(created_schema.error)));
        }
        relation_close(created, NoLock);
        CacheInvalidateRelcacheByRelid(relation_oid);
        CacheInvalidateRelcacheByRelid(created_schema.primary_index_oid);
        // Broadcast on the owning relation because every residency key carries
        // `user_relid`, not the physical sidecar OID.  Other backends process
        // this relcache signal and can therefore evict legacy bytes after the
        // same-name DROP/recreate commits.
        CacheInvalidateRelcacheByRelid(user_relid_);
        evict_generation_storage_residency_();
        return relation_oid;
    }

    /// Transactionally remove the complete physical generation family for
    /// this one logical minimizer index.  Head, manifest, immutable payload,
    /// and temporary-run objects are row classes in the canonical sidecars,
    /// so dropping every table under the exact identity prefix clears all of
    /// them together.  This is an inactive prerequisite: no production caller
    /// is routed here until the format-4 reader/writer cutover is atomic.
    [[nodiscard]] MinimizerGenerationClearResult
    clear_minimizer_generation_storage() {
        struct PhysicalRelation {
            Oid oid = InvalidOid;
            std::string name;
            std::uint8_t bucket_idx = 0;
            std::uint32_t shard_idx = 0;
        };
        struct ClearState {
            bool spi_active = false;
            Relation locked_relation = nullptr;
            LOCKMODE locked_relation_mode = NoLock;
            char* spi_text = nullptr;
            char* quoted_literal = nullptr;
            Oid internal_namespace = InvalidOid;
            std::string index_name;
            std::string prefix;
            std::string owned_shards_sql;
            std::string discover_sql;
            std::string delete_age_sql;
            std::string ddl_sql;
            std::vector<std::pair<std::uint8_t, std::uint32_t>>
                owned_shards;
            std::vector<PhysicalRelation> candidates;
            std::vector<PhysicalRelation> relations;
            std::vector<PhysicalRelation> verification_candidates;
            MinimizerGenerationClearResult result{};
            std::exception_ptr cpp_error;
            bool completed = false;
        };
        auto* state = new (std::nothrow) ClearState{};
        if (state == nullptr) {
            ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("bioseq revision-3 index: generation clear state allocation failed")));
        }
        MemoryContext saved_context = CurrentMemoryContext;

        PG_TRY();
        {
            try {
                if (index_name_ != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
                    && index_name_ != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
                    rev3_storage_error(
                        "generation clear is only valid for minimizer indexes");
                }
                if (db_oid_ != MyDatabaseId || !OidIsValid(user_relid_)
                    || !OidIsValid(tablespace_oid_)
                    || !OidIsValid(main_relfilenode_)
                    || column_attno_ <= 0) {
                    rev3_storage_error("generation clear identity is invalid");
                }

                acquire_index_advisory_xact_lock(
                    db_oid_, tablespace_oid_, main_relfilenode_,
                    column_attno_, index_name_);

                // A failed operation or enclosing savepoint rollback retains
                // no stale local object; the next access resolves by name.
                index_relid_ = InvalidOid;
                chunk_payload_bytes_.clear();
                release_runs_noexcept();
                evict_generation_storage_residency_();

                state->prefix = fmt::format(
                    "seqidx_{}_{:016x}_", short_index_for(index_name_),
                    identity_hash64(db_oid_, tablespace_oid_,
                        main_relfilenode_, column_attno_, index_name_));
                state->index_name.assign(index_name_);
                state->internal_namespace = bioseq_internal_ns();
                state->owned_shards_sql = fmt::format(
                    "SELECT bucket_idx::integer, shard_idx "
                    "FROM bioseq_internal.seq_sub_table "
                    "WHERE database_oid={}::oid "
                    "AND tablespace_oid={}::oid "
                    "AND main_relfilenode={}::oid "
                    "AND column_attno={}::smallint "
                    "ORDER BY bucket_idx, shard_idx",
                    static_cast<unsigned>(db_oid_),
                    static_cast<unsigned>(tablespace_oid_),
                    static_cast<unsigned>(main_relfilenode_),
                    static_cast<int>(column_attno_));
                state->quoted_literal =
                    quote_literal_cstr(state->prefix.c_str());
                state->discover_sql = fmt::format(
                    "SELECT c.oid, c.relname::text FROM pg_class c "
                    "WHERE c.relnamespace={}::oid "
                    "AND c.relkind='r' "
                    "AND left(c.relname::text, length({}))={} "
                    "ORDER BY c.oid",
                    static_cast<unsigned>(state->internal_namespace),
                    state->quoted_literal, state->quoted_literal);
                pfree(state->quoted_literal);
                state->quoted_literal = nullptr;

                const int discovery_connect_rc = SPI_connect();
                if (discovery_connect_rc != SPI_OK_CONNECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear discovery SPI_connect failed"),
                         errdetail("SPI rc=%d", discovery_connect_rc)));
                }
                state->spi_active = true;
                const int owned_rc = SPI_execute(
                    state->owned_shards_sql.c_str(), true, 0);
                if (owned_rc != SPI_OK_SELECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear owned-shard discovery failed"),
                         errdetail("SPI rc=%d", owned_rc)));
                }
                state->owned_shards.reserve(
                    static_cast<std::size_t>(SPI_processed));
                for (std::uint64_t row = 0; row < SPI_processed; ++row) {
                    bool bucket_is_null = false;
                    bool shard_is_null = false;
                    const std::int32_t bucket = DatumGetInt32(SPI_getbinval(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1,
                        &bucket_is_null));
                    const std::int32_t shard = DatumGetInt32(SPI_getbinval(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2,
                        &shard_is_null));
                    if (bucket_is_null || shard_is_null || bucket < 0
                        || bucket > (std::numeric_limits<std::uint8_t>::max)()
                        || shard < 0) {
                        rev3_storage_error(
                            "generation clear found an invalid owned shard identity");
                    }
                    state->owned_shards.emplace_back(
                        static_cast<std::uint8_t>(bucket),
                        static_cast<std::uint32_t>(shard));
                }

                const int discover_rc = SPI_execute(
                    state->discover_sql.c_str(), true, 0);
                if (discover_rc != SPI_OK_SELECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear relation discovery failed"),
                         errdetail("SPI rc=%d", discover_rc)));
                }
                state->candidates.reserve(
                    static_cast<std::size_t>(SPI_processed));
                for (std::uint64_t row = 0; row < SPI_processed; ++row) {
                    bool oid_is_null = false;
                    const Datum oid_datum = SPI_getbinval(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1,
                        &oid_is_null);
                    state->spi_text = SPI_getvalue(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2);
                    if (oid_is_null || state->spi_text == nullptr) {
                        rev3_storage_error(
                            "generation clear discovery returned a NULL identity");
                    }
                    state->candidates.emplace_back();
                    PhysicalRelation& candidate = state->candidates.back();
                    candidate.oid = DatumGetObjectId(oid_datum);
                    candidate.name.assign(state->spi_text);
                    pfree(state->spi_text);
                    state->spi_text = nullptr;
                    if (!OidIsValid(candidate.oid)
                        || !candidate.name.starts_with(state->prefix)) {
                        rev3_storage_error(
                            "generation clear discovery escaped its exact prefix");
                    }
                }
                const int discovery_finish_rc = SPI_finish();
                state->spi_active =
                    discovery_finish_rc != SPI_OK_FINISH;
                if (discovery_finish_rc != SPI_OK_FINISH) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear discovery SPI_finish failed"),
                         errdetail("SPI rc=%d", discovery_finish_rc)));
                }

                // A broad prefix match is only a candidate.  Ownership also
                // requires canonical decimal spelling, an exact source shard
                // catalog identity, and the exact generation tuple schema.
                for (std::size_t candidate_index = 0;
                     candidate_index < state->candidates.size();
                     ++candidate_index) {
                    PhysicalRelation& candidate =
                        state->candidates[candidate_index];
                    const CanonicalGenerationSidecarName parsed =
                        parse_canonical_generation_sidecar_name(
                            candidate.name, state->prefix);
                    if (!parsed.valid) continue;
                    bool owned = false;
                    for (const auto& shard : state->owned_shards) {
                        if (shard.first == parsed.bucket_idx
                            && shard.second == parsed.shard_idx) {
                            owned = true;
                            break;
                        }
                    }
                    if (!owned) continue;
                    state->locked_relation = try_safe_relation_open(
                        candidate.oid, AccessShareLock);
                    state->locked_relation_mode = AccessShareLock;
                    if (state->locked_relation == nullptr
                        || get_relname_relid(candidate.name.c_str(),
                            state->internal_namespace) != candidate.oid
                        || RelationGetNamespace(state->locked_relation)
                            != state->internal_namespace
                        || std::string_view{RelationGetRelationName(
                            state->locked_relation)} != candidate.name) {
                        rev3_storage_error(
                            "generation clear candidate identity changed during inspection");
                    }
                    const Rev3IndexSchemaInspection schema =
                        inspect_rev3_index_schema(state->locked_relation);
                    relation_close(
                        state->locked_relation, AccessShareLock);
                    state->locked_relation = nullptr;
                    state->locked_relation_mode = NoLock;
                    if (!schema) continue;
                    candidate.bucket_idx = parsed.bucket_idx;
                    candidate.shard_idx = parsed.shard_idx;
                    state->relations.push_back(candidate);
                }

                for (std::size_t relation_index = 0;
                     relation_index < state->relations.size();
                     ++relation_index) {
                    PhysicalRelation& target =
                        state->relations[relation_index];
                    state->locked_relation = try_safe_relation_open(
                        target.oid, AccessExclusiveLock);
                    state->locked_relation_mode = AccessExclusiveLock;
                    if (state->locked_relation == nullptr
                        || get_relname_relid(target.name.c_str(),
                            state->internal_namespace) != target.oid
                        || RelationGetNamespace(state->locked_relation)
                            != state->internal_namespace
                        || std::string_view{RelationGetRelationName(
                            state->locked_relation)} != target.name) {
                        rev3_storage_error(
                            "generation relation identity changed while acquiring clear lock");
                    }
                    const Rev3IndexSchemaInspection locked_schema =
                        inspect_rev3_index_schema(state->locked_relation);
                    if (!locked_schema) {
                        rev3_storage_error(
                            "generation relation schema changed while acquiring clear lock");
                    }
                    const Oid primary_index =
                        locked_schema.primary_index_oid;
                    CacheInvalidateRelcacheByRelid(target.oid);
                    CacheInvalidateRelcacheByRelid(primary_index);
                    relation_close(state->locked_relation, NoLock);
                    state->locked_relation = nullptr;
                    state->locked_relation_mode = NoLock;

                    state->ddl_sql.assign(
                        "DROP TABLE bioseq_internal.");
                    state->ddl_sql.append(
                        quote_identifier(target.name.c_str()));
                    run_checked_generation_clear_ddl(
                        state->ddl_sql.c_str(), state->spi_active);
                    CommandCounterIncrement();
                    if (OidIsValid(get_relname_relid(
                            target.name.c_str(),
                            state->internal_namespace))) {
                        rev3_storage_error(
                            "generation clear DDL retained an owned relation");
                    }
                }
                state->result.dropped_relation_count =
                    static_cast<std::uint64_t>(state->relations.size());

                state->quoted_literal = quote_literal_cstr(
                    state->index_name.c_str());
                state->delete_age_sql = fmt::format(
                    "DELETE FROM bioseq_internal.metadata_age_log "
                    "WHERE user_relid={}::oid "
                    "AND main_relfilenode={}::oid "
                    "AND column_attno={}::smallint AND index_name={}",
                    static_cast<unsigned>(user_relid_),
                    static_cast<unsigned>(main_relfilenode_),
                    static_cast<int>(column_attno_),
                    state->quoted_literal);
                pfree(state->quoted_literal);
                state->quoted_literal = nullptr;
                const int metadata_connect_rc = SPI_connect();
                if (metadata_connect_rc != SPI_OK_CONNECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear metadata SPI_connect failed"),
                         errdetail("SPI rc=%d", metadata_connect_rc)));
                }
                state->spi_active = true;
                const int delete_age_rc = SPI_execute(
                    state->delete_age_sql.c_str(), false, 0);
                if (delete_age_rc != SPI_OK_DELETE || SPI_processed > 1) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear metadata deletion failed"),
                         errdetail("SPI rc=%d rows=%llu", delete_age_rc,
                            static_cast<unsigned long long>(SPI_processed))));
                }
                state->result.deleted_metadata_age_rows =
                    static_cast<std::uint64_t>(SPI_processed);
                const int metadata_finish_rc = SPI_finish();
                state->spi_active =
                    metadata_finish_rc != SPI_OK_FINISH;
                if (metadata_finish_rc != SPI_OK_FINISH) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear metadata SPI_finish failed"),
                         errdetail("SPI rc=%d", metadata_finish_rc)));
                }
                CommandCounterIncrement();

                // Re-discover broadly, but fail only if a canonical,
                // catalog-owned, exact generation relation survived.  Benign
                // near-prefix relations are deliberately ignored.
                const int verify_connect_rc = SPI_connect();
                if (verify_connect_rc != SPI_OK_CONNECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear verification SPI_connect failed"),
                         errdetail("SPI rc=%d", verify_connect_rc)));
                }
                state->spi_active = true;
                const int verify_rc = SPI_execute(
                    state->discover_sql.c_str(), true, 0);
                if (verify_rc != SPI_OK_SELECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear verification query failed"),
                         errdetail("SPI rc=%d", verify_rc)));
                }
                state->verification_candidates.reserve(
                    static_cast<std::size_t>(SPI_processed));
                for (std::uint64_t row = 0; row < SPI_processed; ++row) {
                    bool oid_is_null = false;
                    const Datum oid_datum = SPI_getbinval(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1,
                        &oid_is_null);
                    state->spi_text = SPI_getvalue(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2);
                    if (oid_is_null || state->spi_text == nullptr) {
                        rev3_storage_error(
                            "generation clear verification returned a NULL identity");
                    }
                    state->verification_candidates.emplace_back();
                    PhysicalRelation& candidate =
                        state->verification_candidates.back();
                    candidate.oid = DatumGetObjectId(oid_datum);
                    candidate.name.assign(state->spi_text);
                    pfree(state->spi_text);
                    state->spi_text = nullptr;
                }
                const int verify_finish_rc = SPI_finish();
                state->spi_active =
                    verify_finish_rc != SPI_OK_FINISH;
                if (verify_finish_rc != SPI_OK_FINISH) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear verification SPI_finish failed"),
                         errdetail("SPI rc=%d", verify_finish_rc)));
                }
                for (std::size_t candidate_index = 0;
                     candidate_index
                        < state->verification_candidates.size();
                     ++candidate_index) {
                    PhysicalRelation& candidate =
                        state->verification_candidates[candidate_index];
                    const CanonicalGenerationSidecarName parsed =
                        parse_canonical_generation_sidecar_name(
                            candidate.name, state->prefix);
                    if (!parsed.valid) continue;
                    bool owned = false;
                    for (const auto& shard : state->owned_shards) {
                        if (shard.first == parsed.bucket_idx
                            && shard.second == parsed.shard_idx) {
                            owned = true;
                            break;
                        }
                    }
                    if (!owned) continue;
                    state->locked_relation = try_safe_relation_open(
                        candidate.oid, AccessShareLock);
                    state->locked_relation_mode = AccessShareLock;
                    if (state->locked_relation == nullptr) {
                        rev3_storage_error(
                            "generation clear verification relation disappeared");
                    }
                    const bool exact_identity =
                        RelationGetNamespace(state->locked_relation)
                            == state->internal_namespace
                        && std::string_view{RelationGetRelationName(
                            state->locked_relation)} == candidate.name;
                    const Rev3IndexSchemaInspection schema =
                        inspect_rev3_index_schema(state->locked_relation);
                    relation_close(
                        state->locked_relation, AccessShareLock);
                    state->locked_relation = nullptr;
                    state->locked_relation_mode = NoLock;
                    if (exact_identity && schema) {
                        rev3_storage_error(
                            "generation clear verification retained an owned relation");
                    }
                }

                index_relid_ = InvalidOid;
                chunk_payload_bytes_.clear();
                release_runs_noexcept();
                evict_generation_storage_residency_();
                CacheInvalidateRelcacheByRelid(user_relid_);
                state->completed = true;
            } catch (...) {
                state->cpp_error = std::current_exception();
            }
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            ErrorData* volatile saved_error = CopyErrorData();
            FlushErrorState();
            if (state->spi_text != nullptr) {
                PG_TRY(); { pfree(state->spi_text); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
                state->spi_text = nullptr;
            }
            if (state->quoted_literal != nullptr) {
                PG_TRY(); { pfree(state->quoted_literal); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
                state->quoted_literal = nullptr;
            }
            if (state->locked_relation != nullptr) {
                Relation cleanup = state->locked_relation;
                const LOCKMODE cleanup_mode =
                    state->locked_relation_mode;
                state->locked_relation = nullptr;
                state->locked_relation_mode = NoLock;
                PG_TRY(); { relation_close(cleanup, cleanup_mode); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
            }
            if (state->spi_active) {
                state->spi_active = false;
                PG_TRY(); { (void)SPI_finish(); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
            }
            delete state;
            ReThrowError(saved_error);
        }
        PG_END_TRY();

        if (state->cpp_error != nullptr) {
            if (state->spi_text != nullptr) {
                PG_TRY(); { pfree(state->spi_text); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
                state->spi_text = nullptr;
            }
            if (state->quoted_literal != nullptr) {
                PG_TRY(); { pfree(state->quoted_literal); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
                state->quoted_literal = nullptr;
            }
            if (state->locked_relation != nullptr) {
                Relation cleanup = state->locked_relation;
                const LOCKMODE cleanup_mode =
                    state->locked_relation_mode;
                state->locked_relation = nullptr;
                state->locked_relation_mode = NoLock;
                PG_TRY(); { relation_close(cleanup, cleanup_mode); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
            }
            if (state->spi_active) {
                state->spi_active = false;
                PG_TRY(); { (void)SPI_finish(); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
            }
            char cpp_error_detail[512] = "unknown C++ exception";
            try {
                std::rethrow_exception(state->cpp_error);
            } catch (const std::exception& error) {
                const char* message = error.what();
                strlcpy(cpp_error_detail,
                    message == nullptr
                        ? "std::exception without detail" : message,
                    sizeof(cpp_error_detail));
            } catch (...) {
                strlcpy(cpp_error_detail, "unknown C++ exception",
                    sizeof(cpp_error_detail));
            }
            state->cpp_error = nullptr;
            delete state;
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq revision-3 index: generation clear failed"),
                 errdetail("C++ error=%s", cpp_error_detail)));
        }
        if (!state->completed) {
            delete state;
            rev3_storage_error(
                "generation clear did not complete or report an error");
        }
        const MinimizerGenerationClearResult result = state->result;
        delete state;
        return result;
    }

    /// Open the inactive revision-3 reader against the exact physical name,
    /// intentionally bypassing the legacy cached OID. A five-column relation
    /// therefore fails schema validation rather than entering a legacy route.
    [[nodiscard]] GenerationReadSession begin_generation_read(
            std::uint64_t query_key,
            Snapshot executor_snapshot,
            ExpectedMinimizerCompatibility expected) const {
        if (query_key != query_key_) {
            rev3_storage_error("generation read changed the handle query key");
        }
        return GenerationReadSession{
            this_table_name(), query_key, executor_snapshot, expected,
            Rev3SnapshotRegistration::borrow_registered};
    }

    /// Open the exact common-encoder row sink after atomically ensuring the
    /// canonical same-name relation.  The caller still owns generation
    /// encoding, complete reread, and READY-head publication.
    [[nodiscard]] GenerationRowSink begin_generation_write(
            std::uint64_t query_key,
            std::uint64_t generation_id) {
        if (query_key != query_key_) {
            rev3_storage_error("generation writer changed the handle query key");
        }
        (void)ensure_minimizer_generation_storage();
        return GenerationRowSink{
            Rev3IndexAdministrationIdentity{
                .database_oid = db_oid_,
                .tablespace_oid = tablespace_oid_,
                .main_relfilenode = main_relfilenode_,
                .user_relid = user_relid_,
                .column_attno = column_attno_,
                .index_name = std::string{index_name_},
                .table_name = this_table_name(),
            },
            query_key,
            generation_id};
    }

    [[nodiscard]] Oid resolve_relid_(bool create_if_missing) const {
        const std::string nm = this_table_name();
        const Oid ns = bioseq_internal_ns();
        Oid rid = get_relname_relid(nm.c_str(), ns);
        // The C++ handle can outlive a PostgreSQL subtransaction.  Never trust
        // a cached OID unless the authoritative namespace/name lookup still
        // resolves to exactly that OID; rollback may have restored an old
        // relation or removed a relation created below a SAVEPOINT.
        if (OidIsValid(index_relid_) && index_relid_ == rid) {
            return index_relid_;
        }
        index_relid_ = InvalidOid;
        if (!OidIsValid(rid) && create_if_missing) {
            const char* pk = persistence_kw_for(user_relid_);
            std::string ddl = fmt::format(
                "CREATE {} TABLE IF NOT EXISTS bioseq_internal.{} ("
                "query_key BIGINT NOT NULL, sub_key BIGINT NOT NULL, chunk_no INT NOT NULL, "
                "chunk_bytes INT NOT NULL, blob BYTEA NOT NULL, "
                "PRIMARY KEY (query_key, sub_key, chunk_no)) USING heap;",
                pk, quote_identifier(nm.c_str()));
            run_internal_ddl(ddl.c_str());
            run_internal_ddl(fmt::format(
                "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
                quote_identifier(nm.c_str())).c_str());
            rid = get_relname_relid(nm.c_str(), ns);
            if (!OidIsValid(rid)) index_error("failed to create seqidx table");
        }
        index_relid_ = rid;
        return rid;
    }
    [[nodiscard]] Relation open_index_relation_(bool create_if_missing, LOCKMODE lock) const {
        const Oid rid = resolve_relid_(create_if_missing);
        if (!OidIsValid(rid)) return nullptr;
        Relation rel = try_safe_relation_open(rid, lock);
        if (rel == nullptr) return nullptr;
        const std::string expected_name = this_table_name();
        if (rel->rd_rel->relam != HEAP_TABLE_AM_OID
            || RelationGetNamespace(rel) != bioseq_internal_ns()
            || std::string_view{RelationGetRelationName(rel)}
                != expected_name) {
            relation_close(rel, lock);
            index_relid_ = InvalidOid;
            bioseq_log<ERR, PG>(
                "bioseq index: {} has the wrong physical identity",
                expected_name);
        }

        // This API is the legacy five-column surface.  It must never apply its
        // attnos to the canonical eight-column generation relation, even while
        // the production writer/reader cutover is being landed in slices.
        TupleDesc tuple_desc = RelationGetDescr(rel);
        constexpr std::array<Rev3IndexAttributeDescriptor, 5>
            legacy_descriptors{{
                {kAttQueryKey, "query_key", INT8OID},
                {kAttSubKey, "sub_key", INT8OID},
                {kAttChunkNo, "chunk_no", INT4OID},
                {kAttChunkBytes, "chunk_bytes", INT4OID},
                {kAttBlob, "blob", BYTEAOID},
            }};
        bool legacy_shape = tuple_desc != nullptr
            && tuple_desc->natts
                == static_cast<int>(legacy_descriptors.size());
        for (std::size_t attribute_index = 0;
             legacy_shape && attribute_index < legacy_descriptors.size();
             ++attribute_index) {
            const Rev3IndexAttributeDescriptor& expected =
                legacy_descriptors[attribute_index];
            Form_pg_attribute attribute = TupleDescAttr(
                tuple_desc, static_cast<int>(attribute_index));
            legacy_shape = attribute->attnum == expected.attno
                && !attribute->attisdropped
                && std::string_view{NameStr(attribute->attname)}
                    == expected.name
                && attribute->atttypid == expected.type_oid
                && attribute->attnotnull;
        }
        if (!legacy_shape) {
            relation_close(rel, lock);
            index_relid_ = InvalidOid;
            bioseq_log<ERR, PG>(
                "bioseq index: legacy fragment API refuses non-legacy schema for {}",
                expected_name);
        }
        return rel;
    }

    void ensure_buffer_(std::size_t units) const {
        if (decoded_units_ >= units && decoded_ != nullptr) return;
        MemoryContext old = MemoryContextSwitchTo(own_mctx_);
        decoded_ = (decoded_ == nullptr) ? (std::int64_t*)palloc(units*sizeof(std::int64_t))
                                         : (std::int64_t*)repalloc(decoded_, units*sizeof(std::int64_t));
        decoded_units_ = units;
        MemoryContextSwitchTo(old);
    }
    [[nodiscard]] std::int64_t* scratch_buffer_(std::size_t units) {
        if (scratch_units_ < units || scratch_ == nullptr) {
            MemoryContext old = MemoryContextSwitchTo(own_mctx_);
            scratch_ = (scratch_ == nullptr) ? (std::int64_t*)palloc(units*sizeof(std::int64_t))
                                             : (std::int64_t*)repalloc(scratch_, units*sizeof(std::int64_t));
            scratch_units_ = units;
            MemoryContextSwitchTo(old);
        }
        return scratch_;
    }
    [[nodiscard]] std::size_t scratch_buffer_bytes_() const { return scratch_units_ * sizeof(std::int64_t); }

    [[nodiscard]] IndexChunkKey chunk_cache_key_(
            index_sub_key_t sub_key,
            std::size_t chunk_no) const noexcept {
        return IndexChunkKey{
            .db_oid = db_oid_,
            .user_relid = user_relid_,
            .tablespace_oid = tablespace_oid_,
            .main_relfilenode = main_relfilenode_,
            .column_attno = column_attno_,
            .index_name_hash = index_name_hash(
                db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, index_name_),
            .bucket_idx = bucket_idx_,
            .shard_idx = shard_idx_,
            .query_key = query_key_,
            .sub_key = sub_key,
            .chunk_no = static_cast<std::uint32_t>(chunk_no),
        };
    }

    void admit_cached_chunk_(
            index_sub_key_t sub_key,
            std::size_t chunk_no,
            std::span<const char> payload,
            bool written_in_current_xact) const {
        (void)store_cached_chunk(
            chunk_cache_key_(sub_key, chunk_no),
            payload,
            written_in_current_xact);
    }

    void load_chunk_into_buffer_(index_sub_key_t sub_key, std::size_t chunk_no) const {
        const IndexChunkKey key = chunk_cache_key_(sub_key, chunk_no);
        if (auto cached = lookup_cached_chunk(key)) {
            const std::size_t units =
                (cached->size + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
            ensure_buffer_(units == 0 ? 1 : units);
            if (cached->size > 0) {
                std::memcpy(decoded_, cached->data, cached->size);
            }
            decoded_sub_ = sub_key;
            decoded_chunk_ = chunk_no;
            decoded_bytes_ = cached->size;
            return;
        }
        Relation rel = open_index_relation_(false, AccessShareLock);
        if (rel == nullptr) bioseq_log<ERR, PG>( "bioseq index: chunk ({},{},{}) relation missing",
                                   (long long)query_key_, (long long)sub_key, chunk_no);
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[3];
        ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw());
        ScanKeyInit(&k[1], kAttSubKey,   BTEqualStrategyNumber, F_INT8EQ,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(sub_key)).raw());
        ScanKeyInit(&k[2], kAttChunkNo,  BTEqualStrategyNumber, F_INT4EQ,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int32_t>(chunk_no)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 3, k);
        HeapTuple t = systable_getnext(s);
        if (!HeapTupleIsValid(t)) { systable_endscan(s); relation_close(rel, AccessShareLock);
            bioseq_log<ERR, PG>( "bioseq index chunk ({},{},{}) missing", (long long)query_key_, (long long)sub_key, chunk_no); }
        bool isnull=false; Datum d = heap_getattr(t, kAttBlob, RelationGetDescr(rel), &isnull);
        if (isnull) bioseq_log<ERR, PG>( "bioseq index chunk blob NULL");
        auto raw_detoasted = bioseq::pg_utils::detoast_packed_as<bytea>(
            bioseq::pg_utils::pg_datum::from_raw(d));
        bytea* raw = raw_detoasted.ptr;
        const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
        const std::size_t units   = (payload + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        ensure_buffer_(units == 0 ? 1 : units);
        if (payload > 0) std::memcpy(decoded_, VARDATA_ANY(raw), payload);
        admit_cached_chunk_(
            sub_key,
            chunk_no,
            std::span<const char>(reinterpret_cast<const char*>(VARDATA_ANY(raw)), payload),
            false);
        raw_detoasted.free_if_copy();
        decoded_sub_ = sub_key; decoded_chunk_ = chunk_no; decoded_bytes_ = payload;
        systable_endscan(s);
        relation_close(rel, AccessShareLock);
    }

    // ---- const metadata queries (total: 0/empty on absent) ----
    [[nodiscard]] std::size_t number_of_sub_keys() const {
        Relation rel = open_index_relation_(false, AccessShareLock);
        if (rel == nullptr) return 0;
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[2];
        ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw());
        ScanKeyInit(&k[1], kAttSubKey,   BTLessStrategyNumber,  F_INT8LT,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(TEMP_RUN_BASE)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 2, k);
        std::int64_t last = std::numeric_limits<std::int64_t>::min(); bool have=false; std::size_t count=0;
        for (HeapTuple t; (t = systable_getnext(s)) != nullptr; ) {
            bool isnull=false; std::int64_t sk = bioseq::pg_utils::pg_datum::to<std::int64_t>(
                bioseq::pg_utils::pg_datum::from_raw(heap_getattr(t, kAttSubKey, RelationGetDescr(rel), &isnull)));
            if (!have || sk != last) { ++count; last = sk; have = true; }
        }
        systable_endscan(s); relation_close(rel, AccessShareLock);
        return count;
    }
    [[nodiscard]] std::size_t number_of_chunks(index_sub_key_t sub_key) const {
        Relation rel = open_index_relation_(false, AccessShareLock);
        if (rel == nullptr) return 0;
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[2];
        ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw());
        ScanKeyInit(&k[1], kAttSubKey,   BTEqualStrategyNumber, F_INT8EQ,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(sub_key)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 2, k);
        std::size_t n=0; for (; systable_getnext(s) != nullptr; ) ++n;
        systable_endscan(s); relation_close(rel, AccessShareLock);
        return n;
    }
    [[nodiscard]] std::size_t chunk_byte_size(index_sub_key_t sub_key, std::size_t chunk_no) const {
        Relation rel = open_index_relation_(false, AccessShareLock);
        if (rel == nullptr) return 0;
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[3];
        ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw());
        ScanKeyInit(&k[1], kAttSubKey,   BTEqualStrategyNumber, F_INT8EQ,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(sub_key)).raw());
        ScanKeyInit(&k[2], kAttChunkNo,  BTEqualStrategyNumber, F_INT4EQ,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int32_t>(chunk_no)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 3, k);
        std::size_t out=0; HeapTuple t = systable_getnext(s);
        if (HeapTupleIsValid(t)) { bool isnull=false; out = static_cast<std::size_t>(bioseq::pg_utils::pg_datum::to<std::int32_t>(
            bioseq::pg_utils::pg_datum::from_raw(heap_getattr(t, kAttChunkBytes, RelationGetDescr(rel), &isnull)))); }
        systable_endscan(s); relation_close(rel, AccessShareLock);
        return out;
    }

    template <class T>
    [[nodiscard]] const T* read_chunk(index_sub_key_t sub_key, std::size_t chunk_no) {
        static_assert(alignof(T) <= alignof(std::int64_t), "read_chunk<T>: T must be int64-aligned-compatible");
        if (decoded_ != nullptr && decoded_sub_ == sub_key && decoded_chunk_ == chunk_no)
            return reinterpret_cast<const T*>(decoded_);
        load_chunk_into_buffer_(sub_key, chunk_no);
        return reinterpret_cast<const T*>(decoded_);
    }

    void resize(index_sub_key_t sub_key, std::size_t bytes_count) {
        const std::size_t n = bytes_count == 0 ? 1 : (bytes_count + MAX_INDEX_CHUNK_SIZE_BYTES - 1) / MAX_INDEX_CHUNK_SIZE_BYTES;
        std::vector<std::size_t> plan(n);
        std::size_t rem = bytes_count;
        for (std::size_t i = 0; i < n; ++i) { const std::size_t c = std::min(rem, MAX_INDEX_CHUNK_SIZE_BYTES); plan[i] = c; rem -= c; }
        chunk_payload_bytes_[sub_key] = std::move(plan);
    }
    void resize_chunks(index_sub_key_t sub_key, std::span<const std::size_t> per_chunk_bytes) {
        std::vector<std::size_t> plan(per_chunk_bytes.begin(), per_chunk_bytes.end());
        for (std::size_t b : plan) if (b > MAX_INDEX_CHUNK_SIZE_BYTES) bioseq_log<ERR, PG>( "bioseq index: chunk payload exceeds MAX");
        chunk_payload_bytes_[sub_key] = std::move(plan);
    }
    void prefetch_chunk(index_sub_key_t sub_key, std::size_t chunk_no) {
        if (decoded_sub_ == sub_key && decoded_chunk_ == chunk_no) {
            return;
        }
        load_chunk_into_buffer_(sub_key, chunk_no);
    }

    void upsert_chunk_row_(Relation rel, std::uint64_t query_key, index_sub_key_t sub_key, std::size_t chunk_no,
                           std::size_t chunk_bytes, std::span<const char> payload) {
        {
            const Oid pk = find_primary_key_oid(rel);
            ScanKeyData k[3];
            ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ,
                        bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(query_key)).raw());
            ScanKeyInit(&k[1], kAttSubKey, BTEqualStrategyNumber, F_INT8EQ,
                        bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(sub_key)).raw());
            ScanKeyInit(&k[2], kAttChunkNo, BTEqualStrategyNumber, F_INT4EQ,
                        bioseq::pg_utils::pg_datum::from(static_cast<std::int32_t>(chunk_no)).raw());
            SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 3, k);
            HeapTuple old_t = systable_getnext(s);
            if (HeapTupleIsValid(old_t)) {
                simple_heap_delete(rel, &old_t->t_self);
            }
            systable_endscan(s);
        }
        bytea* blob = reinterpret_cast<bytea*>(palloc(VARHDRSZ + payload.size()));
        SET_VARSIZE(blob, VARHDRSZ + payload.size());
        if (!payload.empty()) std::memcpy(VARDATA(blob), payload.data(), payload.size());
        TupleDesc td = RelationGetDescr(rel);
        bioseq::pg_utils::pg_datum_list<5> values{};
        values.set(1, static_cast<std::int64_t>(query_key));
        values.set(2, static_cast<std::int64_t>(sub_key));
        values.set(3, static_cast<std::int32_t>(chunk_no));
        values.set(4, static_cast<std::int32_t>(chunk_bytes));
        values.set_raw(5, bioseq::pg_utils::pg_datum::from_pointer(blob));
        HeapTuple tup = values.form_tuple(td);
        CatalogTupleInsert(rel, tup);
        heap_freetuple(tup); pfree(blob);
    }

    template <class T, class F>
    void write_to_each(index_sub_key_t sub_key, F&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<F, T>,
            "write_to_each<T>: writer must be (size_t, T*, size_t) -> size_t");
        Relation rel = open_index_relation_(true, RowExclusiveLock);
        auto it = chunk_payload_bytes_.find(sub_key);
        if (it == chunk_payload_bytes_.end())
            bioseq_log<ERR, PG>( "bioseq index: write_to_each(sub_key={}) without a preceding resize", (long long)sub_key);
        const std::vector<std::size_t>& plan = it->second;
        const std::size_t n = plan.size();
        std::size_t max_payload = 0; for (std::size_t c=0;c<n;++c) max_payload = std::max(max_payload, plan[c]);
        std::int64_t* buf = scratch_buffer_((max_payload + sizeof(std::int64_t) - 1) / sizeof(std::int64_t) + 1);
        for (std::size_t chunk_no = 0; chunk_no < n; /* writer may skip */) {
            const std::size_t payload_bytes = plan[chunk_no];
            if (scratch_buffer_bytes_() < payload_bytes) bioseq_log<ERR, PG>( "bioseq index: scratch undersized");
            if (payload_bytes % sizeof(T) != 0) bioseq_log<ERR, PG>( "bioseq index: chunk payload not a whole number of T");
            const std::size_t cap_units = payload_bytes / sizeof(T);
            if (payload_bytes > 0) std::memset(buf, 0, payload_bytes);
            const std::size_t next = writer(chunk_no, reinterpret_cast<T*>(buf), cap_units);
            if (next < chunk_no) bioseq_log<ERR, PG>( "bioseq index: writer moved backwards");
            upsert_chunk_row_(rel, query_key_, sub_key, chunk_no, payload_bytes,
                              std::span<const char>(reinterpret_cast<const char*>(buf), payload_bytes));
            admit_cached_chunk_(sub_key, chunk_no,
                std::span<const char>(reinterpret_cast<const char*>(buf), payload_bytes),
                true);
            chunk_no = (next == chunk_no) ? chunk_no + 1 : next;
        }
        relation_close(rel, RowExclusiveLock);
        chunk_payload_bytes_.erase(it);
    }

    template <class F> void read_chunks_in_range(index_sub_key_t lo, index_sub_key_t hi, F&& cb) {
        if (hi <= lo) return;
        Relation rel = open_index_relation_(false, AccessShareLock);
        if (rel == nullptr) return;
        const Oid pk = find_primary_key_oid(rel);
        if (!OidIsValid(pk)) { relation_close(rel, AccessShareLock); index_error("range scan: no PK index"); }
        Relation idx = index_open(pk, AccessShareLock);
        ScanKeyData k[3];
        ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber,        F_INT8EQ,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw());
        ScanKeyInit(&k[1], kAttSubKey,   BTGreaterEqualStrategyNumber, F_INT8GE,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(lo)).raw());
        ScanKeyInit(&k[2], kAttSubKey,   BTLessEqualStrategyNumber,    F_INT8LE,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(hi - 1)).raw());
        SysScanDesc s = systable_beginscan_ordered(rel, idx, nullptr, 3, k);
        std::vector<char> tmp;
        for (HeapTuple t; (t = systable_getnext_ordered(s, ForwardScanDirection)) != nullptr; ) {
            bool isnull=false;
            const std::int64_t sk = bioseq::pg_utils::pg_datum::to<std::int64_t>(
                bioseq::pg_utils::pg_datum::from_raw(heap_getattr(t, kAttSubKey, RelationGetDescr(rel), &isnull)));
            const std::int32_t chunk_no = bioseq::pg_utils::pg_datum::to<std::int32_t>(
                bioseq::pg_utils::pg_datum::from_raw(heap_getattr(t, kAttChunkNo, RelationGetDescr(rel), &isnull)));
            Datum d = heap_getattr(t, kAttBlob, RelationGetDescr(rel), &isnull);
            auto raw_detoasted = bioseq::pg_utils::detoast_packed_as<bytea>(
                bioseq::pg_utils::pg_datum::from_raw(d));
            bytea* raw = raw_detoasted.ptr;
            const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
            tmp.resize(payload); if (payload>0) std::memcpy(tmp.data(), VARDATA_ANY(raw), payload);
            const auto cached = store_cached_chunk(
                chunk_cache_key_((index_sub_key_t)sk, static_cast<std::size_t>(chunk_no)),
                std::span<const char>(tmp.data(), tmp.size()),
                false);
            raw_detoasted.free_if_copy();
            if (cached) {
                cb((index_sub_key_t)sk, cached->data, cached->size);
            } else {
                cb((index_sub_key_t)sk, tmp.data(), payload);
            }
        }
        systable_endscan_ordered(s);
        index_close(idx, AccessShareLock);
        relation_close(rel, AccessShareLock);
    }

    template <class F> void for_each_sub_key(F&& fn) const {
        Relation rel = open_index_relation_(false, AccessShareLock);
        if (rel == nullptr) return;
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[2];
        ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw());
        ScanKeyInit(&k[1], kAttSubKey,   BTLessStrategyNumber,  F_INT8LT,
                    bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(TEMP_RUN_BASE)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 2, k);
        std::int64_t last = std::numeric_limits<std::int64_t>::min(); bool have=false;
        for (HeapTuple t; (t = systable_getnext(s)) != nullptr; ) {
            bool isnull=false; std::int64_t sk = bioseq::pg_utils::pg_datum::to<std::int64_t>(
                bioseq::pg_utils::pg_datum::from_raw(heap_getattr(t, kAttSubKey, RelationGetDescr(rel), &isnull)));
            if (!have || sk != last) { fn((index_sub_key_t)sk); last = sk; have = true; }
        }
        systable_endscan(s); relation_close(rel, AccessShareLock);
    }

    // ---- lifecycle ----
    void delete_where_(
            std::optional<std::pair<index_sub_key_t, index_sub_key_t>>
                inclusive_sub_key_range) {
        if (inclusive_sub_key_range
            && inclusive_sub_key_range->second
                < inclusive_sub_key_range->first) {
            return;
        }
        // Opening through the legacy validator is intentional: a canonical
        // generation table has query_key/sub_key columns too, so name-based
        // DELETE SQL could otherwise erase immutable generations while
        // bypassing every old-attno check.
        Relation relation = open_index_relation_(false, RowExclusiveLock);
        if (relation == nullptr) return;
        const Oid primary_index = find_primary_key_oid(relation);
        if (!OidIsValid(primary_index)) {
            relation_close(relation, RowExclusiveLock);
            index_error("legacy fragment delete requires a primary index");
        }
        std::array<ScanKeyData, 3> keys{};
        ScanKeyInit(&keys[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ,
            bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw());
        int key_count = 1;
        if (inclusive_sub_key_range) {
            ScanKeyInit(&keys[1], kAttSubKey,
                BTGreaterEqualStrategyNumber, F_INT8GE,
                bioseq::pg_utils::pg_datum::from(
                    static_cast<std::int64_t>(
                        inclusive_sub_key_range->first)).raw());
            ScanKeyInit(&keys[2], kAttSubKey,
                BTLessEqualStrategyNumber, F_INT8LE,
                bioseq::pg_utils::pg_datum::from(
                    static_cast<std::int64_t>(
                        inclusive_sub_key_range->second)).raw());
            key_count = 3;
        }
        SysScanDesc scan = systable_beginscan(
            relation, primary_index, true, nullptr, key_count, keys.data());
        for (HeapTuple tuple;
             (tuple = systable_getnext(scan)) != nullptr;) {
            simple_heap_delete(relation, &tuple->t_self);
        }
        systable_endscan(scan);
        relation_close(relation, RowExclusiveLock);
        CommandCounterIncrement();
        evict_generation_storage_residency_();
    }
    void clear_entity() {
        delete_where_(std::nullopt);
        decoded_sub_ = DIRECTORY_SUB_KEY;
        decoded_chunk_ = (std::numeric_limits<std::size_t>::max)();
        decoded_bytes_ = 0;
    }
    void discard_sub_keys(index_sub_key_t lo, index_sub_key_t hi) {
        delete_where_(std::pair{lo, hi});
    }
    void clear(std::string_view name); // index-wide drop across all shards (defined below, uses seq_sub_table enumeration)

    [[nodiscard]] std::size_t run_buffer_bytes() const {
        if (run_buffer_override_bytes_ > 0) return run_buffer_override_bytes_;
        constexpr std::uint64_t floor = std::uint64_t{64} << 20;
        constexpr std::uint64_t ceil = std::uint64_t{8} << 30;
        const std::uint64_t desired = std::uint64_t{2} << 30;
        std::uint64_t capped =
            bioseq::memory::default_budget().clamp_working_set(desired, floor, ceil);
        const std::uint64_t index_cap =
            bioseq::parallelism::MemoryAdmissionController::instance().index_memory_cap_bytes();
        if (index_cap != 0) {
            capped = std::min(capped, index_cap);
        }
        return static_cast<std::size_t>(capped);
    }

    // ---- external sort spill (BufFile-backed sorted runs) ----
    [[nodiscard]] SortedPairRunStore& sorted_runs_ensure_() {
        if (!sorted_runs_.has_value()) {
            sorted_runs_.emplace();
        }
        return *sorted_runs_;
    }
    std::size_t spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted);
    std::size_t spill_sorted_run_owned(std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted);
	    std::size_t spill_sorted_run_owned_admitted(
	        std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted,
	        std::uint64_t admitted_bytes,
	        bioseq::parallelism::MemoryPermit permit);
	    void demote_resident_runs_until_available(std::uint64_t bytes_needed);
	    template <class F> void merge_runs(F&& on_kmer);
	    std::size_t spill_sorted_segment_run(
	        std::uint32_t position_segment,
	        std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted);
	    bool reroute_segment_runs(
	        std::uint64_t old_span,
	        std::uint64_t new_span,
	        std::uint64_t target_length);
	    template <class F> void merge_segment_runs(F&& on_segment_kmer);
	    [[nodiscard]] MpsSegmentSpillStorageBinding
	    begin_segment_spill_generation() noexcept;
	    [[nodiscard]] MpsSegmentSpillStorageBinding
	    segment_spill_storage_binding() const noexcept {
	        return segment_spill_binding_;
	    }
	    [[nodiscard]] SortedRunStoreStats sorted_run_stats() const noexcept;
	    void snapshot_sorted_run_stats_before_release();
    void release_runs_noexcept() noexcept;
    void release_runs();
};

// A bounded position cursor over one kmer's unique positions (one vector).
struct PositionBatchCursor {
    const std::vector<std::uint64_t>* v = nullptr;
    [[nodiscard]] std::uint64_t total_count() const { return v->size(); }
    template <class F> void for_each_batch(F&& f) const {
        if (!v->empty()) f(std::span<const std::uint64_t>(v->data(), v->size()));
    }
};

// =========================================================================
//  IndexStorageSequencePg  —  INLINE_PER_SEQUENCE
// =========================================================================
struct IndexStorageSequencePg {
    Oid           db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid, user_relid_ = InvalidOid;
    seq_caches*   read_caches_ = nullptr;
    std::int16_t  column_attno_ = 0;
    std::string_view index_name_{};
    std::uint8_t  bucket_idx_ = 0;
    std::uint32_t shard_idx_ = 0;
    std::uint64_t query_key_ = 0;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid   index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr;
    mutable std::size_t   decoded_units_ = 0;
    mutable std::size_t   decoded_bytes_ = 0;
    mutable bool          decoded_valid_ = false;
    std::size_t           planned_bytes_ = std::numeric_limits<std::size_t>::max();

    IndexStorageSequencePg() = default;
    IndexStorageSequencePg(const IndexStorageSequencePg&) = delete;
    IndexStorageSequencePg& operator=(const IndexStorageSequencePg&) = delete;
    IndexStorageSequencePg(IndexStorageSequencePg&& o) noexcept { steal_(std::move(o)); }
    IndexStorageSequencePg& operator=(IndexStorageSequencePg&& o) noexcept { if (this!=&o){release_(); steal_(std::move(o));} return *this; }
    ~IndexStorageSequencePg() { release_(); }
    void steal_(IndexStorageSequencePg&& o) noexcept {
        db_oid_=o.db_oid_; tablespace_oid_=o.tablespace_oid_; main_relfilenode_=o.main_relfilenode_; user_relid_=o.user_relid_;
        read_caches_=o.read_caches_; column_attno_=o.column_attno_; index_name_=o.index_name_;
        bucket_idx_=o.bucket_idx_; shard_idx_=o.shard_idx_; query_key_=o.query_key_;
        own_mctx_=o.own_mctx_; index_relid_=o.index_relid_; decoded_=o.decoded_; decoded_units_=o.decoded_units_;
        decoded_bytes_=o.decoded_bytes_; decoded_valid_=o.decoded_valid_; planned_bytes_=o.planned_bytes_;
        o.own_mctx_=nullptr; o.decoded_=nullptr; o.decoded_units_=0; o.index_relid_=InvalidOid; o.decoded_valid_=false;
    }
    void release_() noexcept { if (own_mctx_!=nullptr){ MemoryContextDelete(own_mctx_); own_mctx_=nullptr; decoded_=nullptr; } }

    [[noreturn]] void index_error(std::string_view msg) const { bioseq_log<ERR, PG>( "bioseq index: {}", msg); abort(); }

    [[nodiscard]] std::string this_table_name() const {
        return format_seqidx_name(index_name_, db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, bucket_idx_, shard_idx_);
    }
    [[nodiscard]] Oid resolve_relid_(bool create_if_missing) const {
        if (OidIsValid(index_relid_)) return index_relid_;
        const std::string nm = this_table_name(); const Oid ns = bioseq_internal_ns();
        Oid rid = get_relname_relid(nm.c_str(), ns);
        if (!OidIsValid(rid) && create_if_missing) {
            const char* pk = persistence_kw_for(user_relid_);
            std::string ddl = fmt::format(
                "CREATE {} TABLE IF NOT EXISTS bioseq_internal.{} ("
                "query_key BIGINT NOT NULL, chunk_bytes INT NOT NULL, blob BYTEA NOT NULL, "
                "PRIMARY KEY (query_key)) USING heap;", pk, quote_identifier(nm.c_str()));
            run_internal_ddl(ddl.c_str());
            run_internal_ddl(fmt::format(
                "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
                quote_identifier(nm.c_str())).c_str());
            rid = get_relname_relid(nm.c_str(), ns);
            if (!OidIsValid(rid)) index_error("failed to create inline seqidx table");
        }
        index_relid_ = rid; return rid;
    }
    [[nodiscard]] Relation open_(bool create, LOCKMODE lock) const {
        const Oid rid = resolve_relid_(create);
        if (!OidIsValid(rid)) return nullptr;
        return try_safe_relation_open(rid, lock);
    }
    void ensure_buffer_(std::size_t units) const {
        if (decoded_units_ >= units && decoded_!=nullptr) return;
        MemoryContext old = MemoryContextSwitchTo(own_mctx_);
        decoded_ = (decoded_==nullptr)? (std::int64_t*)palloc(units*sizeof(std::int64_t)) : (std::int64_t*)repalloc(decoded_, units*sizeof(std::int64_t));
        decoded_units_ = units; MemoryContextSwitchTo(old);
    }
    void load_() const {
        Relation rel = open_(false, AccessShareLock);
        if (rel == nullptr) { decoded_bytes_=0; decoded_valid_=true; return; }
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[1]; ScanKeyInit(&k[0], 1, BTEqualStrategyNumber, F_INT8EQ,
            bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
        HeapTuple t = systable_getnext(s);
        if (HeapTupleIsValid(t)) {
            bool isnull=false; Datum d = heap_getattr(t, 3, RelationGetDescr(rel), &isnull);
            auto raw_detoasted = bioseq::pg_utils::detoast_packed_as<bytea>(
                bioseq::pg_utils::pg_datum::from_raw(d));
            bytea* raw = raw_detoasted.ptr;
            const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
            const std::size_t units = (payload + sizeof(std::int64_t) - 1)/sizeof(std::int64_t);
            ensure_buffer_(units==0?1:units);
            if (payload>0) std::memcpy(decoded_, VARDATA_ANY(raw), payload);
            raw_detoasted.free_if_copy();
            decoded_bytes_ = payload;
        } else decoded_bytes_ = 0;
        decoded_valid_ = true;
        systable_endscan(s); relation_close(rel, AccessShareLock);
    }
    [[nodiscard]] std::size_t number_of_chunks() const { return chunk_byte_size() == 0 ? 0 : 1; }
    [[nodiscard]] std::size_t chunk_byte_size() const { if (!decoded_valid_) load_(); return decoded_bytes_; }
    template <class T> [[nodiscard]] const T* read_chunk() { if (!decoded_valid_) load_(); return reinterpret_cast<const T*>(decoded_); }
    void resize(std::size_t bytes) { if (bytes > MAX_INDEX_CHUNK_SIZE_BYTES) bioseq_log<ERR, PG>( "bioseq index inline: bytes exceed single-chunk MAX"); planned_bytes_ = bytes; }
    void prefetch_chunk() {}
    template <class T, class F> void write_to_each(F&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<F, T>, "inline write_to_each writer shape");
        if (planned_bytes_ == std::numeric_limits<std::size_t>::max()) bioseq_log<ERR, PG>( "bioseq index inline: write_to_each without resize");
        const std::size_t payload = planned_bytes_;
        if (payload % sizeof(T) != 0) bioseq_log<ERR, PG>( "bioseq index inline: payload not whole T");
        std::vector<char> buf(payload, 0);
        const std::size_t next = writer(0, reinterpret_cast<T*>(buf.data()), payload / sizeof(T));
        if (next != 0) bioseq_log<ERR, PG>( "bioseq index inline: writer emitted >1 chunk");
        Relation rel = open_(true, RowExclusiveLock);
        // upsert (delete then insert) so rebuild replaces the single row.
        { const Oid pk = find_primary_key_oid(rel);
          ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,
              bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw());
          SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
          HeapTuple old_t = systable_getnext(s);
          if (HeapTupleIsValid(old_t)) simple_heap_delete(rel, &old_t->t_self);
          systable_endscan(s); }
        bytea* blob = reinterpret_cast<bytea*>(palloc(VARHDRSZ + payload));
        SET_VARSIZE(blob, VARHDRSZ + payload); if (payload>0) std::memcpy(VARDATA(blob), buf.data(), payload);
        bioseq::pg_utils::pg_datum_list<3> values{};
        values.set(1, static_cast<std::int64_t>(query_key_));
        values.set(2, static_cast<std::int32_t>(payload));
        values.set_raw(3, bioseq::pg_utils::pg_datum::from_pointer(blob));
        HeapTuple tup = values.form_tuple(RelationGetDescr(rel));
        CatalogTupleInsert(rel, tup); heap_freetuple(tup); pfree(blob);
        relation_close(rel, RowExclusiveLock);
        decoded_valid_ = false; planned_bytes_ = std::numeric_limits<std::size_t>::max();
    }
    void clear_entity() {
        const Oid rid = resolve_relid_(false); if (!OidIsValid(rid)) return;
        std::string sql = fmt::format("DELETE FROM bioseq_internal.{} WHERE query_key = {}", quote_identifier(this_table_name().c_str()), (long long)query_key_);
        if (SPI_connect()==SPI_OK_CONNECT){ SPI_execute(sql.c_str(), false, 0); SPI_finish(); }
        decoded_valid_ = false;
    }
    void clear(std::string_view name);
    /// pg-only reuse hook (§8.3) — reposition without re-allocating own_mctx_.
    void reset_entity(const seq_locator& loc, std::uint64_t sequence_rowno) {
        bucket_idx_ = (std::uint8_t)loc.bucket_idx; shard_idx_ = loc.shard_idx; query_key_ = sequence_rowno;
        index_relid_ = InvalidOid; decoded_valid_ = false; decoded_bytes_ = 0;
    }
};

// =========================================================================
//  IndexStorageTablePg  —  CHUNKED_PER_TABLE
// =========================================================================
struct IndexStorageTablePg {
    Oid db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid, user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string_view index_name_{};
    seq_caches* read_caches_ = nullptr;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr; mutable std::size_t decoded_units_=0, decoded_bytes_=0;
    mutable std::int64_t decoded_chunk_ = -1;
    std::size_t planned_bytes_for_chunk_ = 0;

    IndexStorageTablePg() = default;
    IndexStorageTablePg(const IndexStorageTablePg&) = delete;
    IndexStorageTablePg& operator=(const IndexStorageTablePg&) = delete;
    IndexStorageTablePg(IndexStorageTablePg&& o) noexcept { steal_(std::move(o)); }
    IndexStorageTablePg& operator=(IndexStorageTablePg&& o) noexcept { if (this!=&o){release_(); steal_(std::move(o));} return *this; }
    ~IndexStorageTablePg() { release_(); }
    void steal_(IndexStorageTablePg&& o) noexcept {
        db_oid_=o.db_oid_; tablespace_oid_=o.tablespace_oid_; main_relfilenode_=o.main_relfilenode_; user_relid_=o.user_relid_;
        column_attno_=o.column_attno_; index_name_=o.index_name_; read_caches_=o.read_caches_; own_mctx_=o.own_mctx_;
        index_relid_=o.index_relid_; decoded_=o.decoded_; decoded_units_=o.decoded_units_; decoded_bytes_=o.decoded_bytes_;
        decoded_chunk_=o.decoded_chunk_; planned_bytes_for_chunk_=o.planned_bytes_for_chunk_;
        o.own_mctx_=nullptr; o.decoded_=nullptr; o.index_relid_=InvalidOid; o.decoded_units_=0;
    }
    void release_() noexcept { if (own_mctx_!=nullptr){ MemoryContextDelete(own_mctx_); own_mctx_=nullptr; decoded_=nullptr; } }
    [[noreturn]] void index_error(std::string_view msg) const { bioseq_log<ERR, PG>( "bioseq index: {}", msg); abort(); }

    [[nodiscard]] std::string this_table_name(std::int32_t shard=0) const {
        return format_seqidxtab_name(index_name_, db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, shard);
    }
    [[nodiscard]] Oid resolve_relid_(bool create) const {
        if (OidIsValid(index_relid_)) return index_relid_;
        const std::string nm = this_table_name(0); const Oid ns = bioseq_internal_ns();
        Oid rid = get_relname_relid(nm.c_str(), ns);
        if (!OidIsValid(rid) && create) {
            const char* pk = persistence_kw_for(user_relid_);
            std::string ddl = fmt::format(
                "CREATE {} TABLE IF NOT EXISTS bioseq_internal.{} ("
                "chunk_no BIGINT NOT NULL, chunk_bytes INT NOT NULL, blob BYTEA NOT NULL, "
                "PRIMARY KEY (chunk_no)) USING heap;", pk, quote_identifier(nm.c_str()));
            run_internal_ddl(ddl.c_str());
            run_internal_ddl(fmt::format(
                "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
                quote_identifier(nm.c_str())).c_str());
            rid = get_relname_relid(nm.c_str(), ns);
        }
        index_relid_ = rid; return rid;
    }
    [[nodiscard]] Relation open_(bool create, LOCKMODE lock) const { const Oid rid = resolve_relid_(create); if (!OidIsValid(rid)) return nullptr; return try_safe_relation_open(rid, lock); }
    void ensure_buffer_(std::size_t units) const {
        if (decoded_units_>=units && decoded_!=nullptr) return;
        MemoryContext old = MemoryContextSwitchTo(own_mctx_);
        decoded_ = (decoded_==nullptr)?(std::int64_t*)palloc(units*sizeof(std::int64_t)):(std::int64_t*)repalloc(decoded_, units*sizeof(std::int64_t));
        decoded_units_=units; MemoryContextSwitchTo(old);
    }
    [[nodiscard]] std::size_t number_of_chunks() const {
        Relation rel = open_(false, AccessShareLock); if (rel==nullptr) return 0;
        SysScanDesc s = systable_beginscan(rel, InvalidOid, false, nullptr, 0, nullptr);
        std::size_t n=0; for (; systable_getnext(s)!=nullptr;) ++n; systable_endscan(s); relation_close(rel, AccessShareLock); return n;
    }
    [[nodiscard]] std::size_t chunk_byte_size(std::size_t chunk_no) const {
        Relation rel = open_(false, AccessShareLock); if (rel==nullptr) return 0;
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,
            bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(chunk_no)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
        std::size_t out=0; HeapTuple t = systable_getnext(s);
        if (HeapTupleIsValid(t)){ bool isnull=false; out=static_cast<std::size_t>(bioseq::pg_utils::pg_datum::to<std::int32_t>(
            bioseq::pg_utils::pg_datum::from_raw(heap_getattr(t,2,RelationGetDescr(rel),&isnull)))); }
        systable_endscan(s); relation_close(rel, AccessShareLock); return out;
    }
    template <class T> [[nodiscard]] const T* read_chunk(std::size_t chunk_no) {
        if (decoded_!=nullptr && decoded_chunk_==(std::int64_t)chunk_no) return reinterpret_cast<const T*>(decoded_);
        Relation rel = open_(false, AccessShareLock);
        if (rel==nullptr) index_error("table index relation missing");
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,
            bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(chunk_no)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
        HeapTuple t = systable_getnext(s);
        if (!HeapTupleIsValid(t)){ systable_endscan(s); relation_close(rel,AccessShareLock); index_error("table chunk missing"); }
        bool isnull=false; Datum d = heap_getattr(t,3,RelationGetDescr(rel),&isnull);
        auto raw_detoasted = bioseq::pg_utils::detoast_packed_as<bytea>(
            bioseq::pg_utils::pg_datum::from_raw(d));
        bytea* raw = raw_detoasted.ptr;
        const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
        const std::size_t units = (payload+sizeof(std::int64_t)-1)/sizeof(std::int64_t);
        ensure_buffer_(units==0?1:units); if (payload>0) std::memcpy(decoded_, VARDATA_ANY(raw), payload);
        raw_detoasted.free_if_copy();
        decoded_chunk_=(std::int64_t)chunk_no; decoded_bytes_=payload;
        systable_endscan(s); relation_close(rel, AccessShareLock);
        return reinterpret_cast<const T*>(decoded_);
    }
    void resize(std::size_t bytes) { planned_bytes_for_chunk_ = bytes; }
    void prefetch_chunk(std::size_t) {}
    template <class T, class F> void write_to_each(F&& writer) { write_to<T>(0, std::forward<F>(writer)); }
    template <class T, class F> void write_to(std::size_t chunk_no, F&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<F, T>, "table write_to writer shape");
        const std::size_t payload = planned_bytes_for_chunk_;
        if (payload % sizeof(T) != 0) bioseq_log<ERR, PG>( "bioseq index table: payload not whole T");
        std::vector<char> buf(payload, 0);
        writer(chunk_no, reinterpret_cast<T*>(buf.data()), payload / sizeof(T));
        Relation rel = open_(true, RowExclusiveLock);
        { const Oid pk = find_primary_key_oid(rel);
          ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,
              bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(chunk_no)).raw());
          SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
          HeapTuple old_t = systable_getnext(s); if (HeapTupleIsValid(old_t)) simple_heap_delete(rel, &old_t->t_self);
          systable_endscan(s); }
        bytea* blob = reinterpret_cast<bytea*>(palloc(VARHDRSZ + payload)); SET_VARSIZE(blob, VARHDRSZ + payload);
        if (payload>0) std::memcpy(VARDATA(blob), buf.data(), payload);
        bioseq::pg_utils::pg_datum_list<3> values{};
        values.set(1, static_cast<std::int64_t>(chunk_no));
        values.set(2, static_cast<std::int32_t>(payload));
        values.set_raw(3, bioseq::pg_utils::pg_datum::from_pointer(blob));
        HeapTuple tup = values.form_tuple(RelationGetDescr(rel));
        CatalogTupleInsert(rel, tup); heap_freetuple(tup); pfree(blob);
        relation_close(rel, RowExclusiveLock); decoded_chunk_ = -1;
    }
    template <class F> void for_each_sequence(F&& loader);
    void clear(std::string_view name);
};

// ---- shared drop-by-prefix helper -----------------------------------------
inline void clear_with_prefix_(const std::string& prefix) {
    // escape LIKE metacharacters in the literal prefix
    std::string esc; for (char c : prefix) { if (c=='_'||c=='%'||c=='\\') esc.push_back('\\'); esc.push_back(c); }
    std::string sql = fmt::format(
        "DO $$ DECLARE r record; BEGIN "
        "FOR r IN SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='bioseq_internal' AND c.relname LIKE '{}%' ESCAPE '\\' LOOP "
        "EXECUTE format('DROP TABLE IF EXISTS bioseq_internal.%I CASCADE', r.relname); END LOOP; END $$;", esc);
    if (SPI_connect()==SPI_OK_CONNECT){ SPI_execute(sql.c_str(), false, 0); SPI_finish(); }
}

inline void IndexStorageFragmentPg::clear(std::string_view name) {
    clear_with_prefix_(fmt::format("seqidx_{}_{:016x}_", short_index_for(name),
                                   identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid;
}
inline void IndexStorageSequencePg::clear(std::string_view name) {
    clear_with_prefix_(fmt::format("seqidx_{}_{:016x}_", short_index_for(name),
                                   identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid; decoded_valid_ = false;
}
inline void IndexStorageTablePg::clear(std::string_view name) {
    clear_with_prefix_(fmt::format("seqidxtab_{}_{:016x}_", short_index_for(name),
                                   identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid;
}

inline std::size_t IndexStorageFragmentPg::spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted) {
    return sorted_runs_ensure_().spill_sorted_run(sorted);
}

inline std::size_t IndexStorageFragmentPg::spill_sorted_run_owned(
        std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted) {
    return sorted_runs_ensure_().spill_sorted_run_owned(std::move(sorted));
}

	inline std::size_t IndexStorageFragmentPg::spill_sorted_run_owned_admitted(
	        std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted,
	        std::uint64_t admitted_bytes,
	        bioseq::parallelism::MemoryPermit permit) {
	    return sorted_runs_ensure_().spill_sorted_run_owned_admitted(
	        std::move(sorted), admitted_bytes, std::move(permit));
	}

	inline void IndexStorageFragmentPg::demote_resident_runs_until_available(
	        std::uint64_t bytes_needed) {
	    if (sorted_runs_.has_value()) {
	        sorted_runs_->demote_resident_runs_until_available(bytes_needed);
	    }
	}

template <class F>
inline void IndexStorageFragmentPg::merge_runs(F&& on_kmer) {
    if (!sorted_runs_.has_value()) {
        return;
    }
    g_last_index_merge_run_count = std::max<std::uint64_t>(
        g_last_index_merge_run_count, sorted_runs_->run_count());
    sorted_runs_->merge_runs(std::forward<F>(on_kmer));
}

inline MpsSegmentSpillStorageBinding
IndexStorageFragmentPg::begin_segment_spill_generation() noexcept {
    if (segment_spill_pg_poisoned_) {
        segment_spill_binding_ = {};
        return segment_spill_binding_;
    }
    for (auto& [segment, runs] : segment_sorted_runs_) {
        (void)segment;
        runs.release_runs_noexcept();
    }
    segment_sorted_runs_.clear();
    std::uint64_t token = next_segment_spill_generation_token_.load(
        std::memory_order_relaxed);
    for (;;) {
        if (token == 0) {
            segment_spill_binding_ = {};
            return segment_spill_binding_;
        }
        const std::uint64_t next = token
                == (std::numeric_limits<std::uint64_t>::max)()
            ? 0u : token + 1u;
        if (next_segment_spill_generation_token_.compare_exchange_weak(
                token,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            break;
        }
    }
    segment_spill_binding_ = MpsSegmentSpillStorageBinding{
        .generation_token = token,
        .mutation_version = 0,
    };
    return segment_spill_binding_;
}

inline void IndexStorageFragmentPg::record_segment_spill_mutation_() noexcept {
    if (!segment_spill_binding_.valid()) {
        return;
    }
    if (segment_spill_binding_.mutation_version
            == (std::numeric_limits<std::uint64_t>::max)()) {
        segment_spill_binding_ = {};
        return;
    }
    ++segment_spill_binding_.mutation_version;
}

inline std::size_t IndexStorageFragmentPg::spill_sorted_segment_run(
        std::uint32_t position_segment,
        std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted) {
    if (segment_spill_pg_poisoned_) {
        rev3_storage_error_as_cpp(
            "poisoned MPS spill storage reused after PostgreSQL error");
    }
    if (!segment_spill_binding_.valid()) {
        (void)begin_segment_spill_generation();
    }
    auto [iterator, inserted] = segment_sorted_runs_.try_emplace(
        position_segment);
    (void)inserted;
    // SortedPairRunStore may compact/demote physical runs internally. Those
    // transformations preserve the exact canonical pair stream; this one
    // version increment therefore names the logical run injection, not its
    // eventual memory-vs-BufFile representation.
    try {
        inject_rev3_pg_failure_as_cpp(
            Rev3PgFailureInjection::mps_spill, "mps_spill");
        const std::size_t run = iterator->second.spill_sorted_run(sorted);
        record_segment_spill_mutation_();
        return run;
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
}

inline bool IndexStorageFragmentPg::reroute_segment_runs(
        std::uint64_t old_span,
        std::uint64_t new_span,
        std::uint64_t target_length) {
    if (!segment_spill_binding_.valid()
        || old_span == 0 || new_span == 0) {
        if (segment_spill_pg_poisoned_) {
            rev3_storage_error_as_cpp(
                "poisoned MPS reroute storage reused after PostgreSQL error");
        }
        return false;
    }
    std::map<std::uint32_t, SortedPairRunStore> rerouted;
    bool valid = true;
    try {
        for (auto& [old_segment, runs] : segment_sorted_runs_) {
            runs.merge_runs(
                [&](std::uint64_t hash, auto&& positions) {
                    positions.for_each_batch(
                        [&](std::span<const std::uint64_t> batch) {
                            if (!valid) {
                                return;
                            }
                            std::map<
                                std::uint32_t,
                                std::vector<std::pair<std::uint64_t,std::uint64_t>>>
                                routed_batch;
                            for (const std::uint64_t encoded_position : batch) {
                                const std::uint64_t position = encoded_position >> 1u;
                                const std::uint64_t new_segment = position / new_span;
                                if (position >= target_length
                                    || position / old_span != old_segment
                                    || new_segment
                                        > (std::numeric_limits<std::uint32_t>::max)()) {
                                    valid = false;
                                    return;
                                }
                                routed_batch[static_cast<std::uint32_t>(new_segment)]
                                    .emplace_back(hash, encoded_position);
                            }
                            for (auto& [new_segment, routed] : routed_batch) {
                                if (routed.empty()) {
                                    continue;
                                }
                                auto [iterator, inserted] = rerouted.try_emplace(
                                    new_segment);
                                (void)inserted;
                                iterator->second.spill_sorted_run(
                                    std::span<const std::pair<
                                        std::uint64_t,std::uint64_t>>{routed});
                            }
                        });
                });
            if (!valid) {
                return false;
            }
        }
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        for (auto& [segment, runs] : rerouted) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
    segment_sorted_runs_.swap(rerouted);
    record_segment_spill_mutation_();
    return true;
}

template <class F>
inline void IndexStorageFragmentPg::merge_segment_runs(
        F&& on_segment_kmer) {
    if (segment_spill_pg_poisoned_) {
        rev3_storage_error_as_cpp(
            "poisoned MPS merge storage reused after PostgreSQL error");
    }
    std::uint64_t total_runs = 0;
    for (const auto& [segment, runs] : segment_sorted_runs_) {
        (void)segment;
        total_runs += runs.run_count();
    }
    g_last_index_merge_run_count = std::max(
        g_last_index_merge_run_count, total_runs);
    try {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            bool first_callback = true;
            runs.merge_runs(
                [&on_segment_kmer, segment, &first_callback](
                        std::uint64_t hash,
                        auto&& positions) {
                    if (first_callback) {
                        first_callback = false;
                        inject_rev3_pg_failure_as_cpp(
                            Rev3PgFailureInjection::mps_merge,
                            "mps_merge_callback");
                    }
                    on_segment_kmer(segment, hash, positions);
                });
        }
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
}

inline SortedRunStoreStats IndexStorageFragmentPg::sorted_run_stats() const noexcept {
    return sorted_runs_.has_value() ? sorted_runs_->stats() : SortedRunStoreStats{};
}

inline void IndexStorageFragmentPg::snapshot_sorted_run_stats_before_release() {
    if (!sorted_runs_.has_value()) {
        return;
    }
    const SortedRunStoreStats st = sorted_runs_->stats();
    const std::uint64_t total_runs = sorted_runs_->run_count();
    g_last_index_merge_run_count = total_runs;
    g_last_mps_build_stats = LastMpsBuildStats{
        .backend_pid = static_cast<std::uint64_t>(MyProcPid),
        .db_oid = static_cast<std::uint64_t>(db_oid_),
        .relfilenode = static_cast<std::uint64_t>(main_relfilenode_),
        .sequence_rowno = static_cast<std::uint64_t>(query_key_),
        .total_run_count = total_runs,
        .resident_runs = st.resident_runs,
        .file_runs = st.file_runs,
        .resident_run_bytes = st.resident_bytes,
        .resident_peak_bytes = st.resident_peak_bytes,
        .spilled_run_bytes = st.spilled_bytes,
        .postgres_temp_file_bytes = st.postgres_temp_file_bytes,
        .postgres_temp_file_count = st.postgres_temp_file_count,
        .merge_reader_buffer_bytes = st.merge_reader_buffer_bytes,
        .merge_pass_count = st.merge_pass_count,
        .spill_count_from_pressure = st.spill_count_from_pressure,
        .resident_demotions = st.resident_demotions,
        .position_spill_bytes = st.position_spill_bytes,
        .position_spill_count = st.position_spill_count,
    };
    bioseq_log<DBG>("[bioseq.index.mps_build.row] backend_pid={} db_oid={} relfilenode={} sequence_rowno={} "
            "resident_runs={} file_runs={} resident_run_bytes={} resident_peak_bytes={} "
            "spilled_run_bytes={} postgres_temp_file_bytes={} postgres_temp_file_count={} "
            "merge_reader_buffer_bytes={} merge_pass_count={} spill_count_from_pressure={} "
            "resident_demotions={} position_spill_bytes={} position_spill_count={}",
            MyProcPid,
            static_cast<unsigned long long>(db_oid_),
            static_cast<unsigned long long>(main_relfilenode_),
            static_cast<unsigned long long>(query_key_),
            static_cast<unsigned long long>(st.resident_runs),
            static_cast<unsigned long long>(st.file_runs),
            static_cast<unsigned long long>(st.resident_bytes),
            static_cast<unsigned long long>(st.resident_peak_bytes),
            static_cast<unsigned long long>(st.spilled_bytes),
            static_cast<unsigned long long>(st.postgres_temp_file_bytes),
            static_cast<unsigned long long>(st.postgres_temp_file_count),
            static_cast<unsigned long long>(st.merge_reader_buffer_bytes),
            static_cast<unsigned long long>(st.merge_pass_count),
            static_cast<unsigned long long>(st.spill_count_from_pressure),
            static_cast<unsigned long long>(st.resident_demotions),
            static_cast<unsigned long long>(st.position_spill_bytes),
            static_cast<unsigned long long>(st.position_spill_count));
}

inline void IndexStorageFragmentPg::release_runs_noexcept() noexcept {
    if (sorted_runs_.has_value()) {
        sorted_runs_->release_runs_noexcept();
        sorted_runs_.reset();
    }
    for (auto& [segment, runs] : segment_sorted_runs_) {
        (void)segment;
        runs.release_runs_noexcept();
    }
    segment_sorted_runs_.clear();
    record_segment_spill_mutation_();
}

inline void IndexStorageFragmentPg::release_runs() {
    release_runs_noexcept();
}

inline void write_serialized_index_chunk_pg(
        Oid user_relid,
        Oid db_oid,
        Oid tablespace_oid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        std::string_view index_name,
        std::uint8_t bucket_idx,
        std::uint32_t shard_idx,
        std::uint64_t sequence_rowno,
        index_sub_key_t sub_key,
        std::uint32_t chunk_no,
        std::span<const char> bytes) {
    IndexStorageFragmentPg storage;
    storage.db_oid_ = db_oid;
    storage.tablespace_oid_ = tablespace_oid;
    storage.main_relfilenode_ = main_relfilenode;
    storage.user_relid_ = user_relid;
    storage.column_attno_ = column_attno;
    storage.index_name_ = index_name;
    storage.bucket_idx_ = bucket_idx;
    storage.shard_idx_ = shard_idx;
    storage.query_key_ = sequence_rowno;
    storage.own_mctx_ = AllocSetContextCreate(
        CurrentMemoryContext,
        "bioseq_async_index_persist",
        ALLOCSET_SMALL_SIZES);
    Relation rel = storage.open_index_relation_(true, RowExclusiveLock);
    if (rel == nullptr) {
        storage.index_error("async persist relation missing");
    }
    storage.upsert_chunk_row_(
        rel,
        sequence_rowno,
        sub_key,
        chunk_no,
        bytes.size(),
        bytes);
    storage.admit_cached_chunk_(sub_key, chunk_no, bytes, false);
    relation_close(rel, RowExclusiveLock);
}

inline void mark_serialized_index_write_successful_pg(
        Oid user_relid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        std::string_view index_name) {
    if (SPI_connect() != SPI_OK_CONNECT) {
        return;
    }
    const std::string sql = fmt::format(
        "INSERT INTO bioseq_internal.metadata_age_log "
        "(user_relid, main_relfilenode, column_attno, index_name, last_updated) "
        "VALUES ({}::oid, {}::oid, {}, {}, now()) "
        "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) "
        "DO UPDATE SET last_updated = now()",
        static_cast<unsigned>(user_relid),
        static_cast<unsigned>(main_relfilenode),
        static_cast<int>(column_attno),
        quote_literal_cstr(std::string(index_name).c_str()));
    SPI_execute(sql.c_str(), false, 0);
    SPI_finish();
}

// ---- IndexStorageTablePg::for_each_sequence (test-only CHUNKED_PER_TABLE) ---
template <class F>
inline void IndexStorageTablePg::for_each_sequence(F&& loader) {
    Relation main = table_open(user_relid_, AccessShareLock);
    TupleDesc td = RelationGetDescr(main);
    Snapshot snap = RegisterSnapshot(GetTransactionSnapshot());
    const std::size_t total = (std::size_t)BioseqColumnTableRowCount(main);
    List* projected = list_make1_int(column_attno_);
    BioseqColumnReadState* rs = BioseqColumnBeginRead(main, td, projected, NIL, own_mctx_, snap, /*randomAccess=*/false, nullptr);
    MemoryContext inner = AllocSetContextCreate(own_mctx_, "seqidx_table_inner_seq", ALLOCSET_DEFAULT_SIZES);
    std::vector<Datum> vals(td->natts);
    std::vector<bool>  nullsv(td->natts);
    Datum* valp = vals.data();
    std::vector<char> nullbuf(td->natts);
    bool* nulls = reinterpret_cast<bool*>(nullbuf.data());
    std::uint64_t rowno = 0;
	    while (BioseqColumnReadNextRow(rs, valp, nulls, &rowno)) {
	        if (nulls[column_attno_ - 1]) continue;
	        MemoryContextReset(inner);
	        MemoryContext old = MemoryContextSwitchTo(inner);
	        auto hdr_detoasted = bioseq::pg_utils::detoast_as<pgseq_t>(
	            bioseq::pg_utils::pg_datum::from_raw(valp[column_attno_ - 1]));
	        pgseq_t* hdr = hdr_detoasted.ptr;
	        MemoryContextSwitchTo(old);
        const std::uint8_t alpha = hdr->alphabet_code;
        const std::uint8_t bw = logical_storage_unit_byte_width(hdr);
        if (!is_locator_stub(hdr)) {
            pgseq_t::dispatch_instance(alpha, bw, [&]<class S>(){
                compact_sequence_range<S> rng{*hdr};
                bioseq::storage::lazy_compact_sequence<S, bioseq::storage::sequence_scanner_pg<S>>
                    lazy{ bioseq::storage::make_scanner_pg_in_memory<S>(rng) };
                loader(lazy, (std::size_t)rowno, total);
            });
        } else {
            const seq_locator loc = decode_locator_from_stub(hdr);
            pgseq_t::dispatch_instance(alpha, bw, [&]<class S>(){
                constexpr std::size_t lpu = compact_sequence_range<S>::traits::letters_per_unit;
                const std::size_t pad = (loc.total_bp_size % lpu) == 0
                    ? 0
                    : static_cast<std::size_t>(
                        lpu - loc.total_bp_size % lpu);
                bioseq::storage::with_lazy_postgres<S>(
                    db_oid_, tablespace_oid_, main_relfilenode_,
                    column_attno_, loc, pad, rs->snapshot, read_caches_, inner,
                    [&](auto& lazy_seq){
                        loader(lazy_seq, static_cast<std::size_t>(rowno), total);
                    });
            });
        }
        hdr_detoasted.free_if_copy();
    }
    // with_lazy_postgres keyed cached fragment readers by rs->snapshot.
    // Release them before the outer reader unregisters a refreshed snapshot.
    if (read_caches_ != nullptr) {
        read_caches_->close_all(/*commit=*/false);
    }
    BioseqColumnEndRead(rs);
    UnregisterSnapshot(snap);
    MemoryContextDelete(inner);
    table_close(main, AccessShareLock);
}

// ---- conformance gates (§8) ----
template <class Storage>
concept publicly_mutable_mps_segment_spill_state = requires(Storage& storage) {
    storage.segment_sorted_runs_;
    storage.segment_spill_binding_;
    storage.segment_spill_pg_poisoned_;
    storage.next_segment_spill_generation_token_;
};
static_assert(!publicly_mutable_mps_segment_spill_state<
    IndexStorageFragmentPg>);
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  IndexStorageFragmentPg, SequenceIndexType::CHUNKED_PER_FRAGMENT>,
              "IndexStorageFragmentPg must satisfy CHUNKED_PER_FRAGMENT");
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  IndexStorageSequencePg, SequenceIndexType::INLINE_PER_SEQUENCE>,
              "IndexStorageSequencePg must satisfy INLINE_PER_SEQUENCE");
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  IndexStorageTablePg, SequenceIndexType::CHUNKED_PER_TABLE>,
              "IndexStorageTablePg must satisfy CHUNKED_PER_TABLE");

}  // namespace bioseq::storage::indexing
