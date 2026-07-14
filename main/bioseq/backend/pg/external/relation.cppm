/*-------------------------------------------------------------------------
 *
 * external/relation.cppm
 *
 * DOC0372 — "PostgreSQL logical tables". The bioseq_external table access
 * method (TAM) and its relation-option surface.
 *
 * This is the read-only, storage-free counterpart of the columnar TAM in
 * backend/pg/table/access_method.cppm. Its TableAmRoutine advertises a
 * distinct identity (GetBioseqExternalTableAmRoutine) so the planner hook
 * can recognise external relations, but every physical callback either:
 *   - defers to the CustomScan (scan_begin/rescan/getnextslot),
 *   - raises a read-only SQL error (all DML), or
 *   - is a safe no-op / zero (vacuum, size, analyze).
 * No physical storage fork is ever created and no provider is ever called
 * from this file — it imports NO external/provider module on purpose (the
 * CustomScan bridges to provider core; see scan_state.cppm).
 *
 * It also owns the `entity` / `alphabet` / `storage_width` / `chunk_bases`
 * relation options and the two marker predicate functions
 * (bioseq_external.taxon_matches / bioseq_external.native_query), which are
 * dummies that raise if they ever reach a physical executor.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include <climits>
#include <cstddef>

/* Not pulled in by pg_compat.h; needed for the relation-option API below. */
extern "C" {
#include "access/reloptions.h"
}

export module bioseq.pg.external.relation;

/*
 * DOC0372 §"table access method". The required `entity` option selects one
 * of the six logical relation shapes. UNSPECIFIED is the sentinel used as the
 * enum-reloption default so a missing `entity` is detectable (and rejected)
 * rather than silently defaulting to a real entity.
 */
export enum BioseqExternalEntityKind : int
{
	EXTERNAL_ENTITY_UNSPECIFIED = 0,
	EXTERNAL_ENTITY_TAXONOMY,
	EXTERNAL_ENTITY_CATALOG,
	EXTERNAL_ENTITY_RECORD_GENE,
	EXTERNAL_ENTITY_RECORD_LOCATION,
	EXTERNAL_ENTITY_SEQUENCE,
	EXTERNAL_ENTITY_SEQUENCE_CHUNK
};

/*
 * Parsed relation options. Layout follows the standard reloptions contract
 * (see StdRdOptions): first member is the varlena header, string options are
 * stored as byte offsets into the trailing region (0 == unset), and
 * enum/int options are stored inline. build_reloptions() fills this from the
 * WITH (...) clause; read it back through external_entity_kind_from_options()
 * or the GET_STRING_RELOPTION() macro for `alphabet`.
 */
export struct BioseqExternalRelationOptions
{
	int32 vl_len_;      /* varlena header — do not touch directly */
	int   entity;       /* BioseqExternalEntityKind (enum symbol_val) */
	int   alphabet;     /* offset of `alphabet` string; 0 when unset */
	int   storage_width;/* sequence storage unit width; 0 when unset */
	int   chunk_bases;  /* sequence_chunk length in bases; 0 when unset */
};

/*
 * Parse `reloptions` into a BioseqExternalRelationOptions bytea. Intended to
 * be the relation-option parser for bioseq_external relations; the integrator
 * routes DDL WITH-option parsing here (via the existing table-options hook)
 * so rel->rd_options holds this struct for external relations. Registration
 * of the option names happens in bioseq_external_tableam_init().
 */
export bytea *bioseq_external_reloptions(Datum reloptions, bool validate);

/*
 * Return the entity kind for a bioseq_external relation, raising a DDL/usage
 * error when the required `entity` option is missing or unknown. Only valid
 * on relations that use this AM (guard with relation_uses_bioseq_external_am).
 */
export BioseqExternalEntityKind external_entity_kind_from_options(Relation rel);

/* Distinct TAM identity; also the value the planner hook compares against. */
export const TableAmRoutine *GetBioseqExternalTableAmRoutine(void);

/* True iff `rel` uses the bioseq_external access method. */
export bool relation_uses_bioseq_external_am(Relation rel);

/* Register the relation-option kind + option names. Called once from _PG_init. */
export void bioseq_external_tableam_init(void);

/* ======================================================================== */
/* Relation options                                                          */
/* ======================================================================== */

namespace {

/* Allocated in bioseq_external_tableam_init(); 0 until then. */
relopt_kind bioseq_external_relopt_kind = static_cast<relopt_kind>(0);

/* Acceptable `entity` values, null-terminated as the API requires. */
relopt_enum_elt_def bioseq_external_entity_members[] = {
	{ "taxonomy",        EXTERNAL_ENTITY_TAXONOMY },
	{ "catalog",         EXTERNAL_ENTITY_CATALOG },
	{ "record_gene",     EXTERNAL_ENTITY_RECORD_GENE },
	{ "record_location", EXTERNAL_ENTITY_RECORD_LOCATION },
	{ "sequence",        EXTERNAL_ENTITY_SEQUENCE },
	{ "sequence_chunk",  EXTERNAL_ENTITY_SEQUENCE_CHUNK },
	{ nullptr,           0 }
};

/* Maps each option name to its field in BioseqExternalRelationOptions. */
const relopt_parse_elt bioseq_external_relopt_tab[] = {
	{ "entity",        RELOPT_TYPE_ENUM,   offsetof(BioseqExternalRelationOptions, entity) },
	{ "alphabet",      RELOPT_TYPE_STRING, offsetof(BioseqExternalRelationOptions, alphabet) },
	{ "storage_width", RELOPT_TYPE_INT,    offsetof(BioseqExternalRelationOptions, storage_width) },
	{ "chunk_bases",   RELOPT_TYPE_INT,    offsetof(BioseqExternalRelationOptions, chunk_bases) },
};

/*
 * Light validator for the `alphabet` string option. Kept dependency-light on
 * purpose: it only rejects an empty value. The integrator tightens this to
 * check the registered alphabet set (e.g. dna5) once the sequence/alphabet
 * modules are in scope — doing so here would pull an alphabet module into the
 * otherwise self-contained table-AM translation unit.
 */
void
validate_bioseq_external_alphabet(const char *value)
{
	/*
	 * PostgreSQL calls this validator once at registration time with the option
	 * default (NULL here) to sanity-check it, and again for any user-supplied
	 * value. A NULL/empty value is the unset default and must be accepted, or
	 * add_string_reloption() in bioseq_external_tableam_init() would raise during
	 * _PG_init and break CREATE EXTENSION. The concrete alphabet set is enforced
	 * later, when the sequence data plane (DOC0372 stretch) is implemented; until
	 * then any provided value is accepted as-is.
	 */
	(void) value;
}

} // namespace

bytea *
bioseq_external_reloptions(Datum reloptions, bool validate)
{
	return static_cast<bytea *>(
		build_reloptions(reloptions, validate, bioseq_external_relopt_kind,
						 sizeof(BioseqExternalRelationOptions),
						 bioseq_external_relopt_tab,
						 lengthof(bioseq_external_relopt_tab)));
}

BioseqExternalEntityKind
external_entity_kind_from_options(Relation rel)
{
	BioseqExternalRelationOptions *opts =
		reinterpret_cast<BioseqExternalRelationOptions *>(rel->rd_options);

	if (opts == nullptr || opts->entity == EXTERNAL_ENTITY_UNSPECIFIED)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bioseq_external relation \"%s\" is missing the required \"entity\" option",
						RelationGetRelationName(rel)),
				 errhint("Valid entity values are taxonomy, catalog, record_gene, "
						 "record_location, sequence, and sequence_chunk.")));
	}

	return static_cast<BioseqExternalEntityKind>(opts->entity);
}

/* ======================================================================== */
/* Error helpers                                                             */
/* ======================================================================== */

namespace {

/* A physical scan callback fired: the CustomScan must own external reads. */
[[noreturn]] void
external_requires_custom_scan()
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("Custom Scan required"),
			 errdetail("bioseq_external relations are read through the bioseq_external "
					   "Custom Scan; a physical scan path was reached instead."),
			 errhint("Ensure the extension is loaded and bioseq.enable_custom_scan is on.")));
	pg_unreachable();
}

/* Any write path fired against an inherently read-only external relation. */
[[noreturn]] void
external_read_only()
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("external relations are read-only")));
	pg_unreachable();
}

/* An unsupported physical capability (index/parallel/sample/rewrite) fired. */
[[noreturn]] void
external_not_supported(const char *feature)
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
bioseq_external_slot_callbacks(Relation relation)
{
	return &TTSOpsVirtual;
}

/* --- sequential scan: owned by the CustomScan -------------------------- */

TableScanDesc
bioseq_external_scan_begin(Relation relation, Snapshot snapshot,
						   int nkeys, ScanKey key,
						   ParallelTableScanDesc parallel_scan,
						   uint32 flags)
{
	external_requires_custom_scan();
}

void
bioseq_external_scan_end(TableScanDesc scan)
{
	/* Idempotent no-op: scan_begin never returns a descriptor, so there is
	 * nothing to tear down. Safe to call regardless. */
}

void
bioseq_external_rescan(TableScanDesc scan, ScanKey key, bool set_params,
					   bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	external_requires_custom_scan();
}

bool
bioseq_external_getnextslot(TableScanDesc scan, ScanDirection direction,
							TupleTableSlot *slot)
{
	external_requires_custom_scan();
}

/* --- parallel scan: external paths are always non-parallel ------------- */

Size
bioseq_external_parallelscan_estimate(Relation rel)
{
	external_not_supported("parallel scan");
}

Size
bioseq_external_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	external_not_supported("parallel scan");
}

void
bioseq_external_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	external_not_supported("parallel scan");
}

/* --- index fetch: external relations reject index DDL ------------------ */

IndexFetchTableData *
bioseq_external_index_fetch_begin(Relation rel)
{
	external_not_supported("index scan");
}

void
bioseq_external_index_fetch_reset(IndexFetchTableData *scan)
{
	/* no-op: index_fetch_begin never returns a handle */
}

void
bioseq_external_index_fetch_end(IndexFetchTableData *scan)
{
	/* no-op: index_fetch_begin never returns a handle */
}

bool
bioseq_external_index_fetch_tuple(IndexFetchTableData *scan, ItemPointer tid,
								  Snapshot snapshot, TupleTableSlot *slot,
								  bool *call_again, bool *all_dead)
{
	external_not_supported("index scan");
}

/* --- tuple fetch / tid / snapshot -------------------------------------- */

bool
bioseq_external_fetch_row_version(Relation relation, ItemPointer tid,
								  Snapshot snapshot, TupleTableSlot *slot)
{
	external_requires_custom_scan();
}

bool
bioseq_external_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	/* No physical tuples exist; a tid is never valid. Returning false (rather
	 * than raising) keeps predicate-style callers well-behaved. */
	return false;
}

void
bioseq_external_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	external_not_supported("tuple version chains");
}

bool
bioseq_external_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
										 Snapshot snapshot)
{
	/* No stored tuple can satisfy a snapshot. */
	return false;
}

TransactionId
bioseq_external_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	/* External relations have no indexes; nothing to delete. */
	delstate->ndeltids = 0;
	return InvalidTransactionId;
}

/* --- all DML is rejected (read-only) ----------------------------------- */

void
bioseq_external_tuple_insert(Relation relation, TupleTableSlot *slot,
							 CommandId cid, int options, BulkInsertState bistate)
{
	external_read_only();
}

void
bioseq_external_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
										 CommandId cid, int options,
										 BulkInsertState bistate, uint32 specToken)
{
	external_read_only();
}

void
bioseq_external_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
										   uint32 specToken, bool succeeded)
{
	external_read_only();
}

void
bioseq_external_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
							 CommandId cid, int options, BulkInsertState bistate)
{
	external_read_only();
}

TM_Result
bioseq_external_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
							 Snapshot snapshot, Snapshot crosscheck, bool wait,
							 TM_FailureData *tmfd, bool changingPart)
{
	external_read_only();
}

TM_Result
bioseq_external_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
							 CommandId cid, Snapshot snapshot, Snapshot crosscheck,
							 bool wait, TM_FailureData *tmfd,
							 LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	external_read_only();
}

TM_Result
bioseq_external_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot,
						   TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
						   LockWaitPolicy wait_policy, uint8 flags,
						   TM_FailureData *tmfd)
{
	external_read_only();
}

void
bioseq_external_finish_bulk_insert(Relation relation, int options)
{
	external_read_only();
}

/* --- storage lifecycle: validate, but create no physical storage ------- */

void
bioseq_external_relation_set_new_filelocator(Relation rel,
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
	 * Validate the options that were parsed into rd_options for this relation
	 * (a missing/unknown entity is a DDL error). When rd_options is null the
	 * WITH clause was absent entirely; the integrator's DDL/table-options hook
	 * is the authoritative gate for that case, so we do not hard-fail here.
	 */
	if (rel->rd_options != nullptr)
	{
		(void) external_entity_kind_from_options(rel);
	}

	/*
	 * External relations are non-materialised: no heap/init fork is created.
	 * relation_size() therefore reports zero and never touches smgr. Set the
	 * freeze horizons to "not tracked", matching a relation with no tuples.
	 */
	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;
}

void
bioseq_external_relation_nontransactional_truncate(Relation rel)
{
	external_not_supported("TRUNCATE");
}

void
bioseq_external_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	external_not_supported("storage rewrite");
}

void
bioseq_external_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
										  Relation OldIndex, bool use_sort,
										  TransactionId OldestXmin,
										  TransactionId *xid_cutoff,
										  MultiXactId *multi_cutoff,
										  double *num_tuples,
										  double *tups_vacuumed,
										  double *tups_recently_dead)
{
	external_not_supported("CLUSTER / VACUUM FULL");
}

void
bioseq_external_relation_vacuum(Relation rel, VacuumParams *params,
								BufferAccessStrategy bstrategy)
{
	/* Safe no-op: external relations have zero physical pages, so VACUUM and
	 * autovacuum have nothing to do. */
}

/* --- analyze: no local rows, no remote I/O ----------------------------- */

bool
bioseq_external_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	/* No physical blocks to sample. */
	return false;
}

bool
bioseq_external_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
										double *liverows, double *deadrows,
										TupleTableSlot *slot)
{
	/* Never contacts a provider; yields no sample tuples. */
	return false;
}

/* --- index build/validate: external relations reject index DDL --------- */

double
bioseq_external_index_build_range_scan(Relation table_rel, Relation index_rel,
									   IndexInfo *index_info, bool allow_sync,
									   bool anyvisible, bool progress,
									   BlockNumber start_blockno,
									   BlockNumber numblocks,
									   IndexBuildCallback callback,
									   void *callback_state, TableScanDesc scan)
{
	external_not_supported("index build");
}

void
bioseq_external_index_validate_scan(Relation table_rel, Relation index_rel,
									IndexInfo *index_info, Snapshot snapshot,
									ValidateIndexState *state)
{
	external_not_supported("index validate");
}

/* --- size / toast / estimate: zero physical footprint ------------------ */

uint64
bioseq_external_relation_size(Relation rel, ForkNumber forkNumber)
{
	/* No physical storage. */
	return 0;
}

bool
bioseq_external_relation_needs_toast_table(Relation rel)
{
	/* No tuples are stored, so no TOAST table is ever needed. */
	return false;
}

/*
 * Conservative, provider-free size estimate. Uses a fixed default row count
 * and the local column widths only; never issues a remote request. The
 * integrator may replace BIOSEQ_EXTERNAL_DEFAULT_ESTIMATED_ROWS with a GUC or
 * provider-supplied estimate later.
 */
constexpr double BIOSEQ_EXTERNAL_DEFAULT_ESTIMATED_ROWS = 1000.0;

void
bioseq_external_estimate_rel_size(Relation rel, int32 *attr_widths,
								  BlockNumber *pages, double *tuples,
								  double *allvisfrac)
{
	*pages = 0;
	*tuples = BIOSEQ_EXTERNAL_DEFAULT_ESTIMATED_ROWS;
	*allvisfrac = 0.0;

	/* Local (statistics/type) column widths only — no remote I/O. */
	get_rel_data_width(rel, attr_widths);
}

/* --- sample scan: rejected (also blocked at plan time) ----------------- */

bool
bioseq_external_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	external_not_supported("TABLESAMPLE");
}

bool
bioseq_external_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
									   TupleTableSlot *slot)
{
	external_not_supported("TABLESAMPLE");
}

/*
 * The routine itself. Field set and ORDER mirror bioseq_am_methods in
 * access_method.cppm exactly (PG17 TableAmRoutine); unset fields are
 * value-initialised to NULL. Bitmap callbacks are NULL so the planner never
 * generates a bitmap-heap path for external relations.
 */
const TableAmRoutine bioseq_external_am_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = bioseq_external_slot_callbacks,

	.scan_begin = bioseq_external_scan_begin,
	.scan_end = bioseq_external_scan_end,
	.scan_rescan = bioseq_external_rescan,
	.scan_getnextslot = bioseq_external_getnextslot,

	.parallelscan_estimate = bioseq_external_parallelscan_estimate,
	.parallelscan_initialize = bioseq_external_parallelscan_initialize,
	.parallelscan_reinitialize = bioseq_external_parallelscan_reinitialize,

	.index_fetch_begin = bioseq_external_index_fetch_begin,
	.index_fetch_reset = bioseq_external_index_fetch_reset,
	.index_fetch_end = bioseq_external_index_fetch_end,
	.index_fetch_tuple = bioseq_external_index_fetch_tuple,

	.tuple_fetch_row_version = bioseq_external_fetch_row_version,
	.tuple_tid_valid = bioseq_external_tuple_tid_valid,
	.tuple_get_latest_tid = bioseq_external_get_latest_tid,
	.tuple_satisfies_snapshot = bioseq_external_tuple_satisfies_snapshot,
	.index_delete_tuples = bioseq_external_index_delete_tuples,

	.tuple_insert = bioseq_external_tuple_insert,
	.tuple_insert_speculative = bioseq_external_tuple_insert_speculative,
	.tuple_complete_speculative = bioseq_external_tuple_complete_speculative,
	.multi_insert = bioseq_external_multi_insert,
	.tuple_delete = bioseq_external_tuple_delete,
	.tuple_update = bioseq_external_tuple_update,
	.tuple_lock = bioseq_external_tuple_lock,
	.finish_bulk_insert = bioseq_external_finish_bulk_insert,

	.relation_set_new_filelocator = bioseq_external_relation_set_new_filelocator,
	.relation_nontransactional_truncate = bioseq_external_relation_nontransactional_truncate,
	.relation_copy_data = bioseq_external_relation_copy_data,
	.relation_copy_for_cluster = bioseq_external_relation_copy_for_cluster,
	.relation_vacuum = bioseq_external_relation_vacuum,
	.scan_analyze_next_block = bioseq_external_scan_analyze_next_block,
	.scan_analyze_next_tuple = bioseq_external_scan_analyze_next_tuple,
	.index_build_range_scan = bioseq_external_index_build_range_scan,
	.index_validate_scan = bioseq_external_index_validate_scan,

	.relation_size = bioseq_external_relation_size,
	.relation_needs_toast_table = bioseq_external_relation_needs_toast_table,

	.relation_estimate_size = bioseq_external_estimate_rel_size,

/*
 * PG-18 dropped the per-block bitmap callback; keep the PG-17 field guarded so
 * this mirrors access_method.cppm and compiles on both.
 */
#if PG_VERSION_NUM < PG_VERSION_18
	.scan_bitmap_next_block = NULL,
#endif
	.scan_bitmap_next_tuple = NULL,
	.scan_sample_next_block = bioseq_external_scan_sample_next_block,
	.scan_sample_next_tuple = bioseq_external_scan_sample_next_tuple
};

} // namespace

const TableAmRoutine *
GetBioseqExternalTableAmRoutine(void)
{
	return &bioseq_external_am_methods;
}

bool
relation_uses_bioseq_external_am(Relation rel)
{
	return rel != nullptr && rel->rd_tableam == &bioseq_external_am_methods;
}

void
bioseq_external_tableam_init(void)
{
	if (bioseq_external_relopt_kind != static_cast<relopt_kind>(0))
	{
		/* Idempotent: already registered. */
		return;
	}

	bioseq_external_relopt_kind = add_reloption_kind();

	add_enum_reloption(bioseq_external_relopt_kind, "entity",
					   "Logical entity kind for a bioseq_external relation.",
					   bioseq_external_entity_members, EXTERNAL_ENTITY_UNSPECIFIED,
					   "Valid entity values are \"taxonomy\", \"catalog\", "
					   "\"record_gene\", \"record_location\", \"sequence\", "
					   "and \"sequence_chunk\".",
					   AccessExclusiveLock);

	add_string_reloption(bioseq_external_relopt_kind, "alphabet",
						 "Sequence alphabet for sequence/sequence_chunk entities.",
						 nullptr, validate_bioseq_external_alphabet,
						 AccessExclusiveLock);

	add_int_reloption(bioseq_external_relopt_kind, "storage_width",
					  "Sequence storage unit width for sequence entities.",
					  0, 0, 64, AccessExclusiveLock);

	add_int_reloption(bioseq_external_relopt_kind, "chunk_bases",
					  "Chunk length in bases for sequence_chunk entities.",
					  0, 0, INT_MAX, AccessExclusiveLock);
}

/* ======================================================================== */
/* C-ABI entry points called by bioseq_external--1.0.sql                     */
/* ======================================================================== */

/*
 * bioseq_external_handler — the real table_am_handler. Returns the distinct
 * external TAM identity (DOC0372 §"table access method").
 */
POSTGRES_FN(bioseq_external_handler)
{
	PG_RETURN_POINTER(GetBioseqExternalTableAmRoutine());
}

/*
 * Marker predicate functions. They carry semantics that are not row fields
 * (descendant-taxon matching; provider-native query) and are consumed by the
 * CustomScan at plan time. If one ever reaches a physical executor — i.e. the
 * custom path was not chosen — it raises rather than silently evaluating with
 * invented semantics. Same shape as the mmsearch stub in mod.cppm.
 */
extern "C" {

CXX_PG_FUNCTION_INFO_V1(bioseq_external_taxon_matches);
Datum
bioseq_external_taxon_matches(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("bioseq_external.taxon_matches is only valid inside a bioseq_external Custom Scan")));
	pg_unreachable();
}

CXX_PG_FUNCTION_INFO_V1(bioseq_external_native_query);
Datum
bioseq_external_native_query(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("bioseq_external.native_query is only valid inside a bioseq_external Custom Scan")));
	pg_unreachable();
}

} // extern "C"
