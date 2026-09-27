/**
 * @file publication.cppm
 * @brief Allocate sequence versions and publish or invalidate their catalog records.
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

export module bioseq.postgres.storage.manager:sequence_storage_catalog_publication;

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
import bioseq.common.seqan_adapter; // ceil_log2 for calculating letters per storage unit.
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
// Bucket calculations and sequence_locator are independent of PostgreSQL
// so unit tests can use them.
import bioseq.storage.layout;

export import :sequence_storage_catalog_generation;

export namespace bioseq::storage {
class pending_sequence_source;

/// Manage catalog records for sequence versions identified by source epochs.
/// Reads use the supplied snapshot. Publication and invalidation use SnapshotSelf
/// so they can see changes made by the current transaction.
struct sequence_source_version_manager {
    /// Allocate a new positive source epoch from the extension's sequence.
    /// @return A value that fits PostgreSQL bigint.
    /// @throws A PostgreSQL error if the sequence is missing, altered or exhausted.
    [[nodiscard]] static std::uint64_t allocate_epoch() {
        const Oid namespace_oid = source_version_detail::internal_namespace_oid();
        const Oid sequence_oid =
            storage_manager_entity_handles::sequence_source_epoch_seq.resolve_oid(
                bioseq::postgres::support::catalog_missing_policy::return_invalid);
        if (!OidIsValid(sequence_oid)) {
            source_version_detail::source_catalog_error("owned epoch sequence is missing");
        }
        Relation sequence_relation = bioseq::postgres::support::invoke_pg_as_cpp(
            [sequence_oid] { return relation_open(sequence_oid, AccessShareLock); });
        const bool shape_ok = RelationGetNamespace(sequence_relation) == namespace_oid &&
                              sequence_relation->rd_rel->relkind == RELKIND_SEQUENCE &&
                              sequence_relation->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [sequence_relation] { relation_close(sequence_relation, AccessShareLock); });
        if (!shape_ok) {
            source_version_detail::source_catalog_error(
                "owned epoch sequence relation contract changed");
        }
        HeapTuple sequence_tuple = bioseq::postgres::support::invoke_pg_as_cpp(
            [sequence_oid] { return SearchSysCache1(SEQRELID, ObjectIdGetDatum(sequence_oid)); });
        if (!HeapTupleIsValid(sequence_tuple)) {
            source_version_detail::source_catalog_error("owned epoch pg_sequence row is missing");
        }
        const Form_pg_sequence sequence =
            reinterpret_cast<Form_pg_sequence>(GETSTRUCT(sequence_tuple));
        const bool contract_ok =
            sequence->seqtypid == INT8OID && sequence->seqincrement == 1 && sequence->seqmin == 1 &&
            sequence->seqmax == (std::numeric_limits<std::int64_t>::max)() &&
            sequence->seqstart == 1 && sequence->seqcache == 1 && !sequence->seqcycle;
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

    /// Record a new sequence version as unpublished.
    /// Check its locator and storage identity, then insert under RowExclusiveLock.
    /// @param input Source record whose epoch matches its locator.
    /// @throws A PostgreSQL error if validation or insertion fails.
    static void insert_unpublished(const sequence_source_version_row& input) {
        if (input.source_epoch == 0 ||
            input.source_epoch >
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
            input.locator.source_epoch != input.source_epoch ||
            input.locator.payload_version != sequence_locator_payload_version ||
            input.locator.reserved_16 != 0 || input.locator.reserved_32 != 0 ||
            input.locator.shard_index >
                static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) ||
            input.locator.first_fragment_row >
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
            input.locator.logical_length >
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
            !OidIsValid(input.user_relid) || input.database_oid != MyDatabaseId ||
            !OidIsValid(input.tablespace_oid) || !OidIsValid(input.main_relfilenode) ||
            input.main_storage_id == 0 ||
            input.main_storage_id >
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
            input.column_attno <= 0 || input.storage_unit_byte_width == 0 ||
            input.storage_unit_byte_width > 0x0Fu ||
            input.state != k_sequence_source_state_unpublished ||
            (input.locator.fragment_count == 0
                 ? (OidIsValid(input.fragment_relid) || input.fragment_storage_id != 0 ||
                    input.locator.first_fragment_row != 0)
                 : (!OidIsValid(input.fragment_relid) || input.fragment_storage_id == 0 ||
                    input.fragment_storage_id >
                        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())))) {
            source_version_detail::source_catalog_error("unpublished source input is invalid");
        }
        Relation relation = nullptr;
        HeapTuple tuple = nullptr;
        std::exception_ptr failure;
        try {
            const Oid relation_oid = source_version_detail::relation_oid();
            relation = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_oid] { return table_open(relation_oid, RowExclusiveLock); });
            source_version_detail::validate_catalog_schema(relation);
            tuple = source_version_detail::form(RelationGetDescr(relation), input);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation, tuple] { CatalogTupleInsert(relation, tuple); });
        } catch (...) {
            failure = std::current_exception();
        }
        // Run each release step while keeping the first insertion failure.
        const auto cleanup = [&failure](auto&& operation) {
            try {
                operation();
            } catch (...) {
                if (failure == nullptr)
                    failure = std::current_exception();
            }
        };
        if (tuple != nullptr) {
            cleanup([tuple] {
                bioseq::postgres::support::invoke_pg_as_cpp([tuple] { heap_freetuple(tuple); });
            });
        }
        if (relation != nullptr) {
            cleanup([relation] {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation] { table_close(relation, RowExclusiveLock); });
            });
        }
        if (failure != nullptr)
            std::rethrow_exception(failure);
        bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
    }

    /// Read a source version through its checked primary index.
    /// Release both relation locks when done.
    /// @param epoch Source epoch.
    /// @param snapshot Borrowed scan snapshot.
    /// @param require_ready Return ready versions only.
    /// @param exact_primary_index_scan Optional flag set after a completed scan.
    /// @param telemetry Optional read and lock counters.
    /// @return The row, or no value for invalid input, absence or a filtered state.
    /// @throws A PostgreSQL error if the catalog or index is invalid.
    [[nodiscard]] static std::optional<sequence_source_version_row>
    lookup(std::uint64_t epoch,
           Snapshot snapshot,
           bool require_ready = true,
           bool* exact_primary_index_scan = nullptr,
           sequence_source_lookup_telemetry* telemetry = nullptr) {
        if (exact_primary_index_scan != nullptr) {
            *exact_primary_index_scan = false;
        }
        if (telemetry != nullptr) {
            *telemetry = {};
        }
        if (epoch == 0 ||
            epoch > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
            snapshot == nullptr) {
            return std::nullopt;
        }
        Relation relation = table_open(source_version_detail::relation_oid(), AccessShareLock);
        if (telemetry != nullptr) {
            ++telemetry->heap_opens;
            ++telemetry->lock_acquires;
        }
        const Oid primary_oid = source_version_detail::primary_index_oid();
        if (!OidIsValid(primary_oid)) {
            source_version_detail::source_catalog_error("catalog primary index is missing");
        }
        Relation primary = index_open(primary_oid, AccessShareLock);
        if (telemetry != nullptr) {
            ++telemetry->index_opens;
            ++telemetry->lock_acquires;
        }
        source_version_detail::validate_bound_read_catalog_contract(relation, primary, telemetry);
        ScanKeyData key = sequence_source_version_codec::scan_key<
            &sequence_source_version_catalog_row::source_epoch>(static_cast<std::int64_t>(epoch));
        SysScanDesc scan = systable_beginscan_ordered(relation, primary, snapshot, 1, &key);
        HeapTuple tuple = systable_getnext_ordered(scan, ForwardScanDirection);
        std::optional<sequence_source_version_row> result;
        if (HeapTupleIsValid(tuple)) {
            sequence_source_version_row row =
                source_version_detail::deform(tuple, RelationGetDescr(relation));
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
    /// Change a sequence version to ready or invalid if that transition is allowed.
    /// @param epoch Existing source epoch.
    /// @param requested_state Ready or invalid.
    /// @throws A PostgreSQL error if the row is missing or the transition fails.
    static void set_state(std::uint64_t epoch, std::int16_t requested_state) {
        if (epoch == 0 ||
            epoch > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
            (requested_state != k_sequence_source_state_ready &&
             requested_state != k_sequence_source_state_invalid)) {
            source_version_detail::source_catalog_error("source state transition input is invalid");
        }
        Relation relation = nullptr;
        SysScanDesc scan = nullptr;
        HeapTuple new_tuple = nullptr;
        std::exception_ptr failure;
        try {
            const Oid relation_oid = source_version_detail::relation_oid();
            relation = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_oid] { return table_open(relation_oid, RowExclusiveLock); });
            const Oid proven_primary = source_version_detail::validate_catalog_contract(relation);
            ScanKeyData key = sequence_source_version_codec::scan_key<
                &sequence_source_version_catalog_row::source_epoch>(
                static_cast<std::int64_t>(epoch));
            scan = bioseq::postgres::support::invoke_pg_as_cpp([relation, proven_primary, &key] {
                return systable_beginscan(relation, proven_primary, true, SnapshotSelf, 1, &key);
            });
            HeapTuple old_tuple = bioseq::postgres::support::invoke_pg_as_cpp(
                [scan] { return systable_getnext(scan); });
            if (!HeapTupleIsValid(old_tuple)) {
                source_version_detail::source_catalog_error(
                    "source epoch row is missing during state transition");
            }
            const sequence_source_version_row current =
                source_version_detail::deform(old_tuple, RelationGetDescr(relation));
            if (!sequence_source_state_transition_allowed(current.state, requested_state)) {
                source_version_detail::source_catalog_error(
                    "source state transition violates the terminal state machine");
            }
            sequence_source_version_codec::update update{};
            update.set<&sequence_source_version_catalog_row::state>(requested_state);
            new_tuple = update.modify(old_tuple, RelationGetDescr(relation));
            bioseq::postgres::support::invoke_pg_as_cpp([relation, old_tuple, new_tuple] {
                CatalogTupleUpdate(relation, &old_tuple->t_self, new_tuple);
            });
        } catch (...) {
            failure = std::current_exception();
        }
        // Run each release step while keeping the first transition failure.
        const auto cleanup = [&failure](auto&& operation) {
            try {
                operation();
            } catch (...) {
                if (failure == nullptr)
                    failure = std::current_exception();
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
                bioseq::postgres::support::invoke_pg_as_cpp([scan] { systable_endscan(scan); });
            });
        }
        if (relation != nullptr) {
            cleanup([relation] {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation] { table_close(relation, RowExclusiveLock); });
            });
        }
        if (failure != nullptr)
            std::rethrow_exception(failure);
        bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
    }

    /// Mark an unpublished sequence version ready for readers.
    /// @param epoch Unpublished source epoch.
    /// @throws A PostgreSQL error if publication is forbidden or fails.
    static void publish_ready(std::uint64_t epoch) {
        set_state(epoch, k_sequence_source_state_ready);
    }

    friend class pending_sequence_source;

  public:
    /// Mark a sequence version invalid so it can no longer be published.
    /// @param epoch Source epoch. Zero, absent and already invalid versions are ignored.
    /// @throws A PostgreSQL error if lookup or invalidation fails.
    static void invalidate_epoch(std::uint64_t epoch) {
        if (epoch == 0)
            return;
        const auto row = lookup(epoch, SnapshotSelf, /*require_ready=*/false);
        if (!row || row->state == k_sequence_source_state_invalid)
            return;
        set_state(epoch, k_sequence_source_state_invalid);
    }

    /// Delete source records belonging to one physical parent table.
    /// Scan with SnapshotSelf under RowExclusiveLock.
    /// @param database_oid Parent database.
    /// @param tablespace_oid Parent tablespace.
    /// @param main_relfilenode Parent file number.
    /// @param allow_missing_during_extension_teardown Allow an absent catalog during extension removal.
    /// @throws A PostgreSQL error if the required catalog is absent or invalid.
    static void delete_for_main(Oid database_oid,
                                Oid tablespace_oid,
                                Oid main_relfilenode,
                                bool allow_missing_during_extension_teardown = false) {
        const Oid relation_oid = source_version_detail::relation_oid();
        if (!OidIsValid(relation_oid)) {
            if (internal_catalog_detail::absence_is_benign(allow_missing_during_extension_teardown))
                return;
            internal_catalog_detail::missing_outside_teardown(
                "bioseq_internal.sequence_source_version");
        }
        Relation relation =
            bioseq::postgres::support::try_open_postgres_relation(relation_oid, RowExclusiveLock);
        if (relation == nullptr) {
            if (internal_catalog_detail::absence_is_benign(allow_missing_during_extension_teardown))
                return;
            internal_catalog_detail::missing_outside_teardown(
                "bioseq_internal.sequence_source_version");
        }
        source_version_detail::validate_catalog_schema(relation);
        std::array<ScanKeyData, 3> keys{
            sequence_source_version_codec::scan_key<
                &sequence_source_version_catalog_row::database_oid>(database_oid),
            sequence_source_version_codec::scan_key<
                &sequence_source_version_catalog_row::tablespace_oid>(tablespace_oid),
            sequence_source_version_codec::scan_key<
                &sequence_source_version_catalog_row::main_relfilenode>(main_relfilenode),
        };
        SysScanDesc scan = systable_beginscan(
            relation, InvalidOid, false, SnapshotSelf, static_cast<int>(keys.size()), keys.data());
        HeapTuple tuple = nullptr;
        while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
            CatalogTupleDelete(relation, &tuple->t_self);
        }
        systable_endscan(scan);
        table_close(relation, RowExclusiveLock);
        CommandCounterIncrement();
    }

    /// Invalidate every source version that uses a fragment table.
    /// Scan with SnapshotSelf under RowExclusiveLock.
    /// @param fragment_relation_oid Fragment table, or InvalidOid for no work.
    /// @param allow_missing_during_extension_teardown Allow an absent catalog during extension removal.
    /// @throws A PostgreSQL error if the required catalog is absent or invalid.
    static void invalidate_for_fragment(Oid fragment_relation_oid,
                                        bool allow_missing_during_extension_teardown = false) {
        if (!OidIsValid(fragment_relation_oid))
            return;
        const Oid catalog_oid = source_version_detail::relation_oid();
        if (!OidIsValid(catalog_oid)) {
            if (internal_catalog_detail::absence_is_benign(allow_missing_during_extension_teardown))
                return;
            internal_catalog_detail::missing_outside_teardown(
                "bioseq_internal.sequence_source_version");
        }
        Relation relation =
            bioseq::postgres::support::try_open_postgres_relation(catalog_oid, RowExclusiveLock);
        if (relation == nullptr) {
            if (internal_catalog_detail::absence_is_benign(allow_missing_during_extension_teardown))
                return;
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
            sequence_source_version_codec::
                field<&sequence_source_version_catalog_row::fragment_relid>::comparison_procedure(
                    bioseq::postgres::support::scan_comparison::equal),
            ObjectIdGetDatum(fragment_relation_oid));
        // Scan the checked catalog heap so a missing or damaged secondary index
        // cannot leave source versions ready after their fragments are dropped.
        SysScanDesc scan = systable_beginscan(relation, InvalidOid, false, SnapshotSelf, 1, &key);
        HeapTuple tuple = nullptr;
        while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
            const sequence_source_version_row current =
                source_version_detail::deform(tuple, RelationGetDescr(relation));
            if (current.state == k_sequence_source_state_invalid)
                continue;
            sequence_source_version_codec::update update{};
            update.set<&sequence_source_version_catalog_row::state>(
                k_sequence_source_state_invalid);
            HeapTuple updated = update.modify(tuple, RelationGetDescr(relation));
            CatalogTupleUpdate(relation, &tuple->t_self, updated);
            heap_freetuple(updated);
        }
        systable_endscan(scan);
        table_close(relation, RowExclusiveLock);
        CommandCounterIncrement();
    }
};

/// Storage manager allowed to create pending source tokens.
/// @tparam p_length_type Sequence length type.
/// @tparam p_bucket_minimum_length_bits Smallest bucket exponent.
/// @tparam p_bucket_maximum_length_bits Largest bucket exponent.
template <class p_length_type,
          std::size_t p_bucket_minimum_length_bits,
          std::size_t p_bucket_maximum_length_bits>
struct sequence_storage_manager;
/// Streaming writer allowed to create pending source tokens.
struct sequence_stream_writer;

/// A movable token for a stored sequence whose parent row is still being inserted.
/// After inserting that row, consume the token to publish the source. If the
/// insert is rejected, consume it to invalidate the source instead.
class pending_sequence_source {
  private:
    sequence_locator locator_{};
    bool pending_ = false;

    /// Create a pending token for a newly stored locator.
    /// @param locator Locator containing the unpublished source epoch.
    explicit pending_sequence_source(sequence_locator locator) noexcept
        : locator_(locator), pending_(locator.source_epoch != 0) {}

    template <class p_length_type,
              std::size_t p_bucket_minimum_length_bits,
              std::size_t p_bucket_maximum_length_bits>
    friend struct sequence_storage_manager;
    friend struct sequence_stream_writer;

  public:
    pending_sequence_source() = delete;
    pending_sequence_source(const pending_sequence_source&) = delete;
    pending_sequence_source& operator=(const pending_sequence_source&) = delete;
    /// Transfer the pending token and disable the source token.
    /// @param other Token to move from.
    pending_sequence_source(pending_sequence_source&& other) noexcept
        : locator_(other.locator_), pending_(std::exchange(other.pending_, false)) {}
    pending_sequence_source& operator=(pending_sequence_source&&) = delete;
    ~pending_sequence_source() = default;

    /// @return Locator reference valid for this token's lifetime.
    [[nodiscard]] const sequence_locator& locator() const noexcept {
        return locator_;
    }
    /// @return Whether this token can still publish or invalidate its source.
    [[nodiscard]] bool pending() const noexcept {
        return pending_;
    }

    /// Publish the source after the caller has inserted its parent row.
    /// @throws A PostgreSQL error if the token was consumed or publication fails.
    void publish_after_parent_row() {
        if (!pending_) {
            storage_error_as_cpp(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                                 "bioseq sequence source catalog: publication token was reused");
        }
        sequence_source_version_manager::publish_ready(locator_.source_epoch);
        pending_ = false;
    }

    /// Invalidate a source whose attempted parent-row insert was rejected.
    /// Consume the token so the rejected source cannot later be published.
    /// @throws A PostgreSQL error if the token was consumed or invalidation fails.
    void invalidate_without_parent_row() {
        if (!pending_) {
            storage_error_as_cpp(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                                 "bioseq sequence source catalog: invalidation token was reused");
        }
        sequence_source_version_manager::invalidate_epoch(locator_.source_epoch);
        pending_ = false;
    }
};

static_assert(!std::is_move_assignable_v<pending_sequence_source>,
              "A pending source authority must never be overwritten by move assignment.");



} // namespace bioseq::storage
