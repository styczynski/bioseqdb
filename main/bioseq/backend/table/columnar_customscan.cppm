/*-------------------------------------------------------------------------
 *
 * table/columnar_customscan.cppm
 *
 * Bioseq columnar CustomScan node. The planner_hook injects a CustomPath
 * pointing at this node ahead of postgres' default seq/index path; at
 * plan time it materializes a CustomScan, and at execution time we drive
 * the bioseq columnar reader through the CustomScanState helpers
 * defined here. The state object is BioseqScanState (defined in
 * table/bioseq_scan_state.cppm), which embeds CustomScanState as its
 * first field so PostgreSQL's reinterpret_cast across the C ABI stays
 * sound.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include <algorithm>
#include <cmath>

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"

#include "columnar/vectorization/columnar_vector_execution.h"
#include "columnar/vectorization/columnar_vector_types.h"

export module bioseq_table:custom_scan;

import bioseq_table_io;
import mlogging;
import storage_metadata;
import bioseq_table_types;
import bioseq_scan_state;
import pg_utils;
import bioseq_scheduler.dispatch_buffer;

ItemPointerData row_number_to_tid(uint64_t rowNumber);
const TableAmRoutine * GetColumnarTableAmRoutine(void);
uint64_t ColumnarTableRowCount(Relation relation);
bool IsColumnarTableAmTable(Oid relationId);
Bitmapset* ColumnarAttrNeeded(ScanState *ss, List *customList);
int64_t ColumnarScanChunkGroupsFiltered(ColumnarScanDesc columnarScanDesc);
IndexFetchTableData* bioseq_index_fetch_begin_extended(Relation rel, Bitmapset *attr_needed);
TableScanDesc
bioseq_beginscan_extended(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelColumnarScan parallelColumnarScan,
							bool returnVectorizedTuple);


static int32_t counter = 100;
static List* newtargetlist = nullptr;
static Const* const_score_node = nullptr;
static bioseq::simd::N_NEON::TupleDispatchBuffer buffer(1000);

/*
 * Pair of OIDs looked up from pg_proc for the bioseq extension's stub
 * SQL functions. Looked up on demand via LookupStubOids(); never cached
 * across calls because `drop extension; create extension` would leave a
 * process-wide cache pointing at the old, dropped pg_proc rows.
 */
struct BioseqStubOids {
	Oid mmsearch_oid;
	Oid bioseq_score_oid;
};

/*
 * LookupStubOids resolves the OIDs of mmsearch(anyelement, text) and
 * bioseq.score(anyelement). regprocedurein raises ereport(ERROR, ...)
 * on miss, so we assume both functions are present by the time any
 * bioseq-aware code path runs (CREATE EXTENSION has to succeed first).
 */
[[nodiscard]] static BioseqStubOids LookupStubOids() {
	BioseqStubOids result{};
	result.mmsearch_oid = DatumGetObjectId(DirectFunctionCall1(regprocedurein,
		CStringGetDatum("mmsearch(anyelement, text)")));
	result.bioseq_score_oid = DatumGetObjectId(DirectFunctionCall1(regprocedurein,
		CStringGetDatum("bioseq.score(anyelement)")));
	return result;
}

/*
 * Context passed to VarInjectorMutator through the void* context arg.
 */
struct VarInjectorCtx {
	Const* const_score_node;
	Oid bioseq_score_oid;
};

/* functions to cost paths in-place */
static void CostColumnarPaths(PlannerInfo *root, RelOptInfo *rel, Oid relationId);
static void CostColumnarIndexPath(PlannerInfo *root, RelOptInfo *rel, Oid relationId,
								  IndexPath *indexPath);
static void CostColumnarSeqPath(RelOptInfo *rel, Oid relationId, Path *path);
static void AdjustColumnarParallelScanCost(Path *path);
static void CostColumnarScan(PlannerInfo *root, RelOptInfo *rel, Oid relationId,
							 CustomPath *cpath, int numberOfColumnsRead,
							 int nClauses);

/* functions to add new paths */
static void AddColumnarScanPaths(PlannerInfo *root, RelOptInfo *rel,
								 RangeTblEntry *rte);
static Path * AddColumnarScanPath(PlannerInfo *root, RelOptInfo *rel,
								  RangeTblEntry *rte, Relids required_relids);

/* helper functions to be used when costing paths or altering them */
static void RemovePathsByPredicate(RelOptInfo *rel, PathPredicate removePathPredicate);
static bool IsNotIndexPath(Path *path);
static Cost ColumnarIndexScanAdditionalCost(PlannerInfo *root, RelOptInfo *rel,
											Oid relationId, IndexPath *indexPath);
static int RelationIdGetNumberOfAttributes(Oid relationId);
static Cost ColumnarPerStripeScanCost(RelOptInfo *rel, Oid relationId,
									  int numberOfColumnsRead);
static uint64_t ColumnarTableStripeCount(Oid relationId);
static Path * CreateColumnarSeqScanPath(PlannerInfo *root, RelOptInfo *rel,
										Oid relationId);
static void AddColumnarScanPathsRec(PlannerInfo *root, RelOptInfo *rel,
									RangeTblEntry *rte, Relids paramRelids,
									Relids candidateRelids,
									int depthLimit);

/* hooks and callbacks */
static void ColumnarSetRelPathlistHook(PlannerInfo *root, RelOptInfo *rel, Index rti,
									   RangeTblEntry *rte);
static void ColumnarGetRelationInfoHook(PlannerInfo *root, Oid relationObjectId,
										bool inhparent, RelOptInfo *rel);

static Plan * ColumnarScanPath_PlanCustomPath(PlannerInfo *root,
											  RelOptInfo *rel,
											  CustomPath *best_path,
											  List *tlist,
											  List *clauses,
											  List *custom_plans);
static List * ColumnarScanPath_ReparameterizeCustomPathByChild(PlannerInfo *root,
															   List *custom_private,
															   RelOptInfo *child_rel);
static Node * ColumnarScan_CreateCustomScanState(CustomScan *cscan);

static void ColumnarScan_BeginCustomScan(CustomScanState *node, EState *estate,
										 int eflags);

static TupleTableSlot * CustomExecScan(BioseqScanState *node,
									   ExecScanAccessMtd accessMtd,
									   ExecScanRecheckMtd recheckMtd);
static TupleTableSlot * ColumnarScan_ExecCustomScan(CustomScanState *node);
static void ColumnarScan_EndCustomScan(CustomScanState *node);
static void ColumnarScan_ReScanCustomScan(CustomScanState *node);
static void ColumnarScan_ExplainCustomScan(CustomScanState *node, List *ancestors,
										   ExplainState *es);
static Size Columnar_EstimateDSMCustomScan(CustomScanState *node,
										   ParallelContext *pcxt);
static void Columnar_InitializeDSMCustomScan(CustomScanState *node,
											ParallelContext *pcxt,
											void *coordinate);
static void Columnar_ReinitializeDSMCustomScan(CustomScanState *node,
											   ParallelContext *pcxt,
											   void *coordinate);
static void Columnar_InitializeWorkerCustomScan(CustomScanState *node,
												shm_toc *toc,
												void *coordinate);

/* helper functions to build strings for EXPLAIN */
static const char * ColumnarPushdownClausesStr(List *context, List *clauses);
static const char * ColumnarProjectedColumnsStr(List *context,
												List *projectedColumns);
static List * set_deparse_context_planstate(List *dpcontext, Node *node,
											List *ancestors);

/* other helpers */
static List * ColumnarVarNeeded(BioseqScanState *columnarScanState);

/* saved hook value in case of unload */
static set_rel_pathlist_hook_type PreviousSetRelPathlistHook = NULL;
static get_relation_info_hook_type PreviousGetRelationInfoHook = NULL;

static bool EnableColumnarCustomScan = true;
static bool EnableColumnarQualPushdown = true;
static double ColumnarQualPushdownCorrelationThreshold = 0.4;
static int ColumnarMaxCustomScanPaths = 64;
static int ColumnarPlannerDebugLevel = DEBUG3;


const CustomPathMethods ColumnarScanPathMethods = {
	.CustomName = "BioseqScan",
	.PlanCustomPath = ColumnarScanPath_PlanCustomPath,
	.ReparameterizeCustomPathByChild = ColumnarScanPath_ReparameterizeCustomPathByChild,
};

const CustomScanMethods ColumnarScanScanMethods = {
	.CustomName = "BioseqScan",
	.CreateCustomScanState = ColumnarScan_CreateCustomScanState,
};

const CustomExecMethods ColumnarScanExecuteMethods = {
	.CustomName = "BioseqScan",

	.BeginCustomScan = ColumnarScan_BeginCustomScan,
	.ExecCustomScan = ColumnarScan_ExecCustomScan,
	.EndCustomScan = ColumnarScan_EndCustomScan,
	.ReScanCustomScan = ColumnarScan_ReScanCustomScan,

	.ExplainCustomScan = ColumnarScan_ExplainCustomScan,

	.EstimateDSMCustomScan = Columnar_EstimateDSMCustomScan,
	.InitializeDSMCustomScan = Columnar_InitializeDSMCustomScan,
	.ReInitializeDSMCustomScan = Columnar_ReinitializeDSMCustomScan,
	.InitializeWorkerCustomScan = Columnar_InitializeWorkerCustomScan
};

static const config_enum_entry debug_level_options[] = {
	{ "debug5", DEBUG5, false },
	{ "debug4", DEBUG4, false },
	{ "debug3", DEBUG3, false },
	{ "debug2", DEBUG2, false },
	{ "debug1", DEBUG1, false },
	{ "debug", DEBUG2, true },
	{ "info", INFO, false },
	{ "notice", NOTICE, false },
	{ "warning", WARNING, false },
	{ "log", LOG, false },
	{ NULL, 0, false }
};


export const CustomScanMethods *
bioseq_customscan_methods(void)
{
	return &ColumnarScanScanMethods;
}


/*
 * bioseq_customscan_init installs the hook required to intercept the postgres planner and
 * provide extra paths for columnar tables
 */
export void
bioseq_customscan_init()
{
	PreviousSetRelPathlistHook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = ColumnarSetRelPathlistHook;

	PreviousGetRelationInfoHook = get_relation_info_hook;
	get_relation_info_hook = ColumnarGetRelationInfoHook;

	/* register customscan specific GUC's */
	DefineCustomBoolVariable(
		"bioseq.enable_custom_scan",
		"Enables the use of a custom scan to push projections and quals "
					 "into the storage layer.",
		NULL,
		&EnableColumnarCustomScan,
		true,
		PGC_USERSET,
		GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
		NULL, NULL, NULL);
	DefineCustomBoolVariable(
		"bioseq.enable_qual_pushdown",
		"Enables qual pushdown into bioseq. This has no effect unless "
					 "bioseq.enable_custom_scan is true.",
		NULL,
		&EnableColumnarQualPushdown,
		true,
		PGC_USERSET,
		GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
		NULL, NULL, NULL);
	DefineCustomRealVariable(
		"bioseq.qual_pushdown_correlation_threshold",
		"Correlation threshold to attempt to push a qual "
					 "referencing the given column. A value of 0 means "
					 "attempt to push down all quals, even if the column "
					 "is uncorrelated.",
		NULL,
		&ColumnarQualPushdownCorrelationThreshold,
		0.4,
		0.0,
		1.0,
		PGC_USERSET,
		GUC_NO_SHOW_ALL |GUC_NOT_IN_SAMPLE,
		NULL, NULL, NULL);
	DefineCustomIntVariable(
		"bioseq.max_custom_scan_paths",
		"Maximum number of custom scan paths to generate "
					 "for a columnar table when planning.",
		NULL,
		&ColumnarMaxCustomScanPaths,
		64,
		1,
		1024,
		PGC_USERSET,
		GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
		NULL, NULL, NULL);
	DefineCustomEnumVariable(
		"bioseq.planner_debug_level",
		"Message level for columnar planning information.",
		NULL,
		&ColumnarPlannerDebugLevel,
		DEBUG3,
		debug_level_options,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	RegisterCustomScanMethods(&ColumnarScanScanMethods);
}

static void
ColumnarSetRelPathlistHook(PlannerInfo *root, RelOptInfo *rel, Index rti,
						   RangeTblEntry *rte)
{
	JustLog("[bioseq] pg: ColumnarSetRelPathlistHook() entered");
	/* call into previous hook if assigned */
	if (PreviousSetRelPathlistHook)
	{
		PreviousSetRelPathlistHook(root, rel, rti, rte);
	}

	if (!OidIsValid(rte->relid) || rte->rtekind != RTE_RELATION || rte->inh)
	{
		/* some calls to the pathlist hook don't have a valid relation set. Do nothing */
		return;
	}

	/*
	 * Here we want to inspect if this relation pathlist hook is accessing a columnar table.
	 * If that is the case we want to insert an extra path that pushes down the projection
	 * into the scan of the table to minimize the data read.
	 */
	Relation relation = RelationIdGetRelation(rte->relid);
	if (relation->rd_tableam == GetColumnarTableAmRoutine())
	{
		if (rte->tablesample != NULL)
		{
			PgReport(ERROR).code(ERRCODE_FEATURE_NOT_SUPPORTED)
							.msg("sample scans not supported on columnar tables");
		}

		/*
		 * There are cases where IndexPath is normally more preferrable over
		 * SeqPath for heapAM but not for columnarAM. In such cases, an
		 * IndexPath could wrongly dominate a SeqPath based on the costs
		 * estimated by postgres earlier. For this reason, here we manually
		 * create a SeqPath, estimate the cost based on columnarAM and append
		 * to pathlist.
		 *
		 * Before doing that, we first re-cost all the existing paths so that
		 * add_path makes correct cost comparisons when appending our SeqPath.
		 */
		CostColumnarPaths(root, rel, rte->relid);

		Path *seqPath = CreateColumnarSeqScanPath(root, rel, rte->relid);
		add_path(rel, seqPath);

		if (EnableColumnarCustomScan)
		{
			/*
			 * When columnar custom scan is enabled (bioseq.enable_custom_scan),
			 * we only consider ColumnarScanPath's & IndexPath's. For this reason,
			 * we remove other paths and re-estimate IndexPath costs to make accurate
			 * comparisons between them.
			 *
			 * Even more, we might calculate an equal cost for a
			 * ColumnarCustomScan and a SeqPath if we are reading all columns
			 * of given table since we don't consider chunk group filtering
			 * when costing ColumnarCustomScan.
			 * In that case, if we don't remove SeqPath's, we might wrongly choose
			 * SeqPath thinking that its cost would be equal to ColumnarCustomScan.
			 */
			RemovePathsByPredicate(rel, IsNotIndexPath);
			AddColumnarScanPaths(root, rel, rte);
		}
	}
	JustLog("[bioseq] pg: ColumnarSetRelPathlistHook() relation_close");
	RelationClose(relation);
}


static void
ColumnarGetRelationInfoHook(PlannerInfo *root, Oid relationObjectId,
							bool inhparent, RelOptInfo *rel)
{
	JustLog("[bioseq] pg: ColumnarGetRelationInfoHook() entered");
	if (PreviousGetRelationInfoHook)
	{
		PreviousGetRelationInfoHook(root, relationObjectId, inhparent, rel);
	}

	if (IsColumnarTableAmTable(relationObjectId))
	{
		/* disable parallel query */
		rel->rel_parallel_workers = 0;

		/* disable index-only scan */
		for(auto indexOptInfo : PList<IndexOptInfo>(rel->indexlist)) {
			memset(indexOptInfo->canreturn, false, indexOptInfo->ncolumns * sizeof(bool));
		}
	}
}


/*
 * RemovePathsByPredicate removes the paths that removePathPredicate
 * evaluates to true from pathlist of given rel.
 */
static void
RemovePathsByPredicate(RelOptInfo *rel, PathPredicate removePathPredicate)
{
	JustLog("[bioseq] pg: RemovePathsByPredicate() entered");
	PList<Path> filteredPathList;
	PList<Path> filteredPartialPathList;

	for(auto path : PList<Path>(rel->pathlist))
	{
		if (!removePathPredicate(path))
		{
			filteredPathList.append(path);
		}
	}

	rel->pathlist = filteredPathList.release();

	for(auto path : PList<Path>(rel->partial_pathlist))
	{
		if (!removePathPredicate(path))
		{
			filteredPartialPathList.append(path);
		}
	}

	rel->partial_pathlist = filteredPartialPathList.release();
}


/*
 * IsNotIndexPath returns true if given path is not an IndexPath.
 */
static bool
IsNotIndexPath(Path *path)
{
	return !IsA(path, IndexPath);
}


/*
 * CreateColumnarSeqScanPath returns Path for sequential scan on columnar
 * table with relationId.
 */
static Path *
CreateColumnarSeqScanPath(PlannerInfo *root, RelOptInfo *rel, Oid relationId)
{
	JustLog("[bioseq] pg: CreateColumnarSeqScanPath() entered");
	/* columnar doesn't support parallel scan */
	int parallelWorkers = 0;

	Relids requiredOuter = rel->lateral_relids;
	Path *path = create_seqscan_path(root, rel, requiredOuter, parallelWorkers);
	CostColumnarSeqPath(rel, relationId, path);
	return path;
}


/*
 * CostColumnarPaths re-costs paths of given RelOptInfo for
 * columnar table with relationId.
 */
static void
CostColumnarPaths(PlannerInfo *root, RelOptInfo *rel, Oid relationId)
{
	JustLog("[bioseq] pg: CostColumnarPaths() entered");
	for(auto path : PList<Path>(rel->pathlist))
	{
		if (IsA(path, IndexPath))
		{
			/*
			 * Since we don't provide implementations for scan_bitmap_next_block
			 * & scan_bitmap_next_tuple, postgres doesn't generate bitmap index
			 * scan paths for columnar tables already (see related comments in
			 * TableAmRoutine). For this reason, we only consider IndexPath's
			 * here.
			 */
			CostColumnarIndexPath(root, rel, relationId, reinterpret_cast<IndexPath *>(path));
		}
		else if (path->pathtype == T_SeqScan)
		{
			CostColumnarSeqPath(rel, relationId, path);
		}
	}
}


/*
 * CostColumnarIndexPath re-costs given index path for columnar table with
 * relationId.
 */
static void
CostColumnarIndexPath(PlannerInfo *root, RelOptInfo *rel, Oid relationId,
					  IndexPath *indexPath)
{
	if (!enable_indexscan)
	{
		JustLog("[bioseq] pg: CostColumnarIndexPath() skipped (enable_indexscan=false)");
		/* costs are already set to disable_cost, don't adjust them */
		return;
	}

	pg_log(DEBUG4, "columnar table index scan costs estimated by "
				   "indexAM: startup cost = {:.10f}, total cost = "
				   "{:.10f}", indexPath->path.startup_cost,
				   indexPath->path.total_cost);

	/*
	 * We estimate the cost for columnar table read during index scan. Also,
	 * instead of overwriting total cost, we "add" ours to the cost estimated
	 * by indexAM since we should consider index traversal related costs too.
	 */
	
	if (!bioseq_index_scan)
	{
		JustLog("[bioseq] pg: CostColumnarIndexPath() bioseq_index_scan disabled, adding columnar cost");
		Cost columnarIndexScanCost = ColumnarIndexScanAdditionalCost(root, rel, relationId,
																	indexPath);
		indexPath->path.total_cost += columnarIndexScanCost;
	}

	JustLog("[bioseq] pg: CostColumnarIndexPath() cost computation complete");

	pg_log(DEBUG4, "columnar table index scan costs re-estimated "
				   "by columnarAM (including indexAM costs): "
				   "startup cost = {:.10f}, total cost = {:.10f}",
				   indexPath->path.startup_cost,
				   indexPath->path.total_cost);
}


/*
 * ColumnarIndexScanAdditionalCost returns additional cost estimated for
 * index scan described by IndexPath for columnar table with relationId.
 */
static Cost
ColumnarIndexScanAdditionalCost(PlannerInfo *root, RelOptInfo *rel,
								Oid relationId, IndexPath *indexPath)
{
	JustLog("[bioseq] pg: ColumnarIndexScanAdditionalCost() entered");
	int numberOfColumnsRead = RelationIdGetNumberOfAttributes(relationId);
	Cost perStripeCost = ColumnarPerStripeScanCost(rel, relationId, numberOfColumnsRead);

	/*
	 * We don't need to pass correct loop count to amcostestimate since we
	 * will only use index correlation & index selectivity, and loop count
	 * doesn't have any effect on those two.
	 */
	double fakeLoopCount = 1;
	Cost fakeIndexStartupCost;
	Cost fakeIndexTotalCost;
	double fakeIndexPages;
	Selectivity indexSelectivity;
	double indexCorrelation;
	amcostestimate_function amcostestimate = indexPath->indexinfo->amcostestimate;
	amcostestimate(root, indexPath, fakeLoopCount, &fakeIndexStartupCost,
				   &fakeIndexTotalCost, &indexSelectivity,
				   &indexCorrelation, &fakeIndexPages);

	Relation relation = RelationIdGetRelation(relationId);
	uint64_t rowCount = ColumnarTableRowCount(relation);
	RelationClose(relation);
	double estimatedRows = rowCount * indexSelectivity;

	/*
	 * In the worst case (i.e no correlation between the column & the index),
	 * we need to read a different stripe for each row.
	 */
	double maxStripeReadCount = estimatedRows;

	/*
	 * In the best case (i.e the column is fully correlated with the index),
	 * we wouldn't read the same stripe again and again thanks
	 * to locality.
	 */
	double avgStripeRowCount =
		rowCount / static_cast<double>(ColumnarTableStripeCount(relationId));
	double minStripeReadCount = estimatedRows / avgStripeRowCount;

	/*
	 * While being close to 0 means low correlation, being close to -1 or +1
	 * means high correlation. For index scans on columnar tables, it doesn't
	 * matter if the column and the index are "correlated" (+1) or
	 * "anti-correlated" (-1) since both help us avoiding from reading the
	 * same stripe again and again.
	 */
	double absIndexCorrelation = std::abs(indexCorrelation);

	/*
	 * To estimate the number of stripes that we need to read, we do linear
	 * interpolation between minStripeReadCount & maxStripeReadCount. To do
	 * that, we use complement to 1 of absolute correlation, where being
	 * close to 0 means high correlation and being close to 1 means low
	 * correlation.
	 * In practice, we only want to do an index scan when absIndexCorrelation
	 * is 1 (or extremely close to it), or when the absolute number of tuples
	 * returned is very small. Other cases will have a prohibitive cost.
	 */
	double complementIndexCorrelation = 1 - absIndexCorrelation;
	double estimatedStripeReadCount =
		minStripeReadCount + complementIndexCorrelation * (maxStripeReadCount -
														   minStripeReadCount);

	/* even in the best case, we will read a single stripe */
	estimatedStripeReadCount = std::max(estimatedStripeReadCount, 1.0);

	Cost scanCost = perStripeCost * estimatedStripeReadCount;

	pg_log(DEBUG4, "re-costing index scan for columnar table: "
				   "selectivity = {:.10f}, complement abs "
				   "correlation = {:.10f}, per stripe cost = {:.10f}, "
				   "estimated stripe read count = {:.10f}, "
				   "total additional cost = {:.10f}",
				   indexSelectivity, complementIndexCorrelation,
				   perStripeCost, estimatedStripeReadCount,
				   scanCost);

	return scanCost;
}


/*
 * CostColumnarSeqPath sets costs given seq path for columnar table with
 * relationId.
 */
static void
CostColumnarSeqPath(RelOptInfo *rel, Oid relationId, Path *path)
{
	if (!enable_seqscan)
	{
		/* costs are already set to disable_cost, don't adjust them */
		return;
	}

	/*
	 * Seq scan doesn't support projection or qual pushdown, so we will read
	 * all the stripes and all the columns.
	 */
	double stripesToRead = ColumnarTableStripeCount(relationId);
	int numberOfColumnsRead = RelationIdGetNumberOfAttributes(relationId);

	path->rows = rel->tuples;
	path->startup_cost = 0;
	path->total_cost = stripesToRead *
					   ColumnarPerStripeScanCost(rel, relationId, numberOfColumnsRead);
}


/*
 * RelationIdGetNumberOfAttributes returns number of attributes that relation
 * with relationId has.
 */
static int
RelationIdGetNumberOfAttributes(Oid relationId)
{
	Relation relation = RelationIdGetRelation(relationId);
	int nattrs = relation->rd_att->natts;
	RelationClose(relation);
	return nattrs;
}


/*
 * CheckVarStats() checks whether a qual involving this Var is likely to be
 * useful based on the correlation stats. If so, or if stats are unavailable,
 * return true; otherwise return false and sets absVarCorrelation in case
 * caller wants to use for logging purposes.
 */
static bool
CheckVarStats(PlannerInfo *root, Var *var, Oid sortop, float *absVarCorrelation)
{
	JustLog("[bioseq] pg: CheckVarStats() entered");
	/*
	 * Collect isunique, ndistinct, and varCorrelation.
	 */
	VariableStatData varStatData;
	examine_variable(root, reinterpret_cast<Node *>(var), var->varno, &varStatData);
	if (varStatData.rel == NULL ||
		!HeapTupleIsValid(varStatData.statsTuple))
	{
		return true;
	}

	AttStatsSlot sslot;
	if (!get_attstatsslot(&sslot, varStatData.statsTuple,
						  STATISTIC_KIND_CORRELATION, sortop,
						  ATTSTATSSLOT_NUMBERS))
	{
		ReleaseVariableStats(varStatData);
		return true;
	}

	Assert(sslot.nnumbers == 1);

	float varCorrelation = sslot.numbers[0];

	ReleaseVariableStats(varStatData);

	/*
	 * If the Var is not highly correlated, then the chunk's min/max bounds
	 * will be nearly useless.
	 */
	if (std::abs(varCorrelation) < ColumnarQualPushdownCorrelationThreshold)
	{
		if (absVarCorrelation)
		{
			/*
			 * Report absVarCorrelation if caller wants to know why given
			 * var is rejected.
			 */
			*absVarCorrelation = std::abs(varCorrelation);
		}
		return false;
	}

	return true;
}


/*
 * ExprReferencesRelid returns true if any of the Expr's Vars refer to the
 * given relid; false otherwise.
 */
static bool
ExprReferencesRelid(Expr *expr, Index relid)
{
	List *exprVars = pull_var_clause(
		reinterpret_cast<Node *>(expr), PVC_RECURSE_AGGREGATES |
		PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS);
	for (auto var : PList<Var>(exprVars))
	{
		if (var->varno == relid)
		{
			return true;
		}
	}
	return false;
}

/* ColumnarTableRowCount returns the exact row count of a table using skiplists */
uint64_t
ColumnarTableRowCount(Relation relation)
{
	uint64_t totalRowCount = 0;
	List *stripeList = StripesForRelfilenode(relation->rd_locator, ForwardScanDirection);

	for (auto stripeMetadata : PList<StripeMetadata>(stripeList))
	{
		totalRowCount += stripeMetadata->rowCount;
	}

	return totalRowCount;
}


/*
 * ExtractPushdownClause extracts an Expr node from given clause for pushing down
 * into the given rel (including join clauses). This test may not be exact in
 * all cases; it's used to reduce the search space for parameterization.
 *
 * Note that we don't try to handle cases like "Var + ExtParam = 3". That
 * would require going through eval_const_expression after parameter binding,
 * and that doesn't seem worth the effort. Here we just look for "Var op Expr"
 * or "Expr op Var", where Var references rel and Expr references other rels
 * (or no rels at all).
 *
 * Moreover, this function also looks into BoolExpr's to recursively extract
 * pushdownable OpExpr's of them:
 * i)   AND_EXPR:
 *      Take pushdownable args of AND expressions by ignoring the other args.
 * ii)  OR_EXPR:
 *      Ignore the whole OR expression if we cannot extract a pushdownable Expr
 *      from one of its args.
 * iii) NOT_EXPR:
 *      Simply ignore NOT expressions since we don't expect to see them before
 *      an expression that we can pushdown, see the comment in function.
 *
 * The reasoning for those three rules could also be summarized as such;
 * for any expression that we cannot push-down, we must assume that it
 * evaluates to true.
 *
 * For example, given following WHERE clause:
 * (
 *     (a > random() OR a < 30)
 *     AND
 *     a < 200
 * ) OR
 * (
 *     a = 300
 *     OR
 *     a > 400
 * );
 * Even if we can pushdown (a < 30), we cannot pushdown (a > random() OR a < 30)
 * due to (a > random()). However, we can pushdown (a < 200), so we extract
 * (a < 200) from the lhs of the top level OR expression.
 *
 * For the rhs of the top level OR expression, since we can pushdown both (a = 300)
 * and (a > 400), we take this part as is.
 *
 * Finally, since both sides of the top level OR expression yielded pushdownable
 * expressions, we will pushdown the following:
 *  (a < 200) OR ((a = 300) OR (a > 400))
 */
static Expr *
ExtractPushdownClause(PlannerInfo *root, RelOptInfo *rel, Node *node)
{
	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	if (node == NULL)
	{
		return NULL;
	}

	if (IsA(node, BoolExpr))
	{
		BoolExpr *boolExpr = castNode(BoolExpr, node);
		if (boolExpr->boolop == NOT_EXPR)
		{
			/*
			 * Standard planner should have already applied de-morgan rule to
			 * simple NOT expressions. If we encounter with such an expression
			 * here, then it can't be a pushdownable one, such as:
			 *   WHERE id NOT IN (SELECT id FROM something).
			 */
			pg_log(ColumnarPlannerDebugLevel,
				   "columnar planner: cannot push down clause: "
				   "must not contain a subplan");
			return NULL;
		}

		PList<Expr> pushdownableArgs;

		for(auto boolExprArg : PList<Node>(boolExpr->args))
		{
			Expr *pushdownableArg = ExtractPushdownClause(root, rel,
														  reinterpret_cast<Node *>(boolExprArg));
			if (pushdownableArg)
			{
				pushdownableArgs.append(pushdownableArg);
			}
			else if (boolExpr->boolop == OR_EXPR)
			{
				pg_log(ColumnarPlannerDebugLevel,
					   "columnar planner: cannot push down clause: "
					   "all arguments of an OR expression must be "
					   "pushdownable but one of them was not, due "
					   "to the reason given above");
				return NULL;
			}

			/* simply skip AND args that we cannot pushdown */
		}

		int npushdownableArgs = pushdownableArgs.length();
		if (npushdownableArgs == 0)
		{
			pg_log(ColumnarPlannerDebugLevel,
				   "columnar planner: cannot push down clause: "
				   "none of the arguments were pushdownable, "
				   "due to the reason(s) given above");
			return NULL;
		}
		else if (npushdownableArgs == 1)
		{
			return pushdownableArgs.first();
		}

		if (boolExpr->boolop == AND_EXPR)
		{
			return make_andclause(pushdownableArgs);
		}
		else if (boolExpr->boolop == OR_EXPR)
		{
			return make_orclause(pushdownableArgs);
		}
		else
		{
			/* already discarded NOT expr, so should not be reachable */
			return NULL;
		}
	}

	if (!IsA(node, OpExpr) || list_length(reinterpret_cast<OpExpr *>(node)->args) != 2)
	{
		pg_log(ColumnarPlannerDebugLevel,
			   "columnar planner: cannot push down clause: "
			   "must be binary operator expression");
		return NULL;
	}

	OpExpr *opExpr = castNode(OpExpr, node);
	PList<Expr> args(opExpr->args);
	Expr *lhs = args[0];
	Expr *rhs = args[1];

	Var *varSide;
	Expr *exprSide;

	if (IsA(lhs, Var) && reinterpret_cast<Var *>(lhs)->varno == rel->relid &&
		!ExprReferencesRelid(reinterpret_cast<Expr *>(rhs), rel->relid))
	{
		varSide = castNode(Var, lhs);
		exprSide = rhs;
	}
	else if (IsA(rhs, Var) && reinterpret_cast<Var *>(rhs)->varno == rel->relid &&
			 !ExprReferencesRelid(reinterpret_cast<Expr *>(lhs), rel->relid))
	{
		varSide = castNode(Var, rhs);
		exprSide = lhs;
	}
	else
	{
		PgReport(ColumnarPlannerDebugLevel)
				.msg("columnar planner: cannot push down clause: "
					 "must match 'Var <op> Expr' or 'Expr <op> Var'")
				.hint("Var must only reference this rel, "
					  "and Expr must not reference this rel");
		return NULL;
	}

	if (varSide->varattno <= 0)
	{
		pg_log(ColumnarPlannerDebugLevel,
			   "columnar planner: cannot push down clause: "
			   "var is whole-row reference or system column");
		return NULL;
	}

	if (contain_volatile_functions(reinterpret_cast<Node *>(exprSide)))
	{
		pg_log(ColumnarPlannerDebugLevel,
			   "columnar planner: cannot push down clause: "
			   "expr contains volatile functions");
		return NULL;
	}

	/* only the default opclass is used for qual pushdown. */
	Oid varOpClass = GetDefaultOpClass(varSide->vartype, BTREE_AM_OID);
	Oid varOpFamily;
	Oid varOpcInType;

	if (!OidIsValid(varOpClass) ||
		!get_opclass_opfamily_and_input_type(varOpClass, &varOpFamily,
											 &varOpcInType))
	{
		pg_log(ColumnarPlannerDebugLevel,
			   "columnar planner: cannot push down clause: "
			   "cannot find default btree opclass and opfamily for type: {}",
			   format_type_be(varSide->vartype));
		return NULL;
	}

	if (!op_in_opfamily(opExpr->opno, varOpFamily))
	{
		pg_log(ColumnarPlannerDebugLevel,
			   "columnar planner: cannot push down clause: "
			   "operator {} not a member of opfamily {}",
			   opExpr->opno, varOpFamily);
		return NULL;
	}

	Oid sortop = get_opfamily_member(varOpFamily, varOpcInType,
									 varOpcInType, BTLessStrategyNumber);
	Assert(OidIsValid(sortop));

	/*
	 * Check that statistics on the Var support the utility of this
	 * clause.
	 */
	float absVarCorrelation = 0;
	if (!CheckVarStats(root, varSide, sortop, &absVarCorrelation))
	{
		pg_log(ColumnarPlannerDebugLevel,
			   "columnar planner: cannot push down clause: "
			   "absolute correlation ({:.3f}) of var attribute {} is "
			   "smaller than the value configured in "
			   "\"bioseq.qual_pushdown_correlation_threshold\" "
			   "({:.3f})", absVarCorrelation, varSide->varattno,
			   ColumnarQualPushdownCorrelationThreshold);
		return NULL;
	}

	return reinterpret_cast<Expr *>(node);
}


/*
 * FilterPushdownClauses filters for clauses that are candidates for pushing
 * down into rel.
 */
static List *
FilterPushdownClauses(PlannerInfo *root, RelOptInfo *rel, List *inputClauses)
{
	PList<RestrictInfo> filteredClauses;
	for (auto rinfo : PList<RestrictInfo>(inputClauses))
	{
		/*
		 * Ignore clauses that don't refer to this rel, and pseudoconstants.
		 *
		 * XXX: A pseudoconstant may be of use, but it doesn't make sense to
		 * push it down because it doesn't contain any Vars. Look into if
		 * there's something we should do with pseudoconstants here.
		 */
		if (rinfo->pseudoconstant ||
			!bms_is_member(rel->relid, rinfo->required_relids))
		{
			continue;
		}

		Expr *pushdownableExpr = ExtractPushdownClause(root, rel, reinterpret_cast<Node *>(rinfo->clause));
		if (!pushdownableExpr)
		{
			continue;
		}

		rinfo = copyObject(rinfo);
		rinfo->clause = pushdownableExpr;
		filteredClauses.append(rinfo);
	}

	return filteredClauses.release();
}


/*
 * PushdownJoinClauseMatches is a callback that returns true, indicating that
 * we want all of the clauses from generate_implied_equalities_for_column().
 */
static bool
PushdownJoinClauseMatches(PlannerInfo *root, RelOptInfo *rel,
						  EquivalenceClass *ec, EquivalenceMember *em,
						  void *arg)
{
	return true;
}


/*
 * FindPushdownJoinClauses finds join clauses, including those implied by ECs,
 * that may be pushed down.
 */
static List *
FindPushdownJoinClauses(PlannerInfo *root, RelOptInfo *rel)
{
	PList<RestrictInfo> joinClauses = copyObject(rel->joininfo);

	/*
	 * Here we are generating the clauses just so we can later extract the
	 * interesting relids. This is somewhat wasteful, but it allows us to
	 * filter out joinclauses, reducing the number of relids we need to
	 * consider.
	 *
	 * XXX: also find additional clauses for joininfo that are implied by ECs?
	 */
	List *ecClauses = generate_implied_equalities_for_column(
		root, rel, PushdownJoinClauseMatches, NULL,
		rel->lateral_referencers);
	joinClauses.concat(ecClauses);

	return FilterPushdownClauses(root, rel, joinClauses.release());
}


/*
 * FindCandidateRelids identifies candidate rels for parameterization from the
 * list of join clauses.
 *
 * Some rels cannot be considered for parameterization, such as a partitioned
 * parent of the given rel. Other rels are just not useful because they don't
 * appear in a join clause that could be pushed down.
 */
static Relids
FindCandidateRelids(PlannerInfo *root, RelOptInfo *rel, List *joinClauses)
{
	Relids candidateRelids = NULL;
	for (auto rinfo : PList<RestrictInfo>(joinClauses))
	{
		candidateRelids = bms_add_members(candidateRelids,
										  rinfo->required_relids);
	}

	candidateRelids = bms_del_members(candidateRelids, rel->relids);
	candidateRelids = bms_del_members(candidateRelids, rel->lateral_relids);
	candidateRelids = bms_del_members(candidateRelids, root->outer_join_rels);
	return candidateRelids;
}


/*
 * Combinations() calculates the number of combinations of n things taken k at
 * a time. When the correct result is large, the calculation may produce a
 * non-integer result, or overflow to inf, which caller should handle
 * appropriately.
 *
 * Use the following two formulae from Knuth TAoCP, 1.2.6:
 *    (2) Combinations(n, k) = (n*(n-1)..(n-k+1)) / (k*(k-1)..1)
 *    (5) Combinations(n, k) = Combinations(n, n-k)
 */
static double
Combinations(int n, int k)
{
	double v = 1;

	/*
	 * If k is close to n, then both the numerator and the denominator are
	 * close to n!, and we may overflow even if the input is reasonable
	 * (e.g. Combinations(500, 500)). Use formula (5) to choose the smaller,
	 * but equivalent, k.
	 */
	k = std::min(k, n - k);

	/* calculate numerator of formula (2) first */
	for (int i = n; i >= n - k + 1; i--)
	{
		v *= i;
	}

	/*
	 * Divide by each factor in the denominator of formula (2), skipping
	 * division by 1.
	 */
	for (int i = k; i >= 2; i--)
	{
		v /= i;
	}

	return v;
}


/*
 * ChooseDepthLimit() calculates the depth limit for the parameterization
 * search, given the number of candidate relations.
 *
 * The maximum number of paths generated for a given depthLimit is:
 *
 *    Combinations(nCandidates, 0) + Combinations(nCandidates, 1) + ... +
 *    Combinations(nCandidates, depthLimit)
 *
 * There's no closed formula for a partial sum of combinations, so just keep
 * increasing the depth until the number of combinations exceeds the limit.
 */
static int
ChooseDepthLimit(int nCandidates)
{
	if (!EnableColumnarQualPushdown)
	{
		return 0;
	}

	int depth = 0;
	double numPaths = 1;

	while (depth < nCandidates)
	{
		numPaths += Combinations(nCandidates, depth + 1);

		if (numPaths > static_cast<double>(ColumnarMaxCustomScanPaths))
		{
			break;
		}

		depth++;
	}

	return depth;
}


/*
 * AddColumnarScanPaths is the entry point for recursively generating
 * parameterized paths. See AddColumnarScanPathsRec() for discussion.
 */
static void
AddColumnarScanPaths(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	JustLog("[bioseq] pg: AddColumnarScanPaths() entered");
	List *joinClauses = FindPushdownJoinClauses(root, rel);
	Relids candidateRelids = FindCandidateRelids(root, rel, joinClauses);

	int depthLimit = ChooseDepthLimit(bms_num_members(candidateRelids));

	/* must always parameterize by lateral refs */
	Relids paramRelids = bms_copy(rel->lateral_relids);

	AddColumnarScanPathsRec(root, rel, rte, paramRelids, candidateRelids,
							depthLimit);

	JustLog("[bioseq] pg: AddColumnarScanPaths() returning");
}


/*
 * AddColumnarScanPathsRec is a recursive function to search the
 * parameterization space and add CustomPaths for columnar scans.
 *
 * The set paramRelids is the parameterization at the current level, and
 * candidateRelids is the set from which we draw to generate paths with
 * greater parameterization.
 *
 * Columnar tables resemble indexes because of the ability to push down
 * quals. Ordinary quals, such as x = 7, can be pushed down easily. But join
 * quals of the form "x = y" (where "y" comes from another rel) require the
 * proper parameterization.
 *
 * Paths that require more outer rels can push down more join clauses that
 * depend on those outer rels. But requiring more outer rels gives the planner
 * fewer options for the shape of the plan. That means there is a trade-off,
 * and we should generate plans of various parameterizations, then let the
 * planner choose. We always need to generate one minimally-parameterized path
 * (parameterized only by lateral refs, if present) to make sure that at least
 * one path can be chosen. Then, we generate as many parameterized paths as we
 * reasonably can.
 *
 * The set of all possible parameterizations is the power set of
 * candidateRelids. The power set has cardinality 2^N, where N is the
 * cardinality of candidateRelids. To avoid creating a huge number of paths,
 * limit the depth of the search; the depthLimit is equivalent to the maximum
 * number of required outer rels (beyond the minimal parameterization) for the
 * path. A depthLimit of zero means that only the minimally-parameterized path
 * will be generated.
 */
static void
AddColumnarScanPathsRec(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
						Relids paramRelids, Relids candidateRelids,
						int depthLimit)
{
	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	Assert(!bms_overlap(paramRelids, candidateRelids));

	Path *columnarScanPath = AddColumnarScanPath(root, rel, rte, paramRelids);
	add_path(rel, columnarScanPath);
	if (bioseq_enable_parallel_execution)
		columnarScanPath->total_cost += columnarScanPath->rows * 0.1;

	/* For columnar custom scan we should also do planning for parallel exeuction
	 * if it is enabled with GUC.
	 */
	if (bioseq_enable_parallel_execution)
	{
		int bioseq_min_parallel_process_running = bioseq_min_parallel_processes;

		if (bioseq_min_parallel_processes > max_parallel_workers)
		{
			pg_log(DEBUG1, "bioseq.min_parallel_proceses is set higher than max_parallel_workers.");
			pg_log(DEBUG1, "Using max_parallel_workers instead for parallel columnar scan.");
			bioseq_min_parallel_process_running = 
				std::min(max_parallel_workers, bioseq_min_parallel_processes);
		}

		if (parallel_leader_participation)
			bioseq_min_parallel_process_running--;

		/*
		 * We know how many workers are we going to run. Since workers are using single stripe
		 * probably it doesn't have much sense to spawn more workers than stripes in table.
		 */
		bioseq_min_parallel_process_running = std::min<int64_t>(bioseq_min_parallel_process_running,
													parallel_leader_participation ?
													ColumnarTableStripeCount(rte->relid) - 1 :
													ColumnarTableStripeCount(rte->relid));

		// FIXME: Fix this!!!
		const bool force_parallel = true;
		if (force_parallel) {
			JustLog("[bioseq] pg: AddColumnarScanPathsRec() force_parallel override active");
			bioseq_min_parallel_process_running = bioseq_min_parallel_processes;
		}

		if (force_parallel || (rel->consider_parallel && rel->lateral_relids == NULL && 
			bioseq_min_parallel_process_running > 0))
		{
			JustLog("[bioseq] pg: AddColumnarScanPathsRec() add_partial_path for parallel scan");
			/*
			 * Passing NULL for JOIN quals in parallel execution.
			 */
			Path *parallelColumnarScanPath = 
				AddColumnarScanPath(root, rel, rte, NULL);

			parallelColumnarScanPath->parallel_workers = bioseq_min_parallel_process_running;
			parallelColumnarScanPath->parallel_aware = true;
			AdjustColumnarParallelScanCost(parallelColumnarScanPath);

			add_partial_path(rel, parallelColumnarScanPath);
		}
	}

	JustLog("[bioseq] pg: AddColumnarScanPathsRec() recursing into candidate relids");

	/* recurse for all candidateRelids, unless we hit the depth limit */
	Assert(depthLimit >= 0);
	if (depthLimit-- == 0)
	{
		return;
	}

	/*
	 * Iterate through parameter combinations depth-first. Deeper levels
	 * generate paths of greater parameterization (and hopefully lower
	 * cost).
	 */
	JustLog("[bioseq] pg: AddColumnarScanPathsRec() iterating parameter combinations depth-first");
	Relids tmpCandidateRelids = bms_copy(candidateRelids);
	int relid = -1;
	while ((relid = bms_next_member(candidateRelids, relid)) >= 0)
	{
		Relids tmpParamRelids = bms_add_member(
			bms_copy(paramRelids), relid);

		/*
		 * Because we are generating combinations (not permutations), remove
		 * the relid from the set of candidates at this level as we descend to
		 * the next.
		 */
		tmpCandidateRelids = bms_del_member(tmpCandidateRelids, relid);

		AddColumnarScanPathsRec(root, rel, rte, tmpParamRelids,
								tmpCandidateRelids, depthLimit);
	}

	bms_free(tmpCandidateRelids);
}


/*
 * ParameterizationAsString returns the string representation of the set of
 * rels given in paramRelids.
 *
 * Takes a StringInfo so that it doesn't return palloc'd memory. This makes it
 * easy to call this function as an argument to ereport(), such that it won't
 * be evaluated unless the message is going to be output somewhere.
 */
static char *
ParameterizationAsString(PlannerInfo *root, Relids paramRelids, StringInfo buf)
{
	PBitmapset paramRelidsView{ paramRelids };

	if (paramRelidsView.empty())
	{
		return "unparameterized";
	}

	bool firstTime = true;
	appendStringInfoString(buf, "parameterized by rels {");
	for (int relid : paramRelidsView)
	{
		RangeTblEntry *rte = root->simple_rte_array[relid];
		const char *relname = quote_identifier(rte->eref->aliasname);

		appendStringInfo(buf, "%s%s", firstTime ? "" : ", ", relname);

		if (relname != rte->eref->aliasname)
		{
			pfree(const_cast<char *>(relname));
		}

		firstTime = false;
	}
	appendStringInfoString(buf, "}");
	return buf->data;
}


/*
 * ContainsExecParams tests whether the node contains any exec params. The
 * signature accepts an extra argument for use with expression_tree_walker.
 */
static bool
ContainsExecParams(Node *node, void *notUsed)
{
	if (node == NULL)
	{
		return false;
	}
	else if (IsA(node, Param))
	{
		Param *param = castNode(Param, node);
		if (param->paramkind == PARAM_EXEC)
		{
			return true;
		}
	}
	return expression_tree_walker(node, ContainsExecParams, NULL);
}

/*
 * fixup_inherited_columns
 *
 * Exact function Copied from PG16 as it's static.
 *
 * When user is querying on a table with children, it implicitly accesses
 * child tables also. So, we also need to check security label of child
 * tables and columns, but there is no guarantee attribute numbers are
 * same between the parent and children.
 * It returns a bitmapset which contains attribute number of the child
 * table based on the given bitmapset of the parent.
 */
static Bitmapset *
fixup_inherited_columns(Oid parentId, Oid childId, Bitmapset *columns)
{
	PBitmapset result;

	/*
	 * obviously, no need to do anything here
	 */
	if (parentId == childId)
	{
		return columns;
	}

	for (int index : PBitmapset(columns))
	{
		/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
		AttrNumber attno = index + FirstLowInvalidHeapAttributeNumber;

		/*
		 * whole-row-reference shall be fixed-up later
		 */
		if (attno == InvalidAttrNumber)
		{
			result.add(index);
			continue;
		}

		char *attname = get_attname(parentId, attno, false);
		attno = get_attnum(childId, attname);
		if (attno == InvalidAttrNumber)
		{
			pg_log(ERROR, "cache lookup failed for attribute {} of relation {}",
				   attname, childId);
		}

		result.add(attno - FirstLowInvalidHeapAttributeNumber);

		pfree(attname);
	}

	return result.release();
}

static bool
CostTestWalker(Node *node, Query *notUsed)
{
	JustLog("[bioseq] pg: CostTestWalker() entered");
	for (auto target_entry : PList<Node>(notUsed->targetList)) {
		JustLog("[bioseq] pg: CostTestWalker() targetlist node=[{}]", nodeStr(target_entry));
	}
	if (node == NULL)
	{
		return false;
	}
	JustLog("[bioseq] pg: CostTestWalker() walking node=[{}]", nodeStr(node));
	if (IsA(node, Var)) {
		Var* var_node = reinterpret_cast<Var*>(node);
		const auto var_no = var_node->varno;
		List* range_table = notUsed->rtable;
		if (range_table == nullptr) {
			JustLog("[bioseq] pg: CostTestWalker() var rtable is null");
		} else if (var_no == 0 || var_no > list_length(range_table)) {
			JustLog("[bioseq] pg: CostTestWalker() var varno out of bounds varno={} rtable_len={}", var_no, list_length(range_table));
		} else {
			RangeTblEntry* rte = rt_fetch(var_no, range_table);
			JustLog("[bioseq] pg: CostTestWalker() rte_kind={} relid={}", static_cast<int>(rte->rtekind), static_cast<int>(rte->relid));
		}
	}
	
	return expression_tree_walker(node, CostTestWalker, static_cast<void*>(notUsed));
}

/*
 * Create and add a path with the given parameterization paramRelids.
 *
 * XXX: Consider refactoring to be more like postgresGetForeignPaths(). The
 * only differences are param_info and custom_private.
 */
static Path *
AddColumnarScanPath(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
					Relids paramRelids)
{
	JustLog("[bioseq] pg: AddColumnarScanPath() entered");
	/*
	 * Must return a CustomPath, not a larger structure containing a
	 * CustomPath as the first field. Otherwise, nodeToString() will fail to
	 * output the additional fields.
	 */
	CustomPath *cpath = makeNode(CustomPath);

	cpath->methods = &ColumnarScanPathMethods;
	// PAPIESZ: Do we support projections ????
	//cpath->flags = CUSTOMPATH_SUPPORT_PROJECTION;

	/*
	 * populate generic path information
	 */
	Path *path = &cpath->path;
	path->pathtype = T_CustomScan;
	path->parent = rel;
	path->pathtarget = copy_pathtarget(rel->reltarget);

	const BioseqStubOids stub_oids = LookupStubOids();

	bool wehavemagic = false;
	int wehavemagic_cnt = 0;
	JustLog("[bioseq] pg: AddColumnarScanPath() mmsearch_oid={} bioseq_score_oid={}",
		stub_oids.mmsearch_oid, stub_oids.bioseq_score_oid);
	for (auto tgt : PList<TargetEntry>(root->parse->targetList)) {
		if (IsA(tgt->expr, FuncExpr)) {
			FuncExpr* fe = reinterpret_cast<FuncExpr *>(tgt->expr);
			if (fe->funcid == stub_oids.mmsearch_oid) {
				JustLog("[bioseq] pg: AddColumnarScanPath() detected mmsearch funcexpr in targetlist");
				wehavemagic = true;
				wehavemagic_cnt++;
			} else if (fe->funcid == stub_oids.bioseq_score_oid) {
				JustLog("[bioseq] pg: AddColumnarScanPath() detected bioseq.score funcexpr in targetlist");
				wehavemagic = true;
				wehavemagic_cnt++;
			}
		}
	}

	JustLog("[bioseq] pg: AddColumnarScanPath() walking pathtarget expressions");
	for (auto expr : PList<Node>(path->pathtarget->exprs)) {
		JustLog("[bioseq] pg: AddColumnarScanPath() pathtarget expression");
		CostTestWalker(expr, root->parse);
	}
	JustLog("[bioseq] pg: AddColumnarScanPath() pathtarget walk complete");

	/* columnar scans are not parallel-aware, but they are parallel-safe */
	path->parallel_safe = rel->consider_parallel;

	path->param_info = get_baserel_parampathinfo(root, rel, paramRelids);

	/*
	 * Usable clauses for this parameterization exist in baserestrictinfo and
	 * ppi_clauses.
	 */
	PList<RestrictInfo> allClauses = copyObject(rel->baserestrictinfo);
	if (path->param_info != NULL)
	{
		allClauses.concat(path->param_info->ppi_clauses);
	}

	JustLog("[bioseq] pg: AddColumnarScanPath() iterating allClauses (no-op pass for tracing)");
	for (auto restrict_info : allClauses)
	{
		(void) restrict_info;
	}

	allClauses = FilterPushdownClauses(root, rel, allClauses);

	/*
	 * Plain clauses may contain extern params, but not exec params, and can
	 * be evaluated at init time or rescan time. Track them in another list
	 * that is a subset of allClauses.
	 *
	 * Note: although typically baserestrictinfo contains plain clauses,
	 * that's not always true. It can also contain a qual referencing a Var at
	 * a higher query level, which can be turned into an exec param, and
	 * therefore it won't be a plain clause.
	 */
	PList<RestrictInfo> plainClauses;
	for (auto rinfo : allClauses)
	{
		if (bms_is_subset(rinfo->required_relids, rel->relids) &&
			!ContainsExecParams(reinterpret_cast<Node *>(rinfo->clause), nullptr))
		{
			plainClauses.append(rinfo);
		}
	}

	/*
	 * We can't make our own CustomPath structure, so we need to put
	 * everything in the custom_private list. To keep the two lists separate,
	 * we make them sublists in a 2-element list.
	 */
	// FIXME: Thios hould be changed
	// auto custom_path_data = CustomPathNodeData();
	if (EnableColumnarQualPushdown)
	{
		cpath->custom_private = list_make2(copyObject(plainClauses.ptr()),
										   copyObject(allClauses.ptr()));
	}
	else
	{
		cpath->custom_private = list_make2(NIL, NIL);
	}
	// cpath->custom_private = custom_path_data.ptr();

	int numberOfColumnsRead = 0;
	if (rte->perminfoindex > 0)
	{
		/*
		 * If perminfoindex > 0, that means that this relation's permission info
		 * is directly found in the list of rteperminfos of the Query(root->parse)
		 * So, all we have to do here is retrieve that info.
		 */
		RTEPermissionInfo *perminfo = getRTEPermissionInfo(root->parse->rteperminfos,
														   rte);
		numberOfColumnsRead = bms_num_members(perminfo->selectedCols);
	}
	else
	{
		/*
		 * If perminfoindex = 0, that means we are skipping the check for permission info
		 * for this relation, which means that it's either a partition or an inheritance child.
		 * In these cases, we need to access the permission info of the top parent of this relation.
		 * After thorough checking, we found that the index of the top parent pointing to the correct
		 * range table entry in Query's range tables (root->parse->rtable) is found under
		 * RelOptInfo rel->top_parent->relid.
		 * For reference, check expand_partitioned_rtentry and expand_inherited_rtentry PG functions
		 */
		Assert(rel->top_parent);
		RangeTblEntry *parent_rte = rt_fetch(rel->top_parent->relid, root->parse->rtable);
		RTEPermissionInfo *perminfo = getRTEPermissionInfo(root->parse->rteperminfos,
														   parent_rte);
		numberOfColumnsRead = bms_num_members(fixup_inherited_columns(perminfo->relid,
																	  rte->relid,
																	  perminfo->
																	  selectedCols));
	}
	int numberOfClausesPushed = allClauses.length();

	/* Queries that contain only aggregate with STAR doesn't have any
	 * selectedCols so we should consider at least one column that needs
	 * read (for better cost calculation).
	 */
	if (numberOfColumnsRead == 0)
		numberOfColumnsRead = 1;

	CostColumnarScan(root, rel, rte->relid, cpath, numberOfColumnsRead,
					 numberOfClausesPushed);

	if (wehavemagic) {
		//(&cpath->path)->total_cost -= 90000;
		PathTarget* pt = (&cpath->path)->pathtarget;
		//auto curcost = pt->cost.per_tuple;
		JustLog("[bioseq] pg: AddColumnarScanPath() reducing mmsearch cost path_total={} path_startup={} pertuple={} pt_startup={} cpath_startup={} cpath_total={}",
			path->total_cost, path->startup_cost, pt->cost.per_tuple, pt->cost.startup,
			(&cpath->path)->startup_cost, (&cpath->path)->total_cost);
		cpath->path.total_cost = -100000 * wehavemagic_cnt;
		cpath->path.startup_cost = -100000 * wehavemagic_cnt;
	}


	StringInfoData buf;
	initStringInfo(&buf);
	PgReport(ColumnarPlannerDebugLevel)
			.msg("columnar planner: adding CustomScan path for {}",
				 rte->eref->aliasname)
			.detail("{}; {} clauses pushed down",
					ParameterizationAsString(root, paramRelids, &buf),
					numberOfClausesPushed);

	return path;
}


/*
 * AdjustColumnarParallelScanCost calculates path cost based on
 * number of parallel workers (based on postgres code).
 */
static void
AdjustColumnarParallelScanCost(Path *path) 
{
	/* Adjust costing for parallelism, if used. */
	if (path->parallel_workers > 0)
	{
		double parallel_divisor = path->parallel_workers;

		if (parallel_leader_participation)
		{
			double leader_contribution;

			leader_contribution = 1.0 - (0.3 * path->parallel_workers);
			if (leader_contribution > 0)
				parallel_divisor += leader_contribution;
		}

		/* The CPU cost is divided among all the workers. */
		path->total_cost /= parallel_divisor;

		// FIXME: Remove
		//JustLog("PARA Set parallel cost to 0");
		//path->total_cost = 0;

		path->rows = clamp_row_est(path->rows / parallel_divisor);
	}
}


/*
 * CostColumnarScan calculates the cost of scanning the columnar table. The
 * cost is estimated by using all stripe metadata to estimate based on the
 * columns to read how many pages need to be read.
 */
static void
CostColumnarScan(PlannerInfo *root, RelOptInfo *rel, Oid relationId,
				 CustomPath *cpath, int numberOfColumnsRead, int nClauses)
{
	Path *path = &cpath->path;

	List *allClauses = PList<List>(cpath->custom_private).second();
	Selectivity clauseSel = clauselist_selectivity(
		root, allClauses, rel->relid, JOIN_INNER, NULL);

	/*
	 * We already filtered out clauses where the overall selectivity would be
	 * misleading, such as inequalities involving an uncorrelated column. So
	 * we can apply the selectivity directly to the number of stripes.
	 */
	double stripesToRead = clauseSel * ColumnarTableStripeCount(relationId);
	stripesToRead = std::max(stripesToRead, 1.0);

	path->rows = rel->tuples;
	path->startup_cost = 0;
	path->total_cost = stripesToRead *
					   ColumnarPerStripeScanCost(rel, relationId, numberOfColumnsRead);
}


/*
 * ColumnarPerStripeScanCost calculates the cost to scan a single stripe
 * of given columnar table based on number of columns that needs to be
 * read during scan operation.
 */
static Cost
ColumnarPerStripeScanCost(RelOptInfo *rel, Oid relationId, int numberOfColumnsRead)
{
	Relation relation = RelationIdGetRelation(relationId);
	List *stripeList = StripesForRelfilenode(RelationPhysicalIdentifier_compat(relation), ForwardScanDirection);
	RelationClose(relation);

	uint32_t maxColumnCount = 0;
	uint64_t totalStripeSize = 0;
	for(auto stripeMetadata : PList<StripeMetadata>(stripeList))
	{
		totalStripeSize += stripeMetadata->dataLength;
		maxColumnCount = std::max(maxColumnCount, stripeMetadata->columnCount);
	}

	/*
	 * When no stripes are in the table we don't have a count in maxColumnCount. To
	 * prevent a division by zero turning into a NaN we keep the ratio on zero.
	 * This will result in a cost of 0 for scanning the table which is a reasonable
	 * cost on an empty table.
	 */
	if (maxColumnCount == 0)
	{
		return 0;
	}

	double columnSelectionRatio = numberOfColumnsRead / static_cast<double>(maxColumnCount);
	Cost tableScanCost = static_cast<double>(totalStripeSize) / BLCKSZ * columnSelectionRatio;
	Cost perStripeScanCost = tableScanCost / list_length(stripeList);

	/*
	 * Finally, multiply the cost of reading a single stripe by seq page read
	 * cost to make our estimation scale compatible with postgres.
	 * Since we are calculating the cost for a single stripe here, we use seq
	 * page cost instead of random page cost. This is because, random page
	 * access only happens when switching between columns, which is pretty
	 * much neglactable.
	 */
	double relSpaceSeqPageCost;
	get_tablespace_page_costs(rel->reltablespace,
							  NULL, &relSpaceSeqPageCost);
	perStripeScanCost = perStripeScanCost * relSpaceSeqPageCost;

	return perStripeScanCost;
}


/*
 * ColumnarTableStripeCount returns the number of stripes that columnar
 * table with relationId has by using stripe metadata.
 */
static uint64_t
ColumnarTableStripeCount(Oid relationId)
{
	Relation relation = RelationIdGetRelation(relationId);
	List *stripeList = StripesForRelfilenode(RelationPhysicalIdentifier_compat(relation), ForwardScanDirection);
	int stripeCount = list_length(stripeList);
	RelationClose(relation);

	return stripeCount;
}


static Plan *
ColumnarScanPath_PlanCustomPath(PlannerInfo *root,
								RelOptInfo *rel,
								CustomPath *best_path,
								List *tlist,
								List *clauses,
								List *custom_plans)
{
	JustLog("[bioseq] pg: ColumnarScanPath_PlanCustomPath() entered");
	// PAPIESZ: ADD HERE
	/*
	 * Must return a CustomScan, not a larger structure containing a
	 * CustomScan as the first field. Otherwise, copyObject() will fail to
	 * copy the additional fields.
	 */
	CustomScan *cscan = makeNode(CustomScan);

	cscan->methods = &ColumnarScanScanMethods;

	/* XXX: also need to store projected column list for EXPLAIN */

	if (EnableColumnarQualPushdown)
	{
		/*
		 * Lists of pushed-down clauses. The Vars in custom_exprs referencing
		 * other relations will be changed into exec Params by
		 * create_customscan_plan().
		 *
		 * Like CustomPath->custom_private, keep a list of plain clauses
		 * separate from the list of all clauses by making them sublists of a
		 * 2-element list.
		 *
		 * XXX: custom_exprs are the quals that will be pushed into the
		 * columnar reader code; some of these may not be usable. We should
		 * fix this by processing the quals more completely and using
		 * ScanKeys.
		 */
		PList<List> private_lists(best_path->custom_private);
		List *plainClauses = extract_actual_clauses(
			private_lists.first(), false /* no pseudoconstants */);
		List *allClauses = extract_actual_clauses(
			private_lists.second(), false /* no pseudoconstants */);
		cscan->custom_exprs = copyObject(list_make2(plainClauses, allClauses));
	}
	else
	{
		cscan->custom_exprs = list_make2(NIL, NIL);
	}

	cscan->scan.plan.qual = extract_actual_clauses(
		clauses, false /* no pseudoconstants */);
	//cscan->scan.plan.targetlist = list_copy(tlist);

	// ?????? AYY LMAO
	//ColumnarScanPath_PlanCustomPath tlist member => [{TARGETENTRY :expr {FUNCEXPR :funcid 535007 :funcresulttype 16 :funcretset false :funcvariadic false :funcformat 0 :funccollid 0 :inputcollid 100 :args ({VAR :varno 1 :varattno 1 :vartype 23 :vartypmod -1 :varcollid 0 :varnullingrels (b) :varlevelsup 0 :varnosyn 1 :varattnosyn 1 :location 24} {CONST :consttype 25 :consttypmod -1 :constcollid 100 :constlen -1 :constbyval false :constisnull false :location 27 :constvalue 5 [ 20 0 0 0 65 ]}) :location 15} :resno 3 :resname mmsearch :ressortgroupref 0 :resorigtbl 0 :resorigcol 0 :resjunk false}]

	for(auto n : PList<TargetEntry>(root->processed_tlist)) {
		JustLog("[bioseq] pg: ColumnarScanPath_PlanCustomPath() processed_tlist node=[{}]", nodeStr(n));
		if(IsA(n->expr, FuncExpr)) {
			tlist = lappend(tlist, flatCopyTargetEntry(n));
		}
	}
	JustLog("[bioseq] pg: ColumnarScanPath_PlanCustomPath() tlist_is_null={}", tlist == nullptr);
	for(auto n : PList<TargetEntry>(tlist)) {
		JustLog("[bioseq] pg: ColumnarScanPath_PlanCustomPath() tlist node=[{}]", nodeStr(n));
	}



	cscan->scan.plan.targetlist = list_copy(tlist);
	cscan->scan.scanrelid = best_path->path.parent->relid;

	/*
	 * Vectorized QUAL execution. Split QUAL list into vectorized list 
	 * stored in third position of custom_exprs list and rest of
	 * qual list (which can't be vectorized) in scan.plan.qual.
	 */
	if (bioseq_enable_vectorization)
	{
		List *candidateQualList = CreateVectorizedExprList(cscan->scan.plan.qual);
		List *listDifference = list_difference(candidateQualList, cscan->scan.plan.qual);

		JustLog("[bioseq] pg: ColumnarScanPath_PlanCustomPath() splitting qual list for vectorization");

		cscan->custom_exprs = lappend(cscan->custom_exprs, listDifference);

		if (listDifference != NULL)
			cscan->scan.plan.qual = list_intersection(cscan->scan.plan.qual,
													  candidateQualList);
	}
	else
	{
		JustLog("[bioseq] pg: ColumnarScanPath_PlanCustomPath() vectorization disabled, qual not split");
		/* We still need to create third entry in custom_exprs */
		cscan->custom_exprs = lappend(cscan->custom_exprs, NIL);
	}

	JustLog("[bioseq] pg: ColumnarScanPath_PlanCustomPath() returning");
	return reinterpret_cast<Plan *>(cscan);
}


/*
 * ReparameterizeMutator changes all varnos referencing the topmost parent of
 * child_rel to instead reference child_rel directly.
 */
static Node *
ReparameterizeMutator(Node *node, RelOptInfo *child_rel)
{
	if (node == NULL)
	{
		return NULL;
	}
	if (IsA(node, Var))
	{
		Var *var = castNode(Var, node);
		if (bms_is_member(var->varno, child_rel->top_parent_relids))
		{
			var = copyObject(var);
			var->varno = child_rel->relid;
		}
		return reinterpret_cast<Node *>(var);
	}

	if (IsA(node, RestrictInfo))
	{
		RestrictInfo *rinfo = castNode(RestrictInfo, node);
		rinfo = copyObject(rinfo);
		rinfo->clause = reinterpret_cast<Expr *>(expression_tree_mutator(
			reinterpret_cast<Node *>(rinfo->clause), ReparameterizeMutator, static_cast<void *>(child_rel)));
		return reinterpret_cast<Node *>(rinfo);
	}
	return expression_tree_mutator(node, ReparameterizeMutator,
								   static_cast<void *>(child_rel));
}


/*
 * ColumnarScanPath_ReparameterizeCustomPathByChild is a method called when a
 * path is reparameterized directly to a child relation, rather than the
 * top-level parent.
 *
 * For instance, let there be a join of two partitioned columnar relations PX
 * and PY. A path for a ColumnarScan of PY3 might be parameterized by PX so
 * that the join qual "PY3.a = PX.a" (referencing the parent PX) can be pushed
 * down. But if the planner decides on a partition-wise join, then the path
 * will be reparameterized on the child table PX3 directly.
 *
 * When that happens, we need to update all Vars in the pushed-down quals to
 * reference PX3, not PX, to match the new parameterization. This method
 * notifies us that it needs to be done, and allows us to update the
 * information in custom_private.
 */
static List *
ColumnarScanPath_ReparameterizeCustomPathByChild(PlannerInfo *root,
												 List *custom_private,
												 RelOptInfo *child_rel)
{
	return reinterpret_cast<List *>(ReparameterizeMutator(reinterpret_cast<Node *>(custom_private), child_rel));
}


static Node *
ColumnarScan_CreateCustomScanState(CustomScan *cscan)
{
	const_score_node = nullptr;
	JustLog("[bioseq] pg: ColumnarScan_CreateCustomScanState() entered");
	BioseqScanState* bioseq_scan_state = BioseqScanState::allocate();
	bioseq_scan_state->as_custom_scan_state()->methods = &ColumnarScanExecuteMethods;
	JustLog("[bioseq] pg: ColumnarScan_CreateCustomScanState() returning");
	return bioseq_scan_state->as_node();
}


/*
 * EvalParamsMutator evaluates Params in the expression and replaces them with
 * Consts.
 */
static Node *
EvalParamsMutator(Node *node, ExprContext *econtext)
{
	if (node == NULL)
	{
		return NULL;
	}

	if (IsA(node, Param))
	{
		Param *param = reinterpret_cast<Param *>(node);
		int16_t typLen;
		bool typByVal;
		bool isnull;

		get_typlenbyval(param->paramtype, &typLen, &typByVal);

		/* XXX: should save ExprState for efficiency */
		ExprState *exprState = ExecInitExprWithParams(reinterpret_cast<Expr *>(node),
													  econtext->ecxt_param_list_info);
		Datum pval = ExecEvalExpr(exprState, econtext, &isnull);

		return reinterpret_cast<Node *>(makeConst(param->paramtype,
								  param->paramtypmod,
								  param->paramcollid,
								  static_cast<int>(typLen),
								  pval,
								  isnull,
								  typByVal));
	}

	return expression_tree_mutator(node, EvalParamsMutator, static_cast<void *>(econtext));
}


static void
ColumnarScan_BeginCustomScan(CustomScanState *cscanstate, EState *estate, int eflags)
{
	JustLog("[bioseq] pg: ColumnarScan_BeginCustomScan() entered");
	CustomScan *cscan = reinterpret_cast<CustomScan *>(cscanstate->ss.ps.plan);
	BioseqScanState* columnarScanState = BioseqScanState::from_custom_scan_state(cscanstate);
	ExprContext *stdecontext = cscanstate->ss.ps.ps_ExprContext;

	/*
	 * Make a new ExprContext just like the existing one, except that we don't
	 * reset it every tuple.
	 */
	ExecAssignExprContext(estate, &cscanstate->ss.ps);
	columnarScanState->css_RuntimeContext = cscanstate->ss.ps.ps_ExprContext;
	cscanstate->ss.ps.ps_ExprContext = stdecontext;

	ResetExprContext(columnarScanState->css_RuntimeContext);
	PList<List> custom_exprs(cscan->custom_exprs);
	List *plainClauses = custom_exprs.first();
	columnarScanState->qual = reinterpret_cast<List *>(EvalParamsMutator(
		reinterpret_cast<Node *>(plainClauses), columnarScanState->css_RuntimeContext));

	/*
	 * Third list in custom_exprs should contain vectorized qual list if not 
	 * NULL.
	 */
	JustLog("[bioseq] pg: ColumnarScan_BeginCustomScan() examining custom_exprs for vectorized qual");
	List* vectorized_qual_list = custom_exprs.third();
	if (vectorized_qual_list != NIL) {
		JustLog("[bioseq] pg: ColumnarScan_BeginCustomScan() vectorized qual list present");
		for (auto qual_node : PList<Node>(cscan->custom_exprs)) {
			JustLog("[bioseq] pg: ColumnarScan_BeginCustomScan() vectorized qual=[{}]", nodeStr(qual_node));
		}
		columnarScanState->vectorization.vectorizedQualList = vectorized_qual_list;
	}

	JustLog("[bioseq] pg: ColumnarScan_BeginCustomScan() iterating custom_private");
	for (auto private_const_node : PList<Const>(cscan->custom_private))
	{
		JustLog("[bioseq] pg: ColumnarScan_BeginCustomScan() custom_private node=[{}]", nodeStr(reinterpret_cast<Node*>(private_const_node)));
		if (private_const_node->consttype == CUSTOM_SCAN_VECTORIZED_AGGREGATE)
		{
			JustLog("[bioseq] pg: ColumnarScan_BeginCustomScan() setting vectorizationAggregate");
			columnarScanState->vectorization.vectorizationAggregate = private_const_node->constvalue;
		}
	}
	JustLog("[bioseq] pg: ColumnarScan_BeginCustomScan() custom_private iteration complete");

	/*
	 * Vectorization is enabled if global variable is set and there is at least one
	 * filter which can be vectorized.
	 */
	columnarScanState->vectorization.vectorizationEnabled =
		bioseq_enable_vectorization &&
		(columnarScanState->vectorization.vectorizedQualList != NULL ||
		 columnarScanState->vectorization.vectorizationAggregate);

	JustLog("[bioseq] pg: ColumnarScan_BeginCustomScan() guc_vec={} has_vec_qual={} vec_aggregate={}",
		bioseq_enable_vectorization,
		columnarScanState->vectorization.vectorizedQualList != NULL,
		columnarScanState->vectorization.vectorizationAggregate);
	if (columnarScanState->vectorization.vectorizationAggregate)
	{
		ScanState *node = reinterpret_cast<ScanState *>(&columnarScanState->custom_scanstate.ss);

		if (node->ps.ps_ProjInfo)
		{
			columnarScanState->vectorization.resultVectorSlot =
				CreateVectorTupleTableSlot(node->ps.ps_ProjInfo->pi_state.resultslot->tts_tupleDescriptor);
		}
		else
		{
			columnarScanState->vectorization.resultVectorSlot =
				CreateVectorTupleTableSlot(node->ps.ps_ResultTupleDesc);
		}
	}

	if (columnarScanState->vectorization.vectorizationEnabled)
	{
		columnarScanState->vectorization.scanVectorSlot =
			CreateVectorTupleTableSlot(cscanstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	}

	columnarScanState->attrNeeded = 
		ColumnarAttrNeeded(&cscanstate->ss, columnarScanState->vectorization.vectorizedQualList);

	JustLog("[bioseq] pg: ColumnarScan_BeginCustomScan() iterating attrNeeded bitmap members");
	for (int attrIndex : columnarScanState->attrNeeded)
	{
		columnarScanState->vectorization.attrNeededList.append(attrIndex);
	}

	/*
	 * If we have pending changes that need to be flushed (row_mask after update/delete)
	 * or new stripe we need to to them here because sequential columnar scan 
	 * can have parallel execution and updated are not allowed in parallel mode.
	 */
	columnarScanState->snapshot = estate->es_snapshot;
	columnarScanState->snapshotRegisteredByUs = false;
	Oid relationOid = cscanstate->ss.ss_currentRelation->rd_locator.relNumber;

	JustLog("[bioseq] pg: ColumnarScan_BeginCustomScan() checking parallel mode");
	if(!IsInParallelMode())
	{
		RowMaskFlushWriteStateForRelfilenode(relationOid, GetCurrentSubTransactionId());

		FlushWriteStateWithNewSnapshot(relationOid, &columnarScanState->snapshot,
									   &columnarScanState->snapshotRegisteredByUs);
	}

	/*
	 * Walk cscan->scan.plan.qual to discover the WHERE mmsearch(...) call,
	 * pull out (varattno of the TEXT column, query literal) and configure
	 * the dispatch buffer so on_submit() has enough context to emit the
	 * per-tuple log line.
	 */
	buffer.clear_mmsearch_context();

	const BioseqStubOids scan_stub_oids = LookupStubOids();
	Relation relation_for_attname = cscanstate->ss.ss_currentRelation;
	const Oid relation_oid_for_attname = RelationGetRelid(relation_for_attname);

	for (auto qual_node : PList<Node>(cscan->scan.plan.qual))
	{
		if (qual_node == nullptr || !IsA(qual_node, FuncExpr))
		{
			continue;
		}
		FuncExpr* fe = reinterpret_cast<FuncExpr*>(qual_node);
		if (fe->funcid != scan_stub_oids.mmsearch_oid)
		{
			continue;
		}
		if (list_length(fe->args) != 2)
		{
			JustLog("[bioseq] pg: BeginCustomScan() mmsearch FuncExpr has unexpected arg count={}",
					list_length(fe->args));
			continue;
		}

		PList<Node> fe_args(fe->args);
		Node* arg0 = fe_args.first();
		Node* arg1 = fe_args.second();
		if (arg0 == nullptr || arg1 == nullptr)
		{
			continue;
		}
		if (!IsA(arg0, Var))
		{
			JustLog("[bioseq] pg: BeginCustomScan() mmsearch arg0 is not a Var (tag={})",
					static_cast<int>(nodeTag(arg0)));
			continue;
		}
		if (!IsA(arg1, Const))
		{
			JustLog("[bioseq] pg: BeginCustomScan() mmsearch arg1 is not a Const (tag={})",
					static_cast<int>(nodeTag(arg1)));
			continue;
		}

		Var* col_var = reinterpret_cast<Var*>(arg0);
		Const* query_const = reinterpret_cast<Const*>(arg1);

		if (query_const->consttype != TEXTOID || query_const->constisnull)
		{
			JustLog("[bioseq] pg: BeginCustomScan() mmsearch arg1 is not a non-null TEXT Const");
			continue;
		}

		TupleDesc tuple_descriptor = RelationGetDescr(relation_for_attname);
		if (col_var->varattno <= 0 || col_var->varattno > tuple_descriptor->natts)
		{
			JustLog("[bioseq] pg: BeginCustomScan() mmsearch arg0 varattno={} out of range natts={}",
					static_cast<int>(col_var->varattno), tuple_descriptor->natts);
			continue;
		}
		Form_pg_attribute attribute_form = TupleDescAttr(tuple_descriptor, col_var->varattno - 1);
		if (attribute_form->atttypid != TEXTOID)
		{
			JustLog("[bioseq] pg: BeginCustomScan() mmsearch arg0 column type is {}, expected TEXTOID",
					attribute_form->atttypid);
			continue;
		}

		char* attname_cstr = get_attname(relation_oid_for_attname, col_var->varattno, /*missing_ok=*/true);
		if (attname_cstr == nullptr)
		{
			JustLog("[bioseq] pg: BeginCustomScan() get_attname returned NULL for attno={}",
					static_cast<int>(col_var->varattno));
			continue;
		}
		char* query_cstr = TextDatumGetCString(query_const->constvalue);

		buffer.configure_mmsearch(col_var->varattno,
								  std::string(attname_cstr),
								  std::string(query_cstr));

		pfree(attname_cstr);
		pfree(query_cstr);
		break; /* POC: only honour the first mmsearch() qual we find */
	}

	/* Reset the counter so each scan starts from 100 deterministically. */
	counter = 100;

	JustLog("[bioseq] pg: ColumnarScan_BeginCustomScan() scan initialised successfully");
	/* scan slot is already initialized */
}

/*
 * ExecScanFetch -- check interrupts & fetch next potential tuple
 *
 * This routine is concerned with substituting a test tuple if we are
 * inside an EvalPlanQual recheck.  If we aren't, just execute
 * the access method's next-tuple routine.
 */
static inline TupleTableSlot *
ExecScanFetch(ScanState *node,
			  ExecScanAccessMtd accessMtd,
			  ExecScanRecheckMtd recheckMtd)
{
	JustLog("[bioseq] pg: ExecScanFetch() entered");
	EState	   *estate = node->ps.state;

	CHECK_FOR_INTERRUPTS();

	if (estate->es_epq_active != NULL)
	{
		EPQState   *epqstate = estate->es_epq_active;

		/*
		 * We are inside an EvalPlanQual recheck.  Return the test tuple if
		 * one is available, after rechecking any access-method-specific
		 * conditions.
		 */
		Index		scanrelid = reinterpret_cast<Scan *>(node->ps.plan)->scanrelid;

		if (scanrelid == 0)
		{
			/*
			 * This is a ForeignScan or CustomScan which has pushed down a
			 * join to the remote side.  The recheck method is responsible not
			 * only for rechecking the scan/join quals but also for storing
			 * the correct tuple in the slot.
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			if (!(*recheckMtd) (node, slot))
				ExecClearTuple(slot);	/* would not be returned by scan */
			return slot;
		}
		else if (epqstate->relsubs_done[scanrelid - 1])
		{
			/*
			 * Return empty slot, as we already performed an EPQ substitution
			 * for this relation.
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			/* Return empty slot, as we already returned a tuple */
			return ExecClearTuple(slot);
		}
		else if (epqstate->relsubs_slot[scanrelid - 1] != NULL)
		{
			/*
			 * Return replacement tuple provided by the EPQ caller.
			 */

			TupleTableSlot *slot = epqstate->relsubs_slot[scanrelid - 1];

			Assert(epqstate->relsubs_rowmark[scanrelid - 1] == NULL);

			/* Mark to remember that we shouldn't return more */
			epqstate->relsubs_done[scanrelid - 1] = true;

			/* Return empty slot if we haven't got a test tuple */
			if (TupIsNull(slot))
				return NULL;

			/* Check if it meets the access-method conditions */
			if (!(*recheckMtd) (node, slot))
				return ExecClearTuple(slot);	/* would not be returned by
												 * scan */
			return slot;
		}
		else if (epqstate->relsubs_rowmark[scanrelid - 1] != NULL)
		{
			/*
			 * Fetch and return replacement tuple using a non-locking rowmark.
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			/* Mark to remember that we shouldn't return more */
			epqstate->relsubs_done[scanrelid - 1] = true;

			if (!EvalPlanQualFetchRowMark(epqstate, scanrelid, slot))
				return NULL;

			/* Return empty slot if we haven't got a test tuple */
			if (TupIsNull(slot))
				return NULL;

			/* Check if it meets the access-method conditions */
			if (!(*recheckMtd) (node, slot))
				return ExecClearTuple(slot);	/* would not be returned by
												 * scan */
			return slot;
		}
	}

	/*
	 * Run the node-type-specific access method function to get the next tuple
	 */
	return (*accessMtd) (node);
}

static Node *
VarInjectorMutator(Node *node, void* context)
{
	if (node == NULL)
	{
		return NULL;
	}
	auto* injector_context = static_cast<VarInjectorCtx*>(context);
	if (IsA(node, FuncExpr)) {
		auto funcExpr = reinterpret_cast<FuncExpr *>(node);
		if (funcExpr->funcid == injector_context->bioseq_score_oid) {
			JustLog("[bioseq] pg: VarInjectorMutator() replacing bioseq.score FuncExpr with Const");
			return reinterpret_cast<Node *>(injector_context->const_score_node);
		}
		/* Fall through to recurse into arg subtrees of unrelated FuncExprs. */
	}
	return expression_tree_mutator(node, VarInjectorMutator, context);
}


static TupleTableSlot *
Inner_CustomExecScan(BioseqScanState *columnarScanState,
			   ExecScanAccessMtd accessMtd,
			   ExecScanRecheckMtd recheckMtd)
{
	bool rec = false;
	JustLog("[bioseq] pg: Inner_CustomExecScan() entered");
	ExprContext *econtext;
	ExprState  *qual;
	ProjectionInfo *projInfo;

	ScanState *node = reinterpret_cast<ScanState *>(&columnarScanState->custom_scanstate.ss);

	/*
	 * Fetch data from node
	 */
	qual = node->ps.qual;
	projInfo = node->ps.ps_ProjInfo;
	econtext = node->ps.ps_ExprContext;

	JustLog("[bioseq] pg: Inner_CustomExecScan() projection_info_is_null={}", projInfo == nullptr);

	/* interrupt checks are in ExecScanFetch */

	/*
	 * No qual to check and no projection to do and vectorization is not enabled,
	 * just skip all the overhead and return the raw scan tuple.
	 */
	// FIXME: Temporary disable this check
	if (false && !qual && !projInfo && !columnarScanState->vectorization.vectorizationEnabled)
	{
		if(rec) ResetExprContext(econtext);
		return ExecScanFetch(node, accessMtd, recheckMtd);
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	if(rec) ResetExprContext(econtext);
	
	uint resultVectorTupleSlotIdx = 0;

	if (columnarScanState->vectorization.vectorizationAggregate)
	{
		ExecClearTuple(columnarScanState->vectorization.resultVectorSlot);
		CleanupVectorSlot(reinterpret_cast<VectorTupleTableSlot *>(
							columnarScanState->vectorization.resultVectorSlot));
	}

	/*
	 * get a tuple from the access method.  Loop until we obtain a tuple that
	 * passes the qualification.
	 */
	for (;;)
	{

		TupleTableSlot *slot = NULL;

		if (buffer.has_finished_writing()) {
			JustLog("[bioseq] pg: Inner_CustomExecScan() draining buffered slot");
			return buffer.read_next();
		}

		if (columnarScanState->vectorization.vectorizationEnabled && 
			columnarScanState->vectorization.vectorPendingRowNumber == 0 &&
			resultVectorTupleSlotIdx > 0)
		{
			JustLog("[bioseq] pg: Inner_CustomExecScan() vectorized fast-exit returning result vector slot");
			reinterpret_cast<VectorTupleTableSlot *>(columnarScanState->vectorization.resultVectorSlot)->dimension = resultVectorTupleSlotIdx;
			ExecStoreVirtualTuple(columnarScanState->vectorization.resultVectorSlot);
			resultVectorTupleSlotIdx = 0;
			return columnarScanState->vectorization.resultVectorSlot;
		}

        // Fake proj here
		const_score_node->constvalue = counter++;
		const_score_node->constisnull = false;
		JustLog("[bioseq] pg: Inner_CustomExecScan() advance const_score counter to {}", counter);

		auto newProj = ExecBuildProjectionInfo(
			newtargetlist,
			econtext,
			node->ps.ps_ResultTupleSlot,
			&(node->ps),
			node->ss_ScanTupleSlot->tts_tupleDescriptor
		);
		JustLog("[bioseq] pg: Inner_CustomExecScan() existing_projInfo_null={} replacing with built projection", projInfo == nullptr);
		projInfo = newProj;

		//JustLog("NEW PROJECTION [{}]", nodeStr(newProj));

		/*
		 * No vectorizaion in place so fetch slot by slot.
		 */
		if (!columnarScanState->vectorization.vectorizationEnabled)
		{

			JustLog("[bioseq] pg: Inner_CustomExecScan() scalar fetch via ExecScanFetch");
			slot = ExecScanFetch(node, accessMtd, recheckMtd);

			resultVectorTupleSlotIdx = 0;

			/*
			 * if the slot returned by the accessMtd contains NULL, then it means
			 * there is nothing more to scan so we just return an empty slot,
			 * being careful to use the projection result slot so it has correct
			 * tupleDesc.
			 */
			if (TupIsNull(slot))
			{
				JustLog("[bioseq] pg: Inner_CustomExecScan() source slot null, finishing dispatch buffer");
				return buffer.finish_writing();
				// if (projInfo)
				// 	return ExecClearTuple(projInfo->pi_state.resultslot);
				// else
				// 	return slot;
			}
		}
		/*
		 * Get next tuple from vector tuple slot if vectorization exeuction is enabled
		 * and if we exhausted previous vector.
		 */
		else if (columnarScanState->vectorization.vectorizationEnabled &&
				 columnarScanState->vectorization.vectorPendingRowNumber == 0)
		{
			JustLog("[bioseq] pg: Inner_CustomExecScan() vectorized fetch (initial vector pull)");
			slot = ExecScanFetch(node, accessMtd, recheckMtd);

			/*
			 * if the slot returned by the accessMtd contains NULL, then it means
			 * there is nothing more to scan so we just return an empty slot,
			 * being careful to use the projection result slot so it has correct
			 * tupleDesc.
			 */
			if (TupIsNull(slot))
			{
				// ????
				JustLog("[bioseq] pg: Inner_CustomExecScan() vectorized source exhausted projInfo_present={} finalizing buffer",
					projInfo != nullptr);
				return buffer.finish_writing();
			}

			if (columnarScanState->vectorization.vectorizedQualList != NULL)
			{

				if (columnarScanState->vectorization.constructedVectorizedQualList == NULL)
				{
					columnarScanState->vectorization.constructedVectorizedQualList =
						ConstructVectorizedQualList(slot, columnarScanState->vectorization.vectorizedQualList);
				}

				VectorTupleTableSlot *vectorSlot = reinterpret_cast<VectorTupleTableSlot *>(slot);

				bool *resultQual = 
					ExecuteVectorizedQual(slot,
										columnarScanState->vectorization.constructedVectorizedQualList,
										AND_EXPR, econtext);

				memcpy(vectorSlot->keep, resultQual, COLUMNAR_VECTOR_COLUMN_SIZE);
			}
			/*
			 * No qual, no vectorized qual, no projection but we need to return vector
			 * for vectorized aggregate
			 */
			else if (!qual && !projInfo && columnarScanState->vectorization.vectorizationAggregate)
			{
				columnarScanState->vectorization.vectorPendingRowNumber = 0;
				JustLog("[bioseq] pg: Inner_CustomExecScan() submit vectorized aggregate slot to dispatch buffer");
				buffer.submit(slot);
				continue;
				//return slot;
			}
		}

		uint64_t rowNumber = 0;

		/*
		 * Get next tuple from vector tuple slot if vectorization execution 
		 * is enabled.
		 */
		if (columnarScanState->vectorization.vectorizationEnabled)
		{
			JustLog("[bioseq] pg: Inner_CustomExecScan() vectorized fetch (post-qual extraction)");
			VectorTupleTableSlot *vectorSlot =
				reinterpret_cast<VectorTupleTableSlot *>(columnarScanState->vectorization.scanVectorSlot);

			bool rowFound = false;

			while (columnarScanState->vectorization.vectorPendingRowNumber != 0 &&
				   rowFound == false)
			{
				if (vectorSlot->keep[columnarScanState->vectorization.vectorRowIndex])
				{
					slot = columnarScanState->custom_scanstate.ss.ss_ScanTupleSlot;
					ExecClearTuple(slot);
					ExtractTupleFromVectorSlot(slot,
											   vectorSlot,
											   columnarScanState->vectorization.vectorRowIndex,
											   columnarScanState->vectorization.attrNeededList);
					
					rowNumber = vectorSlot->rowNumber[columnarScanState->vectorization.vectorRowIndex];
					if (!columnarScanState->vectorization.vectorizationAggregate)
						slot->tts_tid = row_number_to_tid(rowNumber);
					rowFound = true;
				}
				columnarScanState->vectorization.vectorPendingRowNumber--;
				columnarScanState->vectorization.vectorRowIndex++;
			}

			if (rowFound == false)
				continue;
		}

		/*
		 * place the current tuple into the expr context
		 */
		econtext->ecxt_scantuple = slot;

		/*
		 * check that the current tuple satisfies the qual-clause
		 *
		 * check for non-null qual here to avoid a function call to ExecQual()
		 * when the qual is null ... saves only a few cycles, but they add up
		 * ... 
		 */
		JustLog("[bioseq] pg: Inner_CustomExecScan() applying qual filter");
		if (qual == NULL || ExecQual(qual, econtext))
		{
			JustLog("[bioseq] pg: Inner_CustomExecScan() qual passed");

			/*
			 * Fire the TupleDispatchBuffer on_submit hook while the scan
			 * slot still contains every attribute (notably `seq` which
			 * is required for the mmsearch read but is dropped by the
			 * projection). Must come BEFORE ExecProject since ExecProject
			 * reassigns `slot` to ps_ResultTupleSlot.
			 */
			if (!columnarScanState->vectorization.vectorizationEnabled)
			{
				buffer.on_submit(econtext->ecxt_scantuple);
			}

			if (projInfo)
			{
				/*
				 * Form a projection tuple, store it in the result tuple slot
				 * and return it.
				 */
				slot = ExecProject(projInfo);
			}

			if (columnarScanState->vectorization.vectorizationAggregate)
			{
				WriteTupleToVectorSlot(slot,
									  reinterpret_cast<VectorTupleTableSlot *>(columnarScanState->vectorization.resultVectorSlot),
									  resultVectorTupleSlotIdx);
				reinterpret_cast<VectorTupleTableSlot *>(
					columnarScanState->vectorization.resultVectorSlot)->rowNumber[resultVectorTupleSlotIdx] = rowNumber;
				resultVectorTupleSlotIdx++;
				continue;
			}
			else
			{
				JustLog("[bioseq] pg: Inner_CustomExecScan() submit projected slot to dispatch buffer");
				buffer.submit(slot);
			}
		}
		else
			InstrCountFiltered1(node, 1);

		/*
		 * Tuple fails qual, so free per-tuple memory and try again.
		 */
		//if(rec) ResetExprContext(econtext);
	}
}


static TupleTableSlot *
CustomExecScan(BioseqScanState *columnarScanState,
			   ExecScanAccessMtd accessMtd,
			   ExecScanRecheckMtd recheckMtd) {

	// Testing custom exec scan
	JustLog("[bioseq] pg: CustomExecScan() entered");

	ScanState *node = reinterpret_cast<ScanState *>(&columnarScanState->custom_scanstate.ss);
	PlanState ps = node->ps;
	auto context = ps.ps_ExprContext;
	
	JustLog("[bioseq] pg: CustomExecScan() reading plan targetlist");
	auto targetlist = ps.plan->targetlist;
	JustLog("[bioseq] pg: CustomExecScan() targetlist_is_null={}", targetlist == nullptr);
	for (auto tgt : PList<Node>(targetlist)) {
		if (tgt == nullptr) {
			JustLog("[bioseq] pg: CustomExecScan() targetlist contained null node");
		} else {
			JustLog("[bioseq] pg: CustomExecScan() targetlist node=[{}]", nodeStr(tgt));
		}
	}
	JustLog("[bioseq] pg: CustomExecScan() targetlist iteration complete");

	if (const_score_node == nullptr) {
		JustLog("[bioseq] pg: CustomExecScan() initialising const_score node");
		const_score_node = makeConst(
			INT4OID,
			-1,
			InvalidOid,
			4,
			static_cast<Datum>(static_cast<int32_t>(0)),
			/*constisnull=*/ true,
			/*constbyval=*/ true
		);
		/*
		 * Reset only the slot buffer; keep the mmsearch_context that
		 * BeginCustomScan configured earlier for this scan.
		 */
		buffer.clear_slots();

		VarInjectorCtx injector_ctx;
		injector_ctx.const_score_node = const_score_node;
		injector_ctx.bioseq_score_oid = LookupStubOids().bioseq_score_oid;

		newtargetlist = reinterpret_cast<List *>(VarInjectorMutator(
			reinterpret_cast<Node *>(targetlist),
			static_cast<void*>(&injector_ctx)));
	}


	JustLog("[bioseq] pg: CustomExecScan() dispatching to Inner_CustomExecScan");

	auto tupleSlot = Inner_CustomExecScan(columnarScanState, accessMtd, recheckMtd);



	//MemoryContextReset(econtext->ecxt_per_tuple_memory);

	// (*(*state.projection_info()).pi_exprContext).ecxt_per_tuple_memory,
	//ExecProject(newProj);


	return tupleSlot;
}


static TupleTableSlot *
ColumnarScanNext(BioseqScanState *columnarScanState)
{
	JustLog("[bioseq] pg: ColumnarScanNext() entered");
	CustomScanState *node = columnarScanState->as_custom_scan_state();

	/*
	 * get information from the estate and scan state
	 */
	TableScanDesc scandesc = node->ss.ss_currentScanDesc;
	EState *estate = node->ss.ps.state;
	ScanDirection direction = estate->es_direction;
	bool vectorizationEnabled = columnarScanState->vectorization.vectorizationEnabled;

	TupleTableSlot *slot = nullptr;

	if (vectorizationEnabled)
		slot = columnarScanState->vectorization.scanVectorSlot;
	else
		slot = node->ss.ss_ScanTupleSlot;

	if (scandesc == NULL)
	{
		/* the columnar access method does not use the flags, they are specific to heap */
		uint32_t flags = 0;

		/*
		 * We reach here if the scan is not parallel, or if we're serially
		 * executing a scan that was planned to be parallel.
		 */
		scandesc = bioseq_beginscan_extended(node->ss.ss_currentRelation,
											   columnarScanState->snapshot,
											   0, NULL, NULL, flags,
											   columnarScanState->attrNeeded,
											   columnarScanState->qual,
											   columnarScanState->parallelColumnarScan,
											   vectorizationEnabled);

		node->ss.ss_currentScanDesc = scandesc;
	}

	/* 
	* Cleanup vector tuple table slot when reading next chunk
	 */
	if (vectorizationEnabled)
	{
		VectorTupleTableSlot *vectorSlot = reinterpret_cast<VectorTupleTableSlot *>(slot);
		for (int attrIndex : PIntList(columnarScanState->vectorization.attrNeededList))
		{
			VectorColumn *column = reinterpret_cast<VectorColumn *>(vectorSlot->tts.tts_values[attrIndex]);
			memset(column->isnull, true, COLUMNAR_VECTOR_COLUMN_SIZE);
			column->dimension = 0;
		}
		vectorSlot->dimension = 0;
	}

	if (table_scan_getnextslot(scandesc, direction, slot))
	{
		/*
		* Setup custom scan state for reading tuples directly from stored
		* vector tuple table slot
		*/
		if (vectorizationEnabled)
		{
			columnarScanState->vectorization.vectorPendingRowNumber =
				reinterpret_cast<VectorTupleTableSlot *>(slot)->dimension;
			columnarScanState->vectorization.vectorRowIndex = 0;
		}
		return slot;
	}

	return NULL;
}


/*
 * SeqRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
ColumnarScanRecheck(BioseqScanState *node, TupleTableSlot *slot)
{
	return true;
}


static TupleTableSlot *
ColumnarScan_ExecCustomScan(CustomScanState *node)
{
	return CustomExecScan(BioseqScanState::from_custom_scan_state(node),
						  reinterpret_cast<ExecScanAccessMtd>(ColumnarScanNext),
						  reinterpret_cast<ExecScanRecheckMtd>(ColumnarScanRecheck));
}


static void
ColumnarScan_EndCustomScan(CustomScanState *node)
{
	TableScanDesc scanDesc = node->ss.ss_currentScanDesc;
	BioseqScanState* columnarScanState = BioseqScanState::from_custom_scan_state(node);

	/*
	 * Cleanup BMS of selected scan attributes
	 */
	columnarScanState->attrNeeded.free();

	/*
	 * clean out the tuple table
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
	{
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	}
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close heap scan
	 */
	if (scanDesc != NULL)
	{
		table_endscan(scanDesc);
	}

	/* Unregister snapshot */
	if (columnarScanState->snapshotRegisteredByUs)
	{
		UnregisterSnapshot(columnarScanState->snapshot);
	}
}


static void
ColumnarScan_ReScanCustomScan(CustomScanState *node)
{
	CustomScan *cscan = reinterpret_cast<CustomScan *>(node->ss.ps.plan);
	BioseqScanState* columnarScanState = BioseqScanState::from_custom_scan_state(node);

	ResetExprContext(columnarScanState->css_RuntimeContext);
	List *allClauses = PList<List>(cscan->custom_exprs).second();
	columnarScanState->qual = reinterpret_cast<List *>(EvalParamsMutator(
		reinterpret_cast<Node *>(allClauses), columnarScanState->css_RuntimeContext));

	TableScanDesc scanDesc = node->ss.ss_currentScanDesc;

	if (scanDesc != NULL)
	{
		/* XXX: hack to pass quals as scan keys */
		ScanKey scanKeys = reinterpret_cast<ScanKey>(columnarScanState->qual.ptr());
		table_rescan(node->ss.ss_currentScanDesc,
					 scanKeys);
	}
}


static void
ColumnarScan_ExplainCustomScan(CustomScanState *node, List *ancestors,
							   ExplainState *es)
{
	JustLog("[bioseq] pg: ColumnarScan_ExplainCustomScan() entered");
	BioseqScanState* columnarScanState = BioseqScanState::from_custom_scan_state(node);

	List *context = set_deparse_context_planstate(
		es->deparse_cxt, reinterpret_cast<Node *>(&node->ss.ps), ancestors);

	List *projectedColumns = ColumnarVarNeeded(columnarScanState);
	const char *projectedColumnsStr = ColumnarProjectedColumnsStr(
		context, projectedColumns);
	ExplainPropertyText("Bioseq Projected Columns",
						projectedColumnsStr, es);

	CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
	List *chunkGroupFilter = PList<List>(cscan->custom_exprs).second();
	if (chunkGroupFilter != NULL)
	{
		const char *pushdownClausesStr = ColumnarPushdownClausesStr(
			context, chunkGroupFilter);
		ExplainPropertyText("Bioseq Chunk Group Filters",
							pushdownClausesStr, es);

		ColumnarScanDesc columnarScanDesc =
			reinterpret_cast<ColumnarScanDesc>(node->ss.ss_currentScanDesc);
		if (columnarScanDesc != NULL)
		{
			ExplainPropertyInteger(
				"Bioseq Chunk Groups Removed by Filter",
				NULL, ColumnarScanChunkGroupsFiltered(columnarScanDesc), es);
		}
	}

	if (columnarScanState->vectorization.vectorizationEnabled &&
		columnarScanState->vectorization.vectorizedQualList != NULL)
	{
		const char *vectorizedWhereClauses = ColumnarPushdownClausesStr(
					context, columnarScanState->vectorization.vectorizedQualList);
		ExplainPropertyText("Bioseq Vectorized Filter", 
							vectorizedWhereClauses, es);
	}

	if (bioseq_enable_page_cache)
	{
		ExplainOpenGroup("Cache", "Cache", true, es);
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str, "->  Cache\n");
			es->indent += 3;
		}
		
		ColumnarCacheStatistics *statistics = ColumnarGetCacheStatistics();

		ExplainPropertyUInteger(
			"Cache Hits",
			NULL,
			statistics->hits,
			es);

		ExplainPropertyUInteger(
			"Cache Misses",
			NULL,
			statistics->misses,
			es);

		ExplainPropertyUInteger(
			"Cache Evictions",
			NULL,
			statistics->evictions,
			es);

		ExplainPropertyUInteger(
			"Cache Writes",
			NULL,
			statistics->writes,
			es);

		ExplainPropertyUInteger(
			"Cache Maximum Size",
			NULL,
			statistics->maximumCacheSize,
			es);

		ExplainPropertyUInteger(
			"Cache Ending Size", 
			NULL, 
			statistics->endingCacheSize,
			es);

		ExplainPropertyUInteger(
			"Total Cache Entries",
			NULL,
			statistics->entries,
			es
		);
		ExplainCloseGroup("Cache", "Cache", true, es);
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			es->indent -= 3;
		}
	}
}


/* Parallel Execution */

static Size 
Columnar_EstimateDSMCustomScan(CustomScanState *node,
							   ParallelContext *pcxt)
{
	Size nbytes;

	BioseqScanState* columnarScanState = BioseqScanState::from_custom_scan_state(node);

	nbytes = offsetof(ParallelColumnarScanData, snapshotData);
	nbytes = add_size(nbytes, EstimateSnapshotSpace(columnarScanState->snapshot));
	nbytes = MAXALIGN(nbytes);

	return nbytes;
}


static void 
Columnar_InitializeDSMCustomScan(CustomScanState *node,
								 ParallelContext *pcxt,
								 void *coordinate)
{
	ParallelColumnarScan pscan = reinterpret_cast<ParallelColumnarScan>(coordinate);
	BioseqScanState* columnarScanState = BioseqScanState::from_custom_scan_state(node);
	
	/* 
	 * Serialize scan snapshot for workers so they see changes
	 * if we have flushed stripe / row_mask during this scan.
	 */
	SerializeSnapshot(columnarScanState->snapshot, pscan->snapshotData);

	/* Initialize parallel scan mutex */
	SpinLockInit(&pscan->mutex);

	/* Stripe numbers are starting from index 1 */
	pg_atomic_init_u64(&pscan->nextStripeId, 1);

	if(parallel_leader_participation)
		columnarScanState->parallelColumnarScan = pscan;
	else
		columnarScanState->parallelColumnarScan = NULL;
}


static void 
Columnar_ReinitializeDSMCustomScan(CustomScanState *node,
								   ParallelContext *pcxt,
								   void *coordinate)
{
	ParallelColumnarScan pscan = reinterpret_cast<ParallelColumnarScan>(coordinate);
	BioseqScanState* columnarScanState = BioseqScanState::from_custom_scan_state(node);

	/* Reset atomic nextStripeId to initial value */
	pg_atomic_init_u64(&pscan->nextStripeId, 1);

	if(parallel_leader_participation)
		columnarScanState->parallelColumnarScan = pscan;
	else
		columnarScanState->parallelColumnarScan = NULL;
}


static void 
Columnar_InitializeWorkerCustomScan(CustomScanState *node,
									shm_toc *toc,
									void *coordinate)
{
	BioseqScanState* columnarScanState = BioseqScanState::from_custom_scan_state(node);
	ParallelColumnarScan pscan = reinterpret_cast<ParallelColumnarScan>(coordinate);
	columnarScanState->parallelColumnarScan = pscan;
	columnarScanState->snapshot = RestoreSnapshot(pscan->snapshotData);
}


/*
 * ColumnarPushdownClausesStr represents the clauses to push down as a string.
 */
static const char *
ColumnarPushdownClausesStr(List *context, List *clauses)
{
	Expr *conjunction;

	PList<Expr> clauseList{ clauses };
	Assert(clauseList.length() > 0);

	if (clauseList.length() == 1)
	{
		conjunction = clauseList.first();
	}
	else
	{
		conjunction = make_andclause(clauses);
	}

	bool useTableNamePrefix = false;
	bool showImplicitCast = false;
	return deparse_expression(reinterpret_cast<Node *>(conjunction), context,
							  useTableNamePrefix, showImplicitCast);
}


/*
 * ColumnarProjectedColumnsStr generates projected column string for
 * explain output.
 */
static const char *
ColumnarProjectedColumnsStr(List *context, List *projectedColumns)
{
	if (list_length(projectedColumns) == 0)
	{
		return "<columnar optimized out all columns>";
	}

	bool useTableNamePrefix = false;
	bool showImplicitCast = false;
	return deparse_expression(reinterpret_cast<Node *>(projectedColumns), context,
							  useTableNamePrefix, showImplicitCast);
}


/*
 * ColumnarVarNeeded returns a list of Var objects for the ones that are
 * needed during columnar custom scan.
 * Throws an error if finds a Var referencing to an attribute not supported
 * by ColumnarScan.
 */
static List *
ColumnarVarNeeded(BioseqScanState *columnarScanState)
{
	ScanState *scanState = &columnarScanState->custom_scanstate.ss;

	PList<Var> varList;

	PBitmapset neededAttrSet =
		ColumnarAttrNeeded(scanState, columnarScanState->vectorization.vectorizedQualList);

	for (int bmsMember : neededAttrSet)
	{
		Relation columnarRelation = scanState->ss_currentRelation;

		/* neededAttrSet already represents 0-indexed attribute numbers */
		Form_pg_attribute columnForm =
			TupleDescAttr(RelationGetDescr(columnarRelation), bmsMember);
		if (columnForm->attisdropped)
		{
			PgReport(ERROR).code(ERRCODE_UNDEFINED_COLUMN)
							.msg("cannot explain column with attrNum={} "
								 "of columnar table {} since it is dropped",
								 bmsMember + 1,
								 RelationGetRelationName(columnarRelation));
		}
		else if (columnForm->attnum <= 0)
		{
			/*
			 * ColumnarAttrNeeded should have already thrown an error for
			 * system columns. Similarly, it should have already expanded
			 * whole-row references to individual attributes.
			 */
			PgReport(ERROR).code(ERRCODE_FEATURE_NOT_SUPPORTED)
							.msg("cannot explain column with attrNum={} "
								 "of columnar table {} since it is either "
								 "a system column or a whole-row "
								 "reference", columnForm->attnum,
								 RelationGetRelationName(columnarRelation));
		}


		/*
		 * varlevelsup is used to figure out the (query) level of the Var
		 * that we are investigating. Since we are dealing with a particular
		 * relation, it is useless here.
		 */
		Index varlevelsup = 0;

		CustomScan *customScan = reinterpret_cast<CustomScan *>(columnarScanState->plan_state()->plan);
		Index scanrelid = customScan->scan.scanrelid;
		Var *var = makeVar(scanrelid, columnForm->attnum, columnForm->atttypid,
						   columnForm->atttypmod, columnForm->attcollation,
						   varlevelsup);
		varList.append(var);
	}

	return varList.release();
}


/*
 * set_deparse_context_planstate forwards to set_deparse_context_plan, going
 * through the PlanState* indirection that the EXPLAIN code already holds.
 */
static List *
set_deparse_context_planstate(List *dpcontext, Node *node, List *ancestors)
{
	PlanState *ps = reinterpret_cast<PlanState *>(node);
	return set_deparse_context_plan(dpcontext, ps->plan, ancestors);
}
