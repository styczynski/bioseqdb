/**
 * @file catalog_lifecycle.cppm
 * @brief Test row codecs aganst catalogs, TOAST data and memory-context cleanup.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "miscadmin.h"
#include "varatt.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.support.row_codec_runtime_test_support:catalog_lifecycle;

import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.indexing.sql_rows;
import bioseq.postgres.storage.io.sql_row_codecs;
import bioseq.postgres.storage.io.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.metadata.sql_row_codecs;
import bioseq.postgres.storage.metadata.sql_rows;
import bioseq.postgres.table.sql_row_codecs;
import bioseq.postgres.table.sql_rows;
import :contract_core;

namespace bioseq::postgres::support::row_codec_runtime_test_support {

/**
 * Check row insertion, decoding, updates and deletion using external TOAST data.
 * Verify decoded values after the original tuple and input memory are released.
 * @return Number of text and bytea fields verified as stored outside the row,
 * or zero if the codec has none.
 */
template <class p_codec,
          class p_live_row_factory,
          class p_live_updated_row_factory,
          class p_apply_update>
[[nodiscard]] std::size_t
exercise_external_toast_lifecycle(Relation relation,
                                  TupleDesc descriptor,
                                  p_live_row_factory make_live_row,
                                  p_live_updated_row_factory make_live_updated_row,
                                  p_apply_update apply_update,
                                  MemoryContext value_context,
                                  MemoryContext result_context,
                                  MemoryContext comparison_context) {
    constexpr std::size_t k_expected_toastable =
        toastable_field_count<p_codec>();
    if constexpr (k_expected_toastable == 0) {
        static_cast<void>(relation);
        static_cast<void>(descriptor);
        return 0;
    }

    p_codec::validate_descriptor(descriptor);
    if (!OidIsValid(relation->rd_rel->reltoastrelid)) {
        contract_failure(std::string{p_codec::row_name} +
                         " external carrier has no TOAST relation");
    }
    using row_type = typename p_codec::row_type;
    const row_type row = make_live_row(value_context);
    auto encoded = p_codec::encode(row, value_context);
    HeapTuple inserted = encoded.form_tuple(descriptor);
    invoke_pg_as_cpp([relation, inserted] { CatalogTupleInsert(relation, inserted); });
    const ItemPointerData inserted_id = inserted->t_self;
    invoke_pg_as_cpp([inserted] { heap_freetuple(inserted); });
    invoke_pg_as_cpp([] { CommandCounterIncrement(); });

    const auto find_by_tid = [relation](ItemPointerData tuple_id) {
        SysScanDesc scan = invoke_pg_as_cpp([relation] {
            return systable_beginscan(relation, InvalidOid, false, SnapshotSelf, 0, nullptr);
        });
        HeapTuple tuple = nullptr;
        while (true) {
            tuple = invoke_pg_as_cpp([scan] { return systable_getnext(scan); });
            if (!HeapTupleIsValid(tuple) || ItemPointerEquals(&tuple->t_self, &tuple_id)) {
                break;
            }
        }
        return std::pair{scan, tuple};
    };

    auto [read_scan, read_tuple] = find_by_tid(inserted_id);
    if (!HeapTupleIsValid(read_tuple)) {
        contract_failure(std::string{p_codec::row_name} +
                         " external carrier tuple was not readable");
    }
    std::size_t verified = 0;
    for (std::size_t index = 0; index < p_codec::field_count; ++index) {
        const Oid type_oid = p_codec::expected_descriptor[index].type_oid;
        if (type_oid != TEXTOID && type_oid != BYTEAOID) {
            continue;
        }
        bool is_null = false;
        const AttrNumber attribute_number = static_cast<AttrNumber>(index + 1);
        const Datum raw = invoke_pg_as_cpp([read_tuple, descriptor, attribute_number, &is_null] {
            return heap_getattr(read_tuple, attribute_number, descriptor, &is_null);
        });
        if (is_null || !VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(raw))) {
            contract_failure(std::string{p_codec::row_name} + " external carrier attribute " +
                             std::to_string(index + 1) + " was not stored out of line");
        }
        ++verified;
    }

    ItemPointerData changed_id{};
    bool result_was_reset = false;
    {
        const row_type read_row = p_codec::deform(read_tuple, descriptor, result_context);
        typename p_codec::update changes{};
        apply_update(changes, value_context);
        HeapTuple changed = changes.modify(read_tuple, descriptor);
        invoke_pg_as_cpp([relation, read_tuple, changed] {
            CatalogTupleUpdate(relation, &read_tuple->t_self, changed);
        });
        changed_id = changed->t_self;
        invoke_pg_as_cpp([changed] { heap_freetuple(changed); });
        invoke_pg_as_cpp([read_scan] { systable_endscan(read_scan); });
        invoke_pg_as_cpp([] { CommandCounterIncrement(); });
        invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
        result_was_reset = verify_decoded_result<p_codec>(read_row,
                                                          descriptor,
                                                          make_live_row,
                                                          comparison_context,
                                                          result_context,
                                                          std::string{p_codec::row_name} +
                                                              " external carrier read");
    }
    if (!result_was_reset) {
        reset_context_once(result_context,
                           std::string{p_codec::row_name} + " external carrier read");
    }

    auto [updated_scan, updated_tuple] = find_by_tid(changed_id);
    if (!HeapTupleIsValid(updated_tuple)) {
        contract_failure(std::string{p_codec::row_name} +
                         " external carrier update was not readable");
    }
    ItemPointerData deleted_id = updated_tuple->t_self;
    result_was_reset = false;
    {
        const row_type updated_row = p_codec::deform(updated_tuple, descriptor, result_context);
        invoke_pg_as_cpp([updated_scan] { systable_endscan(updated_scan); });
        invoke_pg_as_cpp([relation, &deleted_id] { CatalogTupleDelete(relation, &deleted_id); });
        invoke_pg_as_cpp([] { CommandCounterIncrement(); });
        result_was_reset = verify_decoded_result<p_codec>(updated_row,
                                                          descriptor,
                                                          make_live_updated_row,
                                                          comparison_context,
                                                          result_context,
                                                          std::string{p_codec::row_name} +
                                                              " external carrier update");
    }
    if (!result_was_reset) {
        reset_context_once(result_context,
                           std::string{p_codec::row_name} + " external carrier update");
    }

    auto [deleted_scan, deleted_tuple] = find_by_tid(deleted_id);
    const bool deleted = !HeapTupleIsValid(deleted_tuple);
    invoke_pg_as_cpp([deleted_scan] { systable_endscan(deleted_scan); });
    if (!deleted) {
        contract_failure(std::string{p_codec::row_name} +
                         " external carrier delete remained visible");
    }
    return verified;
}

/**
 * Create a temporary copy of the source table's columns and defaults for TOAST tests.
 * The copy has no indexes and is dropped at commit.
 * @return Open  test table held with RowExclusiveLock.
 */
template <class p_codec>
[[nodiscard]] Relation create_unindexed_toast_carrier(Relation source_relation) {
    ++toast_carrier_sequence;
    const std::string carrier_name =
        fmt::format("row_codec_toast_{}_{}", probe_invocation, toast_carrier_sequence);
    const Oid source_namespace_oid = RelationGetNamespace(source_relation);
    const char* source_namespace = invoke_pg_as_cpp(
        [source_namespace_oid] { return get_namespace_name(source_namespace_oid); });
    if (source_namespace == nullptr) {
        contract_failure(std::string{p_codec::row_name} + " source namespace is missing");
    }
    const std::string command = fmt::format("CREATE TEMP TABLE {} "
                                            "(LIKE {}.{} INCLUDING DEFAULTS) ON COMMIT DROP",
                                            carrier_name,
                                            source_namespace,
                                            RelationGetRelationName(source_relation));
    const char* command_bytes = command.c_str();
    invoke_pg_as_cpp([command_bytes] {
        const int connected = SPI_connect();
        if (connected != SPI_OK_CONNECT) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal("could not connect SPI for "
                                     "row-codec TOAST carrier")));
        }
        const int created = SPI_execute(command_bytes, false, 0);
        const int finished = SPI_finish();
        if (created != SPI_OK_UTILITY || finished != SPI_OK_FINISH) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal("could not create row-codec "
                                     "TOAST carrier")));
        }
    });
    const char* carrier_name_bytes = carrier_name.c_str();
    const Oid relation_oid = invoke_pg_as_cpp([carrier_name_bytes] {
        Oid temp_namespace = InvalidOid;
        Oid temp_toast_namespace = InvalidOid;
        GetTempNamespaceState(&temp_namespace, &temp_toast_namespace);
        const Oid resolved = get_relname_relid(carrier_name_bytes, temp_namespace);
        if (!OidIsValid(resolved)) {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg_internal("row-codec TOAST carrier is missing")));
        }
        return resolved;
    });
    return invoke_pg_as_cpp([relation_oid] { return table_open(relation_oid, RowExclusiveLock); });
}

/**
 * Test a codec's tuple conversion, NULL checks and decoded-value lifetimes.
 * Table codecs also insert, update and delete catalog rows. Text and bytea
 * fields are checked, with values stored in external TOAST tables
 * @return Field counts and the checks completed for this codec.
 */
template <class p_codec,
          class p_null_row_factory,
          class p_full_row_factory,
          class p_live_row_factory,
          class p_updated_row_factory,
          class p_live_updated_row_factory,
          class p_apply_update,
          class p_verify_scan>
[[nodiscard]] runtime_receipt_entry
exercise_codec_with_external(TupleDesc descriptor,
                             Relation live_relation,
                             Relation external_relation,
                             TupleDesc external_descriptor,
                             std::string_view entity_kind,
                             std::string_view owner,
                             p_null_row_factory make_null_row,
                             p_full_row_factory make_full_row,
                             p_live_row_factory make_live_row,
                             p_updated_row_factory make_updated_row,
                             p_live_updated_row_factory make_live_updated_row,
                             p_apply_update apply_update,
                             p_verify_scan verify_scan) {
    using row_type = typename p_codec::row_type;
    static_assert(p_codec::field_count == p_codec::expected_descriptor.size());
    static_assert(by_reference_field_count<p_codec>() ==
                  context_pointer_field_count<p_codec>() +
                      owned_value_byref_field_count<p_codec>());
    static_assert(toastable_field_count<p_codec>() == context_pointer_field_count<p_codec>() +
                                                          owned_text_string_field_count<p_codec>());
    validate_descriptor_mutations<p_codec>(descriptor);

    MemoryContext value_context = make_context();
    MemoryContext result_context = make_context();
    MemoryContext comparison_context = make_context();

    bool result_was_reset = false;
    {
        const row_type row = make_null_row(value_context);
        auto encoded = p_codec::encode(row, value_context);
        const auto tuple_data = std::as_const(encoded).tuple_data(descriptor);
        require_encoded_null_pattern<p_codec>(tuple_data, true);
        HeapTuple tuple = encoded.form_tuple(descriptor);
        const row_type decoded = p_codec::deform(tuple, descriptor, result_context);
        invoke_pg_as_cpp([tuple] { heap_freetuple(tuple); });
        invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
        result_was_reset =
            verify_decoded_result<p_codec>(decoded,
                                           descriptor,
                                           make_null_row,
                                           comparison_context,
                                           result_context,
                                           std::string{p_codec::row_name} + " NULL result");
    }
    if (!result_was_reset) {
        reset_context_once(result_context, p_codec::row_name);
    }

    result_was_reset = false;
    {
        const row_type row = make_full_row(value_context);
        auto encoded = p_codec::encode(row, value_context);
        const auto tuple_data = std::as_const(encoded).tuple_data(descriptor);
        require_encoded_null_pattern<p_codec>(tuple_data, false);
        HeapTuple tuple = encoded.form_tuple(descriptor);
        const row_type decoded = p_codec::deform(tuple, descriptor, result_context);
        invoke_pg_as_cpp([tuple] { heap_freetuple(tuple); });
        invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
        result_was_reset =
            verify_decoded_result<p_codec>(decoded,
                                           descriptor,
                                           make_full_row,
                                           comparison_context,
                                           result_context,
                                           std::string{p_codec::row_name} + " inline result");
    }
    if (!result_was_reset) {
        reset_context_once(result_context, p_codec::row_name);
    }

    result_was_reset = false;
    {
        const row_type row = make_full_row(value_context);
        auto encoded = p_codec::encode(row, value_context);
        HeapTuple old_tuple = encoded.form_tuple(descriptor);
        typename p_codec::update changes{};
        apply_update(changes, value_context);
        HeapTuple changed_tuple = changes.modify(old_tuple, descriptor);
        const row_type decoded = p_codec::deform(changed_tuple, descriptor, result_context);
        invoke_pg_as_cpp([old_tuple] { heap_freetuple(old_tuple); });
        invoke_pg_as_cpp([changed_tuple] { heap_freetuple(changed_tuple); });
        invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
        result_was_reset =
            verify_decoded_result<p_codec>(decoded,
                                           descriptor,
                                           make_updated_row,
                                           comparison_context,
                                           result_context,
                                           std::string{p_codec::row_name} + " inline update");
    }
    if (!result_was_reset) {
        reset_context_once(result_context, p_codec::row_name);
    }

    constexpr std::size_t k_nullable_count = nullable_field_count<p_codec>();
    constexpr std::size_t k_required_count =
        p_codec::field_count - k_nullable_count;
    for (std::size_t required_index = 0; required_index < p_codec::field_count; ++required_index) {
        if (p_codec::expected_descriptor[required_index].nullable) {
            continue;
        }
        const row_type row = make_full_row(value_context);
        auto encoded = p_codec::encode(row, value_context);
        auto tuple_data = encoded.tuple_data(descriptor);
        tuple_data.nulls[required_index] = true;
        HeapTuple tuple = encoded.form_tuple(descriptor);
        bool rejected = false;
        try {
            static_cast<void>(p_codec::deform(tuple, descriptor, result_context));
        } catch (const row_codec_error&) {
            rejected = true;
        }
        invoke_pg_as_cpp([tuple] { heap_freetuple(tuple); });
        invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
        invoke_pg_as_cpp([result_context] { MemoryContextReset(result_context); });
        if (!rejected) {
            contract_failure(std::string{p_codec::row_name} +
                             " accepted NULL for required attribute " +
                             std::to_string(required_index + 1));
        }
    }

    {
        const row_type row = make_full_row(value_context);
        auto encoded = p_codec::encode(row, value_context);
        HeapTuple tuple = encoded.form_tuple(descriptor);
        bool rejected = false;
        try {
            static_cast<void>(p_codec::deform(tuple, descriptor, nullptr));
        } catch (const row_codec_error&) {
            rejected = true;
        }
        invoke_pg_as_cpp([tuple] { heap_freetuple(tuple); });
        invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
        if (!rejected) {
            contract_failure(std::string{p_codec::row_name} +
                             " accepted a null deformation result context");
        }
    }

    const std::optional<ScanKeyData> identity_key = verify_scan();
    bool catalog_crud_completed = false;
    std::size_t external_toast_verified = 0;

    if (live_relation != nullptr) {
        if (entity_kind == "sql:table" && !identity_key.has_value()) {
            contract_failure(std::string{p_codec::row_name} +
                             " live table has no collision-check scan key");
        }
        if (identity_key.has_value()) {
            ScanKeyData key = *identity_key;
            SysScanDesc collision_scan = invoke_pg_as_cpp([live_relation, &key] {
                return systable_beginscan(live_relation, InvalidOid, false, SnapshotSelf, 1, &key);
            });
            HeapTuple collision =
                invoke_pg_as_cpp([collision_scan] { return systable_getnext(collision_scan); });
            const bool absent = !HeapTupleIsValid(collision);
            invoke_pg_as_cpp([collision_scan] { systable_endscan(collision_scan); });
            if (!absent) {
                contract_failure(std::string{p_codec::row_name} +
                                 " live probe identity already exists");
            }
        }

        const row_type row = make_full_row(value_context);
        auto encoded = p_codec::encode(row, value_context);
        HeapTuple inserted = encoded.form_tuple(descriptor);
        invoke_pg_as_cpp(
            [live_relation, inserted] { CatalogTupleInsert(live_relation, inserted); });
        const ItemPointerData inserted_id = inserted->t_self;
        invoke_pg_as_cpp([inserted] { heap_freetuple(inserted); });
        invoke_pg_as_cpp([] { CommandCounterIncrement(); });

        const auto find_by_tid = [live_relation](ItemPointerData tuple_id) {
            SysScanDesc scan = invoke_pg_as_cpp([live_relation] {
                return systable_beginscan(
                    live_relation, InvalidOid, false, SnapshotSelf, 0, nullptr);
            });
            HeapTuple tuple = nullptr;
            while (true) {
                tuple = invoke_pg_as_cpp([scan] { return systable_getnext(scan); });
                if (!HeapTupleIsValid(tuple) || ItemPointerEquals(&tuple->t_self, &tuple_id)) {
                    break;
                }
            }
            return std::pair{scan, tuple};
        };

        auto [read_scan, read_tuple] = find_by_tid(inserted_id);
        if (!HeapTupleIsValid(read_tuple)) {
            contract_failure(std::string{p_codec::row_name} + " inserted tuple was not readable");
        }

        ItemPointerData changed_id{};
        result_was_reset = false;
        {
            const row_type read_row = p_codec::deform(read_tuple, descriptor, result_context);
            typename p_codec::update live_changes{};
            apply_update(live_changes, value_context);
            HeapTuple changed = live_changes.modify(read_tuple, descriptor);
            invoke_pg_as_cpp([live_relation, read_tuple, changed] {
                CatalogTupleUpdate(live_relation, &read_tuple->t_self, changed);
            });
            changed_id = changed->t_self;
            invoke_pg_as_cpp([changed] { heap_freetuple(changed); });
            invoke_pg_as_cpp([read_scan] { systable_endscan(read_scan); });
            invoke_pg_as_cpp([] { CommandCounterIncrement(); });
            invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
            result_was_reset =
                verify_decoded_result<p_codec>(read_row,
                                               descriptor,
                                               make_full_row,
                                               comparison_context,
                                               result_context,
                                               std::string{p_codec::row_name} + " catalog read");
        }
        if (!result_was_reset) {
            reset_context_once(result_context, std::string{p_codec::row_name} + " catalog read");
        }

        auto [updated_scan, updated_tuple] = find_by_tid(changed_id);
        if (!HeapTupleIsValid(updated_tuple)) {
            contract_failure(std::string{p_codec::row_name} + " updated tuple was not readable");
        }
        ItemPointerData deleted_id = updated_tuple->t_self;
        result_was_reset = false;
        {
            const row_type updated_row = p_codec::deform(updated_tuple, descriptor, result_context);
            invoke_pg_as_cpp([updated_scan] { systable_endscan(updated_scan); });
            invoke_pg_as_cpp(
                [live_relation, &deleted_id] { CatalogTupleDelete(live_relation, &deleted_id); });
            invoke_pg_as_cpp([] { CommandCounterIncrement(); });
            result_was_reset =
                verify_decoded_result<p_codec>(updated_row,
                                               descriptor,
                                               make_updated_row,
                                               comparison_context,
                                               result_context,
                                               std::string{p_codec::row_name} + " catalog update");
        }
        if (!result_was_reset) {
            reset_context_once(result_context, std::string{p_codec::row_name} + " catalog update");
        }

        auto [deleted_scan, deleted_tuple] = find_by_tid(deleted_id);
        const bool deleted = !HeapTupleIsValid(deleted_tuple);
        invoke_pg_as_cpp([deleted_scan] { systable_endscan(deleted_scan); });
        if (!deleted) {
            contract_failure(std::string{p_codec::row_name} + " deleted tuple remained visible");
        }
        catalog_crud_completed = true;
    }

    constexpr std::size_t k_toastable_count = toastable_field_count<p_codec>();
    if constexpr (k_toastable_count != 0) {
        Relation toast_relation = external_relation;
        TupleDesc toast_descriptor = external_descriptor;
        bool owns_toast_relation = false;
        if (toast_relation == nullptr && entity_kind == "sql:table" && live_relation != nullptr) {
            toast_relation = create_unindexed_toast_carrier<p_codec>(live_relation);
            toast_descriptor = RelationGetDescr(toast_relation);
            owns_toast_relation = true;
        }
        if (toast_relation == nullptr || toast_descriptor == nullptr) {
            contract_failure(std::string{p_codec::row_name} + " has no external-TOAST carrier");
        }
        external_toast_verified = exercise_external_toast_lifecycle<p_codec>(toast_relation,
                                                                             toast_descriptor,
                                                                             make_live_row,
                                                                             make_live_updated_row,
                                                                             apply_update,
                                                                             value_context,
                                                                             result_context,
                                                                             comparison_context);
        if (owns_toast_relation) {
            invoke_pg_as_cpp([toast_relation] { table_close(toast_relation, RowExclusiveLock); });
        }
    }
    if (external_toast_verified != k_toastable_count) {
        contract_failure(std::string{p_codec::row_name} +
                         " external-TOAST evidence count mismatch");
    }
    if (entity_kind == "sql:table" && !catalog_crud_completed) {
        contract_failure(std::string{p_codec::row_name} + " did not complete live catalog CRUD");
    }

    invoke_pg_as_cpp([comparison_context] { MemoryContextDelete(comparison_context); });
    invoke_pg_as_cpp([result_context] { MemoryContextDelete(result_context); });
    invoke_pg_as_cpp([value_context] { MemoryContextDelete(value_context); });

    return runtime_receipt_entry{
        .identity = std::string{entity_kind} + ":" + std::string{p_codec::row_name},
        .owner = std::string{owner},
        .fields = p_codec::field_count,
        .nullable_fields = k_nullable_count,
        .by_reference_fields = by_reference_field_count<p_codec>(),
        .toastable_fields = k_toastable_count,
        .required_null_checks = k_required_count,
        .scan_policy_verified = identity_key.has_value(),
        .catalog_crud_completed = catalog_crud_completed,
    };
}

/**
 * Test a codec without a caller-supplied TOAST test table. For table codecs,
 * the shared test function creates a temporary table when one is needed.
 * @return Field counts and completed checks.
 */
template <class p_codec,
          class p_null_row_factory,
          class p_full_row_factory,
          class p_live_row_factory,
          class p_updated_row_factory,
          class p_live_updated_row_factory,
          class p_apply_update,
          class p_verify_scan>
[[nodiscard]] runtime_receipt_entry exercise_codec(TupleDesc descriptor,
                                                   Relation live_relation,
                                                   std::string_view entity_kind,
                                                   std::string_view owner,
                                                   p_null_row_factory make_null_row,
                                                   p_full_row_factory make_full_row,
                                                   p_live_row_factory make_live_row,
                                                   p_updated_row_factory make_updated_row,
                                                   p_live_updated_row_factory make_live_updated_row,
                                                   p_apply_update apply_update,
                                                   p_verify_scan verify_scan) {
    return exercise_codec_with_external<p_codec>(descriptor,
                                                 live_relation,
                                                 nullptr,
                                                 nullptr,
                                                 entity_kind,
                                                 owner,
                                                 make_null_row,
                                                 make_full_row,
                                                 make_live_row,
                                                 make_updated_row,
                                                 make_live_updated_row,
                                                 apply_update,
                                                 verify_scan);
}

/**
 * Open a catalog table and pass its descriptor and relation to `exercise`.
 * @return The callback's test report.
 */
template <class p_codec, class p_exercise>
[[nodiscard]] runtime_receipt_entry
with_relation_descriptor(const char* schema_name, const char* relation_name, p_exercise exercise) {
    const Oid relation_oid = invoke_pg_as_cpp([schema_name, relation_name] {
        const Oid namespace_oid = get_namespace_oid(schema_name, false);
        const Oid resolved_relation_oid = get_relname_relid(relation_name, namespace_oid);
        if (!OidIsValid(resolved_relation_oid)) {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg_internal("generated row-codec relation %s.%s "
                                     "is missing",
                                     schema_name,
                                     relation_name)));
        }
        return resolved_relation_oid;
    });
    Relation relation =
        invoke_pg_as_cpp([relation_oid] { return table_open(relation_oid, RowExclusiveLock); });
    TupleDesc descriptor = RelationGetDescr(relation);
    const runtime_receipt_entry result = exercise(descriptor, relation);
    invoke_pg_as_cpp([relation] { table_close(relation, RowExclusiveLock); });
    return result;
}

/**
 * Pass a composite type's descriptor to `exercise`, with a null relation.
 * `$extension` selects the installed Bioseq schema.
 * @return The callback's test  report.
 */
template <class p_codec, class p_exercise>
[[nodiscard]] runtime_receipt_entry
with_composite_descriptor(const char* schema_name, const char* type_name, p_exercise exercise) {
    const Oid type_oid = invoke_pg_as_cpp([schema_name, type_name] {
        Oid namespace_oid = InvalidOid;
        if (std::string_view{schema_name} == "$extension") {
            const Oid extension_oid = get_extension_oid("bioseq", false);
            namespace_oid = get_extension_schema(extension_oid);
        } else {
            namespace_oid = get_namespace_oid(schema_name, false);
        }
        const Oid resolved = GetSysCacheOid2(TYPENAMENSP,
                                             Anum_pg_type_oid,
                                             CStringGetDatum(type_name),
                                             ObjectIdGetDatum(namespace_oid));
        if (!OidIsValid(resolved)) {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg_internal("generated row-codec composite %s.%s "
                                     "is missing",
                                     schema_name,
                                     type_name)));
        }
        return resolved;
    });
    TupleDesc descriptor =
        invoke_pg_as_cpp([type_oid] { return lookup_rowtype_tupdesc(type_oid, -1); });
    const runtime_receipt_entry result = exercise(descriptor, nullptr);
    invoke_pg_as_cpp([descriptor] { ReleaseTupleDesc(descriptor); });
    return result;
}

/** Sort reports by codec identity and join them into a semicolon-separated string. */
[[nodiscard]] std::string render_receipt(std::vector<runtime_receipt_entry> entries) {
    std::ranges::sort(entries, {}, &runtime_receipt_entry::identity);
    std::string result;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) {
            result.push_back(';');
        }
        const auto& entry = entries[index];
        result += entry.identity;
        result += "|owner=";
        result += entry.owner;
        result += "|fields=";
        result += std::to_string(entry.fields);
        result += "|nullable=";
        result += std::to_string(entry.nullable_fields);
        result += "|required-null-checks=";
        result += std::to_string(entry.required_null_checks);
        result += "|byref=";
        result += std::to_string(entry.by_reference_fields);
        result += "|toastable=";
        result += std::to_string(entry.toastable_fields);
        result += "|inline-lifetime=";
        result += std::to_string(entry.by_reference_fields);
        result += "|external-toast=";
        result += std::to_string(entry.toastable_fields);
        result += "|scan=";
        if (entry.identity.starts_with("sql:table:")) {
            result += entry.scan_policy_verified ? "verified" : "missing";
        } else {
            result += "not-applicable";
        }
        result += "|catalog=";
        if (entry.identity.starts_with("sql:table:")) {
            result += entry.catalog_crud_completed ? "insert+read+update+delete" : "missing";
        } else {
            result += "not-applicable";
        }
        result += "|ops=";
        result += k_operation_evidence;
        result += "|lifetime=verified";
    }
    return result;
}

/** @brief Rolls back @p transaction and rethrows the active exception. */
[[noreturn]] void rethrow_after_rollback(subtransaction& transaction) {
    const std::exception_ptr original = std::current_exception();
    try {
        transaction.rollback();
    } catch (...) {
        /* Keep the original codec failure if rollback also reports an ermor. */
    }
    std::rethrow_exception(original);
}

} // namespace bioseq::postgres::support::row_codec_runtime_test_support
