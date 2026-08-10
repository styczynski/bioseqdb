/*-------------------------------------------------------------------------
 *
 * storage/sequence_storage.cppm
 *
 * Length-bucketed sub-table sharding for sequence
 * storage. Owns:
 *   - the locator stub format (a fixed-size bioseq::pg_types::seq-shaped varlena that
 *     the main columnar relation stores in place of the full sequence);
 *   - the consteval bucket-threshold builder and the per-call
 *     sequence_bucket_index LUT;
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

export module bioseq.postgres.storage.manager:sequence_storage;

import bioseq.common.logging;
import bioseq.postgres.support;
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
export import bioseq.storage.layout;

static_assert(__cplusplus >= 202302L,
              "sequence storage requires C++23-or-later (project default: C++26)");

namespace storage_manager_entity_handles =
    bioseq::sql::generated::core::
        storage_manager_sequence_storage_manager::entity_handles;
namespace storage_manager_catalog_rows =
    bioseq::storage::catalog_rows;
namespace storage_manager_row_codecs =
    bioseq::storage::manager::sql_row_codecs;
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
    bioseq::postgres::support::invoke_pg_as_cpp([&owned] {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq internal catalog %s is missing outside extension teardown",
                    owned.c_str())));
    });
    pg_unreachable();
}

} // namespace internal_catalog_detail

inline constexpr std::int16_t k_sequence_source_state_unpublished = 0;
inline constexpr std::int16_t k_sequence_source_state_ready = 1;
inline constexpr std::int16_t k_sequence_source_state_invalid = 2;

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

inline constexpr AttrNumber source_epoch =
    source_attribute<&sequence_source_version_catalog_row::source_epoch>;

[[nodiscard]] inline Oid internal_namespace_oid() {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return get_namespace_oid("bioseq_internal", true); });
}

[[nodiscard]] inline Oid relation_oid() {
    const Oid namespace_oid = internal_namespace_oid();
    return OidIsValid(namespace_oid)
        ? bioseq::postgres::support::invoke_pg_as_cpp([namespace_oid] {
            return get_relname_relid(
                "sequence_source_version", namespace_oid);
        })
        : InvalidOid;
}

[[nodiscard]] inline Oid primary_index_oid() {
    const Oid namespace_oid = internal_namespace_oid();
    return OidIsValid(namespace_oid)
        ? bioseq::postgres::support::invoke_pg_as_cpp([namespace_oid] {
            return get_relname_relid(
                "sequence_source_version_pkey", namespace_oid);
        })
        : InvalidOid;
}

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
                sequence_source_version_codec::fields_count)) {
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

[[nodiscard]] inline std::uint64_t positive_bigint(
        const std::int64_t signed_value) {
    if (signed_value <= 0) {
        source_catalog_error("positive BIGINT field is invalid");
    }
    return static_cast<std::uint64_t>(signed_value);
}

[[nodiscard]] inline std::uint64_t bigint_bits(
        const std::int64_t value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

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

enum class default_kind : std::uint8_t {
    none,
    zero_int2,
    zero_int4,
    zero_int8,
    empty_text,
};

inline constexpr std::array<
    default_kind,
    minimizer_generation_storage_codec::fields_count>
k_storage_defaults{};

inline constexpr std::array<
    default_kind,
    minimizer_generation_reaper_codec::fields_count>
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

[[nodiscard]] inline bool exact_default(
        Relation relation,
        const bioseq::postgres::support::row_descriptor_field& expected,
        const default_kind default_kind) {
    Node* node = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation, &expected] {
            return build_column_default(
                relation, expected.attribute_number);
        });
    if (default_kind == default_kind::none) {
        return node == nullptr;
    }
    if (node == nullptr || !IsA(node, Const)) return false;
    const auto* constant = castNode(Const, node);
    if (constant->constisnull) return false;
    switch (default_kind) {
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

    static void publish_ready(std::uint64_t epoch) {
        set_state(epoch, k_sequence_source_state_ready);
    }

    friend class pending_sequence_source;

public:

    static void invalidate_epoch(std::uint64_t epoch) {
        if (epoch == 0) return;
        const auto row = lookup(epoch, SnapshotSelf, /*require_ready=*/false);
        if (!row || row->state == k_sequence_source_state_invalid) return;
        set_state(epoch, k_sequence_source_state_invalid);
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
    pending_sequence_source(pending_sequence_source&& other) noexcept
        : locator_(other.locator_), pending_(std::exchange(other.pending_, false)) {}
    pending_sequence_source& operator=(pending_sequence_source&&) = delete;
    ~pending_sequence_source() = default;

    [[nodiscard]] const sequence_locator& locator() const noexcept { return locator_; }
    [[nodiscard]] bool pending() const noexcept { return pending_; }

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
// Current locator stub representation. A 56-byte bioseq::pg_types::seq-shaped
// varlena lives in the main columnar relation in place of the full sequence
// payload. No alternate on-disk representation is accepted.
// ---------------------------------------------------------------------------

// `sequence_locator_payload_version` and the `sequence_locator` value
// (with its 32-byte / offset static_asserts) live in the PG-free
// `bioseq_bucket_math` module so the unit-test TU can link them; they are
// re-exported above via `export import bioseq_bucket_math`, so every use of
// `sequence_locator` / `sequence_locator_payload_version` below resolves unchanged.

constexpr std::size_t k_locator_stub_varlena_bytes =
    bioseq::pg_types::seq::header_bytes() + sizeof(sequence_locator);
static_assert(k_locator_stub_varlena_bytes == 56,
              "sequence locator stub must be exactly 56 bytes");

[[nodiscard]] [[gnu::always_inline]]
inline bool is_locator_stub(const bioseq::pg_types::seq* hdr) noexcept {
    return hdr != nullptr
        && is_sequence_locator_stub_width(
            static_cast<std::uint8_t>(hdr->storage_unit_byte_width));
}

[[nodiscard]] [[gnu::always_inline]]
inline std::uint8_t logical_storage_unit_byte_width(const bioseq::pg_types::seq* hdr) noexcept {
    return static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(hdr->storage_unit_byte_width) & 0x0Fu);
}

[[nodiscard]] [[gnu::always_inline]]
inline sequence_locator decode_locator_from_stub(const bioseq::pg_types::seq* hdr) {
    if (!is_locator_stub(hdr)) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: decode_locator_from_stub called on a "
               "non-stub value (sub_w={}). Caller must guard with "
               "is_locator_stub(hdr) first.",
               static_cast<unsigned>(hdr->storage_unit_byte_width));
    }
    if (VARSIZE(hdr) != k_locator_stub_varlena_bytes) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: locator stub size mismatch (got {}, expected {})",
               static_cast<unsigned long long>(VARSIZE(hdr)),
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
[[gnu::always_inline]]
inline void validate_locator_flat_materialization_length(
    const sequence_locator& loc)
{
    using error = flat_locator_validation_error;
    switch (validate_sequence_locator(loc)) {
        case error::none:
            return;
        case error::logical_length_exceeds_bigint:
            ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq locator logical length exceeds bigint")));
        case error::logical_length_exceeds_flat_field:
            ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq sequence is too long to flatten")));
        case error::invalid_wire_metadata:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator wire metadata is invalid")));
        case error::shard_exceeds_catalog_domain:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator shard exceeds the catalog domain")));
        case error::first_row_exceeds_catalog_domain:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator first fragment row exceeds bigint")));
        case error::fragment_geometry_inconsistent:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator fragment geometry is inconsistent"),
                 errdetail("logical length=%llu fragments=%u first row=%llu",
                     static_cast<unsigned long long>(loc.logical_length),
                     loc.fragment_count,
                     static_cast<unsigned long long>(loc.first_fragment_row))));
        case error::fragment_row_range_exceeds_catalog_domain:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator fragment row range exceeds bigint")));
    }
    pg_unreachable();
}

[[gnu::always_inline]]
inline void validate_sequence_fragment_frame(
    Size raw_bytes, Size expected_bytes, std::uint64_t row_no)
{
    using error = fragment_frame_validation_error;
    switch (validate_fragment_frame_size(
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

[[gnu::always_inline]]
inline void validate_sequence_fragment_metadata(
    const bioseq::pg_types::seq* raw,
    std::uint8_t expected_alphabet,
    std::uint8_t expected_width,
    std::uint64_t expected_bp_size,
    std::uint64_t expected_padding,
    std::uint64_t row_no)
{
    using error = fragment_metadata_validation_error;
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

[[gnu::always_inline]]
inline void encode_locator_into_stub(
    bioseq::pg_types::seq* dst,
    std::uint8_t logical_alphabet_code,
    std::uint8_t logical_byte_width,
    std::uint16_t padding_letters,
    sequence_size_t total_bp_size,
    const sequence_locator& loc)
{
    if (logical_byte_width == 0 || logical_byte_width > 0x0Fu
        || loc.payload_version != sequence_locator_payload_version || loc.source_epoch == 0
        || loc.reserved_16 != 0 || loc.reserved_32 != 0) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: locator stub input is outside the revision-3 wire domain");
    }
    std::memset(dst, 0, k_locator_stub_varlena_bytes);
    SET_VARSIZE(dst, k_locator_stub_varlena_bytes);
    dst->alphabet_code = static_cast<bioseq::pg_types::seq::alphabet_code_t>(logical_alphabet_code);
    dst->storage_unit_byte_width = static_cast<std::uint8_t>(
        sequence_locator_stub_width_marker | logical_byte_width);
    dst->padding_letters = padding_letters;
    constexpr sequence_size_t k_base_pair_size_field_maximum =
        static_cast<sequence_size_t>(std::numeric_limits<bioseq::pg_types::seq::bp_size_t>::max());
    dst->bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(
        std::min<sequence_size_t>(
            total_bp_size, k_base_pair_size_field_maximum));
    const auto payload = std::span<std::byte>{
        reinterpret_cast<std::byte*>(dst->data), sizeof(sequence_locator)};
    if (!encode_sequence_locator_payload(payload, loc)) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: failed to encode canonical revision-3 locator payload");
    }
}

[[nodiscard]] [[gnu::always_inline]]
inline bioseq::pg_types::seq* alloc_locator_stub_in(
    MemoryContext mctx,
    std::uint8_t logical_alphabet_code,
    std::uint8_t logical_byte_width,
    std::uint16_t padding_letters,
    sequence_size_t total_bp_size,
    const sequence_locator& loc)
{
    bioseq::postgres::support::memory_context_scope ctx_guard{mctx};
    bioseq::pg_types::seq* dst = static_cast<bioseq::pg_types::seq*>(
        palloc0(k_locator_stub_varlena_bytes));
    encode_locator_into_stub(dst, logical_alphabet_code, logical_byte_width,
                             padding_letters, total_bp_size, loc);
    return dst;
}

// ---------------------------------------------------------------------------
// Bucket math. The consteval threshold builder, len_log2_for_size, the
// branchless L->bucket LUT and sequence_bucket_index now live in the PG-free
// `bioseq_bucket_math` module and are re-exported above via
// `export import bioseq_bucket_math`. The template below delegates to them.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Per-scan / per-batch relation cache. Owned by column_scan_descriptor,
// index_fetch_state, column_write_state and tuple_dispatch_buffer.
// Always allocated separately (`palloc(sizeof(seq_caches)) + placement-new`)
// because std::map's non-standard-layout would otherwise spoil the surrounding
// POD struct's std::is_standard_layout_v guarantee.
// ---------------------------------------------------------------------------

struct seq_caches {
    struct entry {
        Relation rel = nullptr;
        column_write_state* write_state = nullptr;
        column_read_state*  read_state  = nullptr;
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

    // A sorted-vector map avoids per-node allocation, preserves deterministic
    // allocation, deterministic sorted iteration order for close_all, and
    // cache-friendly for the typical <50-entry working set. The hot paths only
    // do find / operator[] / clear, and never hold an `entry&` across an insert.
    entry_map entries{};
    LOCKMODE lock_mode = AccessShareLock;

    enum class access_side { read, write };
    access_side side = access_side::read;

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
// dependent consumers (column_scan_descriptor, column_write_state, ...)
// never see a half-constructed map. The header type stays
// reinterpret_cast-safe via void* opaque pointers in the consumer structs.
//
// destroy_seq_caches is NOT inline because write_state_management.cppm
// forward-declares it to avoid `import bioseq_storage` (which would
// create a module cycle with bioseq_table). Out-of-line gives it external
// linkage so the linker resolves the forward declaration.
[[nodiscard]] inline seq_caches* alloc_seq_caches(MemoryContext mctx) {
    bioseq::postgres::support::memory_context_scope ctx_guard{mctx};
    void* raw = palloc0(sizeof(seq_caches));
    return ::new (raw) seq_caches{};
}

void destroy_seq_caches(seq_caches* caches) noexcept;

[[gnu::always_inline]] inline void close_seq_caches(seq_caches& caches,
                                                    bool commit) noexcept {
    caches.close_all(commit);
}

void close_and_destroy_seq_caches(seq_caches* caches, bool commit) noexcept;

// Public alias for the per-fragment storage-unit count
// (detail::k_fragment_storage_units below aliases this so the two never drift).
// `bioseq.load`'s streaming sink uses it to size fragment payloads.
inline constexpr std::size_t k_sequence_fragment_storage_units =
    std::size_t{1024};

// Flush every open sub-table write state owned by `caches` so an
// in-statement index scan (e.g. the batched update_index `bioseq.load` runs
// after streaming) sees the just-written fragment rows. Unlike `close_all`,
// this only flushes pending columnar writes (it does NOT pop/close the write
// state); the central xact-commit teardown still owns lifecycle.
void flush_seq_cache_write_states(seq_caches* caches,
                                  SubTransactionId current_sub_xid);

/// Bounded streaming fragment writer for one sequence. Accepts
/// column-native packed bytes, writes full fragment rows (each
/// `k_sequence_fragment_storage_units` storage units) as its internal buffer fills,
/// retains only the final partial fragment until `finish`, and returns the
/// current `sequence_locator`. Peak extra memory is one ≤8 KiB fragment buffer + one
/// `bioseq::pg_types::seq` fragment row, so a >MaxAllocSize record never materialises as one
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
    [[nodiscard]] pending_sequence_source finish(sequence_size_t total_bp,
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

struct sequence_column_type_parameters {
    std::string alphabet_name;
    int storage_unit_bit_width = 0;
};

[[nodiscard]] sequence_column_type_parameters
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
// sequence_storage_manager template.
// ---------------------------------------------------------------------------

template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
struct sequence_storage_manager {
    using len_t = p_length_type;
    static constexpr std::size_t bucket_min_len_bits =
        p_bucket_minimum_length_bits;
    static constexpr std::size_t bucket_max_len_bits =
        p_bucket_maximum_length_bits;
    static constexpr std::size_t total_len_bits      = sizeof(len_t) * 8u;
    static constexpr auto table_seq_buckets_len_thresholds =
        generate_seq_buckets_len_thresholds<
            len_t, bucket_min_len_bits, bucket_max_len_bits>();
    static constexpr std::size_t bucket_count =
        table_seq_buckets_len_thresholds.size();

    static_assert(bucket_count <= std::numeric_limits<std::uint8_t>::max(),
                  "sequence_locator::bucket_idx is uint8_t; raise the locator "
                  "wire revision before configuring more than 255 buckets");

    // Delegate to the PG-free template so production and
    // the unit-test TU resolve to identical bucketing code (the LUT itself is
    // bucket_lut_v in bioseq_bucket_math).
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
    [[nodiscard]] static bioseq::pg_types::seq*
    fetch_payload(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const sequence_locator& loc,
                  Snapshot snapshot,
                  MemoryContext out_mctx, seq_caches* cache);

    // Read path: bulk per-bucket.
    struct fetch_request {
        std::size_t out_slot = 0;
        sequence_locator locator{};
    };

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
    static void validate_drop_catalog_contracts();

    static void
    drop_sub_tables_for_main(
        Oid db_oid, Oid spc_oid, Oid main_relfilenode,
        bool allow_missing_during_extension_teardown = false);
};

using bioseq_default_storage_manager =
    sequence_storage_manager<sequence_size_t, /*min=*/8, /*max=*/42>;

// Exported entry point so the source-backed adapter
// can resolve a sub-table relation via the
// per-scan `seq_caches`. Implementation lives below in the .cppm body.
[[nodiscard]] Relation
open_sub_table_for_locator(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                           std::int16_t column_attno, const sequence_locator& loc,
                           LOCKMODE lock_mode, seq_caches* cache);

/// True only when `relid` is the sub-table relation of an installed
/// seq_sub_table catalog row.  VACUUM and mutation guards use catalog
/// registration rather than trusting the relation-name convention.
[[nodiscard]] bool is_registered_sequence_fragment_relation(
    Oid relid,
    bool allow_missing_during_extension_teardown = false);

// ---------------------------------------------------------------------------
// Sidecar I/O. Per-shard heap relations co-sharded with sub-tables.
// ---------------------------------------------------------------------------

void
sidecar_write_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                   std::int16_t column_attno, const sequence_locator& loc,
                   std::span<const std::uint8_t> blob, seq_caches* cache);

[[nodiscard]] std::optional<std::vector<char>>
sidecar_read_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const sequence_locator& loc,
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
// Catalog reaper and cascade-drop entry point.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// TAM hot-path helper for write-state cap-snapshot read.
// ---------------------------------------------------------------------------

[[nodiscard]] sequence_size_t read_max_sub_table_cap_bytes();

}  // namespace bioseq::storage

// ============================================================================
// Implementation
// ============================================================================

namespace bioseq::storage {

constexpr std::int64_t k_sequence_storage_advisory_lock_class = 0x42105260LL;

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

sequence_column_type_parameters column_typmod_at(TupleDesc parent_tupdesc, std::int16_t column_attno) {
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
                "bioseq storage: SEQ column attno={} has no typmod; "
                "requires every bioseq.seq column to carry an "
                "(alphabet, width) typmod",
                column_attno));
    }
    const compact_sequence_stdc_type_params params =
        compact_sequence_stdc_type_params::decode(typmod);
    std::string_view name_sv = bioseq::pg_types::seq::name_of_alphabet_code(params.alphabet_code);
    if (name_sv == std::string_view{"?"}) {
        storage_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            fmt::format(
                "bioseq storage: alphabet_code={} not in "
                "pgseq_supported_alphabets",
                static_cast<unsigned>(params.alphabet_code)));
    }
    sequence_column_type_parameters out;
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
    Relation rel = bioseq::postgres::support::try_open_postgres_relation(main_relid, AccessShareLock);
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
    const int guc = std::atomic_ref<int>(bioseq::storage::settings::sequence_shard::max_size_gib)
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

                if (side == access_side::write) {
                    if (commit && e.pending_bytes_delta != 0) {
                        (void) bioseq::postgres::support::invoke_pg_as_cpp([&] {
                            return sequence_sub_table_manager::update_shard_bytes_written(
                                main_database_oid, main_tablespace_oid,
                                main_relfilenode, column_attno, bucket_idx,
                                shard_idx, e.pending_bytes_delta,
                                cap_bytes_snapshot);
                        });
                    }
                    e.pending_bytes_delta = 0;
                    e.pending_total_payload_delta = 0;
                    // The shard write state remains owned by write_state_cache and
                    // commits with the parent; closing it here double-flushes.
                    e.write_state = nullptr;
                } else {
                    if (e.read_state != nullptr) {
                        column_read_state* read_state = e.read_state;
                        const ResourceOwner saved_owner = CurrentResourceOwner;
                        bioseq::postgres::support::invoke_pg_as_cpp([read_state,
                                                           saved_owner,
                                                           this] {
                            if (read_resource_owner != nullptr) {
                                CurrentResourceOwner = read_resource_owner;
                            }
                            end_column_read(read_state);
                            CurrentResourceOwner = saved_owner;
                        });
                        e.read_state = nullptr;
                    }
                    e.read_snapshot_source = nullptr;
                }
                if (e.rel != nullptr) {
                    Relation relation = e.rel;
                    if (side == access_side::write) {
                        const ResourceOwner saved_owner = CurrentResourceOwner;
                        bioseq::postgres::support::invoke_pg_as_cpp(
                            [relation, saved_owner, this] {
                                CurrentResourceOwner =
                                    TopTransactionResourceOwner;
                                relation_close(relation, lock_mode);
                                CurrentResourceOwner = saved_owner;
                            });
                    } else if (read_resource_owner != nullptr) {
                        const ResourceOwner saved_owner = CurrentResourceOwner;
                        bioseq::postgres::support::invoke_pg_as_cpp(
                            [relation, saved_owner, this] {
                                CurrentResourceOwner = read_resource_owner;
                                relation_close(relation, lock_mode);
                                CurrentResourceOwner = saved_owner;
                            });
                    } else {
                        bioseq::postgres::support::invoke_pg_as_cpp(
                            [relation, this] {
                                relation_close(relation, lock_mode);
                            });
                    }
                    e.rel = nullptr;
                }
            }
            entries.clear();
        } catch (bioseq::postgres::support::postgres_error& error) {
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

// Flush, but do not pop or close, every open sub-table write state
// so a same-statement index scan sees the just-written fragment rows.
// `flush_relation_write_state` is keyed on the shard relation's relfilenode.
void flush_seq_cache_write_states(seq_caches* caches,
                                  SubTransactionId current_sub_xid) {
    if (caches == nullptr || caches->side != seq_caches::access_side::write) return;
    for (auto& kv : caches->entries) {
        seq_caches::entry& e = kv.second;
        if (e.rel != nullptr) {
            flush_relation_write_state(e.rel->rd_locator.relNumber,
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
        // The resource owner with which the shard Relation is
        // pinned under MUST match the owner that is current when
        // `seq_caches::close_all` runs `relation_close()`. That owner differs
        // by side:
        //
        //   * Write side: the seq_caches lives on the transaction-scoped
        //     column_write_state and is closed by
        //     pop_all_column_write_states at xact commit, under
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
        if (cache->side == seq_caches::access_side::write) {
            const ResourceOwner saved_owner = CurrentResourceOwner;
            rel = bioseq::postgres::support::invoke_pg_as_cpp(
                [sub_table_relid, lock_mode, saved_owner] {
                    CurrentResourceOwner = TopTransactionResourceOwner;
                    Relation result = bioseq::postgres::support::try_open_postgres_relation(
                        sub_table_relid, lock_mode);
                    CurrentResourceOwner = saved_owner;
                    return result;
                });
        } else if (cache->read_resource_owner != nullptr) {
            const ResourceOwner saved_owner = CurrentResourceOwner;
            rel = bioseq::postgres::support::invoke_pg_as_cpp(
                [sub_table_relid, lock_mode, saved_owner, cache] {
                    CurrentResourceOwner = cache->read_resource_owner;
                    Relation result = bioseq::postgres::support::try_open_postgres_relation(
                        sub_table_relid, lock_mode);
                    CurrentResourceOwner = saved_owner;
                    return result;
                });
        } else {
            rel = bioseq::postgres::support::invoke_pg_as_cpp(
                [sub_table_relid, lock_mode] {
                    return bioseq::postgres::support::try_open_postgres_relation(
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
    Relation rel = bioseq::postgres::support::invoke_pg_as_cpp(
        [sub_table_relid, lock_mode] {
            return bioseq::postgres::support::try_open_postgres_relation(sub_table_relid, lock_mode);
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

/// Creates one lazy shard pair inside a rollback-safe SQL subtransaction.
void create_shard_relations_checked(
        StringInfo ddl,
        const char* sub_name,
        const char* sidecar_name,
        Oid& new_sub_relid,
        Oid& new_sidecar_relid) {
    auto subtransaction =
        bioseq::postgres::support::subtransaction::begin();
    bioseq::postgres::support::spi_session spi;
    const int connect_status = spi.connect();
    if (connect_status != SPI_OK_CONNECT) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            fmt::format(
                "bioseq storage: SPI_connect failed while allocating "
                "shard (status={})",
                connect_status));
    }
    const int execute_status =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [ddl] {
                return SPI_execute(
                    ddl->data, /*read_only=*/false, /*tcount=*/0);
            });
    if (execute_status != SPI_OK_UTILITY
        && execute_status != SPI_OK_SELECT) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            fmt::format(
                "bioseq storage: failed to create sub-table {} and "
                "sidecar {} (SPI status={})",
                sub_name,
                sidecar_name,
                execute_status));
    }
    const int finish_status = spi.finish();
    if (finish_status != SPI_OK_FINISH) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            fmt::format(
                "bioseq storage: SPI_finish failed while allocating "
                "shard (status={})",
                finish_status));
    }
    const auto [created_sub_relid, created_sidecar_relid] =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [sub_name, sidecar_name] {
                const Oid namespace_oid =
                    get_namespace_oid("bioseq_internal", false);
                return std::pair{
                    get_relname_relid(sub_name, namespace_oid),
                    get_relname_relid(sidecar_name, namespace_oid)};
            });
    if (!OidIsValid(created_sub_relid)
        || !OidIsValid(created_sidecar_relid)) {
        storage_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            "bioseq storage: sub-table or sidecar not found after CREATE");
    }
    subtransaction.commit();
    new_sub_relid = created_sub_relid;
    new_sidecar_relid = created_sidecar_relid;
}

[[nodiscard]] Relation
allocate_new_shard_and_open(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                            std::int16_t column_attno, TupleDesc parent_tupdesc,
                            std::int16_t bucket_idx, std::int32_t starting_shard_idx,
                            LOCKMODE lock_mode, seq_caches* cache)
{
    const std::int64_t lock_key =
        shard_alloc_lock_key(db_oid, main_relfilenode, column_attno, bucket_idx);
    bioseq::postgres::support::invoke_pg_as_cpp([lock_key] {
        (void) DirectFunctionCall1(
            pg_advisory_xact_lock_int8, Int64GetDatum(lock_key));
    });

    if (auto rechecked = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return sequence_sub_table_manager::lookup_active_shard(
                db_oid, spc_oid, main_relfilenode,
                column_attno, bucket_idx);
        });
        rechecked.has_value() && rechecked->shard_idx >= starting_shard_idx)
    {
        return open_relation_cached(rechecked->sub_table_relid, lock_mode, cache,
                                    column_attno, bucket_idx, rechecked->shard_idx);
    }

    const sequence_column_type_parameters tm = bioseq::postgres::support::invoke_pg_as_cpp(
        [parent_tupdesc, column_attno] {
            return column_typmod_at(parent_tupdesc, column_attno);
        });
    const char* const persistence_kw = bioseq::postgres::support::invoke_pg_as_cpp(
        [db_oid, spc_oid, main_relfilenode] {
            return persistence_keyword_for_relfilenode(
                db_oid, spc_oid, main_relfilenode);
        });

    const std::string sub_name = format_sub_table_name(
        db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx, starting_shard_idx);
    const std::string sidecar_name = format_sidecar_name(
        db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx, starting_shard_idx);

    // Lazily create the sub-table and matching sidecar.
    // We deliberately do NOT register the freshly-created relations with the
    // extension via ALTER EXTENSION ADD TABLE here: doing so requires a
    // SECURITY DEFINER plpgsql helper that issues ALTER EXTENSION inside SPI
    // inside the parent INSERT's access-method callback. That nests three
    // ResourceOwner layers (executor → SPI → SECURITY DEFINER) and Postgres
    // refuses to clean up the inner TupleDescs at xact end ("tupdesc
    // reference is not owned by resource owner TopTransaction" → PANIC).
    // The bioseq_internal.seq_sub_table catalog row is the canonical
    // tracker for cleanup (the column table drop hook /
    // bioseq_relation_set_new_filenode + drop_sub_tables_for_main); the
    // ALTER EXTENSION ADD TABLE was a pg_dump convenience that we trade
    // for correctness — pg_dump still round-trips data through SELECT/COPY.
    StringInfo ddl = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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

    sequence_sub_table_row row = bioseq::postgres::support::invoke_pg_as_cpp([&] {
        return sequence_sub_table_manager::insert_shard(
            db_oid, spc_oid, main_relfilenode,
            column_attno, bucket_idx, starting_shard_idx,
            new_sub_relid, new_sidecar_relid);
    });

    if (row.sub_table_relid != new_sub_relid) {
        // Another backend won the race; we DROP what we just made.
        // Reaper is the long-tail safety net.
        StringInfo drop = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            StringInfo result = makeStringInfo();
            appendStringInfo(result,
                "DROP TABLE bioseq_internal.%s; "
                "DROP TABLE bioseq_internal.%s;",
                quote_identifier(sub_name.c_str()),
                quote_identifier(sidecar_name.c_str()));
            return result;
        });
        struct race_drop_spi_state {
            int connect_status = 0;
            int execute_status = 0;
            int finish_status = 0;
        } spi{};
        const char* drop_sql = drop->data;
        bioseq::postgres::support::invoke_pg_as_cpp([&spi, drop_sql] noexcept {
            spi.connect_status = SPI_connect();
            if (spi.connect_status != SPI_OK_CONNECT) return;
            spi.execute_status = SPI_execute(
                drop_sql, /*read_only=*/false, /*tcount=*/0);
            spi.finish_status = SPI_finish();
        });
        if (spi.connect_status != SPI_OK_CONNECT
            || spi.execute_status != SPI_OK_UTILITY
            || spi.finish_status != SPI_OK_FINISH) {
            bioseq::postgres::support::invoke_pg_as_cpp([&spi] {
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
    if (auto active = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return sequence_sub_table_manager::lookup_active_shard(
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
    // shard full at commit. In case (b), shard 0 and any following shard
    // already exist, so starting at shard_idx 0 would re-INSERT an existing
    // catalog PK ("duplicate key violates seq_sub_table_pkey"). Allocate the
    // next index after the highest existing shard for this (column, bucket).
    std::int32_t next_shard_idx = 0;
    const auto shards = bioseq::postgres::support::invoke_pg_as_cpp([&] {
        return sequence_sub_table_manager::list_shards_for_main(
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
                           std::int16_t column_attno, const sequence_locator& loc,
                           LOCKMODE lock_mode, seq_caches* cache)
{
    if (cache != nullptr) {
        const std::uint64_t key = seq_caches::key_for(
            column_attno,
            static_cast<std::int16_t>(loc.bucket_index),
            static_cast<std::int32_t>(loc.shard_index));
        auto it = cache->entries.find(key);
        if (it != cache->entries.end() && it->second.rel != nullptr) {
            return it->second.rel;
        }
    }
    auto row = sequence_sub_table_manager::lookup_shard(db_oid, spc_oid, main_relfilenode,
        column_attno, static_cast<std::int16_t>(loc.bucket_index),
        static_cast<std::int32_t>(loc.shard_index));
    if (!row.has_value()) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: locator references nonexistent shard "
               "(db={}, spc={}, relfilenode={}, attno={}, bucket={}, shard={})",
               static_cast<unsigned>(db_oid),
               static_cast<unsigned>(spc_oid),
               static_cast<unsigned>(main_relfilenode),
               static_cast<int>(column_attno),
               static_cast<unsigned>(loc.bucket_index),
               loc.shard_index);
    }
    return open_relation_cached(row->sub_table_relid, lock_mode, cache,
                                column_attno,
                                static_cast<std::int16_t>(loc.bucket_index),
                                static_cast<std::int32_t>(loc.shard_index));
}

namespace fragment_registry_detail {

struct attribute_contract {
    AttrNumber attno = InvalidAttrNumber;
    std::string_view name{};
    Oid type_oid = InvalidOid;
};

inline constexpr std::array<attribute_contract, 11> k_attribute_contracts{{
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
    bioseq::postgres::support::invoke_pg_as_cpp([&owned] {
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
            != static_cast<int>(k_attribute_contracts.size())) {
        contract_error("attribute count changed");
    }
    for (const attribute_contract& expected : k_attribute_contracts) {
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
    Relation catalog = bioseq::postgres::support::try_open_postgres_relation(catalog_oid, AccessShareLock);
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
    // The schema validation above pins sub_table_relid to ordinal 7. This is
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
// insert_sequence_streamed write hot path.
//
// One sequence → ceil(total_bp / fragment_bp) sub-table rows. Producer is
// invoked once per fragment in a per-fragment MemoryContext that is reset
// between fragments so peak RSS is bounded by one fragment's worth of state
// Per-sequence shard-rollover pre-allocation keeps a sequence's fragments in a
// single shard so the locator's
// shard_idx is a single value.
// ---------------------------------------------------------------------------

namespace detail {

// Per-fragment storage-unit count. The class member from
// `resident_sequence_scanner<T>::fragment_storage_units` is 1024. This aliases
// the exported `k_sequence_fragment_storage_units` so the
// public constant and this hot-path detail constant can never drift.
inline constexpr std::size_t k_fragment_storage_units =
    k_sequence_fragment_storage_units;

// Look-up bits_per_char via the supported-alphabet pack so a future alphabet
// addition does not silently produce wrong fragment_bp values.
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
[[nodiscard]] inline std::size_t letters_per_unit_for(
    bioseq::pg_types::seq::alphabet_code_t alpha, std::uint8_t logical_byte_width) noexcept {
    const std::size_t bpc = bits_per_char_for_alphabet_code(alpha);
    return bpc > 0
        ? (static_cast<std::size_t>(logical_byte_width) * 8u) / bpc
        : 0u;
}

/// Feed the exact logical symbol bytes used by the existing revision-3
/// independent checksum oracle. Fragment-sized decoding keeps memory bounded
/// while moving the O(N) work to insertion rather than a cold index read.
inline void update_canonical_source_checksum(
        bioseq::indexing::crc64_ecma182_accumulator& checksum,
        const bioseq::pg_types::seq* fragment) {
    if (fragment == nullptr || is_locator_stub(fragment)) {
        source_version_detail::source_catalog_error(
            "canonical checksum received a non-fragment value");
    }
    fragment->convert_to([&checksum]<class p_alphabet>(
            const compact_sequence_range<p_alphabet>& range) {
        const std::string decoded =
            ::bioseq::simd::active_target::decode_sequence_simd<p_alphabet>(
                range);
        if (decoded.size() != range.bp_size()) {
            source_version_detail::source_catalog_error(
                "canonical checksum decoded length disagrees");
        }
        checksum.update(std::string_view{decoded});
    });
}

}  // namespace detail

template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
template<class p_fragment_producer>
pending_sequence_source
sequence_storage_manager<
    p_length_type,
    p_bucket_minimum_length_bits,
    p_bucket_maximum_length_bits>::insert_sequence_streamed(
    Oid user_relid, std::uint64_t main_storage_id,
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, TupleDesc parent_tupdesc,
    std::uint8_t logical_alphabet_code,
    std::uint8_t logical_storage_unit_byte_width,
    sequence_size_t total_bp_size,
    std::uint16_t last_fragment_padding_letters,
    sequence_size_t current_cap_bytes,
    seq_caches* cache,
    p_fragment_producer&& producer)
{
    if (!OidIsValid(user_relid) || main_storage_id == 0) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq storage: source epoch requires parent relation/storage identity");
    }
    if (cache == nullptr || cache->side != seq_caches::access_side::write) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq storage: insert_sequence_streamed requires a write-side seq_caches");
    }
    if (cache->main_database_oid == InvalidOid) {
        cache->main_database_oid   = db_oid;
        cache->main_tablespace_oid = spc_oid;
        cache->main_relfilenode    = main_relfilenode;
    }

    const auto alpha_code = static_cast<bioseq::pg_types::seq::alphabet_code_t>(logical_alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha_code,
                                                         logical_storage_unit_byte_width);
    if (lpu == 0) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            fmt::format(
                "bioseq storage: insert_sequence_streamed could not derive "
                "letters_per_unit (alphabet_code={}, byte_width={})",
                static_cast<unsigned>(alpha_code),
                static_cast<unsigned>(logical_storage_unit_byte_width)));
    }
    const sequence_size_t frag_bp =
        static_cast<sequence_size_t>(detail::k_fragment_storage_units) * lpu;
    const sequence_size_t num_fragments =
        total_bp_size == 0 ? 0u : (total_bp_size + frag_bp - 1) / frag_bp;
    if (num_fragments > static_cast<sequence_size_t>(
            std::numeric_limits<std::uint32_t>::max())) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            fmt::format(
                "bioseq storage: fragment count {} exceeds UINT32_MAX",
                num_fragments));
    }

    // Per-row varlena bytes for a full (= non-last) fragment.
    const std::size_t frag_varlena_bytes =
        bioseq::pg_types::seq::header_bytes()
        + detail::k_fragment_storage_units
              * static_cast<std::size_t>(logical_storage_unit_byte_width);
    // Total varlena bytes the sequence occupies in the shard (= sum of
    // fragment-row sizes). Used for the per-shard cap accounting; each row
    // has the same size because the last fragment row is also
    // allocated at `frag_varlena_bytes` (palloc0 leaves trailing units 0).
    const sequence_size_t total_varlena_bytes =
        static_cast<sequence_size_t>(num_fragments) *
        static_cast<sequence_size_t>(frag_varlena_bytes);

    const std::int16_t bucket_idx =
        static_cast<std::int16_t>(sequence_bucket_index(total_bp_size));

    const std::uint64_t source_epoch =
        sequence_source_version_manager::allocate_epoch();
    bioseq::indexing::crc64_ecma182_accumulator source_checksum{};

    if (num_fragments == 0) {
        sequence_locator empty_locator{};
        empty_locator.payload_version = sequence_locator_payload_version;
        empty_locator.bucket_index = static_cast<std::uint8_t>(bucket_idx);
        empty_locator.shard_index = 0;
        empty_locator.fragment_count = 0;
        empty_locator.logical_length = 0;
        empty_locator.first_fragment_row = 0;
        empty_locator.source_epoch = source_epoch;
        sequence_source_version_manager::insert_unpublished(
            sequence_source_version_row{
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
                .state = k_sequence_source_state_unpublished,
            });
        return pending_sequence_source{empty_locator};
    }

    // Per-sequence shard pre-allocation. If the active shard
    // cannot fit the WHOLE sequence's bytes, allocate a fresh shard up front.
    Relation shard = open_or_create_shard(db_oid, spc_oid, main_relfilenode,
                                          column_attno, parent_tupdesc, bucket_idx,
                                          RowExclusiveLock, cache);

    auto active = bioseq::postgres::support::invoke_pg_as_cpp(
        [db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx] {
            return sequence_sub_table_manager::lookup_active_shard(
                db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx);
        });
    if (!active.has_value()) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq storage: active shard disappeared mid-insert");
    }
    const std::uint64_t active_key =
        seq_caches::key_for(column_attno, bucket_idx, active->shard_idx);
    sequence_size_t pending = 0;
    if (auto it = cache->entries.find(active_key); it != cache->entries.end()) {
        pending = it->second.pending_bytes_delta;
    }
    const sequence_size_t projected =
        active->bytes_written + pending + total_varlena_bytes;
    // Pre-allocate a fresh shard when the active one cannot fit the whole
    // sequence. This follows the normal per-row rollover semantics,
    // but the unit of accounting is the whole sequence now.
    const bool rollover_required =
        active->bytes_written + pending > 0
        && (current_cap_bytes == 0 || projected > current_cap_bytes);

    std::int32_t shard_idx = 0;
    if (rollover_required) {
        if (auto it = cache->entries.find(active_key);
            it != cache->entries.end() && it->second.pending_bytes_delta != 0)
        {
            const std::int32_t active_shard_idx = active->shard_idx;
            const sequence_size_t pending_bytes =
                it->second.pending_bytes_delta;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [db_oid, spc_oid, main_relfilenode, column_attno,
                 bucket_idx, active_shard_idx, pending_bytes,
                 current_cap_bytes] {
                    (void) sequence_sub_table_manager::update_shard_bytes_written(
                        db_oid, spc_oid, main_relfilenode,
                        column_attno, bucket_idx, active_shard_idx,
                        pending_bytes, current_cap_bytes);
                });
            it->second.pending_bytes_delta = 0;
            it->second.pending_total_payload_delta = 0;
        } else {
            const std::int32_t active_shard_idx = active->shard_idx;
            const sequence_size_t effective_cap =
                current_cap_bytes == 0
                ? sequence_size_t{1}
                : current_cap_bytes;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [db_oid, spc_oid, main_relfilenode, column_attno,
                 bucket_idx, active_shard_idx, effective_cap] {
                    (void) sequence_sub_table_manager::update_shard_bytes_written(
                        db_oid, spc_oid, main_relfilenode,
                        column_attno, bucket_idx, active_shard_idx,
                        /*delta_bytes=*/0, effective_cap);
                });
        }
        shard = allocate_new_shard_and_open(db_oid, spc_oid, main_relfilenode,
                                            column_attno, parent_tupdesc, bucket_idx,
                                            active->shard_idx + 1,
                                            RowExclusiveLock, cache);
        shard_idx = active->shard_idx + 1;
    } else {
        shard_idx = active->shard_idx;
    }

    // Refuse to write a sequence whose total bytes
    // exceed the cap (would not fit any shard, including a freshly-allocated
    // one). current_cap_bytes==0 is the sentinel "no cap, one shard per
    // sequence" and the check is suppressed.
    if (current_cap_bytes > 0 && total_varlena_bytes > current_cap_bytes) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            fmt::format(
                "bioseq storage: sequence with {} bytes exceeds shard cap "
                "{} bytes ({} GiB). Raise "
                "bioseq.storage.sequence_shard.max_size_gib.",
                total_varlena_bytes, current_cap_bytes,
                current_cap_bytes >> 30));
    }

    const std::uint64_t cache_key =
        seq_caches::key_for(column_attno, bucket_idx, shard_idx);
    seq_caches::entry& entry = cache->entries[cache_key];
    entry.rel = shard;

    if (entry.write_state == nullptr) {
        const SubTransactionId current_subtransaction =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return GetCurrentSubTransactionId(); });
        entry.write_state = bioseq::postgres::support::invoke_pg_as_cpp(
            [shard, current_subtransaction] {
                return initialize_column_write_state(
                    shard, RelationGetDescr(shard),
                    RelationGetRelid(shard),
                    current_subtransaction);
            });
    }

    // Serialize writers on this parent, column, bucket, and shard
    // shard) so the row numbers write_column_row assigns are CONTIGUOUS
    // within one sequence's fragments.
    const std::int64_t row_lock_key =
          (static_cast<std::int64_t>(db_oid)                << 48)
        ^ (static_cast<std::int64_t>(main_relfilenode)      << 16)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(column_attno))    << 8)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(bucket_idx))      << 4)
        ^  static_cast<std::int64_t>(shard_idx);
    bioseq::postgres::support::invoke_pg_as_cpp([row_lock_key] {
        (void) DirectFunctionCall1(
            pg_advisory_xact_lock_int8, Int64GetDatum(row_lock_key));
    });

    // Use separate scratch and per-fragment contexts. The producer
    // may allocate long-lived state in `scratch_mctx`; per-fragment palloc
    // happens in `per_fragment_mctx` which is RESET between fragments to
    // keep peak RSS bounded.
    std::uint64_t first_sub_row_no = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t last_sub_row_no  = std::numeric_limits<std::uint64_t>::max();
    MemoryContext scratch_mctx = nullptr;
    MemoryContext per_fragment_mctx = nullptr;
    std::exception_ptr fragment_failure;
    try {
        const MemoryContext parent_mctx = CurrentMemoryContext;
        scratch_mctx = bioseq::postgres::support::invoke_pg_as_cpp([parent_mctx] {
            return AllocSetContextCreate(
                parent_mctx, "bioseq insert_sequence scratch",
                ALLOCSET_DEFAULT_SIZES);
        });
        per_fragment_mctx =
            bioseq::postgres::support::invoke_pg_as_cpp([parent_mctx] {
                return AllocSetContextCreate(
                    parent_mctx, "bioseq insert_sequence per-fragment",
                    ALLOCSET_DEFAULT_SIZES);
            });

        for (sequence_size_t fragment_no = 0;
             fragment_no < num_fragments;
             ++fragment_no) {
            bioseq::pg_types::seq* frag =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [per_fragment_mctx, frag_varlena_bytes] {
                        MemoryContextReset(per_fragment_mctx);
                        const MemoryContext previous =
                            MemoryContextSwitchTo(per_fragment_mctx);
                        auto* result =
                            static_cast<bioseq::pg_types::seq*>(
                                palloc0(frag_varlena_bytes));
                        MemoryContextSwitchTo(previous);
                        return result;
                    });
            const bool is_last = (fragment_no + 1 == num_fragments);
            bioseq::postgres::support::memory_context_scope frag_ctx{
                per_fragment_mctx};

            SET_VARSIZE(frag, frag_varlena_bytes);
            frag->alphabet_code =
                static_cast<bioseq::pg_types::seq::alphabet_code_t>(
                    logical_alphabet_code);
            frag->storage_unit_byte_width =
                logical_storage_unit_byte_width;
            // Producer fills bp_size + padding_letters + data[] for fragment `n`.
            std::forward<p_fragment_producer>(producer)(
                static_cast<std::size_t>(fragment_no),
                frag,
                scratch_mctx,
                is_last);

            // Cross-check the producer's bp_size / padding consistency.
            const sequence_size_t expected_bp = is_last
                ? (total_bp_size - fragment_no * frag_bp)
                : frag_bp;
            if (static_cast<sequence_size_t>(frag->bp_size)
                != expected_bp) {
                storage_error_as_cpp(
                    ERRCODE_INTERNAL_ERROR,
                    fmt::format(
                        "bioseq storage: producer bp_size mismatch on "
                        "fragment {} (got {}, expected {})",
                        fragment_no,
                        static_cast<sequence_size_t>(frag->bp_size),
                        expected_bp));
            }
            if (is_last
                && frag->padding_letters
                    != last_fragment_padding_letters) {
                storage_error_as_cpp(
                    ERRCODE_INTERNAL_ERROR,
                    fmt::format(
                        "bioseq storage: producer last-fragment "
                        "padding_letters mismatch (got {}, expected {})",
                        static_cast<unsigned>(frag->padding_letters),
                        static_cast<unsigned>(
                            last_fragment_padding_letters)));
            }
            if (!is_last && frag->padding_letters != 0) {
                storage_error_as_cpp(
                    ERRCODE_INTERNAL_ERROR,
                    fmt::format(
                        "bioseq storage: producer non-last "
                        "padding_letters must be 0 (fragment {}, got {})",
                        fragment_no,
                        static_cast<unsigned>(frag->padding_letters)));
            }

            detail::update_canonical_source_checksum(
                source_checksum, frag);

            Datum values[1] = {PointerGetDatum(frag)};
            bool nulls[1] = {false};
            const std::uint64_t row_no =
                write_column_row(
                    entry.write_state, values, nulls);
            if (fragment_no == 0) {
                first_sub_row_no = row_no;
            } else if (row_no != last_sub_row_no + 1) {
                storage_error_as_cpp(
                    ERRCODE_INTERNAL_ERROR,
                    fmt::format(
                        "bioseq storage: non-contiguous row numbers "
                        "during streamed insert (expected {}, got {})",
                        last_sub_row_no + 1, row_no));
            }
            last_sub_row_no = row_no;
        }
    } catch (...) {
        fragment_failure = std::current_exception();
    }

    const auto delete_context =
        [&fragment_failure](MemoryContext& context) {
            const MemoryContext doomed =
                std::exchange(context, nullptr);
            if (doomed == nullptr) return;
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [doomed] { MemoryContextDelete(doomed); });
            } catch (...) {
                if (fragment_failure == nullptr) {
                    fragment_failure = std::current_exception();
                }
            }
        };
    delete_context(per_fragment_mctx);
    delete_context(scratch_mctx);
    if (fragment_failure != nullptr) {
        std::rethrow_exception(fragment_failure);
    }

    entry.pending_bytes_delta         += total_varlena_bytes;
    entry.pending_total_payload_delta += total_varlena_bytes;

    if (num_fragments == 0) {
        // Empty sequence — no rows were written. Choose a deterministic row
        // number (0) as the stub's first_sub_row_no; the scanner short-
        // circuits when num_fragments == 0 and never reads this row.
        first_sub_row_no = 0;
    }

    sequence_locator out{};
    out.payload_version          = sequence_locator_payload_version;
    out.bucket_index       = static_cast<std::uint8_t>(bucket_idx);
    out.reserved_16           = 0;
    out.shard_index        = static_cast<std::uint32_t>(shard_idx);
    out.fragment_count    = static_cast<std::uint32_t>(num_fragments);
    out.reserved_32           = 0;
    out.logical_length    = total_bp_size;
    out.first_fragment_row = first_sub_row_no;
    out.source_epoch     = source_epoch;

    const Oid fragment_relid = RelationGetRelid(shard);
    const std::uint64_t fragment_storage_id =
        bioseq::postgres::support::invoke_pg_as_cpp([shard] {
            return column_storage_id(shard, false);
        });
    sequence_source_version_manager::insert_unpublished(
        sequence_source_version_row{
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
            .state = k_sequence_source_state_unpublished,
        });
    return pending_sequence_source{out};
}

template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
pending_sequence_source
sequence_storage_manager<
    p_length_type,
    p_bucket_minimum_length_bits,
    p_bucket_maximum_length_bits>::insert_payload_as_fragments(
    Oid user_relid, std::uint64_t main_storage_id,
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, TupleDesc parent_tupdesc,
    const bioseq::pg_types::seq* original_hdr,
    sequence_size_t current_cap_bytes,
    seq_caches* cache)
{
    if (original_hdr == nullptr) {
        bioseq_log<logging_level::error, logging_target::postgres>( "bioseq storage: insert_payload_as_fragments received null original_hdr");
    }
    if (is_locator_stub(original_hdr)) {
        bioseq_log<logging_level::error, logging_target::postgres>( "bioseq storage: insert_payload_as_fragments received a stub; "
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

    const auto alpha_code = static_cast<bioseq::pg_types::seq::alphabet_code_t>(logical_alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha_code, logical_byte_width);
    if (lpu == 0) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: insert_payload_as_fragments could not derive "
               "letters_per_unit (alphabet_code={}, byte_width={})",
               static_cast<unsigned>(alpha_code),
               static_cast<unsigned>(logical_byte_width));
    }
    const sequence_size_t frag_bp =
        static_cast<sequence_size_t>(detail::k_fragment_storage_units) * lpu;
    const sequence_size_t num_fragments =
        total_bp_size == 0 ? 0u : (total_bp_size + frag_bp - 1) / frag_bp;
    const std::size_t bytes_per_unit =
        static_cast<std::size_t>(logical_byte_width);
    const std::size_t fragment_bytes =
        detail::k_fragment_storage_units * bytes_per_unit;

    // Slice the original payload into N fragment rows.
    const std::uint8_t* const src_data =
        reinterpret_cast<const std::uint8_t*>(original_hdr->data);
    const std::size_t src_bytes =
        static_cast<std::size_t>(VARSIZE(original_hdr)) - bioseq::pg_types::seq::header_bytes();

    auto producer = [src_data, src_bytes, fragment_bytes, frag_bp, total_bp_size,
                     last_fragment_padding](
        std::size_t fragment_no, bioseq::pg_types::seq* dst, MemoryContext /*scratch*/,
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
        dst->bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(this_fragment_bp);
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
// sequence_stream_writer implementation. Mirrors the per-fragment
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
    if (caches == nullptr || caches->side != seq_caches::access_side::write) {
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

    const auto alpha = static_cast<bioseq::pg_types::seq::alphabet_code_t>(alphabet_code);
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
    frag_payload_bytes_ = detail::k_fragment_storage_units
        * static_cast<std::size_t>(byte_width);
    frag_varlena_bytes_ = bioseq::pg_types::seq::header_bytes() + frag_payload_bytes_;
    frag_bp_ = static_cast<sequence_size_t>(detail::k_fragment_storage_units)
        * lpu;

    buffer_.clear();
    buffer_.reserve(frag_payload_bytes_ + 256);
    first_sub_row_no_ = std::numeric_limits<std::uint64_t>::max();
    last_sub_row_no_  = std::numeric_limits<std::uint64_t>::max();
    num_fragments_ = 0;
    total_varlena_bytes_ = 0;
    source_checksum_value_ = 0;
    source_epoch_ = sequence_source_version_manager::allocate_epoch();
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
        bioseq_default_storage_manager::sequence_bucket_index(
            std::numeric_limits<sequence_size_t>::max()));
    shard_ = open_or_create_shard(
        db_oid_, spc_oid_, main_relfilenode_, column_attno_, parent_td_,
        bucket_idx_, RowExclusiveLock, caches_);
    auto active = bioseq::postgres::support::invoke_pg_as_cpp([this] {
        return sequence_sub_table_manager::lookup_active_shard(
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
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return GetCurrentSubTransactionId(); });
        entry.write_state = initialize_column_write_state(
            shard_, RelationGetDescr(shard_), RelationGetRelid(shard_),
            subtransaction);
    }
    write_state_ = entry.write_state;
    // Serialize writers on this parent, column, bucket, and shard
    // so the row numbers write_column_row assigns stay contiguous within one
    // sequence's fragments (and across the sequences of one load).
    const std::int64_t row_lock_key =
          (static_cast<std::int64_t>(db_oid_)            << 48)
        ^ (static_cast<std::int64_t>(main_relfilenode_)  << 16)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(column_attno_)) << 8)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(bucket_idx_))   << 4)
        ^  static_cast<std::int64_t>(shard_idx_);
    bioseq::postgres::support::invoke_pg_as_cpp([row_lock_key] {
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
                "bioseq.storage.sequence_shard.max_size_gib.",
                total_varlena_bytes_ + frag_varlena_bytes_,
                cap_bytes_ >> 30, num_fragments_));
    }
    // Keep the allocation in a checked leaf so palloc cannot longjmp across
    // caller-owned checksum/state objects. write_column_row provides its
    // own checked writer leaf below.
    bioseq::pg_types::seq* frag = bioseq::postgres::support::invoke_pg_as_cpp([this] {
        const MemoryContext old = MemoryContextSwitchTo(
            get_column_write_per_tuple_context(write_state_));
        bioseq::pg_types::seq* result = static_cast<bioseq::pg_types::seq*>(
            palloc0(frag_varlena_bytes_));
        MemoryContextSwitchTo(old);
        return result;
    });
    SET_VARSIZE(frag, frag_varlena_bytes_);
    frag->alphabet_code =
        static_cast<bioseq::pg_types::seq::alphabet_code_t>(alphabet_code_);
    frag->storage_unit_byte_width = byte_width_;
    frag->bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(bp);
    frag->padding_letters = pad;
    if (payload_bytes > 0) {
        std::memcpy(frag->data, payload, payload_bytes);
    }
    bioseq::indexing::crc64_ecma182_accumulator checksum{
        .value = source_checksum_value_};
    detail::update_canonical_source_checksum(checksum, frag);
    source_checksum_value_ = checksum.checksum();
    Datum values[1] = {PointerGetDatum(frag)};
    bool nulls[1] = {false};
    const std::uint64_t row_no =
        write_column_row(write_state_, values, nulls);
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

pending_sequence_source sequence_stream_writer::finish(
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

    sequence_locator out{};
    out.payload_version          = sequence_locator_payload_version;
    out.bucket_index       = static_cast<std::uint8_t>(bucket_idx_);
    out.reserved_16           = 0;
    out.shard_index        = static_cast<std::uint32_t>(shard_idx_);
    out.fragment_count    = num_fragments_;
    out.reserved_32           = 0;
    out.logical_length    = total_bp;
    out.first_fragment_row = first_sub_row_no_;
    out.source_epoch     = source_epoch_;

    const Oid fragment_relid = num_fragments_ == 0
        ? InvalidOid : RelationGetRelid(shard_);
    const std::uint64_t fragment_storage_id = num_fragments_ == 0
        ? 0 : bioseq::postgres::support::invoke_pg_as_cpp([this] {
            return column_storage_id(shard_, false);
        });
    sequence_source_version_manager::insert_unpublished(
        sequence_source_version_row{
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
            .state = k_sequence_source_state_unpublished,
        });
    active_ = false;
    return pending_sequence_source{out};
}

void sequence_stream_writer::abort() {
    // Transaction rollback reclaims any fragment rows already written; just drop
    // the in-flight buffer + state so a re-begin starts clean.
    buffer_.clear();
    active_ = false;
    source_epoch_ = 0;
}

// ---------------------------------------------------------------------------
// fetch_payload streams all fragments into a single contiguous
// bioseq::pg_types::seq* in `out_mctx`. It opens the shard once through the
// caller-owned cache, opens a random-access column_read_state,
// walks all `num_fragments` rows and memcpys each fragment's `data[]` into
// the right offset of the destination varlena's payload region.
//
// Raises bioseq_log<logging_level::error, logging_target::postgres>() if the assembled varlena would exceed MaxAllocSize —
// streaming readers must use the source-backed facade instead. The bounded
// callers (auto-hydration of stub-backed scan slots) are bounded by the
// MaxAllocSize gate in `hydrate_seq_columns_in_slot`.
// ---------------------------------------------------------------------------

[[nodiscard]] inline column_read_state*
begin_sequence_payload_read(Relation shard,
                            MemoryContext payload_mctx,
                            Snapshot snapshot) {
    bioseq::postgres::support::memory_context_scope ctx{payload_mctx};
    TupleDesc td = RelationGetDescr(shard);
    auto attr_needed = bioseq::postgres::support::postgres_bitmapset::
        make_member_range(td->natts);
    bioseq::postgres::support::postgres_integer_list projected;
    for (int i = 0; i < td->natts; ++i) {
        if (!TupleDescAttr(td, i)->attisdropped && attr_needed.has_member(i)) {
            projected.append(static_cast<AttrNumber>(i + 1));
        }
    }
    column_read_state* read_state = begin_column_read(
        shard, td, projected.release(), /*whereClauseList=*/NIL,
        payload_mctx, snapshot, /*randomAccess=*/true, nullptr);
    flush_pending_writes_for_read(read_state);
    attr_needed.clear();
    return read_state;
}

template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
bioseq::pg_types::seq*
sequence_storage_manager<
    p_length_type,
    p_bucket_minimum_length_bits,
    p_bucket_maximum_length_bits>::fetch_payload(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, const sequence_locator& loc,
    Snapshot snapshot,
    MemoryContext out_mctx, seq_caches* cache)
{
    if (cache == nullptr) {
        bioseq_log<logging_level::error, logging_target::postgres>( "bioseq storage: fetch_payload requires a seq_caches");
    }
    if (snapshot == nullptr) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: fetch_payload requires an explicit snapshot");
    }

    validate_locator_flat_materialization_length(loc);
    const bool registered_mvcc_snapshot =
        IsMVCCSnapshot(snapshot) && snapshot->regd_count != 0;
    // SnapshotAny is backend-lifetime static state. Other non-MVCC snapshots
    // (especially caller-stack Dirty snapshots) get a per-call reader so no
    // cache entry can outlive their owner.
    // An unregistered MVCC value can still be used for this synchronous call,
    // but cannot be retained beyond it.
    const bool retain_read_state =
        registered_mvcc_snapshot || snapshot == SnapshotAny;

    // Empty sequence (num_fragments == 0). The locator is the
    // only on-disk artefact; there are no sub-table rows to fetch. Return a
    // minimal empty bioseq::pg_types::seq in `out_mctx`.
    if (loc.fragment_count == 0) {
        bioseq::postgres::support::memory_context_scope ctx{out_mctx};
        bioseq::pg_types::seq* out = static_cast<bioseq::pg_types::seq*>(palloc0(bioseq::pg_types::seq::header_bytes()));
        SET_VARSIZE(out, bioseq::pg_types::seq::header_bytes());
        // alphabet/width/bp_size/padding are all zero — caller-side stub
        // hydration sees an empty payload (compact_sequence_range handles
        // size=0 correctly).
        return out;
    }

    Relation shard = open_sub_table_for_locator(db_oid, spc_oid, main_relfilenode,
        column_attno, loc, AccessShareLock, cache);
    const std::uint64_t cache_key = seq_caches::key_for(
        column_attno,
        static_cast<std::int16_t>(loc.bucket_index),
        static_cast<std::int32_t>(loc.shard_index));
    seq_caches::entry& entry = cache->entries[cache_key];
    entry.rel = shard;

    MemoryContext payload_mctx =
        cache->read_state_mctx != nullptr ? cache->read_state_mctx
                                          : CurrentMemoryContext;
    column_read_state* read_state = nullptr;
    bool transient_read_state = false;
    if (retain_read_state) {
        // Key reuse by the caller-owned source snapshot, not the
        // read-state pointer: flushing may replace read_state->snapshot with
        // its own refreshed registered copy while the owner remains the same.
        if (entry.read_state != nullptr
            && entry.read_snapshot_source != snapshot) {
            end_column_read(entry.read_state);
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

    // Derive fragment shape from logical alphabet and width on
    // first sub-table row.
    Datum first_vals[1]  = { 0 };
    bool  first_nulls[1] = { true };
    if (!read_column_row_by_number(read_state, loc.first_fragment_row,
                                    first_vals, first_nulls)) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: missing first sub-table row {} "
               "(locator bucket={} shard={} num_fragments={})",
               loc.first_fragment_row,
               static_cast<unsigned>(loc.bucket_index),
               loc.shard_index, loc.fragment_count);
    }
    if (first_nulls[0]) {
        bioseq_log<logging_level::error, logging_target::postgres>( "bioseq storage: first sub-table row {} is NULL",
               loc.first_fragment_row);
    }
    bioseq::pg_types::seq* first_raw = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(first_vals[0]));
    const Size first_raw_bytes = static_cast<Size>(VARSIZE(first_raw));
    if (first_raw_bytes < bioseq::pg_types::seq::header_bytes()) {
        validate_sequence_fragment_frame(
            first_raw_bytes, bioseq::pg_types::seq::header_bytes(),
            loc.first_fragment_row);
    }
    if (is_locator_stub(first_raw)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq fragment row is itself a locator stub"),
             errdetail("fragment row=%llu",
                 static_cast<unsigned long long>(loc.first_fragment_row))));
    }

    const std::uint8_t logical_alphabet_code =
        static_cast<std::uint8_t>(first_raw->alphabet_code);
    const std::uint8_t logical_byte_width =
        static_cast<std::uint8_t>(first_raw->storage_unit_byte_width);
    if (logical_alphabet_code >= pgseq_supported_alphabets::alphabet_count) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq fragment row has an unsupported alphabet"),
             errdetail("fragment row=%llu alphabet=%u",
                 static_cast<unsigned long long>(loc.first_fragment_row),
                 static_cast<unsigned>(logical_alphabet_code))));
    }
    if (logical_byte_width != 1u && logical_byte_width != 2u
        && logical_byte_width != 4u && logical_byte_width != 8u) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq fragment row has an unsupported storage width"),
             errdetail("fragment row=%llu width=%u",
                 static_cast<unsigned long long>(loc.first_fragment_row),
                 static_cast<unsigned>(logical_byte_width))));
    }

    const auto alpha_code = static_cast<bioseq::pg_types::seq::alphabet_code_t>(logical_alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha_code, logical_byte_width);
    if (lpu == 0) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq fragment row has an unsupported sequence type"),
             errdetail("fragment row=%llu alphabet=%u width=%u",
                 static_cast<unsigned long long>(loc.first_fragment_row),
                 static_cast<unsigned>(alpha_code),
                 static_cast<unsigned>(logical_byte_width))));
    }
    const std::size_t bytes_per_unit = static_cast<std::size_t>(logical_byte_width);
    const Size fragment_storage_bytes = checked_mul_size(
        checked_size_cast(detail::k_fragment_storage_units), bytes_per_unit);
    const Size expected_fragment_bytes = checked_add_size(
        bioseq::pg_types::seq::header_bytes(), fragment_storage_bytes);
    validate_sequence_fragment_frame(
        first_raw_bytes, expected_fragment_bytes,
        loc.first_fragment_row);

    // Compute exact payload/flat sizes without overflowing before either
    // PostgreSQL's Size or MaxAllocSize boundary can reject the request.
    const std::uint64_t total_units_u64 = checked_ceil_div(
        loc.logical_length, static_cast<std::uint64_t>(lpu));
    const Size total_units = checked_size_cast(total_units_u64);
    const Size total_payload_bytes = checked_mul_size(
        total_units, bytes_per_unit);
    const Size assembled_varlena_bytes =
        checked_materialization_allocation_size(
            checked_round_up_to_uint64(total_payload_bytes),
            bioseq::pg_types::seq::header_bytes(), "flat sequence");

    const std::uint64_t expected_fragments =
        expected_flat_locator_fragment_count(
            loc.logical_length, static_cast<std::uint64_t>(lpu),
            detail::k_fragment_storage_units);
    if (expected_fragments != loc.fragment_count) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq locator fragment geometry is inconsistent"),
             errdetail("expected fragments=%llu locator fragments=%u",
                 static_cast<unsigned long long>(expected_fragments),
                 loc.fragment_count)));
    }
    const std::uint64_t fragment_bp =
        static_cast<std::uint64_t>(detail::k_fragment_storage_units)
        * static_cast<std::uint64_t>(lpu);
    const std::uint64_t expected_last_padding =
        (static_cast<std::uint64_t>(lpu)
            - loc.logical_length % static_cast<std::uint64_t>(lpu))
        % static_cast<std::uint64_t>(lpu);
    const std::uint64_t first_expected_bp = loc.fragment_count == 1u
        ? loc.logical_length
        : fragment_bp;
    const std::uint64_t first_expected_padding = loc.fragment_count == 1u
        ? expected_last_padding
        : 0u;
    validate_sequence_fragment_metadata(
        first_raw, logical_alphabet_code, logical_byte_width,
        first_expected_bp, first_expected_padding,
        loc.first_fragment_row);

    bioseq::postgres::support::memory_context_scope ctx{out_mctx};
    bioseq::pg_types::seq* out = static_cast<bioseq::pg_types::seq*>(palloc0(assembled_varlena_bytes));
    SET_VARSIZE(out, assembled_varlena_bytes);
    out->alphabet_code           = static_cast<bioseq::pg_types::seq::alphabet_code_t>(logical_alphabet_code);
    out->storage_unit_byte_width = logical_byte_width;
    out->bp_size                 = static_cast<bioseq::pg_types::seq::bp_size_t>(loc.logical_length);
    // Last-fragment padding is also the padding of the assembled varlena.

    std::uint8_t* const out_data = reinterpret_cast<std::uint8_t*>(out->data);

    // Copy the already-fetched and fully validated first fragment.
    const Size first_copy_bytes =
        std::min(fragment_storage_bytes, total_payload_bytes);
    if (first_copy_bytes > 0) {
        std::memcpy(out_data, first_raw->data, first_copy_bytes);
    }
    out->padding_letters =
        static_cast<std::uint16_t>(expected_last_padding);

    for (std::uint32_t fragment_no = 1; fragment_no < loc.fragment_count; ++fragment_no) {
        // Full hydration of a multi-Gbp target into one varlena walks
        // tens of thousands of fragment rows. Make it cancellable and observable.
        CHECK_FOR_INTERRUPTS();
        if (bioseq::settings::diagnostics::logging::enabled && (fragment_no & 0x3FFF) == 0) {
            bioseq_log_progress<DBG>("[fetch_payload] hydrated {}/{} fragments",
                            fragment_no, loc.fragment_count);
        }
        const std::uint64_t row_no = loc.first_fragment_row + fragment_no;
        Datum vals[1]  = { 0 };
        bool  nulls[1] = { true };
        if (!read_column_row_by_number(read_state, row_no, vals, nulls)) {
            bioseq_log<logging_level::error, logging_target::postgres>(
                   "bioseq storage: missing sub-table fragment row {} "
                   "(locator bucket={} shard={} fragment={}/{})",
                   row_no,
                   static_cast<unsigned>(loc.bucket_index),
                   loc.shard_index, fragment_no, loc.fragment_count);
        }
        if (nulls[0]) {
            bioseq_log<logging_level::error, logging_target::postgres>( "bioseq storage: sub-table row {} is NULL", row_no);
        }
        bioseq::pg_types::seq* raw = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(vals[0]));
        const Size raw_bytes = static_cast<Size>(VARSIZE(raw));
        if (raw_bytes < bioseq::pg_types::seq::header_bytes()) {
            validate_sequence_fragment_frame(
                raw_bytes, bioseq::pg_types::seq::header_bytes(), row_no);
        }
        if (is_locator_stub(raw)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq fragment row is itself a locator stub"),
                 errdetail("fragment row=%llu",
                     static_cast<unsigned long long>(row_no))));
        }
        validate_sequence_fragment_frame(
            raw_bytes, expected_fragment_bytes, row_no);
        const bool is_last = fragment_no + 1u == loc.fragment_count;
        const std::uint64_t expected_bp = is_last
            ? loc.logical_length
                - static_cast<std::uint64_t>(fragment_no) * fragment_bp
            : fragment_bp;
        validate_sequence_fragment_metadata(
            raw, logical_alphabet_code, logical_byte_width,
            expected_bp, is_last ? expected_last_padding : 0u,
            row_no);
        const Size dst_offset = checked_mul_size(
            checked_size_cast(fragment_no), fragment_storage_bytes);
        if (dst_offset > total_payload_bytes) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator fragment offset exceeds its payload")));
        }
        const Size copy_bytes =
            std::min(fragment_storage_bytes,
                     total_payload_bytes - dst_offset);
        if (copy_bytes > 0) {
            std::memcpy(out_data + dst_offset, raw->data, copy_bytes);
        }
    }

    if (transient_read_state) {
        end_column_read(read_state);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Bulk fetch.
// ---------------------------------------------------------------------------

template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
void
sequence_storage_manager<
    p_length_type,
    p_bucket_minimum_length_bits,
    p_bucket_maximum_length_bits>::fetch_payload_bulk(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, std::int16_t bucket_idx,
    std::span<const fetch_request> requests,
    std::span<bioseq::pg_types::seq*>            out_payloads,
    std::span<fetch_request>       sort_scratch,
    Snapshot                       snapshot,
    MemoryContext                  out_mctx,
    seq_caches*                    cache)
{
    (void)bucket_idx;  // The bucket is implicit in each locator; carried for symmetry.
    if (snapshot == nullptr) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: fetch_payload_bulk requires an explicit snapshot");
    }
    if (requests.empty()) {
        return;
    }
    if (sort_scratch.size() < requests.size()) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: fetch_payload_bulk sort_scratch too small "
               "(have {} need {})", sort_scratch.size(), requests.size());
    }
    std::copy(requests.begin(), requests.end(), sort_scratch.begin());

    std::sort(sort_scratch.begin(), sort_scratch.begin() + requests.size(),
        [](const fetch_request& a, const fetch_request& b) noexcept {
            return std::tie(a.locator.shard_index, a.locator.first_fragment_row)
                 < std::tie(b.locator.shard_index, b.locator.first_fragment_row);
        });

    for (std::size_t i = 0; i < requests.size(); ++i) {
        const fetch_request& req = sort_scratch[i];
        out_payloads[req.out_slot] = fetch_payload(
            db_oid, spc_oid, main_relfilenode,
            column_attno, req.locator, snapshot, out_mctx, cache);
    }
}

// ---------------------------------------------------------------------------
// DROP cascade.
// ---------------------------------------------------------------------------

template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
void
sequence_storage_manager<
    p_length_type,
    p_bucket_minimum_length_bits,
    p_bucket_maximum_length_bits>::validate_drop_catalog_contracts()
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
    Relation source_guard = bioseq::postgres::support::try_open_postgres_relation(
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

template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
void
sequence_storage_manager<
    p_length_type,
    p_bucket_minimum_length_bits,
    p_bucket_maximum_length_bits>::drop_sub_tables_for_main(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    const bool allow_missing_during_extension_teardown)
{
    // Keep the validated registry relation locked across the manager calls
    // below. Those calls return an empty set when the
    // relation disappears; the guard makes that benign only for the explicit
    // extension-teardown context and closes the validation-to-use race.
    Relation registry_guard = fragment_registry_detail::open_catalog(
        allow_missing_during_extension_teardown);
    if (registry_guard == nullptr) {
        return;
    }
    sequence_source_version_manager::delete_for_main(
        db_oid, spc_oid, main_relfilenode,
        allow_missing_during_extension_teardown);
    const auto rows = sequence_sub_table_manager::list_shards_for_main(db_oid, spc_oid, main_relfilenode);
    if (rows.empty()) {
        table_close(registry_guard, AccessShareLock);
        return;
    }

    constexpr std::size_t k_drop_batch_size = 32;
    StringInfo buf = makeStringInfo();
    for (std::size_t i = 0; i < rows.size(); i += k_drop_batch_size) {
        resetStringInfo(buf);
        appendStringInfoString(buf, "DROP TABLE IF EXISTS ");
        const std::size_t end = std::min(i + k_drop_batch_size, rows.size());
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

    for (std::size_t i = 0; i < rows.size(); i += k_drop_batch_size) {
        resetStringInfo(buf);
        appendStringInfoString(buf, "DROP TABLE IF EXISTS ");
        const std::size_t end = std::min(i + k_drop_batch_size, rows.size());
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

    sequence_sub_table_manager::delete_shards_for_main(db_oid, spc_oid, main_relfilenode);
    table_close(registry_guard, AccessShareLock);
}

// Explicit instantiation of the default alias so non-template TUs see the
// out-of-line definitions.
template struct sequence_storage_manager<sequence_size_t, 8, 42>;

// ---------------------------------------------------------------------------
// Per-shard sidecar I/O.
// ---------------------------------------------------------------------------

static Oid lookup_sidecar_relid(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                std::int16_t column_attno,
                                std::int16_t bucket_idx, std::int32_t shard_idx)
{
    auto row = sequence_sub_table_manager::lookup_shard(db_oid, spc_oid, main_relfilenode,
                                   column_attno, bucket_idx, shard_idx);
    if (!row.has_value()) {
        return InvalidOid;
    }
    return row->sidecar_relid;
}

void sidecar_write_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                        std::int16_t column_attno, const sequence_locator& loc,
                        std::span<const std::uint8_t> blob, seq_caches* /*cache*/)
{
    const Oid sidecar_relid = lookup_sidecar_relid(
        db_oid, spc_oid, main_relfilenode, column_attno,
        static_cast<std::int16_t>(loc.bucket_index),
        static_cast<std::int32_t>(loc.shard_index));
    if (!OidIsValid(sidecar_relid)) {
        return;
    }
    Relation rel = bioseq::postgres::support::try_open_postgres_relation(sidecar_relid, RowExclusiveLock);
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
        Int64GetDatum(static_cast<std::int64_t>(loc.first_fragment_row)),
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
                  std::int16_t column_attno, const sequence_locator& loc,
                  seq_caches* /*cache*/)
{
    const Oid sidecar_relid = lookup_sidecar_relid(
        db_oid, spc_oid, main_relfilenode, column_attno,
        static_cast<std::int16_t>(loc.bucket_index),
        static_cast<std::int32_t>(loc.shard_index));
    if (!OidIsValid(sidecar_relid)) {
        return std::nullopt;
    }
    Relation rel = bioseq::postgres::support::try_open_postgres_relation(sidecar_relid, AccessShareLock);
    if (rel == nullptr) {
        return std::nullopt;
    }
    const Oid pk_index_oid = bioseq::postgres::support::find_primary_key_oid(rel);

    ScanKeyData keys[1];
    ScanKeyInit(&keys[0], 1 /*sub_rowno*/,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(loc.first_fragment_row)));
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
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: sidecar_read_blob_bulk sort_scratch too small "
               "(have {} need {})", sort_scratch.size(), requests.size());
    }
    std::copy(requests.begin(), requests.end(), sort_scratch.begin());
    using fetch_request_type = sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request;
    std::sort(sort_scratch.begin(), sort_scratch.begin() + requests.size(),
        [](const fetch_request_type& a, const fetch_request_type& b) noexcept {
            return std::tie(a.locator.shard_index, a.locator.first_fragment_row)
                 < std::tie(b.locator.shard_index, b.locator.first_fragment_row);
        });
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const fetch_request_type& req = sort_scratch[i];
        out_blobs[req.out_slot] = sidecar_read_blob(
            db_oid, spc_oid, main_relfilenode,
            column_attno, req.locator, cache);
    }
}

// ---------------------------------------------------------------------------
// Orphan relation maintenance.
// ---------------------------------------------------------------------------

/** Sweeps stale catalog rows and unreferenced sequence-shard relations. */
export [[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::volatile_,
    .parallel = bioseq::sql::parallel_safety::unsafe,
})]]
[[bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::execute,
    .to = bioseq::sql::public_role,
})]]
[[nodiscard]] bioseq::postgres::support::table_rows<reaper_event>
reap_orphan_seq_sub_tables() {
    bioseq::postgres::support::spi_session spi;
    std::vector<reaper_event> events;
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    bioseq::postgres::support::safety_runtime_probe::destructor_sentinel
        reaper_call_sentinel{"orphan_reaper_call"};
#endif
    const auto append_event =
        [&events](reaper_event event) {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.event_allocation")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.event_allocation");
                throw std::bad_alloc{};
            }
#endif
            events.push_back(std::move(event));
        };
    const int connect_status = spi.connect();
    if (connect_status != SPI_OK_CONNECT) {
        throw std::runtime_error{
            "could not connect to SPI for the orphan relation reaper"};
    }

    /*
     * Serialize Direction-A discovery/deletion against seq_sub_table writers.
     * Direction B reacquires this self-conflicting mode per physical candidate
     * in the production writer's physical-then-catalog lock order.
     */
    const Oid internal_namespace_oid =
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            return get_namespace_oid("bioseq_internal", false);
        });
    const Oid seq_sub_table_oid =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [internal_namespace_oid] {
                return get_relname_relid(
                    "seq_sub_table", internal_namespace_oid);
            });
    if (!OidIsValid(seq_sub_table_oid)) {
        throw std::runtime_error{
            "bioseq_internal.seq_sub_table is missing"};
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [seq_sub_table_oid] {
            LockRelationOid(
                seq_sub_table_oid, ShareRowExclusiveLock);
        });

    const auto require_status = [](
            int actual,
            int expected,
            const char* operation) {
        if (actual == expected)
            return;
        throw std::runtime_error{
            std::string{"SPI "} + operation
            + " returned status " + std::to_string(actual)
            + ", expected " + std::to_string(expected)};
    };

    const char* find_orphan_rows =
        "SELECT s.database_oid::text || '/' || "
        "       s.main_relfilenode::text || '/' || "
        "       s.column_attno::text || '/' || "
        "       s.bucket_idx::text || '/' || "
        "       s.shard_idx::text "
        "FROM bioseq_internal.seq_sub_table s "
        "WHERE s.database_oid = $1 "
        "  AND NOT EXISTS (SELECT 1 FROM pg_class c "
        "                  WHERE c.oid = s.sub_table_relid);";
    std::array<Oid, 1> argument_types{OIDOID};
    std::array<Datum, 1> argument_values{
        ObjectIdGetDatum(MyDatabaseId)};
    const int find_rows_status = bioseq::postgres::support::invoke_pg_as_cpp(
        [find_orphan_rows, &argument_types, &argument_values] {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.direction_a_select")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.direction_a_select");
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal(
                         "bioseq safety runtime injected reaper "
                         "Direction-A SELECT error")));
            }
#endif
            return SPI_execute_with_args(
                find_orphan_rows,
                static_cast<int>(argument_types.size()),
                argument_types.data(),
                argument_values.data(),
                /*Nulls=*/nullptr, /*read_only=*/false, /*tcount=*/0);
        });
    require_status(
        find_rows_status,
        SPI_OK_SELECT,
        "orphan catalog SELECT");

    SPITupleTable* const orphan_rows = SPI_tuptable;
    const std::uint64_t orphan_row_count = SPI_processed;
    for (std::uint64_t row = 0; row < orphan_row_count; ++row) {
        char* key = bioseq::postgres::support::invoke_pg_as_cpp(
            [orphan_rows, row] {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
                if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                        "orphan_reaper.direction_a_tuple_copy")) {
                    bioseq::postgres::support::safety_runtime_probe::
                        record_site_visit(
                            "orphan_reaper.direction_a_tuple_copy");
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal(
                             "bioseq safety runtime injected reaper "
                             "Direction-A tuple-copy error")));
                }
#endif
                return SPI_getvalue(
                    orphan_rows->vals[row],
                    orphan_rows->tupdesc,
                    1);
            });
        if (key != nullptr) {
            append_event({
                .action = "delete-catalog-row",
                .target = key,
            });
            bioseq::postgres::support::invoke_pg_as_cpp(
                [key] { pfree(key); });
        }
    }
    if (orphan_rows != nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [orphan_rows] { SPI_freetuptable(orphan_rows); });
    }

    const char* del_orphan_rows =
        "DELETE FROM bioseq_internal.seq_sub_table s "
        "WHERE s.database_oid = $1 "
        "  AND NOT EXISTS (SELECT 1 FROM pg_class c "
        "                  WHERE c.oid = s.sub_table_relid);";
    const int delete_rows_status = bioseq::postgres::support::invoke_pg_as_cpp(
        [del_orphan_rows, &argument_types, &argument_values] {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.direction_a_delete")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.direction_a_delete");
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal(
                         "bioseq safety runtime injected reaper "
                         "Direction-A DELETE error")));
            }
#endif
            return SPI_execute_with_args(
                del_orphan_rows,
                static_cast<int>(argument_types.size()),
                argument_types.data(),
                argument_values.data(),
                /*Nulls=*/nullptr,
                /*read_only=*/false, /*tcount=*/0);
        });
    require_status(
        delete_rows_status,
        SPI_OK_DELETE,
        "orphan catalog DELETE");
    bioseq::postgres::support::invoke_pg_as_cpp(
        [seq_sub_table_oid] {
            UnlockRelationOid(
                seq_sub_table_oid, ShareRowExclusiveLock);
        });

    const char* find_orphan_relations =
        "SELECT c.oid, c.relname, "
        "       n.nspname || '.' || quote_ident(c.relname) "
        "FROM pg_class c JOIN pg_namespace n ON c.relnamespace = n.oid "
        "WHERE n.nspname = 'bioseq_internal' "
        "  AND (c.relname LIKE 'seq\\_sub\\_%' ESCAPE '\\' "
        "       OR c.relname LIKE 'idx\\_sidecar\\_%' ESCAPE '\\') "
        "  AND c.relkind <> 'i' "
        "  AND c.oid <> 'bioseq_internal.seq_sub_table'::regclass "
        "  AND NOT EXISTS ("
        "      SELECT 1 FROM bioseq_internal.seq_sub_table s "
        "      WHERE s.sub_table_relid = c.oid OR s.sidecar_relid = c.oid) "
        "ORDER BY c.oid;";
    struct orphan_relation_candidate final {
        Oid relation_oid = InvalidOid;
        std::string leaf_name;
        std::string qualified_name;
    };
    std::vector<orphan_relation_candidate> to_drop;
    const int find_relations_status = bioseq::postgres::support::invoke_pg_as_cpp(
        [find_orphan_relations] {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.direction_b_select")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.direction_b_select");
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal(
                         "bioseq safety runtime injected reaper "
                         "Direction-B SELECT error")));
            }
#endif
            return SPI_execute(
                find_orphan_relations,
                /*read_only=*/true,
                /*tcount=*/0);
        });
    require_status(
        find_relations_status,
        SPI_OK_SELECT,
        "orphan relation SELECT");

    SPITupleTable* const orphan_relations = SPI_tuptable;
    const std::uint64_t orphan_relation_count = SPI_processed;
    to_drop.reserve(orphan_relation_count);
    for (std::uint64_t row = 0;
         row < orphan_relation_count;
         ++row) {
        const auto relation_oid = bioseq::postgres::support::invoke_pg_as_cpp(
            [orphan_relations, row] {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
                if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                        "orphan_reaper.direction_b_tuple_copy")) {
                    bioseq::postgres::support::safety_runtime_probe::
                        record_site_visit(
                            "orphan_reaper.direction_b_tuple_copy");
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal(
                             "bioseq safety runtime injected reaper "
                             "Direction-B tuple-copy error")));
                }
#endif
                bool is_null = false;
                const Datum value = SPI_getbinval(
                    orphan_relations->vals[row],
                    orphan_relations->tupdesc,
                    1,
                    &is_null);
                return std::pair{
                    is_null
                        ? InvalidOid
                        : DatumGetObjectId(value),
                    is_null};
            });
        char* leaf_name = bioseq::postgres::support::invoke_pg_as_cpp(
            [orphan_relations, row] {
                return SPI_getvalue(
                    orphan_relations->vals[row],
                    orphan_relations->tupdesc,
                    2);
            });
        char* qualified_name = bioseq::postgres::support::invoke_pg_as_cpp(
            [orphan_relations, row] {
                return SPI_getvalue(
                    orphan_relations->vals[row],
                    orphan_relations->tupdesc,
                    3);
            });
        if (!relation_oid.second
            && OidIsValid(relation_oid.first)
            && leaf_name != nullptr
            && qualified_name != nullptr) {
            to_drop.push_back({
                .relation_oid = relation_oid.first,
                .leaf_name = leaf_name,
                .qualified_name = qualified_name,
            });
        }
        if (leaf_name != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [leaf_name] { pfree(leaf_name); });
        }
        if (qualified_name != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [qualified_name] { pfree(qualified_name); });
        }
    }
    if (orphan_relations != nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [orphan_relations] {
                SPI_freetuptable(orphan_relations);
            });
    }

    const auto relation_is_still_orphan =
        [&require_status](Oid relation_oid) {
            static constexpr char statement[] =
                "SELECT NOT EXISTS ("
                "  SELECT 1 "
                "    FROM bioseq_internal.seq_sub_table s "
                "   WHERE s.sub_table_relid = $1 "
                "      OR s.sidecar_relid = $1)";
            std::array<Oid, 1> types{OIDOID};
            std::array<Datum, 1> values{
                ObjectIdGetDatum(relation_oid)};
            const int status =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&types, &values] {
                        return SPI_execute_with_args(
                            statement,
                            static_cast<int>(types.size()),
                            types.data(),
                            values.data(),
                            /*Nulls=*/nullptr,
                            /*read_only=*/false,
                            /*tcount=*/1);
                    });
            require_status(
                status,
                SPI_OK_SELECT,
                "orphan relation revalidation SELECT");
            SPITupleTable* const table = SPI_tuptable;
            const bool valid_shape =
                table != nullptr && SPI_processed == 1;
            bool is_null = true;
            bool is_orphan = false;
            if (valid_shape) {
                is_orphan =
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [table, &is_null] {
                            const Datum value = SPI_getbinval(
                                table->vals[0],
                                table->tupdesc,
                                1,
                                &is_null);
                            return !is_null
                                && DatumGetBool(value);
                        });
            }
            if (table != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [table] { SPI_freetuptable(table); });
            }
            return valid_shape && !is_null && is_orphan;
        };

#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    bool catchable_drop_requires_continuation = false;
#endif
    for (const orphan_relation_candidate& candidate : to_drop) {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
        if (catchable_drop_requires_continuation) {
            bioseq::postgres::support::safety_runtime_probe::record_cleanup_complete(
                "orphan_reaper.catchable_drop.continued");
            throw std::runtime_error{
                "bioseq safety runtime observed catchable reaper "
                "DROP continuation"};
        }
#endif
        const std::string drop_sql =
            "DROP TABLE IF EXISTS " + candidate.qualified_name;
#if defined(BIOSEQ_TEST_INDEXES)
        orphan_reaper_test_support::pass_drop_barrier();
#endif
        auto subtransaction =
            bioseq::postgres::support::subtransaction::begin();
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
        bool catchable_drop_probe_injected = false;
#endif
        try {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            bioseq::postgres::support::safety_runtime_probe::destructor_sentinel
                reaper_object_sentinel{"orphan_reaper_object"};
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.query_canceled")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.query_canceled");
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_QUERY_CANCELED),
                         errmsg(
                             "bioseq safety runtime injected reaper "
                             "query cancellation")));
                });
            }
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.assertion_failure")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.assertion_failure");
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_ASSERT_FAILURE),
                         errmsg(
                             "bioseq safety runtime injected reaper "
                             "assertion failure")));
                });
            }
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.std_exception")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.std_exception");
                throw std::runtime_error{
                    "bioseq safety runtime injected reaper "
                    "std::runtime_error"};
            }
            if (bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.unknown_exception")) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.unknown_exception");
                throw std::uint32_t{388};
            }
#endif
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_oid = candidate.relation_oid] {
                    LockRelationOid(
                        relation_oid, AccessExclusiveLock);
                });
            /*
             * Writers lock a physical shard before recording or updating its
             * catalog row.  Match that order, then hold the catalog lock
             * through revalidation and DROP.  Releasing this one explicit
             * lock before the next candidate avoids cross-candidate
             * physical/catalog lock inversion.
             */
            bioseq::postgres::support::invoke_pg_as_cpp(
                [seq_sub_table_oid] {
                    LockRelationOid(
                        seq_sub_table_oid,
                        ShareRowExclusiveLock);
                });
            const bool identity_is_current =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&candidate, internal_namespace_oid] {
                        HeapTuple tuple = SearchSysCache1(
                            RELOID,
                            ObjectIdGetDatum(
                                candidate.relation_oid));
                        if (!HeapTupleIsValid(tuple))
                            return false;
                        const Form_pg_class relation =
                            reinterpret_cast<Form_pg_class>(
                                GETSTRUCT(tuple));
                        const bool matches =
                            relation->relnamespace
                                == internal_namespace_oid
                            && std::strncmp(
                                   NameStr(relation->relname),
                                   candidate.leaf_name.c_str(),
                                   NAMEDATALEN) == 0;
                        ReleaseSysCache(tuple);
                        return matches;
                    });
            if (!identity_is_current
                || !relation_is_still_orphan(
                    candidate.relation_oid)) {
                subtransaction.rollback();
                continue;
            }
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            catchable_drop_probe_injected =
                bioseq::postgres::support::safety_runtime_probe::consume_fault(
                    "orphan_reaper.direction_b_drop_catchable");
            if (catchable_drop_probe_injected) {
                bioseq::postgres::support::safety_runtime_probe::record_site_visit(
                    "orphan_reaper.direction_b_drop_catchable");
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal(
                             "bioseq safety runtime injected reaper "
                             "Direction-B catchable DROP error")));
                });
            }
#endif
            const int drop_status =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [statement = drop_sql.c_str()] {
                        return SPI_execute(
                            statement,
                            /*read_only=*/false,
                            /*tcount=*/0);
                    });
            require_status(
                drop_status,
                SPI_OK_UTILITY,
                "orphan relation DROP TABLE");
            bioseq::postgres::support::invoke_pg_as_cpp(
                [seq_sub_table_oid] {
                    UnlockRelationOid(
                        seq_sub_table_oid,
                        ShareRowExclusiveLock);
                });
        } catch (
                const bioseq::postgres::support::postgres_error& error) {
            if (error.get_sql_state_code() == ERRCODE_QUERY_CANCELED
                || error.get_sql_state_code() == ERRCODE_ASSERT_FAILURE) {
                subtransaction.rollback();
                throw;
            }
            std::string failure_action =
                "drop-relation-failed:"
                + std::string{error.what()};
            subtransaction.rollback();
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            if (catchable_drop_probe_injected) {
                bioseq::postgres::support::safety_runtime_probe::
                    record_cleanup_complete(
                        "orphan_reaper.catchable_drop.rollback");
            }
#endif
            append_event({
                .action = std::move(failure_action),
                .target = candidate.qualified_name,
            });
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
            catchable_drop_requires_continuation =
                catchable_drop_probe_injected;
#endif
            continue;
        }

        subtransaction.commit();
        append_event({
            .action = "drop-relation",
            .target = candidate.qualified_name,
        });
    }
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
    if (catchable_drop_requires_continuation) {
        throw std::runtime_error{
            "bioseq safety runtime catchable reaper DROP fixture "
            "did not provide a continuation candidate"};
    }
#endif

    require_status(
        spi.finish(),
        SPI_OK_FINISH,
        "SPI_finish");
    return bioseq::postgres::support::table_rows<reaper_event>{
        std::move(events)};
}

namespace {

struct vacuum_target_pair {
    Oid sub_relation = InvalidOid;
    Oid sidecar_relation = InvalidOid;
};

}  // namespace

/** Vacuums and analyzes each sequence-shard relation owned by a table. */
export [[bioseq_sql::procedure({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::execute,
    .to = bioseq::sql::public_role,
})]]
void vacuum_seq_storage(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::regclass,
        })]]
        std::optional<Oid> rel,
        bioseq::postgres::support::function_call_context context) {
    bioseq::postgres::support::spi_session spi;
    std::vector<vacuum_target_pair> targets;
    if (rel.has_value()) {
        HeapTuple relation_tuple =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_id = *rel] {
                    return SearchSysCache1(
                        RELOID,
                        ObjectIdGetDatum(relation_id));
                });
        if (HeapTupleIsValid(relation_tuple)) {
            const auto* relation_form =
                reinterpret_cast<Form_pg_class>(
                    GETSTRUCT(relation_tuple));
            Oid tablespace_id = relation_form->reltablespace;
            if (!OidIsValid(tablespace_id)) {
                tablespace_id = MyDatabaseTableSpace;
            }
            const Oid relfilenode = relation_form->relfilenode;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_tuple] {
                    ReleaseSysCache(relation_tuple);
                });

            auto rows = sequence_sub_table_manager::list_shards_for_main(
                MyDatabaseId,
                tablespace_id,
                relfilenode);
            std::ranges::sort(
                rows,
                {},
                [](const auto& row) {
                    return std::tuple{
                        row.column_attno,
                        row.bucket_idx,
                        row.shard_idx};
                });
            targets.reserve(rows.size());
            for (const auto& row : rows) {
                targets.push_back({
                    .sub_relation = row.sub_table_relid,
                    .sidecar_relation = row.sidecar_relid,
                });
            }
        }
    }

    if (targets.empty()) {
        return;
    }

    context.require_nonatomic_call();
    const int connect_status = spi.connect(SPI_OPT_NONATOMIC);
    if (connect_status != SPI_OK_CONNECT) {
        throw std::runtime_error{
            "could not start a non-atomic SPI connection for shard vacuum"};
    }

    const auto vacuum_analyze = [](Oid relation_id) {
        auto* statement = makeNode(VacuumStmt);
        statement->options =
            list_make1(
                makeDefElem(pstrdup("analyze"), nullptr, -1));
        statement->rels =
            list_make1(
                makeVacuumRelation(
                    nullptr,
                    relation_id,
                    NIL));
        statement->is_vacuumcmd = true;
        ExecVacuum(nullptr, statement, true);
    };

    for (const vacuum_target_pair& target : targets) {
        context.commit_transaction();
        const int pre_vacuum_finish_status = spi.finish();
        if (pre_vacuum_finish_status != SPI_OK_FINISH) {
            throw std::runtime_error{
                "could not suspend the non-atomic SPI connection "
                "before shard vacuum"};
        }
        try {
            bioseq::postgres::support::invoke_pg_across_transactions_as_cpp(
                [vacuum_analyze,
                 relation_id = target.sub_relation] {
                    vacuum_analyze(relation_id);
                });
            bioseq::postgres::support::invoke_pg_across_transactions_as_cpp(
                [vacuum_analyze,
                 relation_id = target.sidecar_relation] {
                    vacuum_analyze(relation_id);
                });
        } catch (bioseq::postgres::support::postgres_error& error) {
            if (error.get_sql_state_code() != ERRCODE_UNDEFINED_TABLE) {
                throw;
            }
            const int reconnect_status =
                spi.connect(SPI_OPT_NONATOMIC);
            if (reconnect_status != SPI_OK_CONNECT) {
                throw std::runtime_error{
                    "could not restore the non-atomic SPI connection "
                    "after shard vacuum failure"};
            }
            FreeErrorData(error.release_error_data());
            context.rollback_transaction();
            continue;
        }
        const int reconnect_status =
            spi.connect(SPI_OPT_NONATOMIC);
        if (reconnect_status != SPI_OK_CONNECT) {
            throw std::runtime_error{
                "could not restore the non-atomic SPI connection "
                "after shard vacuum"};
        }
    }

    const int finish_status = spi.finish();
    if (finish_status != SPI_OK_FINISH) {
        throw std::runtime_error{
            "could not finish the non-atomic SPI connection "
            "after shard vacuum"};
    }
}

}  // namespace bioseq::storage
