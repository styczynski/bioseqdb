/*-------------------------------------------------------------------------
 *
 * storage/sequence_storage_manager.cppm
 *
 * Phase A (DOC0260) length-bucketed sub-table sharding for sequence
 * storage. Owns:
 *   - the locator stub format (a fixed-size pgseq_t-shaped varlena that
 *     the main columnar relation stores in place of the full sequence);
 *   - the consteval bucket-threshold builder and the per-call
 *     bucket_index_for_size LUT;
 *   - the per-scan / per-batch seq_caches struct that consumers thread
 *     through every hot-path entry point (no thread-local state);
 *   - the stateless sequence_storage_manager<len_t, min, max> template
 *     that drives shard create/open and the write/read hot paths;
 *   - the bioseq_default_storage_manager =
 *     sequence_storage_manager<sequence_size_t, 8, 42> alias used by the
 *     TAM and dispatch buffer.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

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
#include "miscadmin.h"
#include "fmgr.h"
#include "nodes/primnodes.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.storage.manager:sequence_storage_manager;

import bioseq.common.mlogging;
import bioseq.pg_utils;
import bioseq.config;                  // DOC0322 §9.3 — bioseq_storage_max_seq_sub_table_gbs GUC
import bioseq.storage.metadata;
import bioseq.storage.physical;
import bioseq.table.io;
import bioseq.types.pg_compact_sequence;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.indexing.storage;
import bioseq.common.sequan_compat;  // DOC0264 §8.1 — ceil_log2 used in letters_per_unit_for
import bioseq.storage.size;  // DOC0263 §2.2 — standalone module (was :sequence_size)
import bioseq.storage.coordinates;
// DOC0260 §12.1 — the PG-free bucket math + seq_locator POD live in their own
// module so the unit-test TU can link them without the Postgres backend.
// Re-exported here so existing `import bioseq_storage` consumers are unchanged.
export import bioseq.storage.bucket_math;

static_assert(__cplusplus >= 202302L,
              "DOC0260 requires C++23-or-later (project default: C++26)");

export namespace bioseq::storage {

namespace internal_catalog_detail {

[[nodiscard]] inline bool bioseq_extension_installed() {
    return OidIsValid(get_extension_oid("bioseq", true));
}

[[nodiscard]] inline bool absence_is_benign(
        const bool allow_missing_during_extension_teardown) {
    return allow_missing_during_extension_teardown;
}

[[noreturn]] inline void missing_outside_teardown(
        const std::string_view object_name) {
    const std::string owned{object_name};
    bioseq::pg_utils::invoke_pg_as_cpp([&owned] {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq internal catalog %s is missing outside extension teardown",
                    owned.c_str())));
    });
    pg_unreachable();
}

} // namespace internal_catalog_detail

inline constexpr std::int16_t kSequenceSourceStateUnpublished = 0;
inline constexpr std::int16_t kSequenceSourceStateReady = 1;
inline constexpr std::int16_t kSequenceSourceStateInvalid = 2;

[[noreturn]] inline void storage_error_as_cpp(
        int sqlstate, std::string message) {
    bioseq::pg_utils::invoke_pg_as_cpp([sqlstate, &message] {
        ereport(ERROR,
            (errcode(sqlstate), errmsg("%s", message.c_str())));
    });
    pg_unreachable();
}

/// The source epoch state machine is deliberately one-way. In particular, a
/// source invalidated by parent-row or fragment mutation can never be
/// resurrected by manufacturing another publication token.
[[nodiscard]] constexpr bool sequence_source_state_transition_allowed(
        const std::int16_t current,
        const std::int16_t requested) noexcept {
    return (current == kSequenceSourceStateUnpublished
            && (requested == kSequenceSourceStateReady
                || requested == kSequenceSourceStateInvalid))
        || (current == kSequenceSourceStateReady
            && requested == kSequenceSourceStateInvalid);
}

static_assert(sequence_source_state_transition_allowed(
    kSequenceSourceStateUnpublished, kSequenceSourceStateReady));
static_assert(sequence_source_state_transition_allowed(
    kSequenceSourceStateReady, kSequenceSourceStateInvalid));
static_assert(!sequence_source_state_transition_allowed(
    kSequenceSourceStateInvalid, kSequenceSourceStateReady));

/// Exact PostgreSQL-owned identity record for one immutable sequence source.
/// The checksum is the CRC-64/ECMA-182 of decoded logical symbols, calculated
/// while the source is already streaming into fragment storage.
struct SequenceSourceVersionRow {
    std::uint64_t source_epoch = 0;
    Oid user_relid = InvalidOid;
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::uint64_t main_storage_id = 0;
    std::int16_t column_attno = 0;
    seq_locator locator{};
    Oid fragment_relid = InvalidOid;
    std::uint64_t fragment_storage_id = 0;
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t padding_letters = 0;
    std::uint64_t source_content_checksum = 0;
    std::int16_t state = kSequenceSourceStateUnpublished;
};

/// Optional observational counters for one immutable source-catalog lookup.
/// The lookup owns the only truthful view of its relation/index open sites;
/// callers that do not request telemetry retain the exact previous path.
struct SequenceSourceLookupTelemetryPg {
    std::uint64_t heap_opens = 0;
    std::uint64_t index_opens = 0;
    std::uint64_t schema_validations = 0;
    std::uint64_t lock_acquires = 0;
    std::uint64_t lock_releases = 0;
};

namespace source_version_detail {

enum Attribute : AttrNumber {
    source_epoch = 1,
    user_relid,
    database_oid,
    tablespace_oid,
    main_relfilenode,
    main_storage_id,
    column_attno,
    locator_version,
    bucket_idx,
    shard_idx,
    fragment_relid,
    fragment_storage_id,
    first_sub_row_no,
    num_fragments,
    total_bp_size,
    alphabet_code,
    storage_unit_byte_width,
    padding_letters,
    source_content_checksum,
    state,
    attribute_count = state,
};

struct AttributeContract {
    AttrNumber attno = InvalidAttrNumber;
    std::string_view name{};
    Oid type_oid = InvalidOid;
};

inline constexpr std::array<AttributeContract, attribute_count>
kAttributeContracts{{
    {source_epoch, "source_epoch", INT8OID},
    {user_relid, "user_relid", OIDOID},
    {database_oid, "database_oid", OIDOID},
    {tablespace_oid, "tablespace_oid", OIDOID},
    {main_relfilenode, "main_relfilenode", OIDOID},
    {main_storage_id, "main_storage_id", INT8OID},
    {column_attno, "column_attno", INT2OID},
    {locator_version, "locator_version", INT2OID},
    {bucket_idx, "bucket_idx", INT2OID},
    {shard_idx, "shard_idx", INT4OID},
    {fragment_relid, "fragment_relid", OIDOID},
    {fragment_storage_id, "fragment_storage_id", INT8OID},
    {first_sub_row_no, "first_sub_row_no", INT8OID},
    {num_fragments, "num_fragments", INT8OID},
    {total_bp_size, "total_bp_size", INT8OID},
    {alphabet_code, "alphabet_code", INT2OID},
    {storage_unit_byte_width, "storage_unit_byte_width", INT2OID},
    {padding_letters, "padding_letters", INT4OID},
    {source_content_checksum, "source_content_checksum", INT8OID},
    {state, "state", INT2OID},
}};

[[nodiscard]] inline Oid internal_namespace_oid() {
    return bioseq::pg_utils::invoke_pg_as_cpp(
        [] { return get_namespace_oid("bioseq_internal", true); });
}

[[nodiscard]] inline Oid relation_oid() {
    const Oid namespace_oid = internal_namespace_oid();
    return OidIsValid(namespace_oid)
        ? bioseq::pg_utils::invoke_pg_as_cpp([namespace_oid] {
            return get_relname_relid(
                "sequence_source_version", namespace_oid);
        })
        : InvalidOid;
}

[[nodiscard]] inline Oid primary_index_oid() {
    const Oid namespace_oid = internal_namespace_oid();
    return OidIsValid(namespace_oid)
        ? bioseq::pg_utils::invoke_pg_as_cpp([namespace_oid] {
            return get_relname_relid(
                "sequence_source_version_pkey", namespace_oid);
        })
        : InvalidOid;
}

[[noreturn]] inline void source_catalog_error(std::string_view message) {
    const std::string owned{message};
    bioseq::pg_utils::invoke_pg_as_cpp([&owned] {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq sequence source catalog: %s", owned.c_str())));
    });
    pg_unreachable();
}

/// Validate the exact extension-owned source catalog and its one-column
/// primary key before any fixed-ordinal tuple deformation. Besides making the
/// timed lookup genuinely index-backed, this prevents a changed tuple
/// descriptor from overflowing the fixed Datum/null arrays below.
inline void validate_catalog_schema(Relation relation) {
    if (relation == nullptr
        || RelationGetRelid(relation) != relation_oid()
        || RelationGetNamespace(relation) != internal_namespace_oid()
        || relation->rd_rel->relkind != RELKIND_RELATION
        || relation->rd_rel->relam != HEAP_TABLE_AM_OID) {
        source_catalog_error("catalog relation identity or heap access method changed");
    }
    TupleDesc descriptor = RelationGetDescr(relation);
    if (descriptor == nullptr
        || descriptor->natts != static_cast<int>(kAttributeContracts.size())) {
        source_catalog_error("catalog attribute count changed");
    }
    for (const AttributeContract& expected : kAttributeContracts) {
        const Form_pg_attribute attribute = TupleDescAttr(
            descriptor, expected.attno - 1);
        const bool expected_not_null = expected.attno != fragment_relid;
        if (attribute->attnum != expected.attno
            || attribute->attisdropped
            || attribute->attnotnull != expected_not_null
            || attribute->atttypid != expected.type_oid
            || std::string_view{NameStr(attribute->attname)} != expected.name) {
            source_catalog_error("catalog attribute contract changed");
        }
    }
}

[[nodiscard]] inline Oid validate_catalog_contract(
        Relation relation,
        SequenceSourceLookupTelemetryPg* telemetry = nullptr) {
    validate_catalog_schema(relation);
    if (telemetry != nullptr) {
        ++telemetry->schema_validations;
    }

    const Oid expected_primary = primary_index_oid();
    if (!OidIsValid(expected_primary)) {
        source_catalog_error("catalog primary index is missing");
    }
    List* indexes = bioseq::pg_utils::invoke_pg_as_cpp(
        [relation] { return RelationGetIndexList(relation); });
    Oid proven_primary = InvalidOid;
    std::size_t primary_count = 0;
    ListCell* cell = nullptr;
    foreach (cell, indexes) {
        const Oid index_oid = lfirst_oid(cell);
        Relation index = bioseq::pg_utils::invoke_pg_as_cpp(
            [index_oid] { return index_open(index_oid, AccessShareLock); });
        if (telemetry != nullptr) {
            ++telemetry->index_opens;
            ++telemetry->lock_acquires;
        }
        const bool is_primary = index->rd_index != nullptr
            && index->rd_index->indisprimary;
        if (is_primary) {
            ++primary_count;
            const bool exact = index_oid == expected_primary
                && index->rd_rel->relkind == RELKIND_INDEX
                && index->rd_rel->relam == BTREE_AM_OID
                && index->rd_index->indisunique
                && index->rd_index->indimmediate
                && index->rd_index->indisvalid
                && index->rd_index->indisready
                && index->rd_index->indislive
                && index->rd_index->indnkeyatts == 1
                && index->rd_index->indnatts == 1
                && index->rd_index->indkey.values[0] == source_epoch
                && bioseq::pg_utils::invoke_pg_as_cpp(
                    [index] { return RelationGetIndexExpressions(index); }) == NIL
                && bioseq::pg_utils::invoke_pg_as_cpp(
                    [index] { return RelationGetIndexPredicate(index); }) == NIL
                && bioseq::pg_utils::invoke_pg_as_cpp(
                    [index_oid] { return get_index_column_opclass(index_oid, 1); })
                    == INT8_BTREE_OPS_OID
                && index->rd_indcollation != nullptr
                && !OidIsValid(index->rd_indcollation[0])
                && index->rd_indoption != nullptr
                && index->rd_indoption[0] == 0
                && (index->rd_opcoptions == nullptr
                    || index->rd_opcoptions[0] == nullptr);
            if (exact) proven_primary = index_oid;
        }
        bioseq::pg_utils::invoke_pg_as_cpp(
            [index] { index_close(index, AccessShareLock); });
        if (telemetry != nullptr) {
            ++telemetry->lock_releases;
        }
    }
    bioseq::pg_utils::invoke_pg_as_cpp([indexes] { list_free(indexes); });
    if (primary_count != 1 || proven_primary != expected_primary) {
        source_catalog_error(
            "catalog primary index is not the exact live unique immediate btree contract");
    }
    return proven_primary;
}

/// Validate the exact source-version primary index that is already open for a
/// cold reader.  The same descriptor is used by the ordered scan, avoiding the
/// former validate-all-indexes followed by primary-index reopen sequence.
inline void validate_bound_read_catalog_contract(
        Relation relation,
        Relation primary,
        SequenceSourceLookupTelemetryPg* telemetry = nullptr) {
    validate_catalog_schema(relation);
    if (telemetry != nullptr) {
        ++telemetry->schema_validations;
    }
    const Oid expected_primary = primary_index_oid();
    const Form_pg_index index_form = primary == nullptr
        ? nullptr : primary->rd_index;
    const bool exact = OidIsValid(expected_primary)
        && primary != nullptr
        && RelationGetRelid(primary) == expected_primary
        && RelationGetPrimaryKeyIndex(relation)
            == expected_primary
        && index_form != nullptr
        && index_form->indrelid == RelationGetRelid(relation)
        && primary->rd_rel->relkind == RELKIND_INDEX
        && primary->rd_rel->relam == BTREE_AM_OID
        && index_form->indisprimary
        && index_form->indisunique
        && index_form->indimmediate
        && index_form->indisvalid
        && index_form->indisready
        && index_form->indislive
        && index_form->indnkeyatts == 1
        && index_form->indnatts == 1
        && index_form->indkey.values[0] == source_epoch
        && bioseq::pg_utils::invoke_pg_as_cpp(
            [primary] { return RelationGetIndexExpressions(primary); }) == NIL
        && bioseq::pg_utils::invoke_pg_as_cpp(
            [primary] { return RelationGetIndexPredicate(primary); }) == NIL
        && bioseq::pg_utils::invoke_pg_as_cpp([expected_primary] {
            return get_index_column_opclass(expected_primary, 1);
        }) == INT8_BTREE_OPS_OID
        && primary->rd_indcollation != nullptr
        && !OidIsValid(primary->rd_indcollation[0])
        && primary->rd_indoption != nullptr
        && primary->rd_indoption[0] == 0
        && (primary->rd_opcoptions == nullptr
            || primary->rd_opcoptions[0] == nullptr);
    if (!exact) {
        source_catalog_error(
            "catalog primary index is not the exact live unique immediate btree contract");
    }
}

[[nodiscard]] inline std::uint64_t positive_bigint(Datum value) {
    const std::int64_t signed_value = DatumGetInt64(value);
    if (signed_value <= 0) {
        source_catalog_error("positive BIGINT field is invalid");
    }
    return static_cast<std::uint64_t>(signed_value);
}

[[nodiscard]] inline std::uint64_t bigint_bits(Datum value) noexcept {
    return std::bit_cast<std::uint64_t>(DatumGetInt64(value));
}

[[nodiscard]] inline SequenceSourceVersionRow deform(
        HeapTuple tuple, TupleDesc descriptor) {
    std::array<Datum, attribute_count> values{};
    std::array<bool, attribute_count> nulls{};
    bioseq::pg_utils::invoke_pg_as_cpp([&] {
        heap_deform_tuple(tuple, descriptor, values.data(), nulls.data());
    });
    for (std::size_t index = 0; index < nulls.size(); ++index) {
        if (nulls[index]
            && index != static_cast<std::size_t>(fragment_relid - 1)) {
            source_catalog_error("NOT NULL catalog row contains NULL");
        }
    }
    SequenceSourceVersionRow row{};
    row.source_epoch = positive_bigint(values[source_epoch - 1]);
    row.user_relid = DatumGetObjectId(values[user_relid - 1]);
    row.database_oid = DatumGetObjectId(values[database_oid - 1]);
    row.tablespace_oid = DatumGetObjectId(values[tablespace_oid - 1]);
    row.main_relfilenode = DatumGetObjectId(values[main_relfilenode - 1]);
    row.main_storage_id = positive_bigint(values[main_storage_id - 1]);
    row.column_attno = DatumGetInt16(values[column_attno - 1]);
    const std::int16_t locator_version_value =
        DatumGetInt16(values[locator_version - 1]);
    const std::int16_t bucket_value = DatumGetInt16(values[bucket_idx - 1]);
    if (locator_version_value != kLocatorVersion
        || bucket_value < 0
        || bucket_value > (std::numeric_limits<std::uint8_t>::max)()) {
        source_catalog_error("locator version or bucket is outside its wire domain");
    }
    row.locator.version = static_cast<std::uint8_t>(locator_version_value);
    row.locator.bucket_idx = static_cast<std::uint8_t>(bucket_value);
    const std::int32_t shard_value = DatumGetInt32(values[shard_idx - 1]);
    if (shard_value < 0) {
        source_catalog_error("locator shard is outside its wire domain");
    }
    row.locator.shard_idx = static_cast<std::uint32_t>(shard_value);
    row.fragment_relid = nulls[fragment_relid - 1]
        ? InvalidOid
        : DatumGetObjectId(values[fragment_relid - 1]);
    const std::int64_t fragment_storage =
        DatumGetInt64(values[fragment_storage_id - 1]);
    if (fragment_storage < 0) {
        source_catalog_error("fragment storage ID is negative");
    }
    row.fragment_storage_id = static_cast<std::uint64_t>(fragment_storage);
    const std::int64_t first_row = DatumGetInt64(values[first_sub_row_no - 1]);
    const std::int64_t fragments = DatumGetInt64(values[num_fragments - 1]);
    const std::int64_t total_bp = DatumGetInt64(values[total_bp_size - 1]);
    if (first_row < 0 || fragments < 0 || total_bp < 0
        || static_cast<std::uint64_t>(fragments)
            > (std::numeric_limits<std::uint32_t>::max)()) {
        source_catalog_error("locator geometry is outside its wire domain");
    }
    row.locator.first_sub_row_no = static_cast<std::uint64_t>(first_row);
    row.locator.num_fragments = static_cast<std::uint32_t>(fragments);
    row.locator.total_bp_size = static_cast<std::uint64_t>(total_bp);
    row.locator.source_epoch = row.source_epoch;
    const std::int16_t alphabet_value =
        DatumGetInt16(values[alphabet_code - 1]);
    const std::int16_t storage_width_value =
        DatumGetInt16(values[storage_unit_byte_width - 1]);
    if (alphabet_value < 0
        || alphabet_value > (std::numeric_limits<std::uint8_t>::max)()
        || storage_width_value <= 0 || storage_width_value > 0x0F) {
        source_catalog_error("source alphabet or storage width is outside its wire domain");
    }
    row.alphabet_code = static_cast<std::uint8_t>(alphabet_value);
    row.storage_unit_byte_width = static_cast<std::uint8_t>(storage_width_value);
    const std::int32_t padding = DatumGetInt32(values[padding_letters - 1]);
    if (padding < 0
        || static_cast<std::uint32_t>(padding)
            > (std::numeric_limits<std::uint16_t>::max)()) {
        source_catalog_error("source padding is outside its wire domain");
    }
    row.padding_letters = static_cast<std::uint16_t>(padding);
    row.source_content_checksum = bigint_bits(
        values[source_content_checksum - 1]);
    row.state = DatumGetInt16(values[state - 1]);
    if (row.state != kSequenceSourceStateUnpublished
        && row.state != kSequenceSourceStateReady
        && row.state != kSequenceSourceStateInvalid) {
        source_catalog_error("source state is outside its state-machine domain");
    }
    return row;
}

[[nodiscard]] inline HeapTuple form(
        TupleDesc descriptor, const SequenceSourceVersionRow& row) {
    std::array<Datum, attribute_count> values{};
    std::array<bool, attribute_count> nulls{};
    values[source_epoch - 1] = Int64GetDatum(
        static_cast<std::int64_t>(row.source_epoch));
    values[user_relid - 1] = ObjectIdGetDatum(row.user_relid);
    values[database_oid - 1] = ObjectIdGetDatum(row.database_oid);
    values[tablespace_oid - 1] = ObjectIdGetDatum(row.tablespace_oid);
    values[main_relfilenode - 1] = ObjectIdGetDatum(row.main_relfilenode);
    values[main_storage_id - 1] = Int64GetDatum(
        static_cast<std::int64_t>(row.main_storage_id));
    values[column_attno - 1] = Int16GetDatum(row.column_attno);
    values[locator_version - 1] = Int16GetDatum(row.locator.version);
    values[bucket_idx - 1] = Int16GetDatum(row.locator.bucket_idx);
    values[shard_idx - 1] = Int32GetDatum(
        static_cast<std::int32_t>(row.locator.shard_idx));
    if (OidIsValid(row.fragment_relid)) {
        values[fragment_relid - 1] = ObjectIdGetDatum(row.fragment_relid);
    } else {
        nulls[fragment_relid - 1] = true;
    }
    values[fragment_storage_id - 1] = Int64GetDatum(
        static_cast<std::int64_t>(row.fragment_storage_id));
    values[first_sub_row_no - 1] = Int64GetDatum(
        static_cast<std::int64_t>(row.locator.first_sub_row_no));
    values[num_fragments - 1] = Int64GetDatum(row.locator.num_fragments);
    values[total_bp_size - 1] = Int64GetDatum(
        static_cast<std::int64_t>(row.locator.total_bp_size));
    values[alphabet_code - 1] = Int16GetDatum(row.alphabet_code);
    values[storage_unit_byte_width - 1] = Int16GetDatum(
        row.storage_unit_byte_width);
    values[padding_letters - 1] = Int32GetDatum(row.padding_letters);
    values[source_content_checksum - 1] = Int64GetDatum(
        std::bit_cast<std::int64_t>(row.source_content_checksum));
    values[state - 1] = Int16GetDatum(row.state);
    return bioseq::pg_utils::invoke_pg_as_cpp([&] {
        return heap_form_tuple(descriptor, values.data(), nulls.data());
    });
}

} // namespace source_version_detail

// DOC0383 P5 — the descriptor catalog deliberately permits nullable payload
// locator attributes and owns two partial indexes, so the legacy eight-column
// sidecar validator cannot describe it.  Keep this cold schema authority next
// to the existing PostgreSQL-owned sequence-source catalog contracts; mapped
// image/lifecycle code consumes the exported entry point without importing a
// second tuple-shape authority.
namespace minimizer_generation_catalog_detail {

enum class DefaultKind : std::uint8_t {
    none,
    zero_int2,
    zero_int4,
    zero_int8,
    empty_text,
};

struct AttributeContract {
    AttrNumber attno = InvalidAttrNumber;
    std::string_view name{};
    Oid type_oid = InvalidOid;
    bool not_null = false;
    DefaultKind default_kind = DefaultKind::none;
};

inline constexpr std::array<AttributeContract, 18>
kStorageAttributeContracts{{
    {1, "database_oid", OIDOID, true},
    {2, "tablespace_oid", OIDOID, true},
    {3, "main_relfilenode", OIDOID, true},
    {4, "user_relid", OIDOID, true},
    {5, "column_attno", INT2OID, true},
    {6, "index_name", NAMEOID, true},
    {7, "query_key", INT8OID, true},
    {8, "generation_id", INT8OID, true},
    {9, "provider", INT2OID, true},
    {10, "payload_relation_oid", OIDOID, false},
    {11, "payload_database_oid", OIDOID, false},
    {12, "payload_tablespace_oid", OIDOID, false},
    {13, "payload_relfilenode", OIDOID, false},
    {14, "payload_relation_name", NAMEOID, false},
    {15, "image_bytes", INT8OID, true},
    {16, "header_checksum", INT8OID, true},
    {17, "retire_full_xid", XID8OID, false},
    {18, "state", INT2OID, true},
}};

inline constexpr std::array<AttributeContract, 5>
kReaperAttributeContracts{{
    {1, "database_oid", OIDOID, true},
    {2, "tablespace_root_ordinal", INT4OID, true,
        DefaultKind::zero_int4},
    {3, "shard_ordinal", INT2OID, true, DefaultKind::zero_int2},
    {4, "last_canonical_filename", TEXTOID, true,
        DefaultKind::empty_text},
    {5, "pass", INT8OID, true, DefaultKind::zero_int8},
}};

inline constexpr std::array<std::string_view, 13>
kStorageCheckExpressions{{
    "database_oid <> 0",
    "tablespace_oid <> 0",
    "main_relfilenode <> 0",
    "user_relid <> 0",
    "column_attno > 0",
    "index_name = 'minimizers_per_fragment'",
    "query_key >= 0",
    "generation_id > 0",
    "provider IN (0, 1)",
    "image_bytes >= 4096",
    "state IN (1, 2)",
    "((provider = 0 "
        "AND payload_relation_oid IS NOT NULL "
        "AND payload_database_oid IS NOT NULL "
        "AND payload_tablespace_oid IS NOT NULL "
        "AND payload_relfilenode IS NOT NULL "
        "AND payload_relation_name IS NOT NULL "
        "AND payload_relation_oid <> 0 "
        "AND payload_database_oid = database_oid "
        "AND payload_tablespace_oid <> 0 "
        "AND payload_relfilenode <> 0) "
      "OR (provider = 1 "
        "AND payload_relation_oid IS NULL "
        "AND payload_database_oid IS NULL "
        "AND payload_tablespace_oid IS NULL "
        "AND payload_relfilenode IS NULL "
        "AND payload_relation_name IS NULL))",
    "((state = 1 AND retire_full_xid IS NULL) "
      "OR (state = 2 AND retire_full_xid IS NOT NULL))",
}};

inline constexpr std::array<std::string_view, 4>
kReaperCheckExpressions{{
    "database_oid <> 0",
    "tablespace_root_ordinal >= 0",
    "shard_ordinal BETWEEN 0 AND 255",
    "pass >= 0",
}};

[[noreturn]] inline void contract_error(
        const std::string_view catalog,
        const std::string_view detail) {
    const std::string owned_catalog{catalog};
    const std::string owned_detail{detail};
    bioseq::pg_utils::invoke_pg_as_cpp(
        [&owned_catalog, &owned_detail] {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq minimizer generation catalog contract changed"),
                 errdetail("catalog=%s invariant=%s",
                    owned_catalog.c_str(), owned_detail.c_str())));
        });
    pg_unreachable();
}

[[nodiscard]] inline bool exact_default(
        Relation relation,
        const AttributeContract& expected) {
    Node* node = bioseq::pg_utils::invoke_pg_as_cpp(
        [relation, &expected] {
            return build_column_default(relation, expected.attno);
        });
    if (expected.default_kind == DefaultKind::none) {
        return node == nullptr;
    }
    if (node == nullptr || !IsA(node, Const)) return false;
    const auto* constant = castNode(Const, node);
    if (constant->constisnull) return false;
    switch (expected.default_kind) {
        case DefaultKind::none:
            return false;
        case DefaultKind::zero_int2:
            return constant->consttype == INT2OID
                && DatumGetInt16(constant->constvalue) == 0;
        case DefaultKind::zero_int4:
            return constant->consttype == INT4OID
                && DatumGetInt32(constant->constvalue) == 0;
        case DefaultKind::zero_int8:
            return constant->consttype == INT8OID
                && DatumGetInt64(constant->constvalue) == 0;
        case DefaultKind::empty_text: {
            if (constant->consttype != TEXTOID) return false;
            const text* value = DatumGetTextPP(constant->constvalue);
            return VARSIZE_ANY_EXHDR(value) == 0;
        }
    }
    return false;
}

template <std::size_t AttributeCount>
inline void validate_relation_shape(
        Relation relation,
        const Oid expected_relation_oid,
        const std::string_view expected_name,
        const std::array<AttributeContract, AttributeCount>& attributes,
        const std::uint16_t expected_check_count) {
    if (relation == nullptr
        || RelationGetRelid(relation) != expected_relation_oid
        || RelationGetNamespace(relation)
            != bioseq::pg_utils::invoke_pg_as_cpp(
                [] { return get_namespace_oid("bioseq_internal", true); })
        || relation->rd_rel == nullptr
        || relation->rd_rel->relkind != RELKIND_RELATION
        || relation->rd_rel->relam != HEAP_TABLE_AM_OID
        || relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
        contract_error(expected_name,
            "relation identity, UNLOGGED persistence, or heap AM");
    }
    TupleDesc descriptor = RelationGetDescr(relation);
    if (descriptor == nullptr
        || descriptor->natts != static_cast<int>(attributes.size())) {
        contract_error(expected_name, "attribute count");
    }
    std::uint16_t observed_defaults = 0;
    for (const AttributeContract& expected : attributes) {
        const Form_pg_attribute attribute = TupleDescAttr(
            descriptor, expected.attno - 1);
        const bool expects_default =
            expected.default_kind != DefaultKind::none;
        if (attribute->attnum != expected.attno
            || attribute->attisdropped
            || attribute->attinhcount != 0
            || !attribute->attislocal
            || attribute->attnotnull != expected.not_null
            || attribute->atttypid != expected.type_oid
            || attribute->atttypmod != -1
            || attribute->attcollation
                != bioseq::pg_utils::invoke_pg_as_cpp(
                    [&expected] {
                        return get_typcollation(expected.type_oid);
                    })
            || attribute->atthasdef != expects_default
            || attribute->atthasmissing
            || attribute->attidentity != '\0'
            || attribute->attgenerated != '\0'
            || std::string_view{NameStr(attribute->attname)}
                != expected.name
            || !exact_default(relation, expected)) {
            contract_error(expected_name,
                "attribute ordinal/name/type/typmod/nullability/default");
        }
        if (expects_default) ++observed_defaults;
    }
    const TupleConstr* constraints = descriptor->constr;
    if (constraints == nullptr
        || !constraints->has_not_null
        || constraints->has_generated_stored
        || constraints->num_defval != observed_defaults
        || constraints->num_check != expected_check_count
        || (expected_check_count != 0 && constraints->check == nullptr)) {
        contract_error(expected_name,
            "NOT NULL/default/CHECK constraint counts");
    }
    for (std::uint16_t index = 0;
         index < constraints->num_check; ++index) {
        const ConstrCheck& check = constraints->check[index];
        if (check.ccname == nullptr || check.ccname[0] == '\0'
            || check.ccbin == nullptr || check.ccbin[0] == '\0'
            || !check.ccvalid || check.ccnoinherit) {
            contract_error(expected_name,
                "CHECK constraint identity, validation, or inheritance");
        }
    }
}

[[nodiscard]] inline Node* analyze_expected_check_expression(
        const std::string_view relation_name,
        const std::string_view expression) {
    const std::string sql = fmt::format(
        "SELECT ({}) FROM ONLY bioseq_internal.{};",
        expression, relation_name);
    return bioseq::pg_utils::invoke_pg_as_cpp([&sql] {
        List* raw_statements = raw_parser(sql.c_str(), RAW_PARSE_DEFAULT);
        if (list_length(raw_statements) != 1) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq generation schema expectation did not parse as one statement")));
        }
        auto* raw = castNode(RawStmt, linitial(raw_statements));
        Query* query = parse_analyze_fixedparams(
            raw, sql.c_str(), nullptr, 0, nullptr);
        if (query == nullptr
            || query->commandType != CMD_SELECT
            || list_length(query->targetList) != 1) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq generation schema expectation did not analyze as one expression")));
        }
        auto* target = castNode(TargetEntry, linitial(query->targetList));
        return reinterpret_cast<Node*>(target->expr);
    });
}

template <std::size_t CheckCount>
inline void validate_check_expressions(
        Relation relation,
        const std::string_view relation_name,
        const std::array<std::string_view, CheckCount>& expected_checks) {
    const TupleConstr* constraints = RelationGetDescr(relation)->constr;
    if (constraints == nullptr
        || constraints->num_check != expected_checks.size()) {
        contract_error(relation_name, "exact CHECK expression count");
    }
    std::array<Node*, CheckCount> expected_nodes{};
    std::array<bool, CheckCount> matched{};
    for (std::size_t expected_index = 0;
         expected_index < expected_checks.size(); ++expected_index) {
        expected_nodes[expected_index] = analyze_expected_check_expression(
            relation_name, expected_checks[expected_index]);
    }
    for (std::uint16_t actual_index = 0;
         actual_index < constraints->num_check; ++actual_index) {
        const ConstrCheck& check = constraints->check[actual_index];
        Node* actual = static_cast<Node*>(stringToNode(check.ccbin));
        bool found = false;
        for (std::size_t expected_index = 0;
             expected_index < expected_nodes.size(); ++expected_index) {
            if (!matched[expected_index]
                && equal(actual, expected_nodes[expected_index])) {
                matched[expected_index] = true;
                found = true;
                break;
            }
        }
        if (!found) {
            contract_error(relation_name,
                "CHECK expression differs from the frozen contract");
        }
    }
    if (std::find(matched.begin(), matched.end(), false) != matched.end()) {
        contract_error(relation_name,
            "required CHECK expression is missing");
    }
}

[[nodiscard]] inline bool exact_state_predicate(
        Relation index,
        const std::int16_t expected_state) {
    List* predicate = bioseq::pg_utils::invoke_pg_as_cpp(
        [index] { return RelationGetIndexPredicate(index); });
    if (predicate == NIL || list_length(predicate) != 1) return false;
    const Node* node = static_cast<const Node*>(linitial(predicate));
    if (node == nullptr || !IsA(node, OpExpr)) return false;
    const auto* equality = castNode(OpExpr, node);
    if (equality->opresulttype != BOOLOID
        || list_length(equality->args) != 2
        || bioseq::pg_utils::invoke_pg_as_cpp(
            [equality] { return get_opcode(equality->opno); }) != F_INT2EQ) {
        return false;
    }
    const Node* left = static_cast<const Node*>(linitial(equality->args));
    const Node* right = static_cast<const Node*>(lsecond(equality->args));
    if (left == nullptr || right == nullptr
        || !IsA(left, Var) || !IsA(right, Const)) return false;
    const auto* variable = castNode(Var, left);
    const auto* constant = castNode(Const, right);
    return variable->varattno == 18
        && variable->vartype == INT2OID
        && variable->varlevelsup == 0
        && constant->consttype == INT2OID
        && !constant->constisnull
        && DatumGetInt16(constant->constvalue) == expected_state;
}

template <std::size_t KeyCount>
[[nodiscard]] inline bool exact_index(
        Relation index,
        const Oid expected_table_oid,
        const std::string_view expected_name,
        const bool expected_primary,
        const bool expected_unique,
        const std::array<AttrNumber, KeyCount>& keys,
        const std::array<Oid, KeyCount>& key_types,
        const std::optional<std::int16_t> expected_state) {
    const Form_pg_index shape = index == nullptr ? nullptr : index->rd_index;
    if (shape == nullptr
        || RelationGetRelid(index) == InvalidOid
        || std::string_view{RelationGetRelationName(index)} != expected_name
        || shape->indrelid != expected_table_oid
        || index->rd_rel == nullptr
        || index->rd_rel->relkind != RELKIND_INDEX
        || index->rd_rel->relam != BTREE_AM_OID
        || index->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED
        || shape->indisprimary != expected_primary
        || shape->indisunique != expected_unique
        || !shape->indimmediate
        || !shape->indisvalid
        || !shape->indisready
        || !shape->indislive
        || shape->indisexclusion
        || shape->indisclustered
        || shape->indisreplident
        || shape->indnullsnotdistinct
        || shape->indnkeyatts != static_cast<int>(keys.size())
        || shape->indnatts != static_cast<int>(keys.size())
        || bioseq::pg_utils::invoke_pg_as_cpp(
            [index] { return RelationGetIndexExpressions(index); }) != NIL) {
        return false;
    }
    const List* predicate = bioseq::pg_utils::invoke_pg_as_cpp(
        [index] { return RelationGetIndexPredicate(index); });
    if ((!expected_state.has_value() && predicate != NIL)
        || (expected_state.has_value()
            && !exact_state_predicate(index, *expected_state))) {
        return false;
    }
    for (std::size_t key = 0; key < keys.size(); ++key) {
        const Oid expected_opclass =
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&key_types, key] {
                    return GetDefaultOpClass(
                        key_types[key], BTREE_AM_OID);
                });
        if (!OidIsValid(expected_opclass)
            || shape->indkey.values[key] != keys[key]
            || get_index_column_opclass(
                RelationGetRelid(index), static_cast<int>(key + 1))
                != expected_opclass
            || index->rd_indcollation == nullptr
            || index->rd_indcollation[key]
                != get_typcollation(key_types[key])
            || index->rd_indoption == nullptr
            || index->rd_indoption[key] != 0
            || (index->rd_opcoptions != nullptr
                && index->rd_opcoptions[key] != nullptr)) {
            return false;
        }
    }
    return true;
}

inline void validate_storage_indexes(Relation storage) {
    static constexpr std::array<AttrNumber, 8> primary_keys{
        1, 2, 3, 4, 5, 6, 7, 8};
    static constexpr std::array<Oid, 8> primary_types{
        OIDOID, OIDOID, OIDOID, OIDOID, INT2OID, NAMEOID, INT8OID, INT8OID};
    static constexpr std::array<AttrNumber, 7> live_keys{
        1, 2, 3, 4, 5, 6, 7};
    static constexpr std::array<Oid, 7> live_types{
        OIDOID, OIDOID, OIDOID, OIDOID, INT2OID, NAMEOID, INT8OID};
    static constexpr std::array<AttrNumber, 9> retired_keys{
        17, 1, 2, 3, 4, 5, 6, 7, 8};
    static constexpr std::array<Oid, 9> retired_types{
        XID8OID, OIDOID, OIDOID, OIDOID, OIDOID,
        INT2OID, NAMEOID, INT8OID, INT8OID};

    List* indexes = bioseq::pg_utils::invoke_pg_as_cpp(
        [storage] { return RelationGetIndexList(storage); });
    std::size_t exact_count = 0;
    ListCell* cell = nullptr;
    foreach (cell, indexes) {
        const Oid index_oid = lfirst_oid(cell);
        Relation index = bioseq::pg_utils::invoke_pg_as_cpp(
            [index_oid] { return index_open(index_oid, AccessShareLock); });
        const std::string_view name{RelationGetRelationName(index)};
        const bool exact =
            (name == "minimizer_generation_storage_pkey"
             && exact_index(index, RelationGetRelid(storage), name,
                true, true, primary_keys, primary_types, std::nullopt))
            || (name == "minimizer_generation_storage_one_live"
                && exact_index(index, RelationGetRelid(storage), name,
                    false, true, live_keys, live_types,
                    std::optional<std::int16_t>{1}))
            || (name == "minimizer_generation_storage_retired"
                && exact_index(index, RelationGetRelid(storage), name,
                    false, false, retired_keys, retired_types,
                    std::optional<std::int16_t>{2}));
        bioseq::pg_utils::invoke_pg_as_cpp(
            [index] { index_close(index, AccessShareLock); });
        if (!exact) {
            contract_error("minimizer_generation_storage",
                "primary/one-LIVE/retired index contract");
        }
        ++exact_count;
    }
    bioseq::pg_utils::invoke_pg_as_cpp([indexes] { list_free(indexes); });
    if (exact_count != 3) {
        contract_error("minimizer_generation_storage",
            "exactly three authoritative indexes");
    }
}

inline void validate_reaper_indexes(Relation reaper) {
    static constexpr std::array<AttrNumber, 1> primary_keys{1};
    static constexpr std::array<Oid, 1> primary_types{OIDOID};
    List* indexes = bioseq::pg_utils::invoke_pg_as_cpp(
        [reaper] { return RelationGetIndexList(reaper); });
    if (list_length(indexes) != 1) {
        contract_error("minimizer_generation_reaper_state",
            "exactly one primary index");
    }
    const Oid index_oid = lfirst_oid(list_head(indexes));
    Relation index = bioseq::pg_utils::invoke_pg_as_cpp(
        [index_oid] { return index_open(index_oid, AccessShareLock); });
    const bool exact = exact_index(
        index, RelationGetRelid(reaper),
        "minimizer_generation_reaper_state_pkey",
        true, true, primary_keys, primary_types, std::nullopt);
    bioseq::pg_utils::invoke_pg_as_cpp(
        [index] { index_close(index, AccessShareLock); });
    bioseq::pg_utils::invoke_pg_as_cpp([indexes] { list_free(indexes); });
    if (!exact) {
        contract_error("minimizer_generation_reaper_state",
            "primary index order/opclass/validity");
    }
}

} // namespace minimizer_generation_catalog_detail

/// Fail closed when either installation-local DOC0383 authority drifts.  This
/// is intentionally a cold publication/open guard, not a per-anchor check.
inline void validate_minimizer_generation_storage_catalogs_pg() {
    using namespace minimizer_generation_catalog_detail;
    const Oid namespace_oid = bioseq::pg_utils::invoke_pg_as_cpp(
        [] { return get_namespace_oid("bioseq_internal", true); });
    if (!OidIsValid(namespace_oid)) {
        contract_error("bioseq_internal", "internal schema is missing");
    }
    const Oid storage_oid = bioseq::pg_utils::invoke_pg_as_cpp(
        [namespace_oid] {
            return get_relname_relid(
                "minimizer_generation_storage", namespace_oid);
        });
    const Oid reaper_oid = bioseq::pg_utils::invoke_pg_as_cpp(
        [namespace_oid] {
            return get_relname_relid(
                "minimizer_generation_reaper_state", namespace_oid);
        });
    if (!OidIsValid(storage_oid) || !OidIsValid(reaper_oid)) {
        contract_error("bioseq_internal",
            "generation descriptor or reaper catalog is missing");
    }
    Relation storage = bioseq::pg_utils::invoke_pg_as_cpp(
        [storage_oid] {
            return relation_open(storage_oid, AccessShareLock);
        });
    Relation reaper = bioseq::pg_utils::invoke_pg_as_cpp(
        [reaper_oid] {
            return relation_open(reaper_oid, AccessShareLock);
        });
    validate_relation_shape(
        storage, storage_oid, "minimizer_generation_storage",
        kStorageAttributeContracts, 13);
    validate_check_expressions(
        storage, "minimizer_generation_storage",
        kStorageCheckExpressions);
    validate_storage_indexes(storage);
    validate_relation_shape(
        reaper, reaper_oid, "minimizer_generation_reaper_state",
        kReaperAttributeContracts, 4);
    validate_check_expressions(
        reaper, "minimizer_generation_reaper_state",
        kReaperCheckExpressions);
    validate_reaper_indexes(reaper);
    bioseq::pg_utils::invoke_pg_as_cpp(
        [reaper] { relation_close(reaper, AccessShareLock); });
    bioseq::pg_utils::invoke_pg_as_cpp(
        [storage] { relation_close(storage, AccessShareLock); });
}

class PendingSequenceSource;

/// PostgreSQL catalog operations for immutable source epochs. Every lookup
/// accepts the caller's snapshot; no backend-global or SnapshotSelf read is
/// allowed in a reader proof.
struct SequenceSourceVersionManager {
    [[nodiscard]] static std::uint64_t allocate_epoch() {
        const Oid namespace_oid =
            source_version_detail::internal_namespace_oid();
        const Oid sequence_oid = bioseq::pg_utils::invoke_pg_as_cpp(
            [namespace_oid] {
                return get_relname_relid(
                    "sequence_source_epoch_seq", namespace_oid);
            });
        if (!OidIsValid(sequence_oid)) {
            source_version_detail::source_catalog_error(
                "owned epoch sequence is missing");
        }
        Relation sequence_relation = bioseq::pg_utils::invoke_pg_as_cpp(
            [sequence_oid] {
                return relation_open(sequence_oid, AccessShareLock);
            });
        const bool shape_ok =
            RelationGetNamespace(sequence_relation) == namespace_oid
            && sequence_relation->rd_rel->relkind == RELKIND_SEQUENCE
            && sequence_relation->rd_rel->relpersistence
                == RELPERSISTENCE_PERMANENT;
        bioseq::pg_utils::invoke_pg_as_cpp([sequence_relation] {
            relation_close(sequence_relation, AccessShareLock);
        });
        if (!shape_ok) {
            source_version_detail::source_catalog_error(
                "owned epoch sequence relation contract changed");
        }
        HeapTuple sequence_tuple = bioseq::pg_utils::invoke_pg_as_cpp(
            [sequence_oid] {
                return SearchSysCache1(
                    SEQRELID, ObjectIdGetDatum(sequence_oid));
            });
        if (!HeapTupleIsValid(sequence_tuple)) {
            source_version_detail::source_catalog_error(
                "owned epoch pg_sequence row is missing");
        }
        const Form_pg_sequence sequence =
            reinterpret_cast<Form_pg_sequence>(GETSTRUCT(sequence_tuple));
        const bool contract_ok = sequence->seqtypid == INT8OID
            && sequence->seqincrement == 1
            && sequence->seqmin == 1
            && sequence->seqmax
                == (std::numeric_limits<std::int64_t>::max)()
            && sequence->seqstart == 1
            && sequence->seqcache == 1
            && !sequence->seqcycle;
        bioseq::pg_utils::invoke_pg_as_cpp(
            [sequence_tuple] { ReleaseSysCache(sequence_tuple); });
        if (!contract_ok) {
            source_version_detail::source_catalog_error(
                "owned epoch sequence numeric contract changed");
        }
        const std::int64_t epoch = bioseq::pg_utils::invoke_pg_as_cpp(
            [sequence_oid] { return nextval_internal(sequence_oid, false); });
        if (epoch <= 0) {
            source_version_detail::source_catalog_error(
                "owned epoch sequence exhausted positive BIGINT");
        }
        return static_cast<std::uint64_t>(epoch);
    }

    static void insert_unpublished(const SequenceSourceVersionRow& input) {
        if (input.source_epoch == 0
            || input.source_epoch
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int64_t>::max)())
            || input.locator.source_epoch != input.source_epoch
            || input.locator.version != kLocatorVersion
            || input.locator._pad_2 != 0 || input.locator._pad_4 != 0
            || input.locator.shard_idx
                > static_cast<std::uint32_t>(
                    (std::numeric_limits<std::int32_t>::max)())
            || input.locator.first_sub_row_no
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int64_t>::max)())
            || input.locator.total_bp_size
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int64_t>::max)())
            || !OidIsValid(input.user_relid)
            || input.database_oid != MyDatabaseId
            || !OidIsValid(input.tablespace_oid)
            || !OidIsValid(input.main_relfilenode)
            || input.main_storage_id == 0
            || input.main_storage_id
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int64_t>::max)())
            || input.column_attno <= 0
            || input.storage_unit_byte_width == 0
            || input.storage_unit_byte_width > 0x0Fu
            || input.state != kSequenceSourceStateUnpublished
            || (input.locator.num_fragments == 0
                ? (OidIsValid(input.fragment_relid)
                    || input.fragment_storage_id != 0
                    || input.locator.first_sub_row_no != 0)
                : (!OidIsValid(input.fragment_relid)
                    || input.fragment_storage_id == 0
                    || input.fragment_storage_id
                        > static_cast<std::uint64_t>(
                            (std::numeric_limits<std::int64_t>::max)())))) {
            source_version_detail::source_catalog_error(
                "unpublished source input is invalid");
        }
        Relation relation = nullptr;
        HeapTuple tuple = nullptr;
        std::exception_ptr failure;
        try {
            const Oid relation_oid = source_version_detail::relation_oid();
            relation = bioseq::pg_utils::invoke_pg_as_cpp(
                [relation_oid] {
                    return table_open(relation_oid, RowExclusiveLock);
                });
            source_version_detail::validate_catalog_schema(relation);
            tuple = source_version_detail::form(
                RelationGetDescr(relation), input);
            bioseq::pg_utils::invoke_pg_as_cpp(
                [relation, tuple] { CatalogTupleInsert(relation, tuple); });
        } catch (...) {
            failure = std::current_exception();
        }
        const auto cleanup = [&failure](auto&& operation) {
            try {
                operation();
            } catch (...) {
                if (failure == nullptr) failure = std::current_exception();
            }
        };
        if (tuple != nullptr) {
            cleanup([tuple] {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [tuple] { heap_freetuple(tuple); });
            });
        }
        if (relation != nullptr) {
            cleanup([relation] {
                bioseq::pg_utils::invoke_pg_as_cpp([relation] {
                    table_close(relation, RowExclusiveLock);
                });
            });
        }
        if (failure != nullptr) std::rethrow_exception(failure);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [] { CommandCounterIncrement(); });
    }

    [[nodiscard]] static std::optional<SequenceSourceVersionRow> lookup(
            std::uint64_t epoch, Snapshot snapshot, bool require_ready = true,
            bool* exact_primary_index_scan = nullptr,
            SequenceSourceLookupTelemetryPg* telemetry = nullptr) {
        if (exact_primary_index_scan != nullptr) {
            *exact_primary_index_scan = false;
        }
        if (telemetry != nullptr) {
            *telemetry = {};
        }
        if (epoch == 0
            || epoch > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())
            || snapshot == nullptr) {
            return std::nullopt;
        }
        Relation relation = table_open(
            source_version_detail::relation_oid(), AccessShareLock);
        if (telemetry != nullptr) {
            ++telemetry->heap_opens;
            ++telemetry->lock_acquires;
        }
        const Oid primary_oid = source_version_detail::primary_index_oid();
        if (!OidIsValid(primary_oid)) {
            source_version_detail::source_catalog_error(
                "catalog primary index is missing");
        }
        Relation primary = index_open(primary_oid, AccessShareLock);
        if (telemetry != nullptr) {
            ++telemetry->index_opens;
            ++telemetry->lock_acquires;
        }
        source_version_detail::validate_bound_read_catalog_contract(
            relation, primary, telemetry);
        ScanKeyData key;
        ScanKeyInit(&key, source_version_detail::source_epoch,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(epoch)));
        SysScanDesc scan = systable_beginscan_ordered(
            relation, primary, snapshot, 1, &key);
        HeapTuple tuple = systable_getnext_ordered(
            scan, ForwardScanDirection);
        std::optional<SequenceSourceVersionRow> result;
        if (HeapTupleIsValid(tuple)) {
            SequenceSourceVersionRow row = source_version_detail::deform(
                tuple, RelationGetDescr(relation));
            if (!require_ready || row.state == kSequenceSourceStateReady) {
                result = row;
            }
        }
        systable_endscan_ordered(scan);
        index_close(primary, AccessShareLock);
        if (telemetry != nullptr) {
            ++telemetry->lock_releases;
        }
        table_close(relation, AccessShareLock);
        if (telemetry != nullptr) {
            ++telemetry->lock_releases;
        }
        if (exact_primary_index_scan != nullptr) {
            *exact_primary_index_scan = true;
        }
        return result;
    }

private:
    static void set_state(std::uint64_t epoch, std::int16_t requested_state) {
        if (epoch == 0
            || epoch > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())
            || (requested_state != kSequenceSourceStateReady
                && requested_state != kSequenceSourceStateInvalid)) {
            source_version_detail::source_catalog_error(
                "source state transition input is invalid");
        }
        Relation relation = nullptr;
        SysScanDesc scan = nullptr;
        HeapTuple new_tuple = nullptr;
        std::exception_ptr failure;
        try {
            const Oid relation_oid = source_version_detail::relation_oid();
            relation = bioseq::pg_utils::invoke_pg_as_cpp(
                [relation_oid] {
                    return table_open(relation_oid, RowExclusiveLock);
                });
            const Oid proven_primary =
                source_version_detail::validate_catalog_contract(relation);
            ScanKeyData key;
            ScanKeyInit(&key, source_version_detail::source_epoch,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(epoch)));
            scan = bioseq::pg_utils::invoke_pg_as_cpp(
                [relation, proven_primary, &key] {
                    return systable_beginscan(
                        relation, proven_primary, true,
                        SnapshotSelf, 1, &key);
                });
            HeapTuple old_tuple = bioseq::pg_utils::invoke_pg_as_cpp(
                [scan] { return systable_getnext(scan); });
            if (!HeapTupleIsValid(old_tuple)) {
                source_version_detail::source_catalog_error(
                    "source epoch row is missing during state transition");
            }
            const SequenceSourceVersionRow current =
                source_version_detail::deform(
                    old_tuple, RelationGetDescr(relation));
            if (!sequence_source_state_transition_allowed(
                    current.state, requested_state)) {
                source_version_detail::source_catalog_error(
                    "source state transition violates the terminal state machine");
            }
            std::array<Datum, source_version_detail::attribute_count> values{};
            std::array<bool, source_version_detail::attribute_count> nulls{};
            std::array<bool, source_version_detail::attribute_count> replaces{};
            values[source_version_detail::state - 1] =
                Int16GetDatum(requested_state);
            replaces[source_version_detail::state - 1] = true;
            new_tuple = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return heap_modify_tuple(
                    old_tuple, RelationGetDescr(relation), values.data(),
                    nulls.data(), replaces.data());
            });
            bioseq::pg_utils::invoke_pg_as_cpp(
                [relation, old_tuple, new_tuple] {
                    CatalogTupleUpdate(
                        relation, &old_tuple->t_self, new_tuple);
                });
        } catch (...) {
            failure = std::current_exception();
        }
        const auto cleanup = [&failure](auto&& operation) {
            try {
                operation();
            } catch (...) {
                if (failure == nullptr) failure = std::current_exception();
            }
        };
        if (new_tuple != nullptr) {
            cleanup([new_tuple] {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [new_tuple] { heap_freetuple(new_tuple); });
            });
        }
        if (scan != nullptr) {
            cleanup([scan] {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [scan] { systable_endscan(scan); });
            });
        }
        if (relation != nullptr) {
            cleanup([relation] {
                bioseq::pg_utils::invoke_pg_as_cpp([relation] {
                    table_close(relation, RowExclusiveLock);
                });
            });
        }
        if (failure != nullptr) std::rethrow_exception(failure);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [] { CommandCounterIncrement(); });
    }

    static void publish_ready(std::uint64_t epoch) {
        set_state(epoch, kSequenceSourceStateReady);
    }

    friend class PendingSequenceSource;

public:

    static void invalidate_epoch(std::uint64_t epoch) {
        if (epoch == 0) return;
        const auto row = lookup(epoch, SnapshotSelf, /*require_ready=*/false);
        if (!row || row->state == kSequenceSourceStateInvalid) return;
        set_state(epoch, kSequenceSourceStateInvalid);
    }

    static void delete_for_main(
            Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
            bool allow_missing_during_extension_teardown = false) {
        const Oid relation_oid = source_version_detail::relation_oid();
        if (!OidIsValid(relation_oid)) {
            if (internal_catalog_detail::absence_is_benign(
                    allow_missing_during_extension_teardown)) return;
            internal_catalog_detail::missing_outside_teardown(
                "bioseq_internal.sequence_source_version");
        }
        Relation relation = try_safe_relation_open(
            relation_oid, RowExclusiveLock);
        if (relation == nullptr) {
            if (internal_catalog_detail::absence_is_benign(
                    allow_missing_during_extension_teardown)) return;
            internal_catalog_detail::missing_outside_teardown(
                "bioseq_internal.sequence_source_version");
        }
        source_version_detail::validate_catalog_schema(relation);
        ScanKeyData keys[3];
        ScanKeyInit(&keys[0], source_version_detail::database_oid,
            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(database_oid));
        ScanKeyInit(&keys[1], source_version_detail::tablespace_oid,
            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(tablespace_oid));
        ScanKeyInit(&keys[2], source_version_detail::main_relfilenode,
            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(main_relfilenode));
        SysScanDesc scan = systable_beginscan(
            relation, InvalidOid, false, SnapshotSelf, 3, keys);
        HeapTuple tuple = nullptr;
        while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
            CatalogTupleDelete(relation, &tuple->t_self);
        }
        systable_endscan(scan);
        table_close(relation, RowExclusiveLock);
        CommandCounterIncrement();
    }

    static void invalidate_for_fragment(
            Oid fragment_relation_oid,
            bool allow_missing_during_extension_teardown = false) {
        if (!OidIsValid(fragment_relation_oid)) return;
        const Oid catalog_oid = source_version_detail::relation_oid();
        if (!OidIsValid(catalog_oid)) {
            if (internal_catalog_detail::absence_is_benign(
                    allow_missing_during_extension_teardown)) return;
            internal_catalog_detail::missing_outside_teardown(
                "bioseq_internal.sequence_source_version");
        }
        Relation relation = try_safe_relation_open(
            catalog_oid, RowExclusiveLock);
        if (relation == nullptr) {
            if (internal_catalog_detail::absence_is_benign(
                    allow_missing_during_extension_teardown)) return;
            internal_catalog_detail::missing_outside_teardown(
                "bioseq_internal.sequence_source_version");
        }
        source_version_detail::validate_catalog_schema(relation);
        ScanKeyData key;
        ScanKeyInit(&key, source_version_detail::fragment_relid,
            BTEqualStrategyNumber, F_OIDEQ,
            ObjectIdGetDatum(fragment_relation_oid));
        // DROP/invalidation is not a timed path. A validated heap scan avoids
        // trusting a missing, renamed, partial, or otherwise corrupted
        // secondary index to decide whether READY epochs survive a drop.
        SysScanDesc scan = systable_beginscan(
            relation, InvalidOid, false,
            SnapshotSelf, 1, &key);
        HeapTuple tuple = nullptr;
        while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
            const SequenceSourceVersionRow current =
                source_version_detail::deform(tuple, RelationGetDescr(relation));
            if (current.state == kSequenceSourceStateInvalid) continue;
            std::array<Datum, source_version_detail::attribute_count> values{};
            std::array<bool, source_version_detail::attribute_count> nulls{};
            std::array<bool, source_version_detail::attribute_count> replaces{};
            values[source_version_detail::state - 1] =
                Int16GetDatum(kSequenceSourceStateInvalid);
            replaces[source_version_detail::state - 1] = true;
            HeapTuple updated = heap_modify_tuple(
                tuple, RelationGetDescr(relation), values.data(),
                nulls.data(), replaces.data());
            CatalogTupleUpdate(relation, &tuple->t_self, updated);
            heap_freetuple(updated);
        }
        systable_endscan(scan);
        table_close(relation, RowExclusiveLock);
        CommandCounterIncrement();
    }
};

template<class, std::size_t, std::size_t>
struct sequence_storage_manager;
struct sequence_stream_writer;

/// Move-only publication authority. Fragment/catalog creation returns this
/// token in UNPUBLISHED state; only the caller that has appended the parent
/// row may consume it to publish READY.
class PendingSequenceSource {
private:
    seq_locator locator_{};
    bool pending_ = false;

    explicit PendingSequenceSource(seq_locator locator) noexcept
        : locator_(locator), pending_(locator.source_epoch != 0) {}

    template<class, std::size_t, std::size_t>
    friend struct sequence_storage_manager;
    friend struct sequence_stream_writer;

public:
    PendingSequenceSource() = delete;
    PendingSequenceSource(const PendingSequenceSource&) = delete;
    PendingSequenceSource& operator=(const PendingSequenceSource&) = delete;
    PendingSequenceSource(PendingSequenceSource&& other) noexcept
        : locator_(other.locator_), pending_(std::exchange(other.pending_, false)) {}
    PendingSequenceSource& operator=(PendingSequenceSource&&) = delete;
    ~PendingSequenceSource() = default;

    [[nodiscard]] const seq_locator& locator() const noexcept { return locator_; }
    [[nodiscard]] bool pending() const noexcept { return pending_; }

    void publish_after_parent_row() {
        if (!pending_) {
            storage_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq sequence source catalog: publication token was reused");
        }
        SequenceSourceVersionManager::publish_ready(locator_.source_epoch);
        pending_ = false;
    }

    /// Consume a speculative-insert authority whose parent row was rejected.
    /// This is intentionally token-owned: the losing path cannot retain a raw
    /// epoch and later manufacture READY publication authority.
    void invalidate_without_parent_row() {
        if (!pending_) {
            storage_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq sequence source catalog: invalidation token was reused");
        }
        SequenceSourceVersionManager::invalidate_epoch(locator_.source_epoch);
        pending_ = false;
    }
};

static_assert(!std::is_move_assignable_v<PendingSequenceSource>,
              "A pending source authority must never be overwritten by move assignment.");

// ---------------------------------------------------------------------------
// DOC0371 — Phase C v3 locator stub format. A 56-byte pgseq_t-shaped varlena
// that lives in the main columnar relation in place of the full sequence
// payload. Replaces the Phase A 40-byte v1 stub (no on-disk back-compat —
// DOC0264 R0 / §1.1#10).
// ---------------------------------------------------------------------------

// DOC0260 §12.1 + DOC0264 §5 — `kLocatorVersion` and the `seq_locator` POD
// (with its 32-byte / offset static_asserts) live in the PG-free
// `bioseq_bucket_math` module so the unit-test TU can link them; they are
// re-exported above via `export import bioseq_bucket_math`, so every use of
// `seq_locator` / `kLocatorVersion` below resolves unchanged.

constexpr std::size_t kLocatorStubVarlenaBytes =
    pgseq_t::header_bytes() + sizeof(seq_locator);
static_assert(kLocatorStubVarlenaBytes == 56,
              "DOC0371: Phase C v3 locator stub must be exactly 56 bytes.");

[[nodiscard]] [[gnu::always_inline]]
inline bool is_locator_stub(const pgseq_t* hdr) noexcept {
    return hdr != nullptr
        && is_seq_locator_stub_width(
            static_cast<std::uint8_t>(hdr->storage_unit_byte_width));
}

[[nodiscard]] [[gnu::always_inline]]
inline std::uint8_t logical_storage_unit_byte_width(const pgseq_t* hdr) noexcept {
    return static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(hdr->storage_unit_byte_width) & 0x0Fu);
}

[[nodiscard]] [[gnu::always_inline]]
inline seq_locator decode_locator_from_stub(const pgseq_t* hdr) {
    if (!is_locator_stub(hdr)) {
        bioseq_log<ERR, PG>(
               "bioseq storage: decode_locator_from_stub called on a "
               "non-stub value (sub_w={}). Caller must guard with "
               "is_locator_stub(hdr) first.",
               static_cast<unsigned>(hdr->storage_unit_byte_width));
    }
    if (VARSIZE(hdr) != kLocatorStubVarlenaBytes) {
        bioseq_log<ERR, PG>(
               "bioseq storage: locator stub size mismatch (got {}, expected {})",
               static_cast<unsigned long long>(VARSIZE(hdr)),
               static_cast<unsigned long long>(kLocatorStubVarlenaBytes));
    }
    const auto payload = std::span<std::byte const>{
        reinterpret_cast<std::byte const*>(hdr->data), sizeof(seq_locator)};
    const auto decoded = decode_seq_locator_payload(payload);
    if (!decoded.has_value()) {
        bioseq_log<ERR, PG>(
               "bioseq storage: locator payload is not canonical v3 data");
    }
    return *decoded;
}

[[gnu::always_inline]]
inline void encode_locator_into_stub(
    pgseq_t* dst,
    std::uint8_t logical_alphabet_code,
    std::uint8_t logical_byte_width,
    std::uint16_t padding_letters,
    sequence_size_t total_bp_size,
    const seq_locator& loc)
{
    if (logical_byte_width == 0 || logical_byte_width > 0x0Fu
        || loc.version != kLocatorVersion || loc.source_epoch == 0
        || loc._pad_2 != 0 || loc._pad_4 != 0) {
        bioseq_log<ERR, PG>(
            "bioseq storage: locator stub input is outside the v3 wire domain");
    }
    std::memset(dst, 0, kLocatorStubVarlenaBytes);
    SET_VARSIZE(dst, kLocatorStubVarlenaBytes);
    dst->alphabet_code = static_cast<pgseq_t::alphabet_code_t>(logical_alphabet_code);
    dst->storage_unit_byte_width = static_cast<std::uint8_t>(
        k_seq_locator_stub_marker | logical_byte_width);
    dst->padding_letters = padding_letters;
    constexpr sequence_size_t kBpSizeFieldMax =
        static_cast<sequence_size_t>(std::numeric_limits<pgseq_t::bp_size_t>::max());
    dst->bp_size = static_cast<pgseq_t::bp_size_t>(
        std::min<sequence_size_t>(total_bp_size, kBpSizeFieldMax));
    const auto payload = std::span<std::byte>{
        reinterpret_cast<std::byte*>(dst->data), sizeof(seq_locator)};
    if (!encode_seq_locator_payload(payload, loc)) {
        bioseq_log<ERR, PG>(
            "bioseq storage: failed to encode canonical v3 locator payload");
    }
}

[[nodiscard]] [[gnu::always_inline]]
inline pgseq_t* alloc_locator_stub_in(
    MemoryContext mctx,
    std::uint8_t logical_alphabet_code,
    std::uint8_t logical_byte_width,
    std::uint16_t padding_letters,
    sequence_size_t total_bp_size,
    const seq_locator& loc)
{
    MemoryContextScope ctx_guard{mctx};
    pgseq_t* dst = static_cast<pgseq_t*>(palloc0(kLocatorStubVarlenaBytes));
    encode_locator_into_stub(dst, logical_alphabet_code, logical_byte_width,
                             padding_letters, total_bp_size, loc);
    return dst;
}

// ---------------------------------------------------------------------------
// §4 — Bucket math. The consteval threshold builder, len_log2_for_size, the
// branchless L->bucket LUT and bucket_index_for_size now live in the PG-free
// `bioseq_bucket_math` module (DOC0260 §12.1) and are re-exported above via
// `export import bioseq_bucket_math`. The template below delegates to them.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// §7 — Per-scan / per-batch relation cache. Owned by BioseqColumnScanDescData,
// IndexFetchBioseqColumnData, BioseqColumnWriteState and TupleDispatchBuffer.
// Always allocated separately (`palloc(sizeof(seq_caches)) + placement-new`)
// because std::map's non-standard-layout would otherwise spoil the surrounding
// POD struct's std::is_standard_layout_v guarantee.
// ---------------------------------------------------------------------------

struct seq_caches {
    struct entry {
        Relation rel = nullptr;
        BioseqColumnWriteState* write_state = nullptr;
        BioseqColumnReadState*  read_state  = nullptr;
        // Exact caller-owned snapshot used to create read_state. Registered
        // MVCC snapshots and permanent SnapshotAny may be retained; shorter-
        // lived special snapshots are served by a per-call reader instead.
        Snapshot read_snapshot_source = nullptr;
        sequence_size_t pending_bytes_delta = 0;
        sequence_size_t pending_total_payload_delta = 0;
    };

    struct entry_map {
        using value_type = std::pair<std::uint64_t, entry>;
        using storage_type = std::vector<value_type>;
        using iterator = storage_type::iterator;
        using const_iterator = storage_type::const_iterator;

        [[nodiscard]] iterator begin() noexcept { return entries_.begin(); }
        [[nodiscard]] iterator end() noexcept { return entries_.end(); }
        [[nodiscard]] const_iterator begin() const noexcept { return entries_.begin(); }
        [[nodiscard]] const_iterator end() const noexcept { return entries_.end(); }
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        void clear() noexcept { entries_.clear(); }

        [[nodiscard]] iterator find(std::uint64_t key) noexcept {
            auto it = lower_bound(key);
            return it != entries_.end() && it->first == key ? it : entries_.end();
        }

        [[nodiscard]] const_iterator find(std::uint64_t key) const noexcept {
            auto it = lower_bound(key);
            return it != entries_.end() && it->first == key ? it : entries_.end();
        }

        entry& operator[](std::uint64_t key) {
            auto it = lower_bound(key);
            if (it == entries_.end() || it->first != key) {
                it = entries_.emplace(it, key, entry{});
            }
            return it->second;
        }

    private:
        struct less_by_key {
            [[nodiscard]] constexpr bool operator()(const value_type& lhs,
                                                   std::uint64_t rhs) const noexcept {
                return lhs.first < rhs;
            }
        };

        [[nodiscard]] iterator lower_bound(std::uint64_t key) noexcept {
            return std::lower_bound(
                entries_.begin(), entries_.end(), key, less_by_key{});
        }

        [[nodiscard]] const_iterator lower_bound(std::uint64_t key) const noexcept {
            return std::lower_bound(
                entries_.begin(), entries_.end(), key, less_by_key{});
        }

        storage_type entries_{};
    };

    // DOC0260 R14 — sorted-vector map rather than std::map: no per-node
    // allocation, deterministic sorted iteration order for close_all, and
    // cache-friendly for the typical <50-entry working set. The hot paths only
    // do find / operator[] / clear, and never hold an `entry&` across an insert.
    entry_map entries{};
    LOCKMODE lock_mode = AccessShareLock;

    enum class Side { Read, Write };
    Side side = Side::Read;

    MemoryContext read_state_mctx = nullptr;

    // Optional explicit owner for read-side caches whose lifetime is longer
    // than the executor scan that first opened them.  Ordinary scan caches
    // leave this null and continue to use CurrentResourceOwner.  Locator
    // source domains set it to their transaction-owned child so a retained
    // expanded datum can finish reading after the provider scan has ended.
    ResourceOwner read_resource_owner = nullptr;

    sequence_size_t cap_bytes_snapshot = std::numeric_limits<sequence_size_t>::max();

    Oid main_database_oid   = InvalidOid;
    Oid main_tablespace_oid = InvalidOid;
    Oid main_relfilenode    = InvalidOid;

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

    void close_all(bool commit) noexcept;
};

// alloc_seq_caches / destroy_seq_caches own the placement-new dance so the
// dependent consumers (BioseqColumnScanDescData, BioseqColumnWriteState, ...)
// never see a half-constructed map. The header type stays
// reinterpret_cast-safe via void* opaque pointers in the consumer structs.
//
// destroy_seq_caches is NOT inline because write_state_management.cppm
// forward-declares it to avoid `import bioseq_storage` (which would
// create a module cycle with bioseq_table). Out-of-line gives it external
// linkage so the linker resolves the forward declaration.
[[nodiscard]] inline seq_caches* alloc_seq_caches(MemoryContext mctx) {
    MemoryContextScope ctx_guard{mctx};
    void* raw = palloc0(sizeof(seq_caches));
    return ::new (raw) seq_caches{};
}

void destroy_seq_caches(seq_caches* caches) noexcept;

[[gnu::always_inline]] inline void close_seq_caches(seq_caches& caches,
                                                    bool commit) noexcept {
    caches.close_all(commit);
}

void close_and_destroy_seq_caches(seq_caches* caches, bool commit) noexcept;

/**
 * Opaque registry owned by one locator-producing table scan.  The registry
 * creates exact, epoch-bound source domains and owns the registry reference to
 * each domain; source leaves retain an independent reference through the
 * compact-sequence source ABI.
 */
struct pg_sequence_source_provider;

[[nodiscard]] pg_sequence_source_provider*
create_pg_sequence_source_provider(
    Relation owning_relation,
    Snapshot authoritative_registered_snapshot);

/**
 * Close the provider side of the registry.  Leaf-held domains remain usable
 * until their expanded values are flattened/released or the transaction ends.
 */
void close_pg_sequence_source_provider(
    pg_sequence_source_provider* provider,
    bool is_commit) noexcept;

/**
 * Promote one canonical locator stub from the provider's owning relation into
 * a read-only expanded bioseq.seq backed by a verified immutable source
 * domain.  No whole-sequence payload is read here.
 */
[[nodiscard]] Datum promote_pg_sequence_locator(
    pg_sequence_source_provider* provider,
    AttrNumber column_attno,
    Datum locator_datum,
    MemoryContext result_context,
    slice_trace_attachment* trace = nullptr);

// DOC0286 §4.3 — public alias for the per-fragment storage-unit count
// (detail::kFragmentStorageUnits below aliases this so the two never drift).
// `bioseq.load`'s streaming sink uses it to size fragment payloads.
inline constexpr std::size_t kSequenceFragmentStorageUnits = std::size_t{1024};

// DOC0286 §4.4 — flush every open sub-table write state owned by `caches` so an
// in-statement index scan (e.g. the batched update_index `bioseq.load` runs
// after streaming) sees the just-written fragment rows. Unlike `close_all`,
// this only flushes pending columnar writes (it does NOT pop/close the write
// state); the central xact-commit teardown still owns lifecycle.
void flush_seq_cache_write_states(seq_caches* caches,
                                  SubTransactionId current_sub_xid) noexcept;

/// DOC0286 §4.3 — bounded streaming fragment writer for one sequence. Accepts
/// column-native packed bytes, writes full fragment rows (each
/// `kSequenceFragmentStorageUnits` storage units) as its internal buffer fills,
/// retains only the final partial fragment until `finish`, and returns the
/// Phase-C `seq_locator`. Peak extra memory is one ≤8 KiB fragment buffer + one
/// `pgseq_t` fragment row, so a >MaxAllocSize record never materialises as one
/// buffer. Writes to ONE shard; a single sequence exceeding the sub-table cap
/// raises ERROR (transaction rollback cleans the partial rows).
struct sequence_stream_writer {
    void begin(Oid user_relid, Oid db_oid, Oid spc_oid,
               RelFileNumber main_relfilenode, std::uint64_t main_storage_id,
               AttrNumber seq_attno, TupleDesc parent_td,
               std::uint8_t alphabet_code, std::uint8_t byte_width,
               sequence_size_t cap_bytes, seq_caches* caches);
    void append_packed_bytes(std::span<const std::uint8_t> bytes,
                             sequence_size_t bp_count);
    [[nodiscard]] PendingSequenceSource finish(sequence_size_t total_bp,
                                               std::uint16_t last_pad);
    void abort();

private:
    void open_shard();
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
    BioseqColumnWriteState* write_state_ = nullptr;

    std::size_t         frag_payload_bytes_ = 0;  // kSequenceFragmentStorageUnits * byte_width
    std::size_t         frag_varlena_bytes_ = 0;  // header + payload
    sequence_size_t     frag_bp_ = 0;             // kSequenceFragmentStorageUnits * letters_per_unit
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

// DOC0269 — Phase A required exporting this block so importers (notably
// access_method.cppm) can name `sequence_storage_manager`,
// `bioseq_default_storage_manager`, the §7.10 sidecar I/O helpers, and
// `read_max_sub_table_cap_bytes`. Without the `export` keyword on this
// re-opening of `namespace bioseq::storage`, the declarations land in the
// module purview but are not exported, so the names are present in the
// translation unit yet refused at import (clang's "declaration must be
// imported from module 'bioseq_storage' before it is required").
export namespace bioseq::storage {

// ---------------------------------------------------------------------------
// §6 — Catalog access shims. These call into storage_metadata which owns the
// bioseq_internal.seq_sub_table catalog table.
// ---------------------------------------------------------------------------

struct SeqColumnTypmod {
    std::string alphabet_name;
    int storage_unit_bit_width = 0;
};

[[nodiscard]] SeqColumnTypmod
column_typmod_at(TupleDesc parent_tupdesc, std::int16_t column_attno);

[[nodiscard]] const char*
persistence_keyword_for_relfilenode(Oid main_database_oid,
                                    Oid main_tablespace_oid,
                                    Oid main_relfilenode);

[[nodiscard]] std::string
format_sub_table_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                      std::int16_t column_attno, std::int16_t bucket_idx,
                      std::int32_t shard_idx);

[[nodiscard]] std::string
format_sidecar_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                    std::int16_t column_attno, std::int16_t bucket_idx,
                    std::int32_t shard_idx);

// ---------------------------------------------------------------------------
// §7.1 — sequence_storage_manager template.
// ---------------------------------------------------------------------------

template<class p_len_t,
         std::size_t p_bucket_min_len_bits,
         std::size_t p_bucket_max_len_bits>
struct sequence_storage_manager {
    using len_t = p_len_t;
    static constexpr std::size_t bucket_min_len_bits = p_bucket_min_len_bits;
    static constexpr std::size_t bucket_max_len_bits = p_bucket_max_len_bits;
    static constexpr std::size_t total_len_bits      = sizeof(len_t) * 8u;
    static constexpr auto table_seq_buckets_len_thresholds =
        generate_seq_buckets_len_thresholds<
            len_t, bucket_min_len_bits, bucket_max_len_bits>();
    static constexpr std::size_t bucket_count =
        table_seq_buckets_len_thresholds.size();

    static_assert(bucket_count <= std::numeric_limits<std::uint8_t>::max(),
                  "seq_locator::bucket_idx is uint8_t; raise the locator "
                  "format version before configuring more than 255 buckets");

    // DOC0260 §12.1 — delegate to the PG-free free template so production and
    // the unit-test TU resolve to identical bucketing code (the LUT itself is
    // bucket_lut_v in bioseq_bucket_math).
    [[nodiscard]] [[gnu::always_inline]] static constexpr std::size_t
    bucket_index_for_size(sequence_size_t seq_size) noexcept {
        return bioseq::storage::bucket_index_for_size<
            len_t, bucket_min_len_bits, bucket_max_len_bits>(seq_size);
    }

    // Shard lifecycle / write path.
    //
    // DOC0264 §8 — `insert_sequence_streamed` writes one fragment per
    // sub-table row from a per-fragment producer callback. The producer
    // receives a palloc'd `pgseq_t*` whose header is pre-filled with
    // alphabet/width, and fills `data[]` + `bp_size` + `padding_letters`
    // for fragment `n` (last fragment uses `is_last_fragment=true`). The
    // per-fragment MemoryContext is reset between fragments to bound peak
    // RSS. Returns an INVALID-to-READY publication token carrying the v3
    // locator and its PostgreSQL-owned source epoch.
    //
    // `insert_payload_as_fragments` is the convenience wrapper that the
    // existing single-varlena code paths (TAM rewrite-to-stubs, CLUSTER)
    // use: it slices an already-materialised `pgseq_t*` into fragments
    // via `insert_sequence_streamed`.
    template<class FragmentProducer>
    [[nodiscard]] static PendingSequenceSource
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
                             FragmentProducer&& producer);

    [[nodiscard]] static PendingSequenceSource
    insert_payload_as_fragments(Oid user_relid,
                                std::uint64_t main_storage_id,
                                Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                std::int16_t column_attno,
                                TupleDesc parent_tupdesc,
                                const pgseq_t* original_hdr,
                                sequence_size_t current_cap_bytes,
                                seq_caches* cache);

    // Read path: single sequence — stitches all fragments into a single
    // contiguous palloc'd `pgseq_t*` in `out_mctx`. Used by the legacy
    // hydration path; Phase C raises bioseq_log<ERR, PG>() if the stitched varlena
    // would exceed MaxAllocSize (DOC0264 §9.4). Streaming readers should
    // use `sequence_scanner_pg<S>` directly.
    [[nodiscard]] static pgseq_t*
    fetch_payload(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const seq_locator& loc,
                  Snapshot snapshot,
                  MemoryContext out_mctx, seq_caches* cache);

    // Read path: bulk per-bucket.
    struct fetch_request {
        std::size_t out_slot = 0;
        seq_locator locator{};
    };

    static void
    fetch_payload_bulk(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                       std::int16_t column_attno, std::int16_t bucket_idx,
                       std::span<const fetch_request> requests,
                       std::span<pgseq_t*>            out_payloads,
                       std::span<fetch_request>       sort_scratch,
                       Snapshot                       snapshot,
                       MemoryContext                  out_mctx,
                       seq_caches*                    cache);

    // Cascade on DROP TABLE / TRUNCATE / CLUSTER swap.
    // ProcessUtility calls this read-only preflight before PostgreSQL enters
    // relation deletion.  OAT_DROP repeats the same validation immediately
    // before child cleanup, closing the preflight-to-delete race.
    static void validate_drop_catalog_contracts();

    static void
    drop_sub_tables_for_main(
        Oid db_oid, Oid spc_oid, Oid main_relfilenode,
        bool allow_missing_during_extension_teardown = false);
};

using bioseq_default_storage_manager =
    sequence_storage_manager<sequence_size_t, /*min=*/8, /*max=*/42>;

// DOC0264 §7.9 — exported entry point so the Phase C `sequence_scanner_pg<S>`
// (in `bioseq_storage_scanner_pg`) can resolve a sub-table relation via the
// per-scan `seq_caches`. Implementation lives below in the .cppm body.
[[nodiscard]] Relation
open_sub_table_for_locator(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                           std::int16_t column_attno, const seq_locator& loc,
                           LOCKMODE lock_mode, seq_caches* cache);

/// True only when `relid` is the sub-table relation of an installed
/// seq_sub_table catalog row.  VACUUM and mutation guards use catalog
/// registration rather than trusting the relation-name convention.
[[nodiscard]] bool is_registered_sequence_fragment_relation(
    Oid relid,
    bool allow_missing_during_extension_teardown = false);

// ---------------------------------------------------------------------------
// §7.10 — Sidecar I/O. Per-shard heap relations co-sharded with sub-tables.
// ---------------------------------------------------------------------------

void
sidecar_write_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                   std::int16_t column_attno, const seq_locator& loc,
                   std::span<const std::uint8_t> blob, seq_caches* cache);

[[nodiscard]] std::optional<std::vector<char>>
sidecar_read_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const seq_locator& loc,
                  seq_caches* cache);

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
// §6.4 / §11.3 — Catalog reaper, cascade-drop entry point.
// ---------------------------------------------------------------------------

void ReapOrphanSeqSubTables();

// ---------------------------------------------------------------------------
// §8 — TAM hot-path helper for write-state cap-snapshot read.
// ---------------------------------------------------------------------------

[[nodiscard]] sequence_size_t read_max_sub_table_cap_bytes();

}  // namespace bioseq::storage

// ============================================================================
// Implementation
// ============================================================================

namespace bioseq::storage {

constexpr std::int64_t kSeqStorageAdvisoryLockClass = 0x42105260LL;

[[nodiscard]] static std::int64_t
shard_alloc_lock_key(Oid db_oid, Oid main_relfilenode,
                     std::int16_t column_attno, std::int16_t bucket_idx) noexcept {
    return (static_cast<std::int64_t>(db_oid)                << 48)
         ^ (static_cast<std::int64_t>(main_relfilenode)      << 16)
         ^ (static_cast<std::int64_t>(
                static_cast<std::uint16_t>(column_attno))    << 8)
         ^  static_cast<std::int64_t>(
                static_cast<std::uint16_t>(bucket_idx));
}

SeqColumnTypmod column_typmod_at(TupleDesc parent_tupdesc, std::int16_t column_attno) {
    if (column_attno <= 0 || column_attno > parent_tupdesc->natts) {
        storage_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            fmt::format(
                "bioseq storage: column_attno {} out of bounds (natts={})",
                column_attno, parent_tupdesc->natts));
    }
    Form_pg_attribute attr = TupleDescAttr(parent_tupdesc, column_attno - 1);
    const std::int32_t typmod = attr->atttypmod;
    if (typmod < 0) {
        storage_error_as_cpp(
            ERRCODE_INVALID_TABLE_DEFINITION,
            fmt::format(
                "bioseq storage: SEQ column attno={} has no typmod; Phase A "
                "requires every bioseq.seq column to carry an "
                "(alphabet, width) typmod",
                column_attno));
    }
    const compact_sequence_stdc_type_params params =
        compact_sequence_stdc_type_params::decode(typmod);
    std::string_view name_sv = pgseq_t::name_of_alphabet_code(params.alphabet_code);
    if (name_sv == std::string_view{"?"}) {
        storage_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            fmt::format(
                "bioseq storage: alphabet_code={} not in "
                "pgseq_supported_alphabets",
                static_cast<unsigned>(params.alphabet_code)));
    }
    SeqColumnTypmod out;
    out.alphabet_name.assign(name_sv.data(), name_sv.size());
    out.storage_unit_bit_width =
        static_cast<int>(params.storage_unit_byte_width) * 8;
    return out;
}

const char*
persistence_keyword_for_relfilenode(Oid /*db_oid*/, Oid /*spc_oid*/,
                                    Oid main_relfilenode) {
    const Oid main_relid = RelidByRelfilenumber(InvalidOid, main_relfilenode);
    if (!OidIsValid(main_relid)) {
        return "";
    }
    Relation rel = try_safe_relation_open(main_relid, AccessShareLock);
    if (rel == nullptr) {
        return "";
    }
    const char persistence = rel->rd_rel->relpersistence;
    relation_close(rel, AccessShareLock);
    switch (persistence) {
        case RELPERSISTENCE_UNLOGGED: return "UNLOGGED";
        case RELPERSISTENCE_TEMP:     return "TEMPORARY";
        default:                      return "";
    }
}

std::string format_sub_table_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                  std::int16_t column_attno, std::int16_t bucket_idx,
                                  std::int32_t shard_idx) {
    return fmt::format("seq_sub_{}_{}_{}_{}_{}_{}",
                       static_cast<unsigned>(db_oid),
                       static_cast<unsigned>(spc_oid),
                       static_cast<unsigned>(main_relfilenode),
                       static_cast<int>(column_attno),
                       static_cast<int>(bucket_idx),
                       static_cast<int>(shard_idx));
}

std::string format_sidecar_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                std::int16_t column_attno, std::int16_t bucket_idx,
                                std::int32_t shard_idx) {
    return fmt::format("idx_sidecar_{}_{}_{}_{}_{}_{}",
                       static_cast<unsigned>(db_oid),
                       static_cast<unsigned>(spc_oid),
                       static_cast<unsigned>(main_relfilenode),
                       static_cast<int>(column_attno),
                       static_cast<int>(bucket_idx),
                       static_cast<int>(shard_idx));
}

sequence_size_t read_max_sub_table_cap_bytes() {
    const int guc = std::atomic_ref<int>(bioseq_storage_max_seq_sub_table_gbs)
                        .load(std::memory_order_relaxed);
    if (guc <= 0) {
        return 0;
    }
    return static_cast<sequence_size_t>(guc) * (sequence_size_t{1} << 30);
}

}  // namespace bioseq::storage

namespace bioseq::storage {

void destroy_seq_caches(seq_caches* caches) noexcept {
    if (caches == nullptr) {
        return;
    }
    caches->~seq_caches();
    pfree(caches);
}

void close_and_destroy_seq_caches(seq_caches* caches, bool commit) noexcept {
    if (caches == nullptr) {
        return;
    }
    close_seq_caches(*caches, commit);
    destroy_seq_caches(caches);
}

void seq_caches::close_all(bool commit) noexcept {
    ErrorData* exact_error = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    {
        try {
            for (auto& kv : entries) {
                const std::uint64_t key = kv.first;
                entry& e = kv.second;
                const std::int16_t column_attno =
                    static_cast<std::int16_t>(key >> 48);
                const std::int16_t bucket_idx =
                    static_cast<std::int16_t>((key >> 32) & 0xFFFFu);
                const std::int32_t shard_idx =
                    static_cast<std::int32_t>(key & 0xFFFFFFFFu);

                if (side == Side::Write) {
                    if (commit && e.pending_bytes_delta != 0) {
                        (void) bioseq::pg_utils::invoke_pg_as_cpp([&] {
                            return SubTableManager::update_shard_bytes_written(
                                main_database_oid, main_tablespace_oid,
                                main_relfilenode, column_attno, bucket_idx,
                                shard_idx, e.pending_bytes_delta,
                                cap_bytes_snapshot);
                        });
                    }
                    e.pending_bytes_delta = 0;
                    e.pending_total_payload_delta = 0;
                    // The shard WriteState remains owned by WriteStateCache and
                    // commits with the parent; closing it here double-flushes.
                    e.write_state = nullptr;
                } else {
                    if (e.read_state != nullptr) {
                        BioseqColumnReadState* read_state = e.read_state;
                        const ResourceOwner saved_owner = CurrentResourceOwner;
                        bioseq::pg_utils::invoke_pg_as_cpp([read_state,
                                                           saved_owner,
                                                           this] {
                            if (read_resource_owner != nullptr) {
                                CurrentResourceOwner = read_resource_owner;
                            }
                            BioseqColumnEndRead(read_state);
                            CurrentResourceOwner = saved_owner;
                        });
                        e.read_state = nullptr;
                    }
                    e.read_snapshot_source = nullptr;
                }
                if (e.rel != nullptr) {
                    Relation relation = e.rel;
                    if (side == Side::Write) {
                        const ResourceOwner saved_owner = CurrentResourceOwner;
                        bioseq::pg_utils::invoke_pg_as_cpp(
                            [relation, saved_owner, this] {
                                CurrentResourceOwner =
                                    TopTransactionResourceOwner;
                                relation_close(relation, lock_mode);
                                CurrentResourceOwner = saved_owner;
                            });
                    } else if (read_resource_owner != nullptr) {
                        const ResourceOwner saved_owner = CurrentResourceOwner;
                        bioseq::pg_utils::invoke_pg_as_cpp(
                            [relation, saved_owner, this] {
                                CurrentResourceOwner = read_resource_owner;
                                relation_close(relation, lock_mode);
                                CurrentResourceOwner = saved_owner;
                            });
                    } else {
                        bioseq::pg_utils::invoke_pg_as_cpp(
                            [relation, this] {
                                relation_close(relation, lock_mode);
                            });
                    }
                    e.rel = nullptr;
                }
            }
            entries.clear();
        } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
            exact_error = error.release_error_data();
        } catch (const std::exception& error) {
            strlcpy(
                detail,
                error.what() == nullptr
                    ? "std::exception without detail"
                    : error.what(),
                sizeof(detail));
            cpp_error = true;
        } catch (...) {
            unknown_cpp_error = true;
        }
    }

    // The unordered_map iterator and all per-entry references have left scope.
    // This noexcept transaction callback may now take a PostgreSQL terminal
    // path without bypassing a C++ owner.
    if (exact_error != nullptr) ReThrowError(exact_error);
    if (cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq sequence-cache teardown raised a C++ exception"),
             errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq sequence-cache teardown raised an unknown C++ exception")));
    }
}

// DOC0286 §4.4 — flush (but do NOT pop/close) every open sub-table write state
// so a same-statement index scan sees the just-written fragment rows.
// `FlushWriteStateForRelfilenode` is keyed on the shard relation's relfilenode.
void flush_seq_cache_write_states(seq_caches* caches,
                                  SubTransactionId current_sub_xid) noexcept {
    if (caches == nullptr || caches->side != seq_caches::Side::Write) return;
    for (auto& kv : caches->entries) {
        seq_caches::entry& e = kv.second;
        if (e.rel != nullptr) {
            FlushWriteStateForRelfilenode(e.rel->rd_locator.relNumber,
                                          current_sub_xid);
        }
    }
}

// ---------------------------------------------------------------------------
// Shard open / create / lookup helpers.
// ---------------------------------------------------------------------------

[[nodiscard]] Relation
open_relation_cached(Oid sub_table_relid, LOCKMODE lock_mode,
                     seq_caches* cache,
                     std::int16_t column_attno, std::int16_t bucket_idx,
                     std::int32_t shard_idx)
{
    if (cache != nullptr) {
        const std::uint64_t key = seq_caches::key_for(column_attno, bucket_idx, shard_idx);
        auto it = cache->entries.find(key);
        if (it != cache->entries.end() && it->second.rel != nullptr) {
            return it->second.rel;
        }
        // DOC0260 §7.9 / §A1.4 — the resource owner the shard Relation is
        // pinned under MUST match the owner that is current when
        // `seq_caches::close_all` runs `relation_close()`. That owner differs
        // by side:
        //
        //   * Write side: the seq_caches lives on the transaction-scoped
        //     BioseqColumnWriteState and is closed by
        //     BioseqColumnPopWriteStateForAllRels at xact commit, under
        //     TopTransactionResourceOwner. The cached Relation therefore has
        //     to outlive the per-statement owner (which is torn down at the
        //     end of the surrounding INSERT). We pin it under
        //     TopTransactionResourceOwner so open/close balance. Pinning it
        //     under the per-stmt owner instead leaks it (force-closed at
        //     statement end) and then double-closes at xact commit.
        //
        //   * Read side: the seq_caches lives for the duration of one scan and
        //     is closed in bioseq_endscan / bioseq_index_fetch_end under the
        //     SAME portal/executor owner that is current here. We must NOT
        //     swap — pinning under TopTransaction trips "relcache reference
        //     0x... is not owned by resource owner Portal" at scan teardown.
        Relation rel = nullptr;
        if (cache->side == seq_caches::Side::Write) {
            const ResourceOwner saved_owner = CurrentResourceOwner;
            rel = bioseq::pg_utils::invoke_pg_as_cpp(
                [sub_table_relid, lock_mode, saved_owner] {
                    CurrentResourceOwner = TopTransactionResourceOwner;
                    Relation result = try_safe_relation_open(
                        sub_table_relid, lock_mode);
                    CurrentResourceOwner = saved_owner;
                    return result;
                });
        } else if (cache->read_resource_owner != nullptr) {
            const ResourceOwner saved_owner = CurrentResourceOwner;
            rel = bioseq::pg_utils::invoke_pg_as_cpp(
                [sub_table_relid, lock_mode, saved_owner, cache] {
                    CurrentResourceOwner = cache->read_resource_owner;
                    Relation result = try_safe_relation_open(
                        sub_table_relid, lock_mode);
                    CurrentResourceOwner = saved_owner;
                    return result;
                });
        } else {
            rel = bioseq::pg_utils::invoke_pg_as_cpp(
                [sub_table_relid, lock_mode] {
                    return try_safe_relation_open(
                        sub_table_relid, lock_mode);
                });
        }
        if (rel == nullptr) {
            storage_error_as_cpp(
                ERRCODE_UNDEFINED_TABLE,
                fmt::format(
                    "bioseq storage: failed to open sub-table relid {}",
                    static_cast<unsigned>(sub_table_relid)));
        }
        seq_caches::entry& entry = cache->entries[key];
        entry.rel = rel;
        return rel;
    }
    Relation rel = bioseq::pg_utils::invoke_pg_as_cpp(
        [sub_table_relid, lock_mode] {
            return try_safe_relation_open(sub_table_relid, lock_mode);
        });
    if (rel == nullptr) {
        storage_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            fmt::format(
                "bioseq storage: failed to open sub-table relid {}",
                static_cast<unsigned>(sub_table_relid)));
    }
    return rel;
}

/// One hard PG leaf for lazy shard DDL. All caller-owned strings remain outside
/// this sigsetjmp frame; this function itself owns only trivial PG handles and
/// copies the exact ErrorData before restoring/rolling back the savepoint.
void create_shard_relations_checked(
        StringInfo ddl,
        const char* sub_name,
        const char* sidecar_name,
        Oid& new_sub_relid,
        Oid& new_sidecar_relid) {
    const MemoryContext saved_context = CurrentMemoryContext;
    const ResourceOwner saved_owner = CurrentResourceOwner;
    ErrorData* volatile exact_error = nullptr;
    bool volatile subtransaction_started = false;

    PG_TRY();
    {
        BeginInternalSubTransaction("bioseq storage CREATE shard");
        subtransaction_started = true;
        MemoryContextSwitchTo(saved_context);
        const int connect_status = SPI_connect();
        if (connect_status != SPI_OK_CONNECT) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq storage: SPI_connect failed while allocating shard"),
                 errdetail("status=%d", connect_status)));
        }
        const int execute_status = SPI_execute(
            ddl->data, /*read_only=*/false, /*tcount=*/0);
        if (execute_status != SPI_OK_UTILITY
            && execute_status != SPI_OK_SELECT) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq storage: failed to create sub-table and sidecar"),
                 errdetail("sub_table=%s sidecar=%s SPI_status=%d",
                           sub_name, sidecar_name, execute_status)));
        }
        const int finish_status = SPI_finish();
        if (finish_status != SPI_OK_FINISH) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq storage: SPI_finish failed while allocating shard"),
                 errdetail("status=%d", finish_status)));
        }
        const Oid namespace_oid = get_namespace_oid(
            "bioseq_internal", false);
        new_sub_relid = get_relname_relid(sub_name, namespace_oid);
        new_sidecar_relid = get_relname_relid(sidecar_name, namespace_oid);
        if (!OidIsValid(new_sub_relid)
            || !OidIsValid(new_sidecar_relid)) {
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("bioseq storage: sub-table or sidecar not found after CREATE")));
        }
        ReleaseCurrentSubTransaction();
        subtransaction_started = false;
        MemoryContextSwitchTo(saved_context);
        CurrentResourceOwner = saved_owner;
    }
    PG_CATCH();
    {
        MemoryContext error_owner = TopTransactionContext != nullptr
            ? TopTransactionContext
            : TopMemoryContext;
        MemoryContextSwitchTo(error_owner);
        exact_error = CopyErrorData();
        MemoryContextSwitchTo(saved_context);
        FlushErrorState();

        if (subtransaction_started) {
            PG_TRY();
            {
                RollbackAndReleaseCurrentSubTransaction();
                subtransaction_started = false;
            }
            PG_CATCH();
            {
                // Preserve the original DDL/catalog error. Transaction abort
                // still owns any resources a failed rollback could not release.
                FlushErrorState();
            }
            PG_END_TRY();
        }
        MemoryContextSwitchTo(saved_context);
        CurrentResourceOwner = saved_owner;
    }
    PG_END_TRY();

    MemoryContextSwitchTo(saved_context);
    CurrentResourceOwner = saved_owner;
    if (exact_error != nullptr) {
        throw bioseq::pg_utils::PostgresErrorAsCpp{exact_error};
    }
}

[[nodiscard]] Relation
allocate_new_shard_and_open(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                            std::int16_t column_attno, TupleDesc parent_tupdesc,
                            std::int16_t bucket_idx, std::int32_t starting_shard_idx,
                            LOCKMODE lock_mode, seq_caches* cache)
{
    const std::int64_t lock_key =
        shard_alloc_lock_key(db_oid, main_relfilenode, column_attno, bucket_idx);
    bioseq::pg_utils::invoke_pg_as_cpp([lock_key] {
        (void) DirectFunctionCall1(
            pg_advisory_xact_lock_int8, Int64GetDatum(lock_key));
    });

    if (auto rechecked = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return SubTableManager::lookup_active_shard(
                db_oid, spc_oid, main_relfilenode,
                column_attno, bucket_idx);
        });
        rechecked.has_value() && rechecked->shard_idx >= starting_shard_idx)
    {
        return open_relation_cached(rechecked->sub_table_relid, lock_mode, cache,
                                    column_attno, bucket_idx, rechecked->shard_idx);
    }

    const SeqColumnTypmod tm = bioseq::pg_utils::invoke_pg_as_cpp(
        [parent_tupdesc, column_attno] {
            return column_typmod_at(parent_tupdesc, column_attno);
        });
    const char* const persistence_kw = bioseq::pg_utils::invoke_pg_as_cpp(
        [db_oid, spc_oid, main_relfilenode] {
            return persistence_keyword_for_relfilenode(
                db_oid, spc_oid, main_relfilenode);
        });

    const std::string sub_name = format_sub_table_name(
        db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx, starting_shard_idx);
    const std::string sidecar_name = format_sidecar_name(
        db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx, starting_shard_idx);

    // DOC0260 §7.3 — lazy CREATE TABLE for the sub-table + matching sidecar.
    // We deliberately do NOT register the freshly-created relations with the
    // extension via ALTER EXTENSION ADD TABLE here: doing so requires a
    // SECURITY DEFINER plpgsql helper that issues ALTER EXTENSION inside SPI
    // inside the parent INSERT's access-method callback. That nests three
    // ResourceOwner layers (executor → SPI → SECURITY DEFINER) and Postgres
    // refuses to clean up the inner TupleDescs at xact end ("tupdesc
    // reference is not owned by resource owner TopTransaction" → PANIC).
    // The bioseq_internal.seq_sub_table catalog row is the canonical
    // tracker for cleanup (BioseqColumnTableDropHook /
    // bioseq_relation_set_new_filenode + drop_sub_tables_for_main); the
    // ALTER EXTENSION ADD TABLE was a pg_dump convenience that we trade
    // for correctness — pg_dump still round-trips data through SELECT/COPY.
    StringInfo ddl = bioseq::pg_utils::invoke_pg_as_cpp([&] {
        StringInfo result = makeStringInfo();
        appendStringInfo(result,
            "CREATE %s TABLE IF NOT EXISTS bioseq_internal.%s "
                "(seq bioseq.seq('%s', %d)) USING bioseq;\n"
            "CREATE %s TABLE IF NOT EXISTS bioseq_internal.%s "
                "(sub_rowno BIGINT PRIMARY KEY, blob BYTEA NOT NULL);",
            persistence_kw, quote_identifier(sub_name.c_str()),
            tm.alphabet_name.c_str(),
            tm.storage_unit_bit_width,
            persistence_kw, quote_identifier(sidecar_name.c_str()));
        return result;
    });

    Oid new_sub_relid = InvalidOid;
    Oid new_sidecar_relid = InvalidOid;

    // The leaf owns the internal savepoint and restores the caller's memory /
    // resource owner on both commit and rollback. Caller-owned strings never
    // enter its sigsetjmp frame.
    create_shard_relations_checked(
        ddl, sub_name.c_str(), sidecar_name.c_str(),
        new_sub_relid, new_sidecar_relid);

    SubTableRow row = bioseq::pg_utils::invoke_pg_as_cpp([&] {
        return SubTableManager::insert_shard(
            db_oid, spc_oid, main_relfilenode,
            column_attno, bucket_idx, starting_shard_idx,
            new_sub_relid, new_sidecar_relid);
    });

    if (row.sub_table_relid != new_sub_relid) {
        // Another backend won the race; we DROP what we just made.
        // Reaper is the long-tail safety net.
        StringInfo drop = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            StringInfo result = makeStringInfo();
            appendStringInfo(result,
                "DROP TABLE bioseq_internal.%s; "
                "DROP TABLE bioseq_internal.%s;",
                quote_identifier(sub_name.c_str()),
                quote_identifier(sidecar_name.c_str()));
            return result;
        });
        struct RaceDropSpiState {
            int connect_status = 0;
            int execute_status = 0;
            int finish_status = 0;
        } spi{};
        const char* drop_sql = drop->data;
        bioseq::pg_utils::invoke_pg_as_cpp([&spi, drop_sql] noexcept {
            spi.connect_status = SPI_connect();
            if (spi.connect_status != SPI_OK_CONNECT) return;
            spi.execute_status = SPI_execute(
                drop_sql, /*read_only=*/false, /*tcount=*/0);
            spi.finish_status = SPI_finish();
        });
        if (spi.connect_status != SPI_OK_CONNECT
            || spi.execute_status != SPI_OK_UTILITY
            || spi.finish_status != SPI_OK_FINISH) {
            bioseq::pg_utils::invoke_pg_as_cpp([&spi] {
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq race-loser shard cleanup failed"),
                     errdetail("SPI_connect_status=%d SPI_execute_status=%d "
                               "SPI_finish_status=%d",
                               spi.connect_status, spi.execute_status,
                               spi.finish_status)));
            });
            pg_unreachable();
        }
    }

    return open_relation_cached(row.sub_table_relid, lock_mode, cache,
                                column_attno, bucket_idx, row.shard_idx);
}

[[nodiscard]] Relation
open_or_create_shard(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                     std::int16_t column_attno, TupleDesc parent_tupdesc,
                     std::int16_t bucket_idx,
                     LOCKMODE lock_mode, seq_caches* cache)
{
    if (auto active = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return SubTableManager::lookup_active_shard(
                db_oid, spc_oid, main_relfilenode,
                column_attno, bucket_idx);
        });
        active.has_value())
    {
        return open_relation_cached(active->sub_table_relid, lock_mode, cache,
                                    column_attno, bucket_idx, active->shard_idx);
    }
    // No ACTIVE shard. This happens (a) on the very first insert into the
    // (parent, column, bucket), or (b) when every existing shard has been
    // marked full — e.g. under the cap=0 sentinel where close_all flags each
    // shard full at commit (DOC0260 §10.2). In case (b) shard 0 (and others)
    // already exist, so starting at shard_idx 0 would re-INSERT an existing
    // catalog PK ("duplicate key violates seq_sub_table_pkey"). Allocate the
    // next index after the highest existing shard for this (column, bucket).
    std::int32_t next_shard_idx = 0;
    const auto shards = bioseq::pg_utils::invoke_pg_as_cpp([&] {
        return SubTableManager::list_shards_for_main(
            db_oid, spc_oid, main_relfilenode);
    });
    for (const auto& r : shards) {
        if (r.column_attno == column_attno && r.bucket_idx == bucket_idx
            && r.shard_idx >= next_shard_idx) {
            next_shard_idx = r.shard_idx + 1;
        }
    }
    return allocate_new_shard_and_open(db_oid, spc_oid, main_relfilenode,
                                       column_attno, parent_tupdesc, bucket_idx,
                                       next_shard_idx,
                                       lock_mode, cache);
}

[[nodiscard]] Relation
open_sub_table_for_locator(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                           std::int16_t column_attno, const seq_locator& loc,
                           LOCKMODE lock_mode, seq_caches* cache)
{
    if (cache != nullptr) {
        const std::uint64_t key = seq_caches::key_for(
            column_attno,
            static_cast<std::int16_t>(loc.bucket_idx),
            static_cast<std::int32_t>(loc.shard_idx));
        auto it = cache->entries.find(key);
        if (it != cache->entries.end() && it->second.rel != nullptr) {
            return it->second.rel;
        }
    }
    auto row = SubTableManager::lookup_shard(db_oid, spc_oid, main_relfilenode,
        column_attno, static_cast<std::int16_t>(loc.bucket_idx),
        static_cast<std::int32_t>(loc.shard_idx));
    if (!row.has_value()) {
        bioseq_log<ERR, PG>(
               "bioseq storage: locator references nonexistent shard "
               "(db={}, spc={}, relfilenode={}, attno={}, bucket={}, shard={})",
               static_cast<unsigned>(db_oid),
               static_cast<unsigned>(spc_oid),
               static_cast<unsigned>(main_relfilenode),
               static_cast<int>(column_attno),
               static_cast<unsigned>(loc.bucket_idx),
               loc.shard_idx);
    }
    return open_relation_cached(row->sub_table_relid, lock_mode, cache,
                                column_attno,
                                static_cast<std::int16_t>(loc.bucket_idx),
                                static_cast<std::int32_t>(loc.shard_idx));
}

namespace fragment_registry_detail {

struct AttributeContract {
    AttrNumber attno = InvalidAttrNumber;
    std::string_view name{};
    Oid type_oid = InvalidOid;
};

inline constexpr std::array<AttributeContract, 11> kAttributeContracts{{
    {1, "database_oid", OIDOID},
    {2, "tablespace_oid", OIDOID},
    {3, "main_relfilenode", OIDOID},
    {4, "column_attno", INT2OID},
    {5, "bucket_idx", INT2OID},
    {6, "shard_idx", INT4OID},
    {7, "sub_table_relid", OIDOID},
    {8, "sidecar_relid", OIDOID},
    {9, "bytes_written", INT8OID},
    {10, "is_full", BOOLOID},
    {11, "total_payload_bytes", INT8OID},
}};

[[noreturn]] inline void contract_error(const std::string_view detail) {
    const std::string owned{detail};
    bioseq::pg_utils::invoke_pg_as_cpp([&owned] {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq fragment registry catalog contract changed"),
             errdetail("%s", owned.c_str())));
    });
    pg_unreachable();
}

inline void validate_catalog(
        Relation catalog,
        const Oid expected_catalog_oid,
        const Oid expected_namespace_oid) {
    if (catalog == nullptr
        || RelationGetRelid(catalog) != expected_catalog_oid
        || RelationGetNamespace(catalog) != expected_namespace_oid
        || catalog->rd_rel == nullptr
        || catalog->rd_rel->relkind != RELKIND_RELATION
        || catalog->rd_rel->relam != HEAP_TABLE_AM_OID
        || catalog->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT) {
        contract_error("relation identity, persistence, or heap access method changed");
    }
    TupleDesc descriptor = RelationGetDescr(catalog);
    if (descriptor == nullptr
        || descriptor->natts
            != static_cast<int>(kAttributeContracts.size())) {
        contract_error("attribute count changed");
    }
    for (const AttributeContract& expected : kAttributeContracts) {
        const Form_pg_attribute attribute = TupleDescAttr(
            descriptor, expected.attno - 1);
        if (attribute->attnum != expected.attno
            || attribute->attisdropped
            || !attribute->attnotnull
            || attribute->atttypid != expected.type_oid
            || std::string_view{NameStr(attribute->attname)}
                != expected.name) {
            contract_error("fixed attribute ordinal, name, type, or NOT NULL contract changed");
        }
    }
}

[[nodiscard]] inline Relation open_catalog(
        const bool allow_missing_during_extension_teardown) {
    const Oid namespace_oid = get_namespace_oid("bioseq_internal", true);
    if (!OidIsValid(namespace_oid)) {
        if (internal_catalog_detail::absence_is_benign(
                allow_missing_during_extension_teardown)) return nullptr;
        internal_catalog_detail::missing_outside_teardown(
            "bioseq_internal.seq_sub_table");
    }
    const Oid catalog_oid = get_relname_relid(
        "seq_sub_table", namespace_oid);
    if (!OidIsValid(catalog_oid)) {
        if (internal_catalog_detail::absence_is_benign(
                allow_missing_during_extension_teardown)) return nullptr;
        internal_catalog_detail::missing_outside_teardown(
            "bioseq_internal.seq_sub_table");
    }
    Relation catalog = try_safe_relation_open(catalog_oid, AccessShareLock);
    if (catalog == nullptr) {
        if (internal_catalog_detail::absence_is_benign(
                allow_missing_during_extension_teardown)) return nullptr;
        internal_catalog_detail::missing_outside_teardown(
            "bioseq_internal.seq_sub_table");
    }
    validate_catalog(catalog, catalog_oid, namespace_oid);
    return catalog;
}

} // namespace fragment_registry_detail

bool is_registered_sequence_fragment_relation(
        Oid relid,
        const bool allow_missing_during_extension_teardown) {
    if (!OidIsValid(relid)) {
        return false;
    }
    Relation catalog = fragment_registry_detail::open_catalog(
        allow_missing_during_extension_teardown);
    if (catalog == nullptr) {
        return false;
    }
    // The exact schema proof above pins sub_table_relid to ordinal 7. This is
    // intentionally a heap scan: registration truth cannot depend on an
    // optional or corrupted secondary index.
    ScanKeyData key;
    ScanKeyInit(&key, 7, BTEqualStrategyNumber, F_OIDEQ,
        ObjectIdGetDatum(relid));
    SysScanDesc scan = systable_beginscan(
        catalog, InvalidOid, /*indexOK=*/false, SnapshotSelf, 1, &key);
    const bool found = HeapTupleIsValid(systable_getnext(scan));
    systable_endscan(scan);
    table_close(catalog, AccessShareLock);
    return found;
}

// ---------------------------------------------------------------------------
// DOC0264 §8 — insert_sequence_streamed (write hot path).
//
// One sequence → ceil(total_bp / fragment_bp) sub-table rows. Producer is
// invoked once per fragment in a per-fragment MemoryContext that is reset
// between fragments so peak RSS is bounded by one fragment's worth of state
// (consolidated-review C3). Per-sequence shard-rollover pre-allocation
// (§8.3) keeps a sequence's fragments in a single shard so the locator's
// shard_idx is a single value.
// ---------------------------------------------------------------------------

namespace detail {

// DOC0264 §4 — per-fragment storage_unit count. Class member from
// `sequence_scanner_none<T>::fragment_storage_units` (DOC0263 §3) = 1024.
// DOC0286 §4.3 — aliases the exported `kSequenceFragmentStorageUnits` so the
// public constant and this hot-path detail constant can never drift.
inline constexpr std::size_t kFragmentStorageUnits = kSequenceFragmentStorageUnits;

// Look-up bits_per_char via the supported-alphabet pack so a future alphabet
// addition does not silently produce wrong fragment_bp values.
[[nodiscard]] inline std::size_t bits_per_char_for_alphabet_code(
    pgseq_t::alphabet_code_t alpha) noexcept {
    std::size_t bits_per_char = 0;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(alpha,
        [&bits_per_char]<class A>() {
            bits_per_char = static_cast<std::size_t>(
                seqan3::detail::ceil_log2(
                    static_cast<std::size_t>(seqan3::alphabet_size<A>)));
        });
    return bits_per_char;
}

// letters_per_unit = (storage_unit_byte_width * 8) / bits_per_char.
[[nodiscard]] inline std::size_t letters_per_unit_for(
    pgseq_t::alphabet_code_t alpha, std::uint8_t logical_byte_width) noexcept {
    const std::size_t bpc = bits_per_char_for_alphabet_code(alpha);
    return bpc > 0
        ? (static_cast<std::size_t>(logical_byte_width) * 8u) / bpc
        : 0u;
}

/// Feed the exact logical symbol bytes used by the existing revision-3
/// independent checksum oracle. Fragment-sized decoding keeps memory bounded
/// while moving the O(N) work to insertion rather than a cold index read.
inline void update_canonical_source_checksum(
        bioseq::indexing::Crc64Ecma182& checksum,
        const pgseq_t* fragment) {
    if (fragment == nullptr || is_locator_stub(fragment)) {
        source_version_detail::source_catalog_error(
            "canonical checksum received a non-fragment value");
    }
    fragment->convert_to([&checksum]<class S>(
            const compact_sequence_range<S>& range) {
        const std::string decoded =
            ::bioseq::simd::active_target::decode_sequence_simd<S>(range);
        if (decoded.size() != range.bp_size()) {
            source_version_detail::source_catalog_error(
                "canonical checksum decoded length disagrees");
        }
        checksum.update(std::string_view{decoded});
    });
}

}  // namespace detail

template<class L, std::size_t M, std::size_t X>
template<class FragmentProducer>
PendingSequenceSource
sequence_storage_manager<L, M, X>::insert_sequence_streamed(
    Oid user_relid, std::uint64_t main_storage_id,
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, TupleDesc parent_tupdesc,
    std::uint8_t logical_alphabet_code,
    std::uint8_t logical_storage_unit_byte_width,
    sequence_size_t total_bp_size,
    std::uint16_t last_fragment_padding_letters,
    sequence_size_t current_cap_bytes,
    seq_caches* cache,
    FragmentProducer&& producer)
{
    if (!OidIsValid(user_relid) || main_storage_id == 0) {
        bioseq_log<ERR, PG>(
            "bioseq storage: source epoch requires parent relation/storage identity");
    }
    if (cache == nullptr || cache->side != seq_caches::Side::Write) {
        bioseq_log<ERR, PG>( "bioseq storage: insert_sequence_streamed requires a write-side seq_caches");
    }
    if (cache->main_database_oid == InvalidOid) {
        cache->main_database_oid   = db_oid;
        cache->main_tablespace_oid = spc_oid;
        cache->main_relfilenode    = main_relfilenode;
    }

    const auto alpha_code = static_cast<pgseq_t::alphabet_code_t>(logical_alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha_code,
                                                         logical_storage_unit_byte_width);
    if (lpu == 0) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: insert_sequence_streamed could not derive "
               "letters_per_unit (alphabet_code={}, byte_width={})",
               static_cast<unsigned>(alpha_code),
               static_cast<unsigned>(logical_storage_unit_byte_width));
    }
    const sequence_size_t frag_bp =
        static_cast<sequence_size_t>(detail::kFragmentStorageUnits) * lpu;
    const sequence_size_t num_fragments =
        total_bp_size == 0 ? 0u : (total_bp_size + frag_bp - 1) / frag_bp;
    if (num_fragments > static_cast<sequence_size_t>(
            std::numeric_limits<std::uint32_t>::max())) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: num_fragments {} exceeds UINT32_MAX",
               num_fragments);
    }

    // Per-row varlena bytes for a full (= non-last) fragment.
    const std::size_t frag_varlena_bytes =
        pgseq_t::header_bytes()
        + detail::kFragmentStorageUnits
              * static_cast<std::size_t>(logical_storage_unit_byte_width);
    // Total varlena bytes the sequence occupies in the shard (= sum of
    // fragment-row sizes). Used for the per-shard cap accounting; each row
    // is the same size in Phase C because the last fragment row is also
    // allocated at `frag_varlena_bytes` (palloc0 leaves trailing units 0).
    const sequence_size_t total_varlena_bytes =
        static_cast<sequence_size_t>(num_fragments) *
        static_cast<sequence_size_t>(frag_varlena_bytes);

    const std::int16_t bucket_idx =
        static_cast<std::int16_t>(bucket_index_for_size(total_bp_size));

    const std::uint64_t source_epoch =
        SequenceSourceVersionManager::allocate_epoch();
    bioseq::indexing::Crc64Ecma182 source_checksum{};

    if (num_fragments == 0) {
        seq_locator empty_locator{};
        empty_locator.version = kLocatorVersion;
        empty_locator.bucket_idx = static_cast<std::uint8_t>(bucket_idx);
        empty_locator.shard_idx = 0;
        empty_locator.num_fragments = 0;
        empty_locator.total_bp_size = 0;
        empty_locator.first_sub_row_no = 0;
        empty_locator.source_epoch = source_epoch;
        SequenceSourceVersionManager::insert_unpublished(
            SequenceSourceVersionRow{
                .source_epoch = source_epoch,
                .user_relid = user_relid,
                .database_oid = db_oid,
                .tablespace_oid = spc_oid,
                .main_relfilenode = main_relfilenode,
                .main_storage_id = main_storage_id,
                .column_attno = column_attno,
                .locator = empty_locator,
                .fragment_relid = InvalidOid,
                .fragment_storage_id = 0,
                .alphabet_code = logical_alphabet_code,
                .storage_unit_byte_width = logical_storage_unit_byte_width,
                .padding_letters = last_fragment_padding_letters,
                .source_content_checksum = source_checksum.checksum(),
                .state = kSequenceSourceStateUnpublished,
            });
        return PendingSequenceSource{empty_locator};
    }

    // DOC0264 §8.3 — per-sequence shard pre-allocation. If the active shard
    // cannot fit the WHOLE sequence's bytes, allocate a fresh shard up front.
    Relation shard = open_or_create_shard(db_oid, spc_oid, main_relfilenode,
                                          column_attno, parent_tupdesc, bucket_idx,
                                          RowExclusiveLock, cache);

    auto active = SubTableManager::lookup_active_shard(db_oid, spc_oid, main_relfilenode,
                                            column_attno, bucket_idx);
    if (!active.has_value()) {
        bioseq_log<ERR, PG>( "bioseq storage: active shard disappeared mid-insert");
    }
    const std::uint64_t active_key =
        seq_caches::key_for(column_attno, bucket_idx, active->shard_idx);
    sequence_size_t pending = 0;
    if (auto it = cache->entries.find(active_key); it != cache->entries.end()) {
        pending = it->second.pending_bytes_delta;
    }
    const sequence_size_t projected =
        active->bytes_written + pending + total_varlena_bytes;
    // DOC0264 §8.3 — pre-allocate a fresh shard when the active one cannot
    // fit the whole sequence. Same semantics as Phase A's per-row rollover,
    // but the unit of accounting is the whole sequence now.
    const bool rollover_required =
        active->bytes_written + pending > 0
        && (current_cap_bytes == 0 || projected > current_cap_bytes);

    std::int32_t shard_idx = 0;
    if (rollover_required) {
        if (auto it = cache->entries.find(active_key);
            it != cache->entries.end() && it->second.pending_bytes_delta != 0)
        {
            SubTableManager::update_shard_bytes_written(
                db_oid, spc_oid, main_relfilenode,
                column_attno, bucket_idx, active->shard_idx,
                it->second.pending_bytes_delta,
                current_cap_bytes);
            it->second.pending_bytes_delta = 0;
            it->second.pending_total_payload_delta = 0;
        } else {
            SubTableManager::update_shard_bytes_written(
                db_oid, spc_oid, main_relfilenode,
                column_attno, bucket_idx, active->shard_idx,
                /*delta_bytes=*/0,
                current_cap_bytes == 0 ? sequence_size_t{1} : current_cap_bytes);
        }
        shard = allocate_new_shard_and_open(db_oid, spc_oid, main_relfilenode,
                                            column_attno, parent_tupdesc, bucket_idx,
                                            active->shard_idx + 1,
                                            RowExclusiveLock, cache);
        shard_idx = active->shard_idx + 1;
    } else {
        shard_idx = active->shard_idx;
    }

    // DOC0264 §8.3 hard guard: refuse to write a sequence whose total bytes
    // exceed the cap (would not fit any shard, including a freshly-allocated
    // one). current_cap_bytes==0 is the sentinel "no cap, one shard per
    // sequence" and the check is suppressed.
    if (current_cap_bytes > 0 && total_varlena_bytes > current_cap_bytes) {
        bioseq_log<ERR, PG>(
            "bioseq storage Phase C: sequence with {} bytes exceeds shard cap "
            "{} bytes ({} GiB). Raise bioseq.storage.max_seq_sub_table_gbs.",
            total_varlena_bytes, current_cap_bytes,
            current_cap_bytes >> 30);
    }

    const std::uint64_t cache_key =
        seq_caches::key_for(column_attno, bucket_idx, shard_idx);
    seq_caches::entry& entry = cache->entries[cache_key];
    entry.rel = shard;

    if (entry.write_state == nullptr) {
        entry.write_state = bioseq_init_write_state(
            shard, RelationGetDescr(shard),
            RelationGetRelid(shard),
            GetCurrentSubTransactionId());
    }

    // DOC0264 §8.4 — serialise writers on this (parent, column, bucket,
    // shard) so the row numbers BioseqColumnWriteRow assigns are CONTIGUOUS
    // within one sequence's fragments.
    const std::int64_t row_lock_key =
          (static_cast<std::int64_t>(db_oid)                << 48)
        ^ (static_cast<std::int64_t>(main_relfilenode)      << 16)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(column_attno))    << 8)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(bucket_idx))      << 4)
        ^  static_cast<std::int64_t>(shard_idx);
    DirectFunctionCall1(pg_advisory_xact_lock_int8, Int64GetDatum(row_lock_key));

    // DOC0264 §8 — separate scratch + per-fragment contexts. The producer
    // may allocate long-lived state in `scratch_mctx`; per-fragment palloc
    // happens in `per_fragment_mctx` which is RESET between fragments to
    // keep peak RSS bounded.
    MemoryContext scratch_mctx = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq insert_sequence scratch",
        ALLOCSET_DEFAULT_SIZES);
    MemoryContext per_fragment_mctx = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq insert_sequence per-fragment",
        ALLOCSET_DEFAULT_SIZES);

    std::uint64_t first_sub_row_no = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t last_sub_row_no  = std::numeric_limits<std::uint64_t>::max();

    for (sequence_size_t fragment_no = 0; fragment_no < num_fragments; ++fragment_no) {
        MemoryContextReset(per_fragment_mctx);
        const bool is_last = (fragment_no + 1 == num_fragments);
        MemoryContextScope frag_ctx{per_fragment_mctx};

        pgseq_t* frag = static_cast<pgseq_t*>(palloc0(frag_varlena_bytes));
        SET_VARSIZE(frag, frag_varlena_bytes);
        frag->alphabet_code           = static_cast<pgseq_t::alphabet_code_t>(
                                            logical_alphabet_code);
        frag->storage_unit_byte_width = logical_storage_unit_byte_width;
        // Producer fills bp_size + padding_letters + data[] for fragment `n`.
        std::forward<FragmentProducer>(producer)(
            static_cast<std::size_t>(fragment_no), frag, scratch_mctx, is_last);

        // Cross-check the producer's bp_size / padding consistency.
        const sequence_size_t expected_bp = is_last
            ? (total_bp_size - fragment_no * frag_bp)
            : frag_bp;
        if (static_cast<sequence_size_t>(frag->bp_size) != expected_bp) {
            bioseq_log<ERR, PG>(
                "bioseq storage Phase C: producer bp_size mismatch on fragment {} "
                "(got {}, expected {})",
                fragment_no, static_cast<sequence_size_t>(frag->bp_size), expected_bp);
        }
        if (is_last && frag->padding_letters != last_fragment_padding_letters) {
            bioseq_log<ERR, PG>(
                "bioseq storage Phase C: producer last-fragment padding_letters "
                "mismatch (got {}, expected {})",
                static_cast<unsigned>(frag->padding_letters),
                static_cast<unsigned>(last_fragment_padding_letters));
        }
        if (!is_last && frag->padding_letters != 0) {
            bioseq_log<ERR, PG>(
                "bioseq storage Phase C: producer non-last padding_letters must be 0 "
                "(fragment {}, got {})",
                fragment_no, static_cast<unsigned>(frag->padding_letters));
        }

        detail::update_canonical_source_checksum(source_checksum, frag);

        Datum values[1] = { PointerGetDatum(frag) };
        bool  nulls[1]  = { false };
        const std::uint64_t row_no = BioseqColumnWriteRow(entry.write_state, values, nulls);
        if (fragment_no == 0) {
            first_sub_row_no = row_no;
        } else {
            if (row_no != last_sub_row_no + 1) {
                bioseq_log<ERR, PG>(
                    "bioseq storage Phase C: non-contiguous row numbers during "
                    "streamed insert (expected {}, got {})",
                    last_sub_row_no + 1, row_no);
            }
        }
        last_sub_row_no = row_no;
    }

    MemoryContextDelete(per_fragment_mctx);
    MemoryContextDelete(scratch_mctx);

    entry.pending_bytes_delta         += total_varlena_bytes;
    entry.pending_total_payload_delta += total_varlena_bytes;

    if (num_fragments == 0) {
        // Empty sequence — no rows were written. Choose a deterministic row
        // number (0) as the stub's first_sub_row_no; the scanner short-
        // circuits when num_fragments == 0 and never reads this row.
        first_sub_row_no = 0;
    }

    seq_locator out{};
    out.version          = kLocatorVersion;
    out.bucket_idx       = static_cast<std::uint8_t>(bucket_idx);
    out._pad_2           = 0;
    out.shard_idx        = static_cast<std::uint32_t>(shard_idx);
    out.num_fragments    = static_cast<std::uint32_t>(num_fragments);
    out._pad_4           = 0;
    out.total_bp_size    = total_bp_size;
    out.first_sub_row_no = first_sub_row_no;
    out.source_epoch     = source_epoch;

    const Oid fragment_relid = RelationGetRelid(shard);
    const std::uint64_t fragment_storage_id =
        BioseqColumnStorageGetStorageId(shard, false);
    SequenceSourceVersionManager::insert_unpublished(
        SequenceSourceVersionRow{
            .source_epoch = source_epoch,
            .user_relid = user_relid,
            .database_oid = db_oid,
            .tablespace_oid = spc_oid,
            .main_relfilenode = main_relfilenode,
            .main_storage_id = main_storage_id,
            .column_attno = column_attno,
            .locator = out,
            .fragment_relid = fragment_relid,
            .fragment_storage_id = fragment_storage_id,
            .alphabet_code = logical_alphabet_code,
            .storage_unit_byte_width = logical_storage_unit_byte_width,
            .padding_letters = last_fragment_padding_letters,
            .source_content_checksum = source_checksum.checksum(),
            .state = kSequenceSourceStateUnpublished,
        });
    return PendingSequenceSource{out};
}

template<class L, std::size_t M, std::size_t X>
PendingSequenceSource
sequence_storage_manager<L, M, X>::insert_payload_as_fragments(
    Oid user_relid, std::uint64_t main_storage_id,
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, TupleDesc parent_tupdesc,
    const pgseq_t* original_hdr,
    sequence_size_t current_cap_bytes,
    seq_caches* cache)
{
    if (original_hdr == nullptr) {
        bioseq_log<ERR, PG>( "bioseq storage: insert_payload_as_fragments received null original_hdr");
    }
    if (is_locator_stub(original_hdr)) {
        bioseq_log<ERR, PG>( "bioseq storage: insert_payload_as_fragments received a stub; "
               "the caller must screen out stubs before re-inserting");
    }

    const sequence_size_t total_bp_size =
        static_cast<sequence_size_t>(original_hdr->bp_size);
    const std::uint8_t logical_alphabet_code =
        static_cast<std::uint8_t>(original_hdr->alphabet_code);
    const std::uint8_t logical_byte_width =
        static_cast<std::uint8_t>(original_hdr->storage_unit_byte_width);
    const std::uint16_t last_fragment_padding =
        static_cast<std::uint16_t>(original_hdr->padding_letters);

    const auto alpha_code = static_cast<pgseq_t::alphabet_code_t>(logical_alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha_code, logical_byte_width);
    if (lpu == 0) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: insert_payload_as_fragments could not derive "
               "letters_per_unit (alphabet_code={}, byte_width={})",
               static_cast<unsigned>(alpha_code),
               static_cast<unsigned>(logical_byte_width));
    }
    const sequence_size_t frag_bp =
        static_cast<sequence_size_t>(detail::kFragmentStorageUnits) * lpu;
    const sequence_size_t num_fragments =
        total_bp_size == 0 ? 0u : (total_bp_size + frag_bp - 1) / frag_bp;
    const std::size_t bytes_per_unit =
        static_cast<std::size_t>(logical_byte_width);
    const std::size_t fragment_bytes =
        detail::kFragmentStorageUnits * bytes_per_unit;

    // Slice the original payload into N fragment rows.
    const std::uint8_t* const src_data =
        reinterpret_cast<const std::uint8_t*>(original_hdr->data);
    const std::size_t src_bytes =
        static_cast<std::size_t>(VARSIZE(original_hdr)) - pgseq_t::header_bytes();

    auto producer = [src_data, src_bytes, fragment_bytes, frag_bp, total_bp_size,
                     last_fragment_padding](
        std::size_t fragment_no, pgseq_t* dst, MemoryContext /*scratch*/,
        bool is_last) noexcept
    {
        const std::size_t src_offset = fragment_no * fragment_bytes;
        const std::size_t copy_bytes = std::min(fragment_bytes,
            src_offset < src_bytes ? src_bytes - src_offset : std::size_t{0});
        if (copy_bytes > 0) {
            std::memcpy(dst->data, src_data + src_offset, copy_bytes);
        }
        // Trailing bytes already zero from palloc0.
        const sequence_size_t this_fragment_bp = is_last
            ? (total_bp_size - static_cast<sequence_size_t>(fragment_no) * frag_bp)
            : frag_bp;
        dst->bp_size = static_cast<pgseq_t::bp_size_t>(this_fragment_bp);
        dst->padding_letters = is_last ? last_fragment_padding : std::uint16_t{0};
    };

    return insert_sequence_streamed(
        user_relid, main_storage_id,
        db_oid, spc_oid, main_relfilenode, column_attno, parent_tupdesc,
        logical_alphabet_code, logical_byte_width,
        total_bp_size, last_fragment_padding, current_cap_bytes, cache,
        producer);
}

// ---------------------------------------------------------------------------
// DOC0286 §4.3 — sequence_stream_writer implementation. Mirrors the per-fragment
// row write of insert_sequence_streamed, but driven incrementally: the total
// length is unknown until finish(), so the shard is opened lazily and the cap
// is checked as the sequence streams. No cross-shard rollover (one shard per
// sequence); a single sequence over the cap raises ERROR and the transaction
// rollback cleans the partial fragment rows.
// ---------------------------------------------------------------------------
void sequence_stream_writer::begin(
    Oid user_relid, Oid db_oid, Oid spc_oid,
    RelFileNumber main_relfilenode, std::uint64_t main_storage_id,
    AttrNumber seq_attno, TupleDesc parent_td,
    std::uint8_t alphabet_code, std::uint8_t byte_width,
    sequence_size_t cap_bytes, seq_caches* caches)
{
    if (caches == nullptr || caches->side != seq_caches::Side::Write) {
        storage_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq.load: sequence_stream_writer requires a write-side "
            "seq_caches");
    }
    if (!OidIsValid(user_relid) || main_storage_id == 0) {
        storage_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq.load: source epoch requires parent relation/storage identity");
    }
    user_relid_ = user_relid;
    db_oid_ = db_oid; spc_oid_ = spc_oid; main_relfilenode_ = main_relfilenode;
    main_storage_id_ = main_storage_id;
    column_attno_ = static_cast<std::int16_t>(seq_attno);
    parent_td_ = parent_td;
    alphabet_code_ = alphabet_code; byte_width_ = byte_width;
    cap_bytes_ = cap_bytes; caches_ = caches;

    if (caches_->main_database_oid == InvalidOid) {
        caches_->main_database_oid   = db_oid;
        caches_->main_tablespace_oid = spc_oid;
        caches_->main_relfilenode    = main_relfilenode;
    }

    const auto alpha = static_cast<pgseq_t::alphabet_code_t>(alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha, byte_width);
    if (lpu == 0) {
        storage_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            fmt::format(
                "bioseq.load: could not derive letters_per_unit "
                "(alphabet_code={}, byte_width={})",
                static_cast<unsigned>(alpha),
                static_cast<unsigned>(byte_width)));
    }
    frag_payload_bytes_ = detail::kFragmentStorageUnits * static_cast<std::size_t>(byte_width);
    frag_varlena_bytes_ = pgseq_t::header_bytes() + frag_payload_bytes_;
    frag_bp_ = static_cast<sequence_size_t>(detail::kFragmentStorageUnits) * lpu;

    buffer_.clear();
    buffer_.reserve(frag_payload_bytes_ + 256);
    first_sub_row_no_ = std::numeric_limits<std::uint64_t>::max();
    last_sub_row_no_  = std::numeric_limits<std::uint64_t>::max();
    num_fragments_ = 0;
    total_varlena_bytes_ = 0;
    source_checksum_value_ = 0;
    source_epoch_ = SequenceSourceVersionManager::allocate_epoch();
    bucket_idx_ = 0;
    shard_idx_ = 0;
    cache_key_ = 0;
    shard_ = nullptr;
    write_state_ = nullptr;
    active_ = true;
}

void sequence_stream_writer::open_shard() {
    if (shard_ != nullptr) return;
    // All streamed sequences land in the top length bucket: the size is unknown
    // until finish(), the bucket is purely organisational (the locator records
    // it; reads use the locator), and co-locating a load's fragments keeps
    // sequential-read locality.
    bucket_idx_ = static_cast<std::int16_t>(
        bioseq_default_storage_manager::bucket_index_for_size(
            std::numeric_limits<sequence_size_t>::max()));
    shard_ = open_or_create_shard(
        db_oid_, spc_oid_, main_relfilenode_, column_attno_, parent_td_,
        bucket_idx_, RowExclusiveLock, caches_);
    auto active = bioseq::pg_utils::invoke_pg_as_cpp([this] {
        return SubTableManager::lookup_active_shard(
            db_oid_, spc_oid_, main_relfilenode_,
            column_attno_, bucket_idx_);
    });
    if (!active.has_value()) {
        storage_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq.load: active shard disappeared mid-load");
    }
    shard_idx_ = active->shard_idx;
    cache_key_ = seq_caches::key_for(column_attno_, bucket_idx_, shard_idx_);
    seq_caches::entry& entry = caches_->entries[cache_key_];
    entry.rel = shard_;
    if (entry.write_state == nullptr) {
        const SubTransactionId subtransaction =
            bioseq::pg_utils::invoke_pg_as_cpp(
                [] { return GetCurrentSubTransactionId(); });
        entry.write_state = bioseq_init_write_state(
            shard_, RelationGetDescr(shard_), RelationGetRelid(shard_),
            subtransaction);
    }
    write_state_ = entry.write_state;
    // DOC0264 §8.4 — serialise writers on this (parent, column, bucket, shard)
    // so the row numbers BioseqColumnWriteRow assigns stay contiguous within one
    // sequence's fragments (and across the sequences of one load).
    const std::int64_t row_lock_key =
          (static_cast<std::int64_t>(db_oid_)            << 48)
        ^ (static_cast<std::int64_t>(main_relfilenode_)  << 16)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(column_attno_)) << 8)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(bucket_idx_))   << 4)
        ^  static_cast<std::int64_t>(shard_idx_);
    bioseq::pg_utils::invoke_pg_as_cpp([row_lock_key] {
        (void) DirectFunctionCall1(
            pg_advisory_xact_lock_int8, Int64GetDatum(row_lock_key));
    });
}

void sequence_stream_writer::write_fragment(
    const std::uint8_t* payload, std::size_t payload_bytes,
    sequence_size_t bp, std::uint16_t pad)
{
    open_shard();
    if (cap_bytes_ > 0 &&
        total_varlena_bytes_ + frag_varlena_bytes_ > cap_bytes_) {
        storage_error_as_cpp(
            ERRCODE_PROGRAM_LIMIT_EXCEEDED,
            fmt::format(
                "bioseq.load: sequence exceeds shard cap {} bytes ({} GiB) "
                "at fragment {}. Raise "
                "bioseq.storage.max_seq_sub_table_gbs.",
                total_varlena_bytes_ + frag_varlena_bytes_,
                cap_bytes_ >> 30, num_fragments_));
    }
    // Keep the allocation in a checked leaf so palloc cannot longjmp across
    // caller-owned checksum/state objects. BioseqColumnWriteRow provides its
    // own checked writer leaf below.
    pgseq_t* frag = bioseq::pg_utils::invoke_pg_as_cpp([this] {
        const MemoryContext old = MemoryContextSwitchTo(
            BioseqColumnWritePerTupleContext(write_state_));
        pgseq_t* result = static_cast<pgseq_t*>(
            palloc0(frag_varlena_bytes_));
        MemoryContextSwitchTo(old);
        return result;
    });
    SET_VARSIZE(frag, frag_varlena_bytes_);
    frag->alphabet_code =
        static_cast<pgseq_t::alphabet_code_t>(alphabet_code_);
    frag->storage_unit_byte_width = byte_width_;
    frag->bp_size = static_cast<pgseq_t::bp_size_t>(bp);
    frag->padding_letters = pad;
    if (payload_bytes > 0) {
        std::memcpy(frag->data, payload, payload_bytes);
    }
    bioseq::indexing::Crc64Ecma182 checksum{
        .value = source_checksum_value_};
    detail::update_canonical_source_checksum(checksum, frag);
    source_checksum_value_ = checksum.checksum();
    Datum values[1] = {PointerGetDatum(frag)};
    bool nulls[1] = {false};
    const std::uint64_t row_no =
        BioseqColumnWriteRow(write_state_, values, nulls);
    if (num_fragments_ == 0) {
        first_sub_row_no_ = row_no;
    } else if (row_no != last_sub_row_no_ + 1) {
        storage_error_as_cpp(
            ERRCODE_DATA_EXCEPTION,
            fmt::format(
                "bioseq.load: non-contiguous row numbers during streamed "
                "insert (expected {}, got {})",
                last_sub_row_no_ + 1, row_no));
    }
    last_sub_row_no_ = row_no;
    ++num_fragments_;
    total_varlena_bytes_ += frag_varlena_bytes_;
}

void sequence_stream_writer::append_packed_bytes(
    std::span<const std::uint8_t> bytes, sequence_size_t /*bp_count*/)
{
    if (!active_) {
        storage_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq.load: append_packed_bytes before begin()");
    }
    if (bytes.empty()) return;
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    // Write full fragments only while STRICTLY more than one full fragment is
    // buffered, so the final (possibly padded) fragment is always emitted by
    // finish() with the caller's last_pad. Non-final fragments are full units
    // (padding 0, bp = frag_bp_).
    std::size_t off = 0;
    while (buffer_.size() - off > frag_payload_bytes_) {
        write_fragment(buffer_.data() + off, frag_payload_bytes_, frag_bp_, 0);
        off += frag_payload_bytes_;
    }
    if (off > 0) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(off));
    }
}

PendingSequenceSource sequence_stream_writer::finish(
    sequence_size_t total_bp, std::uint16_t last_pad)
{
    if (!active_) {
        storage_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq.load: finish() before begin()");
    }
    // Emit the final fragment (the buffered remainder ≤ one fragment). Its bp is
    // whatever remains after the full fragments already written.
    const sequence_size_t emitted_bp =
        static_cast<sequence_size_t>(num_fragments_) * frag_bp_;
    if (!buffer_.empty()) {
        const sequence_size_t last_bp = total_bp - emitted_bp;
        write_fragment(buffer_.data(), buffer_.size(), last_bp, last_pad);
        buffer_.clear();
    }

    if (num_fragments_ == 0) {
        // Empty sequence — no rows written; the scanner short-circuits on
        // num_fragments == 0 and never reads first_sub_row_no.
        first_sub_row_no_ = 0;
    } else {
        seq_caches::entry& entry = caches_->entries[cache_key_];
        entry.pending_bytes_delta         += total_varlena_bytes_;
        entry.pending_total_payload_delta += total_varlena_bytes_;
    }

    seq_locator out{};
    out.version          = kLocatorVersion;
    out.bucket_idx       = static_cast<std::uint8_t>(bucket_idx_);
    out._pad_2           = 0;
    out.shard_idx        = static_cast<std::uint32_t>(shard_idx_);
    out.num_fragments    = num_fragments_;
    out._pad_4           = 0;
    out.total_bp_size    = total_bp;
    out.first_sub_row_no = first_sub_row_no_;
    out.source_epoch     = source_epoch_;

    const Oid fragment_relid = num_fragments_ == 0
        ? InvalidOid : RelationGetRelid(shard_);
    const std::uint64_t fragment_storage_id = num_fragments_ == 0
        ? 0 : bioseq::pg_utils::invoke_pg_as_cpp([this] {
            return BioseqColumnStorageGetStorageId(shard_, false);
        });
    SequenceSourceVersionManager::insert_unpublished(
        SequenceSourceVersionRow{
            .source_epoch = source_epoch_,
            .user_relid = user_relid_,
            .database_oid = db_oid_,
            .tablespace_oid = spc_oid_,
            .main_relfilenode = static_cast<Oid>(main_relfilenode_),
            .main_storage_id = main_storage_id_,
            .column_attno = column_attno_,
            .locator = out,
            .fragment_relid = fragment_relid,
            .fragment_storage_id = fragment_storage_id,
            .alphabet_code = alphabet_code_,
            .storage_unit_byte_width = byte_width_,
            .padding_letters = last_pad,
            .source_content_checksum = source_checksum_value_,
            .state = kSequenceSourceStateUnpublished,
        });
    active_ = false;
    return PendingSequenceSource{out};
}

void sequence_stream_writer::abort() {
    // Transaction rollback reclaims any fragment rows already written; just drop
    // the in-flight buffer + state so a re-begin starts clean.
    buffer_.clear();
    active_ = false;
    source_epoch_ = 0;
}

// ---------------------------------------------------------------------------
// DOC0264 §9.4 — fetch_payload streams all fragments into a single
// contiguous pgseq_t* in `out_mctx`. For Phase C sequences this opens the
// shard once via Phase A's cache, opens a random-access BioseqColumnReadState,
// walks all `num_fragments` rows and memcpys each fragment's `data[]` into
// the right offset of the destination varlena's payload region.
//
// Raises bioseq_log<ERR, PG>() if the assembled varlena would exceed MaxAllocSize —
// streaming readers must use `sequence_scanner_pg<S>` instead. The legacy
// callers (auto-hydration of stub-backed scan slots) are bounded by the
// MaxAllocSize gate in `hydrate_seq_columns_in_slot` (§9.4 access_method).
// ---------------------------------------------------------------------------

[[nodiscard]] inline BioseqColumnReadState*
begin_sequence_payload_read(Relation shard,
                            MemoryContext payload_mctx,
                            Snapshot snapshot) {
    MemoryContextScope ctx{payload_mctx};
    TupleDesc td = RelationGetDescr(shard);
    PBitmapset attr_needed = PBitmapset::full_range(td->natts);
    PIntList projected;
    for (int i = 0; i < td->natts; ++i) {
        if (!TupleDescAttr(td, i)->attisdropped && attr_needed.contains(i)) {
            projected.append(static_cast<AttrNumber>(i + 1));
        }
    }
    BioseqColumnReadState* read_state = BioseqColumnBeginRead(
        shard, td, projected.release(), /*whereClauseList=*/NIL,
        payload_mctx, snapshot, /*randomAccess=*/true, nullptr);
    BioseqColumnReadFlushPendingWrites(read_state);
    attr_needed.free();
    return read_state;
}

template<class L, std::size_t M, std::size_t X>
pgseq_t*
sequence_storage_manager<L, M, X>::fetch_payload(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, const seq_locator& loc,
    Snapshot snapshot,
    MemoryContext out_mctx, seq_caches* cache)
{
    if (cache == nullptr) {
        bioseq_log<ERR, PG>( "bioseq storage: fetch_payload requires a seq_caches");
    }
    if (snapshot == nullptr) {
        bioseq_log<ERR, PG>(
            "bioseq storage: fetch_payload requires an explicit snapshot");
    }
    const bool registered_mvcc_snapshot =
        IsMVCCSnapshot(snapshot) && snapshot->regd_count != 0;
    // SnapshotAny is backend-lifetime static state. Other non-MVCC snapshots
    // (especially caller-stack Dirty snapshots) get a per-call reader so no
    // cache entry can outlive their owner.
    // An unregistered MVCC value can still be used for this synchronous call,
    // but cannot be retained beyond it.
    const bool retain_read_state =
        registered_mvcc_snapshot || snapshot == SnapshotAny;

    // DOC0264 §6.5 — empty sequence (num_fragments == 0). The locator is the
    // only on-disk artefact; there are no sub-table rows to fetch. Return a
    // minimal empty pgseq_t in `out_mctx`.
    if (loc.num_fragments == 0) {
        MemoryContextScope ctx{out_mctx};
        pgseq_t* out = static_cast<pgseq_t*>(palloc0(pgseq_t::header_bytes()));
        SET_VARSIZE(out, pgseq_t::header_bytes());
        // alphabet/width/bp_size/padding are all zero — caller-side stub
        // hydration sees an empty payload (compact_sequence_range handles
        // size=0 correctly per DOC0258 §5.3).
        return out;
    }

    Relation shard = open_sub_table_for_locator(db_oid, spc_oid, main_relfilenode,
        column_attno, loc, AccessShareLock, cache);
    const std::uint64_t cache_key = seq_caches::key_for(
        column_attno,
        static_cast<std::int16_t>(loc.bucket_idx),
        static_cast<std::int32_t>(loc.shard_idx));
    seq_caches::entry& entry = cache->entries[cache_key];
    entry.rel = shard;

    MemoryContext payload_mctx =
        cache->read_state_mctx != nullptr ? cache->read_state_mctx
                                          : CurrentMemoryContext;
    BioseqColumnReadState* read_state = nullptr;
    bool transient_read_state = false;
    if (retain_read_state) {
        // Key compatibility by the caller-owned source snapshot, not the
        // read-state pointer: flushing may replace read_state->snapshot with
        // its own refreshed registered copy while the owner remains the same.
        if (entry.read_state != nullptr
            && entry.read_snapshot_source != snapshot) {
            BioseqColumnEndRead(entry.read_state);
            entry.read_state = nullptr;
            entry.read_snapshot_source = nullptr;
        }
        if (entry.read_state == nullptr) {
            entry.read_state = begin_sequence_payload_read(
                shard, payload_mctx, snapshot);
            entry.read_snapshot_source = snapshot;
        }
        read_state = entry.read_state;
    } else {
        read_state = begin_sequence_payload_read(
            shard, payload_mctx, snapshot);
        transient_read_state = true;
    }

    // DOC0264 §9.4 — derive fragment shape from logical alphabet/width on
    // first sub-table row.
    Datum first_vals[1]  = { 0 };
    bool  first_nulls[1] = { true };
    if (!BioseqColumnReadRowByRowNumber(read_state, loc.first_sub_row_no,
                                    first_vals, first_nulls)) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: missing first sub-table row {} "
               "(locator bucket={} shard={} num_fragments={})",
               loc.first_sub_row_no,
               static_cast<unsigned>(loc.bucket_idx),
               loc.shard_idx, loc.num_fragments);
    }
    if (first_nulls[0]) {
        bioseq_log<ERR, PG>( "bioseq storage Phase C: first sub-table row {} is NULL",
               loc.first_sub_row_no);
    }
    pgseq_t* first_raw = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(first_vals[0]));
    if (is_locator_stub(first_raw)) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: first sub-table row {} is itself a stub (corruption)",
               loc.first_sub_row_no);
    }

    const std::uint8_t logical_alphabet_code =
        static_cast<std::uint8_t>(first_raw->alphabet_code);
    const std::uint8_t logical_byte_width =
        static_cast<std::uint8_t>(first_raw->storage_unit_byte_width);
    if (is_seq_locator_stub_width(logical_byte_width)) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: fragment-row header carries the stub marker "
               "(sub_w={}) — corruption", static_cast<unsigned>(logical_byte_width));
    }

    const auto alpha_code = static_cast<pgseq_t::alphabet_code_t>(logical_alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha_code, logical_byte_width);
    if (lpu == 0) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: fetch_payload could not derive letters_per_unit "
               "(alphabet_code={}, byte_width={})",
               static_cast<unsigned>(alpha_code),
               static_cast<unsigned>(logical_byte_width));
    }
    const std::size_t bytes_per_unit = static_cast<std::size_t>(logical_byte_width);
    const std::size_t fragment_storage_bytes =
        detail::kFragmentStorageUnits * bytes_per_unit;

    // Compute the assembled varlena size and guard against MaxAllocSize.
    const sequence_size_t total_units =
        loc.total_bp_size == 0
            ? sequence_size_t{0}
            : (loc.total_bp_size + lpu - 1) / lpu;
    const sequence_size_t total_payload_bytes =
        static_cast<sequence_size_t>(total_units)
        * static_cast<sequence_size_t>(bytes_per_unit);
    const sequence_size_t assembled_varlena_bytes =
        static_cast<sequence_size_t>(pgseq_t::header_bytes())
        + total_payload_bytes;
    if (assembled_varlena_bytes > static_cast<sequence_size_t>(MaxAllocSize)) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: sequence too large to materialise as a "
               "single varlena ({} bp ⇒ ~{} bytes > MaxAllocSize). Stream via "
               "sequence_scanner_pg<S> or wait for Phase D D-VARLENA.",
               loc.total_bp_size,
               assembled_varlena_bytes);
    }

    MemoryContextScope ctx{out_mctx};
    pgseq_t* out = static_cast<pgseq_t*>(palloc0(static_cast<std::size_t>(assembled_varlena_bytes)));
    SET_VARSIZE(out, static_cast<std::size_t>(assembled_varlena_bytes));
    out->alphabet_code           = static_cast<pgseq_t::alphabet_code_t>(logical_alphabet_code);
    out->storage_unit_byte_width = logical_byte_width;
    out->bp_size                 = static_cast<pgseq_t::bp_size_t>(loc.total_bp_size);
    // Last-fragment padding is also the padding of the assembled varlena.

    std::uint8_t* const out_data = reinterpret_cast<std::uint8_t*>(out->data);

    // Copy the already-fetched first fragment.
    const std::size_t first_payload_bytes =
        static_cast<std::size_t>(VARSIZE(first_raw)) - pgseq_t::header_bytes();
    const std::size_t first_copy_bytes =
        std::min(first_payload_bytes,
                 static_cast<std::size_t>(total_payload_bytes));
    if (first_copy_bytes > 0) {
        std::memcpy(out_data, first_raw->data, first_copy_bytes);
    }
    if (loc.num_fragments > 0) {
        out->padding_letters = static_cast<std::uint16_t>(first_raw->padding_letters);
    }

    for (std::uint32_t fragment_no = 1; fragment_no < loc.num_fragments; ++fragment_no) {
        // DOC0276 — full-hydration of a multi-Gbp target into one varlena walks
        // tens of thousands of fragment rows. Make it cancellable and observable.
        CHECK_FOR_INTERRUPTS();
        if (enable_logging && (fragment_no & 0x3FFF) == 0) {
            bioseq_log_progress<DBG>("[fetch_payload] hydrated {}/{} fragments",
                            fragment_no, loc.num_fragments);
        }
        const std::uint64_t row_no = loc.first_sub_row_no + fragment_no;
        Datum vals[1]  = { 0 };
        bool  nulls[1] = { true };
        if (!BioseqColumnReadRowByRowNumber(read_state, row_no, vals, nulls)) {
            bioseq_log<ERR, PG>(
                   "bioseq storage Phase C: missing sub-table fragment row {} "
                   "(locator bucket={} shard={} fragment={}/{})",
                   row_no,
                   static_cast<unsigned>(loc.bucket_idx),
                   loc.shard_idx, fragment_no, loc.num_fragments);
        }
        if (nulls[0]) {
            bioseq_log<ERR, PG>( "bioseq storage Phase C: sub-table row {} is NULL", row_no);
        }
        pgseq_t* raw = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(vals[0]));
        if (is_locator_stub(raw)) {
            bioseq_log<ERR, PG>(
                   "bioseq storage Phase C: sub-table fragment row {} is itself a stub",
                   row_no);
        }
        const std::size_t dst_offset =
            static_cast<std::size_t>(fragment_no) * fragment_storage_bytes;
        const std::size_t raw_payload =
            static_cast<std::size_t>(VARSIZE(raw)) - pgseq_t::header_bytes();
        const std::size_t copy_bytes =
            std::min(raw_payload,
                     static_cast<std::size_t>(total_payload_bytes) - dst_offset);
        if (copy_bytes > 0) {
            std::memcpy(out_data + dst_offset, raw->data, copy_bytes);
        }
        if (fragment_no + 1 == loc.num_fragments) {
            out->padding_letters = static_cast<std::uint16_t>(raw->padding_letters);
        }
    }

    if (transient_read_state) {
        BioseqColumnEndRead(read_state);
    }
    return out;
}

// ---------------------------------------------------------------------------
// §7.7 — bulk fetch.
// ---------------------------------------------------------------------------

template<class L, std::size_t M, std::size_t X>
void
sequence_storage_manager<L, M, X>::fetch_payload_bulk(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, std::int16_t bucket_idx,
    std::span<const fetch_request> requests,
    std::span<pgseq_t*>            out_payloads,
    std::span<fetch_request>       sort_scratch,
    Snapshot                       snapshot,
    MemoryContext                  out_mctx,
    seq_caches*                    cache)
{
    (void)bucket_idx;  // The bucket is implicit in each locator; carried for symmetry.
    if (snapshot == nullptr) {
        bioseq_log<ERR, PG>(
            "bioseq storage: fetch_payload_bulk requires an explicit snapshot");
    }
    if (requests.empty()) {
        return;
    }
    if (sort_scratch.size() < requests.size()) {
        bioseq_log<ERR, PG>(
               "bioseq storage: fetch_payload_bulk sort_scratch too small "
               "(have {} need {})", sort_scratch.size(), requests.size());
    }
    std::copy(requests.begin(), requests.end(), sort_scratch.begin());

    std::sort(sort_scratch.begin(), sort_scratch.begin() + requests.size(),
        [](const fetch_request& a, const fetch_request& b) noexcept {
            return std::tie(a.locator.shard_idx, a.locator.first_sub_row_no)
                 < std::tie(b.locator.shard_idx, b.locator.first_sub_row_no);
        });

    for (std::size_t i = 0; i < requests.size(); ++i) {
        const fetch_request& req = sort_scratch[i];
        out_payloads[req.out_slot] = fetch_payload(
            db_oid, spc_oid, main_relfilenode,
            column_attno, req.locator, snapshot, out_mctx, cache);
    }
}

// ---------------------------------------------------------------------------
// §7.8 — DROP cascade.
// ---------------------------------------------------------------------------

template<class L, std::size_t M, std::size_t X>
void
sequence_storage_manager<L, M, X>::validate_drop_catalog_contracts()
{
    // Acquire both catalog locks in the same order as the mutating OAT_DROP
    // cleanup and retain them to transaction end. ALTER/DROP of either catalog
    // therefore cannot race between this preflight and PostgreSQL's relation
    // deletion. This function deliberately performs no row deletion, command
    // counter increment, relfilenode bookkeeping, or recursive utility call:
    // an ERROR here must leave every target in a multi-table DROP untouched.
    Relation registry_guard = fragment_registry_detail::open_catalog(
        /*allow_missing_during_extension_teardown=*/false);

    const Oid source_catalog_oid = source_version_detail::relation_oid();
    if (!OidIsValid(source_catalog_oid)) {
        internal_catalog_detail::missing_outside_teardown(
            "bioseq_internal.sequence_source_version");
    }
    Relation source_guard = try_safe_relation_open(
        source_catalog_oid, AccessShareLock);
    if (source_guard == nullptr) {
        internal_catalog_detail::missing_outside_teardown(
            "bioseq_internal.sequence_source_version");
    }
    source_version_detail::validate_catalog_schema(source_guard);

    // Release relcache pins but retain the AccessShare locks. OAT_DROP still
    // revalidates under its own locks as defence in depth.
    table_close(source_guard, NoLock);
    table_close(registry_guard, NoLock);
}

template<class L, std::size_t M, std::size_t X>
void
sequence_storage_manager<L, M, X>::drop_sub_tables_for_main(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    const bool allow_missing_during_extension_teardown)
{
    // Keep the validated registry relation locked across the legacy manager
    // calls below. Those calls historically return an empty set when the
    // relation disappears; the guard makes that benign only for the explicit
    // extension-teardown context and closes the validation-to-use race.
    Relation registry_guard = fragment_registry_detail::open_catalog(
        allow_missing_during_extension_teardown);
    if (registry_guard == nullptr) {
        return;
    }
    SequenceSourceVersionManager::delete_for_main(
        db_oid, spc_oid, main_relfilenode,
        allow_missing_during_extension_teardown);
    const auto rows = SubTableManager::list_shards_for_main(db_oid, spc_oid, main_relfilenode);
    if (rows.empty()) {
        table_close(registry_guard, AccessShareLock);
        return;
    }

    constexpr std::size_t kDropBatchSize = 32;
    StringInfo buf = makeStringInfo();
    for (std::size_t i = 0; i < rows.size(); i += kDropBatchSize) {
        resetStringInfo(buf);
        appendStringInfoString(buf, "DROP TABLE IF EXISTS ");
        const std::size_t end = std::min(i + kDropBatchSize, rows.size());
        bool wrote_one = false;
        for (std::size_t j = i; j < end; ++j) {
            if (wrote_one) appendStringInfoString(buf, ", ");
            const std::string sub_name = format_sub_table_name(
                rows[j].database_oid, rows[j].tablespace_oid,
                rows[j].main_relfilenode, rows[j].column_attno,
                rows[j].bucket_idx, rows[j].shard_idx);
            appendStringInfo(buf, "bioseq_internal.%s",
                             quote_identifier(sub_name.c_str()));
            wrote_one = true;
        }
        appendStringInfoString(buf, ";");
        if (SPI_connect() == SPI_OK_CONNECT) {
            SPI_execute(buf->data, false, 0);
            SPI_finish();
        }
    }

    for (std::size_t i = 0; i < rows.size(); i += kDropBatchSize) {
        resetStringInfo(buf);
        appendStringInfoString(buf, "DROP TABLE IF EXISTS ");
        const std::size_t end = std::min(i + kDropBatchSize, rows.size());
        bool wrote_one = false;
        for (std::size_t j = i; j < end; ++j) {
            if (wrote_one) appendStringInfoString(buf, ", ");
            const std::string sidecar_name = format_sidecar_name(
                rows[j].database_oid, rows[j].tablespace_oid,
                rows[j].main_relfilenode, rows[j].column_attno,
                rows[j].bucket_idx, rows[j].shard_idx);
            appendStringInfo(buf, "bioseq_internal.%s",
                             quote_identifier(sidecar_name.c_str()));
            wrote_one = true;
        }
        appendStringInfoString(buf, ";");
        if (SPI_connect() == SPI_OK_CONNECT) {
            SPI_execute(buf->data, false, 0);
            SPI_finish();
        }
    }

    SubTableManager::delete_shards_for_main(db_oid, spc_oid, main_relfilenode);
    table_close(registry_guard, AccessShareLock);
}

// Explicit instantiation of the default alias so non-template TUs see the
// out-of-line definitions.
template struct sequence_storage_manager<sequence_size_t, 8, 42>;

// ---------------------------------------------------------------------------
// §7.10 — Per-shard sidecar I/O.
// ---------------------------------------------------------------------------

static Oid lookup_sidecar_relid(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                std::int16_t column_attno,
                                std::int16_t bucket_idx, std::int32_t shard_idx)
{
    auto row = SubTableManager::lookup_shard(db_oid, spc_oid, main_relfilenode,
                                   column_attno, bucket_idx, shard_idx);
    if (!row.has_value()) {
        return InvalidOid;
    }
    return row->sidecar_relid;
}

void sidecar_write_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                        std::int16_t column_attno, const seq_locator& loc,
                        std::span<const std::uint8_t> blob, seq_caches* /*cache*/)
{
    const Oid sidecar_relid = lookup_sidecar_relid(
        db_oid, spc_oid, main_relfilenode, column_attno,
        static_cast<std::int16_t>(loc.bucket_idx),
        static_cast<std::int32_t>(loc.shard_idx));
    if (!OidIsValid(sidecar_relid)) {
        return;
    }
    Relation rel = try_safe_relation_open(sidecar_relid, RowExclusiveLock);
    if (rel == nullptr) {
        return;
    }
    bytea* blob_varlena = reinterpret_cast<bytea*>(
        palloc(VARHDRSZ + blob.size()));
    SET_VARSIZE(blob_varlena, VARHDRSZ + blob.size());
    if (!blob.empty()) {
        std::memcpy(VARDATA(blob_varlena), blob.data(), blob.size());
    }
    TupleDesc td = RelationGetDescr(rel);
    Datum values[2] = {
        Int64GetDatum(static_cast<std::int64_t>(loc.first_sub_row_no)),
        PointerGetDatum(blob_varlena),
    };
    bool nulls[2] = { false, false };
    HeapTuple tup = heap_form_tuple(td, values, nulls);
    CatalogTupleInsert(rel, tup);
    heap_freetuple(tup);
    pfree(blob_varlena);
    relation_close(rel, RowExclusiveLock);
}

std::optional<std::vector<char>>
sidecar_read_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const seq_locator& loc,
                  seq_caches* /*cache*/)
{
    const Oid sidecar_relid = lookup_sidecar_relid(
        db_oid, spc_oid, main_relfilenode, column_attno,
        static_cast<std::int16_t>(loc.bucket_idx),
        static_cast<std::int32_t>(loc.shard_idx));
    if (!OidIsValid(sidecar_relid)) {
        return std::nullopt;
    }
    Relation rel = try_safe_relation_open(sidecar_relid, AccessShareLock);
    if (rel == nullptr) {
        return std::nullopt;
    }
    const Oid pk_index_oid = find_primary_key_oid(rel);

    ScanKeyData keys[1];
    ScanKeyInit(&keys[0], 1 /*sub_rowno*/,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(loc.first_sub_row_no)));
    SysScanDesc scan = systable_beginscan(rel, pk_index_oid,
                                          /*indexOK=*/ pk_index_oid != InvalidOid,
                                          /*snapshot=*/nullptr, 1, keys);
    std::optional<std::vector<char>> out;
    HeapTuple tup = systable_getnext(scan);
    if (HeapTupleIsValid(tup)) {
        bool is_null = false;
        Datum d = heap_getattr(tup, 2 /*blob*/, RelationGetDescr(rel), &is_null);
        if (!is_null) {
            bytea* flat = reinterpret_cast<bytea*>(PG_DETOAST_DATUM(d));
            const std::size_t data_size = VARSIZE_ANY_EXHDR(flat);
            std::vector<char> v(data_size);
            if (data_size > 0) {
                std::memcpy(v.data(), VARDATA_ANY(flat), data_size);
            }
            out = std::move(v);
            if (reinterpret_cast<void*>(flat) !=
                reinterpret_cast<void*>(DatumGetPointer(d))) {
                pfree(flat);
            }
        }
    }
    systable_endscan(scan);
    relation_close(rel, AccessShareLock);
    return out;
}

void sidecar_read_blob_bulk(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, std::int16_t /*bucket_idx*/,
    std::span<const sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request> requests,
    std::span<std::optional<std::vector<char>>> out_blobs,
    std::span<sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request> sort_scratch,
    MemoryContext /*out_mctx*/,
    seq_caches* cache)
{
    if (requests.empty()) {
        return;
    }
    if (sort_scratch.size() < requests.size()) {
        bioseq_log<ERR, PG>(
               "bioseq storage: sidecar_read_blob_bulk sort_scratch too small "
               "(have {} need {})", sort_scratch.size(), requests.size());
    }
    std::copy(requests.begin(), requests.end(), sort_scratch.begin());
    using FR = sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request;
    std::sort(sort_scratch.begin(), sort_scratch.begin() + requests.size(),
        [](const FR& a, const FR& b) noexcept {
            return std::tie(a.locator.shard_idx, a.locator.first_sub_row_no)
                 < std::tie(b.locator.shard_idx, b.locator.first_sub_row_no);
        });
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const FR& req = sort_scratch[i];
        out_blobs[req.out_slot] = sidecar_read_blob(
            db_oid, spc_oid, main_relfilenode,
            column_attno, req.locator, cache);
    }
}

// ---------------------------------------------------------------------------
// DOC0377 -- transaction-safe locator source domains.
// ---------------------------------------------------------------------------

namespace pg_sequence_source_detail {
struct pg_sequence_source_domain;
}

struct pg_sequence_source_provider {
    pg_sequence_backend_thread_token backend_thread;
    Oid user_relid = InvalidOid;
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::uint64_t main_storage_id = 0;
    Snapshot authoritative_snapshot = nullptr;
    SubTransactionId creating_subxid = InvalidSubTransactionId;
    std::uint64_t generation = 0;
    pg_sequence_source_detail::pg_sequence_source_domain* domains = nullptr;
    pg_sequence_source_provider* next = nullptr;
    bool open = false;
};

namespace pg_sequence_source_detail {

struct pg_sequence_source_domain {
    pg_sequence_backend_thread_token backend_thread;
    pg_sequence_source_provider* provider = nullptr;
    pg_sequence_source_domain* next_provider = nullptr;
    pg_sequence_source_domain* next_global = nullptr;
    MemoryContext payload_context = nullptr;
    ResourceOwner resource_owner = nullptr;
    Snapshot registered_snapshot = nullptr;
    seq_caches* caches = nullptr;
    SequenceSourceVersionRow binding{};
    compact_sequence_stdc_type_params stored_type{};
    std::uint16_t last_padding_letters = 0;
    SubTransactionId creating_subxid = InvalidSubTransactionId;
    std::uint64_t generation = 0;
    std::uint32_t refcount = 0;
    bool registry_reference = false;
    bool valid = false;
};

struct source_read_entry {
    pg_sequence_source_domain* domain = nullptr;
    MemoryContext payload_context = nullptr;
};

inline pg_sequence_source_provider* providers = nullptr;
inline pg_sequence_source_domain* domains = nullptr;
inline std::uint64_t next_generation = 1;
inline bool xact_callback_registered = false;
inline bool subxact_callback_registered = false;

[[nodiscard]] inline bool same_locator(
        const seq_locator& left, const seq_locator& right) noexcept {
    return std::memcmp(&left, &right, sizeof(seq_locator)) == 0;
}

[[nodiscard]] inline std::uint64_t allocate_generation() {
    if (next_generation == 0
        || next_generation == std::numeric_limits<std::uint64_t>::max()) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq locator source generation space exhausted")));
    }
    return next_generation++;
}

inline void retain_domain(void* opaque) noexcept {
    auto* domain = static_cast<pg_sequence_source_domain*>(opaque);
    if (domain != nullptr && domain->refcount != UINT32_MAX) {
        ++domain->refcount;
    }
}

inline void release_domain(void* opaque) noexcept {
    auto* domain = static_cast<pg_sequence_source_domain*>(opaque);
    if (domain != nullptr && domain->refcount != 0) {
        --domain->refcount;
    }
    // Cleanup is deferred to a provider/xact sweep.  Arena reset may call this
    // function, so it must remain a pure backend-local decrement.
}

[[nodiscard]] inline pg_sequence_source_identity domain_identity(
        const void* opaque) noexcept {
    auto const* domain = static_cast<pg_sequence_source_domain const*>(opaque);
    return domain == nullptr
        ? pg_sequence_source_identity{}
        : pg_sequence_source_identity{
            .control_address = reinterpret_cast<std::uintptr_t>(domain),
            .generation = domain->generation,
        };
}

[[nodiscard]] inline bool domain_worker_shareable(const void*) noexcept {
    return false;
}

inline void remove_domain_from_global(
        pg_sequence_source_domain* victim) noexcept {
    auto** link = &domains;
    while (*link != nullptr) {
        if (*link == victim) {
            *link = victim->next_global;
            victim->next_global = nullptr;
            return;
        }
        link = &(*link)->next_global;
    }
}

inline void destroy_domain_control(
        pg_sequence_source_domain* domain) noexcept {
    if (domain == nullptr) return;
    remove_domain_from_global(domain);
    domain->~pg_sequence_source_domain();
    pfree(domain);
}

inline void invalidate_domain(pg_sequence_source_domain* domain,
                              bool is_commit) {
    if (domain == nullptr || !domain->valid) return;
    domain->valid = false;

    if (domain->caches != nullptr) {
        close_and_destroy_seq_caches(domain->caches, is_commit);
        domain->caches = nullptr;
    }
    if (domain->registered_snapshot != nullptr
        && domain->resource_owner != nullptr) {
        const ResourceOwner saved_owner = CurrentResourceOwner;
        CurrentResourceOwner = domain->resource_owner;
        UnregisterSnapshotFromOwner(
            domain->registered_snapshot, domain->resource_owner);
        CurrentResourceOwner = saved_owner;
        domain->registered_snapshot = nullptr;
    }
    if (domain->resource_owner != nullptr) {
        ResourceOwnerDelete(domain->resource_owner);
        domain->resource_owner = nullptr;
    }
    if (domain->payload_context != nullptr) {
        MemoryContext payload = domain->payload_context;
        domain->payload_context = nullptr;
        MemoryContextDelete(payload);
    }
}

inline void sweep_dead_domains() noexcept {
    auto* domain = domains;
    while (domain != nullptr) {
        auto* next = domain->next_global;
        if (!domain->valid && domain->refcount == 0) {
            destroy_domain_control(domain);
        }
        domain = next;
    }
}

inline void invalidate_unreferenced_domains(bool is_commit) {
    for (auto* domain = domains; domain != nullptr;
         domain = domain->next_global) {
        if (domain->valid && domain->refcount == 0) {
            invalidate_domain(domain, is_commit);
        }
    }
}

inline void remove_provider_from_global(
        pg_sequence_source_provider* victim) noexcept {
    auto** link = &providers;
    while (*link != nullptr) {
        if (*link == victim) {
            *link = victim->next;
            victim->next = nullptr;
            return;
        }
        link = &(*link)->next;
    }
}

inline void release_provider_registry(
        pg_sequence_source_provider* provider) noexcept {
    auto* domain = provider->domains;
    provider->domains = nullptr;
    while (domain != nullptr) {
        auto* next = domain->next_provider;
        domain->next_provider = nullptr;
        domain->provider = nullptr;
        if (domain->registry_reference) {
            domain->registry_reference = false;
            release_domain(domain);
        }
        domain = next;
    }
}

inline void destroy_provider_control(
        pg_sequence_source_provider* provider) noexcept {
    if (provider == nullptr) return;
    remove_provider_from_global(provider);
    provider->~pg_sequence_source_provider();
    pfree(provider);
}

void xact_callback(XactEvent event, void*);
void subxact_callback(SubXactEvent event, SubTransactionId my_subid,
                      SubTransactionId parent_subid, void*);

inline void ensure_callbacks() {
    if (!xact_callback_registered) {
        RegisterXactCallback(xact_callback, nullptr);
        xact_callback_registered = true;
    }
    if (!subxact_callback_registered) {
        RegisterSubXactCallback(subxact_callback, nullptr);
        subxact_callback_registered = true;
    }
}

[[nodiscard]] inline bool domain_matches(
        const pg_sequence_source_domain& domain,
        AttrNumber column_attno,
        const seq_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        std::uint16_t last_padding_letters) noexcept {
    return domain.valid
        && domain.binding.column_attno == column_attno
        && same_locator(domain.binding.locator, locator)
        && domain.stored_type == stored_type
        && domain.last_padding_letters == last_padding_letters;
}

[[nodiscard]] inline bool binding_matches(
        const pg_sequence_source_provider& provider,
        AttrNumber column_attno,
        const seq_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        std::uint16_t last_padding_letters,
        const SequenceSourceVersionRow& row) noexcept {
    return row.state == kSequenceSourceStateReady
        && row.source_epoch == locator.source_epoch
        && row.user_relid == provider.user_relid
        && row.database_oid == provider.database_oid
        && row.tablespace_oid == provider.tablespace_oid
        && row.main_relfilenode == provider.main_relfilenode
        && row.main_storage_id == provider.main_storage_id
        && row.column_attno == column_attno
        && same_locator(row.locator, locator)
        && row.alphabet_code == stored_type.alphabet_code
        && row.storage_unit_byte_width == stored_type.storage_unit_byte_width
        && row.padding_letters == last_padding_letters;
}

[[nodiscard]] pg_sequence_source_domain* create_domain(
        pg_sequence_source_provider& provider,
        AttrNumber column_attno,
        const seq_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        std::uint16_t last_padding_letters) {
    const auto source_row = bioseq::pg_utils::invoke_pg_as_cpp([
            source_epoch = locator.source_epoch,
            authoritative_snapshot = provider.authoritative_snapshot] {
        return SequenceSourceVersionManager::lookup(
            source_epoch, authoritative_snapshot,
            /*require_ready=*/true);
    });
    if (!source_row.has_value()
        || !binding_matches(provider, column_attno, locator, stored_type,
                            last_padding_letters, *source_row)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq locator no longer matches its immutable source"),
             errdetail("relation, column, snapshot, storage identity, source epoch, or type metadata changed")));
    }

    auto* domain = static_cast<pg_sequence_source_domain*>(
        MemoryContextAllocZero(TopMemoryContext,
                               sizeof(pg_sequence_source_domain)));
    ::new (static_cast<void*>(domain)) pg_sequence_source_domain{};
    try {
        domain->provider = &provider;
        domain->binding = *source_row;
        domain->stored_type = stored_type;
        domain->last_padding_letters = last_padding_letters;
        domain->creating_subxid = GetCurrentSubTransactionId();
        domain->generation = allocate_generation();
        domain->refcount = 1;
        domain->registry_reference = true;
        domain->backend_thread = pg_sequence_backend_thread_token::capture();
        domain->payload_context = AllocSetContextCreate(
            TopTransactionContext, "bioseq locator source domain",
            ALLOCSET_DEFAULT_SIZES);
        domain->resource_owner = ResourceOwnerCreate(
            TopTransactionResourceOwner, "bioseq locator source domain");
        domain->registered_snapshot = RegisterSnapshotOnOwner(
            provider.authoritative_snapshot, domain->resource_owner);
        domain->caches = alloc_seq_caches(domain->payload_context);
        domain->caches->side = seq_caches::Side::Read;
        domain->caches->lock_mode = AccessShareLock;
        domain->caches->read_state_mctx = domain->payload_context;
        domain->caches->read_resource_owner = domain->resource_owner;
        domain->caches->main_database_oid = provider.database_oid;
        domain->caches->main_tablespace_oid = provider.tablespace_oid;
        domain->caches->main_relfilenode = provider.main_relfilenode;
        domain->valid = true;
    } catch (...) {
        if (domain->caches != nullptr) {
            close_and_destroy_seq_caches(domain->caches, false);
            domain->caches = nullptr;
        }
        if (domain->registered_snapshot != nullptr
            && domain->resource_owner != nullptr) {
            UnregisterSnapshotFromOwner(
                domain->registered_snapshot, domain->resource_owner);
        }
        if (domain->resource_owner != nullptr) {
            ResourceOwnerDelete(domain->resource_owner);
        }
        if (domain->payload_context != nullptr) {
            MemoryContextDelete(domain->payload_context);
        }
        domain->~pg_sequence_source_domain();
        pfree(domain);
        throw;
    }

    domain->next_provider = provider.domains;
    provider.domains = domain;
    domain->next_global = domains;
    domains = domain;
    return domain;
}

[[nodiscard]] inline pg_sequence_source_domain* find_or_create_domain(
        pg_sequence_source_provider& provider,
        AttrNumber column_attno,
        const seq_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        std::uint16_t last_padding_letters) {
    for (auto* domain = provider.domains; domain != nullptr;
         domain = domain->next_provider) {
        if (domain_matches(*domain, column_attno, locator, stored_type,
                           last_padding_letters)) {
            return domain;
        }
    }
    return create_domain(provider, column_attno, locator, stored_type,
                         last_padding_letters);
}

[[nodiscard]] void* create_read_entry(
        void* factory_argument, MemoryContext payload_context) {
    auto* domain = static_cast<pg_sequence_source_domain*>(factory_argument);
    auto* entry = static_cast<source_read_entry*>(
        MemoryContextAllocZero(payload_context, sizeof(source_read_entry)));
    entry->domain = domain;
    entry->payload_context = payload_context;
    retain_domain(domain);
    return entry;
}

inline void finish_read_entry(void* opaque, bool) {
    auto* entry = static_cast<source_read_entry*>(opaque);
    if (entry != nullptr) {
        release_domain(entry->domain);
        entry->domain = nullptr;
    }
}

inline void abort_read_entry(void* opaque) noexcept {
    auto* entry = static_cast<source_read_entry*>(opaque);
    if (entry != nullptr) {
        release_domain(entry->domain);
        entry->domain = nullptr;
    }
}

[[nodiscard]] inline pgseq_t* read_fragment(
        source_read_entry& reader,
        std::uint32_t fragment_no) {
    auto& domain = *reader.domain;
    const auto& locator = domain.binding.locator;
    if (fragment_no >= locator.num_fragments) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source fragment number is outside its locator")));
    }

    Relation shard = bioseq::pg_utils::invoke_pg_as_cpp([
            resource_owner = domain.resource_owner,
            database_oid = domain.binding.database_oid,
            tablespace_oid = domain.binding.tablespace_oid,
            main_relfilenode = domain.binding.main_relfilenode,
            column_attno = domain.binding.column_attno,
            locator,
            caches = domain.caches] {
        const ResourceOwner saved_owner = CurrentResourceOwner;
        CurrentResourceOwner = resource_owner;
        Relation result = open_sub_table_for_locator(
            database_oid, tablespace_oid, main_relfilenode, column_attno,
            locator, AccessShareLock, caches);
        CurrentResourceOwner = saved_owner;
        return result;
    });
    if (RelationGetRelid(shard) != domain.binding.fragment_relid
        || bioseq::pg_utils::invoke_pg_as_cpp([shard] {
               return BioseqColumnStorageGetStorageId(shard, false);
           }) != domain.binding.fragment_storage_id) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq locator fragment storage identity changed")));
    }

    const std::uint64_t cache_key = seq_caches::key_for(
        domain.binding.column_attno,
        static_cast<std::int16_t>(locator.bucket_idx),
        static_cast<std::int32_t>(locator.shard_idx));
    seq_caches::entry& cache_entry = domain.caches->entries[cache_key];
    cache_entry.rel = shard;
    if (cache_entry.read_state == nullptr) {
        cache_entry.read_state = bioseq::pg_utils::invoke_pg_as_cpp([
                resource_owner = domain.resource_owner,
                shard,
                payload_context = domain.payload_context,
                registered_snapshot = domain.registered_snapshot] {
            const ResourceOwner saved_owner = CurrentResourceOwner;
            CurrentResourceOwner = resource_owner;
            BioseqColumnReadState* result = begin_sequence_payload_read(
                shard, payload_context, registered_snapshot);
            CurrentResourceOwner = saved_owner;
            return result;
        });
        cache_entry.read_snapshot_source = domain.registered_snapshot;
    } else if (cache_entry.read_snapshot_source
               != domain.registered_snapshot) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source reader snapshot identity changed")));
    }

    Datum values[1] = {0};
    bool nulls[1] = {true};
    const std::uint64_t row_no = locator.first_sub_row_no + fragment_no;
    const bool visible = bioseq::pg_utils::invoke_pg_as_cpp([
            payload_context = reader.payload_context,
            resource_owner = domain.resource_owner,
            read_state = cache_entry.read_state,
            row_no,
            &values,
            &nulls] {
        const MemoryContext saved_context = CurrentMemoryContext;
        const ResourceOwner saved_owner = CurrentResourceOwner;
        MemoryContextSwitchTo(payload_context);
        CurrentResourceOwner = resource_owner;
        const bool result = BioseqColumnReadRowByRowNumber(
            read_state, row_no, values, nulls);
        CurrentResourceOwner = saved_owner;
        MemoryContextSwitchTo(saved_context);
        return result;
    });
    if (!visible || nulls[0]) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source fragment row is missing or null"),
             errdetail("row=%llu fragment=%u epoch=%llu",
                static_cast<unsigned long long>(row_no), fragment_no,
                static_cast<unsigned long long>(locator.source_epoch))));
    }
    auto* raw = bioseq::pg_utils::invoke_pg_as_cpp([
            payload_context = reader.payload_context,
            fragment_datum = values[0]] {
        const MemoryContext saved_context = CurrentMemoryContext;
        MemoryContextSwitchTo(payload_context);
        auto* result = reinterpret_cast<pgseq_t*>(
            PG_DETOAST_DATUM(fragment_datum));
        MemoryContextSwitchTo(saved_context);
        return result;
    });
    if (VARSIZE(raw) < pgseq_t::header_bytes() || is_locator_stub(raw)
        || static_cast<std::uint8_t>(raw->alphabet_code)
            != domain.stored_type.alphabet_code
        || static_cast<std::uint8_t>(raw->storage_unit_byte_width)
            != domain.stored_type.storage_unit_byte_width) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source fragment header does not match its binding")));
    }
    return raw;
}

[[nodiscard]] pg_sequence_source_read_status read_domain_ranks(
        const void* opaque,
        pg_sequence_read_session& session,
        const seq_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        sequence_transform transform,
        std::uint64_t source_from,
        std::span<std::uint8_t> output) {
    auto* domain = const_cast<pg_sequence_source_domain*>(
        static_cast<pg_sequence_source_domain const*>(opaque));
    if (domain == nullptr || !domain->backend_thread.is_current()
        || !session.backend_thread.is_current()) {
        return pg_sequence_source_read_status::wrong_thread;
    }
    if (!domain->valid || domain->registered_snapshot == nullptr
        || domain->resource_owner == nullptr || domain->caches == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq locator source is no longer live"),
             errhint("materialize locator-backed values before the transaction ends")));
    }
    if (!same_locator(domain->binding.locator, locator)
        || domain->stored_type != stored_type
        || source_from > locator.total_bp_size
        || output.size() > locator.total_bp_size - source_from) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source read request does not match its binding")));
    }
    if (output.empty()) return pg_sequence_source_read_status::ok;

    const pg_sequence_read_entry_key key{
        .source = domain_identity(domain),
        .locator = locator,
        .stored_type_params = stored_type,
    };
    auto* reader = static_cast<source_read_entry*>(
        find_or_create_pg_sequence_read_entry(
            session, key, domain, create_read_entry,
            finish_read_entry, abort_read_entry));
    if (reader == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source read session is not accepting readers")));
    }

    bool dispatched = false;
    pgseq_t::dispatch_instance(
        stored_type.alphabet_code,
        stored_type.storage_unit_byte_width,
        [transform, locator, domain, reader, source_from, output,
         &dispatched]<class Instance>() {
            using range = compact_sequence_range<Instance>;
            using traits = typename range::traits;
            using unit = typename traits::storage_unit;
            constexpr std::uint64_t letters_per_fragment =
                static_cast<std::uint64_t>(kSequenceFragmentStorageUnits)
                * traits::letters_per_unit;
            const bool reverse = has_reverse(transform);
            const bool complement = has_complement(transform);
            std::uint32_t resident_fragment = UINT32_MAX;
            pgseq_t* raw = nullptr;

            for (std::size_t output_index = 0;
                 output_index < output.size(); ++output_index) {
                const std::uint64_t physical = source_from
                    + (reverse ? output.size() - 1u - output_index
                               : output_index);
                const auto fragment_no = static_cast<std::uint32_t>(
                    physical / letters_per_fragment);
                if (fragment_no != resident_fragment) {
                    raw = read_fragment(*reader, fragment_no);
                    resident_fragment = fragment_no;
                    const bool is_last =
                        fragment_no + 1u == locator.num_fragments;
                    const auto padding = static_cast<std::uint16_t>(
                        raw->padding_letters);
                    if ((!is_last && padding != 0u)
                        || (is_last
                            && padding != domain->last_padding_letters)) {
                        ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq source fragment padding does not match its binding")));
                    }
                }

                const std::uint64_t local_symbol =
                    physical % letters_per_fragment;
                const std::uint64_t unit_index =
                    local_symbol / traits::letters_per_unit;
                const std::size_t byte_offset =
                    static_cast<std::size_t>(unit_index) * sizeof(unit);
                const std::size_t payload_bytes =
                    static_cast<std::size_t>(VARSIZE(raw))
                    - pgseq_t::header_bytes();
                if (byte_offset > payload_bytes
                    || sizeof(unit) > payload_bytes - byte_offset) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq source fragment payload is truncated")));
                }
                unit word{};
                std::memcpy(&word,
                    reinterpret_cast<std::byte const*>(raw->data)
                        + byte_offset,
                    sizeof(unit));
                const std::size_t shift = static_cast<std::size_t>(
                    local_symbol % traits::letters_per_unit)
                    * traits::bits_per_char;
                std::uint8_t rank = static_cast<std::uint8_t>(
                    (word >> shift) & traits::char_mask);
                if (rank >= seqan3::alphabet_size<
                                typename Instance::alphabet_type>) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq source fragment contains an invalid alphabet rank")));
                }
                if (complement) {
                    rank = static_cast<std::uint8_t>(
                        traits::letter_complement(static_cast<unit>(rank)));
                }
                output[output_index] = rank;
            }
            dispatched = true;
        });
    if (!dispatched) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("bioseq source binding uses an unsupported sequence type")));
    }
    return pg_sequence_source_read_status::ok;
}

inline constexpr pg_sequence_source_backend_ops source_ops{
    .retain = retain_domain,
    .release = release_domain,
    .identity = domain_identity,
    .worker_shareable = domain_worker_shareable,
    .read_ranks = read_domain_ranks,
};

inline void close_provider_impl(
        pg_sequence_source_provider* provider,
        bool destroy_control) noexcept {
    if (provider == nullptr) return;
    if (provider->open) {
        provider->open = false;
        provider->authoritative_snapshot = nullptr;
        release_provider_registry(provider);
    }
    if (destroy_control) {
        destroy_provider_control(provider);
    }
}

void xact_callback(XactEvent event, void*) {
    const bool terminal_commit = event == XACT_EVENT_PRE_COMMIT
        || event == XACT_EVENT_PARALLEL_PRE_COMMIT
        || event == XACT_EVENT_PRE_PREPARE;
    const bool terminal_abort = event == XACT_EVENT_ABORT
        || event == XACT_EVENT_PARALLEL_ABORT;
    if (!terminal_commit && !terminal_abort) return;

    while (providers != nullptr) {
        close_provider_impl(providers, /*destroy_control=*/true);
    }
    for (auto* domain = domains; domain != nullptr;
         domain = domain->next_global) {
        invalidate_domain(domain, terminal_commit);
    }
    sweep_dead_domains();
}

void subxact_callback(SubXactEvent event, SubTransactionId my_subid,
                      SubTransactionId parent_subid, void*) {
    if (event == SUBXACT_EVENT_COMMIT_SUB) {
        for (auto* provider = providers; provider != nullptr;
             provider = provider->next) {
            if (provider->creating_subxid == my_subid) {
                provider->creating_subxid = parent_subid;
            }
        }
        for (auto* domain = domains; domain != nullptr;
             domain = domain->next_global) {
            if (domain->creating_subxid == my_subid) {
                domain->creating_subxid = parent_subid;
            }
        }
        return;
    }
    if (event != SUBXACT_EVENT_ABORT_SUB) return;

    auto* provider = providers;
    while (provider != nullptr) {
        auto* next = provider->next;
        if (provider->creating_subxid == my_subid) {
            close_provider_impl(provider, /*destroy_control=*/false);
        }
        provider = next;
    }
    for (auto* domain = domains; domain != nullptr;
         domain = domain->next_global) {
        if (domain->creating_subxid == my_subid) {
            invalidate_domain(domain, false);
        }
    }
    sweep_dead_domains();
}

} // namespace pg_sequence_source_detail

pg_sequence_source_provider* create_pg_sequence_source_provider(
        Relation owning_relation,
        Snapshot authoritative_registered_snapshot) {
    using namespace pg_sequence_source_detail;
    if (owning_relation == nullptr
        || authoritative_registered_snapshot == nullptr
        || !IsMVCCSnapshot(authoritative_registered_snapshot)
        || authoritative_registered_snapshot->regd_count == 0) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq locator source provider requires an owning relation and registered MVCC snapshot")));
    }
    ensure_callbacks();
    sweep_dead_domains();

    const std::uint64_t storage_id =
        bioseq::pg_utils::invoke_pg_as_cpp([owning_relation] {
            return BioseqColumnStorageGetStorageId(
                owning_relation, false);
        });
    if (storage_id == 0) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq locator source relation has no physical storage identity")));
    }

    auto* provider = static_cast<pg_sequence_source_provider*>(
        MemoryContextAllocZero(TopMemoryContext,
                               sizeof(pg_sequence_source_provider)));
    ::new (static_cast<void*>(provider)) pg_sequence_source_provider{};
    provider->backend_thread = pg_sequence_backend_thread_token::capture();
    provider->user_relid = RelationGetRelid(owning_relation);
    provider->database_oid = owning_relation->rd_locator.dbOid;
    provider->tablespace_oid = owning_relation->rd_locator.spcOid;
    provider->main_relfilenode = owning_relation->rd_locator.relNumber;
    provider->main_storage_id = storage_id;
    provider->authoritative_snapshot = authoritative_registered_snapshot;
    provider->creating_subxid = GetCurrentSubTransactionId();
    provider->generation = allocate_generation();
    provider->open = true;
    provider->next = providers;
    providers = provider;
    return provider;
}

void close_pg_sequence_source_provider(
        pg_sequence_source_provider* provider,
        bool /*is_commit*/) noexcept {
    using namespace pg_sequence_source_detail;
    if (provider == nullptr) return;
    if (!provider->backend_thread.is_current()) {
        return;
    }
    close_provider_impl(provider, /*destroy_control=*/false);
    invalidate_unreferenced_domains(/*is_commit=*/false);
    sweep_dead_domains();
}

Datum promote_pg_sequence_locator(
        pg_sequence_source_provider* provider,
        AttrNumber column_attno,
        Datum locator_datum,
        MemoryContext result_context,
        slice_trace_attachment* trace) {
    using namespace pg_sequence_source_detail;
    if (provider == nullptr || !provider->open
        || !provider->backend_thread.is_current()
        || result_context == nullptr || column_attno <= 0) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq locator promotion has no live owning scan")));
    }

    auto* header = bioseq::pg_utils::invoke_pg_as_cpp([locator_datum] {
        return reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(locator_datum));
    });
    const pg_sequence_descriptor descriptor =
        classify_and_describe_sequence(PointerGetDatum(header));
    if (descriptor.kind != pg_sequence_datum_kind::locator) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("bioseq source promotion requires a locator stub")));
    }
    const seq_locator locator = decode_locator_from_stub(header);
    const compact_sequence_stdc_type_params stored_type{
        .alphabet_code = descriptor.alphabet_code,
        .storage_unit_byte_width = descriptor.storage_unit_byte_width,
    };
    const auto last_padding_letters = static_cast<std::uint16_t>(
        header->padding_letters);
    auto* domain = find_or_create_domain(
        *provider, column_attno, locator, stored_type,
        last_padding_letters);
    const pg_sequence_source_handle handle{
        .state = domain,
        .ops = &source_ops,
    };
    return bioseq::pg_utils::invoke_pg_as_cpp([
            result_context, handle, locator, stored_type,
            last_padding_letters, trace] {
        const MemoryContext saved_context = CurrentMemoryContext;
        MemoryContextSwitchTo(result_context);
        Datum result = make_source_backed_pg_sequence(
            handle, locator, stored_type, last_padding_letters, trace);
        MemoryContextSwitchTo(saved_context);
        return result;
    });
}



// ---------------------------------------------------------------------------
// §11.3 — Orphan reaper.
// ---------------------------------------------------------------------------

void ReapOrphanSeqSubTables() {
    // Two-direction sweep: catalog rows pointing at missing pg_class entries,
    // and pg_class entries matching our naming convention with no catalog row.
    if (SPI_connect() != SPI_OK_CONNECT) {
        return;
    }
    const char* invalidate_orphan_sources =
        "UPDATE bioseq_internal.sequence_source_version v SET state = 2 "
        "WHERE v.state = 1 AND v.num_fragments > 0 "
        "AND NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = v.fragment_relid);";
    SPI_execute(invalidate_orphan_sources, /*read_only=*/false, /*tcount=*/0);
    const char* del_orphan_rows =
        "DELETE FROM bioseq_internal.seq_sub_table s "
        "WHERE s.database_oid = $1 "
        "  AND NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = s.sub_table_relid);";
    Oid arg_types[1] = { OIDOID };
    Datum arg_values[1] = { ObjectIdGetDatum(MyDatabaseId) };
    SPI_execute_with_args(del_orphan_rows, 1, arg_types, arg_values,
                           /*Nulls=*/nullptr,
                           /*read_only=*/false, /*tcount=*/0);

    // pg_class entries with no catalog row — drop them.
    const char* find_orphan_relations =
        "SELECT n.nspname || '.' || quote_ident(c.relname) "
        "FROM pg_class c JOIN pg_namespace n ON c.relnamespace = n.oid "
        "WHERE n.nspname = 'bioseq_internal' "
        "  AND (c.relname LIKE 'seq_sub_%' OR c.relname LIKE 'idx_sidecar_%') "
        "  AND NOT EXISTS ("
        "      SELECT 1 FROM bioseq_internal.seq_sub_table s "
        "      WHERE s.sub_table_relid = c.oid OR s.sidecar_relid = c.oid);";
    if (SPI_execute(find_orphan_relations, /*read_only=*/true, /*tcount=*/0)
        == SPI_OK_SELECT)
    {
        const uint64_t n = SPI_processed;
        std::vector<std::string> to_drop;
        to_drop.reserve(n);
        for (uint64_t r = 0; r < n; ++r) {
            bool is_null = false;
            Datum d = SPI_getbinval(SPI_tuptable->vals[r],
                                    SPI_tuptable->tupdesc, 1, &is_null);
            if (is_null) continue;
            text* t = DatumGetTextPP(d);
            to_drop.emplace_back(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
        }
        for (const auto& q : to_drop) {
            StringInfo drop_stmt = makeStringInfo();
            appendStringInfo(drop_stmt, "DROP TABLE IF EXISTS %s;", q.c_str());
            SPI_execute(drop_stmt->data, /*read_only=*/false, /*tcount=*/0);
        }
    }
    SPI_finish();
}

}  // namespace bioseq::storage
