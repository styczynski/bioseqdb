/**
 * @file catalog.cppm
 * @brief Sequence source catalog and publication authority.
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

export module bioseq.postgres.storage.manager:sequence_storage_catalog;

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
// The PG-free bucket math and sequence_locator value live in their own
// module so the unit-test TU can link them without the Postgres backend.
// Re-exported here so existing `import bioseq_storage` consumers are unchanged.
import bioseq.storage.layout;

namespace storage_manager_entity_handles =
    bioseq::sql::generated::core::
        storage_manager_sequence_storage_manager::entity_handles;
namespace storage_manager_catalog_rows =
    bioseq::storage::catalog_rows;
namespace storage_manager_row_codecs =
    bioseq::postgres::storage::manager::sql_row_codecs;
using sequence_source_version_catalog_row =
    storage_manager_catalog_rows::sequence_source_version;
using sequence_source_version_codec =
    storage_manager_row_codecs::sequence_source_version_codec;
using minimizer_generation_storage_catalog_row =
    storage_manager_catalog_rows::minimizer_generation_storage;
using minimizer_generation_storage_codec =
    storage_manager_row_codecs::minimizer_generation_storage_codec;
using minimizer_generation_reaper_catalog_row =
    storage_manager_catalog_rows::minimizer_generation_reaper_state;
using minimizer_generation_reaper_codec =
    storage_manager_row_codecs::minimizer_generation_reaper_state_codec;

static_assert(
    storage_manager_entity_handles::sequence_source_epoch_seq
        .schema_binding()
    == bioseq::postgres::support::sql_schema_binding::fixed);

export namespace bioseq::storage {

#if defined(BIOSEQ_TEST_INDEXES)
namespace orphan_reaper_test_support {

inline thread_local std::optional<std::int64_t>
    drop_barrier_key;

/** Arm a one-shot backend-local barrier before the next orphan DROP TABLE. */
inline void arm_drop_barrier(std::int64_t key) noexcept {
    drop_barrier_key = key;
}

/// Clear the backend-local orphan-drop barrier without waiting.
inline void disarm_drop_barrier() noexcept {
    drop_barrier_key.reset();
}

/** Wait for the controller's advisory lock, then release our acquired lock. */
inline void pass_drop_barrier() {
    if (!drop_barrier_key.has_value())
        return;
    const std::int64_t key = *drop_barrier_key;
    drop_barrier_key.reset();
    bioseq::postgres::support::invoke_pg_as_cpp([key] {
        static_cast<void>(DirectFunctionCall1(
            pg_advisory_lock_int8,
            Int64GetDatum(key)));
        static_cast<void>(DirectFunctionCall1(
            pg_advisory_unlock_int8,
            Int64GetDatum(key)));
    });
}

}  // namespace orphan_reaper_test_support
#endif

namespace internal_catalog_detail {

/// Check whether the Bioseq extension is present in this database.
[[nodiscard]] inline bool bioseq_extension_installed() {
    return OidIsValid(get_extension_oid("bioseq", true));
}

/// Permit missing internal objects only during explicit extension teardown.
[[nodiscard]] inline bool absence_is_benign(
        const bool allow_missing_during_extension_teardown) {
    return allow_missing_during_extension_teardown;
}

/// Reject a missing extension-owned object outside teardown.
[[noreturn]] inline void missing_outside_teardown(
        const std::string_view object_name) {
    const std::string owned{object_name};
    bioseq::postgres::support::invoke_pg_as_cpp([&owned] {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq internal catalog %s is missing outside extension teardown",
                    owned.c_str())));
    });
    pg_unreachable();
}

} // namespace internal_catalog_detail

/// Persistent states for the immutable sequence-source catalog.
inline constexpr std::int16_t k_sequence_source_state_unpublished = 0;
inline constexpr std::int16_t k_sequence_source_state_ready = 1;
inline constexpr std::int16_t k_sequence_source_state_invalid = 2;

/// Raise a storage error through a checked PostgreSQL leaf.
[[noreturn]] inline void storage_error_as_cpp(
        int sqlstate, std::string message) {
    bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, &message] {
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
    return (current == k_sequence_source_state_unpublished
            && (requested == k_sequence_source_state_ready
                || requested == k_sequence_source_state_invalid))
        || (current == k_sequence_source_state_ready
            && requested == k_sequence_source_state_invalid);
}

static_assert(sequence_source_state_transition_allowed(
    k_sequence_source_state_unpublished, k_sequence_source_state_ready));
static_assert(sequence_source_state_transition_allowed(
    k_sequence_source_state_ready, k_sequence_source_state_invalid));
static_assert(!sequence_source_state_transition_allowed(
    k_sequence_source_state_invalid, k_sequence_source_state_ready));

/// Exact PostgreSQL-owned identity record for one immutable sequence source.
/// The checksum is the CRC-64/ECMA-182 of decoded logical symbols, calculated
/// while the source is already streaming into fragment storage.
struct sequence_source_version_row {
    std::uint64_t source_epoch = 0;
    Oid user_relid = InvalidOid;
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::uint64_t main_storage_id = 0;
    std::int16_t column_attno = 0;
    sequence_locator locator{};
    Oid fragment_relid = InvalidOid;
    std::uint64_t fragment_storage_id = 0;
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t padding_letters = 0;
    std::uint64_t source_content_checksum = 0;
    std::int16_t state = k_sequence_source_state_unpublished;
};

/// Optional observational counters for one immutable source-catalog lookup.
/// The lookup owns the only truthful view of its relation/index open sites;
/// callers that do not request telemetry retain the exact previous path.
struct sequence_source_lookup_telemetry {
    std::uint64_t heap_opens = 0;
    std::uint64_t index_opens = 0;
    std::uint64_t schema_validations = 0;
    std::uint64_t lock_acquires = 0;
    std::uint64_t lock_releases = 0;
};

namespace source_version_detail {

template<auto p_member>
inline constexpr AttrNumber source_attribute =
    sequence_source_version_codec::attribute_number<p_member>();

/// Fixed source-epoch attribute number from the generated row codec.
inline constexpr AttrNumber source_epoch =
    source_attribute<&sequence_source_version_catalog_row::source_epoch>;

/// Resolve the extension-owned internal namespace without creating it.
[[nodiscard]] inline Oid internal_namespace_oid() {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return get_namespace_oid("bioseq_internal", true); });
}

/// Resolve the immutable sequence-source catalog relation.
[[nodiscard]] inline Oid relation_oid() {
    const Oid namespace_oid = internal_namespace_oid();
    return OidIsValid(namespace_oid)
        ? bioseq::postgres::support::invoke_pg_as_cpp([namespace_oid] {
            return get_relname_relid(
                "sequence_source_version", namespace_oid);
        })
        : InvalidOid;
}

/// Resolve the immutable sequence-source primary index.
[[nodiscard]] inline Oid primary_index_oid() {
    const Oid namespace_oid = internal_namespace_oid();
    return OidIsValid(namespace_oid)
        ? bioseq::postgres::support::invoke_pg_as_cpp([namespace_oid] {
            return get_relname_relid(
                "sequence_source_version_pkey", namespace_oid);
        })
        : InvalidOid;
}

/// Raise a fail-closed sequence-source catalog contract error.
[[noreturn]] inline void source_catalog_error(std::string_view message) {
    const std::string owned{message};
    bioseq::postgres::support::invoke_pg_as_cpp([&owned] {
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
        || descriptor->natts
            != static_cast<int>(
                sequence_source_version_codec::field_count)) {
        source_catalog_error("catalog attribute count changed");
    }
    for (const bioseq::postgres::support::row_descriptor_field& expected :
         sequence_source_version_codec::expected_descriptor) {
        const Form_pg_attribute attribute = TupleDescAttr(
            descriptor, expected.attribute_number - 1);
        if (attribute->attnum != expected.attribute_number
            || attribute->attisdropped
            || attribute->attnotnull != !expected.nullable
            || attribute->atttypid != expected.type_oid
            || std::string_view{NameStr(attribute->attname)} != expected.name) {
            source_catalog_error("catalog attribute contract changed");
        }
    }
}

/// Validate the catalog and discover its sole authoritative primary index.
[[nodiscard]] inline Oid validate_catalog_contract(
        Relation relation,
        sequence_source_lookup_telemetry* telemetry = nullptr) {
    validate_catalog_schema(relation);
    if (telemetry != nullptr) {
        ++telemetry->schema_validations;
    }

    const Oid expected_primary = primary_index_oid();
    if (!OidIsValid(expected_primary)) {
        source_catalog_error("catalog primary index is missing");
    }
    List* indexes = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { return RelationGetIndexList(relation); });
    Oid proven_primary = InvalidOid;
    std::size_t primary_count = 0;
    ListCell* cell = nullptr;
    foreach (cell, indexes) {
        const Oid index_oid = lfirst_oid(cell);
        Relation index = bioseq::postgres::support::invoke_pg_as_cpp(
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
                && bioseq::postgres::support::invoke_pg_as_cpp(
                    [index] { return RelationGetIndexExpressions(index); }) == NIL
                && bioseq::postgres::support::invoke_pg_as_cpp(
                    [index] { return RelationGetIndexPredicate(index); }) == NIL
                && bioseq::postgres::support::invoke_pg_as_cpp(
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
        bioseq::postgres::support::invoke_pg_as_cpp(
            [index] { index_close(index, AccessShareLock); });
        if (telemetry != nullptr) {
            ++telemetry->lock_releases;
        }
    }
    bioseq::postgres::support::invoke_pg_as_cpp([indexes] { list_free(indexes); });
    if (primary_count != 1 || proven_primary != expected_primary) {
        source_catalog_error(
            "catalog primary index is not the exact live unique immediate btree contract");
    }
    return proven_primary;
}

/// Validate the exact source-version primary index that is already open for a
/// cold reader. The ordered scan reuses this descriptor, so validation and
/// reading share one open primary-index relation.
inline void validate_bound_read_catalog_contract(
        Relation relation,
        Relation primary,
        sequence_source_lookup_telemetry* telemetry = nullptr) {
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
        && bioseq::postgres::support::invoke_pg_as_cpp(
            [primary] { return RelationGetIndexExpressions(primary); }) == NIL
        && bioseq::postgres::support::invoke_pg_as_cpp(
            [primary] { return RelationGetIndexPredicate(primary); }) == NIL
        && bioseq::postgres::support::invoke_pg_as_cpp([expected_primary] {
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

/// Decode a catalog bigint that must be strictly positive.
[[nodiscard]] inline std::uint64_t positive_bigint(
        const std::int64_t signed_value) {
    if (signed_value <= 0) {
        source_catalog_error("positive BIGINT field is invalid");
    }
    return static_cast<std::uint64_t>(signed_value);
}

/// Preserve all bigint bits when decoding an unsigned checksum.
[[nodiscard]] inline std::uint64_t bigint_bits(
        const std::int64_t value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

/// Decode and validate one immutable source-catalog tuple.
[[nodiscard]] inline sequence_source_version_row deform(
        HeapTuple tuple, TupleDesc descriptor) {
    std::optional<sequence_source_version_catalog_row> decoded;
    try {
        decoded.emplace(sequence_source_version_codec::deform(
            tuple, descriptor, CurrentMemoryContext));
    } catch (const bioseq::postgres::support::row_codec_error& error) {
        source_catalog_error(error.what());
    }
    const sequence_source_version_catalog_row& catalog = *decoded;
    sequence_source_version_row row{};
    row.source_epoch = positive_bigint(catalog.source_epoch);
    row.user_relid = catalog.user_relid;
    row.database_oid = catalog.database_oid;
    row.tablespace_oid = catalog.tablespace_oid;
    row.main_relfilenode = catalog.main_relfilenode;
    row.main_storage_id = positive_bigint(catalog.main_storage_id);
    row.column_attno = catalog.column_attno;
    const std::int16_t locator_version_value =
        catalog.locator_version;
    const std::int16_t bucket_value = catalog.bucket_idx;
    if (locator_version_value != sequence_locator_payload_version
        || bucket_value < 0
        || bucket_value > (std::numeric_limits<std::uint8_t>::max)()) {
        source_catalog_error("locator revision or bucket is outside its wire domain");
    }
    row.locator.payload_version = static_cast<std::uint8_t>(locator_version_value);
    row.locator.bucket_index = static_cast<std::uint8_t>(bucket_value);
    const std::int32_t shard_value = catalog.shard_idx;
    if (shard_value < 0) {
        source_catalog_error("locator shard is outside its wire domain");
    }
    row.locator.shard_index = static_cast<std::uint32_t>(shard_value);
    row.fragment_relid =
        catalog.fragment_relid.value_or(InvalidOid);
    const std::int64_t fragment_storage =
        catalog.fragment_storage_id;
    if (fragment_storage < 0) {
        source_catalog_error("fragment storage ID is negative");
    }
    row.fragment_storage_id = static_cast<std::uint64_t>(fragment_storage);
    const std::int64_t first_row = catalog.first_sub_row_no;
    const std::int64_t fragments = catalog.num_fragments;
    const std::int64_t total_bp = catalog.total_bp_size;
    if (first_row < 0 || fragments < 0 || total_bp < 0
        || static_cast<std::uint64_t>(fragments)
            > (std::numeric_limits<std::uint32_t>::max)()) {
        source_catalog_error("locator geometry is outside its wire domain");
    }
    row.locator.first_fragment_row = static_cast<std::uint64_t>(first_row);
    row.locator.fragment_count = static_cast<std::uint32_t>(fragments);
    row.locator.logical_length = static_cast<std::uint64_t>(total_bp);
    row.locator.source_epoch = row.source_epoch;
    const std::int16_t alphabet_value =
        catalog.alphabet_code;
    const std::int16_t storage_width_value =
        catalog.storage_unit_byte_width;
    if (alphabet_value < 0
        || alphabet_value > (std::numeric_limits<std::uint8_t>::max)()
        || storage_width_value <= 0 || storage_width_value > 0x0F) {
        source_catalog_error("source alphabet or storage width is outside its wire domain");
    }
    row.alphabet_code = static_cast<std::uint8_t>(alphabet_value);
    row.storage_unit_byte_width = static_cast<std::uint8_t>(storage_width_value);
    const std::int32_t padding = catalog.padding_letters;
    if (padding < 0
        || static_cast<std::uint32_t>(padding)
            > (std::numeric_limits<std::uint16_t>::max)()) {
        source_catalog_error("source padding is outside its wire domain");
    }
    row.padding_letters = static_cast<std::uint16_t>(padding);
    row.source_content_checksum =
        bigint_bits(catalog.source_content_checksum);
    row.state = catalog.state;
    if (row.state != k_sequence_source_state_unpublished
        && row.state != k_sequence_source_state_ready
        && row.state != k_sequence_source_state_invalid) {
        source_catalog_error("source state is outside its state-machine domain");
    }
    return row;
}

/// Encode one immutable source record as a catalog heap tuple.
[[nodiscard]] inline HeapTuple form(
        TupleDesc descriptor, const sequence_source_version_row& row) {
    const sequence_source_version_catalog_row catalog{
        .source_epoch = static_cast<std::int64_t>(row.source_epoch),
        .user_relid = row.user_relid,
        .database_oid = row.database_oid,
        .tablespace_oid = row.tablespace_oid,
        .main_relfilenode = row.main_relfilenode,
        .main_storage_id =
            static_cast<std::int64_t>(row.main_storage_id),
        .column_attno = row.column_attno,
        .locator_version =
            static_cast<std::int16_t>(row.locator.payload_version),
        .bucket_idx =
            static_cast<std::int16_t>(row.locator.bucket_index),
        .shard_idx =
            static_cast<std::int32_t>(row.locator.shard_index),
        .fragment_relid = OidIsValid(row.fragment_relid)
            ? std::optional<Oid>{row.fragment_relid}
            : std::nullopt,
        .fragment_storage_id =
            static_cast<std::int64_t>(row.fragment_storage_id),
        .first_sub_row_no =
            static_cast<std::int64_t>(row.locator.first_fragment_row),
        .num_fragments =
            static_cast<std::int64_t>(row.locator.fragment_count),
        .total_bp_size =
            static_cast<std::int64_t>(row.locator.logical_length),
        .alphabet_code =
            static_cast<std::int16_t>(row.alphabet_code),
        .storage_unit_byte_width =
            static_cast<std::int16_t>(row.storage_unit_byte_width),
        .padding_letters =
            static_cast<std::int32_t>(row.padding_letters),
        .source_content_checksum =
            std::bit_cast<std::int64_t>(
                row.source_content_checksum),
        .state = row.state,
    };
    try {
        return sequence_source_version_codec::encode(
            catalog, CurrentMemoryContext).form_tuple(descriptor);
    } catch (const bioseq::postgres::support::row_codec_error& error) {
        source_catalog_error(error.what());
    }
}

} // namespace source_version_detail

// The descriptor catalog owns required relation locator attributes and two
// partial indexes, so the generic eight-column sidecar validator cannot
// describe it. Keep this cold schema authority next to the existing
// PostgreSQL-owned sequence-source catalog contracts; generation-image
// lifecycle code consumes the exported entry point without importing a second
// tuple-shape authority.
namespace minimizer_generation_catalog_detail {

/// Supported canonical defaults in generation catalog descriptors.
enum class default_kind : std::uint8_t {
    none,
    zero_int2,
    zero_int4,
    zero_int8,
    empty_text,
};

inline constexpr std::array<
    default_kind,
    minimizer_generation_storage_codec::field_count>
k_storage_defaults{};

inline constexpr std::array<
    default_kind,
    minimizer_generation_reaper_codec::field_count>
k_reaper_defaults{{
    default_kind::none,
    default_kind::zero_int4,
    default_kind::zero_int2,
    default_kind::empty_text,
    default_kind::zero_int8,
}};

inline constexpr std::array<std::string_view, 13>
k_storage_check_expressions{{
    "database_oid <> 0",
    "tablespace_oid <> 0",
    "main_relfilenode <> 0",
    "user_relid <> 0",
    "column_attno > 0",
    "index_name = 'minimizers_per_fragment'",
    "query_key >= 0",
    "generation_id > 0",
    "provider = 0",
    "image_bytes >= 4096",
    "state IN (1, 2)",
    "(payload_relation_oid <> 0 "
        "AND payload_database_oid = database_oid "
        "AND payload_tablespace_oid <> 0 "
        "AND payload_relfilenode <> 0)",
    "((state = 1 AND retire_full_xid IS NULL) "
      "OR (state = 2 AND retire_full_xid IS NOT NULL))",
}};

inline constexpr std::array<std::string_view, 4>
k_reaper_check_expressions{{
    "database_oid <> 0",
    "tablespace_root_ordinal >= 0",
    "shard_ordinal BETWEEN 0 AND 255",
    "pass >= 0",
}};

/// Raise one generation-catalog schema contract violation.
[[noreturn]] inline void contract_error(
        const std::string_view catalog,
        const std::string_view detail) {
    const std::string owned_catalog{catalog};
    const std::string owned_detail{detail};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&owned_catalog, &owned_detail] {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq minimizer generation catalog contract changed"),
                 errdetail("catalog=%s invariant=%s",
                    owned_catalog.c_str(), owned_detail.c_str())));
        });
    pg_unreachable();
}

/// Check an attribute default against its canonical expression.
[[nodiscard]] inline bool exact_default(
        Relation relation,
        const bioseq::postgres::support::row_descriptor_field& expected,
        const default_kind expected_default_kind) {
    Node* node = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation, &expected] {
            return build_column_default(
                relation, expected.attribute_number);
        });
    if (expected_default_kind == default_kind::none) {
        return node == nullptr;
    }
    if (node == nullptr || !IsA(node, Const)) return false;
    const auto* constant = castNode(Const, node);
    if (constant->constisnull) return false;
    switch (expected_default_kind) {
        case default_kind::none:
            return false;
        case default_kind::zero_int2:
            return constant->consttype == INT2OID
                && DatumGetInt16(constant->constvalue) == 0;
        case default_kind::zero_int4:
            return constant->consttype == INT4OID
                && DatumGetInt32(constant->constvalue) == 0;
        case default_kind::zero_int8:
            return constant->consttype == INT8OID
                && DatumGetInt64(constant->constvalue) == 0;
        case default_kind::empty_text: {
            if (constant->consttype != TEXTOID) return false;
            const text* value = DatumGetTextPP(constant->constvalue);
            return VARSIZE_ANY_EXHDR(value) == 0;
        }
    }
    return false;
}

/// Validate relation identity, access method and every attribute descriptor.
template <std::size_t p_attribute_count>
inline void validate_relation_shape(
        Relation relation,
        const Oid expected_relation_oid,
        const std::string_view expected_name,
        const std::array<
            bioseq::postgres::support::row_descriptor_field,
            p_attribute_count>& attributes,
        const std::array<default_kind, p_attribute_count>& defaults,
        const std::uint16_t expected_check_count) {
    if (relation == nullptr
        || RelationGetRelid(relation) != expected_relation_oid
        || RelationGetNamespace(relation)
            != bioseq::postgres::support::invoke_pg_as_cpp(
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
    for (std::size_t index = 0; index < attributes.size(); ++index) {
        const bioseq::postgres::support::row_descriptor_field& expected =
            attributes[index];
        const default_kind expected_default_kind = defaults[index];
        const Form_pg_attribute attribute = TupleDescAttr(
            descriptor, expected.attribute_number - 1);
        const bool expects_default =
            expected_default_kind != default_kind::none;
        if (attribute->attnum != expected.attribute_number
            || attribute->attisdropped
            || attribute->attinhcount != 0
            || !attribute->attislocal
            || attribute->attnotnull != !expected.nullable
            || attribute->atttypid != expected.type_oid
            || attribute->atttypmod != -1
            || attribute->attcollation
                != bioseq::postgres::support::invoke_pg_as_cpp(
                    [&expected] {
                        return get_typcollation(expected.type_oid);
                    })
            || attribute->atthasdef != expects_default
            || attribute->atthasmissing
            || attribute->attidentity != '\0'
            || attribute->attgenerated != '\0'
            || std::string_view{NameStr(attribute->attname)}
                != expected.name) {
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

/// Parse and analyze one canonical CHECK expression for comparison.
[[nodiscard]] inline Node* analyze_expected_check_expression(
        const std::string_view relation_name,
        const std::string_view expression) {
    const std::string sql = fmt::format(
        "SELECT ({}) FROM ONLY bioseq_internal.{};",
        expression, relation_name);
    return bioseq::postgres::support::invoke_pg_as_cpp([&sql] {
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

/// Require exactly the configured canonical CHECK expressions.
template <std::size_t p_check_count>
inline void validate_check_expressions(
        Relation relation,
        const std::string_view relation_name,
        const std::array<std::string_view, p_check_count>& expected_checks) {
    const TupleConstr* constraints = RelationGetDescr(relation)->constr;
    if (constraints == nullptr
        || constraints->num_check != expected_checks.size()) {
        contract_error(relation_name, "exact CHECK expression count");
    }
    std::array<Node*, p_check_count> expected_nodes{};
    std::array<bool, p_check_count> matched{};
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

/// Match a partial-index predicate against one exact state constant.
[[nodiscard]] inline bool exact_state_predicate(
        Relation index,
        const std::int16_t expected_state) {
    List* predicate = bioseq::postgres::support::invoke_pg_as_cpp(
        [index] { return RelationGetIndexPredicate(index); });
    if (predicate == NIL || list_length(predicate) != 1) return false;
    const Node* node = static_cast<const Node*>(linitial(predicate));
    if (node == nullptr || !IsA(node, OpExpr)) return false;
    const auto* equality = castNode(OpExpr, node);
    if (equality->opresulttype != BOOLOID
        || list_length(equality->args) != 2
        || bioseq::postgres::support::invoke_pg_as_cpp(
            [equality] { return get_opcode(equality->opno); }) != F_INT2EQ) {
        return false;
    }
    const Node* left = static_cast<const Node*>(linitial(equality->args));
    const Node* right = static_cast<const Node*>(lsecond(equality->args));
    if (left == nullptr || right == nullptr
        || !IsA(left, Var) || !IsA(right, Const)) return false;
    const auto* variable = castNode(Var, left);
    const auto* constant = castNode(Const, right);
    return variable->varattno
            == minimizer_generation_storage_codec::attribute_number<
                &minimizer_generation_storage_catalog_row::state>()
        && variable->vartype == INT2OID
        && variable->varlevelsup == 0
        && constant->consttype == INT2OID
        && !constant->constisnull
        && DatumGetInt16(constant->constvalue) == expected_state;
}

/// Validate one authoritative index identity, key and predicate contract.
template <std::size_t p_key_count>
[[nodiscard]] inline bool exact_index(
        Relation index,
        const Oid expected_table_oid,
        const std::string_view expected_name,
        const bool expected_primary,
        const bool expected_unique,
        const std::array<AttrNumber, p_key_count>& keys,
        const std::array<Oid, p_key_count>& key_types,
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
        || bioseq::postgres::support::invoke_pg_as_cpp(
            [index] { return RelationGetIndexExpressions(index); }) != NIL) {
        return false;
    }
    const List* predicate = bioseq::postgres::support::invoke_pg_as_cpp(
        [index] { return RelationGetIndexPredicate(index); });
    if ((!expected_state.has_value() && predicate != NIL)
        || (expected_state.has_value()
            && !exact_state_predicate(index, *expected_state))) {
        return false;
    }
    for (std::size_t key = 0; key < keys.size(); ++key) {
        const Oid expected_opclass =
            bioseq::postgres::support::invoke_pg_as_cpp(
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

/// Validate every authoritative generation-storage index.
inline void validate_storage_indexes(Relation storage) {
    using row = minimizer_generation_storage_catalog_row;
    using codec = minimizer_generation_storage_codec;
    static constexpr std::array<AttrNumber, 8> primary_keys{
        codec::attribute_number<&row::database_oid>(),
        codec::attribute_number<&row::tablespace_oid>(),
        codec::attribute_number<&row::main_relfilenode>(),
        codec::attribute_number<&row::user_relid>(),
        codec::attribute_number<&row::column_attno>(),
        codec::attribute_number<&row::index_name>(),
        codec::attribute_number<&row::query_key>(),
        codec::attribute_number<&row::generation_id>()};
    static constexpr std::array<Oid, 8> primary_types{
        codec::field<&row::database_oid>::type_oid,
        codec::field<&row::tablespace_oid>::type_oid,
        codec::field<&row::main_relfilenode>::type_oid,
        codec::field<&row::user_relid>::type_oid,
        codec::field<&row::column_attno>::type_oid,
        codec::field<&row::index_name>::type_oid,
        codec::field<&row::query_key>::type_oid,
        codec::field<&row::generation_id>::type_oid};
    static constexpr std::array<AttrNumber, 7> live_keys{
        codec::attribute_number<&row::database_oid>(),
        codec::attribute_number<&row::tablespace_oid>(),
        codec::attribute_number<&row::main_relfilenode>(),
        codec::attribute_number<&row::user_relid>(),
        codec::attribute_number<&row::column_attno>(),
        codec::attribute_number<&row::index_name>(),
        codec::attribute_number<&row::query_key>()};
    static constexpr std::array<Oid, 7> live_types{
        codec::field<&row::database_oid>::type_oid,
        codec::field<&row::tablespace_oid>::type_oid,
        codec::field<&row::main_relfilenode>::type_oid,
        codec::field<&row::user_relid>::type_oid,
        codec::field<&row::column_attno>::type_oid,
        codec::field<&row::index_name>::type_oid,
        codec::field<&row::query_key>::type_oid};
    static constexpr std::array<AttrNumber, 9> retired_keys{
        codec::attribute_number<&row::retire_full_xid>(),
        codec::attribute_number<&row::database_oid>(),
        codec::attribute_number<&row::tablespace_oid>(),
        codec::attribute_number<&row::main_relfilenode>(),
        codec::attribute_number<&row::user_relid>(),
        codec::attribute_number<&row::column_attno>(),
        codec::attribute_number<&row::index_name>(),
        codec::attribute_number<&row::query_key>(),
        codec::attribute_number<&row::generation_id>()};
    static constexpr std::array<Oid, 9> retired_types{
        codec::field<&row::retire_full_xid>::type_oid,
        codec::field<&row::database_oid>::type_oid,
        codec::field<&row::tablespace_oid>::type_oid,
        codec::field<&row::main_relfilenode>::type_oid,
        codec::field<&row::user_relid>::type_oid,
        codec::field<&row::column_attno>::type_oid,
        codec::field<&row::index_name>::type_oid,
        codec::field<&row::query_key>::type_oid,
        codec::field<&row::generation_id>::type_oid};

    List* indexes = bioseq::postgres::support::invoke_pg_as_cpp(
        [storage] { return RelationGetIndexList(storage); });
    std::size_t exact_count = 0;
    ListCell* cell = nullptr;
    foreach (cell, indexes) {
        const Oid index_oid = lfirst_oid(cell);
        Relation index = bioseq::postgres::support::invoke_pg_as_cpp(
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
        bioseq::postgres::support::invoke_pg_as_cpp(
            [index] { index_close(index, AccessShareLock); });
        if (!exact) {
            contract_error("minimizer_generation_storage",
                "primary/one-LIVE/retired index contract");
        }
        ++exact_count;
    }
    bioseq::postgres::support::invoke_pg_as_cpp([indexes] { list_free(indexes); });
    if (exact_count != 3) {
        contract_error("minimizer_generation_storage",
            "exactly three authoritative indexes");
    }
}

/// Validate every authoritative generation-reaper index.
inline void validate_reaper_indexes(Relation reaper) {
    using row = minimizer_generation_reaper_catalog_row;
    using codec = minimizer_generation_reaper_codec;
    static constexpr std::array<AttrNumber, 1> primary_keys{
        codec::attribute_number<&row::database_oid>()};
    static constexpr std::array<Oid, 1> primary_types{
        codec::field<&row::database_oid>::type_oid};
    List* indexes = bioseq::postgres::support::invoke_pg_as_cpp(
        [reaper] { return RelationGetIndexList(reaper); });
    if (list_length(indexes) != 1) {
        contract_error("minimizer_generation_reaper_state",
            "exactly one primary index");
    }
    const Oid index_oid = lfirst_oid(list_head(indexes));
    Relation index = bioseq::postgres::support::invoke_pg_as_cpp(
        [index_oid] { return index_open(index_oid, AccessShareLock); });
    const bool exact = exact_index(
        index, RelationGetRelid(reaper),
        "minimizer_generation_reaper_state_pkey",
        true, true, primary_keys, primary_types, std::nullopt);
    bioseq::postgres::support::invoke_pg_as_cpp(
        [index] { index_close(index, AccessShareLock); });
    bioseq::postgres::support::invoke_pg_as_cpp([indexes] { list_free(indexes); });
    if (!exact) {
        contract_error("minimizer_generation_reaper_state",
            "primary index order/opclass/validity");
    }
}

} // namespace minimizer_generation_catalog_detail

/// Fail closed when either installation-local catalog authority drifts. This
/// is intentionally a cold publication/open guard, not a per-anchor check.
inline void validate_minimizer_generation_storage_catalogs_pg() {
    using namespace minimizer_generation_catalog_detail;
    const Oid namespace_oid = bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return get_namespace_oid("bioseq_internal", true); });
    if (!OidIsValid(namespace_oid)) {
        contract_error("bioseq_internal", "internal schema is missing");
    }
    const Oid storage_oid = bioseq::postgres::support::invoke_pg_as_cpp(
        [namespace_oid] {
            return get_relname_relid(
                "minimizer_generation_storage", namespace_oid);
        });
    const Oid reaper_oid = bioseq::postgres::support::invoke_pg_as_cpp(
        [namespace_oid] {
            return get_relname_relid(
                "minimizer_generation_reaper_state", namespace_oid);
        });
    if (!OidIsValid(storage_oid) || !OidIsValid(reaper_oid)) {
        contract_error("bioseq_internal",
            "generation descriptor or reaper catalog is missing");
    }
    Relation storage = bioseq::postgres::support::invoke_pg_as_cpp(
        [storage_oid] {
            return relation_open(storage_oid, AccessShareLock);
        });
    Relation reaper = bioseq::postgres::support::invoke_pg_as_cpp(
        [reaper_oid] {
            return relation_open(reaper_oid, AccessShareLock);
        });
    validate_relation_shape(
        storage, storage_oid, "minimizer_generation_storage",
        minimizer_generation_storage_codec::expected_descriptor,
        k_storage_defaults, 13);
    validate_check_expressions(
        storage, "minimizer_generation_storage",
        k_storage_check_expressions);
    validate_storage_indexes(storage);
    validate_relation_shape(
        reaper, reaper_oid, "minimizer_generation_reaper_state",
        minimizer_generation_reaper_codec::expected_descriptor,
        k_reaper_defaults, 4);
    validate_check_expressions(
        reaper, "minimizer_generation_reaper_state",
        k_reaper_check_expressions);
    validate_reaper_indexes(reaper);
    bioseq::postgres::support::invoke_pg_as_cpp(
        [reaper] { relation_close(reaper, AccessShareLock); });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [storage] { relation_close(storage, AccessShareLock); });
}

class pending_sequence_source;

/// PostgreSQL catalog operations for immutable source epochs. Every lookup
/// accepts the caller's snapshot; no backend-global or SnapshotSelf read is
/// allowed in a validated reader.
struct sequence_source_version_manager {
    /// Allocate the next positive immutable source epoch.
    [[nodiscard]] static std::uint64_t allocate_epoch() {
        const Oid namespace_oid =
            source_version_detail::internal_namespace_oid();
        const Oid sequence_oid =
            storage_manager_entity_handles::sequence_source_epoch_seq
                .resolve_oid(
                    bioseq::postgres::support::catalog_missing_policy::
                        return_invalid);
        if (!OidIsValid(sequence_oid)) {
            source_version_detail::source_catalog_error(
                "owned epoch sequence is missing");
        }
        Relation sequence_relation = bioseq::postgres::support::invoke_pg_as_cpp(
            [sequence_oid] {
                return relation_open(sequence_oid, AccessShareLock);
            });
        const bool shape_ok =
            RelationGetNamespace(sequence_relation) == namespace_oid
            && sequence_relation->rd_rel->relkind == RELKIND_SEQUENCE
            && sequence_relation->rd_rel->relpersistence
                == RELPERSISTENCE_PERMANENT;
        bioseq::postgres::support::invoke_pg_as_cpp([sequence_relation] {
            relation_close(sequence_relation, AccessShareLock);
        });
        if (!shape_ok) {
            source_version_detail::source_catalog_error(
                "owned epoch sequence relation contract changed");
        }
        HeapTuple sequence_tuple = bioseq::postgres::support::invoke_pg_as_cpp(
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
        bioseq::postgres::support::invoke_pg_as_cpp(
            [sequence_tuple] { ReleaseSysCache(sequence_tuple); });
        if (!contract_ok) {
            source_version_detail::source_catalog_error(
                "owned epoch sequence numeric contract changed");
        }
        const std::int64_t epoch = bioseq::postgres::support::invoke_pg_as_cpp(
            [sequence_oid] { return nextval_internal(sequence_oid, false); });
        if (epoch <= 0) {
            source_version_detail::source_catalog_error(
                "owned epoch sequence exhausted positive BIGINT");
        }
        return static_cast<std::uint64_t>(epoch);
    }

    /// Insert one validated source identity in unpublished state.
    static void insert_unpublished(const sequence_source_version_row& input) {
        if (input.source_epoch == 0
            || input.source_epoch
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int64_t>::max)())
            || input.locator.source_epoch != input.source_epoch
            || input.locator.payload_version != sequence_locator_payload_version
            || input.locator.reserved_16 != 0 || input.locator.reserved_32 != 0
            || input.locator.shard_index
                > static_cast<std::uint32_t>(
                    (std::numeric_limits<std::int32_t>::max)())
            || input.locator.first_fragment_row
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int64_t>::max)())
            || input.locator.logical_length
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
            || input.state != k_sequence_source_state_unpublished
            || (input.locator.fragment_count == 0
                ? (OidIsValid(input.fragment_relid)
                    || input.fragment_storage_id != 0
                    || input.locator.first_fragment_row != 0)
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
            relation = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_oid] {
                    return table_open(relation_oid, RowExclusiveLock);
                });
            source_version_detail::validate_catalog_schema(relation);
            tuple = source_version_detail::form(
                RelationGetDescr(relation), input);
            bioseq::postgres::support::invoke_pg_as_cpp(
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
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [tuple] { heap_freetuple(tuple); });
            });
        }
        if (relation != nullptr) {
            cleanup([relation] {
                bioseq::postgres::support::invoke_pg_as_cpp([relation] {
                    table_close(relation, RowExclusiveLock);
                });
            });
        }
        if (failure != nullptr) std::rethrow_exception(failure);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { CommandCounterIncrement(); });
    }

    /// Look up one source epoch through the exact primary index and snapshot.
    [[nodiscard]] static std::optional<sequence_source_version_row> lookup(
            std::uint64_t epoch, Snapshot snapshot, bool require_ready = true,
            bool* exact_primary_index_scan = nullptr,
            sequence_source_lookup_telemetry* telemetry = nullptr) {
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
        ScanKeyData key =
            sequence_source_version_codec::scan_key<
                &sequence_source_version_catalog_row::source_epoch>(
                    static_cast<std::int64_t>(epoch));
        SysScanDesc scan = systable_beginscan_ordered(
            relation, primary, snapshot, 1, &key);
        HeapTuple tuple = systable_getnext_ordered(
            scan, ForwardScanDirection);
        std::optional<sequence_source_version_row> result;
        if (HeapTupleIsValid(tuple)) {
            sequence_source_version_row row = source_version_detail::deform(
                tuple, RelationGetDescr(relation));
            if (!require_ready || row.state == k_sequence_source_state_ready) {
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
    /// Apply one allowed terminal source-state transition.
    static void set_state(std::uint64_t epoch, std::int16_t requested_state) {
        if (epoch == 0
            || epoch > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())
            || (requested_state != k_sequence_source_state_ready
                && requested_state != k_sequence_source_state_invalid)) {
            source_version_detail::source_catalog_error(
                "source state transition input is invalid");
        }
        Relation relation = nullptr;
        SysScanDesc scan = nullptr;
        HeapTuple new_tuple = nullptr;
        std::exception_ptr failure;
        try {
            const Oid relation_oid = source_version_detail::relation_oid();
            relation = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_oid] {
                    return table_open(relation_oid, RowExclusiveLock);
                });
            const Oid proven_primary =
                source_version_detail::validate_catalog_contract(relation);
            ScanKeyData key =
                sequence_source_version_codec::scan_key<
                    &sequence_source_version_catalog_row::source_epoch>(
                        static_cast<std::int64_t>(epoch));
            scan = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation, proven_primary, &key] {
                    return systable_beginscan(
                        relation, proven_primary, true,
                        SnapshotSelf, 1, &key);
                });
            HeapTuple old_tuple = bioseq::postgres::support::invoke_pg_as_cpp(
                [scan] { return systable_getnext(scan); });
            if (!HeapTupleIsValid(old_tuple)) {
                source_version_detail::source_catalog_error(
                    "source epoch row is missing during state transition");
            }
            const sequence_source_version_row current =
                source_version_detail::deform(
                    old_tuple, RelationGetDescr(relation));
            if (!sequence_source_state_transition_allowed(
                    current.state, requested_state)) {
                source_version_detail::source_catalog_error(
                    "source state transition violates the terminal state machine");
            }
            sequence_source_version_codec::update update{};
            update.set<
                &sequence_source_version_catalog_row::state>(
                    requested_state);
            new_tuple = update.modify(
                old_tuple, RelationGetDescr(relation));
            bioseq::postgres::support::invoke_pg_as_cpp(
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
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [new_tuple] { heap_freetuple(new_tuple); });
            });
        }
        if (scan != nullptr) {
            cleanup([scan] {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [scan] { systable_endscan(scan); });
            });
        }
        if (relation != nullptr) {
            cleanup([relation] {
                bioseq::postgres::support::invoke_pg_as_cpp([relation] {
                    table_close(relation, RowExclusiveLock);
                });
            });
        }
        if (failure != nullptr) std::rethrow_exception(failure);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { CommandCounterIncrement(); });
    }

    /// Publish an unpublished source epoch as ready.
    static void publish_ready(std::uint64_t epoch) {
        set_state(epoch, k_sequence_source_state_ready);
    }

    friend class pending_sequence_source;

public:

    /// Irreversibly invalidate one existing source epoch.
    static void invalidate_epoch(std::uint64_t epoch) {
        if (epoch == 0) return;
        const auto row = lookup(epoch, SnapshotSelf, /*require_ready=*/false);
        if (!row || row->state == k_sequence_source_state_invalid) return;
        set_state(epoch, k_sequence_source_state_invalid);
    }

    /// Delete source-catalog rows owned by one main physical relation.
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
        Relation relation = bioseq::postgres::support::try_open_postgres_relation(
            relation_oid, RowExclusiveLock);
        if (relation == nullptr) {
            if (internal_catalog_detail::absence_is_benign(
                    allow_missing_during_extension_teardown)) return;
            internal_catalog_detail::missing_outside_teardown(
                "bioseq_internal.sequence_source_version");
        }
        source_version_detail::validate_catalog_schema(relation);
        std::array<ScanKeyData, 3> keys{
            sequence_source_version_codec::scan_key<
                &sequence_source_version_catalog_row::database_oid>(
                    database_oid),
            sequence_source_version_codec::scan_key<
                &sequence_source_version_catalog_row::tablespace_oid>(
                    tablespace_oid),
            sequence_source_version_codec::scan_key<
                &sequence_source_version_catalog_row::main_relfilenode>(
                    main_relfilenode),
        };
        SysScanDesc scan = systable_beginscan(
            relation, InvalidOid, false, SnapshotSelf,
            static_cast<int>(keys.size()), keys.data());
        HeapTuple tuple = nullptr;
        while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
            CatalogTupleDelete(relation, &tuple->t_self);
        }
        systable_endscan(scan);
        table_close(relation, RowExclusiveLock);
        CommandCounterIncrement();
    }

    /// Invalidate every source epoch referencing one fragment relation.
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
        Relation relation = bioseq::postgres::support::try_open_postgres_relation(
            catalog_oid, RowExclusiveLock);
        if (relation == nullptr) {
            if (internal_catalog_detail::absence_is_benign(
                    allow_missing_during_extension_teardown)) return;
            internal_catalog_detail::missing_outside_teardown(
                "bioseq_internal.sequence_source_version");
        }
        source_version_detail::validate_catalog_schema(relation);
        ScanKeyData key;
        ScanKeyInit(
            &key,
            sequence_source_version_codec::attribute_number<
                &sequence_source_version_catalog_row::fragment_relid>(),
            BTEqualStrategyNumber,
            sequence_source_version_codec::field<
                &sequence_source_version_catalog_row::fragment_relid>::
                    comparison_procedure(
                        bioseq::postgres::support::scan_comparison::equal),
            ObjectIdGetDatum(fragment_relation_oid));
        // DROP/invalidation is not a timed path. A validated heap scan avoids
        // trusting a missing, renamed, partial, or otherwise corrupted
        // secondary index to decide whether ready epochs survive a drop.
        SysScanDesc scan = systable_beginscan(
            relation, InvalidOid, false,
            SnapshotSelf, 1, &key);
        HeapTuple tuple = nullptr;
        while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
            const sequence_source_version_row current =
                source_version_detail::deform(tuple, RelationGetDescr(relation));
            if (current.state == k_sequence_source_state_invalid) continue;
            sequence_source_version_codec::update update{};
            update.set<
                &sequence_source_version_catalog_row::state>(
                    k_sequence_source_state_invalid);
            HeapTuple updated = update.modify(
                tuple, RelationGetDescr(relation));
            CatalogTupleUpdate(relation, &tuple->t_self, updated);
            heap_freetuple(updated);
        }
        systable_endscan(scan);
        table_close(relation, RowExclusiveLock);
        CommandCounterIncrement();
    }
};

template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
struct sequence_storage_manager;
struct sequence_stream_writer;

/// Move-only publication authority. Fragment/catalog creation returns this
/// token in unpublished state; only the caller that has appended the parent
/// row may consume it to publish the ready state.
class pending_sequence_source {
private:
    sequence_locator locator_{};
    bool pending_ = false;

    /// Capture sole publication authority for one newly persisted locator.
    explicit pending_sequence_source(sequence_locator locator) noexcept
        : locator_(locator), pending_(locator.source_epoch != 0) {}

    template<class p_length_type,
             std::size_t p_bucket_minimum_length_bits,
             std::size_t p_bucket_maximum_length_bits>
    friend struct sequence_storage_manager;
    friend struct sequence_stream_writer;

public:
    pending_sequence_source() = delete;
    pending_sequence_source(const pending_sequence_source&) = delete;
    pending_sequence_source& operator=(const pending_sequence_source&) = delete;
    /// Transfer sole source-publication authority.
    pending_sequence_source(pending_sequence_source&& other) noexcept
        : locator_(other.locator_), pending_(std::exchange(other.pending_, false)) {}
    pending_sequence_source& operator=(pending_sequence_source&&) = delete;
    ~pending_sequence_source() = default;

    [[nodiscard]] const sequence_locator& locator() const noexcept { return locator_; }
    [[nodiscard]] bool pending() const noexcept { return pending_; }

    /// Publish the source only after its parent row is visible to the writer.
    void publish_after_parent_row() {
        if (!pending_) {
            storage_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq sequence source catalog: publication token was reused");
        }
        sequence_source_version_manager::publish_ready(locator_.source_epoch);
        pending_ = false;
    }

    /// Consume a speculative-insert authority whose parent row was rejected.
    /// This is intentionally token-owned: the losing path cannot retain a raw
    /// epoch and later manufacture ready-state publication authority.
    void invalidate_without_parent_row() {
        if (!pending_) {
            storage_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq sequence source catalog: invalidation token was reused");
        }
        sequence_source_version_manager::invalidate_epoch(locator_.source_epoch);
        pending_ = false;
    }
};

static_assert(!std::is_move_assignable_v<pending_sequence_source>,
              "A pending source authority must never be overwritten by move assignment.");

// ---------------------------------------------------------------------------

}  // namespace bioseq::storage
