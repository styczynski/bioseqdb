/**
 * @file publication.cppm
 * @brief Sequence-source epoch allocation and publication authority.
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

export import :sequence_storage_catalog_generation;

export namespace bioseq::storage {
class pending_sequence_source;

/// PostgreSQL catalog operations for immutable source epochs. Every lookup
/// accepts the caller's snapshot. No backend-global or SnapshotSelf read is
/// allowed in a validated reader.
struct sequence_source_version_manager {
    /// Allocate the next positive immutable source epoch.
    /// @return Newly allocated epoch in the positive signed 64-bit domain.
    /// @throws PostgreSQL ERROR when the owned sequence contract is absent,
    /// altered, or exhausted.
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

    /// Insert one validated source identity in unpublished state.
    /// @param input Complete source row whose locator and epoch agree.
    /// @throws PostgreSQL ERROR when validation, catalog insertion, or cleanup fails.
    ///
    /// The catalog is held with `RowExclusiveLock` only for this insertion.
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
        // Run each release step while retaining the first insertion failure.
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

    /// Look up one source epoch through the exact primary index and snapshot.
    /// @param epoch Positive source epoch in the signed 64-bit catalog domain.
    /// @param snapshot Explicit caller-owned snapshot used by the ordered scan.
    /// @param require_ready Whether non-ready rows are filtered from the result.
    /// @param exact_primary_index_scan Optional proof flag set after a completed scan.
    /// @param telemetry Optional counters updated for relation and lock operations.
    /// @return Decoded row, or no value for invalid input, absence, or a filtered state.
    /// @throws PostgreSQL ERROR when the bound catalog or primary-index contract differs.
    ///
    /// The snapshot is borrowed only for the scan. Both relations and their
    /// `AccessShareLock` locks are released before return.
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
    /// Apply one allowed terminal source-state transition.
    /// @param epoch Existing positive source epoch.
    /// @param requested_state Ready or terminal-invalid state.
    /// @throws PostgreSQL ERROR when the row is absent, the transition is
    /// forbidden, or catalog update and cleanup fail.
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
        // Run each release step while retaining the first transition failure.
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

    /// Publish an unpublished source epoch as ready.
    /// @param epoch Existing unpublished source epoch.
    /// @throws PostgreSQL ERROR when the terminal transition is not allowed.
    static void publish_ready(std::uint64_t epoch) {
        set_state(epoch, k_sequence_source_state_ready);
    }

    friend class pending_sequence_source;

  public:
    /// Irreversibly invalidate one existing source epoch.
    /// @param epoch Source epoch, with zero treated as no work.
    /// @throws PostgreSQL ERROR when lookup or state transition fails.
    static void invalidate_epoch(std::uint64_t epoch) {
        if (epoch == 0)
            return;
        const auto row = lookup(epoch, SnapshotSelf, /*require_ready=*/false);
        if (!row || row->state == k_sequence_source_state_invalid)
            return;
        set_state(epoch, k_sequence_source_state_invalid);
    }

    /// Delete source-catalog rows owned by one main physical relation.
    /// @param database_oid Database component of the main-relation identity.
    /// @param tablespace_oid Tablespace component of the main-relation identity.
    /// @param main_relfilenode File component of the main-relation identity.
    /// @param allow_missing_during_extension_teardown Whether catalog absence is benign.
    /// @throws PostgreSQL ERROR for absence outside teardown or an invalid catalog.
    ///
    /// Matching `SnapshotSelf` tuples are deleted while holding `RowExclusiveLock`.
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

    /// Invalidate every source epoch referencing one fragment relation.
    /// @param fragment_relation_oid Fragment relation to invalidate, or invalid for no work.
    /// @param allow_missing_during_extension_teardown Whether catalog absence is benign.
    /// @throws PostgreSQL ERROR for absence outside teardown or an invalid catalog.
    ///
    /// The heap scan and updates use `SnapshotSelf` under `RowExclusiveLock`.
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
        // DROP/invalidation is not a timed path. A validated heap scan avoids
        // trusting a missing, renamed, partial, or otherwise corrupted
        // secondary index to decide whether ready epochs survive a drop.
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

/// Forward declaration of the manager allowed to mint publication authority.
/// @tparam p_length_type Unsigned type used for configured sequence lengths.
/// @tparam p_bucket_minimum_length_bits Smallest configured bucket exponent.
/// @tparam p_bucket_maximum_length_bits Largest configured bucket exponent.
template <class p_length_type,
          std::size_t p_bucket_minimum_length_bits,
          std::size_t p_bucket_maximum_length_bits>
struct sequence_storage_manager;
/// Forward declaration of the incremental writer allowed to mint authority.
struct sequence_stream_writer;

/// Move-only publication authority. Fragment/catalog creation returns this
/// token in unpublished state. Only the caller that has appended the parent
/// row may consume it to publish the ready state.
class pending_sequence_source {
  private:
    sequence_locator locator_{};
    bool pending_ = false;

    /// Capture sole publication authority for one newly persisted locator.
    /// @param locator Locator carrying the unpublished source epoch.
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
    /// Transfer sole source-publication authority.
    /// @param other Token made non-pending by the transfer.
    pending_sequence_source(pending_sequence_source&& other) noexcept
        : locator_(other.locator_), pending_(std::exchange(other.pending_, false)) {}
    pending_sequence_source& operator=(pending_sequence_source&&) = delete;
    ~pending_sequence_source() = default;

    /// @return Locator reference valid for this token's lifetime.
    [[nodiscard]] const sequence_locator& locator() const noexcept {
        return locator_;
    }
    /// @return Whether this token still owns an unpublished transition.
    [[nodiscard]] bool pending() const noexcept {
        return pending_;
    }

    /// Publish the source only after its parent row is visible to the writer.
    /// @throws PostgreSQL ERROR when the token was consumed or publication fails.
    void publish_after_parent_row() {
        if (!pending_) {
            storage_error_as_cpp(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                                 "bioseq sequence source catalog: publication token was reused");
        }
        sequence_source_version_manager::publish_ready(locator_.source_epoch);
        pending_ = false;
    }

    /// Consume a speculative-insert authority whose parent row was rejected.
    /// This is intentionally token-owned: the losing path cannot retain a raw
    /// epoch and later manufacture ready-state publication authority.
    /// @throws PostgreSQL ERROR when the token was consumed or invalidation fails.
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

// ---------------------------------------------------------------------------

} // namespace bioseq::storage
