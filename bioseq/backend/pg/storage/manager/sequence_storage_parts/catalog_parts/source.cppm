/**
 * @file source.cppm
 * @brief Immutable sequence-source catalog contracts and row codecs.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/sequence.h"
#include "commands/vacuum.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
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

export module bioseq.postgres.storage.manager:sequence_storage_catalog_source;

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
import bioseq.common.seqan_adapter; // ceil_log2 used in letters_per_unit_for
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
    bioseq::sql::generated::core::storage_manager_sequence_storage_manager::entity_handles;
namespace storage_manager_catalog_rows = bioseq::storage::catalog_rows;
namespace storage_manager_row_codecs = bioseq::postgres::storage::manager::sql_row_codecs;
using sequence_source_version_catalog_row = storage_manager_catalog_rows::sequence_source_version;
using sequence_source_version_codec = storage_manager_row_codecs::sequence_source_version_codec;
using minimizer_generation_storage_catalog_row =
    storage_manager_catalog_rows::minimizer_generation_storage;
using minimizer_generation_storage_codec =
    storage_manager_row_codecs::minimizer_generation_storage_codec;
using minimizer_generation_reaper_catalog_row =
    storage_manager_catalog_rows::minimizer_generation_reaper_state;
using minimizer_generation_reaper_codec =
    storage_manager_row_codecs::minimizer_generation_reaper_state_codec;

static_assert(storage_manager_entity_handles::sequence_source_epoch_seq.schema_binding() ==
              bioseq::postgres::support::sql_schema_binding::fixed);

export namespace bioseq::storage {

#if defined(BIOSEQ_TEST_INDEXES)
namespace orphan_reaper_test_support {

inline thread_local std::optional<std::int64_t> drop_barrier_key;

/** Arm a one-shot backend-local barrier before the next orphan DROP TABLE. */
/// @param key Session advisory-lock key controlled by the test coordinator.
inline void arm_drop_barrier(std::int64_t key) noexcept {
    drop_barrier_key = key;
}

/// Clear the backend-local orphan-drop barrier without waiting.
inline void disarm_drop_barrier() noexcept {
    drop_barrier_key.reset();
}

/** Wait for the controller's advisory lock, then release our acquired lock. */
///
/// The session-level advisory lock is released before this function returns.
inline void pass_drop_barrier() {
    if (!drop_barrier_key.has_value())
        return;
    const std::int64_t key = *drop_barrier_key;
    drop_barrier_key.reset();
    bioseq::postgres::support::invoke_pg_as_cpp([key] {
        static_cast<void>(DirectFunctionCall1(pg_advisory_lock_int8, Int64GetDatum(key)));
        static_cast<void>(DirectFunctionCall1(pg_advisory_unlock_int8, Int64GetDatum(key)));
    });
}

} // namespace orphan_reaper_test_support
#endif

namespace internal_catalog_detail {

/// Check whether the Bioseq extension is present in this database.
/// @return Whether PostgreSQL can resolve the extension OID.
[[nodiscard]] inline bool bioseq_extension_installed() {
    return OidIsValid(get_extension_oid("bioseq", true));
}

/// Permit missing internal objects only during explicit extension teardown.
/// @param allow_missing_during_extension_teardown Explicit teardown context flag.
/// @return The supplied teardown permission.
[[nodiscard]] inline bool absence_is_benign(const bool allow_missing_during_extension_teardown) {
    return allow_missing_during_extension_teardown;
}

/// Reject a missing extension-owned object outside teardown.
/// @param object_name Fully qualified object name copied into the diagnostic.
/// @throws PostgreSQL ERROR unconditionally.
[[noreturn]] inline void missing_outside_teardown(const std::string_view object_name) {
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
/// @param sqlstate PostgreSQL SQLSTATE code for the error.
/// @param message Owning diagnostic text.
/// @throws PostgreSQL ERROR unconditionally.
[[noreturn]] inline void storage_error_as_cpp(int sqlstate, std::string message) {
    bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, &message] {
        ereport(ERROR, (errcode(sqlstate), errmsg("%s", message.c_str())));
    });
    pg_unreachable();
}

/// The source epoch state machine is deliberately one-way. In particular, a
/// source invalidated by parent-row or fragment mutation can never be
/// resurrected by manufacturing another publication token.
/// @param current Current persistent state.
/// @param requested Requested successor state.
/// @return Whether the one-way state machine permits the transition.
[[nodiscard]] constexpr bool
sequence_source_state_transition_allowed(const std::int16_t current,
                                         const std::int16_t requested) noexcept {
    return (current == k_sequence_source_state_unpublished &&
            (requested == k_sequence_source_state_ready ||
             requested == k_sequence_source_state_invalid)) ||
           (current == k_sequence_source_state_ready &&
            requested == k_sequence_source_state_invalid);
}

static_assert(sequence_source_state_transition_allowed(k_sequence_source_state_unpublished,
                                                       k_sequence_source_state_ready));
static_assert(sequence_source_state_transition_allowed(k_sequence_source_state_ready,
                                                       k_sequence_source_state_invalid));
static_assert(!sequence_source_state_transition_allowed(k_sequence_source_state_invalid,
                                                        k_sequence_source_state_ready));

/// Exact PostgreSQL-owned identity record for one immutable sequence source.
/// The checksum is the CRC-64/ECMA-182 of decoded logical symbols, calculated
/// while the source is already streaming into fragment storage.
struct sequence_source_version_row {
    /// Positive immutable source epoch.
    std::uint64_t source_epoch = 0;
    /// User-visible parent relation OID.
    Oid user_relid = InvalidOid;
    /// Database component of the physical parent identity.
    Oid database_oid = InvalidOid;
    /// Tablespace component of the physical parent identity.
    Oid tablespace_oid = InvalidOid;
    /// File component of the physical parent identity.
    Oid main_relfilenode = InvalidOid;
    /// Storage-generation identity of the parent relation.
    std::uint64_t main_storage_id = 0;
    /// One-based parent sequence attribute number.
    std::int16_t column_attno = 0;
    /// Fragment range and source epoch persisted in the parent stub.
    sequence_locator locator{};
    /// Physical fragment relation OID, invalid only for an empty sequence.
    Oid fragment_relid = InvalidOid;
    /// Storage-generation identity of `fragment_relid`.
    std::uint64_t fragment_storage_id = 0;
    /// Persisted sequence alphabet code.
    std::uint8_t alphabet_code = 0;
    /// Packed storage-unit width in bytes.
    std::uint8_t storage_unit_byte_width = 0;
    /// Unused letters in the final packed unit.
    std::uint16_t padding_letters = 0;
    /// CRC-64/ECMA-182 bits for decoded logical symbols.
    std::uint64_t source_content_checksum = 0;
    /// Unpublished, ready, or terminal-invalid state.
    std::int16_t state = k_sequence_source_state_unpublished;
};

/// Optional observational counters for one immutable source-catalog lookup.
/// The lookup owns the authoritative view of its relation/index open sites.
/// Callers that do not request telemetry retain the same path.
struct sequence_source_lookup_telemetry {
    std::uint64_t heap_opens = 0;
    std::uint64_t index_opens = 0;
    std::uint64_t schema_validations = 0;
    std::uint64_t lock_acquires = 0;
    std::uint64_t lock_releases = 0;
};

namespace source_version_detail {

/// Resolve a generated catalog member to its fixed attribute number.
/// @tparam p_member Pointer to a generated source-row member.
template <auto p_member>
inline constexpr AttrNumber source_attribute =
    sequence_source_version_codec::attribute_number<p_member>();

/// Fixed source-epoch attribute number from the generated row codec.
inline constexpr AttrNumber source_epoch =
    source_attribute<&sequence_source_version_catalog_row::source_epoch>;

/// Resolve the extension-owned internal namespace without creating it.
/// @return Namespace OID, or `InvalidOid` when absent.
[[nodiscard]] inline Oid internal_namespace_oid() {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return get_namespace_oid("bioseq_internal", true); });
}

/// Resolve the immutable sequence-source catalog relation.
/// @return Relation OID, or `InvalidOid` when the namespace or relation is absent.
[[nodiscard]] inline Oid relation_oid() {
    const Oid namespace_oid = internal_namespace_oid();
    return OidIsValid(namespace_oid) ? bioseq::postgres::support::invoke_pg_as_cpp([namespace_oid] {
        return get_relname_relid("sequence_source_version", namespace_oid);
    })
                                     : InvalidOid;
}

/// Resolve the immutable sequence-source primary index.
/// @return Index OID, or `InvalidOid` when the namespace or index is absent.
[[nodiscard]] inline Oid primary_index_oid() {
    const Oid namespace_oid = internal_namespace_oid();
    return OidIsValid(namespace_oid) ? bioseq::postgres::support::invoke_pg_as_cpp([namespace_oid] {
        return get_relname_relid("sequence_source_version_pkey", namespace_oid);
    })
                                     : InvalidOid;
}

/// Raise a fail-closed sequence-source catalog contract error.
/// @param message Diagnostic copied before entering PostgreSQL error handling.
/// @throws PostgreSQL ERROR unconditionally.
[[noreturn]] inline void source_catalog_error(std::string_view message) {
    const std::string owned{message};
    bioseq::postgres::support::invoke_pg_as_cpp([&owned] {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq sequence source catalog: %s", owned.c_str())));
    });
    pg_unreachable();
}

/// Validate the exact extension-owned source catalog descriptor before any
/// fixed-ordinal tuple deformation.
/// @param relation Open source catalog held by the caller.
/// @throws PostgreSQL ERROR when relation identity, access method, or any
/// fixed attribute contract differs.
inline void validate_catalog_schema(Relation relation) {
    if (relation == nullptr || RelationGetRelid(relation) != relation_oid() ||
        RelationGetNamespace(relation) != internal_namespace_oid() ||
        relation->rd_rel->relkind != RELKIND_RELATION ||
        relation->rd_rel->relam != HEAP_TABLE_AM_OID) {
        source_catalog_error("catalog relation identity or heap access method changed");
    }
    TupleDesc descriptor = RelationGetDescr(relation);
    if (descriptor == nullptr ||
        descriptor->natts != static_cast<int>(sequence_source_version_codec::field_count)) {
        source_catalog_error("catalog attribute count changed");
    }
    for (const bioseq::postgres::support::row_descriptor_field& expected :
         sequence_source_version_codec::expected_descriptor) {
        const Form_pg_attribute attribute =
            TupleDescAttr(descriptor, expected.attribute_number - 1);
        if (attribute->attnum != expected.attribute_number || attribute->attisdropped ||
            attribute->attnotnull != !expected.nullable ||
            attribute->atttypid != expected.type_oid ||
            std::string_view{NameStr(attribute->attname)} != expected.name) {
            source_catalog_error("catalog attribute contract changed");
        }
    }
}

/// Validate the catalog and discover its sole authoritative primary index.
/// @param relation Open source catalog held by the caller.
/// @param telemetry Optional counters updated for inspected indexes and locks.
/// @return OID of the proven primary index.
/// @throws PostgreSQL ERROR when relation or index contracts differ.
///
/// Every inspected index is opened and closed with `AccessShareLock` during
/// this call. The caller retains ownership of `relation`.
[[nodiscard]] inline Oid
validate_catalog_contract(Relation relation,
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
        const bool is_primary = index->rd_index != nullptr && index->rd_index->indisprimary;
        if (is_primary) {
            ++primary_count;
            const bool exact =
                index_oid == expected_primary && index->rd_rel->relkind == RELKIND_INDEX &&
                index->rd_rel->relam == BTREE_AM_OID && index->rd_index->indisunique &&
                index->rd_index->indimmediate && index->rd_index->indisvalid &&
                index->rd_index->indisready && index->rd_index->indislive &&
                index->rd_index->indnkeyatts == 1 && index->rd_index->indnatts == 1 &&
                index->rd_index->indkey.values[0] == source_epoch &&
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [index] { return RelationGetIndexExpressions(index); }) == NIL &&
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [index] { return RelationGetIndexPredicate(index); }) == NIL &&
                bioseq::postgres::support::invoke_pg_as_cpp([index_oid] {
                    return get_index_column_opclass(index_oid, 1);
                }) == INT8_BTREE_OPS_OID &&
                index->rd_indcollation != nullptr && !OidIsValid(index->rd_indcollation[0]) &&
                index->rd_indoption != nullptr && index->rd_indoption[0] == 0 &&
                (index->rd_opcoptions == nullptr || index->rd_opcoptions[0] == nullptr);
            if (exact)
                proven_primary = index_oid;
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
/// @param relation Open source catalog held by the caller.
/// @param primary Open primary-index relation held by the caller.
/// @param telemetry Optional counters updated for schema validation.
/// @throws PostgreSQL ERROR when relation or index contracts differ.
inline void validate_bound_read_catalog_contract(
    Relation relation, Relation primary, sequence_source_lookup_telemetry* telemetry = nullptr) {
    validate_catalog_schema(relation);
    if (telemetry != nullptr) {
        ++telemetry->schema_validations;
    }
    const Oid expected_primary = primary_index_oid();
    const Form_pg_index index_form = primary == nullptr ? nullptr : primary->rd_index;
    const bool exact =
        OidIsValid(expected_primary) && primary != nullptr &&
        RelationGetRelid(primary) == expected_primary &&
        bioseq::postgres::support::relation_immediate_primary_key_index(relation) == expected_primary &&
        index_form != nullptr &&
        index_form->indrelid == RelationGetRelid(relation) &&
        primary->rd_rel->relkind == RELKIND_INDEX && primary->rd_rel->relam == BTREE_AM_OID &&
        index_form->indisprimary && index_form->indisunique && index_form->indimmediate &&
        index_form->indisvalid && index_form->indisready && index_form->indislive &&
        index_form->indnkeyatts == 1 && index_form->indnatts == 1 &&
        index_form->indkey.values[0] == source_epoch &&
        bioseq::postgres::support::invoke_pg_as_cpp(
            [primary] { return RelationGetIndexExpressions(primary); }) == NIL &&
        bioseq::postgres::support::invoke_pg_as_cpp(
            [primary] { return RelationGetIndexPredicate(primary); }) == NIL &&
        bioseq::postgres::support::invoke_pg_as_cpp([expected_primary] {
            return get_index_column_opclass(expected_primary, 1);
        }) == INT8_BTREE_OPS_OID &&
        primary->rd_indcollation != nullptr && !OidIsValid(primary->rd_indcollation[0]) &&
        primary->rd_indoption != nullptr && primary->rd_indoption[0] == 0 &&
        (primary->rd_opcoptions == nullptr || primary->rd_opcoptions[0] == nullptr);
    if (!exact) {
        source_catalog_error(
            "catalog primary index is not the exact live unique immediate btree contract");
    }
}

/// Decode a catalog bigint that must be strictly positive.
/// @param signed_value Catalog `BIGINT` value.
/// @return Equivalent unsigned value.
/// @throws PostgreSQL ERROR when the value is not positive.
[[nodiscard]] inline std::uint64_t positive_bigint(const std::int64_t signed_value) {
    if (signed_value <= 0) {
        source_catalog_error("positive BIGINT field is invalid");
    }
    return static_cast<std::uint64_t>(signed_value);
}

/// Preserve all bigint bits when decoding an unsigned checksum.
/// @param value Signed catalog storage for the checksum bits.
/// @return Bit-identical unsigned checksum.
[[nodiscard]] inline std::uint64_t bigint_bits(const std::int64_t value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

/// Decode and validate one immutable source-catalog tuple.
/// @param tuple Borrowed heap tuple visible under the caller's snapshot.
/// @param descriptor Borrowed descriptor for the tuple's open relation.
/// @return Owning C++ source record.
/// @throws PostgreSQL ERROR when decoding or any wire-domain check fails.
[[nodiscard]] inline sequence_source_version_row deform(HeapTuple tuple, TupleDesc descriptor) {
    std::optional<sequence_source_version_catalog_row> decoded;
    try {
        decoded.emplace(
            sequence_source_version_codec::deform(tuple, descriptor, CurrentMemoryContext));
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
    const std::int16_t locator_version_value = catalog.locator_version;
    const std::int16_t bucket_value = catalog.bucket_idx;
    if (locator_version_value != sequence_locator_payload_version || bucket_value < 0 ||
        bucket_value > (std::numeric_limits<std::uint8_t>::max)()) {
        source_catalog_error("locator revision or bucket is outside its wire domain");
    }
    row.locator.payload_version = static_cast<std::uint8_t>(locator_version_value);
    row.locator.bucket_index = static_cast<std::uint8_t>(bucket_value);
    const std::int32_t shard_value = catalog.shard_idx;
    if (shard_value < 0) {
        source_catalog_error("locator shard is outside its wire domain");
    }
    row.locator.shard_index = static_cast<std::uint32_t>(shard_value);
    row.fragment_relid = catalog.fragment_relid.value_or(InvalidOid);
    const std::int64_t fragment_storage = catalog.fragment_storage_id;
    if (fragment_storage < 0) {
        source_catalog_error("fragment storage ID is negative");
    }
    row.fragment_storage_id = static_cast<std::uint64_t>(fragment_storage);
    const std::int64_t first_row = catalog.first_sub_row_no;
    const std::int64_t fragments = catalog.num_fragments;
    const std::int64_t total_bp = catalog.total_bp_size;
    if (first_row < 0 || fragments < 0 || total_bp < 0 ||
        static_cast<std::uint64_t>(fragments) > (std::numeric_limits<std::uint32_t>::max)()) {
        source_catalog_error("locator geometry is outside its wire domain");
    }
    row.locator.first_fragment_row = static_cast<std::uint64_t>(first_row);
    row.locator.fragment_count = static_cast<std::uint32_t>(fragments);
    row.locator.logical_length = static_cast<std::uint64_t>(total_bp);
    row.locator.source_epoch = row.source_epoch;
    const std::int16_t alphabet_value = catalog.alphabet_code;
    const std::int16_t storage_width_value = catalog.storage_unit_byte_width;
    if (alphabet_value < 0 || alphabet_value > (std::numeric_limits<std::uint8_t>::max)() ||
        storage_width_value <= 0 || storage_width_value > 0x0F) {
        source_catalog_error("source alphabet or storage width is outside its wire domain");
    }
    row.alphabet_code = static_cast<std::uint8_t>(alphabet_value);
    row.storage_unit_byte_width = static_cast<std::uint8_t>(storage_width_value);
    const std::int32_t padding = catalog.padding_letters;
    if (padding < 0 ||
        static_cast<std::uint32_t>(padding) > (std::numeric_limits<std::uint16_t>::max)()) {
        source_catalog_error("source padding is outside its wire domain");
    }
    row.padding_letters = static_cast<std::uint16_t>(padding);
    row.source_content_checksum = bigint_bits(catalog.source_content_checksum);
    row.state = catalog.state;
    if (row.state != k_sequence_source_state_unpublished &&
        row.state != k_sequence_source_state_ready &&
        row.state != k_sequence_source_state_invalid) {
        source_catalog_error("source state is outside its state-machine domain");
    }
    return row;
}

/// Encode one immutable source record as a catalog heap tuple.
/// @param descriptor Borrowed descriptor for the destination catalog.
/// @param row Validated source record to encode.
/// @return Heap tuple allocated in `CurrentMemoryContext`. The caller owns it
/// and must release it with `heap_freetuple`.
/// @throws PostgreSQL ERROR when row encoding fails.
[[nodiscard]] inline HeapTuple form(TupleDesc descriptor, const sequence_source_version_row& row) {
    const sequence_source_version_catalog_row catalog{
        .source_epoch = static_cast<std::int64_t>(row.source_epoch),
        .user_relid = row.user_relid,
        .database_oid = row.database_oid,
        .tablespace_oid = row.tablespace_oid,
        .main_relfilenode = row.main_relfilenode,
        .main_storage_id = static_cast<std::int64_t>(row.main_storage_id),
        .column_attno = row.column_attno,
        .locator_version = static_cast<std::int16_t>(row.locator.payload_version),
        .bucket_idx = static_cast<std::int16_t>(row.locator.bucket_index),
        .shard_idx = static_cast<std::int32_t>(row.locator.shard_index),
        .fragment_relid =
            OidIsValid(row.fragment_relid) ? std::optional<Oid>{row.fragment_relid} : std::nullopt,
        .fragment_storage_id = static_cast<std::int64_t>(row.fragment_storage_id),
        .first_sub_row_no = static_cast<std::int64_t>(row.locator.first_fragment_row),
        .num_fragments = static_cast<std::int64_t>(row.locator.fragment_count),
        .total_bp_size = static_cast<std::int64_t>(row.locator.logical_length),
        .alphabet_code = static_cast<std::int16_t>(row.alphabet_code),
        .storage_unit_byte_width = static_cast<std::int16_t>(row.storage_unit_byte_width),
        .padding_letters = static_cast<std::int32_t>(row.padding_letters),
        .source_content_checksum = std::bit_cast<std::int64_t>(row.source_content_checksum),
        .state = row.state,
    };
    try {
        return sequence_source_version_codec::encode(catalog, CurrentMemoryContext)
            .form_tuple(descriptor);
    } catch (const bioseq::postgres::support::row_codec_error& error) {
        source_catalog_error(error.what());
    }
}

} // namespace source_version_detail

} // namespace bioseq::storage
