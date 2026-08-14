/// @brief Read-only PostgreSQL table access method for external relations.
module;
#include <bioseq/pg_compat.h>

#include <climits>
#include <cstddef>
#include <cstring>

extern "C" {
#include "access/reloptions.h"
#include "catalog/pg_class.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
}

export module bioseq.postgres.external.relation;

#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
import bioseq.codegen.sql.annotation_model;

namespace bioseq::postgres::external::sql_routines {

extern "C" {
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
[[bioseq_sql::access_method]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::table_am_handler,
})]]
/** @brief Returns the table access-method handler for external relations. */
PGDLLEXPORT Datum bioseq_external_handler(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::internal,
    })]]
    FunctionCallInfo fcinfo);
}

} // namespace bioseq::postgres::external::sql_routines
#endif

export namespace bioseq::postgres::external {

/** @brief Logical row shape selected by a relation's required `entity` option. */
enum class relation_entity : int
{
	unspecified = 0,
	taxonomy,
	catalog,
	record_gene,
	record_location,
	sequence,
	sequence_chunk
};

/**
 * @brief PostgreSQL varlena payload for external-relation options.
 *
 * Field order and representation are PostgreSQL reloptions ABI: `vl_len_`
 * remains first, string values are offsets into trailing storage, and numeric
 * values are stored inline.
 */
struct relation_options
{
	int32 vl_len_;
	int entity;
	int alphabet;
	int storage_width;
	int chunk_bases;
};

/** @brief Parses a `WITH (...)` datum into PostgreSQL-owned relation options. */
[[nodiscard]] bytea *parse_relation_options(Datum options, bool validate);

/** @brief Returns the required logical entity or raises a PostgreSQL error. */
[[nodiscard]] relation_entity entity_from_options(Relation relation);

/** @brief Returns the stable `TableAmRoutine` registered for external tables. */
[[nodiscard]] const TableAmRoutine *table_access_method() noexcept;

/** @brief Tests whether a relation uses the external table access method. */
[[nodiscard]] bool uses_table_access_method(Relation relation) noexcept;

/** @brief Returns the SQL option spelling for a logical relation entity. */
[[nodiscard]] const char *to_string(relation_entity entity) noexcept;

} // namespace bioseq::postgres::external

/** Registers the external table access method's relation options. */
extern "C" void bioseq_external_tableam_init(void);

/* ======================================================================== */
/* Relation options                                                          */
/* ======================================================================== */

namespace bioseq::postgres::external {

namespace {

/* Allocated in bioseq_external_tableam_init(); 0 until then. */
relopt_kind relation_option_kind = static_cast<relopt_kind>(0);

/* Acceptable `entity` values, null-terminated as the API requires. */
relopt_enum_elt_def entity_option_values[] = {
	{ "taxonomy",        static_cast<int>(relation_entity::taxonomy) },
	{ "catalog",         static_cast<int>(relation_entity::catalog) },
	{ "record_gene",     static_cast<int>(relation_entity::record_gene) },
	{ "record_location", static_cast<int>(relation_entity::record_location) },
	{ "sequence",        static_cast<int>(relation_entity::sequence) },
	{ "sequence_chunk",  static_cast<int>(relation_entity::sequence_chunk) },
	{ nullptr,           0 }
};

const relopt_parse_elt relation_option_fields[] = {
	{ "entity", RELOPT_TYPE_ENUM, offsetof(relation_options, entity) },
	{ "alphabet", RELOPT_TYPE_STRING, offsetof(relation_options, alphabet) },
	{ "storage_width", RELOPT_TYPE_INT,
	  offsetof(relation_options, storage_width) },
	{ "chunk_bases", RELOPT_TYPE_INT,
	  offsetof(relation_options, chunk_bases) },
};

/*
 * PostgreSQL invokes string validators for both the null default and supplied
 * values. Alphabet interpretation belongs to sequence materialization, so the
 * relation boundary accepts the stored spelling without changing it.
 */
void
accept_alphabet_option(const char *value)
{
	(void) value;
}

} // namespace

bytea *
parse_relation_options(Datum options, bool validate)
{
	return static_cast<bytea *>(
		build_reloptions(options, validate, relation_option_kind,
						 sizeof(relation_options), relation_option_fields,
						 lengthof(relation_option_fields)));
}

namespace {

/**
 * Parse and cache this AM's private options without replacing PostgreSQL's
 * StdRdOptions payload in rd_options. PostgreSQL clears rd_amcache when the
 * relcache entry is invalidated, so a reopened relation is parsed from the
 * current pg_class.reloptions value.
 */
relation_options *
cached_relation_options(Relation relation)
{
	if (relation->rd_amcache != nullptr)
	{
		return static_cast<relation_options *>(relation->rd_amcache);
	}

	HeapTuple relation_tuple = SearchSysCacheCopy1(
		RELOID, ObjectIdGetDatum(RelationGetRelid(relation)));
	if (!HeapTupleIsValid(relation_tuple))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not find pg_class row for bioseq_external relation %u",
						RelationGetRelid(relation))));
	}

	bool is_null = false;
	Datum raw_options = SysCacheGetAttr(
		RELOID, relation_tuple, Anum_pg_class_reloptions, &is_null);
	if (is_null)
	{
		heap_freetuple(relation_tuple);
		return nullptr;
	}

	ArrayType *raw_options_copy = DatumGetArrayTypePCopy(raw_options);
	heap_freetuple(relation_tuple);

	bytea *parsed = parse_relation_options(
		PointerGetDatum(raw_options_copy), true);
	pfree(raw_options_copy);
	if (parsed == nullptr ||
		VARSIZE_ANY(parsed) < static_cast<int>(sizeof(relation_options)))
	{
		if (parsed != nullptr)
		{
			pfree(parsed);
		}
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid relation options for bioseq_external relation \"%s\"",
						RelationGetRelationName(relation))));
	}

	const Size parsed_size = static_cast<Size>(VARSIZE_ANY(parsed));
	auto *cached = static_cast<relation_options *>(
		MemoryContextAlloc(CacheMemoryContext, parsed_size));
	std::memcpy(cached, parsed, parsed_size);
	pfree(parsed);

	relation->rd_amcache = cached;
	return cached;
}

} // namespace

relation_entity
entity_from_options(Relation relation)
{
	if (relation == nullptr || !uses_table_access_method(relation))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation does not use the bioseq_external access method")));
	}

	relation_options *options = cached_relation_options(relation);

	if (options == nullptr ||
		options->entity == static_cast<int>(relation_entity::unspecified))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bioseq_external relation \"%s\" is missing the required \"entity\" option",
						RelationGetRelationName(relation)),
				 errhint("Valid entity values are taxonomy, catalog, record_gene, "
						 "record_location, sequence, and sequence_chunk.")));
	}

	const relation_entity entity = static_cast<relation_entity>(options->entity);
	switch (entity)
	{
		case relation_entity::taxonomy:
		case relation_entity::catalog:
		case relation_entity::record_gene:
		case relation_entity::record_location:
		case relation_entity::sequence:
		case relation_entity::sequence_chunk:
			return entity;
		case relation_entity::unspecified:
			break;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid \"entity\" option for bioseq_external relation \"%s\"",
					RelationGetRelationName(relation))));
	pg_unreachable();
}

/* ======================================================================== */
/* Error helpers                                                             */
/* ======================================================================== */

namespace {

/* A physical scan callback fired: the CustomScan must own external reads. */
[[noreturn]] void
raise_custom_scan_required()
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("Custom Scan required"),
			 errdetail("bioseq_external relations are read through the bioseq_external "
					   "Custom Scan; a physical scan path was reached instead."),
			 errhint("Ensure the extension is loaded and "
			         "bioseq.planner.custom_scan.enabled is on.")));
	pg_unreachable();
}

/* Any write path fired against an inherently read-only external relation. */
[[noreturn]] void
raise_read_only()
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("external relations are read-only")));
	pg_unreachable();
}

/* An unsupported physical capability (index/parallel/sample/rewrite) fired. */
[[noreturn]] void
raise_unsupported_operation(const char *feature)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("%s is not supported on bioseq_external relations", feature)));
	pg_unreachable();
}

} // namespace

/* ======================================================================== */
/* TableAmRoutine callbacks (PG17). Signatures mirror                        */
/* backend/pg/table/access_method.cppm's bioseq_am_methods exactly.          */
/* ======================================================================== */

namespace {

/* --- slot ops: virtual slots, as the CustomScan produces --------------- */

const TupleTableSlotOps *
slot_callbacks(Relation relation)
{
	return &TTSOpsVirtual;
}

/* --- sequential scan: owned by the CustomScan -------------------------- */

TableScanDesc
begin_physical_scan(Relation relation, Snapshot snapshot,
						   int nkeys, ScanKey key,
						   ParallelTableScanDesc parallel_scan,
						   uint32 flags)
{
	raise_custom_scan_required();
}

void
end_physical_scan(TableScanDesc scan)
{
	/* Idempotent no-op: scan_begin never returns a descriptor, so there is
	 * nothing to tear down. Safe to call regardless. */
}

void
rescan_physical_scan(TableScanDesc scan, ScanKey key, bool set_params,
					   bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	raise_custom_scan_required();
}

bool
next_physical_tuple(TableScanDesc scan, ScanDirection direction,
							TupleTableSlot *slot)
{
	raise_custom_scan_required();
}

/* --- parallel scan: external paths are always non-parallel ------------- */

Size
estimate_parallel_scan(Relation rel)
{
	raise_unsupported_operation("parallel scan");
}

Size
initialize_parallel_scan(Relation rel, ParallelTableScanDesc pscan)
{
	raise_unsupported_operation("parallel scan");
}

void
reinitialize_parallel_scan(Relation rel, ParallelTableScanDesc pscan)
{
	raise_unsupported_operation("parallel scan");
}

/* --- index fetch: external relations reject index DDL ------------------ */

IndexFetchTableData *
begin_index_fetch(Relation rel)
{
	raise_unsupported_operation("index scan");
}

void
reset_index_fetch(IndexFetchTableData *scan)
{
	/* no-op: index_fetch_begin never returns a handle */
}

void
end_index_fetch(IndexFetchTableData *scan)
{
	/* no-op: index_fetch_begin never returns a handle */
}

bool
fetch_index_tuple(IndexFetchTableData *scan, ItemPointer tid,
								  Snapshot snapshot, TupleTableSlot *slot,
								  bool *call_again, bool *all_dead)
{
	raise_unsupported_operation("index scan");
}

/* --- tuple fetch / tid / snapshot -------------------------------------- */

bool
fetch_row_version(Relation relation, ItemPointer tid,
								  Snapshot snapshot, TupleTableSlot *slot)
{
	raise_custom_scan_required();
}

bool
tuple_identifier_is_valid(TableScanDesc scan, ItemPointer tid)
{
	/* No physical tuples exist; a tid is never valid. Returning false (rather
	 * than raising) keeps predicate-style callers well-behaved. */
	return false;
}

void
get_latest_tuple_identifier(TableScanDesc scan, ItemPointer tid)
{
	raise_unsupported_operation("tuple version chains");
}

bool
tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
										 Snapshot snapshot)
{
	/* No stored tuple can satisfy a snapshot. */
	return false;
}

TransactionId
delete_index_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	/* External relations have no indexes; nothing to delete. */
	delstate->ndeltids = 0;
	return InvalidTransactionId;
}

/* --- all DML is rejected (read-only) ----------------------------------- */

void
insert_tuple(Relation relation, TupleTableSlot *slot,
							 CommandId cid, int options, BulkInsertState bistate)
{
	raise_read_only();
}

void
insert_speculative_tuple(Relation relation, TupleTableSlot *slot,
										 CommandId cid, int options,
										 BulkInsertState bistate, uint32 specToken)
{
	raise_read_only();
}

void
complete_speculative_tuple(Relation relation, TupleTableSlot *slot,
										   uint32 specToken, bool succeeded)
{
	raise_read_only();
}

void
insert_multiple_tuples(Relation relation, TupleTableSlot **slots, int ntuples,
							 CommandId cid, int options, BulkInsertState bistate)
{
	raise_read_only();
}

TM_Result
delete_tuple(Relation relation, ItemPointer tid, CommandId cid,
							 Snapshot snapshot, Snapshot crosscheck, bool wait,
							 TM_FailureData *tmfd, bool changingPart)
{
	raise_read_only();
}

TM_Result
update_tuple(Relation relation, ItemPointer otid, TupleTableSlot *slot,
							 CommandId cid, Snapshot snapshot, Snapshot crosscheck,
							 bool wait, TM_FailureData *tmfd,
							 LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	raise_read_only();
}

TM_Result
lock_tuple(Relation relation, ItemPointer tid, Snapshot snapshot,
						   TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
						   LockWaitPolicy wait_policy, uint8 flags,
						   TM_FailureData *tmfd)
{
	raise_read_only();
}

void
finish_bulk_insert(Relation relation, int options)
{
	raise_read_only();
}

/* --- storage lifecycle: validate, but create no physical storage ------- */

void
set_new_file_locator(Relation rel,
											 const RelFileLocator *newrlocator,
											 char persistence,
											 TransactionId *freezeXid,
											 MultiXactId *minmulti)
{
	if (persistence == RELPERSISTENCE_TEMP)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("bioseq_external relations cannot be TEMPORARY")));
	}

	/*
	 * Supplied values have already passed the PostgreSQL reloptions parser.
	 * A new relation is not yet visible through pg_class here, so the required
	 * entity check is intentionally deferred until the reopened relation is
	 * routed through entity_from_options().
	 */

	/*
	 * External relations are non-materialised: no heap/init fork is created.
	 * relation_size() therefore reports zero and never touches smgr. Set the
	 * freeze horizons to "not tracked", matching a relation with no tuples.
	 */
	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;
}

void
truncate_nontransactional(Relation rel)
{
	raise_unsupported_operation("TRUNCATE");
}

void
copy_relation_data(Relation rel, const RelFileLocator *newrlocator)
{
	raise_unsupported_operation("storage rewrite");
}

void
copy_relation_for_cluster(Relation OldTable, Relation NewTable,
										  Relation OldIndex, bool use_sort,
										  TransactionId OldestXmin,
										  TransactionId *xid_cutoff,
										  MultiXactId *multi_cutoff,
										  double *num_tuples,
										  double *tups_vacuumed,
										  double *tups_recently_dead)
{
	raise_unsupported_operation("CLUSTER / VACUUM FULL");
}

void
vacuum_relation(Relation rel, VacuumParams *params,
								BufferAccessStrategy bstrategy)
{
	/* Safe no-op: external relations have zero physical pages, so VACUUM and
	 * autovacuum have nothing to do. */
}

/* --- analyze: no local rows, no remote I/O ----------------------------- */

bool
next_analyze_block(TableScanDesc scan, ReadStream *stream)
{
	/* No physical blocks to sample. */
	return false;
}

bool
next_analyze_tuple(TableScanDesc scan, TransactionId OldestXmin,
										double *liverows, double *deadrows,
										TupleTableSlot *slot)
{
	/* Never contacts a provider; yields no sample tuples. */
	return false;
}

/* --- index build/validate: external relations reject index DDL --------- */

double
build_index_range_scan(Relation table_rel, Relation index_rel,
									   IndexInfo *index_info, bool allow_sync,
									   bool anyvisible, bool progress,
									   BlockNumber start_blockno,
									   BlockNumber numblocks,
									   IndexBuildCallback callback,
									   void *callback_state, TableScanDesc scan)
{
	raise_unsupported_operation("index build");
}

void
validate_index_scan(Relation table_rel, Relation index_rel,
									IndexInfo *index_info, Snapshot snapshot,
									ValidateIndexState *state)
{
	raise_unsupported_operation("index validate");
}

/* --- size / toast / estimate: zero physical footprint ------------------ */

uint64
relation_size(Relation rel, ForkNumber forkNumber)
{
	/* No physical storage. */
	return 0;
}

bool
relation_needs_toast_table(Relation rel)
{
	/* No tuples are stored, so no TOAST table is ever needed. */
	return false;
}

/* Provider-free estimate: fixed cardinality and local type widths only. */
constexpr double default_estimated_rows = 1000.0;

void
estimate_relation_size(Relation rel, int32 *attr_widths,
								  BlockNumber *pages, double *tuples,
								  double *allvisfrac)
{
	*pages = 0;
	*tuples = default_estimated_rows;
	*allvisfrac = 0.0;

	/* Local (statistics/type) column widths only — no remote I/O. */
	get_rel_data_width(rel, attr_widths);
}

/* --- sample scan: rejected (also blocked at plan time) ----------------- */

bool
next_sample_block(TableScanDesc scan, SampleScanState *scanstate)
{
	raise_unsupported_operation("TABLESAMPLE");
}

bool
next_sample_tuple(TableScanDesc scan, SampleScanState *scanstate,
									   TupleTableSlot *slot)
{
	raise_unsupported_operation("TABLESAMPLE");
}

/* Stable PostgreSQL callback table. Null bitmap callbacks prevent bitmap paths. */
const TableAmRoutine table_access_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = slot_callbacks,

	.scan_begin = begin_physical_scan,
	.scan_end = end_physical_scan,
	.scan_rescan = rescan_physical_scan,
	.scan_getnextslot = next_physical_tuple,

	.parallelscan_estimate = estimate_parallel_scan,
	.parallelscan_initialize = initialize_parallel_scan,
	.parallelscan_reinitialize = reinitialize_parallel_scan,

	.index_fetch_begin = begin_index_fetch,
	.index_fetch_reset = reset_index_fetch,
	.index_fetch_end = end_index_fetch,
	.index_fetch_tuple = fetch_index_tuple,

	.tuple_fetch_row_version = fetch_row_version,
	.tuple_tid_valid = tuple_identifier_is_valid,
	.tuple_get_latest_tid = get_latest_tuple_identifier,
	.tuple_satisfies_snapshot = tuple_satisfies_snapshot,
	.index_delete_tuples = delete_index_tuples,

	.tuple_insert = insert_tuple,
	.tuple_insert_speculative = insert_speculative_tuple,
	.tuple_complete_speculative = complete_speculative_tuple,
	.multi_insert = insert_multiple_tuples,
	.tuple_delete = delete_tuple,
	.tuple_update = update_tuple,
	.tuple_lock = lock_tuple,
	.finish_bulk_insert = finish_bulk_insert,

	.relation_set_new_filelocator = set_new_file_locator,
	.relation_nontransactional_truncate = truncate_nontransactional,
	.relation_copy_data = copy_relation_data,
	.relation_copy_for_cluster = copy_relation_for_cluster,
	.relation_vacuum = vacuum_relation,
	.scan_analyze_next_block = next_analyze_block,
	.scan_analyze_next_tuple = next_analyze_tuple,
	.index_build_range_scan = build_index_range_scan,
	.index_validate_scan = validate_index_scan,

	.relation_size = relation_size,
	.relation_needs_toast_table = relation_needs_toast_table,

	.relation_estimate_size = estimate_relation_size,

/* PostgreSQL 18 removed the per-block bitmap callback. */
#if PG_VERSION_NUM < PG_VERSION_18
	.scan_bitmap_next_block = NULL,
#endif
	.scan_bitmap_next_tuple = NULL,
	.scan_sample_next_block = next_sample_block,
	.scan_sample_next_tuple = next_sample_tuple
};

} // namespace

const TableAmRoutine *
table_access_method() noexcept
{
	return &table_access_methods;
}

bool
uses_table_access_method(Relation relation) noexcept
{
	return relation != nullptr && relation->rd_tableam == &table_access_methods;
}

const char *
to_string(relation_entity entity) noexcept
{
	switch (entity)
	{
		case relation_entity::taxonomy:
			return "taxonomy";
		case relation_entity::catalog:
			return "catalog";
		case relation_entity::record_gene:
			return "record_gene";
		case relation_entity::record_location:
			return "record_location";
		case relation_entity::sequence:
			return "sequence";
		case relation_entity::sequence_chunk:
			return "sequence_chunk";
		case relation_entity::unspecified:
			return "unspecified";
	}
	return "unspecified";
}

void
initialize_relation_options()
{
	if (relation_option_kind != static_cast<relopt_kind>(0))
	{
		return;
	}

	relation_option_kind = add_reloption_kind();
	/* PG17 validates table-AM reloptions through the heap namespace; retain
	 * the private kind as the parser identity for rd_amcache. */
	const bits32 accepted_relation_option_kinds =
		static_cast<bits32>(relation_option_kind) |
		static_cast<bits32>(RELOPT_KIND_HEAP);

	add_enum_reloption(accepted_relation_option_kinds, "entity",
					   "Logical entity kind for a bioseq_external relation.",
					   entity_option_values,
					   static_cast<int>(relation_entity::unspecified),
					   "Valid entity values are \"taxonomy\", \"catalog\", "
					   "\"record_gene\", \"record_location\", \"sequence\", "
					   "and \"sequence_chunk\".",
					   AccessExclusiveLock);

	add_string_reloption(accepted_relation_option_kinds, "alphabet",
						 "Sequence alphabet for sequence/sequence_chunk entities.",
						 nullptr, accept_alphabet_option,
						 AccessExclusiveLock);

	add_int_reloption(accepted_relation_option_kinds, "storage_width",
					  "Sequence storage unit width for sequence entities.",
					  0, 0, 64, AccessExclusiveLock);

	add_int_reloption(accepted_relation_option_kinds, "chunk_bases",
					  "Chunk length in bases for sequence_chunk entities.",
					  0, 0, INT_MAX, AccessExclusiveLock);
}

} // namespace bioseq::postgres::external

extern "C" void
bioseq_external_tableam_init(void)
{
	bioseq::postgres::external::initialize_relation_options();
}

namespace bioseq::postgres::external::sql_routines {

POSTGRES_FN(bioseq_external_handler)
{
	PG_RETURN_POINTER(bioseq::postgres::external::table_access_method());
}

} // namespace bioseq::postgres::external::sql_routines
