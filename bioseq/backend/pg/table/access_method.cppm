/**
 * @file
 * @brief PostgreSQL table-access callbacks for Bioseq column relations.
 *
 * Implements scan, tuple mutation, index fetch/build, vacuum, analyze, and
 * relation lifecycle callbacks while delegating storage work to column I/O.
 */
module;
#include <bioseq/pg_compat.h>

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

export module bioseq.postgres.table:access_method;

import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.compression;
import bioseq.common.logging;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.metadata.sql_entities;
import bioseq.postgres.storage.metadata.sql_rows;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.table.types;
import bioseq.postgres.configuration.core;
import bioseq.postgres.table.io;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.indexing.admin;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.chunked_storage.sql_entities;
import bioseq.postgres.storage.manager;
import bioseq.postgres.storage.manager.sequence_storage.sql_entities;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.scanner;
import bioseq.storage.layout;
import bioseq.postgres.table.am_cache;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.column.table_access;
import :access_identity;
import :access_common;
import :access_hooks;

/*
 * Timing parameters for truncate locking heuristics (file-local).
 *
 * These are the same values from src/backend/access/heap/vacuumlazy.c
 */
namespace {
constexpr const char* k_column_set_options_hook_symbol =
	"BioseqColumnTableSetOptions_hook";

constexpr int k_vacuum_truncate_lock_wait_interval_ms = 50;   /* ms */
constexpr int k_vacuum_truncate_lock_timeout_ms = 4500;       /* ms */

struct pending_speculative_sources {
    SubTransactionId subtransaction_id = InvalidSubTransactionId;
    std::vector<bioseq::storage::pending_sequence_source> sources;
};

using pending_sequence_source_slots =
    std::vector<std::optional<bioseq::storage::pending_sequence_source>>;

static_assert(!std::is_trivially_destructible_v<
    bioseq::postgres::support::memory_context_scope>);
static_assert(!std::is_trivially_destructible_v<pending_sequence_source_slots>);
static_assert(std::is_nothrow_move_constructible_v<
    bioseq::storage::pending_sequence_source>);

using pending_speculative_key = std::pair<Oid, std::uint64_t>;
using pending_speculative_source_map =
    std::map<pending_speculative_key, pending_speculative_sources>;

struct pending_speculative_terminal_quarantine {
    std::uint64_t invariant_events = 0u;
    std::uint64_t quarantined_rows = 0u;
    std::uint64_t quarantined_sources = 0u;
    XactEvent last_event = XACT_EVENT_COMMIT;
    bool storage_exhausted = false;
    bool active_map_quarantined = false;
};

[[nodiscard]] auto& access_pending_speculative_sources() {
    static pending_speculative_source_map pending;
    return pending;
}

[[nodiscard]] auto& quarantined_speculative_sources_noexcept() noexcept {
    // A terminal transaction callback cannot allocate, destroy retained
    // source authority, or raise a PostgreSQL error.  Fixed map slots let the
    // callback quarantine an impossible survivor with an allocation-free,
    // noexcept swap while retaining the move-only authority for diagnosis.
    static std::array<pending_speculative_source_map, 4u> quarantined;
    return quarantined;
}

[[nodiscard]] auto& speculative_source_quarantine_state_noexcept() noexcept {
    static pending_speculative_terminal_quarantine state;
    return state;
}

static_assert(noexcept(
    std::declval<pending_speculative_source_map&>().swap(
        std::declval<pending_speculative_source_map&>())));

void quarantine_pending_speculative_sources_noexcept(XactEvent event) noexcept {
    auto& pending = access_pending_speculative_sources();
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

void require_no_pending_speculative_sources(const char* operation) {
    const auto& pending = access_pending_speculative_sources();
    if (pending.empty()) return;

    std::size_t source_count = 0;
    for (const auto& [key, entry] : pending) {
        (void)key;
        source_count += entry.sources.size();
    }
    ereport(ERROR,
        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
         errmsg("bioseq speculative source publication authority was not consumed"),
         errdetail("operation=%s speculative_rows=%zu pending_sources=%zu",
             operation, pending.size(), source_count)));
}
}

/** Begin a table scan with explicit projection and executor modes. */
export TableScanDesc
bioseq_beginscan_extended(Relation relation, Snapshot snapshot,
							int key_count, ScanKey key,
							ParallelTableScanDesc parallel_table_scan,
							uint32_t flags, Bitmapset *attributes_needed, List *scan_qualifiers,
							parallel_column_scan_handle parallel_column_scan,
							bool returns_vectorized_tuples);

/** Begin a scan with an explicit sequence-hydration policy. */
TableScanDesc
bioseq_beginscan_with_options(Relation relation, Snapshot snapshot,
							int key_count, ScanKey key,
							ParallelTableScanDesc parallel_table_scan,
							uint32_t flags, Bitmapset *attributes_needed, List *scan_qualifiers,
							parallel_column_scan_handle parallel_column_scan,
							bool returns_vectorized_tuples,
							bool defer_sequence_hydration);

/** Callback published through the stable relation-options rendezvous slot. */
typedef void (*column_set_options_hook_type)(Oid relid, column_options options);

/* available to other extensions using find_rendezvous_variable() */
static column_set_options_hook_type column_set_options_hook = NULL;

/* forward declaration for static functions */
static bool try_lock_relation_until_timeout(Relation rel, LOCKMODE lock_mode,
											   int timeout, int retry_interval,
											   bool acquire);
static void log_relation_statistics(Relation rel, int elevel);
static void truncate_column_storage_tail(Relation rel, int elevel);
static bool truncate_and_compact_column_stripes(Relation rel, int elevel);
static void report_total_virtual_blocks(Relation relation, Snapshot snapshot,
											 int progress_arr_index);
static BlockNumber column_virtual_block_count(Relation relation, Snapshot snapshot);
static ItemPointerData highest_column_item_pointer(Relation relation,
													 Snapshot snapshot);
static double build_index_from_column_rows(TableScanDesc scan,
										Relation index_relation,
										IndexInfo *index_info,
										bool progress,
										IndexBuildCallback index_callback,
										void *index_callback_state,
										EState *estate, ExprState *predicate,
										bioseq::storage::pg_sequence_source_provider*
											source_provider);
static void validate_missing_index_rows(TableScanDesc scan, Relation index_relation,
											 IndexInfo *index_info, EState *estate,
											 ExprState *predicate,
											 ValidateIndexState *state,
											 bioseq::storage::pg_sequence_source_provider*
												source_provider);
static ItemPointerData skip_item_pointers_before(Tuplesortstate *tuple_sort,
															ItemPointer target_item_pointer);

/*
 * File-static holder for the page-cache flag value captured when a
 * scan/DML callback first disables the cache. Not convertible to a
 * page_cache_disable_scope guard because the save and restore happen in
 * *different* TableAM callbacks (begin/end for scans; insert/complete
 * for speculative inserts), so the lifetime cannot be expressed as a
 * single C++ scope. The global slot means concurrent scans in the same
 * backend would overwrite each other's saved state -- currently the
 * executor serialises these callbacks per-backend so that is fine, but
 * adding a second slot would require threading the previous value
 * through the per-scan descriptor.
 */
static bool previous_cache_enabled_state = false;

struct ordinary_insert_page_cache_restore {
	ErrorContextCallback callback{};
	bool previous_value{};
	bool armed{};
};

static_assert(std::is_trivially_copyable_v<ordinary_insert_page_cache_restore>);
static_assert(std::is_standard_layout_v<ordinary_insert_page_cache_restore>);

static void
restore_ordinary_insert_page_cache_on_error(void *argument) noexcept
{
	auto *restore = static_cast<ordinary_insert_page_cache_restore *>(argument);
	if (geterrlevel() < ERROR || restore == nullptr || !restore->armed)
	{
		return;
	}
	bioseq::settings::column_cache::enabled = restore->previous_value;
	restore->armed = false;
}

static TableScanDesc
bioseq_beginscan(Relation relation, Snapshot snapshot,
				   int key_count, ScanKey key,
				   ParallelTableScanDesc parallel_table_scan,
				   uint32_t flags)
{
	previous_cache_enabled_state = bioseq::settings::column_cache::enabled;

	int natts = relation->rd_att->natts;

	/* attributes_needed represents 0-indexed attribute numbers */
	bioseq::postgres::support::postgres_bitmapset attributes_needed = bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);

	TableScanDesc scandesc = bioseq_beginscan_extended(relation, snapshot, key_count, key,
														 parallel_table_scan,
														 flags, attributes_needed.get(), NULL,
														 NULL,
														 false);

	attributes_needed.clear();

	return scandesc;
}


export TableScanDesc
bioseq_beginscan_extended(Relation relation, Snapshot snapshot,
							int key_count, ScanKey key,
							ParallelTableScanDesc parallel_table_scan,
							uint32_t flags, Bitmapset *attributes_needed, List *scan_qualifiers,
							parallel_column_scan_handle parallel_column_scan,
							bool returns_vectorized_tuples)
{
	return bioseq_beginscan_with_options(relation, snapshot, key_count, key,
		parallel_table_scan, flags, attributes_needed, scan_qualifiers, parallel_column_scan,
		returns_vectorized_tuples, /*defer_sequence_hydration=*/false);
}

/*
 * the custom scan's deferred-hydration entry point. Same
 * signature as bioseq_beginscan_extended but turns the deferred-hydration
 * flag on, so bioseq_getnextslot returns locator stubs verbatim and the
 * dispatch buffer hydrates them in per-bucket bulk. `export`ed
 * so :custom_scan reaches it via `import :access_method` instead of an
 * `extern "C++"` forward decl.
 */
export TableScanDesc
bioseq_beginscan_deferred(Relation relation, Snapshot snapshot,
							int key_count, ScanKey key,
							ParallelTableScanDesc parallel_table_scan,
							uint32_t flags, Bitmapset *attributes_needed, List *scan_qualifiers,
							parallel_column_scan_handle parallel_column_scan,
							bool returns_vectorized_tuples)
{
	return bioseq_beginscan_with_options(relation, snapshot, key_count, key,
		parallel_table_scan, flags, attributes_needed, scan_qualifiers, parallel_column_scan,
		returns_vectorized_tuples, /*defer_sequence_hydration=*/true);
}

export extern "C" TableScanDesc
bioseq_beginscan_extended_c(Relation relation, Snapshot snapshot,
							int key_count, ScanKey key,
							ParallelTableScanDesc parallel_table_scan,
							uint32_t flags, Bitmapset *attributes_needed, List *scan_qualifiers,
							parallel_column_scan_handle parallel_column_scan,
							bool returns_vectorized_tuples)
{
	return bioseq_beginscan_with_options(relation, snapshot, key_count, key,
		parallel_table_scan, flags, attributes_needed, scan_qualifiers, parallel_column_scan,
		returns_vectorized_tuples, /*defer_sequence_hydration=*/false);
}

export extern "C" TableScanDesc
bioseq_beginscan_deferred_c(Relation relation, Snapshot snapshot,
							int key_count, ScanKey key,
							ParallelTableScanDesc parallel_table_scan,
							uint32_t flags, Bitmapset *attributes_needed, List *scan_qualifiers,
							parallel_column_scan_handle parallel_column_scan,
							bool returns_vectorized_tuples)
{
	return bioseq_beginscan_with_options(relation, snapshot, key_count, key,
		parallel_table_scan, flags, attributes_needed, scan_qualifiers, parallel_column_scan,
		returns_vectorized_tuples, /*defer_sequence_hydration=*/true);
}

TableScanDesc
bioseq_beginscan_with_options(Relation relation, Snapshot snapshot,
							int key_count, ScanKey key,
							ParallelTableScanDesc parallel_table_scan,
							uint32_t flags, Bitmapset *attributes_needed, List *scan_qualifiers,
							parallel_column_scan_handle parallel_column_scan,
							bool returns_vectorized_tuples,
							bool defer_sequence_hydration)
{
	previous_cache_enabled_state = bioseq::settings::column_cache::enabled;
	Oid relfilelocator = relation->rd_locator.relNumber;
	/*
	 * A memory context to use for scan-wide data, including the lazily
	 * initialized read state. We assume that beginscan is called in a
	 * context that will last until end of scan.
	 */
	MemoryContext scan_context = create_column_scan_memory_context();
	column_scan_handle scan;
	{
		bioseq::postgres::support::memory_context_scope ctx_guard{scan_context};

		scan = ::new (palloc0(sizeof(column_scan_descriptor))) column_scan_descriptor{};
		scan->scan_base.rs_rd = relation;
		scan->scan_base.rs_snapshot = snapshot;
		scan->scan_base.rs_nkeys = key_count;
		scan->scan_base.rs_key = key;
		scan->scan_base.rs_flags = flags;
		scan->scan_base.rs_parallel = parallel_table_scan;

		/*
		 * We will initialize this lazily in first tuple, where we have the actual
		 * tuple descriptor to use for reading. In some cases like ALTER TABLE ...
		 * ALTER COLUMN ... TYPE, the tuple descriptor of relation doesn't match
		 * the storage which we are reading, so we need to use the tuple descriptor
		 * of "slot" in first read.
		 */
		scan->read_state = NULL;
		scan->attributes_needed =
			bioseq::postgres::support::postgres_bitmapset{attributes_needed}.clone();
		scan->scan_qualifiers =
			bioseq::postgres::support::postgres_list<Node>{
				copyObject(scan_qualifiers)};
		scan->scan_context = scan_context;

		/* Parallel execution scan data */;
		scan->parallel_scan = parallel_column_scan;

		/* Vectorized result */
		scan->returns_vectorized_tuple = returns_vectorized_tuples;

		/* per-scan storage cache and deferred-hydration flag. */
		scan->defers_sequence_hydration = defer_sequence_hydration;
		auto* caches = bioseq::storage::alloc_seq_caches(scan_context);
		caches->side = bioseq::storage::seq_caches::access_side::read;
		caches->lock_mode = AccessShareLock;
		caches->read_state_mctx = scan_context;
		caches->main_database_oid   = relation->rd_locator.dbOid;
		caches->main_tablespace_oid = relation->rd_locator.spcOid;
		caches->main_relfilenode    = relation->rd_locator.relNumber;
		scan->sequence_cache = caches;

		if (writes_pending_in_parent_transactions(relfilelocator, GetCurrentSubTransactionId()))
		{
			bioseq_log<logging_level::error, logging_target::postgres>(
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
		: locked_relation{relation},
		  close_lock_mode{close_lock},
		  uses_table_close{opened_as_table} {}

	sql_relation_lock(const sql_relation_lock&) = delete;
	sql_relation_lock& operator=(const sql_relation_lock&) = delete;

	~sql_relation_lock() noexcept {
		if (locked_relation == nullptr) return;
		if (uses_table_close) {
			table_close(locked_relation, close_lock_mode);
		} else {
			relation_close(locked_relation, close_lock_mode);
		}
	}

private:
	Relation locked_relation = nullptr;
	LOCKMODE close_lock_mode = NoLock;
	bool uses_table_close = false;
};

[[nodiscard]] std::string quoted_relation_name(Relation relation) {
	const char* const relation_name = RelationGetRelationName(relation);
	const char* const quoted = bioseq::postgres::support::invoke_pg_as_cpp(
		[relation_name] { return quote_identifier(relation_name); });
	return quoted == nullptr ? std::string{} : std::string{quoted};
}

void require_bioseq_table(
		Relation relation,
		Oid relation_id,
		const char* object_kind = "table") {
	const bool is_bioseq = bioseq::postgres::support::invoke_pg_as_cpp(
		[relation_id] {
			return relation_oid_uses_column_table_access_method(relation_id);
		});
	if (is_bioseq) return;
	bioseq::postgres::support::raise_sql_error(
		ERRCODE_INTERNAL_ERROR,
		{
			.message = std::string{object_kind} + " "
				+ quoted_relation_name(relation)
				+ " is not a columnar table",
		});
}

void require_relation_owner(Oid relation_id) {
	const Oid user_id = GetUserId();
	const bool is_owner = bioseq::postgres::support::invoke_pg_as_cpp(
		[relation_id, user_id] {
			return object_ownercheck(
				RelationRelationId, relation_id, user_id);
		});
	if (is_owner) return;
	bioseq::postgres::support::invoke_pg_as_cpp(
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
	Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
		[relation_id] {
			return bioseq::postgres::support::open_postgres_relation(relation_id, AccessShareLock);
		});
	const sql_relation_lock relation_lock{
		relation, AccessShareLock, false};
	require_bioseq_table(relation, relation_id, "relation");
	const std::uint64_t storage_id =
		bioseq::postgres::support::invoke_pg_as_cpp(
			[relation] {
				return column_storage_id(
					relation, false);
			});
	return static_cast<std::int64_t>(storage_id);
}

}  // namespace bioseq::table::sql_routines

/*
 * init_bioseq_read_state initializes a column store table read and returns the
 * state.
 */
static column_read_state *
init_bioseq_read_state(
	Relation relation, TupleDesc tupdesc,
	const bioseq::postgres::support::postgres_bitmapset& attr_needed,
	List *scan_qual, MemoryContext scan_context, Snapshot snapshot,
	bool random_access,
	parallel_column_scan_handle parallel_scan)
{
	bioseq::postgres::support::memory_context_scope ctx_guard{scan_context};

	List *needed_columns = build_needed_column_list(tupdesc, attr_needed);
	column_read_state *read_state = begin_column_read(relation, tupdesc, needed_columns,
												 scan_qual, scan_context, snapshot,
												 random_access,
												 parallel_scan);

	return read_state;
}


static void
bioseq_endscan(TableScanDesc sscan)
{
	column_scan_handle scan = reinterpret_cast<column_scan_handle>(sscan);

	/* close per-scan seq_cache (releases relation pins,
	 * tears down any cached BioseqColumnReadStates on sub-tables). */
	if (scan->sequence_cache != NULL) {
		auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->sequence_cache);
		caches->close_all(/*commit=*/true);
		bioseq::storage::destroy_seq_caches(caches);
		scan->sequence_cache = NULL;
	}

	if (scan->read_state != NULL)
	{
		end_column_read(scan->read_state);
		scan->read_state = NULL;
	}

	if (scan->scan_base.rs_flags & SO_TEMP_SNAPSHOT)
	{
		UnregisterSnapshot(scan->scan_base.rs_snapshot);
	}

	/* clean up any caches. */
	if (bioseq::settings::column_cache::enabled == true)
	{
		reset_column_cache();
	}

	MemoryContextDelete(scan->scan_context);

	bioseq::settings::column_cache::enabled = previous_cache_enabled_state;
}


static void
bioseq_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
				bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	column_scan_handle scan = reinterpret_cast<column_scan_handle>(sscan);

	/* XXX: hack to pass in new quals that aren't actually scan keys */
	List *scan_qual = reinterpret_cast<List *>(key);

	if (scan->read_state != NULL)
	{
		rescan_column_reader(scan->read_state, scan_qual);
	}
}


static bool
bioseq_getnextslot_impl(
	TableScanDesc sscan,
	ScanDirection direction,
	TupleTableSlot *slot)
{
	column_scan_handle scan = reinterpret_cast<column_scan_handle>(sscan);

	/*
	 * if this is the first row, initialize read state.
	 */
	if (scan->read_state == NULL)
	{
		bool random_access = false;
		scan->read_state =
			init_bioseq_read_state(scan->scan_base.rs_rd, slot->tts_tupleDescriptor,
								 scan->attributes_needed, scan->scan_qualifiers.get(),
								 scan->scan_context, scan->scan_base.rs_snapshot,
								 random_access,
								 scan->parallel_scan);
	}

	ExecClearTuple(slot);

	if (scan->returns_vectorized_tuple)
	{
		auto* vector_slot =
			reinterpret_cast<vector_tuple_table_slot_storage*>(slot);

		int new_vector_size = 0;

		bool next_row_found = read_next_column_vector(scan->read_state,
												   vector_slot->virtual_slot.base.tts_values,
												   vector_slot->virtual_slot.base.tts_isnull,
												   vector_slot->source_row_numbers,
												   &new_vector_size);

		if (!next_row_found)
			return false;

		vector_slot->row_count = new_vector_size;

		memset(vector_slot->row_is_active, true, new_vector_size);

		ExecStoreVirtualTuple(slot);
	}
	else
	{
		uint64_t row_number;
		bool next_row_found = read_next_column_row(scan->read_state, slot->tts_values,
													slot->tts_isnull, &row_number);

		if (!next_row_found)
		{
			return false;
		}

		ExecStoreVirtualTuple(slot);
		slot->tts_tid = row_number_to_tid(row_number);

		/*
		 * hydrate every SEQ column unless the caller
		 * asked for deferred hydration (custom scan / dispatch buffer
		 * path, which performs per-bucket bulk hydration later).
		 */
		if (!scan->defers_sequence_hydration)
		{
			const auto& amc = bioseq::storage::get_table_access_cache(scan->scan_base.rs_rd);
			auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->sequence_cache);
			hydrate_sequence_columns_in_slot(scan->scan_base.rs_rd, slot, caches, amc,
			                            scan->read_state->snapshot,
			                            scan->scan_context);
		}
	}

	return true;
}


/*
 * row_number_to_tid / tid_to_row_number / validate_row_number
 * moved to the bioseq_column_tam leaf module (imported above). Their definitions
 * are no longer here; callers in this TU reach the two tid helpers via that import.
 */


static Size
bioseq_parallelscan_estimate(Relation rel)
{
	bioseq_log<logging_level::error, logging_target::postgres>( "bioseq_parallelscan_estimate not implemented");
}


static Size
bioseq_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	bioseq_log<logging_level::error, logging_target::postgres>( "bioseq_parallelscan_initialize not implemented");
}


static void
bioseq_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	bioseq_log<logging_level::error, logging_target::postgres>( "bioseq_parallelscan_reinitialize not implemented");
}


[[gnu::always_inline]]
static inline void
init_index_fetch_seq_cache(index_fetch_state *scan, Relation rel,
                           MemoryContext scan_context)
{
	auto *caches = bioseq::storage::alloc_seq_caches(scan_context);
	caches->side = bioseq::storage::seq_caches::access_side::read;
	caches->lock_mode = AccessShareLock;
	caches->read_state_mctx = scan_context;
	caches->main_database_oid   = rel->rd_locator.dbOid;
	caches->main_tablespace_oid = rel->rd_locator.spcOid;
	caches->main_relfilenode    = rel->rd_locator.relNumber;
	scan->sequence_cache = caches;
	scan->defers_sequence_hydration = false;
}

static IndexFetchTableData *
bioseq_index_fetch_begin(Relation rel)
{
	Oid relfilelocator = rel->rd_locator.relNumber;
	if (writes_pending_in_parent_transactions(relfilelocator, GetCurrentSubTransactionId()))
	{
		/* XXX: maybe we can just flush the data and continue */
		bioseq_log<logging_level::error, logging_target::postgres>( "cannot read from index when there is unflushed data in "
					"upper transactions");
	}

	MemoryContext scan_context = create_column_scan_memory_context();
	index_fetch_state *scan;
	{
		bioseq::postgres::support::memory_context_scope ctx_guard{scan_context};

		scan = ::new (palloc0(sizeof(index_fetch_state))) index_fetch_state{};
		scan->fetch_base.rel = rel;
		scan->read_state = NULL;
		scan->stripe_metadata = {};
		scan->scan_context = scan_context;
		scan->is_select_query = false;
		init_index_fetch_seq_cache(scan, rel, scan_context);
	}

	return &scan->fetch_base;
}

export IndexFetchTableData *
bioseq_index_fetch_begin_extended(Relation rel, Bitmapset *attr_needed)
{
	Oid relfilenode = rel->rd_locator.relNumber;
	if (writes_pending_in_parent_transactions(relfilenode, GetCurrentSubTransactionId()))
	{
		/* XXX: maybe we can just flush the data and continue */
		bioseq_log<logging_level::error, logging_target::postgres>( "cannot read from index when there is unflushed data in "
					"upper transactions");
	}

	MemoryContext scan_context = create_column_scan_memory_context();
	index_fetch_state *scan;
	{
		bioseq::postgres::support::memory_context_scope ctx_guard{scan_context};

		scan = ::new (palloc0(sizeof(index_fetch_state))) index_fetch_state{};
		scan->fetch_base.rel = rel;
		scan->read_state = NULL;
		scan->stripe_metadata = {};
		scan->scan_context = scan_context;

		scan->attributes_needed =
			bioseq::postgres::support::postgres_bitmapset{attr_needed}.clone();
		scan->is_select_query = true;
		init_index_fetch_seq_cache(scan, rel, scan_context);
	}

	return &scan->fetch_base;
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

	index_fetch_state *scan = reinterpret_cast<index_fetch_state *>(sscan);

	/* close per-fetch seq_cache. */
	if (scan->sequence_cache != NULL) {
		auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->sequence_cache);
		caches->close_all(/*commit=*/true);
		bioseq::storage::destroy_seq_caches(caches);
		scan->sequence_cache = NULL;
	}

	if (scan->read_state)
	{
		end_column_read(scan->read_state);
		scan->read_state = NULL;
	}

	scan->attributes_needed.clear();

	/* clean up any caches. */
	if (bioseq::settings::column_cache::enabled == true)
	{
		reset_column_cache();
	}

	MemoryContextDelete(scan->scan_context);
}

static stripe_metadata *
find_stripe_metadata_by_row_number(index_fetch_state *scan, uint64_t row_number)
{
	ListCell *lc = NULL;

	int high = scan->stripe_metadata.size() - 1;
	int low = 0;

	while(low <= high)
	{
		int mid = low + (high - low) / 2;

		lc = list_nth_cell(scan->stripe_metadata.get(), mid);

		stripe_metadata *stripe_metadata_entry = static_cast<stripe_metadata*>(lc->ptr_value);

		if (row_number >= stripe_metadata_entry->first_row_number &&
			row_number < stripe_metadata_entry->first_row_number + stripe_metadata_entry->row_count)
		{
			return stripe_metadata_entry;
		}

		if (stripe_metadata_entry->first_row_number > row_number)
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

	index_fetch_state *scan = reinterpret_cast<index_fetch_state *>(sscan);
	Relation bioseq_column_relation = scan->fetch_base.rel;

	/* initialize read state for the first row */
	if (scan->read_state == NULL)
	{
		/* no quals for index scan */
		List *scan_qual = NIL;

		if (scan->attributes_needed.is_empty())
		{
			/* we need all columns */
			int natts = bioseq_column_relation->rd_att->natts;
			scan->attributes_needed.clear();
			scan->attributes_needed = bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);
		}

		bool random_access = true;
		scan->read_state = init_bioseq_read_state(bioseq_column_relation,
												  slot->tts_tupleDescriptor,
												  scan->attributes_needed, scan_qual,
												  scan->scan_context,
												  snapshot, random_access,
												  NULL);
		if (scan->is_select_query)
			scan->stripe_metadata =
				bioseq::postgres::support::postgres_list<stripe_metadata>{
					list_stripes_for_relation(
						bioseq_column_relation->rd_locator,
						ForwardScanDirection)};

	}

	uint64_t row_number = tid_to_row_number(*tid);

	stripe_metadata *stripe_metadata_entry = NULL;

	if (scan->is_select_query)
		stripe_metadata_entry = find_stripe_metadata_by_row_number(scan, row_number);
	else
		stripe_metadata_entry = find_stripe_by_first_row_number(bioseq_column_relation, row_number, snapshot);

	if (!stripe_metadata_entry)
	{
		/* it is certain that tuple with row_number doesn't exist */
		return false;
	}

	stripe_write_state stripe_write_state_value = stripe_write_state_for(stripe_metadata_entry);

	if (stripe_write_state_value == stripe_write_state::flushed &&
		!read_column_row_by_number(scan->read_state, row_number,
									slot->tts_values, slot->tts_isnull))
	{
		/*
		 * find_stripe_by_first_row_number doesn't verify upper row
		 * number boundary of found stripe. For this reason, we didn't
		 * certainly know if given row number belongs to one of the stripes.
		 */
		return false;
	}
	else if (stripe_write_state_value == stripe_write_state::aborted)
	{
		/*
		 * We only expect to see un-flushed stripes when checking against
		 * constraint violation. In that case, indexAM provides dirty
		 * snapshot to index_fetch_tuple callback.
		 */
		pfree(stripe_metadata_entry);
		Assert(snapshot->snapshot_type == SNAPSHOT_DIRTY);
		return false;
	}
	else if (stripe_write_state_value == stripe_write_state::in_progress)
	{
		if (stripe_metadata_entry->inserted_by_current_transaction)
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
			flush_pending_writes_for_read(scan->read_state);

			/*
			 * Fill the tupleslot and fall through to return true, it
			 * certainly exists.
			 */
			read_column_row_by_number_or_error(scan->read_state, row_number,
											  slot->tts_values, slot->tts_isnull);
		}
		else
		{
			/* similar to aborted writes, it should be dirty snapshot */
			Assert(snapshot->snapshot_type == SNAPSHOT_DIRTY);

			/*
			 * Stripe that "might" contain the tuple with row_number is not
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
		 * read_column_row_by_number successfully filled the tupleslot.
		 */
		Assert(stripe_write_state_value == stripe_write_state::flushed);
	}

	if (!scan->is_select_query)
		pfree(stripe_metadata_entry);
	slot->tts_tableOid = RelationGetRelid(bioseq_column_relation);
	slot->tts_tid = *tid;
	ExecStoreVirtualTuple(slot);

	/*
	 * hydrate SEQ stub columns unless the caller asked
	 * for deferred hydration. For the index-fetch path we always hydrate
	 * because downstream operators (recheck quals, executor projection)
	 * read SEQ values directly.
	 */
	if (!scan->defers_sequence_hydration)
	{
		const auto& amc = bioseq::storage::get_table_access_cache(bioseq_column_relation);
		auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->sequence_cache);
		hydrate_sequence_columns_in_slot(bioseq_column_relation, slot, caches, amc,
		                            scan->read_state->snapshot,
		                            scan->scan_context);
	}

	return true;
}


static bool
bioseq_fetch_row_version(Relation relation,
						   ItemPointer tid,
						   Snapshot snapshot,
						   TupleTableSlot *slot)
{
	uint64_t row_number = tid_to_row_number(*tid);
	column_read_state **read_state =
		find_column_read_state_slot(relation, GetCurrentSubTransactionId());

	if (read_state == NULL)
	{
		read_state = acquire_column_read_state_slot(relation, GetCurrentSubTransactionId());

		int natts = relation->rd_att->natts;
		bioseq::postgres::support::postgres_bitmapset attr_needed = bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);

		List *scan_qual = NIL;

		bool random_access = false;

		*read_state = init_bioseq_read_state(relation,
												  slot->tts_tupleDescriptor,
												  attr_needed, scan_qual,
												  column_read_state_memory_context(),
												  snapshot, random_access,
												  NULL);
	}

	{
		bioseq::postgres::support::memory_context_scope ctx_guard{
			column_read_state_memory_context()};
		read_column_row_by_number(*read_state, row_number,
								   slot->tts_values, slot->tts_isnull);
	}

	slot->tts_tableOid = RelationGetRelid(relation);
	slot->tts_tid = *tid;

	if (TTS_EMPTY(slot))
		ExecStoreVirtualTuple(slot);

	/* hydrate any SEQ stubs in the slot. We do NOT
	 * cache a per-fetch seq_caches here because the row-version fetch
	 * is one-shot; allocate, use, destroy. */
	{
		const auto& amc = bioseq::storage::get_table_access_cache(relation);
		if (!amc.sequence_attribute_numbers.empty()) {
			auto* caches = bioseq::storage::alloc_seq_caches(column_read_state_memory_context());
			caches->side = bioseq::storage::seq_caches::access_side::read;
			caches->lock_mode = AccessShareLock;
			caches->read_state_mctx = column_read_state_memory_context();
			caches->main_database_oid   = relation->rd_locator.dbOid;
			caches->main_tablespace_oid = relation->rd_locator.spcOid;
			caches->main_relfilenode    = relation->rd_locator.relNumber;
			hydrate_sequence_columns_in_slot(relation, slot, caches, amc,
			                            (*read_state)->snapshot,
			                            column_read_state_memory_context());
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
	bioseq_log<logging_level::error, logging_target::postgres>( "bioseq_get_latest_tid not implemented");
}


static bool
bioseq_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	bioseq_log<logging_level::error, logging_target::postgres>( "bioseq_tuple_tid_valid not implemented");
}


static bool
bioseq_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								  Snapshot snapshot)
{
	uint64_t row_number = tid_to_row_number(slot->tts_tid);
	stripe_metadata *stripe_metadata_entry = find_stripe_by_row_number(rel, row_number, snapshot);
	return stripe_metadata_entry != NULL;
}


static TransactionId
bioseq_index_delete_tuples(Relation rel,
							 TM_IndexDeleteOp *delstate)
{
	previous_cache_enabled_state = bioseq::settings::column_cache::enabled;
	bioseq::settings::column_cache::enabled = false;

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
		bioseq_log<logging_level::error, logging_target::postgres>( "bioseq_index_delete_tuples not implemented for simple deletion");
	}
}


[[nodiscard]] static bioseq::storage::seq_caches*
ensure_write_side_seq_caches(Relation relation,
                             column_write_state* write_state)
{
	if (relation == nullptr || write_state == nullptr
		|| write_state->stripe_write_context == nullptr)
	{
		bioseq::postgres::support::invoke_pg_as_cpp([] {
			bioseq_log<logging_level::error, logging_target::postgres>(
				"bioseq storage: sequence write cache owner is unavailable");
		});
	}

	if (write_state->seq_caches == nullptr)
	{
		MemoryContext owner = MemoryContextGetParent(
			write_state->stripe_write_context);
		if (owner == nullptr)
		{
			bioseq::postgres::support::invoke_pg_as_cpp([] {
				bioseq_log<logging_level::error, logging_target::postgres>(
					"bioseq storage: sequence write cache context is unavailable");
			});
		}
		auto* fresh = bioseq::postgres::support::invoke_pg_as_cpp([owner] {
			return bioseq::storage::alloc_seq_caches(owner);
		});
		fresh->side = bioseq::storage::seq_caches::access_side::write;
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
	if (caches->side != bioseq::storage::seq_caches::access_side::write
		|| caches->main_database_oid != relation->rd_locator.dbOid
		|| caches->main_tablespace_oid != relation->rd_locator.spcOid
		|| caches->main_relfilenode != relation->rd_locator.relNumber)
	{
		bioseq::postgres::support::invoke_pg_as_cpp([] {
			bioseq_log<logging_level::error, logging_target::postgres>(
				"bioseq storage: sequence write cache relation identity changed");
		});
	}
	return caches;
}


/*
 * rewrite every non-NULL SEQ column in `detoasted_values`
 * to a 40-byte locator stub. The original payload bytes are written to the
 * matching sub-table shard via sequence_storage_manager::insert_payload.
 * Returns a freshly-palloc'd Datum* (in the per-tuple context); the caller
 * MUST hand THIS to write_column_row (NOT slot->tts_values) so that
 * ExecInsertIndexTuples — which reads from `slot->tts_values[]` to build
 * btree/hash/gin keys — never sees the storage-layout bytes.
 *
 * `out_per_col_locator[c]` is the locator for amc.sequence_attribute_numbers[c]; left as
 * std::nullopt for NULL/already-stub values.
 */
[[nodiscard]] static Datum *
rewrite_seq_columns_to_stubs(Relation relation,
	                             TupleTableSlot *slot,
	                             column_write_state *write_state,
                             const bioseq::storage::table_access_cache &amc,
                             Datum *detoasted_values,
                             std::vector<std::optional<bioseq::storage::sequence_locator>>
                                 &out_per_col_locator,
                             std::vector<std::optional<bioseq::storage::pending_sequence_source>>
                                 &out_pending_sources)
{
	TupleDesc td = slot->tts_tupleDescriptor;
	Datum *values_for_write = bioseq::postgres::support::invoke_pg_as_cpp([td] {
		return static_cast<Datum *>(palloc(sizeof(Datum) * td->natts));
	});
	std::memcpy(values_for_write, detoasted_values, sizeof(Datum) * td->natts);

	out_per_col_locator.assign(amc.sequence_attribute_numbers.size(), std::nullopt);
	out_pending_sources.clear();
	out_pending_sources.resize(amc.sequence_attribute_numbers.size());
	if (amc.sequence_attribute_numbers.empty()) {
		return values_for_write;
	}
	// write_state_management.cppm cannot import bioseq_storage without a
	// module cycle, so the TableAM owns this lazy initialization.  The cache
	// lives above stripe_write_context and therefore survives stripe resets.
	auto* caches = ensure_write_side_seq_caches(relation, write_state);
	const auto cap_bytes = caches->cap_bytes_snapshot;
	const std::uint64_t main_storage_id =
		bioseq::postgres::support::invoke_pg_as_cpp([relation] {
			return column_storage_id(relation, false);
		});

	for (std::size_t c = 0; c < amc.sequence_attribute_numbers.size(); ++c) {
		const std::int16_t attno = amc.sequence_attribute_numbers[c];
		const int idx = attno - 1;
		if (idx < 0 || idx >= td->natts) continue;
		if (slot->tts_isnull[idx]) continue;
		auto *hdr = reinterpret_cast<bioseq::pg_types::seq *>(
			DatumGetPointer(values_for_write[idx]));
		if (bioseq::storage::is_locator_stub(hdr)) {
			bioseq::postgres::support::invoke_pg_as_cpp([] {
				bioseq_log<logging_level::error, logging_target::postgres>(
					"bioseq storage: a locator stub cannot be shared by a second parent row");
			});
		}
		auto pending = bioseq::postgres::support::invoke_pg_as_cpp(
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
		bioseq::pg_types::seq *stub = bioseq::postgres::support::invoke_pg_as_cpp(
			[write_state, hdr, loc] {
				return bioseq::storage::alloc_locator_stub_in(
					get_column_write_per_tuple_context(write_state),
					static_cast<std::uint8_t>(hdr->alphabet_code),
					static_cast<std::uint8_t>(hdr->storage_unit_byte_width),
					hdr->padding_letters,
					/*total_bp_size=*/loc.logical_length,
					loc);
			});
		values_for_write[idx] = PointerGetDatum(stub);
	}
	return values_for_write;
}

static void publish_sequence_sources_after_parent_row(
		std::vector<std::optional<bioseq::storage::pending_sequence_source>>& sources)
{
	for (auto& source : sources) {
		if (source.has_value()) {
			Assert(source->pending());
			bioseq::postgres::support::invoke_pg_as_cpp([&source] {
				source->publish_after_parent_row();
			});
			Assert(!source->pending());
		}
	}
}

template <class p_function>
decltype(auto) run_tableam_cpp_checked(
	const char* operation,
	p_function&& function) {
	ErrorData* exact_error = nullptr;
	std::array<char, 512> detail{};
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	try {
		if constexpr (std::is_void_v<std::invoke_result_t<p_function&>>) {
			function();
			return;
		} else {
			return function();
		}
	} catch (bioseq::postgres::support::postgres_error& error) {
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
	// bioseq::postgres::support::memory_context_scope) has unwound.  Only fixed
	// diagnostic state remains at this PostgreSQL C callback boundary.
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

template <class p_function>
decltype(auto) run_index_source_cpp_checked(
	const char* failure_message,
	p_function&& function)
{
	ErrorData* exact_error = nullptr;
	std::array<char, 512> detail{};
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	try {
		if constexpr (std::is_void_v<std::invoke_result_t<p_function&>>) {
			function();
			return;
		} else {
			return function();
		}
	} catch (bioseq::postgres::support::postgres_error& error) {
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
		Datum *detoasted_values = bioseq::postgres::support::invoke_pg_as_cpp(
			[tuple_desc, values, nulls] {
				return bioseq::postgres::support::detoast_values(
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
	column_write_state *write_state = nullptr;
	ordinary_insert_page_cache_restore cache_restore{
		.callback = {
			.previous = error_context_stack,
			.callback = &restore_ordinary_insert_page_cache_on_error,
			.arg = nullptr,
		},
		.previous_value = bioseq::settings::column_cache::enabled,
		.armed = true,
	};
	cache_restore.callback.arg = &cache_restore;
	error_context_stack = &cache_restore.callback;
	bioseq::settings::column_cache::enabled = false;
	run_tableam_cpp_checked("bioseq tuple insert",
		[relation, slot, &write_state] {
		if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		{
			bioseq::postgres::support::invoke_pg_as_cpp([] {
				bioseq_log<logging_level::error, logging_target::postgres>(
					"bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
			});
		}
		write_state = bioseq::postgres::support::invoke_pg_as_cpp([relation, slot] {
			return initialize_column_write_state(
				relation, RelationGetDescr(relation), slot->tts_tableOid,
				GetCurrentSubTransactionId());
		});
		bioseq::postgres::support::invoke_pg_as_cpp([relation] {
			validate_logical_replication_write(relation);
		});
		const auto *amc = bioseq::postgres::support::invoke_pg_as_cpp([relation] {
			return &bioseq::storage::get_table_access_cache(relation);
		});

		{
			bioseq::postgres::support::memory_context_scope ctx_guard{
				get_column_write_per_tuple_context(write_state)};

			bioseq::postgres::support::invoke_pg_as_cpp([slot] {
				slot_getallattrs(slot);
			});

			Datum *detoasted_values = detoast_values_for_heap_tuple(
				slot->tts_tupleDescriptor, slot->tts_values,
				slot->tts_isnull);

			/* rewrite SEQ columns to locator stubs. */
			std::vector<std::optional<bioseq::storage::sequence_locator>>
				per_col_locator;
			std::vector<std::optional<
				bioseq::storage::pending_sequence_source>> pending_sources;
			Datum *values_for_write = rewrite_seq_columns_to_stubs(
				relation, slot, write_state, *amc, detoasted_values,
				per_col_locator, pending_sources);

			const uint64_t written_row_number =
				bioseq::postgres::support::invoke_pg_as_cpp(
					[write_state, values_for_write, slot] {
						return write_column_row(
							write_state, values_for_write, slot->tts_isnull);
					});
			slot->tts_tid = bioseq::postgres::support::invoke_pg_as_cpp(
				[written_row_number] {
					return row_number_to_tid(written_row_number);
				});
			publish_sequence_sources_after_parent_row(pending_sources);

			/* build every UPDATE_ON_INSERT index (per SEQ column)
			 * via the new chunked-storage drivers. detoasted_values carries the
			 * ORIGINAL payload (pre-stub) so the index sees the real bytes. */
			bioseq::storage::indexing::index_build_on_insert_columns(
				RelationGetRelid(relation), amc->sequence_attribute_numbers,
				slot->tts_isnull, detoasted_values, per_col_locator,
				written_row_number,
				get_column_write_per_tuple_context(write_state));
		}
		bioseq::postgres::support::invoke_pg_as_cpp([write_state] {
			MemoryContextReset(get_column_write_per_tuple_context(write_state));
		});
		bioseq::postgres::support::invoke_pg_as_cpp([relation] {
			pgstat_count_heap_insert(relation, 1);
		});
	});
	Assert(write_state != nullptr);
	Assert(error_context_stack == &cache_restore.callback);
	bioseq::settings::column_cache::enabled = cache_restore.previous_value;
	cache_restore.armed = false;
	error_context_stack = cache_restore.callback.previous;
}


static void
bioseq_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
									  CommandId cid, int options,
									  BulkInsertState bistate, uint32_t spec_token)
{
	previous_cache_enabled_state = bioseq::settings::column_cache::enabled;
	bioseq::settings::column_cache::enabled = false;

	ordinary_insert_page_cache_restore cache_restore{
		.callback = {
			.previous = error_context_stack,
			.callback = &restore_ordinary_insert_page_cache_on_error,
			.arg = nullptr,
		},
		.previous_value = previous_cache_enabled_state,
		.armed = true,
	};
	cache_restore.callback.arg = &cache_restore;
	error_context_stack = &cache_restore.callback;
	column_write_state *write_state = nullptr;
	run_tableam_cpp_checked("bioseq speculative tuple insert",
		[relation, slot, &write_state] {
			if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
			{
				bioseq::postgres::support::invoke_pg_as_cpp([] {
					bioseq_log<logging_level::error, logging_target::postgres>(
						"bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
				});
			}
				write_state = bioseq::postgres::support::invoke_pg_as_cpp(
				[relation, slot] {
					return initialize_column_write_state(
						relation, RelationGetDescr(relation),
						slot->tts_tableOid, GetCurrentSubTransactionId());
				});
			bioseq::postgres::support::invoke_pg_as_cpp([relation] {
				validate_logical_replication_write(relation);
			});
			const auto *amc = bioseq::postgres::support::invoke_pg_as_cpp([relation] {
				return &bioseq::storage::get_table_access_cache(relation);
			});

			{
				bioseq::postgres::support::memory_context_scope ctx_guard{
					get_column_write_per_tuple_context(write_state)};

				bioseq::postgres::support::invoke_pg_as_cpp([slot] {
					slot_getallattrs(slot);
				});

				Datum *detoasted_values = detoast_values_for_heap_tuple(
					slot->tts_tupleDescriptor, slot->tts_values,
					slot->tts_isnull);

				std::vector<std::optional<bioseq::storage::sequence_locator>>
					per_col_locator;
				std::vector<std::optional<
					bioseq::storage::pending_sequence_source>> pending_sources;
				Datum *values_for_write = rewrite_seq_columns_to_stubs(
					relation, slot, write_state, *amc, detoasted_values,
					per_col_locator, pending_sources);

				const uint64_t storage_id =
					bioseq::postgres::support::invoke_pg_as_cpp([relation] {
						return lookup_storage_id(relation->rd_locator);
					});
				const uint64_t written_row_number =
					bioseq::postgres::support::invoke_pg_as_cpp(
						[write_state, values_for_write, slot] {
							return write_column_row(
								write_state, values_for_write,
								slot->tts_isnull);
						});
				bioseq::postgres::support::invoke_pg_as_cpp(
					[relation, storage_id, written_row_number] {
						update_row_mask(
							relation->rd_locator, storage_id, nullptr,
							written_row_number);
					});
					slot->tts_tid = bioseq::postgres::support::invoke_pg_as_cpp(
						[written_row_number] {
							return row_number_to_tid(written_row_number);
						});
					const SubTransactionId subtransaction_id =
						bioseq::postgres::support::invoke_pg_as_cpp([] {
							return GetCurrentSubTransactionId();
						});
					pending_speculative_sources speculative{
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
				const pending_speculative_key key{
					RelationGetRelid(relation), written_row_number};
				if (!access_pending_speculative_sources().emplace(
						key, std::move(speculative)).second) {
					bioseq::postgres::support::invoke_pg_as_cpp([] {
						bioseq_log<logging_level::error, logging_target::postgres>(
							"bioseq storage: duplicate speculative source publication key");
					});
				}
			}
			bioseq::postgres::support::invoke_pg_as_cpp([write_state] {
				MemoryContextReset(
					get_column_write_per_tuple_context(write_state));
			});
			bioseq::postgres::support::invoke_pg_as_cpp([relation] {
				pgstat_count_heap_insert(relation, 1);
			});
		});
	Assert(write_state != nullptr);
	Assert(error_context_stack == &cache_restore.callback);
	cache_restore.armed = false;
	error_context_stack = cache_restore.callback.previous;
}


static void
bioseq_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
									uint32_t spec_token, bool succeeded)
{
	(void)spec_token;
	ordinary_insert_page_cache_restore cache_restore{
		.callback = {
			.previous = error_context_stack,
			.callback = &restore_ordinary_insert_page_cache_on_error,
			.arg = nullptr,
		},
		.previous_value = previous_cache_enabled_state,
		.armed = true,
	};
	cache_restore.callback.arg = &cache_restore;
	error_context_stack = &cache_restore.callback;
	run_tableam_cpp_checked("bioseq complete speculative tuple insert",
		[relation, slot, succeeded] {
			const uint64_t storage_id =
				bioseq::postgres::support::invoke_pg_as_cpp([relation] {
					return lookup_storage_id(relation->rd_locator);
				});
			/* Set lock for relation until transaction ends. */
			bioseq::postgres::support::invoke_pg_as_cpp([storage_id] {
				(void)DirectFunctionCall1(
					pg_advisory_xact_lock_int8,
					Int64GetDatum(static_cast<int64_t>(storage_id)));
			});
			const std::uint64_t row_number =
				bioseq::postgres::support::invoke_pg_as_cpp([slot] {
					return tid_to_row_number(slot->tts_tid);
				});
			const pending_speculative_key key{
				RelationGetRelid(relation), row_number};
			auto pending = access_pending_speculative_sources().find(key);
			if (pending == access_pending_speculative_sources().end()) {
				bioseq::postgres::support::invoke_pg_as_cpp([] {
					bioseq_log<logging_level::error, logging_target::postgres>(
						"bioseq storage: speculative source publication authority is missing");
				});
			}
			if (succeeded) {
				for (auto& source : pending->second.sources) {
					Assert(source.pending());
					bioseq::postgres::support::invoke_pg_as_cpp([&source] {
						source.publish_after_parent_row();
					});
					Assert(!source.pending());
				}
			} else {
				for (auto& source : pending->second.sources) {
					Assert(source.pending());
					bioseq::postgres::support::invoke_pg_as_cpp([&source] {
						source.invalidate_without_parent_row();
					});
					Assert(!source.pending());
				}
			}
			access_pending_speculative_sources().erase(pending);
		});
	Assert(error_context_stack == &cache_restore.callback);
	bioseq::settings::column_cache::enabled = previous_cache_enabled_state;
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
			bioseq::postgres::support::invoke_pg_as_cpp([] {
				bioseq_log<logging_level::error, logging_target::postgres>(
					"bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
			});
		}
		column_write_state *write_state =
			bioseq::postgres::support::invoke_pg_as_cpp([relation, slots] {
				return initialize_column_write_state(
					relation, RelationGetDescr(relation),
					slots[0]->tts_tableOid, GetCurrentSubTransactionId());
			});
		bioseq::postgres::support::invoke_pg_as_cpp([relation] {
			validate_logical_replication_write(relation);
		});
		const auto *amc = bioseq::postgres::support::invoke_pg_as_cpp([relation] {
			return &bioseq::storage::get_table_access_cache(relation);
		});

		{
			bioseq::postgres::support::memory_context_scope ctx_guard{
				get_column_write_per_tuple_context(write_state)};

			for (int i = 0; i < ntuples; i++)
			{
			TupleTableSlot *tuple_slot = slots[i];

			bioseq::postgres::support::invoke_pg_as_cpp([tuple_slot] {
				slot_getallattrs(tuple_slot);
			});

			Datum *detoasted_values = detoast_values_for_heap_tuple(
				tuple_slot->tts_tupleDescriptor,
				tuple_slot->tts_values, tuple_slot->tts_isnull);

				std::vector<std::optional<bioseq::storage::sequence_locator>>
					per_col_locator;
				std::vector<std::optional<
					bioseq::storage::pending_sequence_source>> pending_sources;
				Datum *values_for_write = rewrite_seq_columns_to_stubs(
				relation, tuple_slot, write_state, *amc,
				detoasted_values, per_col_locator, pending_sources);

			const uint64_t written_row_number =
				bioseq::postgres::support::invoke_pg_as_cpp(
					[write_state, values_for_write, tuple_slot] {
						return write_column_row(
							write_state, values_for_write,
							tuple_slot->tts_isnull);
					});
				publish_sequence_sources_after_parent_row(pending_sources);

				bioseq::storage::indexing::index_build_on_insert_columns(
					RelationGetRelid(relation), amc->sequence_attribute_numbers,
				tuple_slot->tts_isnull, detoasted_values,
				per_col_locator, written_row_number,
				get_column_write_per_tuple_context(write_state));

				EState *estate =
					bioseq::postgres::support::invoke_pg_as_cpp([relation] {
						return bioseq::storage::metadata::tuple_ops::create_relation_executor_state(relation);
					});

			ResultRelInfo *result_rel_info =
					bioseq::postgres::support::invoke_pg_as_cpp([] {
						return makeNode(ResultRelInfo);
					});
				bioseq::postgres::support::invoke_pg_as_cpp(
				[result_rel_info, relation] {
					InitResultRelInfo(
						result_rel_info, relation, 1, nullptr, 0);
				});

			bioseq::postgres::support::invoke_pg_as_cpp([result_rel_info] {
				ExecOpenIndices(result_rel_info, false);
			});

			if (relation->rd_att->constr)
				bioseq::postgres::support::invoke_pg_as_cpp(
					[result_rel_info, tuple_slot, estate] {
						ExecConstraints(
							result_rel_info, tuple_slot, estate);
					});

			bioseq::postgres::support::invoke_pg_as_cpp([result_rel_info] {
				ExecCloseIndices(result_rel_info);
				});

				bioseq::postgres::support::invoke_pg_as_cpp([estate] {
					AfterTriggerEndQuery(estate);
				});
				bioseq::postgres::support::invoke_pg_as_cpp([estate] {
					ExecCloseResultRelations(estate);
				});
				bioseq::postgres::support::invoke_pg_as_cpp([estate] {
					ExecCloseRangeTableRelations(estate);
				});

				bioseq::postgres::support::invoke_pg_as_cpp([estate] {
					ExecResetTupleTable(estate->es_tupleTable, false);
				});
				bioseq::postgres::support::invoke_pg_as_cpp([estate] {
					FreeExecutorState(estate);
				});

			tuple_slot->tts_tid = bioseq::postgres::support::invoke_pg_as_cpp(
				[written_row_number] {
					return row_number_to_tid(written_row_number);
				});

			bioseq::postgres::support::invoke_pg_as_cpp([write_state] {
				MemoryContextReset(
					get_column_write_per_tuple_context(write_state));
				});
			}
		}
		bioseq::postgres::support::invoke_pg_as_cpp([relation, ntuples] {
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
	const auto &amc = bioseq::storage::get_table_access_cache(relation);
	if (amc.sequence_attribute_numbers.empty()) return;
	Snapshot read_snapshot = snapshot != nullptr ? snapshot : GetTransactionSnapshot();
	MemoryContext read_context = create_column_scan_memory_context();
	{
		bioseq::postgres::support::memory_context_scope context_guard{read_context};
		TupleDesc descriptor = RelationGetDescr(relation);
		bioseq::postgres::support::postgres_bitmapset attributes = bioseq::postgres::support::postgres_bitmapset::make_member_range(descriptor->natts);
		column_read_state *read_state = init_bioseq_read_state(
			relation, descriptor, attributes, NIL, read_context, read_snapshot,
			/*randomAccess=*/true, nullptr);
		Datum *values = static_cast<Datum *>(palloc0(
			sizeof(Datum) * static_cast<std::size_t>(descriptor->natts)));
		bool *nulls = static_cast<bool *>(palloc0(
			sizeof(bool) * static_cast<std::size_t>(descriptor->natts)));
		if (read_column_row_by_number(read_state, row_number, values, nulls)) {
			for (const std::int16_t attno : amc.sequence_attribute_numbers) {
				const int index = attno - 1;
				if (index < 0 || index >= descriptor->natts || nulls[index]) continue;
				const auto *header = reinterpret_cast<const bioseq::pg_types::seq *>(
					DatumGetPointer(values[index]));
				if (!bioseq::storage::is_locator_stub(header)) continue;
				const auto locator = bioseq::storage::decode_locator_from_stub(header);
				bioseq::storage::sequence_source_version_manager::invalidate_epoch(
					locator.source_epoch);
			}
		}
		end_column_read(read_state);
	}
	MemoryContextDelete(read_context);
}


static TM_Result
bioseq_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
						  Snapshot snapshot, Snapshot crosscheck, bool wait,
						  TM_FailureData *tmfd, bool changing_part)
{
	if (bioseq::storage::is_registered_sequence_fragment_relation(
			RelationGetRelid(relation))) {
		bioseq_log<logging_level::error, logging_target::postgres>(
			"bioseq storage: registered sequence fragments are immutable; DELETE is forbidden");
	}
	uint64_t row_number = tid_to_row_number(*tid);
	uint64_t storage_id = lookup_storage_id(relation->rd_locator);
	/* Set lock for relation until transaction ends */
	DirectFunctionCall1(pg_advisory_xact_lock_int8,
						Int64GetDatum((int64_t) storage_id));

	invalidate_sequence_source_epochs_for_row(relation, row_number, snapshot);
	if (!update_row_mask(relation->rd_locator, storage_id,  snapshot, row_number))
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
		bioseq_log<logging_level::error, logging_target::postgres>(
			"bioseq storage: registered sequence fragments are immutable; UPDATE is forbidden");
	}

	uint64_t row_number = tid_to_row_number(*otid);
	uint64_t storage_id = lookup_storage_id(relation->rd_locator);

	/* Set lock for relation until transaction ends */
	DirectFunctionCall1(pg_advisory_xact_lock_int8,
						Int64GetDatum((int64_t) storage_id));

	invalidate_sequence_source_epochs_for_row(relation, row_number, snapshot);
	if (!update_row_mask(relation->rd_locator, storage_id, snapshot, row_number))
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
	uint64_t row_number = tid_to_row_number(*tid);
	column_read_state *read_state = NULL;

	int natts = relation->rd_att->natts;
	bioseq::postgres::support::postgres_bitmapset attr_needed = bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);

	List *scan_qual = NIL;

	bool random_access = true;

	read_state = init_bioseq_read_state(relation,
									slot->tts_tupleDescriptor,
									attr_needed, scan_qual,
									CurrentMemoryContext, // to be checked
									GetTransactionSnapshot(), random_access,
									NULL);

	read_column_row_by_number(read_state, row_number,
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
									   TransactionId *freeze_xid,
									   MultiXactId *minmulti)
{
	if (bioseq::storage::is_registered_sequence_fragment_relation(
			RelationGetRelid(rel))) {
		bioseq_log<logging_level::error, logging_target::postgres>(
			"bioseq storage: destructive rewrite of a registered sequence fragment relation is forbidden");
	}
	if (persistence == RELPERSISTENCE_TEMP)
	{
		bioseq_log<logging_level::error, logging_target::postgres>(
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
		mark_write_state_relation_dropped(rel->rd_locator.relNumber, GetCurrentSubTransactionId());

		delete_metadata_rows(rel->rd_locator);

		/* cascade-drop sub-tables under the OLD
		 * relfilenode triple. */
		bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
			rel->rd_locator.dbOid,
			rel->rd_locator.spcOid,
			rel->rd_locator.relNumber);
	}
	*freeze_xid = RecentXmin;
	*minmulti = GetOldestMultiXactId();
	SMgrRelation srel = RelationCreateStorage(*newrnode, persistence, true);

	bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_relation_set_new_filenode() initialising columnar storage");
	initialize_column_storage(srel, allocate_column_storage_id());
	bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_relation_set_new_filenode() initialising columnar options");
	initialize_column_options(rel->rd_id);

	smgrclose(srel);

	/* we will lazily initialize metadata in first stripe reservation */
}


static void
bioseq_relation_nontransactional_truncate(Relation rel)
{
	if (bioseq::storage::is_registered_sequence_fragment_relation(
			RelationGetRelid(rel))) {
		bioseq_log<logging_level::error, logging_target::postgres>(
			"bioseq storage: TRUNCATE of a registered sequence fragment relation is forbidden");
	}
	RelFileLocator relfilelocator = rel->rd_locator;

	drop_write_state_outside_transaction(relfilelocator.relNumber);
	/* Delete old relfilelocator metadata */
	delete_metadata_rows(relfilelocator);

	/*
	 * No need to set new relfilelocator, since the table was created in this
	 * transaction and no other transaction can see this relation yet. We
	 * can just truncate the relation.
	 *
	 * This is similar to what is done in heapam_relation_nontransactional_truncate.
	 */
	RelationTruncate(rel, 0);

	bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_relation_nontransactional_truncate() initialising columnar storage");
	uint64_t storage_id = allocate_column_storage_id();

	initialize_column_storage(RelationGetSmgr(rel), storage_id);
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

	for (ForkNumber fork_num = static_cast<ForkNumber>(MAIN_FORKNUM + 1);
		fork_num <= MAX_FORKNUM; fork_num = static_cast<ForkNumber>(static_cast<int>(fork_num) + 1))
	{
		if (smgrexists(srel, fork_num))
		{
			smgrcreate(srel, fork_num, false);

			/*
			 * WAL log creation if the relation is persistent, or
			 * this is the init fork of an unlogged relation.
			 */
			if (RelationIsPermanent(rel) ||
				(persistence == RELPERSISTENCE_UNLOGGED &&
					fork_num == INIT_FORKNUM))
				log_smgrcreate(newrnode, fork_num);

			RelationCopyStorage(RelationGetSmgr(rel), srel,
							fork_num, persistence);
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
bioseq_relation_copy_for_cluster(Relation old_heap, Relation new_heap,
									   Relation old_index, bool use_sort,
									   TransactionId oldest_xmin,
								   TransactionId *xid_cutoff,
								   MultiXactId *multi_cutoff,
								   double *num_tuples,
								   double *tups_vacuumed,
								   double *tups_recently_dead)
{
	TupleDesc source_desc = RelationGetDescr(old_heap);
	TupleDesc target_desc = RelationGetDescr(new_heap);

	if (old_index != NULL || use_sort)
	{
		bioseq_report<logging_level::error, logging_target::postgres>().with_sql_error_code(ERRCODE_FEATURE_NOT_SUPPORTED)
			.with_message("clustering columnar tables using indexes is not supported");
	}

	/*
	 * copy_table_data in cluster.c assumes tuple descriptors are exactly
	 * the same. Even dropped columns exist and are marked as attisdropped
	 * in the target relation.
	 */
	Assert(source_desc->natts == target_desc->natts);

	/* read settings from old heap, relfilelocator will be swapped at the end */
	column_options bioseq_column_options =
		read_column_options(old_heap->rd_id)
			.value_or(column_options{});

	/*
	 * CLUSTER allocates a new relfilenode for NewHeap.
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
	column_write_state *write_state = initialize_column_write_state(new_heap, target_desc,
		RelationGetRelid(new_heap), GetCurrentSubTransactionId());
	auto* write_caches = ensure_write_side_seq_caches(new_heap, write_state);
	const auto cap_bytes = write_caches->cap_bytes_snapshot;

	/* we need all columns */
	int natts = old_heap->rd_att->natts;
	bioseq::postgres::support::postgres_bitmapset attr_needed = bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);

	/* no quals for table rewrite */
	List *scan_qual = NIL;

	/* Unlike heap tuples, columnar dead-row state lives in an MVCC row-mask
	 * relation. SnapshotAny would see both the superseded and current mask and
	 * can therefore copy an UPDATE's dead version into the rewritten table.
	 * Keep one registered transaction snapshot authoritative for the main row,
	 * row mask, and sequence-source fragments throughout the rewrite. */
	Snapshot snapshot = RegisterSnapshot(GetTransactionSnapshot());

	MemoryContext scan_context = create_column_scan_memory_context();
	bool random_access = false;
	column_read_state *read_state = init_bioseq_read_state(old_heap, source_desc,
														attr_needed, scan_qual,
														scan_context, snapshot,
														random_access,
														NULL);

	/* Read-side seq cache: hydrates OldHeap's stubs to real payloads. */
	auto* read_caches = bioseq::storage::alloc_seq_caches(scan_context);
	read_caches->side = bioseq::storage::seq_caches::access_side::read;
	read_caches->lock_mode = AccessShareLock;
	read_caches->read_state_mctx = scan_context;
	read_caches->main_database_oid   = old_heap->rd_locator.dbOid;
	read_caches->main_tablespace_oid = old_heap->rd_locator.spcOid;
	read_caches->main_relfilenode    = old_heap->rd_locator.relNumber;

	Datum *values = bioseq::postgres::support::allocate_zeroed_array<Datum>(source_desc->natts);
	bool *nulls = bioseq::postgres::support::allocate_zeroed_array<bool>(source_desc->natts);

	const auto &source_amc = bioseq::storage::get_table_access_cache(old_heap);
	const std::uint64_t new_main_storage_id =
		column_storage_id(new_heap, false);

	*num_tuples = 0;

	while (read_next_column_row(read_state, values, nulls, NULL))
	{
		std::vector<bioseq::storage::pending_sequence_source> pending_sources;
		/* Hydrate any SEQ stub columns from OldHeap's sub-tables. */
		for (std::int16_t attno : source_amc.sequence_attribute_numbers) {
			const int idx = attno - 1;
			if (idx < 0 || idx >= source_desc->natts) continue;
			if (nulls[idx]) continue;
			auto* hdr = reinterpret_cast<const bioseq::pg_types::seq*>(
				DatumGetPointer(values[idx]));
			if (!bioseq::storage::is_locator_stub(hdr)) continue;
			const auto loc = bioseq::storage::decode_locator_from_stub(hdr);
			bioseq::storage::validate_locator_flat_materialization_length(loc);
			bioseq::pg_types::seq* real =
				bioseq::storage::bioseq_default_storage_manager::fetch_payload(
					old_heap->rd_locator.dbOid, old_heap->rd_locator.spcOid,
					old_heap->rd_locator.relNumber, attno, loc,
					read_state->snapshot, scan_context, read_caches);
			values[idx] = PointerGetDatum(real);
		}

		/* Rewrite SEQ columns into stubs targeting NewHeap's sub-tables.
		 * The write-side cache shares the destination write state's lifetime. */
		for (std::int16_t attno : source_amc.sequence_attribute_numbers) {
			const int idx = attno - 1;
			if (idx < 0 || idx >= source_desc->natts) continue;
			if (nulls[idx]) continue;
			auto* hdr = reinterpret_cast<const bioseq::pg_types::seq*>(
				DatumGetPointer(values[idx]));
			if (bioseq::storage::is_locator_stub(hdr)) continue;
			auto pending =
				bioseq::storage::bioseq_default_storage_manager::insert_payload_as_fragments(
					RelationGetRelid(old_heap), new_main_storage_id,
					new_heap->rd_locator.dbOid, new_heap->rd_locator.spcOid,
					new_heap->rd_locator.relNumber,
					attno, target_desc, hdr, cap_bytes, write_caches);
			const auto loc = pending.locator();
			bioseq::pg_types::seq* stub = bioseq::storage::alloc_locator_stub_in(
				get_column_write_per_tuple_context(write_state),
				static_cast<std::uint8_t>(hdr->alphabet_code),
				static_cast<std::uint8_t>(hdr->storage_unit_byte_width),
				hdr->padding_letters,
				loc.logical_length,
				loc);
			values[idx] = PointerGetDatum(stub);
			pending_sources.push_back(std::move(pending));
		}

		write_column_row(write_state, values, nulls);
		for (auto& source : pending_sources) {
			source.publish_after_parent_row();
		}
		(*num_tuples)++;
	}

	*tups_vacuumed = 0;

	read_caches->close_all(/*commit=*/true);
	bioseq::storage::destroy_seq_caches(read_caches);

	end_column_read(read_state);
	UnregisterSnapshot(snapshot);
	MemoryContextDelete(scan_context);

	/* cascade-drop OldHeap's sub-tables before the
	 * finish_heap_swap. After the swap the relfilenodes are swapped and
	 * OldHeap's sub-tables become orphans under the (now-defunct) old
	 * triple. The reaper is the long-tail safety net; this call makes
	 * the cleanup deterministic. */
	bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
		old_heap->rd_locator.dbOid,
		old_heap->rd_locator.spcOid,
		old_heap->rd_locator.relNumber);
}


/*
 * truncate_and_compact_column_stripes will combine last n stripes so they can fit
 * maximum number of rows per stripe. Stripes that are going to be combined are deleted
 * and new stripe will be written at the end of untouched last stripe. We also include
 * information about deleted rows for this implementation.
 */
static bool
truncate_and_compact_column_stripes(Relation rel, int elevel)
{
	uint64_t total_row_number_count = 0;
	uint32_t starting_stripe_list_position = 0;

	TupleDesc tuple_desc = RelationGetDescr(rel);

	if (tuple_desc->natts == 0)
	{
		bioseq_log_pg(elevel, "\"{}\": stopping vacuum due to zero column table",
			   RelationGetRelationName(rel));
		return false;
	}

	/* Get current columnar options */
	column_options bioseq_column_options =
		read_column_options(rel->rd_id)
			.value_or(column_options{});

	/* Get all stripes in reverse order */
	List *stripe_metadata_list = list_stripes_for_relation(rel->rd_locator, BackwardScanDirection);
	/* Empty table nothing to do */
	if (stripe_metadata_list == NIL)
	{
		bioseq_log_pg(elevel, "\"{}\": stopping vacuum due to empty table",
			   RelationGetRelationName(rel));
		return false;
	}

	//ListCell *lc = NULL;

	uint32_t last_stripe_deleted_rows = 0;

	Size total_decompressed_stripe_length = 0;

	for (auto stripe_metadata_entry : bioseq::postgres::support::postgres_list<stripe_metadata>(stripe_metadata_list))
	{
		last_stripe_deleted_rows = deleted_rows_for_stripe(rel->rd_locator,
														 stripe_metadata_entry->chunk_count,
														 stripe_metadata_entry->id);
		total_decompressed_stripe_length +=
			decompressed_length_for_stripe(rel->rd_locator, stripe_metadata_entry->id);

		if (total_decompressed_stripe_length >= 1024000000)
		{
			break;
		}

		uint64_t stripe_row_limit = stripe_metadata_entry->row_count - last_stripe_deleted_rows;

		if ((total_row_number_count + stripe_row_limit >= bioseq_column_options.stripe_row_limit))
		{
			break;
		}

		total_row_number_count += stripe_row_limit;
		starting_stripe_list_position++;
	}

	/*
	 * One stripe that is "full" so do nothing.
	 */
	if (starting_stripe_list_position == 0)
	{
		return false;
	}

	/*
	 * There is only one stripe that is candidate. Maybe we should vacuum
	 * it if condition is met.
	 */
	else if (starting_stripe_list_position == 1)
	{
		/* Maybe we should vacuum only one stripe if count of
		 * deleted rows is higher than 20 percent.
		 */
		float percentage_of_delete_rows =
			(float)last_stripe_deleted_rows / (float)(total_row_number_count + last_stripe_deleted_rows);

		bool should_vacuum_only_stripe = percentage_of_delete_rows > 0.2f;

		if (!should_vacuum_only_stripe)
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
	column_write_state *write_state = begin_column_write(rel->rd_locator,
														bioseq_column_options,
														tuple_desc);

	/* we need all columns */
	int natts = rel->rd_att->natts;
	bioseq::postgres::support::postgres_bitmapset attr_needed = bioseq::postgres::support::postgres_bitmapset::make_member_range(natts);

	/* no quals for table rewrite */
	List *scan_qual = NIL;

	MemoryContext scan_context = create_column_scan_memory_context();
	bool random_access = true;
	column_read_state *read_state = init_bioseq_read_state(rel, tuple_desc,
														attr_needed, scan_qual,
														scan_context, SnapshotAny,
														random_access,
														NULL);

	set_stripe_read_state(read_state,
								   static_cast<stripe_metadata*>(list_nth(stripe_metadata_list, starting_stripe_list_position - 1)));

	Datum *values = bioseq::postgres::support::allocate_zeroed_array<Datum>(tuple_desc->natts);
	bool *nulls = bioseq::postgres::support::allocate_zeroed_array<bool>(tuple_desc->natts);

	/* we don't need to know row_number here */
	while (read_next_column_row(read_state, values, nulls, NULL))
	{
		write_column_row(write_state, values, nulls);
	}

	uint64_t new_data_reservation;

	if (list_length(stripe_metadata_list) > starting_stripe_list_position)
	{
		stripe_metadata *mtd = static_cast<stripe_metadata*>(list_nth(stripe_metadata_list, starting_stripe_list_position));
		new_data_reservation = mtd->file_offset + mtd->data_length - 1;
	}
	else
	{
		stripe_metadata *mtd = static_cast<stripe_metadata*>(list_nth(stripe_metadata_list, starting_stripe_list_position - 1));
		new_data_reservation = mtd->file_offset;
	}

	truncate_column_storage(rel, new_data_reservation);

	end_column_write(write_state);
	end_column_read(read_state);
	MemoryContextDelete(scan_context);

	for (int i = 0; i < starting_stripe_list_position; i++)
	{
		stripe_metadata *metadata = static_cast<stripe_metadata*>(list_nth(stripe_metadata_list, i));
		delete_metadata_rows_for_stripe(rel->rd_locator, metadata->id);
	}

	PopActiveSnapshot();

	return true;
}

/*
 * column_table_tuple_count returns the number of tuples that columnar
 * table with relationId has by using stripe metadata.
 */
static uint64_t
column_table_tuple_count(Relation relation)
{
	List *stripe_list = list_stripes_for_relation(relation->rd_locator, ForwardScanDirection);
	uint64_t tuple_count = 0;

	for (auto stripe : bioseq::postgres::support::postgres_list<stripe_metadata>(stripe_list))
	{
		tuple_count += stripe->row_count;
	}

	return tuple_count;
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
		bioseq::postgres::support::page_cache_disable_scope cache_guard{
			bioseq::settings::column_cache::enabled};
		bioseq::postgres::support::invoke_pg_as_cpp([rel, params] {
	pgstat_progress_start_command(PROGRESS_COMMAND_VACUUM,
 									RelationGetRelid(rel));

	/*
	 * If metapage version of relation is older, then we hint users to VACUUM
	 * the relation in validate_column_metapage_version. So if needed, upgrade
	 * the metapage before doing anything.
	 */
	const bool upgrades_storage = true;
	ensure_current_column_storage_format(rel, upgrades_storage);

	int elevel = (params->options & VACOPT_VERBOSE) ? INFO : DEBUG2;

	/* this should have been resolved by vacuum.c until now */
	Assert(params->truncate != VACOPTVALUE_UNSPECIFIED);

	if (params->options & VACOPT_VERBOSE)
	{
		log_relation_statistics(rel, elevel);
	}

	/*
	 * We don't have updates, deletes, or concurrent updates, so all we
	 * care for now is truncating the unused space at the end of storage.
	 */
	if (params->truncate == VACOPTVALUE_ENABLED)
	{
		truncate_column_storage_tail(rel, elevel);
	}

	BlockNumber new_rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

	/* get the number of indexes */
	List *index_list = RelationGetIndexList(rel);
	int nindexes = list_length(index_list);

	VacuumCutoffs cutoffs;
	vacuum_get_cutoffs(rel, params, &cutoffs);

	Assert(MultiXactIdPrecedesOrEquals(cutoffs.MultiXactCutoff, cutoffs.OldestMxact));
	Assert(TransactionIdPrecedesOrEquals(cutoffs.FreezeLimit, cutoffs.OldestXmin));

	/*
	 * BioseqColumn storage doesn't hold any transaction IDs, so we can always
	 * just advance to the most aggressive value.
	 */
	TransactionId new_rel_frozen_xid = cutoffs.OldestXmin;
	MultiXactId new_relmin_mxid = cutoffs.OldestMxact;
	double new_live_tuples = column_table_tuple_count(rel);

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
							new_rel_frozen_xid, new_relmin_mxid,
						&frozenxid_updated, &minmulti_updated, false);
#else
	vac_update_relstats(rel, new_rel_pages, new_live_tuples,
						new_rel_allvisible, nindexes > 0,
							new_rel_frozen_xid, new_relmin_mxid,
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
 * log_relation_statistics logs statistics as the output of the VACUUM VERBOSE.
 */
static void
log_relation_statistics(Relation rel, int elevel)
{
	ListCell *stripe_metadata_cell = NULL;
	RelFileLocator relfilelocator = rel->rd_locator;
	StringInfo info_buf = makeStringInfo();

	int compression_stats[static_cast<int>(bioseq::compression_algorithm::count)] = { 0 };
	uint64_t total_stripe_length = 0;
	uint64_t tuple_count = 0;
	uint64_t chunk_count = 0;
	TupleDesc tupdesc = RelationGetDescr(rel);
	uint64_t dropped_chunks_with_data = 0;
	uint64_t total_decompressed_length = 0;

	List *stripe_list = list_stripes_for_relation(relfilelocator, ForwardScanDirection);
	int stripe_count = list_length(stripe_list);

	MemoryContext relation_stats_ctx =
		AllocSetContextCreate(CurrentMemoryContext, "Vacuum Relation Stats Context",
							  ALLOCSET_SMALL_SIZES);

	{
		bioseq::postgres::support::memory_context_scope ctx_guard{relation_stats_ctx};

		for (auto stripe : bioseq::postgres::support::postgres_list<stripe_metadata>(stripe_list))
		{
			stripe_skip_list *skiplist = read_stripe_skip_list(relfilelocator, stripe->id,
														  RelationGetDescr(rel),
														  stripe->chunk_count,
														  GetTransactionSnapshot());
			for (uint32_t column = 0; column < skiplist->column_count; column++)
			{
				bool attr_dropped = TupleDescAttr(tupdesc, column)->attisdropped;
				for (uint32_t chunk = 0; chunk < skiplist->chunk_count; chunk++)
				{
					chunk_skip_node *skipnode =
						&skiplist->chunk_skip_nodes[column][chunk];

					/* ignore zero length chunks for dropped attributes */
					if (skipnode->value_stream_length > 0)
					{
						compression_stats[
							static_cast<int>(skipnode->value_compression_algorithm)]++;
						chunk_count++;

						if (attr_dropped)
						{
							dropped_chunks_with_data++;
						}
					}

					/*
					 * We don't compress exists buffer, so its compressed & decompressed
					 * lengths are the same.
					 */
					total_decompressed_length += skipnode->exists_stream_length;
					total_decompressed_length += skipnode->value_decompressed_length;
				}
			}

			tuple_count += stripe->row_count;
			total_stripe_length += stripe->data_length;

			MemoryContextReset(relation_stats_ctx);
		}
	}

	uint64_t rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
	RelationCloseSmgr(rel);

	const std::int64_t storage_id = static_cast<std::int64_t>(
		column_storage_id(rel, false));

	double compression_rate = total_stripe_length ?
								 (double) total_decompressed_length / total_stripe_length :
								 1.0;

	appendStringInfo(info_buf, "storage id: %ld\n", storage_id);
	appendStringInfo(info_buf, "total file size: %ld, total data size: %ld\n",
						 rel_pages * BLCKSZ, total_stripe_length);
	appendStringInfo(info_buf, "compression rate: %.2fx\n", compression_rate);
	appendStringInfo(info_buf,
					 "total row count: %ld, stripe count: %d, "
					 "average rows per stripe: %ld\n",
						 tuple_count, stripe_count,
						 stripe_count ? tuple_count / stripe_count : 0);
	appendStringInfo(info_buf,
					 "chunk count: %ld"
					 ", containing data for dropped columns: %ld",
						 chunk_count, dropped_chunks_with_data);
	for (int compression_algorithm = 0; compression_algorithm < static_cast<int>(bioseq::compression_algorithm::count); compression_algorithm++)
	{
		const char *compression_name = compression_algorithm_name(static_cast<bioseq::compression_algorithm>(compression_algorithm));

		/* skip if this compression algorithm has not been compiled */
		if (compression_name == NULL)
		{
			continue;
		}

		/* skip if no chunks use this compression type */
		if (compression_stats[compression_algorithm] == 0)
		{
			continue;
		}

		appendStringInfo(info_buf,
							 ", %s compressed: %d",
							 compression_name,
							 compression_stats[compression_algorithm]);
	}
	appendStringInfoString(info_buf, "\n");

	bioseq_log_pg(elevel, "statistics for \"{}\":\n{}", RelationGetRelationName(rel),
			   info_buf->data);
}


/*
 * truncate_column_storage_tail truncates the unused space at the end of main fork for
 * a columnar table. This unused space can be created by aborted transactions.
 *
 * This implementation is based on heap_vacuum_rel in vacuumlazy.c with some
 * changes so it suits columnar store relations.
 */
static void
truncate_column_storage_tail(Relation rel, int elevel)
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
	if (!try_lock_relation_until_timeout(rel, AccessExclusiveLock,
											k_vacuum_truncate_lock_timeout_ms,
											k_vacuum_truncate_lock_wait_interval_ms,
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
	bool stripes_truncated = locator_pinned_fragment
		? false
		: truncate_and_compact_column_stripes(rel, elevel);
	if (locator_pinned_fragment) {
		bioseq_log_pg(elevel,
			"\"{}\": skipping row-moving compaction for locator-pinned sequence fragments",
			RelationGetRelationName(rel));
	}

	/*
	 * If we didn't truncate and combine tail stripes we could have
	 * stituation where we need to truncate storage at the end.
	 */
	if (!stripes_truncated)
	{

		/*
		* Due to the AccessExclusive lock there's no danger that
		* new stripes be added beyond highestPhysicalAddress while
		* we're truncating.
		*/
		uint64_t new_data_reservation = std::max<uint64_t>(find_highest_used_address(rel->rd_locator) + 1,
												first_logical_offset);

		BlockNumber old_rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

		if (!truncate_column_storage(rel, new_data_reservation))
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
				.with_message("\"{}\": truncated {} to {} pages",
					 RelationGetRelationName(rel),
					 old_rel_pages, new_rel_pages)
				.with_detail("{}", pg_rusage_show(&ru0));
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
 * try_lock_relation_until_timeout tries to acquire a relation lock until
 * it either succeeds or timesout. It doesn't enter wait queue and instead it
 * sleeps between lock tries.
 *
 * This is based on the lock loop in lazy_truncate_heap().
 */
static bool
try_lock_relation_until_timeout(Relation rel, LOCKMODE lock_mode, int timeout,
									   int retry_interval, bool acquire)
{
	int lock_retry = 0;

	while (true)
	{
		if (ConditionalLockRelation(rel, lock_mode))
		{
			break;
		}

		/*
		 * Check for interrupts while trying to (re-)acquire the lock
		 */
		CHECK_FOR_INTERRUPTS();

		if (!acquire && (++lock_retry > (timeout / retry_interval)))
		{
			return false;
		}

		pg_usleep(retry_interval * 1000L);
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
		reinterpret_cast<column_scan_handle>(scan);
	return bioseq_scan->read_state == nullptr;
}


static bool
bioseq_scan_analyze_next_tuple(TableScanDesc scan, TransactionId oldest_xmin,
								 double *liverows, double *deadrows,
								 TupleTableSlot *slot)
{
	/* Disable the page cache while ANALYZE collects sample rows. */
	bioseq::postgres::support::page_cache_disable_scope cache_guard{
		bioseq::settings::column_cache::enabled};

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
	MemoryContext result_context)
{
	if (provider == nullptr)
		return;
	const auto &amc = bioseq::storage::get_table_access_cache(relation);
	for (const std::int16_t attno : amc.sequence_attribute_numbers)
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
			[provider, attno, slot, index, result_context] {
				return
				bioseq::storage::promote_pg_sequence_locator(
					provider, attno, slot->tts_values[index], result_context);
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
bioseq_index_build_range_scan(Relation bioseq_column_relation,
									Relation index_relation,
									IndexInfo *index_info,
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
		bioseq_log<logging_level::error, logging_target::postgres>( "BRIN indexes on columnar tables are not supported");
	}

	if (scan)
	{
		/*
		 * Parallel scans on columnar tables are already discardad by
		 * BioseqColumnGetRelationInfoHook but be on the safe side.
		 */
		bioseq_log<logging_level::error, logging_target::postgres>( "parallel scans on columnar are not supported");
	}

	/* Disable the page cache for the duration of the index build. */
	bioseq::postgres::support::page_cache_disable_scope cache_guard{
		bioseq::settings::column_cache::enabled};

	/*
	 * Columnar tuples have no dead-row form, so outside bootstrap an explicitly
	 * registered transaction snapshot is both complete and authoritative for
	 * locator promotion.  Keeping the table scan, source catalog and fragments
	 * on this exact snapshot prevents expression-index code from consulting a
	 * different active snapshot.
	 */
	Snapshot snapshot = { 0 };
	bool owns_snapshot_registration = false;

	/*
	 * For serial index build, we begin our own scan. We may also need to
	 * register a snapshot whose lifetime is under our direct control.
	 */
	if (!IsBootstrapProcessingMode())
	{
		snapshot = RegisterSnapshot(GetTransactionSnapshot());
		owns_snapshot_registration = true;
	}
	else
	{
		snapshot = SnapshotAny;
	}

	int nkeys = 0;
	ScanKeyData *scan_key = NULL;
	bool allow_access_strategy = true;
	scan = table_beginscan_strat(bioseq_column_relation, snapshot, nkeys, scan_key,
									 allow_access_strategy, allow_sync);
	bioseq::storage::pg_sequence_source_provider *volatile source_provider = nullptr;
	if (owns_snapshot_registration)
	{
		auto *bioseq_scan = reinterpret_cast<column_scan_handle>(scan);
		bioseq_scan->defers_sequence_hydration = true;
		source_provider =
			create_index_source_provider(bioseq_column_relation, snapshot);
	}

	if (progress)
	{
		report_total_virtual_blocks(bioseq_column_relation, snapshot,
										 PROGRESS_SCAN_BLOCKS_TOTAL);
	}

	/*
	 * Set up execution state for predicate, if any.
	 * Note that this is only useful for partial indexes.
	 */
	EState *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	econtext->ecxt_scantuple = table_slot_create(bioseq_column_relation, NULL);
	ExprState *predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

	double volatile reltuples = 0;
	PG_TRY();
	{
		reltuples = build_index_from_column_rows(
			scan, index_relation, index_info, progress, callback,
			callback_state, estate, predicate,
			source_provider);
		ResetExprContext(econtext);
		if (source_provider != nullptr)
		{
			bioseq::storage::close_pg_sequence_source_provider(
				source_provider,
				/*is_commit=*/true);
			source_provider = nullptr;
		}
	}
	PG_CATCH();
	{
		if (source_provider != nullptr)
		{
			bioseq::storage::close_pg_sequence_source_provider(
				source_provider,
				/*is_commit=*/false);
			source_provider = nullptr;
		}
		PG_RE_THROW();
	}
	PG_END_TRY();
	table_endscan(scan);

	if (progress)
	{
		/* report the last "virtual" block as "done" */
		report_total_virtual_blocks(bioseq_column_relation, snapshot,
										 PROGRESS_SCAN_BLOCKS_DONE);
	}

	if (owns_snapshot_registration)
	{
		UnregisterSnapshot(snapshot);
	}

	ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
	FreeExecutorState(estate);
	index_info->ii_ExpressionsState = NIL;
	index_info->ii_PredicateState = NULL;

	return static_cast<double>(reltuples);
}


/*
 * report_total_virtual_blocks reports progress for index build based on
 * number of "virtual" blocks that given relation has.
 * "progressArrIndex" argument determines which entry in st_progress_param
 * array should be updated. In this case, we only expect PROGRESS_SCAN_BLOCKS_TOTAL
 * or PROGRESS_SCAN_BLOCKS_DONE to specify whether we want to report calculated
 * number of blocks as "done" or as "total" number of "virtual" blocks to scan.
 */
static void
report_total_virtual_blocks(Relation relation, Snapshot snapshot,
									 int progress_arr_index)
{
	/*
	 * Indeed, columnar tables might have gaps between row numbers, e.g
	 * due to aborted transactions etc. Also, ItemPointer BlockNumber's
	 * for columnar tables don't actually correspond to actual disk blocks
	 * as in heapAM. For this reason, we call them as "virtual" blocks. At
	 * the moment, we believe it is better to report our progress based on
	 * this "virtual" block concept instead of doing nothing.
	 */
	Assert(progress_arr_index == PROGRESS_SCAN_BLOCKS_TOTAL ||
		   progress_arr_index == PROGRESS_SCAN_BLOCKS_DONE);
	BlockNumber nvirtual_blocks =
		column_virtual_block_count(relation, snapshot);
	pgstat_progress_update_param(progress_arr_index, nvirtual_blocks);
}


/*
 * column_virtual_block_count returns total number of "virtual" blocks
 * that given columnar table has based on based on ItemPointer BlockNumber's.
 */
static BlockNumber
column_virtual_block_count(Relation relation, Snapshot snapshot)
{
	ItemPointerData highest_item_pointer =
		highest_column_item_pointer(relation, snapshot);
	if (!ItemPointerIsValid(&highest_item_pointer))
	{
		/* table is empty according to our snapshot */
		return 0;
	}

	/*
	 * Since BlockNumber is 0-based, increment it by 1 to find the total
	 * number of "virtual" blocks.
	 */
	return ItemPointerGetBlockNumber(&highest_item_pointer) + 1;
}


/*
 * highest_column_item_pointer returns ItemPointerData for the tuple with
 * highest tid for given relation.
 * If given relation is empty, then returns invalid item pointer.
 */
static ItemPointerData
highest_column_item_pointer(Relation relation, Snapshot snapshot)
{
	stripe_metadata *stripe_with_highest_row_number =
		find_highest_row_number_stripe(relation, snapshot);
	if (stripe_with_highest_row_number == NULL ||
		stripe_highest_row_number(stripe_with_highest_row_number) == 0)
	{
		/* table is empty according to our snapshot */
		ItemPointerData invalid_item_ptr;
		ItemPointerSetInvalid(&invalid_item_ptr);
		return invalid_item_ptr;
	}

	uint64_t highest_row_number = stripe_highest_row_number(stripe_with_highest_row_number);
	return row_number_to_tid(highest_row_number);
}


/*
 * build_index_from_column_rows builds index_relation tuples by reading the
 * actual relation based on given "scan" and returns number of tuples
 * scanned to build the index_relation.
 */
static double
build_index_from_column_rows(TableScanDesc scan, Relation index_relation,
						  IndexInfo *index_info, bool progress,
						  IndexBuildCallback index_callback,
						  void *index_callback_state, EState *estate,
						  ExprState *predicate,
						  bioseq::storage::pg_sequence_source_provider *source_provider)
{
	double reltuples = 0;

	BlockNumber last_reported_block_number = InvalidBlockNumber;

	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = econtext->ecxt_scantuple;
	while (bioseq_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();

		BlockNumber current_block_number = ItemPointerGetBlockNumber(&slot->tts_tid);
		if (progress && last_reported_block_number != current_block_number)
		{
			/*
			 * bioseq_getnextslot guarantees that returned tuple will
			 * always have a greater ItemPointer than the ones we fetched
			 * before, so we directly use BlockNumber to report our progress.
			 */
			Assert(last_reported_block_number == InvalidBlockNumber ||
				   current_block_number >= last_reported_block_number);
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
										 current_block_number);
			last_reported_block_number = current_block_number;
		}

		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		promote_index_source_locators(
			scan->rs_rd, slot, source_provider,
			econtext->ecxt_per_tuple_memory);

		if (predicate != NULL && !ExecQual(predicate, econtext))
		{
			/* for partial indexes, discard tuples that don't satisfy the predicate */
			continue;
		}

		Datum index_values[INDEX_MAX_KEYS];
		bool index_nulls[INDEX_MAX_KEYS];
		ItemPointerData item_pointer_data = slot->tts_tid;

		/* currently, columnar tables can't have dead tuples */
		bool tuple_is_alive = true;
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
				index_info, slot, estate, index_values, index_nulls);
			index_callback(
				index_relation, &item_pointer_data, index_values, index_nulls,
				tuple_is_alive, index_callback_state);
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
bioseq_index_validate_scan(Relation bioseq_column_relation,
							 Relation index_relation,
							 IndexInfo *index_info,
							 Snapshot snapshot,
							 ValidateIndexState *
							 validate_index_state)
{
	report_total_virtual_blocks(bioseq_column_relation, snapshot,
									 PROGRESS_SCAN_BLOCKS_TOTAL);

	/*
	 * Set up execution state for predicate, if any.
	 * Note that this is only useful for partial indexes.
	 */
	EState *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	econtext->ecxt_scantuple = table_slot_create(bioseq_column_relation, NULL);
	ExprState *predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

	int nkeys = 0;
	ScanKeyData *scan_key = NULL;
	bool allow_access_strategy = true;
	bool allow_sync_scan = false;
	TableScanDesc scan = table_beginscan_strat(bioseq_column_relation, snapshot, nkeys, scan_key,
											   allow_access_strategy, allow_sync_scan);
	bioseq::storage::pg_sequence_source_provider *volatile source_provider = nullptr;
	if (snapshot != nullptr && IsMVCCSnapshot(snapshot)
		&& snapshot->regd_count > 0)
	{
		auto *bioseq_scan = reinterpret_cast<column_scan_handle>(scan);
		bioseq_scan->defers_sequence_hydration = true;
		source_provider =
			create_index_source_provider(bioseq_column_relation, snapshot);
	}

	PG_TRY();
	{
		validate_missing_index_rows(
			scan, index_relation, index_info, estate, predicate,
			validate_index_state, source_provider);
		ResetExprContext(econtext);
		if (source_provider != nullptr)
		{
			bioseq::storage::close_pg_sequence_source_provider(
				source_provider, /*is_commit=*/true);
			source_provider = nullptr;
		}
	}
	PG_CATCH();
	{
		if (source_provider != nullptr)
		{
			bioseq::storage::close_pg_sequence_source_provider(
				source_provider, /*is_commit=*/false);
			source_provider = nullptr;
		}
		PG_RE_THROW();
	}
	PG_END_TRY();

	table_endscan(scan);

	/* report the last "virtual" block as "done" */
	report_total_virtual_blocks(bioseq_column_relation, snapshot,
									 PROGRESS_SCAN_BLOCKS_DONE);

	ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
	FreeExecutorState(estate);
	index_info->ii_ExpressionsState = NIL;
	index_info->ii_PredicateState = NULL;
}


/*
 * validate_missing_index_rows inserts the tuples that are not in
 * the index yet by reading the actual relation based on given "scan".
 */
static void
validate_missing_index_rows(TableScanDesc scan, Relation index_relation,
								 IndexInfo *index_info, EState *estate,
								 ExprState *predicate,
								 ValidateIndexState *validate_index_state,
								 bioseq::storage::pg_sequence_source_provider *source_provider)
{
	BlockNumber last_reported_block_number = InvalidBlockNumber;

	bool index_tuple_sort_empty = false;
	ItemPointerData indexed_item_pointer_data;
	ItemPointerSetInvalid(&indexed_item_pointer_data);

	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = econtext->ecxt_scantuple;
	while (bioseq_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();

		ItemPointer bioseq_column_item_pointer = &slot->tts_tid;
		BlockNumber current_block_number = ItemPointerGetBlockNumber(bioseq_column_item_pointer);
		if (last_reported_block_number != current_block_number)
		{
			/*
			 * bioseq_getnextslot guarantees that returned tuple will
			 * always have a greater ItemPointer than the ones we fetched
			 * before, so we directly use BlockNumber to report our progress.
			 */
			Assert(last_reported_block_number == InvalidBlockNumber ||
				   current_block_number >= last_reported_block_number);
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
										 current_block_number);
			last_reported_block_number = current_block_number;
		}

		validate_index_state->htups += 1;

		if (!index_tuple_sort_empty &&
			(!ItemPointerIsValid(&indexed_item_pointer_data) ||
			 ItemPointerCompare(&indexed_item_pointer_data, bioseq_column_item_pointer) < 0))
		{
			/*
			 * Skip indexed item pointers until we find or pass the current
			 * columnar relation item pointer.
			 */
			indexed_item_pointer_data =
				skip_item_pointers_before(validate_index_state->tuplesort,
												 bioseq_column_item_pointer);
			index_tuple_sort_empty = !ItemPointerIsValid(&indexed_item_pointer_data);
		}

		if (!index_tuple_sort_empty &&
			ItemPointerCompare(&indexed_item_pointer_data, bioseq_column_item_pointer) == 0)
		{
			/* tuple is already covered by the index, skip */
			continue;
		}
		Assert(index_tuple_sort_empty ||
			   ItemPointerCompare(&indexed_item_pointer_data, bioseq_column_item_pointer) > 0);

		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		promote_index_source_locators(
			scan->rs_rd, slot, source_provider,
			econtext->ecxt_per_tuple_memory);

		if (predicate != NULL && !ExecQual(predicate, econtext))
		{
			/* for partial indexes, discard tuples that don't satisfy the predicate */
			continue;
		}

		Datum index_values[INDEX_MAX_KEYS];
		bool index_nulls[INDEX_MAX_KEYS];

		Relation bioseq_column_relation = scan->rs_rd;
		IndexUniqueCheck index_unique_check =
			index_info->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO;
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
				index_info, slot, estate, index_values, index_nulls);
			index_insert(
				index_relation, index_values, index_nulls,
				bioseq_column_item_pointer, bioseq_column_relation,
				index_unique_check, false, index_info);
			restore_slice_materialization_scope(previous_scope);
		}
		PG_CATCH();
		{
			restore_slice_materialization_scope(previous_scope);
			PG_RE_THROW();
		}
		PG_END_TRY();

		validate_index_state->tups_inserted += 1;
	}
}


/*
 * skip_item_pointers_before iterates given tuple_sort until finding an
 * ItemPointer that is greater than or equal to given target_item_pointer and
 * returns that ItemPointer.
 * If such an ItemPointer does not exist, then returns invalid ItemPointer.
 *
 * Note that this function assumes given tuple_sort doesn't have any NULL
 * Datum's.
 */
static ItemPointerData
skip_item_pointers_before(Tuplesortstate *tuple_sort, ItemPointer target_item_pointer)
{
	ItemPointerData ts_item_pointer_data;
	ItemPointerSetInvalid(&ts_item_pointer_data);

	while (!ItemPointerIsValid(&ts_item_pointer_data) ||
		   ItemPointerCompare(&ts_item_pointer_data, target_item_pointer) < 0)
	{
		bool forward_direction = true;
		Datum *abbrev = NULL;
		Datum ts_datum;
		bool ts_datum_is_null;
		if (!tuplesort_getdatum(tuple_sort, forward_direction, false,
									   &ts_datum, &ts_datum_is_null, abbrev))
		{
			ItemPointerSetInvalid(&ts_item_pointer_data);
			break;
		}

		Assert(!ts_datum_is_null);
		itemptr_decode(&ts_item_pointer_data, DatumGetInt64(ts_datum));

#ifndef USE_FLOAT8_BYVAL

		/*
		 * If int8 is pass-by-ref, we need to free Datum memory.
		 * See tuplesort_getdatum function's comment.
		 */
		pfree(DatumGetPointer(ts_datum));
#endif
	}

	return ts_item_pointer_data;
}


static uint64
bioseq_relation_size(Relation rel, ForkNumber fork_number)
{
	uint64 nblocks = 0;

	/* InvalidForkNumber indicates returning the size for all forks */
	if (fork_number == InvalidForkNumber)
	{
		for (int i = 0; i < MAX_FORKNUM; i++)
		{
			nblocks += smgrnblocks(RelationGetSmgr(rel), static_cast<ForkNumber>(i));
		}
	}
	else
	{
		nblocks = smgrnblocks(RelationGetSmgr(rel), fork_number);
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
	*tuples = column_table_row_count(rel);

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
	bioseq_log<logging_level::error, logging_target::postgres>( "bioseq_scan_sample_next_block not implemented");
}


static bool
bioseq_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
								TupleTableSlot *slot)
{
	bioseq_log<logging_level::error, logging_target::postgres>( "bioseq_scan_sample_next_tuple not implemented");
}


static void
column_transaction_callback(XactEvent event, void *arg)
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
			discard_all_relation_write_states(GetCurrentSubTransactionId(), 0);
			release_subtransaction_read_states(GetCurrentSubTransactionId());
			const bool active_map_quarantined =
				speculative_source_quarantine_state_noexcept()
					.active_map_quarantined;
			if (!active_map_quarantined) {
				access_pending_speculative_sources().clear();
			}
			break;
		}

		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
		{
			require_no_pending_speculative_sources("pre-commit/pre-prepare");
			flush_all_relation_write_states(GetCurrentSubTransactionId(), 0);
			release_subtransaction_read_states(GetCurrentSubTransactionId());
			break;
		}
	}
}


static void
column_subtransaction_callback(SubXactEvent event, SubTransactionId my_subid,
						SubTransactionId parent_subid, void *arg)
{
	switch (event)
	{
		case SUBXACT_EVENT_START_SUB:
		case SUBXACT_EVENT_COMMIT_SUB:
		{
			for (auto& [key, pending] : access_pending_speculative_sources()) {
				(void)key;
				if (pending.subtransaction_id == my_subid) {
					pending.subtransaction_id = parent_subid;
				}
			}
			break;
		}

		case SUBXACT_EVENT_ABORT_SUB:
		{
			discard_all_relation_write_states(my_subid, parent_subid);
			release_subtransaction_read_states(my_subid);
			std::erase_if(access_pending_speculative_sources(),
				[my_subid](const auto& entry) {
					return entry.second.subtransaction_id == my_subid;
				});
			break;
		}

		case SUBXACT_EVENT_PRE_COMMIT_SUB:
		{
			flush_all_relation_write_states(my_subid, parent_subid);
			release_subtransaction_read_states(my_subid);
			break;
		}
	}
}


// adapters that keep storage_metadata independent from
// bioseq_storage.  A parent flush must also flush its cached sequence-fragment
// shard states before any caller refreshes or serializes a snapshot.
static void
bioseq_flush_pending_writes_adapter(column_write_state* write_state)
{
	flush_pending_column_writes(write_state);
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
filtered_chunk_group_count(column_scan_handle bioseq_column_scan_desc)
{
	column_read_state *read_state = bioseq_column_scan_desc->read_state;

	/* read_state is initialized lazily */
	if (read_state != NULL)
	{
		return read_chunk_groups_filtered(read_state);
	}
	else
	{
		return 0;
	}
}


// /*
//  * relation_oid_uses_column_table_access_method returns true if relation has bioseq_tableam
//  * access method. This can be called before extension creation.
//  */
// bool
// relation_oid_uses_column_table_access_method(Oid relationId)
// {
// 	bioseq_log<logging_level::debug>("[bioseq] pg: relation_oid_uses_column_table_access_method() entered");
// 	if (!OidIsValid(relationId))
// 	{
// 		return false;
// 	}

// 	/*
// 	 * Lock relation to prevent it from being dropped &
// 	 * avoid race conditions.
// 	 */
// 	Relation rel = bioseq::postgres::support::open_postgres_relation(relationId, AccessShareLock);
// 	bool result = rel->rd_tableam == column_table_access_method();
// 	relation_close(rel, NoLock);

// 	return result;
// }


export const TableAmRoutine bioseq_am_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = column_slot_callbacks,

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

#if PG_VERSION_NUM < PG_VERSION_18
	.scan_bitmap_next_block = NULL,
#endif
	.scan_bitmap_next_tuple = NULL,
	.scan_sample_next_block = bioseq_scan_sample_next_block,
	.scan_sample_next_tuple = bioseq_scan_sample_next_tuple
};


export const TableAmRoutine *
column_table_access_method(void)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: column_table_access_method() entered");
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
			bioseq::storage::metadata::sql_entities::storageid_seq,
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
			bioseq::postgres::support::table_access_method_handler>
			handler_context) {
	static_cast<void>(handler_context);
	bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_handler() entered");
	return &bioseq_am_methods;
}

}  // namespace bioseq::table::sql_routines


export void
bioseq_tableam_init()
{
	bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_tableam_init() entered");

	// publish the bioseq TableAmRoutine identity to the
	// bioseq_column_tam leaf so relation_oid_uses_column_table_access_method can recognise bioseq
	// tables without depending back on this module (dependency inversion).
	register_column_table_access_method(column_table_access_method());

	// wire the inverted write/read-state dependencies. The
	// storage_metadata write/read-state partitions hold function-pointer slots
	// (default nullptr); register the bioseq_table_io / bioseq_storage callees
	// here so storage_metadata stays free of those module edges.
	register_column_write_state_hooks(&begin_column_write, &end_column_write,
							   &bioseq_flush_pending_writes_adapter,
							   &has_pending_column_writes,
							   &bioseq_seq_caches_close_adapter);
	register_column_read_state_hooks(&end_column_read);
	column_set_options_hook_type** column_set_options_hook_slot =
		reinterpret_cast<column_set_options_hook_type **>(find_rendezvous_variable(
			k_column_set_options_hook_symbol));
	*column_set_options_hook_slot = &column_set_options_hook;

	RegisterXactCallback(column_transaction_callback, NULL);
	RegisterSubXactCallback(column_subtransaction_callback, NULL);

	install_column_table_hooks();

	// Custom-scan initialization is owned by the extension initializer so the
	// table partitions retain an acyclic import graph.

	initialize_column_tuple_slot_operations();
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
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = "table_name must not be NULL"});
	}
	const Oid relation_id = *table_name;
	Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
		[relation_id] {
			return table_open(relation_id, AccessExclusiveLock);
		});
	const sql_relation_lock relation_lock{
		relation, NoLock, true};
	require_bioseq_table(relation, relation_id);
	require_relation_owner(relation_id);

	auto maybe_options = bioseq::postgres::support::invoke_pg_as_cpp(
		[relation_id] {
			return read_column_options(relation_id);
		});
	if (!maybe_options)
	{
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = "unable to read current options for table"});
	}
	column_options options = *maybe_options;

	if (chunk_group_row_limit.has_value())
	{
		options.chunk_group_row_limit = *chunk_group_row_limit;
		if (options.chunk_group_row_limit < bioseq::k_minimum_chunk_group_row_count ||
			options.chunk_group_row_limit > bioseq::k_maximum_chunk_group_row_count)
		{
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{
					.message =
						"chunk group row count limit out of range",
					.hint =
						"chunk group row count limit must be between "
						+ std::to_string(bioseq::k_minimum_chunk_group_row_count)
						+ " and "
						+ std::to_string(bioseq::k_maximum_chunk_group_row_count),
				});
		}
	}

	if (stripe_row_limit.has_value())
	{
		options.stripe_row_limit = *stripe_row_limit;
		if (options.stripe_row_limit < bioseq::k_minimum_stripe_row_count ||
			options.stripe_row_limit > bioseq::k_maximum_stripe_row_count)
		{
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{
					.message = "stripe row count limit out of range",
					.hint =
						"stripe row count limit must be between "
						+ std::to_string(bioseq::k_minimum_stripe_row_count)
						+ " and "
						+ std::to_string(bioseq::k_maximum_stripe_row_count),
				});
		}
	}

	if (compression.has_value())
	{
		options.compression_algorithm =
			parse_compression_algorithm(NameStr(*compression));
		if (options.compression_algorithm == bioseq::compression_algorithm::invalid)
		{
			const char* const compression_name =
				NameStr(*compression);
			const char* const quoted_name =
				bioseq::postgres::support::invoke_pg_as_cpp(
					[compression_name] {
						return quote_identifier(
							compression_name);
					});
			bioseq::postgres::support::raise_sql_error(
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
		options.compression_level = *compression_level;
		if (options.compression_level < bioseq::k_minimum_compression_level ||
			options.compression_level > bioseq::k_maximum_compression_level)
		{
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{
					.message = "compression level out of range",
					.hint =
						"compression level must be between "
						+ std::to_string(bioseq::k_minimum_compression_level)
						+ " and "
						+ std::to_string(bioseq::k_maximum_compression_level),
				});
		}
	}

	if (column_set_options_hook != NULL)
	{
		bioseq::postgres::support::invoke_pg_as_cpp(
			[relation_id, options] {
				column_set_options_hook(
					relation_id, options);
			});
	}

	bioseq::postgres::support::invoke_pg_as_cpp(
		[relation_id, &options] {
			set_column_options(relation_id, &options);
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
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = "table_name must not be NULL"});
	}
	const Oid relation_id = *table_name;
	Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
		[relation_id] {
			return table_open(relation_id, AccessExclusiveLock);
		});
	const sql_relation_lock relation_lock{
		relation, NoLock, true};
	require_bioseq_table(relation, relation_id);
	require_relation_owner(relation_id);

	auto maybe_options = bioseq::postgres::support::invoke_pg_as_cpp(
		[relation_id] {
			return read_column_options(relation_id);
		});
	if (!maybe_options)
	{
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = "unable to read current options for table"});
	}
	column_options options = *maybe_options;

	if (chunk_group_row_limit.value_or(false))
	{
		options.chunk_group_row_limit = bioseq::settings::storage::chunk_group::row_limit;
	}

	if (stripe_row_limit.value_or(false))
	{
		options.stripe_row_limit = bioseq::settings::storage::stripe::row_limit;
	}

	if (compression.value_or(false))
	{
		options.compression_algorithm = static_cast<bioseq::compression_algorithm>(static_cast<int>(bioseq::settings::storage::compression::algorithm.get_value()));
	}

	if (compression_level.value_or(false))
	{
		options.compression_level = bioseq::settings::storage::compression::level;
	}

	if (column_set_options_hook != NULL)
	{
		bioseq::postgres::support::invoke_pg_as_cpp(
			[relation_id, options] {
				column_set_options_hook(
					relation_id, options);
			});
	}

	bioseq::postgres::support::invoke_pg_as_cpp(
		[relation_id, &options] {
			set_column_options(relation_id, &options);
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
	Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
		[rel] {
			return table_open(rel, AccessExclusiveLock);
		});
	const sql_relation_lock relation_lock{
		relation, AccessExclusiveLock, true};
	require_bioseq_table(relation, rel);
	bioseq::postgres::support::invoke_pg_as_cpp(
		[relation] {
			ensure_current_column_storage_format(relation, true);
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
	Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
		[rel] {
			return table_open(rel, AccessExclusiveLock);
		});
	const sql_relation_lock relation_lock{
		relation, AccessExclusiveLock, true};
	require_bioseq_table(relation, rel);
	bioseq::postgres::support::invoke_pg_as_cpp(
		[relation] {
			ensure_current_column_storage_format(relation, false);
		});
}

}  // namespace bioseq::table::sql_routines
