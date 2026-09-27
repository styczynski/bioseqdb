/**
 * @file handles.cppm
 * @brief Creates PostgreSQL index handles and resolves active-read policy.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/genam.h"
#include "access/htup.h"
#include "access/sdir.h"
#include "access/skey.h"
#include "access/table.h"
#include "access/tupdesc.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "nodes/pg_list.h"
#include "storage/lockdefs.h"
#include "utils/errcodes.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "varatt.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

export module bioseq.postgres.storage.indexing.handles;

export import bioseq.postgres.storage.indexing.active_read_policy;
export import bioseq.postgres.storage.indexing.active_read_policy_resolution_contract;
export import bioseq.postgres.storage.indexing.active_read_policy_resolver;
export import bioseq.postgres.storage.indexing.external_file_producer_contract;
export import bioseq.postgres.storage.indexing.fragment_handle;
export import bioseq.postgres.storage.indexing.index_relation_identity;
export import bioseq.postgres.storage.indexing.minimizer_persistence_policy;
import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.manager;
import bioseq.storage.layout;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.indexing.chunked_storage;

export namespace bioseq::storage::indexing {

using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::k_directory_sub_key;
using bioseq::indexing::sequence_index_layout;
/// @brief Row codec for the extension-owned index policy catalog.
using index_policy_codec = bioseq::postgres::storage::indexing::sql_row_codecs::index_policy_codec;
/// @brief Decoded representation of one persisted index policy row.
using index_policy_row = index_policy_codec::row_type;

/**
 * @brief Opens a sequence-scoped index storage handle for one locator.
 *
 * @param[in] user_relid User relation whose index metadata is addressed.
 * @param[in] db Database OID in the physical storage identity.
 * @param[in] spc Tablespace OID in the physical storage identity.
 * @param[in] relfn Main relation relfilenode in the storage identity.
 * @param[in] attno One-based PostgreSQL column number.
 * @param[in] index_name Stable index policy name borrowed for the handle lifetime.
 * @param[in] loc Locator that selects the sequence bucket and shard.
 * @param[in] sequence_rowno Logical sequence row number used as the query key.
 * @param[in] caches Borrowed relation cache state, which must outlive the handle.
 * @param[in] mctx Parent for the handle-owned child memory context.
 * @return A handle that requires checked close on success or explicit
 * abandonment after a PostgreSQL error.
 */
[[nodiscard]] inline sequence_index_storage open_sequence_storage(Oid user_relid,
                                                                  Oid db,
                                                                  Oid spc,
                                                                  Oid relfn,
                                                                  std::int16_t attno,
                                                                  std::string_view index_name,
                                                                  const sequence_locator& loc,
                                                                  std::uint64_t sequence_rowno,
                                                                  seq_caches* caches,
                                                                  MemoryContext mctx) {
    return open_sequence_storage_handle(
        user_relid, db, spc, relfn, attno, index_name, loc, sequence_rowno, caches, mctx);
}

/**
 * @brief Opens a table-scoped index storage handle.
 *
 * @param[in] user_relid User relation whose index metadata is addressed.
 * @param[in] db Database OID in the physical storage identity.
 * @param[in] spc Tablespace OID in the physical storage identity.
 * @param[in] relfn Main relation relfilenode in the storage identity.
 * @param[in] attno One-based PostgreSQL column number.
 * @param[in] index_name Stable index policy name borrowed for the handle lifetime.
 * @param[in] caches Borrowed relation cache state, which must outlive the handle.
 * @param[in] mctx Parent for the handle-owned child memory context.
 * @return A handle that requires checked close on success or explicit
 * abandonment after a PostgreSQL error.
 */
[[nodiscard]] inline table_index_storage open_table_storage(Oid user_relid,
                                                            Oid db,
                                                            Oid spc,
                                                            Oid relfn,
                                                            std::int16_t attno,
                                                            std::string_view index_name,
                                                            seq_caches* caches,
                                                            MemoryContext mctx) {
    return open_table_storage_handle(
        user_relid, db, spc, relfn, attno, index_name, caches, mctx);
}

/**
 * @brief Raises an internal PostgreSQL error for an invalid policy state.
 * @param[in] message Primary diagnostic text.
 * @param[in] detail Optional diagnostic detail.
 */
[[noreturn]] inline void active_read_policy_error_as_cpp(std::string_view message,
                                                         std::string_view detail = {}) {
    bioseq::postgres::support::invoke_pg_as_cpp([message, detail] {
        if (detail.empty()) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq active-read policy: %.*s",
                            static_cast<int>(message.size()),
                            message.data())));
        }
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq active-read policy: %.*s",
                        static_cast<int>(message.size()),
                        message.data()),
                 errdetail("%.*s", static_cast<int>(detail.size()), detail.data())));
    });
    pg_unreachable();
}

/**
 * @brief Resolves persisted minimizer policy under a registered MVCC snapshot.
 *
 * The extension catalog and its primary index are held under `AccessShareLock`
 * for the bounded scan and are closed before return. Missing minimizer rows use
 * their declared defaults.
 *
 * @param[in] user_relid User relation named by the policy rows.
 * @param[in] relfilenode Current main-relation relfilenode.
 * @param[in] attno One-based PostgreSQL column number.
 * @param[in] registered_snapshot Registered MVCC snapshot used for the scan.
 * @return Effective enablement and insert-maintenance flags for each minimizer index.
 */
[[nodiscard]] inline minimizer_persistence_policy_resolution
resolve_minimizer_persistence_policy_implementation(Oid user_relid,
                                                    Oid relfilenode,
                                                    std::int16_t attno,
                                                    Snapshot registered_snapshot) {
    if (!OidIsValid(user_relid) || !OidIsValid(relfilenode) || attno <= 0 ||
        registered_snapshot == nullptr || !IsMVCCSnapshot(registered_snapshot) ||
        registered_snapshot->regd_count == 0) {
        active_read_policy_error_as_cpp(
            "deferred-persist resolver identity is invalid",
            fmt::format("relation={} relfilenode={} attno={} snapshot={}",
                        static_cast<unsigned>(user_relid),
                        static_cast<unsigned>(relfilenode),
                        static_cast<int>(attno),
                        registered_snapshot == nullptr ? "null" : "unregistered"));
    }

    constexpr auto minimizers = bioseq::indexing::k_minimizer_index_names;
    minimizer_persistence_policy_resolution result{};
    std::array<bool, minimizers.size()> seen{};
    for (std::size_t i = 0; i < minimizers.size(); ++i) {
        result.enabled[i] = true;
        result.update_on_insert[i] =
            bioseq::indexing::is_default_update_on_insert_index(minimizers[i]);
    }

    // Tracks PostgreSQL resources that require best-effort closure if the scan fails.
    struct policy_scan_state {
        Relation volatile relation = nullptr;
        Relation volatile primary = nullptr;
        SysScanDesc volatile scan = nullptr;
        MemoryContext volatile row_context = nullptr;
    } ledger{};
    std::array<ScanKeyData, 3> keys{
        index_policy_codec::scan_key<&index_policy_row::user_relid>(user_relid),
        index_policy_codec::scan_key<&index_policy_row::main_relfilenode>(relfilenode),
        index_policy_codec::scan_key<&index_policy_row::column_attno>(attno),
    };
    try {
        bioseq::postgres::support::invoke_pg_as_cpp([user_relid,
                                                     relfilenode,
                                                     attno,
                                                     registered_snapshot,
                                                     &result,
                                                     &seen,
                                                     &minimizers,
                                                     &ledger,
                                                     &keys] {
            const Oid namespace_oid = get_namespace_oid("bioseq_internal", false);
            const Oid policy_oid = get_relname_relid("index_policy", namespace_oid);
            if (!OidIsValid(policy_oid)) {
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_TABLE),
                         errmsg("bioseq deferred-persist policy catalog is missing")));
            }
            if (check_enable_rls(policy_oid, InvalidOid, true) == RLS_ENABLED) {
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("row security on bioseq deferred-persist policy is unsupported")));
            }

            ledger.relation = table_open(policy_oid, AccessShareLock);
            TupleDesc tuple_desc = RelationGetDescr(ledger.relation);
            if (ledger.relation->rd_rel->relam != HEAP_TABLE_AM_OID) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq deferred-persist policy catalog shape is invalid")));
            }
            index_policy_codec::validate_descriptor(tuple_desc);
            ledger.row_context = AllocSetContextCreate(
                CurrentMemoryContext, "bioseq_index_policy_row", ALLOCSET_SMALL_SIZES);

            const Oid primary_oid = get_relname_relid("index_policy_pkey", namespace_oid);
            if (!OidIsValid(primary_oid)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq deferred-persist policy primary index is missing")));
            }
            ledger.primary = index_open(primary_oid, AccessShareLock);
            const Form_pg_index primary_form = ledger.primary->rd_index;
            const bool primary_valid =
                primary_form != nullptr && ledger.primary->rd_rel->relam == BTREE_AM_OID &&
                primary_form->indrelid == policy_oid &&
                bioseq::postgres::support::relation_immediate_primary_key_index(ledger.relation) ==
                    primary_oid &&
                primary_form->indisvalid && primary_form->indisready && primary_form->indislive &&
                primary_form->indimmediate && primary_form->indisunique &&
                primary_form->indisprimary && primary_form->indnatts == 4 &&
                primary_form->indnkeyatts == 4 && primary_form->indkey.values[0] == 1 &&
                primary_form->indkey.values[1] == 2 && primary_form->indkey.values[2] == 3 &&
                primary_form->indkey.values[3] == 4 &&
                RelationGetIndexExpressions(ledger.primary) == NIL &&
                RelationGetIndexPredicate(ledger.primary) == NIL;
            if (!primary_valid) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq deferred-persist policy primary index is invalid")));
            }

            ledger.scan = systable_beginscan_ordered(ledger.relation,
                                                     ledger.primary,
                                                     registered_snapshot,
                                                     static_cast<int>(keys.size()),
                                                     keys.data());

            constexpr std::uint64_t max_policy_rows = 64;
            std::uint64_t row_count = 0;
            HeapTuple tuple = nullptr;
            while (HeapTupleIsValid(
                tuple = systable_getnext_ordered(ledger.scan, ForwardScanDirection))) {
                if (++row_count > max_policy_rows) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq deferred-persist policy row bound exceeded")));
                }
                MemoryContextReset(ledger.row_context);
                const index_policy_row row =
                    index_policy_codec::deform(tuple, tuple_desc, ledger.row_context);
                if (row.user_relid != user_relid || row.main_relfilenode != relfilenode ||
                    row.column_attno != attno) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq deferred-persist policy index returned a foreign row")));
                }

                text* name = row.index_name;
                const std::size_t name_size = static_cast<std::size_t>(VARSIZE_ANY_EXHDR(name));
                for (std::size_t i = 0; i < minimizers.size(); ++i) {
                    if (name_size == minimizers[i].size() &&
                        std::memcmp(VARDATA_ANY(name), minimizers[i].data(), name_size) == 0) {
                        if (seen[i]) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_DATA_CORRUPTED),
                                     errmsg("bioseq deferred-persist policy contains a duplicate "
                                            "minimizer row")));
                        }
                        seen[i] = true;
                        result.enabled[i] = row.enabled;
                        result.update_on_insert[i] = row.update_on_insert;
                        break;
                    }
                }
            }

            systable_endscan_ordered(ledger.scan);
            ledger.scan = nullptr;
            index_close(ledger.primary, AccessShareLock);
            ledger.primary = nullptr;
            table_close(ledger.relation, AccessShareLock);
            ledger.relation = nullptr;
            MemoryContextDelete(ledger.row_context);
            ledger.row_context = nullptr;
        });
    } catch (...) {
        if (ledger.scan != nullptr) {
            SysScanDesc scan = ledger.scan;
            ledger.scan = nullptr;
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [scan] { systable_endscan_ordered(scan); });
            } catch (...) {
            }
        }
        if (ledger.primary != nullptr) {
            Relation primary = ledger.primary;
            ledger.primary = nullptr;
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [primary] { index_close(primary, AccessShareLock); });
            } catch (...) {
            }
        }
        if (ledger.relation != nullptr) {
            Relation relation = ledger.relation;
            ledger.relation = nullptr;
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation] { table_close(relation, AccessShareLock); });
            } catch (...) {
            }
        }
        if (ledger.row_context != nullptr) {
            MemoryContext row_context = ledger.row_context;
            ledger.row_context = nullptr;
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [row_context] { MemoryContextDelete(row_context); });
            } catch (...) {
            }
        }
        throw;
    }
    return result;
}

} // namespace bioseq::storage::indexing
