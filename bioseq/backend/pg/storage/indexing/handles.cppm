/**
 * @file handles.cppm
 * @brief Creates PostgreSQL index handles and resolves active-read policy.
 */
module;
#include <bioseq/pg_compat.h>
#include "access/genam.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "access/relation.h"
#include "utils/memutils.h"
extern "C" {
#include "utils/rls.h"
}
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

export module bioseq.postgres.storage.indexing.handles;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.manager;
import bioseq.storage.layout;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.indexing.chunked_storage;

export namespace bioseq::storage::indexing {

using bioseq::indexing::sequence_index_layout;
using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::k_directory_sub_key;
/// @brief Row codec for the extension-owned index policy catalog.
using index_policy_codec =
    bioseq::postgres::storage::indexing::sql_row_codecs::
        index_policy_codec;
/// @brief Decoded representation of one persisted index policy row.
using index_policy_row = index_policy_codec::row_type;

/// @brief Physical identity used to bind index storage to a PostgreSQL relation.
struct relation_identity { Oid db_oid, tablespace_oid, relfilenode; };

/**
 * @brief Reads the current physical identity of a user relation.
 *
 * The relation is held under `AccessShareLock` only while its tablespace and
 * relfilenode are copied. Invalid physical fields are returned when the
 * relation cannot be opened.
 *
 * @param[in] user_relid OID of the relation to inspect.
 * @return Database, tablespace, and relfilenode identity for the relation.
 */
[[nodiscard]] inline relation_identity index_relation_identity(Oid user_relid) {
    relation_identity id{ MyDatabaseId, InvalidOid, InvalidOid };
    Relation r = bioseq::postgres::support::invoke_pg_as_cpp(
        [user_relid] {
            return bioseq::postgres::support::try_open_postgres_relation(user_relid, AccessShareLock);
        });
    if (r != nullptr) {
        id.tablespace_oid = OidIsValid(r->rd_rel->reltablespace) ? r->rd_rel->reltablespace : MyDatabaseTableSpace;
        id.relfilenode = static_cast<Oid>(r->rd_locator.relNumber);
        if (!OidIsValid(id.relfilenode)) id.relfilenode = RelationGetRelid(r);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [r] { relation_close(r, AccessShareLock); });
    }
    return id;
}

/**
 * @brief Creates the child memory context owned by an index handle.
 *
 * @param[in] parent Parent context, or null to use the current context.
 * @return A newly allocated PostgreSQL memory context that the handle must
 * close or abandon before destruction.
 */
[[nodiscard]] inline MemoryContext make_handle_ctx(MemoryContext parent, const char* /*tag*/) {
    const MemoryContext owning_parent = parent ? parent : CurrentMemoryContext;
    return bioseq::postgres::support::invoke_pg_as_cpp([owning_parent] {
        return AllocSetContextCreate(
            owning_parent, "bioseq_index_handle", ALLOCSET_SMALL_SIZES);
    });
}

/**
 * @brief Opens a fragment-scoped index storage handle for one locator.
 *
 * @param[in] user_relid User relation whose index metadata is addressed.
 * @param[in] db Database OID in the physical storage identity.
 * @param[in] spc Tablespace OID in the physical storage identity.
 * @param[in] relfn Main relation relfilenode in the storage identity.
 * @param[in] attno One-based PostgreSQL column number.
 * @param[in] index_name Stable index policy name borrowed for the handle lifetime.
 * @param[in] loc Locator that selects the fragment bucket and shard.
 * @param[in] sequence_rowno Logical sequence row number used as the query key.
 * @param[in] caches Borrowed relation cache state, which must outlive the handle.
 * @param[in] mctx Parent for the handle-owned child memory context.
 * @return A handle that requires checked close on success or explicit
 * abandonment after a PostgreSQL error.
 */
[[nodiscard]] inline fragment_index_storage open_fragment_storage(
        Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view index_name,
        const sequence_locator& loc, std::uint64_t sequence_rowno, seq_caches* caches, MemoryContext mctx) {
    // The returned handle owns an open child MemoryContext. Normal callers
    // must invoke close_checked(); failure paths must invoke
    // abandon_after_pg_error_noexcept() before destruction. The destructor is
    // deliberately zero-PostgreSQL and asserts this ownership transition.
    fragment_index_storage s;
    s.db_oid_=db; s.tablespace_oid_=spc; s.main_relfilenode_=relfn; s.user_relid_=user_relid;
    s.read_caches_=caches; s.column_attno_=attno; s.index_name_=index_name;
    s.bucket_idx_ = static_cast<std::uint8_t>(loc.bucket_index);
    s.shard_idx_ = loc.shard_index;
    s.query_key_ = sequence_rowno;
    s.own_mctx_ = make_handle_ctx(mctx, "idx_fragment_handle");
    s.run_buffer_override_bytes_ =
        bioseq::settings::index_build::run_buffer_bytes > 0
            ? static_cast<std::size_t>(bioseq::settings::index_build::run_buffer_bytes)
            : 0;
    return s;
}

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
[[nodiscard]] inline sequence_index_storage open_sequence_storage(
        Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view index_name,
        const sequence_locator& loc, std::uint64_t sequence_rowno, seq_caches* caches, MemoryContext mctx) {
    // Same explicit ownership contract as open_fragment_storage: checked
    // close on success, zero-PG abandonment before failure destruction.
    sequence_index_storage s;
    s.db_oid_=db; s.tablespace_oid_=spc; s.main_relfilenode_=relfn; s.user_relid_=user_relid;
    s.read_caches_=caches; s.column_attno_=attno; s.index_name_=index_name;
    s.bucket_idx_ = static_cast<std::uint8_t>(loc.bucket_index);
    s.shard_idx_ = loc.shard_index;
    s.query_key_ = sequence_rowno;
    s.own_mctx_ = make_handle_ctx(mctx, "idx_sequence_handle");
    return s;
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
[[nodiscard]] inline table_index_storage open_table_storage(
        Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view index_name,
        seq_caches* caches, MemoryContext mctx) {
    // Same explicit ownership contract as open_fragment_storage: checked
    // close on success, zero-PG abandonment before failure destruction.
    table_index_storage s;
    s.db_oid_=db; s.tablespace_oid_=spc; s.main_relfilenode_=relfn; s.user_relid_=user_relid;
    s.read_caches_=caches; s.column_attno_=attno; s.index_name_=index_name;
    s.own_mctx_ = make_handle_ctx(mctx, "idx_table_handle");
    return s;
}

/// @brief Origin of the active-read index selected for a relation column.
enum class active_read_policy_source : std::uint8_t {
	unresolved,
	non_relation,
    configured_row,
    authoritative_no_row_default,
};

/**
 * @brief Returns the stable diagnostic name for a policy source.
 * @param[in] source Policy source to name.
 * @return A static string view, or `invalid` for an unknown enumerator.
 */
[[nodiscard]] constexpr std::string_view
active_read_policy_resolution_source_name(
		const active_read_policy_source source) noexcept {
	switch (source) {
		case active_read_policy_source::unresolved:
			return "unresolved";
		case active_read_policy_source::non_relation:
			return "non_relation";
		case active_read_policy_source::configured_row:
			return "configured_row";
		case active_read_policy_source::authoritative_no_row_default:
			return "authoritative_no_row_default";
	}
	return "invalid";
}

static_assert(active_read_policy_resolution_source_name(
	active_read_policy_source::unresolved) == "unresolved");
static_assert(active_read_policy_resolution_source_name(
	active_read_policy_source::non_relation) == "non_relation");
static_assert(active_read_policy_resolution_source_name(
	active_read_policy_source::configured_row) == "configured_row");
static_assert(active_read_policy_resolution_source_name(
	active_read_policy_source::authoritative_no_row_default)
	== "authoritative_no_row_default");

/// @brief Selected active-read index and the authority that selected it.
struct active_read_policy_resolution {
    std::string index_name;
    active_read_policy_source source =
		active_read_policy_source::unresolved;
};

/// @brief Backend-local counts of active-read policy resolution outcomes.
struct active_read_policy_resolution_counters {
	std::uint64_t non_relation = 0;
    std::uint64_t configured_rows = 0;
    std::uint64_t authoritative_defaults = 0;
};

/// @brief Backend-local counts of batches dispatched by policy source.
struct active_read_policy_dispatch_counters {
	std::uint64_t configured_batches = 0;
	std::uint64_t authoritative_default_batches = 0;
	std::uint64_t non_relation_batches = 0;
};

/// @brief Entry counters for the compile-disabled external-file producer.
struct prohibited_external_file_producer_counters {
	std::uint64_t constructor_entries = 0;
	std::uint64_t writer_entries = 0;
};

// The external-file producer is compile-disabled in the typed dispatcher.
// These exported immutable counters are queryable by the integration fixture.
inline constexpr prohibited_external_file_producer_counters
	g_prohibited_external_file_producer_entry_counters{};
static_assert(
	g_prohibited_external_file_producer_entry_counters.constructor_entries == 0);
static_assert(
	g_prohibited_external_file_producer_entry_counters.writer_entries == 0);

// PostgreSQL backends are single-threaded. These counters intentionally show
// whether a default came from an authoritative empty policy result rather
// than treating an SPI failure as an empty result.
inline active_read_policy_resolution_counters
    g_active_read_policy_resolution_counters{};
inline active_read_policy_dispatch_counters
	g_active_read_policy_dispatch_stats{};
inline active_read_policy_source
	g_last_active_read_policy_dispatch_source =
		active_read_policy_source::unresolved;

/// @brief Clears backend-local policy diagnostics between integration checks.
inline void reset_active_read_policy_debug_state() noexcept {
	g_active_read_policy_resolution_counters = {};
	g_active_read_policy_dispatch_stats = {};
	g_last_active_read_policy_dispatch_source =
		active_read_policy_source::unresolved;
}

/**
 * @brief Raises an internal PostgreSQL error for an invalid policy state.
 * @param[in] message Primary diagnostic text.
 * @param[in] detail Optional diagnostic detail.
 */
[[noreturn]] inline void active_read_policy_error_as_cpp(
        std::string_view message,
        std::string_view detail = {}) {
    bioseq::postgres::support::invoke_pg_as_cpp([message, detail] {
        if (detail.empty()) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq active-read policy: %.*s",
                    static_cast<int>(message.size()), message.data())));
        }
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq active-read policy: %.*s",
                static_cast<int>(message.size()), message.data()),
             errdetail("%.*s", static_cast<int>(detail.size()),
                detail.data())));
    });
    pg_unreachable();
}

/// @brief Effective enablement and insert-maintenance policy for minimizer indexes.
struct minimizer_persistence_policy_resolution {
    std::array<bool, bioseq::indexing::k_minimizer_index_names.size()> enabled{};
    std::array<bool, bioseq::indexing::k_minimizer_index_names.size()>
        update_on_insert{};
};

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
resolve_minimizer_persistence_policy(
        Oid user_relid,
        Oid relfilenode,
        std::int16_t attno,
        Snapshot registered_snapshot) {
    if (!OidIsValid(user_relid) || !OidIsValid(relfilenode) || attno <= 0
        || registered_snapshot == nullptr
        || !IsMVCCSnapshot(registered_snapshot)
        || registered_snapshot->regd_count == 0) {
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
        index_policy_codec::scan_key<
            &index_policy_row::user_relid>(user_relid),
        index_policy_codec::scan_key<
            &index_policy_row::main_relfilenode>(relfilenode),
        index_policy_codec::scan_key<
            &index_policy_row::column_attno>(attno),
    };
    try {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [user_relid, relfilenode, attno, registered_snapshot,
             &result, &seen, &minimizers, &ledger, &keys] {
                const Oid namespace_oid =
                    get_namespace_oid("bioseq_internal", false);
                const Oid policy_oid = get_relname_relid(
                    "index_policy", namespace_oid);
                if (!OidIsValid(policy_oid)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_TABLE),
                         errmsg("bioseq deferred-persist policy catalog is missing")));
                }
                if (check_enable_rls(policy_oid, InvalidOid, true)
                    == RLS_ENABLED) {
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("row security on bioseq deferred-persist policy is unsupported")));
                }

                ledger.relation = table_open(policy_oid, AccessShareLock);
                TupleDesc tuple_desc = RelationGetDescr(ledger.relation);
                if (ledger.relation->rd_rel->relam
                    != HEAP_TABLE_AM_OID) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq deferred-persist policy catalog shape is invalid")));
                }
                index_policy_codec::validate_descriptor(tuple_desc);
                ledger.row_context = AllocSetContextCreate(
                    CurrentMemoryContext,
                    "bioseq_index_policy_row",
                    ALLOCSET_SMALL_SIZES);

                const Oid primary_oid = get_relname_relid(
                    "index_policy_pkey", namespace_oid);
                if (!OidIsValid(primary_oid)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq deferred-persist policy primary index is missing")));
                }
                ledger.primary = index_open(primary_oid, AccessShareLock);
                const Form_pg_index primary_form = ledger.primary->rd_index;
                const bool primary_valid = primary_form != nullptr
                    && ledger.primary->rd_rel->relam == BTREE_AM_OID
                    && primary_form->indrelid == policy_oid
                    && RelationGetPrimaryKeyIndex(ledger.relation)
                        == primary_oid
                    && primary_form->indisvalid && primary_form->indisready
                    && primary_form->indislive && primary_form->indimmediate
                    && primary_form->indisunique && primary_form->indisprimary
                    && primary_form->indnatts == 4
                    && primary_form->indnkeyatts == 4
                    && primary_form->indkey.values[0] == 1
                    && primary_form->indkey.values[1] == 2
                    && primary_form->indkey.values[2] == 3
                    && primary_form->indkey.values[3] == 4
                    && RelationGetIndexExpressions(ledger.primary) == NIL
                    && RelationGetIndexPredicate(ledger.primary) == NIL;
                if (!primary_valid) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq deferred-persist policy primary index is invalid")));
                }

                ledger.scan = systable_beginscan_ordered(
                    ledger.relation, ledger.primary, registered_snapshot,
                    static_cast<int>(keys.size()), keys.data());

                constexpr std::uint64_t max_policy_rows = 64;
                std::uint64_t row_count = 0;
                HeapTuple tuple = nullptr;
                while (HeapTupleIsValid(tuple = systable_getnext_ordered(
                        ledger.scan, ForwardScanDirection))) {
                    if (++row_count > max_policy_rows) {
                        ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq deferred-persist policy row bound exceeded")));
                    }
                    MemoryContextReset(ledger.row_context);
                    const index_policy_row row =
                        index_policy_codec::deform(
                            tuple, tuple_desc, ledger.row_context);
                    if (row.user_relid != user_relid
                        || row.main_relfilenode != relfilenode
                        || row.column_attno != attno) {
                        ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq deferred-persist policy index returned a foreign row")));
                    }

                    text* name = row.index_name;
                    const std::size_t name_size = static_cast<std::size_t>(
                        VARSIZE_ANY_EXHDR(name));
                    for (std::size_t i = 0; i < minimizers.size(); ++i) {
                        if (name_size == minimizers[i].size()
                            && std::memcmp(VARDATA_ANY(name),
                                minimizers[i].data(), name_size) == 0) {
                            if (seen[i]) {
                                ereport(ERROR,
                                    (errcode(ERRCODE_DATA_CORRUPTED),
                                     errmsg("bioseq deferred-persist policy contains a duplicate minimizer row")));
                            }
                            seen[i] = true;
                            result.enabled[i] = row.enabled;
                            result.update_on_insert[i] =
                                row.update_on_insert;
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

/**
 * @brief Resolves the active read index for an executor-owned relation column.
 *
 * The policy heap and primary index are read under `AccessShareLock` with the
 * executor's registered MVCC snapshot. A single enabled active row selects its
 * index, while an authoritative empty result selects the declared default.
 *
 * @param[in] executor_read Borrowed executor relation and snapshot context.
 * @param[in] attno One-based PostgreSQL column number.
 * @param[in,out] cold_stats Optional counters updated for catalog and lock work.
 * @return Selected index name and its policy source.
 */
[[nodiscard]] inline active_read_policy_resolution
resolve_active_read_index_outcome(
        const borrowed_executor_read_context& executor_read,
        std::int16_t attno,
        cold_index_preparation_stats* cold_stats = nullptr) {
		const Oid user_relid = executor_read.relation_oid();
		const Oid relfn = executor_read.relfilenode();
		Snapshot snapshot = executor_read.snapshot();
		if (!executor_read.valid() || !OidIsValid(user_relid)
			|| !OidIsValid(relfn) || attno <= 0) {
	        active_read_policy_error_as_cpp(
				"relation-backed resolver identity is invalid",
			fmt::format("relation={} relfilenode={} attno={}",
				static_cast<unsigned>(user_relid),
				static_cast<unsigned>(relfn), static_cast<int>(attno)));
	    }
		if (cold_stats != nullptr) {
			cold_stats->add(cold_stats->target_relation_reuses, 1);
		}
    // This resolver is on every physical alignment batch. Read our heap-AM
    // catalog directly instead of recursively invoking the SQL planner and
    // executor through SPI. Besides removing parse/plan/ExecutorStart latency,
    // the direct catalog scan is safe while the outer Bioseq CustomScan is
    // already inside ExecutorRun and retains the same authoritative relation.
    // Holds the bounded result copied out of the PostgreSQL catalog scan.
    struct policy_scan_result {
        std::uint64_t rows = 0;
        std::array<char, 256> index_name{};
    } scanned{};
    std::array<ScanKeyData, 3> keys{
        index_policy_codec::scan_key<
            &index_policy_row::user_relid>(user_relid),
        index_policy_codec::scan_key<
            &index_policy_row::main_relfilenode>(relfn),
        index_policy_codec::scan_key<
            &index_policy_row::column_attno>(attno),
    };
    bioseq::postgres::support::invoke_pg_as_cpp(
        [user_relid, relfn, attno, snapshot, &scanned, cold_stats,
         &keys] {
            const Oid namespace_oid =
                get_namespace_oid("bioseq_internal", false);
            const Oid policy_oid = get_relname_relid(
                "index_policy", namespace_oid);
            if (!OidIsValid(policy_oid)) {
                ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg("bioseq active-read policy catalog is missing")));
            }
            // index_policy is trusted extension metadata.  Installation
            // deliberately revokes ordinary users' access to
            // bioseq_internal; the user-facing relation is authorized by the
            // caller before reaching this resolver.  Requiring ACL_USAGE or
            // ACL_SELECT here would make every legitimate non-owner search
            // fail while adding no authorization boundary.
            if (check_enable_rls(policy_oid, InvalidOid, true)
                == RLS_ENABLED) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("row security on bioseq active-read policy is unsupported")));
            }
	            if (snapshot == nullptr || !IsMVCCSnapshot(snapshot)
	                || snapshot->regd_count == 0) {
	                ereport(ERROR,
	                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
	                     errmsg("bioseq active-read policy requires a registered executor snapshot")));
	            }
	            Relation relation = table_open(policy_oid, AccessShareLock);
	            if (cold_stats != nullptr) {
	                cold_stats->add(cold_stats->policy_heap_opens, 1);
	                cold_stats->add(cold_stats->lock_acquires, 1);
	            }
            TupleDesc tuple_desc = RelationGetDescr(relation);
            constexpr auto& attributes =
                index_policy_codec::expected_descriptor;
            if (relation->rd_rel->relam != HEAP_TABLE_AM_OID
                || tuple_desc->natts
                    != static_cast<int>(attributes.size())) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq active-read policy catalog shape is invalid")));
            }
            for (std::size_t i = 0; i < attributes.size(); ++i) {
                Form_pg_attribute attribute = TupleDescAttr(
                    tuple_desc, static_cast<int>(i));
                if (attribute->attisdropped
                    || attribute->atttypid != attributes[i].type_oid
                    || attribute->attnotnull
                        != !attributes[i].nullable
                    || std::string_view{NameStr(attribute->attname)}
                        != attributes[i].name) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq active-read policy catalog schema is invalid"),
                         errdetail("attribute=%zu", i + 1u)));
                }
            }
	            if (cold_stats != nullptr) {
	                cold_stats->add(
	                    cold_stats->policy_schema_validations, 1);
	            }
            const Oid primary_oid = get_relname_relid(
                "index_policy_pkey", namespace_oid);
            if (!OidIsValid(primary_oid)) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq active-read policy primary index is missing")));
            }
	            Relation primary = index_open(primary_oid, AccessShareLock);
	            if (cold_stats != nullptr) {
	                cold_stats->add(cold_stats->policy_index_opens, 1);
	                cold_stats->add(cold_stats->lock_acquires, 1);
	            }
            const Form_pg_index primary_form = primary->rd_index;
	            const bool primary_valid = primary_form != nullptr
	                && primary->rd_rel->relam == BTREE_AM_OID
	                && primary_form->indrelid == policy_oid
	                && RelationGetPrimaryKeyIndex(relation) == primary_oid
                && primary_form->indisvalid && primary_form->indisready
                && primary_form->indislive
                && primary_form->indimmediate
                && primary_form->indisunique && primary_form->indisprimary
                && primary_form->indnatts == 4
                && primary_form->indnkeyatts == 4
                && primary_form->indkey.values[0] == 1
                && primary_form->indkey.values[1] == 2
                && primary_form->indkey.values[2] == 3
                && primary_form->indkey.values[3] == 4
                && RelationGetIndexExpressions(primary) == NIL
                && RelationGetIndexPredicate(primary) == NIL;
	            if (!primary_valid) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq active-read policy primary index is invalid")));
            }
            SysScanDesc scan = systable_beginscan_ordered(
                relation, primary, snapshot,
                static_cast<int>(keys.size()), keys.data());
	            HeapTuple tuple = nullptr;
	            while (HeapTupleIsValid(tuple = systable_getnext_ordered(
	                    scan, ForwardScanDirection))) {
                bool user_null = false;
                bool relfn_null = false;
                bool attno_null = false;
                bool name_null = false;
                bool enabled_null = false;
                bool active_null = false;
                const Datum row_user = heap_getattr(
                    tuple,
                    index_policy_codec::attribute_number<
                        &index_policy_row::user_relid>(),
                    tuple_desc, &user_null);
                const Datum row_relfn = heap_getattr(
                    tuple,
                    index_policy_codec::attribute_number<
                        &index_policy_row::main_relfilenode>(),
                    tuple_desc, &relfn_null);
                const Datum row_attno = heap_getattr(
                    tuple,
                    index_policy_codec::attribute_number<
                        &index_policy_row::column_attno>(),
                    tuple_desc, &attno_null);
                const Datum row_name = heap_getattr(
                    tuple,
                    index_policy_codec::attribute_number<
                        &index_policy_row::index_name>(),
                    tuple_desc, &name_null);
                const Datum row_enabled = heap_getattr(
                    tuple,
                    index_policy_codec::attribute_number<
                        &index_policy_row::enabled>(),
                    tuple_desc, &enabled_null);
                const Datum row_active = heap_getattr(
                    tuple,
                    index_policy_codec::attribute_number<
                        &index_policy_row::is_active_read>(),
                    tuple_desc, &active_null);
                if (user_null || relfn_null || attno_null || name_null
                    || enabled_null || active_null) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq active-read policy row contains NULL")));
                }
                if (DatumGetObjectId(row_user) != user_relid
                    || DatumGetObjectId(row_relfn) != relfn
                    || DatumGetInt16(row_attno) != attno
                    || !DatumGetBool(row_enabled)
                    || !DatumGetBool(row_active)) {
                    continue;
                }
                ++scanned.rows;
                text* name = DatumGetTextPP(row_name);
                const bool free_name =
                    static_cast<void*>(name) != DatumGetPointer(row_name);
                const std::size_t name_size = static_cast<std::size_t>(
                    VARSIZE_ANY_EXHDR(name));
                if (name_size == 0u
                    || name_size >= scanned.index_name.size()) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq active-read policy index name is invalid")));
                }
                std::memcpy(
                    scanned.index_name.data(), VARDATA_ANY(name), name_size);
                scanned.index_name[name_size] = '\0';
                if (free_name) {
                    pfree(name);
                }
            }
	            systable_endscan_ordered(scan);
	            index_close(primary, AccessShareLock);
	            if (cold_stats != nullptr) {
	                cold_stats->add(cold_stats->lock_releases, 1);
	            }
	            table_close(relation, AccessShareLock);
	            if (cold_stats != nullptr) {
	                cold_stats->add(cold_stats->lock_releases, 1);
	            }
	        });
    const std::uint64_t rows = scanned.rows;
    std::optional<std::string> configured_index;
    std::optional<std::string> semantic_error;
    if (rows > 1) {
        semantic_error = fmt::format(
            "policy query returned {} rows", rows);
    } else if (rows == 1) {
        configured_index.emplace(scanned.index_name.data());
    }
    if (semantic_error.has_value()) {
        active_read_policy_error_as_cpp(
            "policy query result is invalid", *semantic_error);
    }
    if (configured_index.has_value()) {
        ++g_active_read_policy_resolution_counters.configured_rows;
        bioseq_log<logging_level::debug>(
            "[bioseq] active-read policy configured relation={} relfilenode={} attno={} index={}",
            static_cast<unsigned>(user_relid),
            static_cast<unsigned>(relfn), static_cast<int>(attno),
            *configured_index);
        return active_read_policy_resolution{
            .index_name = std::move(*configured_index),
            .source = active_read_policy_source::configured_row,
        };
    }
    ++g_active_read_policy_resolution_counters.authoritative_defaults;
    bioseq_log<logging_level::debug>(
        "[bioseq] active-read policy authoritative no-row default relation={} relfilenode={} attno={} index={}",
        static_cast<unsigned>(user_relid),
        static_cast<unsigned>(relfn), static_cast<int>(attno),
        bioseq::indexing::k_default_active_read_index);
    return active_read_policy_resolution{
        .index_name = std::string{
            bioseq::indexing::k_default_active_read_index},
        .source = active_read_policy_source::
            authoritative_no_row_default,
    };
}

}  // namespace bioseq::storage::indexing
