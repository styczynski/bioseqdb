module; 
#include "common.h"

#include <math.h>

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"

#include "columnar/vectorization/columnar_vector_types.h"

export module bioseq_table_types;

/*
 * ColumnarScanDescData is the scan state passed between beginscan(),
 * getnextslot(), rescan(), and endscan() calls.
 */
export typedef struct ColumnarScanDescData
{
	TableScanDescData cs_base;
	ColumnarReadState *cs_readState;

	/*
	 * We initialize cs_readState lazily in the first getnextslot() call. We
	 * need the following for initialization. We save them in beginscan().
	 */
	MemoryContext scanContext;
	Bitmapset *attr_needed;
	List *scanQual;

	/* Parallel Scan Data */
	ParallelColumnarScan parallelColumnarScan;

	/* Vectorization */
	bool returnVectorizedTuple;
} ColumnarScanDescData;

export typedef struct ColumnarScanDescData *ColumnarScanDesc;

/*
 * IndexFetchColumnarData is the scan state passed between index_fetch_begin,
 * index_fetch_reset, index_fetch_end, index_fetch_tuple calls.
 */
export typedef struct IndexFetchColumnarData
{
	IndexFetchTableData cs_base;
	ColumnarReadState *cs_readState;
	Bitmapset *attr_needed;
	List *stripeMetadataList;
	bool is_select_query; /* CustomIndexScan only gets planned with SELECT query */

	/*
	 * We initialize cs_readState lazily in the first bioseq_index_fetch_tuple
	 * call. However, we want to do memory allocations in a sub MemoryContext of
	 * bioseq_index_fetch_begin. For this reason, we store scanContext in
	 * bioseq_index_fetch_begin.
	 */
	MemoryContext scanContext;
} IndexFetchColumnarData;

/*
 * ColumnarScanState represents the state for a columnar scan. It's a
 * CustomScanState with additional fields specific to columnar scans.
 */
export typedef struct ColumnarScanState
{
	CustomScanState custom_scanstate; /* must be first field */
	Bitmapset *attrNeeded;

	ExprContext *css_RuntimeContext;
	List *qual;

	/* Parallel execution */
	ParallelColumnarScan parallelColumnarScan;

	/* Vectorization */
	struct
	{
		bool vectorizationEnabled;
		bool vectorizationAggregate;
		TupleTableSlot *scanVectorSlot;
		TupleTableSlot *resultVectorSlot;
		uint32 vectorPendingRowNumber;
		uint32 vectorRowIndex;
		List *vectorizedQualList;
		List *constructedVectorizedQualList;
		List *attrNeededList;
	} vectorization;

	/* Scan snapshot*/
	Snapshot snapshot;
	bool snapshotRegisteredByUs;
} ColumnarScanState;


export typedef bool (*PathPredicate)(Path *path);