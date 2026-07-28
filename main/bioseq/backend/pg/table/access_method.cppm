/*-------------------------------------------------------------------------
 *
 * table/access_method.cppm
 *
 * Bioseq table access method (TAM) implementation. Provides the
 * TableAmRoutine entry points (beginscan/endscan/getnextslot, tuple
 * insert/update/delete, index_fetch_*, vacuum, analyze, etc.) that
 * PostgreSQL needs to treat a bioseq columnar relation like any other
 * table. Internally this routes the work to the columnar reader/
 * writer/metadata modules and the in-memory column cache.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <new>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>


#include <signal.h>
using sact = struct sigaction;

export module bioseq.table:access_method;

import bioseq.storage.physical;
import bioseq.common.mlogging;
import bioseq.storage.metadata;
import bioseq.storage_metadata.core;
import bioseq.storage.metadata.sql_rows;
import bioseq.vector.types;
import bioseq.table.types;
import bioseq.config;
import bioseq.table.io;
import bioseq.pg_utils;
import bioseq.codegen.sql.annotation_model;
import bioseq.storage.indexing_pg.admin;   // DOC0279 §11.2 — index_build_on_insert
import bioseq.storage.indexing_pg.chunked_storage;
import bioseq.storage_indexing_pg.chunked_storage;
import bioseq.storage.manager;
import bioseq.storage_manager.sequence_storage_manager;
import bioseq.storage.manager.sql_rows;
import bioseq.storage.scanner_pg;
import bioseq.table.am_cache;
import bioseq.types.pg_compact_sequence;
import bioseq.column.tam;   // DOC0322 §7.4 — tid_to_row_number / row_number_to_tid /
                            // IsBioseqColumnTableAmTable + register_bioseq_am_routine
import :access_identity;
import :access_common;
import :access_hooks;

/*
 * Timing parameters for truncate locking heuristics (file-local).
 *
 * These are the same values from src/backend/access/heap/vacuumlazy.c
 */
namespace {
constexpr int VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL = 50;   /* ms */
constexpr int VACUUM_TRUNCATE_LOCK_TIMEOUT = 4500;       /* ms */

struct PendingSpeculativeSources {
    SubTransactionId subtransaction_id = InvalidSubTransactionId;
    std::vector<bioseq::storage::PendingSequenceSource> sources;
};

using PendingSequenceSourceSlots =
    std::vector<std::optional<bioseq::storage::PendingSequenceSource>>;

static_assert(!std::is_trivially_destructible_v<MemoryContextScope>);
static_assert(!std::is_trivially_destructible_v<PendingSequenceSourceSlots>);
static_assert(std::is_nothrow_move_constructible_v<
    bioseq::storage::PendingSequenceSource>);

using PendingSpeculativeKey = std::pair<Oid, std::uint64_t>;
using PendingSpeculativeSourceMap =
    std::map<PendingSpeculativeKey, PendingSpeculativeSources>;

struct PendingSpeculativeTerminalQuarantine {
    std::uint64_t invariant_events = 0u;
    std::uint64_t quarantined_rows = 0u;
    std::uint64_t quarantined_sources = 0u;
    XactEvent last_event = XACT_EVENT_COMMIT;
    bool storage_exhausted = false;
    bool active_map_quarantined = false;
};

[[nodiscard]] auto& pending_speculative_sources() {
    static PendingSpeculativeSourceMap pending;
    return pending;
}

[[nodiscard]] auto& quarantined_speculative_sources_noexcept() noexcept {
    // A terminal transaction callback cannot allocate, destroy retained
    // source authority, or raise a PostgreSQL error.  Fixed map slots let the
    // callback quarantine an impossible survivor with an allocation-free,
    // noexcept swap while retaining the move-only authority for diagnosis.
    static std::array<PendingSpeculativeSourceMap, 4u> quarantined;
    return quarantined;
}

[[nodiscard]] auto& speculative_source_quarantine_state_noexcept() noexcept {
    static PendingSpeculativeTerminalQuarantine state;
    return state;
}

static_assert(noexcept(
    std::declval<PendingSpeculativeSourceMap&>().swap(
        std::declval<PendingSpeculativeSourceMap&>())));

void quarantine_pending_speculative_sources_noexcept(XactEvent event) noexcept {
    auto& pending = pending_speculative_sources();
    if (pending.empty()) return;

    std::uint64_t source_count = 0u;
    for (const auto& [key, entry] : pending) {
        (void)key;
        const auto entry_count =
            static_cast<std::uint64_t>(entry.sources.size());
        source_count = UINT64_MAX - source_count < entry_count
            ? UINT64_MAX
            : source_count + entry_count;
    }

    auto& state = speculative_source_quarantine_state_noexcept();
    if (state.invariant_events != UINT64_MAX) ++state.invariant_events;
    const auto pending_rows = static_cast<std::uint64_t>(pending.size());
    state.quarantined_rows =
        UINT64_MAX - state.quarantined_rows < pending_rows
            ? UINT64_MAX
            : state.quarantined_rows + pending_rows;
    state.quarantined_sources =
        UINT64_MAX - state.quarantined_sources < source_count
            ? UINT64_MAX
            : state.quarantined_sources + source_count;
    state.last_event = event;

    for (auto& quarantine : quarantined_speculative_sources_noexcept()) {
        if (quarantine.empty()) {
            quarantine.swap(pending);
            return;
        }
    }
    // Retain the authority in the active map if every fixed quarantine slot
    // is occupied.  This poisons later PRE_* validation rather than silently
    // destroying an authority after its parent transaction has committed.
    state.storage_exhausted = true;
    state.active_map_quarantined = true;
}

void require_no_pending_speculative_sources(const char* phase) {
    const auto& pending = pending_speculative_sources();
    if (pending.empty()) return;

    std::size_t source_count = 0;
    for (const auto& [key, entry] : pending) {
        (void)key;
        source_count += entry.sources.size();
    }
    ereport(ERROR,
        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
         errmsg("bioseq speculative source publication authority was not consumed"),
         errdetail("phase=%s speculative_rows=%zu pending_sources=%zu",
             phase, pending.size(), source_count)));
}
}

/*
 * DOC0322 §7.4 — tid_to_row_number / row_number_to_tid / IsBioseqColumnTableAmTable
 * now live in the bioseq_column_tam leaf (imported above). The former
 * `extern "C++"` forward-decls and the in-file BioseqColumnTableRowCount
 * forward-decl (now defined in storage_metadata:core, §7.4) are gone.
 *
 * DOC0322 §7.5 — bioseq_customscan_init is no longer called from here; the
 * call moved into bioseq_init so :access_method does not import :custom_scan.
 */

export TableScanDesc
bioseq_beginscan_extended(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelBioseqColumnScan parallelBioseqColumnScan,
							bool returnVectorizedTuple);

/* DOC0260 Phase A — same as bioseq_beginscan_extended, plus an explicit
 * `defer_seq_hydration` switch. Used by the custom scan / dispatch
 * buffer code paths that hydrate per-bucket later. */
TableScanDesc
bioseq_beginscan_with_options(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelBioseqColumnScan parallelBioseqColumnScan,
							bool returnVectorizedTuple,
							bool defer_seq_hydration);

/*
 * DOC0322 §7.2 — the BioseqColumnTableSetOptions_hook_type typedef used to live in
 * columnar.h; it is module-internal here now (its only user is the static hook
 * variable + the find_rendezvous_variable wiring in bioseq_tableam_init).
 * called when the user changes options on the given relation.
 */
typedef void (*BioseqColumnTableSetOptions_hook_type)(Oid relid, BioseqColumnOptions options);

/* available to other extensions using find_rendezvous_variable() */
static BioseqColumnTableSetOptions_hook_type BioseqColumnTableSetOptions_hook = NULL;

/* forward declaration for static functions */
static bool ConditionalLockRelationWithTimeout(Relation rel, LOCKMODE lockMode,
											   int timeout, int retryInterval,
											   bool acquire);
static void LogRelationStats(Relation rel, int elevel);
static void TruncateBioseqColumn(Relation rel, int elevel);
static bool TruncateAndCombineBioseqColumnStripes(Relation rel, int elevel);
static void BioseqColumnReportTotalVirtualBlocks(Relation relation, Snapshot snapshot,
											 int progressArrIndex);
static BlockNumber BioseqColumnGetNumberOfVirtualBlocks(Relation relation, Snapshot snapshot);
static ItemPointerData BioseqColumnGetHighestItemPointer(Relation relation,
													 Snapshot snapshot);
static double BioseqColumnReadRowsIntoIndex(TableScanDesc scan,
										Relation indexRelation,
										IndexInfo *indexInfo,
										bool progress,
										IndexBuildCallback indexCallback,
										void *indexCallbackState,
										EState *estate, ExprState *predicate,
										bioseq::storage::pg_sequence_source_provider*
											sourceProvider);
static void BioseqColumnReadMissingRowsIntoIndex(TableScanDesc scan, Relation indexRelation,
											 IndexInfo *indexInfo, EState *estate,
											 ExprState *predicate,
											 ValidateIndexState *state,
											 bioseq::storage::pg_sequence_source_provider*
												sourceProvider);
static ItemPointerData TupleSortSkipSmallerItemPointers(Tuplesortstate *tupleSort,
														ItemPointer targetItemPointer);

/*
 * File-static holder for the page-cache flag value captured when a
 * scan/DML callback first disables the cache. Not convertible to a
 * PageCacheDisableScope guard because the save and restore happen in
 * *different* TableAM callbacks (begin/end for scans; insert/complete
 * for speculative inserts), so the lifetime cannot be expressed as a
 * single C++ scope. The global slot means concurrent scans in the same
 * backend would overwrite each other's saved state -- currently the
 * executor serialises these callbacks per-backend so that is fine, but
 * adding a second slot would require threading the previous value
 * through the per-scan descriptor.
 */
static bool previousCacheEnabledState = false;

struct OrdinaryInsertPageCacheRestore {
	ErrorContextCallback callback{};
	bool previous_value{};
	bool armed{};
};

static_assert(std::is_trivially_copyable_v<OrdinaryInsertPageCacheRestore>);
static_assert(std::is_standard_layout_v<OrdinaryInsertPageCacheRestore>);

static void
restore_ordinary_insert_page_cache_on_error(void *argument) noexcept
{
	auto *restore = static_cast<OrdinaryInsertPageCacheRestore *>(argument);
	if (geterrlevel() < ERROR || restore == nullptr || !restore->armed)
	{
		return;
	}
	bioseq::settings::enable_column_cache = restore->previous_value;
	restore->armed = false;
}

static TableScanDesc
bioseq_beginscan(Relation relation, Snapshot snapshot,
				   int nkeys, ScanKey key,
				   ParallelTableScanDesc parallel_scan,
				   uint32_t flags)
{
	previousCacheEnabledState = bioseq::settings::enable_column_cache;

	int natts = relation->rd_att->natts;

	/* attr_needed represents 0-indexed attribute numbers */
	PBitmapset attr_needed = PBitmapset::full_range(natts);

	TableScanDesc scandesc = bioseq_beginscan_extended(relation, snapshot, nkeys, key,
														 parallel_scan,
														 flags, attr_needed, NULL,
														 NULL,
														 false);

	attr_needed.free();

	return scandesc;
}


export TableScanDesc
bioseq_beginscan_extended(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelBioseqColumnScan parallelBioseqColumnScan,
							bool returnVectorizedTuple)
{
	return bioseq_beginscan_with_options(relation, snapshot, nkeys, key,
		parallel_scan, flags, attr_needed, scanQual, parallelBioseqColumnScan,
		returnVectorizedTuple, /*defer_seq_hydration=*/false);
}

/*
 * DOC0260 §8/§9 — the custom scan's deferred-hydration entry point. Same
 * signature as bioseq_beginscan_extended but turns the deferred-hydration
 * flag on, so bioseq_getnextslot returns locator stubs verbatim and the
 * dispatch buffer hydrates them in per-bucket bulk. DOC0322 §7.5 — `export`ed
 * so :custom_scan reaches it via `import :access_method` instead of an
 * `extern "C++"` forward decl.
 */
export TableScanDesc
bioseq_beginscan_deferred(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelBioseqColumnScan parallelBioseqColumnScan,
							bool returnVectorizedTuple)
{
	return bioseq_beginscan_with_options(relation, snapshot, nkeys, key,
		parallel_scan, flags, attr_needed, scanQual, parallelBioseqColumnScan,
		returnVectorizedTuple, /*defer_seq_hydration=*/true);
}

export extern "C" TableScanDesc
bioseq_beginscan_extended_c(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelBioseqColumnScan parallelBioseqColumnScan,
							bool returnVectorizedTuple)
{
	return bioseq_beginscan_with_options(relation, snapshot, nkeys, key,
		parallel_scan, flags, attr_needed, scanQual, parallelBioseqColumnScan,
		returnVectorizedTuple, /*defer_seq_hydration=*/false);
}

export extern "C" TableScanDesc
bioseq_beginscan_deferred_c(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelBioseqColumnScan parallelBioseqColumnScan,
							bool returnVectorizedTuple)
{
	return bioseq_beginscan_with_options(relation, snapshot, nkeys, key,
		parallel_scan, flags, attr_needed, scanQual, parallelBioseqColumnScan,
		returnVectorizedTuple, /*defer_seq_hydration=*/true);
}

TableScanDesc
bioseq_beginscan_with_options(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelBioseqColumnScan parallelBioseqColumnScan,
							bool returnVectorizedTuple,
							bool defer_seq_hydration)
{
	previousCacheEnabledState = bioseq::settings::enable_column_cache;
	Oid relfilelocator = relation->rd_locator.relNumber;
	/*
	 * A memory context to use for scan-wide data, including the lazily
	 * initialized read state. We assume that beginscan is called in a
	 * context that will last until end of scan.
	 */
	MemoryContext scanContext = CreateBioseqColumnScanMemoryContext();
	BioseqColumnScanDesc scan;
	{
		MemoryContextScope ctx_guard{scanContext};

		scan = ::new (palloc0(sizeof(BioseqColumnScanDescData))) BioseqColumnScanDescData{};
		scan->cs_base.rs_rd = relation;
		scan->cs_base.rs_snapshot = snapshot;
		scan->cs_base.rs_nkeys = nkeys;
		scan->cs_base.rs_key = key;
		scan->cs_base.rs_flags = flags;
		scan->cs_base.rs_parallel = parallel_scan;

		/*
		 * We will initialize this lazily in first tuple, where we have the actual
		 * tuple descriptor to use for reading. In some cases like ALTER TABLE ...
		 * ALTER COLUMN ... TYPE, the tuple descriptor of relation doesn't match
		 * the storage which we are reading, so we need to use the tuple descriptor
		 * of "slot" in first read.
		 */
		scan->cs_readState = NULL;
		scan->attr_needed = bms_copy(attr_needed);
		scan->scanQual = copyObject(scanQual);
		scan->scanContext = scanContext;

		/* Parallel execution scan data */;
		scan->parallelBioseqColumnScan = parallelBioseqColumnScan;

		/* Vectorized result */
		scan->returnVectorizedTuple = returnVectorizedTuple;

		/* DOC0260 Phase A — per-scan storage cache and deferred-hydration flag. */
		scan->defer_seq_hydration = defer_seq_hydration;
		auto* caches = bioseq::storage::alloc_seq_caches(scanContext);
		caches->side = bioseq::storage::seq_caches::Side::Read;
		caches->lock_mode = AccessShareLock;
		caches->read_state_mctx = scanContext;
		caches->main_database_oid   = relation->rd_locator.dbOid;
		caches->main_tablespace_oid = relation->rd_locator.spcOid;
		caches->main_relfilenode    = relation->rd_locator.relNumber;
		scan->seq_cache = caches;

		if (PendingWritesInUpperTransactions(relfilelocator, GetCurrentSubTransactionId()))
		{
			bioseq_log<ERR, PG>(
				 "cannot read from table when there is unflushed data in upper transactions");
		}
	}

	return reinterpret_cast<TableScanDesc>(scan);
}

namespace {

class sql_relation_lock final {
public:
	sql_relation_lock(
			Relation relation,
			LOCKMODE close_lock,
			bool opened_as_table) noexcept
		: relation_{relation},
		  close_lock_{close_lock},
		  opened_as_table_{opened_as_table} {}

	sql_relation_lock(const sql_relation_lock&) = delete;
	sql_relation_lock& operator=(const sql_relation_lock&) = delete;

	~sql_relation_lock() noexcept {
		if (relation_ == nullptr) return;
		if (opened_as_table_) {
			table_close(relation_, close_lock_);
		} else {
			relation_close(relation_, close_lock_);
		}
	}

private:
	Relation relation_ = nullptr;
	LOCKMODE close_lock_ = NoLock;
	bool opened_as_table_ = false;
};

[[nodiscard]] std::string quoted_relation_name(Relation relation) {
	const char* const relation_name = RelationGetRelationName(relation);
	const char* const quoted = bioseq::pg_utils::invoke_pg_as_cpp(
		[relation_name] { return quote_identifier(relation_name); });
	return quoted == nullptr ? std::string{} : std::string{quoted};
}

void require_bioseq_table(
		Relation relation,
		Oid relation_id,
		const char* object_kind = "table") {
	const bool is_bioseq = bioseq::pg_utils::invoke_pg_as_cpp(
		[relation_id] {
			return IsBioseqColumnTableAmTable(relation_id);
		});
	if (is_bioseq) return;
	bioseq::pg_utils::raise_sql_error(
		ERRCODE_INTERNAL_ERROR,
		{
			.message = std::string{object_kind} + " "
				+ quoted_relation_name(relation)
				+ " is not a columnar table",
		});
}

void require_relation_owner(Oid relation_id) {
	const Oid user_id = GetUserId();
	const bool is_owner = bioseq::pg_utils::invoke_pg_as_cpp(
		[relation_id, user_id] {
			return object_ownercheck(
				RelationRelationId, relation_id, user_id);
		});
	if (is_owner) return;
	bioseq::pg_utils::invoke_pg_as_cpp(
		[relation_id] {
			aclcheck_error(
				ACLCHECK_NOT_OWNER,
				OBJECT_TABLE,
				get_rel_name(relation_id));
		});
}

}  // namespace

export namespace bioseq::table::sql_routines {

[[bioseq_sql::function({
	.schema = bioseq::sql::fixed_schema<"bioseq">,
	.create = bioseq::sql::create_mode::create,
})]]
std::int64_t get_storage_id(
		[[bioseq_sql::param({
			.sql_type = bioseq::sql::pg::regclass,
		})]]
		Oid relation_id) {
	Relation relation = bioseq::pg_utils::invoke_pg_as_cpp(
		[relation_id] {
			return safe_relation_open(relation_id, AccessShareLock);
		});
	const sql_relation_lock relation_lock{
		relation, AccessShareLock, false};
	require_bioseq_table(relation, relation_id, "relation");
	const std::uint64_t storage_id =
		bioseq::pg_utils::invoke_pg_as_cpp(
			[relation] {
				return BioseqColumnStorageGetStorageId(
					relation, false);
			});
	return static_cast<std::int64_t>(storage_id);
}

}  // namespace bioseq::table::sql_routines

/*
 * init_bioseq_read_state initializes a column store table read and returns the
 * state.
 */
static BioseqColumnReadState *
init_bioseq_read_state(Relation relation, TupleDesc tupdesc, Bitmapset *attr_needed,
						 List *scanQual, MemoryContext scanContext, Snapshot snapshot,
						 bool randomAccess,
						 ParallelBioseqColumnScan parallelBioseqColumnScan)
{
	MemoryContextScope ctx_guard{scanContext};

	List *neededColumnList = NeededColumnsList(tupdesc, attr_needed);
	BioseqColumnReadState *readState = BioseqColumnBeginRead(relation, tupdesc, neededColumnList,
													 scanQual, scanContext, snapshot,
													 randomAccess,
													 parallelBioseqColumnScan);

	return readState;
}


static void
bioseq_endscan(TableScanDesc sscan)
{
	BioseqColumnScanDesc scan = reinterpret_cast<BioseqColumnScanDesc>(sscan);

	/* DOC0260 Phase A — close per-scan seq_cache (releases relation pins,
	 * tears down any cached BioseqColumnReadStates on sub-tables). */
	if (scan->seq_cache != NULL) {
		auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->seq_cache);
		caches->close_all(/*commit=*/true);
		bioseq::storage::destroy_seq_caches(caches);
		scan->seq_cache = NULL;
	}

	if (scan->cs_readState != NULL)
	{
		BioseqColumnEndRead(scan->cs_readState);
		scan->cs_readState = NULL;
	}

	if (scan->cs_base.rs_flags & SO_TEMP_SNAPSHOT)
	{
		UnregisterSnapshot(scan->cs_base.rs_snapshot);
	}

	/* clean up any caches. */
	if (bioseq::settings::enable_column_cache == true)
	{
		BioseqColumnResetCache();
	}

	MemoryContextDelete(scan->scanContext);

	bioseq::settings::enable_column_cache = previousCacheEnabledState;
}


static void
bioseq_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
				bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	BioseqColumnScanDesc scan = reinterpret_cast<BioseqColumnScanDesc>(sscan);

	/* XXX: hack to pass in new quals that aren't actually scan keys */
	List *scanQual = reinterpret_cast<List *>(key);

	if (scan->cs_readState != NULL)
	{
		BioseqColumnRescan(scan->cs_readState, scanQual);
	}
}


static bool
bioseq_getnextslot_impl(
	TableScanDesc sscan,
	ScanDirection direction,
	TupleTableSlot *slot)
{
	BioseqColumnScanDesc scan = reinterpret_cast<BioseqColumnScanDesc>(sscan);

	/*
	 * if this is the first row, initialize read state.
	 */
	if (scan->cs_readState == NULL)
	{
		bool randomAccess = false;
		scan->cs_readState =
			init_bioseq_read_state(scan->cs_base.rs_rd, slot->tts_tupleDescriptor,
									 scan->attr_needed, scan->scanQual,
									 scan->scanContext, scan->cs_base.rs_snapshot,
									 randomAccess,
									 scan->parallelBioseqColumnScan);
	}

	ExecClearTuple(slot);

	if (scan->returnVectorizedTuple)
	{
		VectorTupleTableSlot * vectorTTS = reinterpret_cast<VectorTupleTableSlot *>(slot);

		int newVectorSize = 0;

		bool nextRowFound = BioseqColumnReadNextVector(scan->cs_readState,
												   vectorTTS->tts.tts_values,
												   vectorTTS->tts.tts_isnull,
												   vectorTTS->rowNumber,
												   &newVectorSize);

		if (!nextRowFound)
			return false;

		vectorTTS->dimension = newVectorSize;

		memset(vectorTTS->keep, true, newVectorSize);

		ExecStoreVirtualTuple(slot);
	}
	else
	{
		uint64_t rowNumber;
		bool nextRowFound = BioseqColumnReadNextRow(scan->cs_readState, slot->tts_values,
												slot->tts_isnull, &rowNumber);

		if (!nextRowFound)
		{
			return false;
		}

		ExecStoreVirtualTuple(slot);
		slot->tts_tid = row_number_to_tid(rowNumber);

		/*
		 * DOC0260 Phase A — hydrate every SEQ column unless the caller
		 * asked for deferred hydration (custom scan / dispatch buffer
		 * path, which performs per-bucket bulk hydration later).
		 */
		if (!scan->defer_seq_hydration)
		{
			const auto& amc = bioseq::storage::bioseq_am_cache_for(scan->cs_base.rs_rd);
			auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->seq_cache);
			hydrate_seq_columns_in_slot(scan->cs_base.rs_rd, slot, caches, amc,
			                            scan->cs_readState->snapshot,
			                            scan->scanContext);
		}
	}

	return true;
}


/*
 * DOC0322 §7.4 — row_number_to_tid / tid_to_row_number / ErrorIfInvalidRowNumber
 * moved to the bioseq_column_tam leaf module (imported above). Their definitions
 * are no longer here; callers in this TU reach the two tid helpers via that import.
 */


static Size
bioseq_parallelscan_estimate(Relation rel)
{
	bioseq_log<ERR, PG>( "bioseq_parallelscan_estimate not implemented");
}


static Size
bioseq_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	bioseq_log<ERR, PG>( "bioseq_parallelscan_initialize not implemented");
}


static void
bioseq_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	bioseq_log<ERR, PG>( "bioseq_parallelscan_reinitialize not implemented");
}


[[gnu::always_inline]]
static inline void
init_index_fetch_seq_cache(IndexFetchBioseqColumnData *scan, Relation rel,
                           MemoryContext scanContext)
{
	auto *caches = bioseq::storage::alloc_seq_caches(scanContext);
	caches->side = bioseq::storage::seq_caches::Side::Read;
	caches->lock_mode = AccessShareLock;
	caches->read_state_mctx = scanContext;
	caches->main_database_oid   = rel->rd_locator.dbOid;
	caches->main_tablespace_oid = rel->rd_locator.spcOid;
	caches->main_relfilenode    = rel->rd_locator.relNumber;
	scan->seq_cache = caches;
	scan->defer_seq_hydration = false;
}

static IndexFetchTableData *
bioseq_index_fetch_begin(Relation rel)
{
	Oid relfilelocator = rel->rd_locator.relNumber;
	if (PendingWritesInUpperTransactions(relfilelocator, GetCurrentSubTransactionId()))
	{
		/* XXX: maybe we can just flush the data and continue */
		bioseq_log<ERR, PG>( "cannot read from index when there is unflushed data in "
					"upper transactions");
	}

	MemoryContext scanContext = CreateBioseqColumnScanMemoryContext();
	IndexFetchBioseqColumnData *scan;
	{
		MemoryContextScope ctx_guard{scanContext};

		scan = ::new (palloc0(sizeof(IndexFetchBioseqColumnData))) IndexFetchBioseqColumnData{};
		scan->cs_base.rel = rel;
		scan->cs_readState = NULL;
		scan->stripeMetadataList = NIL;
		scan->scanContext = scanContext;
		scan->is_select_query = false;
		init_index_fetch_seq_cache(scan, rel, scanContext);
	}

	return &scan->cs_base;
}

export IndexFetchTableData *
bioseq_index_fetch_begin_extended(Relation rel, Bitmapset *attr_needed)
{
	Oid relfilenode = rel->rd_locator.relNumber;
	if (PendingWritesInUpperTransactions(relfilenode, GetCurrentSubTransactionId()))
	{
		/* XXX: maybe we can just flush the data and continue */
		bioseq_log<ERR, PG>( "cannot read from index when there is unflushed data in "
					"upper transactions");
	}

	MemoryContext scanContext = CreateBioseqColumnScanMemoryContext();
	IndexFetchBioseqColumnData *scan;
	{
		MemoryContextScope ctx_guard{scanContext};

		scan = ::new (palloc0(sizeof(IndexFetchBioseqColumnData))) IndexFetchBioseqColumnData{};
		scan->cs_base.rel = rel;
		scan->cs_readState = NULL;
		scan->stripeMetadataList = NIL;
		scan->scanContext = scanContext;

		scan->attr_needed = bms_copy(attr_needed);
		scan->is_select_query = true;
		init_index_fetch_seq_cache(scan, rel, scanContext);
	}

	return &scan->cs_base;
}


static void
bioseq_index_fetch_reset(IndexFetchTableData *sscan)
{
	/* no-op */
}


static void
bioseq_index_fetch_end(IndexFetchTableData *sscan)
{
	bioseq_index_fetch_reset(sscan);

	IndexFetchBioseqColumnData *scan = reinterpret_cast<IndexFetchBioseqColumnData *>(sscan);

	/* DOC0260 Phase A — close per-fetch seq_cache. */
	if (scan->seq_cache != NULL) {
		auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->seq_cache);
		caches->close_all(/*commit=*/true);
		bioseq::storage::destroy_seq_caches(caches);
		scan->seq_cache = NULL;
	}

	if (scan->cs_readState)
	{
		BioseqColumnEndRead(scan->cs_readState);
		scan->cs_readState = NULL;
	}

	scan->attr_needed.free();

	/* clean up any caches. */
	if (bioseq::settings::enable_column_cache == true)
	{
		BioseqColumnResetCache();
	}

	MemoryContextDelete(scan->scanContext);
}

static StripeMetadata *
FindStripeMetadataFromListBinarySearch(IndexFetchBioseqColumnData *scan, uint64_t rowNumber)
{
	ListCell *lc = NULL;

	int high = scan->stripeMetadataList.length() - 1;
	int low = 0;

	while(low <= high)
	{
		int mid = low + (high - low) / 2;

		lc = list_nth_cell(scan->stripeMetadataList.ptr(), mid);

		StripeMetadata *stripeMetadata = static_cast<StripeMetadata*>(lc->ptr_value);

		if (rowNumber >= stripeMetadata->firstRowNumber &&
			rowNumber < stripeMetadata->firstRowNumber + stripeMetadata->rowCount)
		{
			return stripeMetadata;
		}

		if (stripeMetadata->firstRowNumber > rowNumber)
		{
			high = mid - 1;
		}
		else
		{
			low = mid + 1;
		}
	}
	return NULL;
}


static bool
bioseq_index_fetch_tuple(IndexFetchTableData *sscan,
						   ItemPointer tid,
						   Snapshot snapshot,
						   TupleTableSlot *slot,
						   bool *call_again, bool *all_dead)
{
	/* no HOT chains are possible in columnar, directly set it to false */
	*call_again = false;

	/*
	 * Initialize all_dead to false if passed to be non-NULL.
	 *
	 * XXX: For aborted writes, we should set all_dead to true but this would
	 * require implementing bioseq_index_delete_tuples for simple deletion
	 * of dead tuples (TM_IndexDeleteOp.bottomup = false).
	 */
	if (all_dead)
	{
		*all_dead = false;
	}

	ExecClearTuple(slot);

	IndexFetchBioseqColumnData *scan = reinterpret_cast<IndexFetchBioseqColumnData *>(sscan);
	Relation bioseqColumnRelation = scan->cs_base.rel;

	/* initialize read state for the first row */
	if (scan->cs_readState == NULL)
	{
		/* no quals for index scan */
		List *scanQual = NIL;

		if (scan->attr_needed.empty())
		{
			/* we need all columns */
			int natts = bioseqColumnRelation->rd_att->natts;
			scan->attr_needed.free();
			scan->attr_needed = PBitmapset::full_range(natts);
		}

		bool randomAccess = true;
		scan->cs_readState = init_bioseq_read_state(bioseqColumnRelation,
													  slot->tts_tupleDescriptor,
													  scan->attr_needed, scanQual,
													  scan->scanContext,
													  snapshot, randomAccess,
													  NULL);
		if (scan->is_select_query)
			scan->stripeMetadataList =
				StripesForRelfilenode(bioseqColumnRelation->rd_locator, ForwardScanDirection);

	}

	uint64_t rowNumber = tid_to_row_number(*tid);

	StripeMetadata *stripeMetadata = NULL;
	
	if (scan->is_select_query)
		stripeMetadata = FindStripeMetadataFromListBinarySearch(scan, rowNumber);
	else
		stripeMetadata = FindStripeWithMatchingFirstRowNumber(bioseqColumnRelation, rowNumber, snapshot);
	
	if (!stripeMetadata)
	{
		/* it is certain that tuple with rowNumber doesn't exist */
		return false;
	}

	StripeWriteStateEnum stripeWriteState = StripeWriteState(stripeMetadata);

	if (stripeWriteState == STRIPE_WRITE_FLUSHED &&
		!BioseqColumnReadRowByRowNumber(scan->cs_readState, rowNumber,
									slot->tts_values, slot->tts_isnull))
	{
		/*
		 * FindStripeWithMatchingFirstRowNumber doesn't verify upper row
		 * number boundary of found stripe. For this reason, we didn't
		 * certainly know if given row number belongs to one of the stripes.
		 */
		return false;
	}
	else if (stripeWriteState == STRIPE_WRITE_ABORTED)
	{
		/*
		 * We only expect to see un-flushed stripes when checking against
		 * constraint violation. In that case, indexAM provides dirty
		 * snapshot to index_fetch_tuple callback.
		 */
		pfree(stripeMetadata);
		Assert(snapshot->snapshot_type == SNAPSHOT_DIRTY);
		return false;
	}
	else if (stripeWriteState == STRIPE_WRITE_IN_PROGRESS)
	{
		if (stripeMetadata->insertedByCurrentXact)
		{
			/*
			 * Stripe write is in progress and its entry is inserted by current
			 * transaction, so obviously it must be written by me. Since caller
			 * might want to use tupleslot datums for some reason, do another
			 * look-up, but this time by first flushing our writes.
			 *
			 * XXX: For index scan, this is the only case that we flush pending
			 * writes of the current backend. If we have taught reader how to
			 * read from WriteStateMap. then we could guarantee that
			 * index_fetch_tuple would never flush pending writes, but this seem
			 * to be too much work for now, but should be doable.
			 */
			BioseqColumnReadFlushPendingWrites(scan->cs_readState);

			/*
			 * Fill the tupleslot and fall through to return true, it
			 * certainly exists.
			 */
			BioseqColumnReadRowByRowNumberOrError(scan->cs_readState, rowNumber,
											  slot->tts_values, slot->tts_isnull);
		}
		else
		{
			/* similar to aborted writes, it should be dirty snapshot */
			Assert(snapshot->snapshot_type == SNAPSHOT_DIRTY);

			/*
			 * Stripe that "might" contain the tuple with rowNumber is not
			 * flushed yet. Here we set all attributes of given tupleslot to NULL
			 * before returning true and expect the indexAM callback that called
			 * us --possibly to check against constraint violation-- blocks until
			 * writer transaction commits or aborts, without requiring us to fill
			 * the tupleslot properly.
			 *
			 * XXX: Note that the assumption we made above for the tupleslot
			 * holds for "unique" constraints defined on "btree" indexes.
			 *
			 * For the other constraints that we support, namely:
			 * * exclusion on btree,
			 * * exclusion on hash,
			 * * unique on btree;
			 * we still need to fill tts_values.
			 *
			 * However, for the same reason, we should have already flushed
			 * single tuple stripes when inserting into table for those three
			 * classes of constraints.
			 *
			 * This is annoying, but this also explains why this hack works for
			 * unique constraints on btree indexes, and also explains how we
			 * would never end up with "else" condition otherwise.
			 */
			memset(slot->tts_isnull, true, slot->tts_nvalid * sizeof(bool));
		}
	}
	else
	{
		/*
		 * At this point, we certainly know that stripe is flushed and
		 * BioseqColumnReadRowByRowNumber successfully filled the tupleslot.
		 */
		Assert(stripeWriteState == STRIPE_WRITE_FLUSHED);
	}

	if (!scan->is_select_query)
		pfree(stripeMetadata);
	slot->tts_tableOid = RelationGetRelid(bioseqColumnRelation);
	slot->tts_tid = *tid;
	ExecStoreVirtualTuple(slot);

	/*
	 * DOC0260 Phase A — hydrate SEQ stub columns unless the caller asked
	 * for deferred hydration. For the index-fetch path we always hydrate
	 * because downstream operators (recheck quals, executor projection)
	 * read SEQ values directly.
	 */
	if (!scan->defer_seq_hydration)
	{
		const auto& amc = bioseq::storage::bioseq_am_cache_for(bioseqColumnRelation);
		auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->seq_cache);
		hydrate_seq_columns_in_slot(bioseqColumnRelation, slot, caches, amc,
		                            scan->cs_readState->snapshot,
		                            scan->scanContext);
	}

	return true;
}


static bool
bioseq_fetch_row_version(Relation relation,
						   ItemPointer tid,
						   Snapshot snapshot,
						   TupleTableSlot *slot)
{
	uint64_t rowNumber = tid_to_row_number(*tid);
	BioseqColumnReadState **readState =
		FindReadStateCache(relation, GetCurrentSubTransactionId());

	if (readState == NULL)
	{
		readState = InitBioseqColumnReadStateCache(relation, GetCurrentSubTransactionId());

		int natts = relation->rd_att->natts;
		PBitmapset attr_needed = PBitmapset::full_range(natts);

		List *scanQual = NIL;

		bool randomAccess = false;

		*readState = init_bioseq_read_state(relation,
											  slot->tts_tupleDescriptor,
											  attr_needed, scanQual,
											  GetBioseqColumnReadStateCache(),
											  snapshot, randomAccess,
											  NULL);
	}

	{
		MemoryContextScope ctx_guard{GetBioseqColumnReadStateCache()};
		BioseqColumnReadRowByRowNumber(*readState, rowNumber,
								   slot->tts_values, slot->tts_isnull);
	}

	slot->tts_tableOid = RelationGetRelid(relation);
	slot->tts_tid = *tid;

	if (TTS_EMPTY(slot))
		ExecStoreVirtualTuple(slot);

	/* DOC0260 Phase A — hydrate any SEQ stubs in the slot. We do NOT
	 * cache a per-fetch seq_caches here because the row-version fetch
	 * is one-shot; allocate, use, destroy. */
	{
		const auto& amc = bioseq::storage::bioseq_am_cache_for(relation);
		if (!amc.seq_attnos.empty()) {
			auto* caches = bioseq::storage::alloc_seq_caches(GetBioseqColumnReadStateCache());
			caches->side = bioseq::storage::seq_caches::Side::Read;
			caches->lock_mode = AccessShareLock;
			caches->read_state_mctx = GetBioseqColumnReadStateCache();
			caches->main_database_oid   = relation->rd_locator.dbOid;
			caches->main_tablespace_oid = relation->rd_locator.spcOid;
			caches->main_relfilenode    = relation->rd_locator.relNumber;
			hydrate_seq_columns_in_slot(relation, slot, caches, amc,
			                            (*readState)->snapshot,
			                            GetBioseqColumnReadStateCache());
			caches->close_all(/*commit=*/true);
			bioseq::storage::destroy_seq_caches(caches);
		}
	}

	return true;
}


static void
bioseq_get_latest_tid(TableScanDesc sscan,
						ItemPointer tid)
{
	bioseq_log<ERR, PG>( "bioseq_get_latest_tid not implemented");
}


static bool
bioseq_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	bioseq_log<ERR, PG>( "bioseq_tuple_tid_valid not implemented");
}


static bool
bioseq_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								  Snapshot snapshot)
{
	uint64_t rowNumber = tid_to_row_number(slot->tts_tid);
	StripeMetadata *stripeMetadata = FindStripeByRowNumber(rel, rowNumber, snapshot);
	return stripeMetadata != NULL;
}


static TransactionId
bioseq_index_delete_tuples(Relation rel,
							 TM_IndexDeleteOp *delstate)
{
	previousCacheEnabledState = bioseq::settings::enable_column_cache;
	bioseq::settings::enable_column_cache = false;

	/*
	 * XXX: We didn't bother implementing index_delete_tuple for neither of
	 * simple deletion and bottom-up deletion cases. There is no particular
	 * reason for that, just to keep things simple.
	 *
	 * See the rest of this function to see how we deal with
	 * index_delete_tuples requests made to columnarAM.
	 */

	if (delstate->bottomup)
	{
		/*
		 * Ignore any bottom-up deletion requests.
		 *
		 * Currently only caller in postgres that does bottom-up deletion is
		 * _bt_bottomupdel_pass, which in turn calls _bt_delitems_delete_check.
		 * And this function is okay with ndeltids being set to 0 by tableAM
		 * for bottom-up deletion.
		 */
		delstate->ndeltids = 0;
		return InvalidTransactionId;
	}
	else
	{
		/*
		 * TableAM is not expected to set ndeltids to 0 for simple deletion
		 * case, so here we cannot do the same trick that we do for
		 * bottom-up deletion.
		 * See the assertion around table_index_delete_tuples call in pg
		 * function index_compute_xid_horizon_for_tuples.
		 *
		 * For this reason, to avoid receiving simple deletion requests for
		 * columnar tables (bottomup = false), bioseq_index_fetch_tuple
		 * doesn't ever set all_dead to true in order to prevent triggering
		 * simple deletion of index tuples. But let's throw an error to be on
		 * the safe side.
		 */
		bioseq_log<ERR, PG>( "bioseq_index_delete_tuples not implemented for simple deletion");
	}
}


[[nodiscard]] static bioseq::storage::seq_caches*
ensure_write_side_seq_caches(Relation relation,
                             BioseqColumnWriteState* write_state)
{
	if (relation == nullptr || write_state == nullptr
		|| write_state->stripeWriteContext == nullptr)
	{
		bioseq::pg_utils::invoke_pg_as_cpp([] {
			bioseq_log<ERR, PG>(
				"bioseq storage: sequence write cache owner is unavailable");
		});
	}

	if (write_state->seq_caches == nullptr)
	{
		MemoryContext owner = MemoryContextGetParent(
			write_state->stripeWriteContext);
		if (owner == nullptr)
		{
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				bioseq_log<ERR, PG>(
					"bioseq storage: sequence write cache context is unavailable");
			});
		}
		auto* fresh = bioseq::pg_utils::invoke_pg_as_cpp([owner] {
			return bioseq::storage::alloc_seq_caches(owner);
		});
		fresh->side = bioseq::storage::seq_caches::Side::Write;
		fresh->lock_mode = RowExclusiveLock;
		fresh->main_database_oid = relation->rd_locator.dbOid;
		fresh->main_tablespace_oid = relation->rd_locator.spcOid;
		fresh->main_relfilenode = relation->rd_locator.relNumber;
		fresh->cap_bytes_snapshot =
			bioseq::storage::read_max_sub_table_cap_bytes();
		write_state->seq_caches = fresh;
	}

	auto* caches = static_cast<bioseq::storage::seq_caches*>(
		write_state->seq_caches);
	if (caches->side != bioseq::storage::seq_caches::Side::Write
		|| caches->main_database_oid != relation->rd_locator.dbOid
		|| caches->main_tablespace_oid != relation->rd_locator.spcOid
		|| caches->main_relfilenode != relation->rd_locator.relNumber)
	{
		bioseq::pg_utils::invoke_pg_as_cpp([] {
			bioseq_log<ERR, PG>(
				"bioseq storage: sequence write cache relation identity changed");
		});
	}
	return caches;
}


/*
 * DOC0260 Phase A — rewrite every non-NULL SEQ column in `detoasted_values`
 * to a 40-byte locator stub. The original payload bytes are written to the
 * matching sub-table shard via sequence_storage_manager::insert_payload.
 * Returns a freshly-palloc'd Datum* (in the per-tuple context); the caller
 * MUST hand THIS to BioseqColumnWriteRow (NOT slot->tts_values) so that
 * ExecInsertIndexTuples — which reads from `slot->tts_values[]` to build
 * btree/hash/gin keys — never sees the storage-layout bytes.
 *
 * `out_per_col_locator[c]` is the locator for amc.seq_attnos[c]; left as
 * std::nullopt for NULL/already-stub values.
 */
[[nodiscard]] static Datum *
rewrite_seq_columns_to_stubs(Relation relation,
                             TupleTableSlot *slot,
                             BioseqColumnWriteState *writeState,
                             const bioseq::storage::BioseqAmCache &amc,
                             Datum *detoasted_values,
                             std::vector<std::optional<bioseq::storage::seq_locator>>
                                 &out_per_col_locator,
                             std::vector<std::optional<bioseq::storage::PendingSequenceSource>>
                                 &out_pending_sources)
{
	TupleDesc td = slot->tts_tupleDescriptor;
	Datum *values_for_write = bioseq::pg_utils::invoke_pg_as_cpp([td] {
		return static_cast<Datum *>(palloc(sizeof(Datum) * td->natts));
	});
	std::memcpy(values_for_write, detoasted_values, sizeof(Datum) * td->natts);

	out_per_col_locator.assign(amc.seq_attnos.size(), std::nullopt);
	out_pending_sources.clear();
	out_pending_sources.resize(amc.seq_attnos.size());
	if (amc.seq_attnos.empty()) {
		return values_for_write;
	}
	// write_state_management.cppm cannot import bioseq_storage without a
	// module cycle, so the TableAM owns this lazy initialization.  The cache
	// lives above stripeWriteContext and therefore survives stripe resets.
	auto* caches = ensure_write_side_seq_caches(relation, writeState);
	const auto cap_bytes = caches->cap_bytes_snapshot;
	const std::uint64_t main_storage_id =
		bioseq::pg_utils::invoke_pg_as_cpp([relation] {
			return BioseqColumnStorageGetStorageId(relation, false);
		});

	for (std::size_t c = 0; c < amc.seq_attnos.size(); ++c) {
		const std::int16_t attno = amc.seq_attnos[c];
		const int idx = attno - 1;
		if (idx < 0 || idx >= td->natts) continue;
		if (slot->tts_isnull[idx]) continue;
		auto *hdr = reinterpret_cast<bioseq::pg_types::seq *>(
			DatumGetPointer(values_for_write[idx]));
		if (bioseq::storage::is_locator_stub(hdr)) {
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				bioseq_log<ERR, PG>(
					"bioseq storage: a locator stub cannot be shared by a second parent row");
			});
		}
		auto pending = bioseq::pg_utils::invoke_pg_as_cpp(
			[relation, main_storage_id, attno, td, hdr, cap_bytes, caches] {
				return bioseq::storage::bioseq_default_storage_manager::
					insert_payload_as_fragments(
				RelationGetRelid(relation), main_storage_id,
				relation->rd_locator.dbOid,
				relation->rd_locator.spcOid,
				relation->rd_locator.relNumber,
				attno, td, hdr, cap_bytes, caches);
			});
		const auto loc = pending.locator();
		out_per_col_locator[c] = loc;
		out_pending_sources[c].emplace(std::move(pending));
		bioseq::pg_types::seq *stub = bioseq::pg_utils::invoke_pg_as_cpp(
			[writeState, hdr, loc] {
				return bioseq::storage::alloc_locator_stub_in(
					BioseqColumnWritePerTupleContext(writeState),
					static_cast<std::uint8_t>(hdr->alphabet_code),
					static_cast<std::uint8_t>(hdr->storage_unit_byte_width),
					hdr->padding_letters,
					/*total_bp_size=*/loc.total_bp_size,
					loc);
			});
		values_for_write[idx] = PointerGetDatum(stub);
	}
	return values_for_write;
}

static void publish_sequence_sources_after_parent_row(
		std::vector<std::optional<bioseq::storage::PendingSequenceSource>>& sources)
{
	for (auto& source : sources) {
		if (source.has_value()) {
			Assert(source->pending());
			bioseq::pg_utils::invoke_pg_as_cpp([&source] {
				source->publish_after_parent_row();
			});
			Assert(!source->pending());
		}
	}
}

template <class Fn>
decltype(auto) run_tableam_cpp_checked(const char* operation, Fn&& function) {
	ErrorData* exact_error = nullptr;
	std::array<char, 512> detail{};
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	try {
		if constexpr (std::is_void_v<std::invoke_result_t<Fn&>>) {
			function();
			return;
		} else {
			return function();
		}
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		exact_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			detail.data(),
			error.what() == nullptr ? "std::exception without detail" : error.what(),
			detail.size());
		cpp_error = true;
	} catch (...) {
		unknown_cpp_error = true;
	}
	// The callback scope (including every vector, pending-source authority and
	// MemoryContextScope) has unwound.  Only fixed diagnostic state remains at
	// this PostgreSQL C callback boundary.
	if (exact_error != nullptr) {
		ReThrowError(exact_error);
	}
	if (cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("%s raised a C++ exception", operation),
			 errdetail_internal("%s", detail.data())));
	}
	if (unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("%s raised an unknown C++ exception", operation)));
	}
	pg_unreachable();
}

template <class Fn>
decltype(auto) run_index_source_cpp_checked(
	const char* failure_message,
	Fn&& function)
{
	ErrorData* exact_error = nullptr;
	std::array<char, 512> detail{};
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	try {
		if constexpr (std::is_void_v<std::invoke_result_t<Fn&>>) {
			function();
			return;
		} else {
			return function();
		}
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		exact_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			detail.data(),
			error.what() == nullptr ? "std::exception without detail" : error.what(),
			detail.size());
		cpp_error = true;
	} catch (...) {
		unknown_cpp_error = true;
	}

	// PostgreSQL reporting is a tail action after every C++ handler has ended.
	// ReThrowError preserves the original ErrorData byte-for-byte; translated
	// C++ failures retain the historical XX000 message/detail contract.
	if (exact_error != nullptr) {
		ReThrowError(exact_error);
	}
	if (cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("%s", failure_message),
			 errdetail("%s", detail.data())));
	}
	if (unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("%s", failure_message),
			 errdetail("unknown C++ exception")));
	}
	pg_unreachable();
}

static bool
bioseq_getnextslot(
	TableScanDesc scan,
	ScanDirection direction,
	TupleTableSlot* slot)
{
	return run_tableam_cpp_checked(
		"bioseq getnextslot",
		[scan, direction, slot] {
			return bioseq_getnextslot_impl(scan, direction, slot);
		});
}

[[nodiscard]] static Datum *
detoast_values_for_heap_tuple(TupleDesc tuple_desc, Datum *values, bool *nulls)
{
	const auto previous_scope = exchange_slice_materialization_scope({
		.attachment = nullptr,
		.boundary_id = 0u,
		.producer_value_id = 0u,
		.mechanism = materialization_mechanism::packed_full,
		.reason = materialization_reason::heap_tuple,
		.local_sink = nullptr,
	});
	const auto scope_was_restored = [&previous_scope] {
		const auto active = get_current_slice_materialization_scope();
		return active.attachment == previous_scope.attachment
			&& active.boundary_id == previous_scope.boundary_id
			&& active.producer_value_id == previous_scope.producer_value_id
			&& active.mechanism == previous_scope.mechanism
			&& active.reason == previous_scope.reason
			&& active.local_sink == previous_scope.local_sink;
	};
	try {
		/* Destination-slot copyslot is the primary durable-write boundary.
		 * Keep this TableAM scope as a fallback when an expanded value reaches
		 * the access method without having been flattened there.  Each attached
		 * EOH can still resolve its exact planned producer boundary. */
		Datum *detoasted_values = bioseq::pg_utils::invoke_pg_as_cpp(
			[tuple_desc, values, nulls] {
				return bioseq::pg_utils::detoast_values(
					tuple_desc, values, nulls);
			});
		restore_slice_materialization_scope(previous_scope);
		Assert(scope_was_restored());
		return detoasted_values;
	} catch (...) {
		restore_slice_materialization_scope(previous_scope);
		Assert(scope_was_restored());
		throw;
	}
}

static void
bioseq_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
					  int options, BulkInsertState bistate)
{
	BioseqColumnWriteState *writeState = nullptr;
	OrdinaryInsertPageCacheRestore cache_restore{
		.callback = {
			.previous = error_context_stack,
			.callback = &restore_ordinary_insert_page_cache_on_error,
			.arg = nullptr,
		},
		.previous_value = bioseq::settings::enable_column_cache,
		.armed = true,
	};
	cache_restore.callback.arg = &cache_restore;
	error_context_stack = &cache_restore.callback;
	bioseq::settings::enable_column_cache = false;
	run_tableam_cpp_checked("bioseq tuple insert",
		[relation, slot, &writeState] {
		if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		{
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				bioseq_log<ERR, PG>(
					"bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
			});
		}
		writeState = bioseq::pg_utils::invoke_pg_as_cpp([relation, slot] {
			return bioseq_init_write_state(
				relation, RelationGetDescr(relation), slot->tts_tableOid,
				GetCurrentSubTransactionId());
		});
		bioseq::pg_utils::invoke_pg_as_cpp([relation] {
			BioseqColumnCheckLogicalReplication(relation);
		});
		const auto *amc = bioseq::pg_utils::invoke_pg_as_cpp([relation] {
			return &bioseq::storage::bioseq_am_cache_for(relation);
		});

		{
			MemoryContextScope ctx_guard{
				BioseqColumnWritePerTupleContext(writeState)};

			bioseq::pg_utils::invoke_pg_as_cpp([slot] {
				slot_getallattrs(slot);
			});

			Datum *detoasted_values = detoast_values_for_heap_tuple(
				slot->tts_tupleDescriptor, slot->tts_values,
				slot->tts_isnull);

			/* DOC0260 Phase A — rewrite SEQ columns to locator stubs. */
			std::vector<std::optional<bioseq::storage::seq_locator>>
				per_col_locator;
			std::vector<std::optional<
				bioseq::storage::PendingSequenceSource>> pending_sources;
			Datum *values_for_write = rewrite_seq_columns_to_stubs(
				relation, slot, writeState, *amc, detoasted_values,
				per_col_locator, pending_sources);

			const uint64_t writtenRowNumber =
				bioseq::pg_utils::invoke_pg_as_cpp(
					[writeState, values_for_write, slot] {
						return BioseqColumnWriteRow(
							writeState, values_for_write, slot->tts_isnull);
					});
			slot->tts_tid = bioseq::pg_utils::invoke_pg_as_cpp(
				[writtenRowNumber] {
					return row_number_to_tid(writtenRowNumber);
				});
			publish_sequence_sources_after_parent_row(pending_sources);

			/* DOC0279 §11.2 — build every UPDATE_ON_INSERT index (per SEQ column)
			 * via the new chunked-storage drivers. detoasted_values carries the
			 * ORIGINAL payload (pre-stub) so the index sees the real bytes. */
			bioseq::storage::indexing::index_build_on_insert_columns(
				RelationGetRelid(relation), amc->seq_attnos,
				slot->tts_isnull, detoasted_values, per_col_locator,
				writtenRowNumber,
				BioseqColumnWritePerTupleContext(writeState));
		}
		bioseq::pg_utils::invoke_pg_as_cpp([writeState] {
			MemoryContextReset(BioseqColumnWritePerTupleContext(writeState));
		});
		bioseq::pg_utils::invoke_pg_as_cpp([relation] {
			pgstat_count_heap_insert(relation, 1);
		});
	});
	Assert(writeState != nullptr);
	Assert(error_context_stack == &cache_restore.callback);
	bioseq::settings::enable_column_cache = cache_restore.previous_value;
	cache_restore.armed = false;
	error_context_stack = cache_restore.callback.previous;
}


static void
bioseq_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
								  CommandId cid, int options,
								  BulkInsertState bistate, uint32_t specToken)
{
	previousCacheEnabledState = bioseq::settings::enable_column_cache;
	bioseq::settings::enable_column_cache = false;

	OrdinaryInsertPageCacheRestore cache_restore{
		.callback = {
			.previous = error_context_stack,
			.callback = &restore_ordinary_insert_page_cache_on_error,
			.arg = nullptr,
		},
		.previous_value = previousCacheEnabledState,
		.armed = true,
	};
	cache_restore.callback.arg = &cache_restore;
	error_context_stack = &cache_restore.callback;
	BioseqColumnWriteState *writeState = nullptr;
	run_tableam_cpp_checked("bioseq speculative tuple insert",
		[relation, slot, &writeState] {
			if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
			{
				bioseq::pg_utils::invoke_pg_as_cpp([] {
					bioseq_log<ERR, PG>(
						"bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
				});
			}
			writeState = bioseq::pg_utils::invoke_pg_as_cpp(
				[relation, slot] {
					return bioseq_init_write_state(
						relation, RelationGetDescr(relation),
						slot->tts_tableOid, GetCurrentSubTransactionId());
				});
			bioseq::pg_utils::invoke_pg_as_cpp([relation] {
				BioseqColumnCheckLogicalReplication(relation);
			});
			const auto *amc = bioseq::pg_utils::invoke_pg_as_cpp([relation] {
				return &bioseq::storage::bioseq_am_cache_for(relation);
			});

			{
				MemoryContextScope ctx_guard{
					BioseqColumnWritePerTupleContext(writeState)};

				bioseq::pg_utils::invoke_pg_as_cpp([slot] {
					slot_getallattrs(slot);
				});

				Datum *detoasted_values = detoast_values_for_heap_tuple(
					slot->tts_tupleDescriptor, slot->tts_values,
					slot->tts_isnull);

				std::vector<std::optional<bioseq::storage::seq_locator>>
					per_col_locator;
				std::vector<std::optional<
					bioseq::storage::PendingSequenceSource>> pending_sources;
				Datum *values_for_write = rewrite_seq_columns_to_stubs(
					relation, slot, writeState, *amc, detoasted_values,
					per_col_locator, pending_sources);

				const uint64_t storageId =
					bioseq::pg_utils::invoke_pg_as_cpp([relation] {
						return LookupStorageId(relation->rd_locator);
					});
				const uint64_t writtenRowNumber =
					bioseq::pg_utils::invoke_pg_as_cpp(
						[writeState, values_for_write, slot] {
							return BioseqColumnWriteRow(
								writeState, values_for_write,
								slot->tts_isnull);
						});
				bioseq::pg_utils::invoke_pg_as_cpp(
					[relation, storageId, writtenRowNumber] {
						UpdateRowMask(
							relation->rd_locator, storageId, nullptr,
							writtenRowNumber);
					});
					slot->tts_tid = bioseq::pg_utils::invoke_pg_as_cpp(
						[writtenRowNumber] {
							return row_number_to_tid(writtenRowNumber);
						});
					const SubTransactionId subtransaction_id =
						bioseq::pg_utils::invoke_pg_as_cpp([] {
							return GetCurrentSubTransactionId();
						});
					PendingSpeculativeSources speculative{
						.subtransaction_id = subtransaction_id,
					};
				speculative.sources.reserve(pending_sources.size());
				for (auto& source : pending_sources) {
					if (source.has_value()) {
						speculative.sources.push_back(std::move(*source));
						Assert(!source->pending());
						Assert(speculative.sources.back().pending());
					}
				}
				const PendingSpeculativeKey key{
					RelationGetRelid(relation), writtenRowNumber};
				if (!pending_speculative_sources().emplace(
						key, std::move(speculative)).second) {
					bioseq::pg_utils::invoke_pg_as_cpp([] {
						bioseq_log<ERR, PG>(
							"bioseq storage: duplicate speculative source publication key");
					});
				}
			}
			bioseq::pg_utils::invoke_pg_as_cpp([writeState] {
				MemoryContextReset(
					BioseqColumnWritePerTupleContext(writeState));
			});
			bioseq::pg_utils::invoke_pg_as_cpp([relation] {
				pgstat_count_heap_insert(relation, 1);
			});
		});
	Assert(writeState != nullptr);
	Assert(error_context_stack == &cache_restore.callback);
	cache_restore.armed = false;
	error_context_stack = cache_restore.callback.previous;
}


static void
bioseq_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
									uint32_t specToken, bool succeeded)
{
	(void)specToken;
	OrdinaryInsertPageCacheRestore cache_restore{
		.callback = {
			.previous = error_context_stack,
			.callback = &restore_ordinary_insert_page_cache_on_error,
			.arg = nullptr,
		},
		.previous_value = previousCacheEnabledState,
		.armed = true,
	};
	cache_restore.callback.arg = &cache_restore;
	error_context_stack = &cache_restore.callback;
	run_tableam_cpp_checked("bioseq complete speculative tuple insert",
		[relation, slot, succeeded] {
			const uint64_t storageId =
				bioseq::pg_utils::invoke_pg_as_cpp([relation] {
					return LookupStorageId(relation->rd_locator);
				});
			/* Set lock for relation until transaction ends. */
			bioseq::pg_utils::invoke_pg_as_cpp([storageId] {
				(void)DirectFunctionCall1(
					pg_advisory_xact_lock_int8,
					Int64GetDatum(static_cast<int64_t>(storageId)));
			});
			const std::uint64_t row_number =
				bioseq::pg_utils::invoke_pg_as_cpp([slot] {
					return tid_to_row_number(slot->tts_tid);
				});
			const PendingSpeculativeKey key{
				RelationGetRelid(relation), row_number};
			auto pending = pending_speculative_sources().find(key);
			if (pending == pending_speculative_sources().end()) {
				bioseq::pg_utils::invoke_pg_as_cpp([] {
					bioseq_log<ERR, PG>(
						"bioseq storage: speculative source publication authority is missing");
				});
			}
			if (succeeded) {
				for (auto& source : pending->second.sources) {
					Assert(source.pending());
					bioseq::pg_utils::invoke_pg_as_cpp([&source] {
						source.publish_after_parent_row();
					});
					Assert(!source.pending());
				}
			} else {
				for (auto& source : pending->second.sources) {
					Assert(source.pending());
					bioseq::pg_utils::invoke_pg_as_cpp([&source] {
						source.invalidate_without_parent_row();
					});
					Assert(!source.pending());
				}
			}
			pending_speculative_sources().erase(pending);
		});
	Assert(error_context_stack == &cache_restore.callback);
	bioseq::settings::enable_column_cache = previousCacheEnabledState;
	cache_restore.armed = false;
	error_context_stack = cache_restore.callback.previous;
}

static void
bioseq_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
					  CommandId cid, int options, BulkInsertState bistate)
{
	run_tableam_cpp_checked("bioseq multi insert",
		[relation, slots, ntuples] {
		if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		{
			bioseq::pg_utils::invoke_pg_as_cpp([] {
				bioseq_log<ERR, PG>(
					"bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
			});
		}
		BioseqColumnWriteState *writeState =
			bioseq::pg_utils::invoke_pg_as_cpp([relation, slots] {
				return bioseq_init_write_state(
					relation, RelationGetDescr(relation),
					slots[0]->tts_tableOid, GetCurrentSubTransactionId());
			});
		bioseq::pg_utils::invoke_pg_as_cpp([relation] {
			BioseqColumnCheckLogicalReplication(relation);
		});
		const auto *amc = bioseq::pg_utils::invoke_pg_as_cpp([relation] {
			return &bioseq::storage::bioseq_am_cache_for(relation);
		});

		{
			MemoryContextScope ctx_guard{
				BioseqColumnWritePerTupleContext(writeState)};

			for (int i = 0; i < ntuples; i++)
			{
				TupleTableSlot *tupleSlot = slots[i];

				bioseq::pg_utils::invoke_pg_as_cpp([tupleSlot] {
					slot_getallattrs(tupleSlot);
				});

				Datum *detoasted_values = detoast_values_for_heap_tuple(
					tupleSlot->tts_tupleDescriptor,
					tupleSlot->tts_values, tupleSlot->tts_isnull);

				std::vector<std::optional<bioseq::storage::seq_locator>>
					per_col_locator;
				std::vector<std::optional<
					bioseq::storage::PendingSequenceSource>> pending_sources;
				Datum *values_for_write = rewrite_seq_columns_to_stubs(
					relation, tupleSlot, writeState, *amc,
					detoasted_values, per_col_locator, pending_sources);

				const uint64_t writtenRowNumber =
					bioseq::pg_utils::invoke_pg_as_cpp(
						[writeState, values_for_write, tupleSlot] {
							return BioseqColumnWriteRow(
								writeState, values_for_write,
								tupleSlot->tts_isnull);
						});
				publish_sequence_sources_after_parent_row(pending_sources);

				bioseq::storage::indexing::index_build_on_insert_columns(
					RelationGetRelid(relation), amc->seq_attnos,
					tupleSlot->tts_isnull, detoasted_values,
					per_col_locator, writtenRowNumber,
					BioseqColumnWritePerTupleContext(writeState));

				EState *estate =
					bioseq::pg_utils::invoke_pg_as_cpp([relation] {
						return create_estate_for_relation(relation);
					});

				ResultRelInfo *resultRelInfo =
					bioseq::pg_utils::invoke_pg_as_cpp([] {
						return makeNode(ResultRelInfo);
					});
				bioseq::pg_utils::invoke_pg_as_cpp(
					[resultRelInfo, relation] {
						InitResultRelInfo(
							resultRelInfo, relation, 1, nullptr, 0);
					});

				bioseq::pg_utils::invoke_pg_as_cpp([resultRelInfo] {
					ExecOpenIndices(resultRelInfo, false);
				});

				if (relation->rd_att->constr)
					bioseq::pg_utils::invoke_pg_as_cpp(
						[resultRelInfo, tupleSlot, estate] {
							ExecConstraints(
								resultRelInfo, tupleSlot, estate);
						});

				bioseq::pg_utils::invoke_pg_as_cpp([resultRelInfo] {
					ExecCloseIndices(resultRelInfo);
				});

				bioseq::pg_utils::invoke_pg_as_cpp([estate] {
					AfterTriggerEndQuery(estate);
				});
				bioseq::pg_utils::invoke_pg_as_cpp([estate] {
					ExecCloseResultRelations(estate);
				});
				bioseq::pg_utils::invoke_pg_as_cpp([estate] {
					ExecCloseRangeTableRelations(estate);
				});

				bioseq::pg_utils::invoke_pg_as_cpp([estate] {
					ExecResetTupleTable(estate->es_tupleTable, false);
				});
				bioseq::pg_utils::invoke_pg_as_cpp([estate] {
					FreeExecutorState(estate);
				});

				tupleSlot->tts_tid = bioseq::pg_utils::invoke_pg_as_cpp(
					[writtenRowNumber] {
						return row_number_to_tid(writtenRowNumber);
					});

				bioseq::pg_utils::invoke_pg_as_cpp([writeState] {
					MemoryContextReset(
						BioseqColumnWritePerTupleContext(writeState));
				});
			}
		}
		bioseq::pg_utils::invoke_pg_as_cpp([relation, ntuples] {
			pgstat_count_heap_insert(relation, ntuples);
		});
	});
}

/// Invalidate only the epoch(s) carried by the exact old parent row before its
/// row mask changes. The catalog update is MVCC: an older registered snapshot
/// continues to see the prior READY tuple, while a newer snapshot cannot admit
/// the masked source.
static void
invalidate_sequence_source_epochs_for_row(Relation relation,
                                           std::uint64_t row_number,
                                           Snapshot snapshot)
{
	const auto &amc = bioseq::storage::bioseq_am_cache_for(relation);
	if (amc.seq_attnos.empty()) return;
	Snapshot read_snapshot = snapshot != nullptr ? snapshot : GetTransactionSnapshot();
	MemoryContext read_context = CreateBioseqColumnScanMemoryContext();
	{
		MemoryContextScope context_guard{read_context};
		TupleDesc descriptor = RelationGetDescr(relation);
		PBitmapset attributes = PBitmapset::full_range(descriptor->natts);
		BioseqColumnReadState *read_state = init_bioseq_read_state(
			relation, descriptor, attributes, NIL, read_context, read_snapshot,
			/*randomAccess=*/true, nullptr);
		Datum *values = static_cast<Datum *>(palloc0(
			sizeof(Datum) * static_cast<std::size_t>(descriptor->natts)));
		bool *nulls = static_cast<bool *>(palloc0(
			sizeof(bool) * static_cast<std::size_t>(descriptor->natts)));
		if (BioseqColumnReadRowByRowNumber(read_state, row_number, values, nulls)) {
			for (const std::int16_t attno : amc.seq_attnos) {
				const int index = attno - 1;
				if (index < 0 || index >= descriptor->natts || nulls[index]) continue;
				const auto *header = reinterpret_cast<const bioseq::pg_types::seq *>(
					DatumGetPointer(values[index]));
				if (!bioseq::storage::is_locator_stub(header)) continue;
				const auto locator = bioseq::storage::decode_locator_from_stub(header);
				bioseq::storage::SequenceSourceVersionManager::invalidate_epoch(
					locator.source_epoch);
			}
		}
		BioseqColumnEndRead(read_state);
	}
	MemoryContextDelete(read_context);
}


static TM_Result
bioseq_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
					  Snapshot snapshot, Snapshot crosscheck, bool wait,
					  TM_FailureData *tmfd, bool changingPart)
{
	if (bioseq::storage::is_registered_sequence_fragment_relation(
			RelationGetRelid(relation))) {
		bioseq_log<ERR, PG>(
			"bioseq storage: registered sequence fragments are immutable; DELETE is forbidden");
	}
	uint64_t rowNumber = tid_to_row_number(*tid);
	uint64_t storageId = LookupStorageId(relation->rd_locator);
	/* Set lock for relation until transaction ends */
	DirectFunctionCall1(pg_advisory_xact_lock_int8,
						Int64GetDatum((int64_t) storageId));

	invalidate_sequence_source_epochs_for_row(relation, rowNumber, snapshot);
	if (!UpdateRowMask(relation->rd_locator, storageId,  snapshot, rowNumber))
		return TM_Deleted;

	pgstat_count_heap_delete(relation);

	return TM_Ok;
}


static TM_Result
bioseq_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
					  CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					  bool wait, TM_FailureData *tmfd,
					  LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	if (bioseq::storage::is_registered_sequence_fragment_relation(
			RelationGetRelid(relation))) {
		bioseq_log<ERR, PG>(
			"bioseq storage: registered sequence fragments are immutable; UPDATE is forbidden");
	}

	
	uint64_t rowNumber = tid_to_row_number(*otid);
	uint64_t storageId = LookupStorageId(relation->rd_locator);

	/* Set lock for relation until transaction ends */
	DirectFunctionCall1(pg_advisory_xact_lock_int8,
						Int64GetDatum((int64_t) storageId));

	invalidate_sequence_source_epochs_for_row(relation, rowNumber, snapshot);
	if (!UpdateRowMask(relation->rd_locator, storageId, snapshot, rowNumber))
		return TM_Deleted;

	bioseq_tuple_insert(relation, slot, cid, 0, NULL);

	*update_indexes = (TU_UpdateIndexes) true;

	pgstat_count_heap_update(relation, false, false);
	return TM_Ok;
}


static TM_Result
bioseq_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot,
					TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
					LockWaitPolicy wait_policy, uint8_t flags,
					TM_FailureData *tmfd)
{
	uint64_t rowNumber = tid_to_row_number(*tid);
	BioseqColumnReadState *readState = NULL;

	int natts = relation->rd_att->natts;
	PBitmapset attr_needed = PBitmapset::full_range(natts);

	List *scanQual = NIL;

	bool randomAccess = true;

	readState = init_bioseq_read_state(relation,
										slot->tts_tupleDescriptor,
										attr_needed, scanQual,
										CurrentMemoryContext, // to be checked
										GetTransactionSnapshot(), randomAccess,
										NULL);

	BioseqColumnReadRowByRowNumber(readState, rowNumber,
							   slot->tts_values, slot->tts_isnull);

	slot->tts_tableOid = RelationGetRelid(relation);
	slot->tts_tid = *tid;

	if (TTS_EMPTY(slot))
	{
		ExecStoreVirtualTuple(slot);
	}

	return TM_Ok;
}


static void
bioseq_finish_bulk_insert(Relation relation, int options)
{
	/*
	 * Nothing to do here. We keep write states live until transaction end.
	 */
}


static void
bioseq_relation_set_new_filenode(Relation rel,
								   const RelFileLocator *newrnode,
								   char persistence,
								   TransactionId *freezeXid,
								   MultiXactId *minmulti)
{
	if (bioseq::storage::is_registered_sequence_fragment_relation(
			RelationGetRelid(rel))) {
		bioseq_log<ERR, PG>(
			"bioseq storage: destructive rewrite of a registered sequence fragment relation is forbidden");
	}
	if (persistence == RELPERSISTENCE_TEMP)
	{
		bioseq_log<ERR, PG>(
			"bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
	}

	/*
	 * If existing and new relfilelocator are different, that means the existing
	 * storage was dropped and we also need to clean up the metadata and
	 * state. If they are equal, this is a new relation object and we don't
	 * need to clean anything.
	 */
	if (rel->rd_locator.relNumber != newrnode->relNumber)
	{
		MarkRelfilenodeDropped(rel->rd_locator.relNumber, GetCurrentSubTransactionId());

		DeleteMetadataRows(rel->rd_locator);

		/* DOC0260 Phase A — cascade-drop sub-tables under the OLD
		 * relfilenode triple. */
		bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
			rel->rd_locator.dbOid,
			rel->rd_locator.spcOid,
			rel->rd_locator.relNumber);
	}
	*freezeXid = RecentXmin;
	*minmulti = GetOldestMultiXactId();
	SMgrRelation srel = RelationCreateStorage(*newrnode, persistence, true);

	bioseq_log<DBG>("[bioseq] pg: bioseq_relation_set_new_filenode() initialising columnar storage");
	BioseqColumnStorageInit(srel, BioseqColumnMetadataNewStorageId());
	bioseq_log<DBG>("[bioseq] pg: bioseq_relation_set_new_filenode() initialising columnar options");
	InitBioseqColumnOptions(rel->rd_id);

	smgrclose(srel);

	/* we will lazily initialize metadata in first stripe reservation */
}


static void
bioseq_relation_nontransactional_truncate(Relation rel)
{
	if (bioseq::storage::is_registered_sequence_fragment_relation(
			RelationGetRelid(rel))) {
		bioseq_log<ERR, PG>(
			"bioseq storage: TRUNCATE of a registered sequence fragment relation is forbidden");
	}
	RelFileLocator relfilelocator = rel->rd_locator;

	NonTransactionDropWriteState(relfilelocator.relNumber);
	/* Delete old relfilelocator metadata */
	DeleteMetadataRows(relfilelocator);

	/*
	 * No need to set new relfilelocator, since the table was created in this
	 * transaction and no other transaction can see this relation yet. We
	 * can just truncate the relation.
	 *
	 * This is similar to what is done in heapam_relation_nontransactional_truncate.
	 */
	RelationTruncate(rel, 0);

	bioseq_log<DBG>("[bioseq] pg: bioseq_relation_nontransactional_truncate() initialising columnar storage");
	uint64_t storageId = BioseqColumnMetadataNewStorageId();

	BioseqColumnStorageInit(RelationGetSmgr(rel), storageId);
}


static void
bioseq_relation_copy_data(Relation rel, const RelFileLocator *newrnode)
{
	char persistence = rel->rd_rel->relpersistence;

	/*
	 * Since we copy the file directly without looking at the shared buffers,
	 * we'd better first flush out any pages of the source relation that are
	 * in shared buffers.  We assume on new changes will be made while we are
	 * holding exclusive lock on the rel.
	 */
	FlushRelationBuffers(rel);

	SMgrRelation srel = RelationCreateStorage(*newrnode, persistence, true);

	/* copy main fork */
	RelationCopyStorage(RelationGetSmgr(rel), srel, MAIN_FORKNUM, persistence);

	for (ForkNumber forkNum = static_cast<ForkNumber>(MAIN_FORKNUM + 1);
		forkNum <= MAX_FORKNUM; forkNum = static_cast<ForkNumber>(static_cast<int>(forkNum) + 1))
	{
		if (smgrexists(srel, forkNum))
		{
			smgrcreate(srel, forkNum, false);

			/*
			 * WAL log creation if the relation is persistent, or
			 * this is the init fork of an unlogged relation.
			 */
			if (RelationIsPermanent(rel) ||
				(persistence == RELPERSISTENCE_UNLOGGED &&
				forkNum == INIT_FORKNUM))
				log_smgrcreate(newrnode, forkNum);

			RelationCopyStorage(RelationGetSmgr(rel), srel,
						forkNum, persistence);
		}
	}
}


/*
 * bioseq_relation_copy_for_cluster is called on VACUUM FULL, at which
 * we should copy data from OldHeap to NewHeap.
 *
 * In general TableAM case this can also be called for the CLUSTER command
 * which is not applicable for columnar since it doesn't support indexes.
 */
static void
bioseq_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap,
								   Relation OldIndex, bool use_sort,
								   TransactionId OldestXmin,
								   TransactionId *xid_cutoff,
								   MultiXactId *multi_cutoff,
								   double *num_tuples,
								   double *tups_vacuumed,
								   double *tups_recently_dead)
{
	TupleDesc sourceDesc = RelationGetDescr(OldHeap);
	TupleDesc targetDesc = RelationGetDescr(NewHeap);

	if (OldIndex != NULL || use_sort)
	{
		bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
			.msg("clustering columnar tables using indexes is not supported");
	}

	/*
	 * copy_table_data in cluster.c assumes tuple descriptors are exactly
	 * the same. Even dropped columns exist and are marked as attisdropped
	 * in the target relation.
	 */
	Assert(sourceDesc->natts == targetDesc->natts);

	/* read settings from old heap, relfilelocator will be swapped at the end */
	BioseqColumnOptions bioseqColumnOptions =
		read_bioseq_column_options(OldHeap->rd_id)
			.value_or(BioseqColumnOptions{});

	/*
	 * DOC0260 Phase A — CLUSTER allocates a new relfilenode for NewHeap.
	 * The catalog rows in bioseq_internal.seq_sub_table are keyed on the
	 * (db, spc, relfilenode) triple, so OldHeap's sub-tables (under
	 * OldHeap's relfilenode) are independent from NewHeap's sub-tables
	 * (under NewHeap's relfilenode).
	 *
	 * We thread the source scan through with defer_seq_hydration=TRUE so
	 * we see OldHeap's stubs verbatim. For each row, decode the stub,
	 * fetch_payload against OldHeap's triple, then re-insert_payload into
	 * NewHeap's triple via the regular rewrite path on the destination
	 * write state.
	 */
	BioseqColumnWriteState *writeState = bioseq_init_write_state(NewHeap, targetDesc,
		RelationGetRelid(NewHeap), GetCurrentSubTransactionId());
	auto* write_caches = ensure_write_side_seq_caches(NewHeap, writeState);
	const auto cap_bytes = write_caches->cap_bytes_snapshot;

	/* we need all columns */
	int natts = OldHeap->rd_att->natts;
	PBitmapset attr_needed = PBitmapset::full_range(natts);

	/* no quals for table rewrite */
	List *scanQual = NIL;

	/* Unlike heap tuples, columnar dead-row state lives in an MVCC row-mask
	 * relation. SnapshotAny would see both the superseded and current mask and
	 * can therefore copy an UPDATE's dead version into the rewritten table.
	 * Keep one registered transaction snapshot authoritative for the main row,
	 * row mask, and sequence-source fragments throughout the rewrite. */
	Snapshot snapshot = RegisterSnapshot(GetTransactionSnapshot());

	MemoryContext scanContext = CreateBioseqColumnScanMemoryContext();
	bool randomAccess = false;
	BioseqColumnReadState *readState = init_bioseq_read_state(OldHeap, sourceDesc,
															attr_needed, scanQual,
															scanContext, snapshot,
															randomAccess,
															NULL);

	/* Read-side seq cache: hydrates OldHeap's stubs to real payloads. */
	auto* read_caches = bioseq::storage::alloc_seq_caches(scanContext);
	read_caches->side = bioseq::storage::seq_caches::Side::Read;
	read_caches->lock_mode = AccessShareLock;
	read_caches->read_state_mctx = scanContext;
	read_caches->main_database_oid   = OldHeap->rd_locator.dbOid;
	read_caches->main_tablespace_oid = OldHeap->rd_locator.spcOid;
	read_caches->main_relfilenode    = OldHeap->rd_locator.relNumber;

	Datum *values = make_array<Datum>(sourceDesc->natts);
	bool *nulls = make_array<bool>(sourceDesc->natts);

	const auto &source_amc = bioseq::storage::bioseq_am_cache_for(OldHeap);
	const std::uint64_t new_main_storage_id =
		BioseqColumnStorageGetStorageId(NewHeap, false);

	*num_tuples = 0;

	while (BioseqColumnReadNextRow(readState, values, nulls, NULL))
	{
		std::vector<bioseq::storage::PendingSequenceSource> pending_sources;
		/* Hydrate any SEQ stub columns from OldHeap's sub-tables. */
		for (std::int16_t attno : source_amc.seq_attnos) {
			const int idx = attno - 1;
			if (idx < 0 || idx >= sourceDesc->natts) continue;
			if (nulls[idx]) continue;
			auto* hdr = reinterpret_cast<const bioseq::pg_types::seq*>(
				DatumGetPointer(values[idx]));
			if (!bioseq::storage::is_locator_stub(hdr)) continue;
			const auto loc = bioseq::storage::decode_locator_from_stub(hdr);
			bioseq::storage::validate_locator_flat_materialization_length(loc);
			bioseq::pg_types::seq* real =
				bioseq::storage::bioseq_default_storage_manager::fetch_payload(
					OldHeap->rd_locator.dbOid, OldHeap->rd_locator.spcOid,
					OldHeap->rd_locator.relNumber, attno, loc,
					readState->snapshot, scanContext, read_caches);
			values[idx] = PointerGetDatum(real);
		}

		/* Rewrite SEQ columns into stubs targeting NewHeap's sub-tables.
		 * The write-side cache shares the destination write state's lifetime. */
		for (std::int16_t attno : source_amc.seq_attnos) {
			const int idx = attno - 1;
			if (idx < 0 || idx >= sourceDesc->natts) continue;
			if (nulls[idx]) continue;
			auto* hdr = reinterpret_cast<const bioseq::pg_types::seq*>(
				DatumGetPointer(values[idx]));
			if (bioseq::storage::is_locator_stub(hdr)) continue;
			auto pending =
				bioseq::storage::bioseq_default_storage_manager::insert_payload_as_fragments(
					RelationGetRelid(OldHeap), new_main_storage_id,
					NewHeap->rd_locator.dbOid, NewHeap->rd_locator.spcOid,
					NewHeap->rd_locator.relNumber,
					attno, targetDesc, hdr, cap_bytes, write_caches);
			const auto loc = pending.locator();
			bioseq::pg_types::seq* stub = bioseq::storage::alloc_locator_stub_in(
				BioseqColumnWritePerTupleContext(writeState),
				static_cast<std::uint8_t>(hdr->alphabet_code),
				static_cast<std::uint8_t>(hdr->storage_unit_byte_width),
				hdr->padding_letters,
				loc.total_bp_size,
				loc);
			values[idx] = PointerGetDatum(stub);
			pending_sources.push_back(std::move(pending));
		}

		BioseqColumnWriteRow(writeState, values, nulls);
		for (auto& source : pending_sources) {
			source.publish_after_parent_row();
		}
		(*num_tuples)++;
	}

	*tups_vacuumed = 0;

	read_caches->close_all(/*commit=*/true);
	bioseq::storage::destroy_seq_caches(read_caches);

	BioseqColumnEndRead(readState);
	UnregisterSnapshot(snapshot);
	MemoryContextDelete(scanContext);

	/* DOC0260 §8.7 — cascade-drop OldHeap's sub-tables before the
	 * finish_heap_swap. After the swap the relfilenodes are swapped and
	 * OldHeap's sub-tables become orphans under the (now-defunct) old
	 * triple. The reaper is the long-tail safety net; this call makes
	 * the cleanup deterministic. */
	bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
		OldHeap->rd_locator.dbOid,
		OldHeap->rd_locator.spcOid,
		OldHeap->rd_locator.relNumber);
}


/*
 * TruncateAndCombineBioseqColumnStripes will combine last n stripes so they can fit
 * maximum number of rows per stripe. Stripes that are going to be combined are deleted
 * and new stripe will be written at the end of untouched last stripe. We also include
 * information about deleted rows for this implementation.
 */
static bool
TruncateAndCombineBioseqColumnStripes(Relation rel, int elevel)
{
	uint64_t totalRowNumberCount = 0;
	uint32_t startingStripeListPosition = 0;

	TupleDesc tupleDesc = RelationGetDescr(rel);

	if (tupleDesc->natts == 0)
	{
		bioseq_log_pg(elevel, "\"{}\": stopping vacuum due to zero column table",
			   RelationGetRelationName(rel));
		return false;
	}

	/* Get current columnar options */
	BioseqColumnOptions bioseqColumnOptions =
		read_bioseq_column_options(rel->rd_id)
			.value_or(BioseqColumnOptions{});

	/* Get all stripes in reverse order */
	List *stripeMetadataList = StripesForRelfilenode(rel->rd_locator, BackwardScanDirection);
	/* Empty table nothing to do */
	if (stripeMetadataList == NIL)
	{
		bioseq_log_pg(elevel, "\"{}\": stopping vacuum due to empty table",
			   RelationGetRelationName(rel));
		return false;
	}

	//ListCell *lc = NULL;

	uint32_t lastStripeDeletedRows = 0;

	Size totalDecompressedStripeLength = 0;

	for (auto stripeMetadata : PList<StripeMetadata>(stripeMetadataList))
	{
		lastStripeDeletedRows = DeletedRowsForStripe(rel->rd_locator,
													 stripeMetadata->chunkCount,
													 stripeMetadata->id);
		totalDecompressedStripeLength +=
			DecompressedLengthForStripe(rel->rd_locator, stripeMetadata->id);

		if (totalDecompressedStripeLength >= 1024000000)
		{
			break;
		}

		uint64_t stripeRowCount = stripeMetadata->rowCount - lastStripeDeletedRows;

		if ((totalRowNumberCount + stripeRowCount >= bioseqColumnOptions.stripeRowCount))
		{
			break;
		}

		totalRowNumberCount += stripeRowCount;
		startingStripeListPosition++;
	}

	/*
	 * One stripe that is "full" so do nothing.
	 */
	if (startingStripeListPosition == 0)
	{
		return false;
	}

	/*
	 * There is only one stripe that is candidate. Maybe we should vacuum
	 * it if condition is met.
	 */
	else if (startingStripeListPosition == 1)
	{
		/* Maybe we should vacuum only one stripe if count of
		 * deleted rows is higher than 20 percent.
		 */
		float percentageOfDeleteRows =
			(float)lastStripeDeletedRows / (float)(totalRowNumberCount + lastStripeDeletedRows);

		bool shouldVacuumOnlyStripe = percentageOfDeleteRows > 0.2f;

		if (!shouldVacuumOnlyStripe)
		{
			return false;
		}
	}

	/*
	 * We need to clear current proces (VACUUUM) status flag here. Why?
	 * Current process has flag `PROC_IN_VACUUM` which is problematic because
	 * we write here into metadata heap tables. If concurrent process
	 * read page in which we inserted metadata tuples these tuples will be considered
	 * DEAD and will be removed (in problematic scenarion).
	 * Concurrent process, when assigning RecentXmin will scan all active processes in
	 * system but will NOT consider this process because of
	 * `PROC_IN_VACUUM` flag set. It looks that invalidating status flag here doesn't affect
	 * further execution.
	 */

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	MyProc->statusFlags = 0;
	ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;

	LWLockRelease(ProcArrayLock);

	/* We need to re-assing RecentXmin here */
	PushActiveSnapshot(GetTransactionSnapshot());
	BioseqColumnWriteState *writeState = BioseqColumnBeginWrite(rel->rd_locator,
														bioseqColumnOptions,
														tupleDesc);

	/* we need all columns */
	int natts = rel->rd_att->natts;
	PBitmapset attr_needed = PBitmapset::full_range(natts);

	/* no quals for table rewrite */
	List *scanQual = NIL;

	MemoryContext scanContext = CreateBioseqColumnScanMemoryContext();
	bool randomAccess = true;
	BioseqColumnReadState *readState = init_bioseq_read_state(rel, tupleDesc,
															attr_needed, scanQual,
															scanContext, SnapshotAny,
															randomAccess,
															NULL);

	BioseqColumnSetStripeReadState(readState,
							   static_cast<StripeMetadata*>(list_nth(stripeMetadataList, startingStripeListPosition - 1)));

	Datum *values = make_array<Datum>(tupleDesc->natts);
	bool *nulls = make_array<bool>(tupleDesc->natts);

	/* we don't need to know rowNumber here */
	while (BioseqColumnReadNextRow(readState, values, nulls, NULL))
	{
		BioseqColumnWriteRow(writeState, values, nulls);
	}

	uint64_t newDataReservation;

	if (list_length(stripeMetadataList) > startingStripeListPosition)
	{
		StripeMetadata *mtd = static_cast<StripeMetadata*>(list_nth(stripeMetadataList, startingStripeListPosition));
		newDataReservation = mtd->fileOffset + mtd->dataLength - 1;
	}
	else
	{
		StripeMetadata *mtd = static_cast<StripeMetadata*>(list_nth(stripeMetadataList, startingStripeListPosition - 1));
		newDataReservation = mtd->fileOffset;
	}

	BioseqColumnStorageTruncate(rel, newDataReservation);

	BioseqColumnEndWrite(writeState);
	BioseqColumnEndRead(readState);
	MemoryContextDelete(scanContext);

	for (int i = 0; i < startingStripeListPosition; i++)
	{
		StripeMetadata *metadata = static_cast<StripeMetadata*>(list_nth(stripeMetadataList, i));
		DeleteMetadataRowsForStripeId(rel->rd_locator, metadata->id);
	}

	PopActiveSnapshot();

	return true;
}

/*
 * BioseqColumnTableTupleCount returns the number of tuples that columnar
 * table with relationId has by using stripe metadata.
 */
static uint64_t
BioseqColumnTableTupleCount(Relation relation)
{
	List *stripeList = StripesForRelfilenode(relation->rd_locator, ForwardScanDirection);
	uint64_t tupleCount = 0;

	for (auto stripe : PList<StripeMetadata>(stripeList))
	{
		tupleCount += stripe->rowCount;
	}

	return tupleCount;
}

/*
 * bioseq_vacuum_rel implements VACUUM without FULL option.
 */
static void
bioseq_vacuum_rel(Relation rel, VacuumParams *params,
					BufferAccessStrategy bstrategy)
{
	static_cast<void>(bstrategy);
	run_tableam_cpp_checked("bioseq_vacuum_rel", [rel, params] {
		PageCacheDisableScope cache_guard{
			bioseq::settings::enable_column_cache};
		bioseq::pg_utils::invoke_pg_as_cpp([rel, params] {
	pgstat_progress_start_command(PROGRESS_COMMAND_VACUUM,
 									RelationGetRelid(rel));

	/*
	 * If metapage version of relation is older, then we hint users to VACUUM
	 * the relation in BioseqColumnMetapageCheckVersion. So if needed, upgrade
	 * the metapage before doing anything.
	 */
	bool isUpgrade = true;
	BioseqColumnStorageUpdateIfNeeded(rel, isUpgrade);

	int elevel = (params->options & VACOPT_VERBOSE) ? INFO : DEBUG2;

	/* this should have been resolved by vacuum.c until now */
	Assert(params->truncate != VACOPTVALUE_UNSPECIFIED);

	if (params->options & VACOPT_VERBOSE)
	{
		LogRelationStats(rel, elevel);
	}

	/*
	 * We don't have updates, deletes, or concurrent updates, so all we
	 * care for now is truncating the unused space at the end of storage.
	 */
	if (params->truncate == VACOPTVALUE_ENABLED)
	{
		TruncateBioseqColumn(rel, elevel);
	}

	BlockNumber new_rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

	/* get the number of indexes */
	List *indexList = RelationGetIndexList(rel);
	int nindexes = list_length(indexList);

	VacuumCutoffs cutoffs;
	vacuum_get_cutoffs(rel, params, &cutoffs);

	Assert(MultiXactIdPrecedesOrEquals(cutoffs.MultiXactCutoff, cutoffs.OldestMxact));
	Assert(TransactionIdPrecedesOrEquals(cutoffs.FreezeLimit, cutoffs.OldestXmin));

	/*
	 * BioseqColumn storage doesn't hold any transaction IDs, so we can always
	 * just advance to the most aggressive value.
	 */
	TransactionId newRelFrozenXid = cutoffs.OldestXmin;
	MultiXactId newRelminMxid = cutoffs.OldestMxact;
	double new_live_tuples = BioseqColumnTableTupleCount(rel);

	/* all visible pages are always 0 */
	BlockNumber new_rel_allvisible = 0;

	bool frozenxid_updated;
	bool minmulti_updated;

	/*
	 * PG-18 added a `num_all_frozen_pages` parameter between
	 * `num_all_visible_pages` and `hasindex`. BioseqColumn storage doesn't
	 * track per-page visibility/freezing so the value is always 0,
	 * matching the behaviour we already report for `new_rel_allvisible`.
	 */
#if PG_VERSION_NUM >= PG_VERSION_18
	BlockNumber new_rel_allfrozen = 0;
	vac_update_relstats(rel, new_rel_pages, new_live_tuples,
						new_rel_allvisible, new_rel_allfrozen,
						nindexes > 0,
						newRelFrozenXid, newRelminMxid,
						&frozenxid_updated, &minmulti_updated, false);
#else
	vac_update_relstats(rel, new_rel_pages, new_live_tuples,
						new_rel_allvisible, nindexes > 0,
						newRelFrozenXid, newRelminMxid,
						&frozenxid_updated, &minmulti_updated, false);
#endif

	/*
	 * PG-18 added a `starttime` argument so pg_stat_progress_vacuum can
	 * report how long the vacuum has been running. We don't have a real
	 * start time here (the columnar vacuum is bookkeeping only) so pass 0
	 * which the stats subsystem treats as "unknown".
	 */
#if PG_VERSION_NUM >= PG_VERSION_18
	pgstat_report_vacuum(RelationGetRelid(rel),
							rel->rd_rel->relisshared,
							std::max<int64_t>(new_live_tuples, 0),
							0,
							0 /* starttime */);
#else
	pgstat_report_vacuum(RelationGetRelid(rel),
							rel->rd_rel->relisshared,
							std::max<int64_t>(new_live_tuples, 0),
							0);
#endif
	pgstat_progress_end_command();
		});
	});
}


/*
 * LogRelationStats logs statistics as the output of the VACUUM VERBOSE.
 */
static void
LogRelationStats(Relation rel, int elevel)
{
	ListCell *stripeMetadataCell = NULL;
	RelFileLocator relfilelocator = rel->rd_locator;
	StringInfo infoBuf = makeStringInfo();

	int compressionStats[COMPRESSION_COUNT] = { 0 };
	uint64_t totalStripeLength = 0;
	uint64_t tupleCount = 0;
	uint64_t chunkCount = 0;
	TupleDesc tupdesc = RelationGetDescr(rel);
	uint64_t droppedChunksWithData = 0;
	uint64_t totalDecompressedLength = 0;

	List *stripeList = StripesForRelfilenode(relfilelocator, ForwardScanDirection);
	int stripeCount = list_length(stripeList);

	MemoryContext relation_stats_ctx =
		AllocSetContextCreate(CurrentMemoryContext, "Vacuum Relation Stats Context",
							  ALLOCSET_SMALL_SIZES);

	{
		MemoryContextScope ctx_guard{relation_stats_ctx};

		for (auto stripe : PList<StripeMetadata>(stripeList))
		{
			StripeSkipList *skiplist = ReadStripeSkipList(relfilelocator, stripe->id,
														  RelationGetDescr(rel),
														  stripe->chunkCount,
														  GetTransactionSnapshot());
			for (uint32_t column = 0; column < skiplist->columnCount; column++)
			{
				bool attrDropped = TupleDescAttr(tupdesc, column)->attisdropped;
				for (uint32_t chunk = 0; chunk < skiplist->chunkCount; chunk++)
				{
					ColumnChunkSkipNode *skipnode =
						&skiplist->chunkSkipNodeArray[column][chunk];

					/* ignore zero length chunks for dropped attributes */
					if (skipnode->valueLength > 0)
					{
						compressionStats[
							static_cast<int>(skipnode->valueCompressionType)]++;
						chunkCount++;

						if (attrDropped)
						{
							droppedChunksWithData++;
						}
					}

					/*
					 * We don't compress exists buffer, so its compressed & decompressed
					 * lengths are the same.
					 */
					totalDecompressedLength += skipnode->existsLength;
					totalDecompressedLength += skipnode->decompressedValueSize;
				}
			}

			tupleCount += stripe->rowCount;
			totalStripeLength += stripe->dataLength;

			MemoryContextReset(relation_stats_ctx);
		}
	}

	uint64_t relPages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
	RelationCloseSmgr(rel);

	const std::int64_t storage_id = static_cast<std::int64_t>(
		BioseqColumnStorageGetStorageId(rel, false));

	double compressionRate = totalStripeLength ?
							 (double) totalDecompressedLength / totalStripeLength :
							 1.0;

	appendStringInfo(infoBuf, "storage id: %ld\n", storage_id);
	appendStringInfo(infoBuf, "total file size: %ld, total data size: %ld\n",
					 relPages * BLCKSZ, totalStripeLength);
	appendStringInfo(infoBuf, "compression rate: %.2fx\n", compressionRate);
	appendStringInfo(infoBuf,
					 "total row count: %ld, stripe count: %d, "
					 "average rows per stripe: %ld\n",
					 tupleCount, stripeCount,
					 stripeCount ? tupleCount / stripeCount : 0);
	appendStringInfo(infoBuf,
					 "chunk count: %ld"
					 ", containing data for dropped columns: %ld",
					 chunkCount, droppedChunksWithData);
	for (int compressionType = 0; compressionType < COMPRESSION_COUNT; compressionType++)
	{
		const char *compressionName = CompressionTypeStr(static_cast<CompressionType>(compressionType));

		/* skip if this compression algorithm has not been compiled */
		if (compressionName == NULL)
		{
			continue;
		}

		/* skip if no chunks use this compression type */
		if (compressionStats[compressionType] == 0)
		{
			continue;
		}

		appendStringInfo(infoBuf,
						 ", %s compressed: %d",
						 compressionName,
						 compressionStats[compressionType]);
	}
	appendStringInfoString(infoBuf, "\n");

	bioseq_log_pg(elevel, "statistics for \"{}\":\n{}", RelationGetRelationName(rel),
		   infoBuf->data);
}


/*
 * TruncateBioseqColumn truncates the unused space at the end of main fork for
 * a columnar table. This unused space can be created by aborted transactions.
 *
 * This implementation is based on heap_vacuum_rel in vacuumlazy.c with some
 * changes so it suits columnar store relations.
 */
static void
TruncateBioseqColumn(Relation rel, int elevel)
{
	PGRUsage ru0;

	pg_rusage_init(&ru0);

	/* Report that we are now truncating */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_TRUNCATE);


	/*
	 * We need access exclusive lock on the relation in order to do
	 * truncation. If we can't get it, give up rather than waiting --- we
	 * don't want to block other backends, and we don't want to deadlock
	 * (which is quite possible considering we already hold a lower-grade
	 * lock).
	 *
	 * The decisions for AccessExclusiveLock and conditional lock with
	 * a timeout is based on lazy_truncate_heap in vacuumlazy.c.
	 */
	if (!ConditionalLockRelationWithTimeout(rel, AccessExclusiveLock,
											VACUUM_TRUNCATE_LOCK_TIMEOUT,
											VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL,
											false))
	{
		/*
		 * We failed to establish the lock in the specified number of
		 * retries. This means we give up truncating.
		 */
		bioseq_log_pg(elevel, "\"{}\": stopping truncate due to conflicting lock request",
			   RelationGetRelationName(rel));
		return;
	}

	const bool locator_pinned_fragment =
		bioseq::storage::is_registered_sequence_fragment_relation(
			RelationGetRelid(rel));
	// Sequence locators persist exact physical row numbers.  The ordinary
	// columnar compactor rewrites surviving tuples at fresh row numbers, so it
	// is never legal for a registered fragment relation.  Physical tail
	// truncation below remains allowed because it preserves every live stripe,
	// row number, and reserved-row-number watermark.
	bool stripesTruncated = locator_pinned_fragment
		? false
		: TruncateAndCombineBioseqColumnStripes(rel, elevel);
	if (locator_pinned_fragment) {
		bioseq_log_pg(elevel,
			"\"{}\": skipping row-moving compaction for locator-pinned sequence fragments",
			RelationGetRelationName(rel));
	}

	/*
	 * If we didn't truncate and combine tail stripes we could have
	 * stituation where we need to truncate storage at the end.
	 */
	if (!stripesTruncated)
	{

		/*
		* Due to the AccessExclusive lock there's no danger that
		* new stripes be added beyond highestPhysicalAddress while
		* we're truncating.
		*/
		uint64_t newDataReservation = std::max<uint64_t>(GetHighestUsedAddress(rel->rd_locator) + 1,
										BioseqColumnFirstLogicalOffset);

		BlockNumber old_rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

		if (!BioseqColumnStorageTruncate(rel, newDataReservation))
		{
			UnlockRelation(rel, AccessExclusiveLock);
			return;
		}

		BlockNumber new_rel_pages = smgrnblocks(rel->rd_smgr, MAIN_FORKNUM);

		bioseq_report_pg_lazy(
			elevel,
			[elevel,
			 rel,
			 old_rel_pages,
			 new_rel_pages,
			 &ru0] {
			bioseq_report_pg(elevel)
				.msg("\"{}\": truncated {} to {} pages",
					 RelationGetRelationName(rel),
					 old_rel_pages, new_rel_pages)
				.detail("{}", pg_rusage_show(&ru0));
		});
	}

	/*
	 * We can release the exclusive lock as soon as we have truncated.
	 * Other backends can't safely access the relation until they have
	 * processed the smgr invalidation that smgrtruncate sent out ... but
	 * that should happen as part of standard invalidation processing once
	 * they acquire lock on the relation.
	 */
	UnlockRelation(rel, AccessExclusiveLock);
}

/*
 * ConditionalLockRelationWithTimeout tries to acquire a relation lock until
 * it either succeeds or timesout. It doesn't enter wait queue and instead it
 * sleeps between lock tries.
 *
 * This is based on the lock loop in lazy_truncate_heap().
 */
static bool
ConditionalLockRelationWithTimeout(Relation rel, LOCKMODE lockMode, int timeout,
								   int retryInterval, bool acquire)
{
	int lock_retry = 0;

	while (true)
	{
		if (ConditionalLockRelation(rel, lockMode))
		{
			break;
		}

		/*
		 * Check for interrupts while trying to (re-)acquire the lock
		 */
		CHECK_FOR_INTERRUPTS();

		if (!acquire && (++lock_retry > (timeout / retryInterval)))
		{
			return false;
		}

		pg_usleep(retryInterval * 1000L);
	}

	return true;
}


static bool
bioseq_scan_analyze_next_block(TableScanDesc scan,
								 ReadStream *stream)
{
	/*
	 * Our access method is not pages based, i.e. tuples are not confined
	 * to page boundaries. Present the relation as one logical block so
	 * acquire_sample_rows() scans it once, then terminates.
	 */
	const auto bioseq_scan =
		reinterpret_cast<BioseqColumnScanDesc>(scan);
	return bioseq_scan->cs_readState == nullptr;
}


static bool
bioseq_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
								 double *liverows, double *deadrows,
								 TupleTableSlot *slot)
{
	/* Disable the page cache while ANALYZE collects sample rows. */
	PageCacheDisableScope cache_guard{bioseq::settings::enable_column_cache};

	/*
	 * Currently we don't do anything smart to reduce number of rows returned
	 * for ANALYZE. The TableAM API's ANALYZE functions are designed for page
	 * based access methods where it chooses random pages, and then reads
	 * tuples from those pages.
	 *
	 * We could do something like that here by choosing sample stripes or chunks,
	 * but getting that correct might need quite some work. Since bioseq_fdw's
	 * ANALYZE scanned all rows, as a starter we do the same here and scan all
	 * rows.
	 */
	if (bioseq_getnextslot(scan, ForwardScanDirection, slot))
	{
		(*liverows)++;
		return true;
	}

	return false;
}


/** Replace only locator stubs before generic predicate/index expression code. */
static void
promote_index_source_locators(
	Relation relation,
	TupleTableSlot *slot,
	bioseq::storage::pg_sequence_source_provider *provider,
	MemoryContext resultContext)
{
	if (provider == nullptr)
		return;
	const auto &amc = bioseq::storage::bioseq_am_cache_for(relation);
	for (const std::int16_t attno : amc.seq_attnos)
	{
		const int index = static_cast<int>(attno) - 1;
		if (index < 0 || index >= slot->tts_tupleDescriptor->natts
			|| slot->tts_isnull[index])
			continue;
		auto *header = reinterpret_cast<const bioseq::pg_types::seq *>(
			DatumGetPointer(slot->tts_values[index]));
		if (!bioseq::storage::is_locator_stub(header))
			continue;
		slot->tts_values[index] = run_index_source_cpp_checked(
			"bioseq expression-index source promotion failed",
			[provider, attno, slot, index, resultContext] {
				return
				bioseq::storage::promote_pg_sequence_locator(
					provider, attno, slot->tts_values[index], resultContext);
			});
	}
}

[[nodiscard]] static bioseq::storage::pg_sequence_source_provider *
create_index_source_provider(Relation relation, Snapshot snapshot)
{
	return run_index_source_cpp_checked(
		"bioseq expression-index source provider failed",
		[relation, snapshot] {
			return bioseq::storage::create_standalone_pg_sequence_source_provider(
				relation, snapshot);
		});
}


static double
bioseq_index_build_range_scan(Relation bioseqColumnRelation,
								Relation indexRelation,
								IndexInfo *indexInfo,
								bool allow_sync,
								bool anyvisible,
								bool progress,
								BlockNumber start_blockno,
								BlockNumber numblocks,
								IndexBuildCallback callback,
								void *callback_state,
								TableScanDesc scan)
{
	if (start_blockno != 0 || numblocks != InvalidBlockNumber)
	{
		/*
		 * BioseqColumn utility hook already errors out for BRIN indexes on columnar
		 * tables, but be on the safe side.
		 */
		bioseq_log<ERR, PG>( "BRIN indexes on columnar tables are not supported");
	}

	if (scan)
	{
		/*
		 * Parallel scans on columnar tables are already discardad by
		 * BioseqColumnGetRelationInfoHook but be on the safe side.
		 */
		bioseq_log<ERR, PG>( "parallel scans on columnar are not supported");
	}

	/* Disable the page cache for the duration of the index build. */
	PageCacheDisableScope cache_guard{bioseq::settings::enable_column_cache};

	/*
	 * Columnar tuples have no dead-row form, so outside bootstrap an explicitly
	 * registered transaction snapshot is both complete and authoritative for
	 * locator promotion.  Keeping the table scan, source catalog and fragments
	 * on this exact snapshot prevents expression-index code from consulting a
	 * different active snapshot.
	 */
	Snapshot snapshot = { 0 };
	bool snapshotRegisteredByUs = false;

	/*
	 * For serial index build, we begin our own scan. We may also need to
	 * register a snapshot whose lifetime is under our direct control.
	 */
	if (!IsBootstrapProcessingMode())
	{
		snapshot = RegisterSnapshot(GetTransactionSnapshot());
		snapshotRegisteredByUs = true;
	}
	else
	{
		snapshot = SnapshotAny;
	}

	int nkeys = 0;
	ScanKeyData *scanKey = NULL;
	bool allowAccessStrategy = true;
	scan = table_beginscan_strat(bioseqColumnRelation, snapshot, nkeys, scanKey,
								 allowAccessStrategy, allow_sync);
	bioseq::storage::pg_sequence_source_provider *volatile sourceProvider = nullptr;
	if (snapshotRegisteredByUs)
	{
		auto *bioseqScan = reinterpret_cast<BioseqColumnScanDesc>(scan);
		bioseqScan->defer_seq_hydration = true;
		sourceProvider =
			create_index_source_provider(bioseqColumnRelation, snapshot);
	}

	if (progress)
	{
		BioseqColumnReportTotalVirtualBlocks(bioseqColumnRelation, snapshot,
										 PROGRESS_SCAN_BLOCKS_TOTAL);
	}

	/*
	 * Set up execution state for predicate, if any.
	 * Note that this is only useful for partial indexes.
	 */
	EState *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	econtext->ecxt_scantuple = table_slot_create(bioseqColumnRelation, NULL);
	ExprState *predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	double volatile reltuples = 0;
	PG_TRY();
	{
		reltuples = BioseqColumnReadRowsIntoIndex(
			scan, indexRelation, indexInfo, progress, callback,
			callback_state, estate, predicate,
			sourceProvider);
		ResetExprContext(econtext);
		if (sourceProvider != nullptr)
		{
			bioseq::storage::close_pg_sequence_source_provider(
				sourceProvider,
				/*is_commit=*/true);
			sourceProvider = nullptr;
		}
	}
	PG_CATCH();
	{
		if (sourceProvider != nullptr)
		{
			bioseq::storage::close_pg_sequence_source_provider(
				sourceProvider,
				/*is_commit=*/false);
			sourceProvider = nullptr;
		}
		PG_RE_THROW();
	}
	PG_END_TRY();
	table_endscan(scan);

	if (progress)
	{
		/* report the last "virtual" block as "done" */
		BioseqColumnReportTotalVirtualBlocks(bioseqColumnRelation, snapshot,
										 PROGRESS_SCAN_BLOCKS_DONE);
	}

	if (snapshotRegisteredByUs)
	{
		UnregisterSnapshot(snapshot);
	}

	ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
	FreeExecutorState(estate);
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;

	return static_cast<double>(reltuples);
}


/*
 * BioseqColumnReportTotalVirtualBlocks reports progress for index build based on
 * number of "virtual" blocks that given relation has.
 * "progressArrIndex" argument determines which entry in st_progress_param
 * array should be updated. In this case, we only expect PROGRESS_SCAN_BLOCKS_TOTAL
 * or PROGRESS_SCAN_BLOCKS_DONE to specify whether we want to report calculated
 * number of blocks as "done" or as "total" number of "virtual" blocks to scan.
 */
static void
BioseqColumnReportTotalVirtualBlocks(Relation relation, Snapshot snapshot,
								 int progressArrIndex)
{
	/*
	 * Indeed, columnar tables might have gaps between row numbers, e.g
	 * due to aborted transactions etc. Also, ItemPointer BlockNumber's
	 * for columnar tables don't actually correspond to actual disk blocks
	 * as in heapAM. For this reason, we call them as "virtual" blocks. At
	 * the moment, we believe it is better to report our progress based on
	 * this "virtual" block concept instead of doing nothing.
	 */
	Assert(progressArrIndex == PROGRESS_SCAN_BLOCKS_TOTAL ||
		   progressArrIndex == PROGRESS_SCAN_BLOCKS_DONE);
	BlockNumber nvirtualBlocks =
		BioseqColumnGetNumberOfVirtualBlocks(relation, snapshot);
	pgstat_progress_update_param(progressArrIndex, nvirtualBlocks);
}


/*
 * BioseqColumnGetNumberOfVirtualBlocks returns total number of "virtual" blocks
 * that given columnar table has based on based on ItemPointer BlockNumber's.
 */
static BlockNumber
BioseqColumnGetNumberOfVirtualBlocks(Relation relation, Snapshot snapshot)
{
	ItemPointerData highestItemPointer =
		BioseqColumnGetHighestItemPointer(relation, snapshot);
	if (!ItemPointerIsValid(&highestItemPointer))
	{
		/* table is empty according to our snapshot */
		return 0;
	}

	/*
	 * Since BlockNumber is 0-based, increment it by 1 to find the total
	 * number of "virtual" blocks.
	 */
	return ItemPointerGetBlockNumber(&highestItemPointer) + 1;
}


/*
 * BioseqColumnGetHighestItemPointer returns ItemPointerData for the tuple with
 * highest tid for given relation.
 * If given relation is empty, then returns invalid item pointer.
 */
static ItemPointerData
BioseqColumnGetHighestItemPointer(Relation relation, Snapshot snapshot)
{
	StripeMetadata *stripeWithHighestRowNumber =
		FindStripeWithHighestRowNumber(relation, snapshot);
	if (stripeWithHighestRowNumber == NULL ||
		StripeGetHighestRowNumber(stripeWithHighestRowNumber) == 0)
	{
		/* table is empty according to our snapshot */
		ItemPointerData invalidItemPtr;
		ItemPointerSetInvalid(&invalidItemPtr);
		return invalidItemPtr;
	}

	uint64_t highestRowNumber = StripeGetHighestRowNumber(stripeWithHighestRowNumber);
	return row_number_to_tid(highestRowNumber);
}


/*
 * BioseqColumnReadRowsIntoIndex builds indexRelation tuples by reading the
 * actual relation based on given "scan" and returns number of tuples
 * scanned to build the indexRelation.
 */
static double
BioseqColumnReadRowsIntoIndex(TableScanDesc scan, Relation indexRelation,
						  IndexInfo *indexInfo, bool progress,
						  IndexBuildCallback indexCallback,
						  void *indexCallbackState, EState *estate,
						  ExprState *predicate,
						  bioseq::storage::pg_sequence_source_provider *sourceProvider)
{
	double reltuples = 0;

	BlockNumber lastReportedBlockNumber = InvalidBlockNumber;

	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = econtext->ecxt_scantuple;
	while (bioseq_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();

		BlockNumber currentBlockNumber = ItemPointerGetBlockNumber(&slot->tts_tid);
		if (progress && lastReportedBlockNumber != currentBlockNumber)
		{
			/*
			 * bioseq_getnextslot guarantees that returned tuple will
			 * always have a greater ItemPointer than the ones we fetched
			 * before, so we directly use BlockNumber to report our progress.
			 */
			Assert(lastReportedBlockNumber == InvalidBlockNumber ||
				   currentBlockNumber >= lastReportedBlockNumber);
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
										 currentBlockNumber);
			lastReportedBlockNumber = currentBlockNumber;
		}

		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		promote_index_source_locators(
			scan->rs_rd, slot, sourceProvider,
			econtext->ecxt_per_tuple_memory);

		if (predicate != NULL && !ExecQual(predicate, econtext))
		{
			/* for partial indexes, discard tuples that don't satisfy the predicate */
			continue;
		}

		Datum indexValues[INDEX_MAX_KEYS];
		bool indexNulls[INDEX_MAX_KEYS];
		ItemPointerData itemPointerData = slot->tts_tid;

		/* currently, columnar tables can't have dead tuples */
		bool tupleIsAlive = true;
		const auto previous_scope = exchange_slice_materialization_scope({
			.attachment = nullptr,
			.boundary_id = 0u,
			.producer_value_id = 0u,
			.mechanism = materialization_mechanism::packed_full,
			.reason = materialization_reason::index_tuple,
			.local_sink = nullptr,
		});
		PG_TRY();
		{
			FormIndexDatum(
				indexInfo, slot, estate, indexValues, indexNulls);
			indexCallback(
				indexRelation, &itemPointerData, indexValues, indexNulls,
				tupleIsAlive, indexCallbackState);
			restore_slice_materialization_scope(previous_scope);
		}
		PG_CATCH();
		{
			restore_slice_materialization_scope(previous_scope);
			PG_RE_THROW();
		}
		PG_END_TRY();

		reltuples++;
	}

	return reltuples;
}


static void
bioseq_index_validate_scan(Relation bioseqColumnRelation,
							 Relation indexRelation,
							 IndexInfo *indexInfo,
							 Snapshot snapshot,
							 ValidateIndexState *
							 validateIndexState)
{
	BioseqColumnReportTotalVirtualBlocks(bioseqColumnRelation, snapshot,
									 PROGRESS_SCAN_BLOCKS_TOTAL);

	/*
	 * Set up execution state for predicate, if any.
	 * Note that this is only useful for partial indexes.
	 */
	EState *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	econtext->ecxt_scantuple = table_slot_create(bioseqColumnRelation, NULL);
	ExprState *predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	int nkeys = 0;
	ScanKeyData *scanKey = NULL;
	bool allowAccessStrategy = true;
	bool allowSyncScan = false;
	TableScanDesc scan = table_beginscan_strat(bioseqColumnRelation, snapshot, nkeys, scanKey,
											   allowAccessStrategy, allowSyncScan);
	bioseq::storage::pg_sequence_source_provider *volatile sourceProvider = nullptr;
	if (snapshot != nullptr && IsMVCCSnapshot(snapshot)
		&& snapshot->regd_count > 0)
	{
		auto *bioseqScan = reinterpret_cast<BioseqColumnScanDesc>(scan);
		bioseqScan->defer_seq_hydration = true;
		sourceProvider =
			create_index_source_provider(bioseqColumnRelation, snapshot);
	}

	PG_TRY();
	{
		BioseqColumnReadMissingRowsIntoIndex(
			scan, indexRelation, indexInfo, estate, predicate,
			validateIndexState, sourceProvider);
		ResetExprContext(econtext);
		if (sourceProvider != nullptr)
		{
			bioseq::storage::close_pg_sequence_source_provider(
				sourceProvider, /*is_commit=*/true);
			sourceProvider = nullptr;
		}
	}
	PG_CATCH();
	{
		if (sourceProvider != nullptr)
		{
			bioseq::storage::close_pg_sequence_source_provider(
				sourceProvider, /*is_commit=*/false);
			sourceProvider = nullptr;
		}
		PG_RE_THROW();
	}
	PG_END_TRY();

	table_endscan(scan);

	/* report the last "virtual" block as "done" */
	BioseqColumnReportTotalVirtualBlocks(bioseqColumnRelation, snapshot,
									 PROGRESS_SCAN_BLOCKS_DONE);

	ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
	FreeExecutorState(estate);
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
}


/*
 * BioseqColumnReadMissingRowsIntoIndex inserts the tuples that are not in
 * the index yet by reading the actual relation based on given "scan".
 */
static void
BioseqColumnReadMissingRowsIntoIndex(TableScanDesc scan, Relation indexRelation,
								 IndexInfo *indexInfo, EState *estate,
								 ExprState *predicate,
								 ValidateIndexState *validateIndexState,
								 bioseq::storage::pg_sequence_source_provider *sourceProvider)
{
	BlockNumber lastReportedBlockNumber = InvalidBlockNumber;

	bool indexTupleSortEmpty = false;
	ItemPointerData indexedItemPointerData;
	ItemPointerSetInvalid(&indexedItemPointerData);

	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = econtext->ecxt_scantuple;
	while (bioseq_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();

		ItemPointer bioseqColumnItemPointer = &slot->tts_tid;
		BlockNumber currentBlockNumber = ItemPointerGetBlockNumber(bioseqColumnItemPointer);
		if (lastReportedBlockNumber != currentBlockNumber)
		{
			/*
			 * bioseq_getnextslot guarantees that returned tuple will
			 * always have a greater ItemPointer than the ones we fetched
			 * before, so we directly use BlockNumber to report our progress.
			 */
			Assert(lastReportedBlockNumber == InvalidBlockNumber ||
				   currentBlockNumber >= lastReportedBlockNumber);
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
										 currentBlockNumber);
			lastReportedBlockNumber = currentBlockNumber;
		}

		validateIndexState->htups += 1;

		if (!indexTupleSortEmpty &&
			(!ItemPointerIsValid(&indexedItemPointerData) ||
			 ItemPointerCompare(&indexedItemPointerData, bioseqColumnItemPointer) < 0))
		{
			/*
			 * Skip indexed item pointers until we find or pass the current
			 * columnar relation item pointer.
			 */
			indexedItemPointerData =
				TupleSortSkipSmallerItemPointers(validateIndexState->tuplesort,
												 bioseqColumnItemPointer);
			indexTupleSortEmpty = !ItemPointerIsValid(&indexedItemPointerData);
		}

		if (!indexTupleSortEmpty &&
			ItemPointerCompare(&indexedItemPointerData, bioseqColumnItemPointer) == 0)
		{
			/* tuple is already covered by the index, skip */
			continue;
		}
		Assert(indexTupleSortEmpty ||
			   ItemPointerCompare(&indexedItemPointerData, bioseqColumnItemPointer) > 0);

		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		promote_index_source_locators(
			scan->rs_rd, slot, sourceProvider,
			econtext->ecxt_per_tuple_memory);

		if (predicate != NULL && !ExecQual(predicate, econtext))
		{
			/* for partial indexes, discard tuples that don't satisfy the predicate */
			continue;
		}

		Datum indexValues[INDEX_MAX_KEYS];
		bool indexNulls[INDEX_MAX_KEYS];

		Relation bioseqColumnRelation = scan->rs_rd;
		IndexUniqueCheck indexUniqueCheck =
			indexInfo->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO;
		const auto previous_scope = exchange_slice_materialization_scope({
			.attachment = nullptr,
			.boundary_id = 0u,
			.producer_value_id = 0u,
			.mechanism = materialization_mechanism::packed_full,
			.reason = materialization_reason::index_tuple,
			.local_sink = nullptr,
		});
		PG_TRY();
		{
			FormIndexDatum(
				indexInfo, slot, estate, indexValues, indexNulls);
			index_insert_compat(
				indexRelation, indexValues, indexNulls,
				bioseqColumnItemPointer, bioseqColumnRelation,
				indexUniqueCheck, false, indexInfo);
			restore_slice_materialization_scope(previous_scope);
		}
		PG_CATCH();
		{
			restore_slice_materialization_scope(previous_scope);
			PG_RE_THROW();
		}
		PG_END_TRY();

		validateIndexState->tups_inserted += 1;
	}
}


/*
 * TupleSortSkipSmallerItemPointers iterates given tupleSort until finding an
 * ItemPointer that is greater than or equal to given targetItemPointer and
 * returns that ItemPointer.
 * If such an ItemPointer does not exist, then returns invalid ItemPointer.
 *
 * Note that this function assumes given tupleSort doesn't have any NULL
 * Datum's.
 */
static ItemPointerData
TupleSortSkipSmallerItemPointers(Tuplesortstate *tupleSort, ItemPointer targetItemPointer)
{
	ItemPointerData tsItemPointerData;
	ItemPointerSetInvalid(&tsItemPointerData);

	while (!ItemPointerIsValid(&tsItemPointerData) ||
		   ItemPointerCompare(&tsItemPointerData, targetItemPointer) < 0)
	{
		bool forwardDirection = true;
		Datum *abbrev = NULL;
		Datum tsDatum;
		bool tsDatumIsNull;
		if (!tuplesort_getdatum_compat(tupleSort, forwardDirection, false,
									   &tsDatum, &tsDatumIsNull, abbrev))
		{
			ItemPointerSetInvalid(&tsItemPointerData);
			break;
		}

		Assert(!tsDatumIsNull);
		itemptr_decode(&tsItemPointerData, DatumGetInt64(tsDatum));

#ifndef USE_FLOAT8_BYVAL

		/*
		 * If int8 is pass-by-ref, we need to free Datum memory.
		 * See tuplesort_getdatum function's comment.
		 */
		pfree(DatumGetPointer(tsDatum));
#endif
	}

	return tsItemPointerData;
}


static uint64
bioseq_relation_size(Relation rel, ForkNumber forkNumber)
{
	uint64 nblocks = 0;

	/* InvalidForkNumber indicates returning the size for all forks */
	if (forkNumber == InvalidForkNumber)
	{
		for (int i = 0; i < MAX_FORKNUM; i++)
		{
			nblocks += smgrnblocks(RelationGetSmgr(rel), static_cast<ForkNumber>(i));
		}
	}
	else
	{
		nblocks = smgrnblocks(RelationGetSmgr(rel), forkNumber);
	}

	return nblocks * BLCKSZ;
}


static bool
bioseq_relation_needs_toast_table(Relation rel)
{
	return false;
}


static void
bioseq_estimate_rel_size(Relation rel, int32_t *attr_widths,
						   BlockNumber *pages, double *tuples,
						   double *allvisfrac)
{
	*pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
	*tuples = BioseqColumnTableRowCount(rel);

	/*
	 * Append-only, so everything is visible except in-progress or rolled-back
	 * transactions.
	 */
	*allvisfrac = 1.0;

	get_rel_data_width(rel, attr_widths);
}


static bool
bioseq_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	bioseq_log<ERR, PG>( "bioseq_scan_sample_next_block not implemented");
}


static bool
bioseq_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
								TupleTableSlot *slot)
{
	bioseq_log<ERR, PG>( "bioseq_scan_sample_next_tuple not implemented");
}


static void
BioseqColumnXactCallback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PREPARE:
		{
			// PRE_COMMIT/PRE_PREPARE below must have proved this empty. A
			// surviving move-only authority here is an internal invariant breach;
			// quarantine it without a PostgreSQL jump or destructor-bearing
			// callback frame after the terminal transaction boundary.
			quarantine_pending_speculative_sources_noexcept(event);
			break;
		}

		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		{
			DiscardWriteStateForAllRels(GetCurrentSubTransactionId(), 0);
			CleanupReadStateCache(GetCurrentSubTransactionId());
			const bool active_map_quarantined =
				speculative_source_quarantine_state_noexcept()
					.active_map_quarantined;
			if (!active_map_quarantined) {
				pending_speculative_sources().clear();
			}
			break;
		}

		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
		{
			require_no_pending_speculative_sources("pre-commit/pre-prepare");
			FlushWriteStateForAllRels(GetCurrentSubTransactionId(), 0);
			CleanupReadStateCache(GetCurrentSubTransactionId());
			break;
		}
	}
}


static void
BioseqColumnSubXactCallback(SubXactEvent event, SubTransactionId mySubid,
						SubTransactionId parentSubid, void *arg)
{
	switch (event)
	{
		case SUBXACT_EVENT_START_SUB:
		case SUBXACT_EVENT_COMMIT_SUB:
		{
			for (auto& [key, pending] : pending_speculative_sources()) {
				(void)key;
				if (pending.subtransaction_id == mySubid) {
					pending.subtransaction_id = parentSubid;
				}
			}
			break;
		}

		case SUBXACT_EVENT_ABORT_SUB:
		{
			DiscardWriteStateForAllRels(mySubid, parentSubid);
			CleanupReadStateCache(mySubid);
			std::erase_if(pending_speculative_sources(),
				[mySubid](const auto& entry) {
					return entry.second.subtransaction_id == mySubid;
				});
			break;
		}

		case SUBXACT_EVENT_PRE_COMMIT_SUB:
		{
			FlushWriteStateForAllRels(mySubid, parentSubid);
			CleanupReadStateCache(mySubid);
			break;
		}
	}
}


// DOC0322 §7.3/§7 — adapters that keep storage_metadata independent from
// bioseq_storage.  A parent flush must also flush its cached sequence-fragment
// shard states before any caller refreshes or serializes a snapshot.
static void
bioseq_flush_pending_writes_adapter(BioseqColumnWriteState* write_state)
{
	BioseqColumnFlushPendingWrites(write_state);
	if (write_state->seq_caches != nullptr)
	{
		bioseq::storage::flush_seq_cache_write_states(
			static_cast<bioseq::storage::seq_caches*>(
				write_state->seq_caches),
			GetCurrentSubTransactionId());
	}
}

static void
bioseq_seq_caches_close_adapter(void* p, bool commit)
{
	bioseq::storage::close_and_destroy_seq_caches(
		static_cast<bioseq::storage::seq_caches*>(p), commit);
}


/*
 * Get the number of chunks filtered out during the given scan.
 */
export int64_t
BioseqColumnScanChunkGroupsFiltered(BioseqColumnScanDesc bioseqColumnScanDesc)
{
	BioseqColumnReadState *readState = bioseqColumnScanDesc->cs_readState;

	/* readState is initialized lazily */
	if (readState != NULL)
	{
		return BioseqColumnReadChunkGroupsFiltered(readState);
	}
	else
	{
		return 0;
	}
}


// /*
//  * IsBioseqColumnTableAmTable returns true if relation has bioseq_tableam
//  * access method. This can be called before extension creation.
//  */
// bool
// IsBioseqColumnTableAmTable(Oid relationId)
// {
// 	bioseq_log<DBG>("[bioseq] pg: IsBioseqColumnTableAmTable() entered");
// 	if (!OidIsValid(relationId))
// 	{
// 		return false;
// 	}

// 	/*
// 	 * Lock relation to prevent it from being dropped &
// 	 * avoid race conditions.
// 	 */
// 	Relation rel = safe_relation_open(relationId, AccessShareLock);
// 	bool result = rel->rd_tableam == GetBioseqColumnTableAmRoutine();
// 	relation_close(rel, NoLock);

// 	return result;
// }


export const TableAmRoutine bioseq_am_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = bioseq_slot_callbacks,

	.scan_begin = bioseq_beginscan,
	.scan_end = bioseq_endscan,
	.scan_rescan = bioseq_rescan,
	.scan_getnextslot = bioseq_getnextslot,

	.parallelscan_estimate = bioseq_parallelscan_estimate,
	.parallelscan_initialize = bioseq_parallelscan_initialize,
	.parallelscan_reinitialize = bioseq_parallelscan_reinitialize,

	.index_fetch_begin = bioseq_index_fetch_begin,
	.index_fetch_reset = bioseq_index_fetch_reset,
	.index_fetch_end = bioseq_index_fetch_end,
	.index_fetch_tuple = bioseq_index_fetch_tuple,

	.tuple_fetch_row_version = bioseq_fetch_row_version,
	.tuple_tid_valid = bioseq_tuple_tid_valid,
	.tuple_get_latest_tid = bioseq_get_latest_tid,
	.tuple_satisfies_snapshot = bioseq_tuple_satisfies_snapshot,
	.index_delete_tuples = bioseq_index_delete_tuples,

	.tuple_insert = bioseq_tuple_insert,
	.tuple_insert_speculative = bioseq_tuple_insert_speculative,
	.tuple_complete_speculative = bioseq_tuple_complete_speculative,
	.multi_insert = bioseq_multi_insert,
	.tuple_delete = bioseq_tuple_delete,
	.tuple_update = bioseq_tuple_update,
	.tuple_lock = bioseq_tuple_lock,
	.finish_bulk_insert = bioseq_finish_bulk_insert,

	.relation_set_new_filelocator = bioseq_relation_set_new_filenode,
	.relation_nontransactional_truncate = bioseq_relation_nontransactional_truncate,
	.relation_copy_data = bioseq_relation_copy_data,
	.relation_copy_for_cluster = bioseq_relation_copy_for_cluster,
	.relation_vacuum = bioseq_vacuum_rel,
	.scan_analyze_next_block = bioseq_scan_analyze_next_block,
	.scan_analyze_next_tuple = bioseq_scan_analyze_next_tuple,
	.index_build_range_scan = bioseq_index_build_range_scan,
	.index_validate_scan = bioseq_index_validate_scan,

	.relation_size = bioseq_relation_size,
	.relation_needs_toast_table = bioseq_relation_needs_toast_table,

	.relation_estimate_size = bioseq_estimate_rel_size,

/*
 * PG-18 dropped the per-block callback (scan_bitmap_next_block) from the
 * TableAmRoutine when bitmap-heap-scan was reorganised; only the per-tuple
 * callback remains. Keep the PG-17 field for backwards compatibility.
 */
#if PG_VERSION_NUM < PG_VERSION_18
	.scan_bitmap_next_block = NULL,
#endif
	.scan_bitmap_next_tuple = NULL,
	.scan_sample_next_block = bioseq_scan_sample_next_block,
	.scan_sample_next_tuple = bioseq_scan_sample_next_tuple
};


export const TableAmRoutine *
GetBioseqColumnTableAmRoutine(void)
{
	bioseq_log<DBG>("[bioseq] pg: GetBioseqColumnTableAmRoutine() entered");
	return &bioseq_am_methods;
}

export namespace bioseq::table::sql_routines {

/** @brief Returns the table access-method handler for Bioseq tables. */
[[bioseq_sql::function({
	.schema = bioseq::sql::fixed_schema<"bioseq">,
	.create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::access_method]]
[[bioseq_sql::am_catalog_dependencies({
	.roots = bioseq::sql::entities<
		bioseq::storage::metadata::catalog_rows::chunk,
		bioseq::storage::metadata::catalog_rows::chunk_group,
		bioseq::storage::metadata::catalog_rows::options,
		bioseq::storage::metadata::catalog_rows::stripe,
		bioseq::storage::catalog_rows::sequence_source_version>(
			bioseq::metadata::sql_entities::storageid_seq,
			bioseq::storage::sql_entities::sequence_source_epoch_seq,
			bioseq::storage::indexing::sql_entities::
				minimizer_generation_id_seq),
})]]
[[bioseq_sql::result({
	.sql_type = bioseq::sql::pg::table_am_handler,
})]]
/**
 * @brief Returns the table access-method handler for Bioseq tables.
 */
const TableAmRoutine* bioseq_handler(
		[[bioseq_sql::param({
			.sql_type = bioseq::sql::pg::internal,
		})]]
		std::optional<
			bioseq::pg_utils::table_am_handler_context>
			handler_context) {
	static_cast<void>(handler_context);
	bioseq_log<DBG>("[bioseq] pg: bioseq_handler() entered");
	return &bioseq_am_methods;
}

}  // namespace bioseq::table::sql_routines


export void
bioseq_tableam_init()
{
	bioseq_log<DBG>("[bioseq] pg: bioseq_tableam_init() entered");

	// DOC0322 §7.4 — publish the bioseq TableAmRoutine identity to the
	// bioseq_column_tam leaf so IsBioseqColumnTableAmTable can recognise bioseq
	// tables without depending back on this module (dependency inversion).
	register_bioseq_am_routine(GetBioseqColumnTableAmRoutine());

	// DOC0322 §7.3 — wire the inverted write/read-state dependencies. The
	// storage_metadata write/read-state partitions hold function-pointer slots
	// (default nullptr); register the bioseq_table_io / bioseq_storage callees
	// here so storage_metadata stays free of those module edges.
	register_write_state_hooks(&BioseqColumnBeginWrite, &BioseqColumnEndWrite,
							   &bioseq_flush_pending_writes_adapter,
							   &ContainsPendingWrites,
							   &bioseq_seq_caches_close_adapter);
	register_read_state_hooks(&BioseqColumnEndRead);
	BioseqColumnTableSetOptions_hook_type **BioseqColumnTableSetOptions_hook_ptr =
		reinterpret_cast<BioseqColumnTableSetOptions_hook_type **>(find_rendezvous_variable(
			BIOSEQ_COLUMN_SETOPTIONS_HOOK_SYM));
	*BioseqColumnTableSetOptions_hook_ptr = &BioseqColumnTableSetOptions_hook;

	RegisterXactCallback(BioseqColumnXactCallback, NULL);
	RegisterSubXactCallback(BioseqColumnSubXactCallback, NULL);

	InstallBioseqColumnTableHooks();

	// DOC0322 §7.5 — bioseq_customscan_init() now runs from bioseq_init (after
	// bioseq_tableam_init), inverting the former :access_method -> :custom_scan
	// edge so the two bioseq_table partitions form a DAG.

	BioseqColumnInitTupleSlotOps();
}


export namespace bioseq::table::sql_routines {

[[bioseq_sql::function({
	.schema = bioseq::sql::pg_catalog,
	.create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Sets selected storage options on a Bioseq table; NULL options are unchanged. */
void alter_bioseq_table_set(
		[[bioseq_sql::param({
			.sql_type = bioseq::sql::pg::regclass,
		})]]
		std::optional<Oid> table_name,
		std::optional<std::int32_t> chunk_group_row_limit =
			std::nullopt,
		std::optional<std::int32_t> stripe_row_limit =
			std::nullopt,
		[[bioseq_sql::param({
			.sql_type = bioseq::sql::pg::name,
		})]]
		std::optional<NameData> compression = std::nullopt,
		std::optional<std::int32_t> compression_level =
			std::nullopt) {
	const bool change_requested =
		chunk_group_row_limit.has_value()
		|| stripe_row_limit.has_value()
		|| compression.has_value()
		|| compression_level.has_value();
	if (!change_requested) return;
	if (!table_name.has_value()) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = "table_name must not be NULL"});
	}
	const Oid relation_id = *table_name;
	Relation relation = bioseq::pg_utils::invoke_pg_as_cpp(
		[relation_id] {
			return table_open(relation_id, AccessExclusiveLock);
		});
	const sql_relation_lock relation_lock{
		relation, NoLock, true};
	require_bioseq_table(relation, relation_id);
	require_relation_owner(relation_id);

	auto maybe_options = bioseq::pg_utils::invoke_pg_as_cpp(
		[relation_id] {
			return read_bioseq_column_options(relation_id);
		});
	if (!maybe_options)
	{
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = "unable to read current options for table"});
	}
	BioseqColumnOptions options = *maybe_options;

	if (chunk_group_row_limit.has_value())
	{
		options.chunkRowCount = *chunk_group_row_limit;
		if (options.chunkRowCount < CHUNK_ROW_COUNT_MINIMUM ||
			options.chunkRowCount > CHUNK_ROW_COUNT_MAXIMUM)
		{
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{
					.message =
						"chunk group row count limit out of range",
					.hint =
						"chunk group row count limit must be between "
						+ std::to_string(CHUNK_ROW_COUNT_MINIMUM)
						+ " and "
						+ std::to_string(CHUNK_ROW_COUNT_MAXIMUM),
				});
		}
	}

	if (stripe_row_limit.has_value())
	{
		options.stripeRowCount = *stripe_row_limit;
		if (options.stripeRowCount < STRIPE_ROW_COUNT_MINIMUM ||
			options.stripeRowCount > STRIPE_ROW_COUNT_MAXIMUM)
		{
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{
					.message = "stripe row count limit out of range",
					.hint =
						"stripe row count limit must be between "
						+ std::to_string(STRIPE_ROW_COUNT_MINIMUM)
						+ " and "
						+ std::to_string(STRIPE_ROW_COUNT_MAXIMUM),
				});
		}
	}

	if (compression.has_value())
	{
		options.compressionType =
			ParseCompressionType(NameStr(*compression));
		if (options.compressionType == COMPRESSION_TYPE_INVALID)
		{
			const char* const compression_name =
				NameStr(*compression);
			const char* const quoted_name =
				bioseq::pg_utils::invoke_pg_as_cpp(
					[compression_name] {
						return quote_identifier(
							compression_name);
					});
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{
					.message =
						"unknown compression type for columnar table: "
						+ std::string{quoted_name},
				});
		}
	}

	if (compression_level.has_value())
	{
		options.compressionLevel = *compression_level;
		if (options.compressionLevel < COMPRESSION_LEVEL_MIN ||
			options.compressionLevel > COMPRESSION_LEVEL_MAX)
		{
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{
					.message = "compression level out of range",
					.hint =
						"compression level must be between "
						+ std::to_string(COMPRESSION_LEVEL_MIN)
						+ " and "
						+ std::to_string(COMPRESSION_LEVEL_MAX),
				});
		}
	}

	if (BioseqColumnTableSetOptions_hook != NULL)
	{
		bioseq::pg_utils::invoke_pg_as_cpp(
			[relation_id, options] {
				BioseqColumnTableSetOptions_hook(
					relation_id, options);
			});
	}

	bioseq::pg_utils::invoke_pg_as_cpp(
		[relation_id, &options] {
			SetBioseqColumnOptions(relation_id, &options);
		});
}

}  // namespace bioseq::table::sql_routines

export namespace bioseq::table::sql_routines {

[[bioseq_sql::function({
	.schema = bioseq::sql::pg_catalog,
	.create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Resets selected Bioseq table options to their system defaults. */
void alter_bioseq_table_reset(
		[[bioseq_sql::param({
			.sql_type = bioseq::sql::pg::regclass,
		})]]
		std::optional<Oid> table_name,
		std::optional<bool> chunk_group_row_limit = false,
		std::optional<bool> stripe_row_limit = false,
		std::optional<bool> compression = false,
		std::optional<bool> compression_level = false) {
	const bool reset_requested =
		chunk_group_row_limit.value_or(false)
		|| stripe_row_limit.value_or(false)
		|| compression.value_or(false)
		|| compression_level.value_or(false);
	if (!reset_requested) return;
	if (!table_name.has_value()) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = "table_name must not be NULL"});
	}
	const Oid relation_id = *table_name;
	Relation relation = bioseq::pg_utils::invoke_pg_as_cpp(
		[relation_id] {
			return table_open(relation_id, AccessExclusiveLock);
		});
	const sql_relation_lock relation_lock{
		relation, NoLock, true};
	require_bioseq_table(relation, relation_id);
	require_relation_owner(relation_id);

	auto maybe_options = bioseq::pg_utils::invoke_pg_as_cpp(
		[relation_id] {
			return read_bioseq_column_options(relation_id);
		});
	if (!maybe_options)
	{
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = "unable to read current options for table"});
	}
	BioseqColumnOptions options = *maybe_options;

	if (chunk_group_row_limit.value_or(false))
	{
		options.chunkRowCount = bioseq::settings::chunk_group_row_limit;
	}

	if (stripe_row_limit.value_or(false))
	{
		options.stripeRowCount = bioseq::settings::stripe_row_limit;
	}

	if (compression.value_or(false))
	{
		options.compressionType = static_cast<CompressionType>(static_cast<int>(bioseq::settings::compression.value()));
	}

	if (compression_level.value_or(false))
	{
		options.compressionLevel = bioseq::settings::compression_level;
	}

	if (BioseqColumnTableSetOptions_hook != NULL)
	{
		bioseq::pg_utils::invoke_pg_as_cpp(
			[relation_id, options] {
				BioseqColumnTableSetOptions_hook(
					relation_id, options);
			});
	}

	bioseq::pg_utils::invoke_pg_as_cpp(
		[relation_id, &options] {
			SetBioseqColumnOptions(relation_id, &options);
		});
}

}  // namespace bioseq::table::sql_routines

export namespace bioseq::table::sql_routines {

[[bioseq_sql::function({
	.schema = bioseq::sql::fixed_schema<"bioseq_internal">,
	.create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Upgrades Bioseq storage metadata when the relation uses an older layout. */
void upgrade_bioseq_storage(
		[[bioseq_sql::param({
			.sql_type = bioseq::sql::pg::regclass,
		})]]
		Oid rel) {
	Relation relation = bioseq::pg_utils::invoke_pg_as_cpp(
		[rel] {
			return table_open(rel, AccessExclusiveLock);
		});
	const sql_relation_lock relation_lock{
		relation, AccessExclusiveLock, true};
	require_bioseq_table(relation, rel);
	bioseq::pg_utils::invoke_pg_as_cpp(
		[relation] {
			BioseqColumnStorageUpdateIfNeeded(relation, true);
		});
}

[[bioseq_sql::function({
	.schema = bioseq::sql::fixed_schema<"bioseq_internal">,
	.create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Downgrades Bioseq storage metadata when the relation uses a newer layout. */
void downgrade_bioseq_storage(
		[[bioseq_sql::param({
			.sql_type = bioseq::sql::pg::regclass,
		})]]
		Oid rel) {
	Relation relation = bioseq::pg_utils::invoke_pg_as_cpp(
		[rel] {
			return table_open(rel, AccessExclusiveLock);
		});
	const sql_relation_lock relation_lock{
		relation, AccessExclusiveLock, true};
	require_bioseq_table(relation, rel);
	bioseq::pg_utils::invoke_pg_as_cpp(
		[relation] {
			BioseqColumnStorageUpdateIfNeeded(relation, false);
		});
}

}  // namespace bioseq::table::sql_routines
