/*-------------------------------------------------------------------------
 *
 * table/io/columnar_reader.cppm
 *
 * Reader side of the bioseq columnar table format. Owns the
 * ColumnarReadState lifecycle: locating stripes via the metadata
 * tables, decompressing chunk groups, applying chunk-group filter
 * pushdowns, and yielding tuples (or vectorised slots) to the
 * executor. Pairs with columnar_writer.cppm.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"
#include "types.h"

#include "safe_lib.h"

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"
#include "columnar/vectorization/columnar_vector_types.h"

export module bioseq_table_io:reader;

import storage_compression;
import storage_physical;
import storage_metadata;
import pg_utils;

/*
 * fmt-style format string used by pg_log() when a stripe is unflushed
 * yet the read path tries to consume it. Kept as a `const char*` rather
 * than an inline string so call sites use the same wording.
 */
static constexpr const char* UNEXPECTED_STRIPE_READ_ERR_MSG =
	"attempted to read an unexpected stripe while reading columnar "
	"table {}, stripe with id={} is not flushed";


export ChunkData *
CreateEmptyChunkData(uint32_t columnCount, bool *columnMask, uint32_t chunkGroupRowCount);
export int64_t
ColumnarReadChunkGroupsFiltered(ColumnarReadState *state);
export bool
ColumnarSetStripeReadState(ColumnarReadState *readState,
						   StripeMetadata *startStripeMetadata);
export bool
ColumnarReadNextRow(ColumnarReadState *readState, Datum *columnValues, bool *columnNulls,
					uint64_t *rowNumber);
export ColumnarReadState *
ColumnarBeginRead(Relation relation, TupleDesc tupleDescriptor,
				  List *projectedColumnList, List *whereClauseList,
				  MemoryContext scanContext, Snapshot snapshot,
				  bool randomAccess,
				  ParallelColumnarScan parallelColumnarScan);
export bool
ColumnarReadNextVector(ColumnarReadState *readState,  Datum *columnValues,
					   bool *columnNulls, uint64 *rowNumber, int *newVectorSize);
export void
ColumnarRescan(ColumnarReadState *readState, List *scanQual);
export void
ColumnarEndRead(ColumnarReadState *readState);
void
ColumnarAddCacheEntry(uint64_t relId, uint64_t stripeId, uint64_t chunkId, 
					  uint32_t columnId, void *data);
export void
ColumnarReadRowByRowNumberOrError(ColumnarReadState *readState,
								  uint64_t rowNumber, Datum *columnValues,
								  bool *columnNulls);
MemoryContext
ColumnarCacheMemoryContext(void);
void*
ColumnarRetrieveCache(uint64_t relId, uint64_t stripeId, uint64_t chunkId, uint32_t columnId);
void FreeChunkBufferValueArray(ChunkData *chunkData);
export void
ColumnarReadFlushPendingWrites(ColumnarReadState *readState);
export bool
ColumnarReadRowByRowNumber(ColumnarReadState *readState,
						   uint64_t rowNumber, Datum *columnValues,
						   bool *columnNulls);
void
ColumnarResetRead(ColumnarReadState *readState);
void
ColumnarMarkChunkGroupInUse(uint64_t relId, uint64_t stripeId, uint32_t chunkId);


/* static function declarations */
static MemoryContext CreateStripeReadMemoryContext(void);
static bool ColumnarReadIsCurrentStripe(ColumnarReadState *readState,
										uint64_t rowNumber);
static StripeMetadata * ColumnarReadGetCurrentStripe(ColumnarReadState *readState);
static bool ReadStripeRowByRowNumber(ColumnarReadState *readState,
									 uint64_t rowNumber, Datum *columnValues,
									 bool *columnNulls);
static bool StripeReadIsCurrentChunkGroup(StripeReadState *stripeReadState,
										  int chunkGroupIndex);
static bool ReadChunkGroupRowByRowOffset(ChunkGroupReadState *chunkGroupReadState,
										 StripeMetadata *stripeMetadata,
										 uint64_t stripeRowOffset, Datum *columnValues,
										 bool *columnNulls);
static bool StripeReadInProgress(ColumnarReadState *readState);
static bool HasUnreadStripe(ColumnarReadState *readState);
static StripeReadState * BeginStripeRead(StripeMetadata *stripeMetadata, Relation rel,
										 TupleDesc tupleDesc, List *projectedColumnList,
										 List *whereClauseList, List *whereClauseVars,
										 MemoryContext stripeReadContext,
										 Snapshot snapshot);
static void AdvanceStripeRead(ColumnarReadState *readState);
static bool SnapshotMightSeeUnflushedStripes(Snapshot snapshot);
static bool ReadStripeNextRow(StripeReadState *stripeReadState, Datum *columnValues,
							  bool *columnNulls,
							  uint64_t stripeFirstRowNumber,
							  Snapshot snapshot, uint64_t stripeId);
static ChunkGroupReadState * BeginChunkGroupRead(StripeBuffers *stripeBuffers, int
												 chunkIndex,
												 TupleDesc tupleDesc,
												 List *projectedColumnList,
												 MemoryContext cxt, StripeReadState *state, uint64_t stripeId);
static void EndChunkGroupRead(ChunkGroupReadState *chunkGroupReadState);
static bool ReadChunkGroupNextRow(ChunkGroupReadState *chunkGroupReadState,
								  Datum *columnValues,
								  bool *columnNulls,
								  int32_t *deletedColumnsNumber);
static StripeBuffers * LoadFilteredStripeBuffers(Relation relation,
												 StripeMetadata *stripeMetadata,
												 TupleDesc tupleDescriptor,
												 List *projectedColumnList,
												 List *whereClauseList,
												 List *whereClauseVars,
												 int64 *chunkGroupsFiltered,
												 Snapshot snapshot);
static ColumnBuffers * LoadColumnBuffers(Relation relation,
										 ColumnChunkSkipNode *chunkSkipNodeArray,
										 uint32_t chunkCount, uint64_t stripeOffset,
										 Form_pg_attribute attributeForm);
static bool * SelectedChunkMask(StripeSkipList *stripeSkipList,
								List *whereClauseList, List *whereClauseVars,
								int64 *chunkGroupsFiltered);
static Node * BuildBaseConstraint(Var *variable);
static List * GetClauseVars(List *clauses, int natts);
static OpExpr * MakeOpExpression(Var *variable, int16_t strategyNumber);
static Oid GetOperatorByType(Oid typeId, Oid accessMethodId, int16_t strategyNumber);
static void UpdateConstraint(Node *baseConstraint, Datum minValue, Datum maxValue);
static StripeSkipList * SelectedChunkSkipList(StripeSkipList *stripeSkipList,
											  bool *projectedColumnMask,
											  bool *selectedChunkMask);
static uint32_t StripeSkipListRowCount(StripeSkipList *stripeSkipList);
static bool * ProjectedColumnMask(uint32_t columnCount, List *projectedColumnList);
static void DeserializeBoolArray(StringInfo boolArrayBuffer, bool *boolArray,
								 uint32_t boolArrayLength);
static void DeserializeDatumArray(StringInfo datumBuffer, bool *existsArray,
								  uint32_t datumCount, bool datumTypeByValue,
								  int datumTypeLength, char datumTypeAlign,
								  Datum *datumArray);
static ChunkData * DeserializeChunkData(StripeBuffers *stripeBuffers, uint64_t chunkIndex,
										uint32_t rowCount, TupleDesc tupleDescriptor,
										List *projectedColumnList, StripeReadState *state, uint64_t stripeId);
static Datum ColumnDefaultValue(TupleConstr *tupleConstraints,
								Form_pg_attribute attributeForm);

/* Vectorization */
static bool ReadStripeNextVector(StripeReadState *stripeReadState, Datum *columnValues,
								 bool *columnNulls, int *newVectorSize,
								 uint64 stripeId,
								 Snapshot snapshot,
								 uint64 *rowNumber,
								 uint64 stripeFirstRowNumber);
static bool ReadChunkGroupNextVector(ChunkGroupReadState *chunkGroupReadState, Datum *columnValues,
									 bool *columnNulls, TupleDesc tupleDesc, 
									 int32_t *columnValueOffset, int *chunkReadRows,
									 uint64 *rowNumber,
									 uint64 stripeFirstRowNumber);

/*
 * ColumnarBeginRead initializes a columnar read operation. This function returns a
 * read handle that's used during reading rows and finishing the read operation.
 *
 * projectedColumnList is an integer list of attribute numbers (1-indexed).
 */
ColumnarReadState *
ColumnarBeginRead(Relation relation, TupleDesc tupleDescriptor,
				  List *projectedColumnList, List *whereClauseList,
				  MemoryContext scanContext, Snapshot snapshot,
				  bool randomAccess,
				  ParallelColumnarScan parallelColumnarScan)
{
	/*
	 * We allocate all stripe specific data in the stripeReadContext, and reset
	 * this memory context before loading a new stripe. This is to avoid memory
	 * leaks.
	 */
	MemoryContext stripeReadContext = CreateStripeReadMemoryContext();

	ColumnarReadState *readState = make0<ColumnarReadState>();
	readState->relation = relation;
	readState->projectedColumnList = projectedColumnList;
	readState->whereClauseList = whereClauseList;
	readState->whereClauseVars = GetClauseVars(whereClauseList, tupleDescriptor->natts);
	readState->chunkGroupsFiltered = 0;
	readState->tupleDescriptor = tupleDescriptor;
	readState->stripeReadContext = stripeReadContext;
	readState->stripeReadState = NULL;
	readState->scanContext = scanContext;

	/*
	 * Note that ColumnarReadFlushPendingWrites might update those two by
	 * registering a new snapshot.
	 */
	readState->snapshot = snapshot;
	readState->snapshotRegisteredByUs = false;

	/* Parallel execution */
	readState->parallelColumnarScan = parallelColumnarScan;

	if (!randomAccess)
	{
		/*
		 * If have parallel columnar scan we don't flush states.
		 */
		if (readState->parallelColumnarScan == NULL)
		{
			/*
			* Flush any pending in-memory changes to row_mask metadata
			* for next scan.
			*/
			RowMaskFlushWriteStateForRelfilenode(readState->relation->rd_locator.relNumber,
												 GetCurrentSubTransactionId());

			/*
			* When doing random access (i.e.: index scan), we don't need to flush
			* pending writes until we need to read them.
			* bioseq_index_fetch_tuple would do so when needed.
			*/
			ColumnarReadFlushPendingWrites(readState);
		}

		/*
		 * AdvanceStripeRead sets currentStripeMetadata for the first stripe
		 * to read if not doing random access. Otherwise, reader (i.e.:
		 * ColumnarReadRowByRowNumber) would already decide the stripe to read
		 * on-the-fly.
		 *
		 * Moreover, Since we don't flush pending writes for random access,
		 * AdvanceStripeRead might encounter with stripe metadata entries due
		 * to current transaction's pending writes even when using an MVCC
		 * snapshot, but AdvanceStripeRead would throw an error for that.
		 * Note that this is not the case with for plain table scan methods
		 * (i.e.: SeqScan and Columnar CustomScan).
		 *
		 * For those reasons, we don't call AdvanceStripeRead if we will do
		 * random access.
		 */
		AdvanceStripeRead(readState);
	}

	return readState;
}


/*
 * ColumnarReadFlushPendingWrites flushes pending writes for read operation
 * and sets a new (registered) snapshot if necessary.
 *
 * If it sets a new snapshot, then sets snapshotRegisteredByUs to true to
 * indicate that caller should unregister the snapshot after finishing read
 * operation.
 *
 * Note that this function assumes that readState's relation and snapshot
 * fields are already set.
 */
void
ColumnarReadFlushPendingWrites(ColumnarReadState *readState)
{
	Assert(!readState->snapshotRegisteredByUs);

	Oid relfilelocator = readState->relation->rd_locator.relNumber;

	FlushWriteStateWithNewSnapshot(relfilelocator, &readState->snapshot,
								   &readState->snapshotRegisteredByUs);
}


/*
 * CreateStripeReadMemoryContext creates a memory context to be used when
 * reading a stripe.
 */
static MemoryContext
CreateStripeReadMemoryContext()
{
	return AllocSetContextCreate(CurrentMemoryContext, "Stripe Read Memory Context",
								 ALLOCSET_DEFAULT_SIZES);
}


/*
 * ColumnarReadNextRow tries to read a row from the columnar table. On success, it sets
 * column values, column nulls and rowNumber (if passed to be non-NULL), and returns true.
 * If there are no more rows to read, the function returns false.
 */
bool
ColumnarReadNextRow(ColumnarReadState *readState, Datum *columnValues, bool *columnNulls,
					uint64_t *rowNumber)
{
	while (true)
	{
		if (!StripeReadInProgress(readState))
		{
			if (!HasUnreadStripe(readState))
			{
				return false;
			}

			readState->stripeReadState = BeginStripeRead(readState->currentStripeMetadata,
														 readState->relation,
														 readState->tupleDescriptor,
														 readState->projectedColumnList,
														 readState->whereClauseList,
														 readState->whereClauseVars,
														 readState->stripeReadContext,
														 readState->snapshot);
		}

		if (!ReadStripeNextRow(readState->stripeReadState, columnValues, columnNulls,
							   readState->currentStripeMetadata->firstRowNumber,
							   readState->snapshot,
								 readState->currentStripeMetadata->id))
		{
			AdvanceStripeRead(readState);
			continue;
		}

		if (rowNumber)
		{
			*rowNumber = readState->currentStripeMetadata->firstRowNumber +
						 readState->stripeReadState->chunkGroupReadState->chunkStripeRowOffset +
						 readState->stripeReadState->chunkGroupReadState->currentRow  - 1;
		}

		return true;
	}

	return false;
}


/*
 * ColumnarReadRowByRowNumberOrError is a wrapper around
 * ColumnarReadRowByRowNumber that throws an error if tuple
 * with rowNumber does not exist.
 */
void
ColumnarReadRowByRowNumberOrError(ColumnarReadState *readState,
								  uint64_t rowNumber, Datum *columnValues,
								  bool *columnNulls)
{
	if (!ColumnarReadRowByRowNumber(readState, rowNumber,
									columnValues, columnNulls))
	{
		pg_log(ERROR, "cannot read from columnar table {}, tuple with "
					  "row number {} does not exist",
					  RelationGetRelationName(readState->relation),
					  rowNumber);
	}
}


/*
 * ColumnarReadRowByRowNumber reads row with rowNumber from given relation
 * into columnValues and columnNulls, and returns true. If no such row
 * exists, then returns false.
 */
bool
ColumnarReadRowByRowNumber(ColumnarReadState *readState,
						   uint64_t rowNumber, Datum *columnValues,
						   bool *columnNulls)
{
	if (!ColumnarReadIsCurrentStripe(readState, rowNumber))
	{
		Relation columnarRelation = readState->relation;
		Snapshot snapshot = readState->snapshot;
		StripeMetadata *stripeMetadata = FindStripeByRowNumber(columnarRelation,
															   rowNumber, snapshot);
		if (stripeMetadata == NULL)
		{
			/* no such row exists */
			return false;
		}

		if (StripeWriteState(stripeMetadata) != STRIPE_WRITE_FLUSHED)
		{
			/*
			 * Callers are expected to skip stripes that are not flushed to
			 * disk yet or should wait for the writer xact to commit or abort,
			 * but let's be on the safe side.
			 */
			pg_log(ERROR, UNEXPECTED_STRIPE_READ_ERR_MSG,
				   RelationGetRelationName(columnarRelation),
				   stripeMetadata->id);
		}

		/* do the cleanup before reading a new stripe */
		ColumnarResetRead(readState);

		TupleDesc relationTupleDesc = RelationGetDescr(columnarRelation);
		List *whereClauseList = NIL;
		List *whereClauseVars = NIL;
		MemoryContext stripeReadContext = readState->stripeReadContext;
		readState->stripeReadState = BeginStripeRead(stripeMetadata,
													 columnarRelation,
													 relationTupleDesc,
													 readState->projectedColumnList,
													 whereClauseList,
													 whereClauseVars,
													 stripeReadContext,
													 snapshot);

		readState->currentStripeMetadata = stripeMetadata;
	}

	return ReadStripeRowByRowNumber(readState, rowNumber, columnValues, columnNulls);
}

/*
 * ColumnarSetStripeReadState 
 */
bool
ColumnarSetStripeReadState(ColumnarReadState *readState,
						   StripeMetadata *startStripeMetadata)
{
	if (!ColumnarReadIsCurrentStripe(readState, startStripeMetadata->firstRowNumber))
	{
		Relation columnarRelation = readState->relation;
		Snapshot snapshot = readState->snapshot;
		StripeMetadata *stripeMetadata = 
			FindStripeByRowNumber(columnarRelation,
								  startStripeMetadata->firstRowNumber, snapshot);
		if (stripeMetadata == NULL)
		{
			/* no such row exists */
			return false;
		}

		if (StripeWriteState(stripeMetadata) != STRIPE_WRITE_FLUSHED)
		{
			/*
			 * Callers are expected to skip stripes that are not flushed to
			 * disk yet or should wait for the writer xact to commit or abort,
			 * but let's be on the safe side.
			 */
			pg_log(ERROR, UNEXPECTED_STRIPE_READ_ERR_MSG,
				   RelationGetRelationName(columnarRelation),
				   stripeMetadata->id);
		}

		/* do the cleanup before reading a new stripe */
		ColumnarResetRead(readState);

		TupleDesc relationTupleDesc = RelationGetDescr(columnarRelation);
		List *whereClauseList = NIL;
		List *whereClauseVars = NIL;
		MemoryContext stripeReadContext = readState->stripeReadContext;
		readState->stripeReadState = BeginStripeRead(stripeMetadata,
													 columnarRelation,
													 relationTupleDesc,
													 readState->projectedColumnList,
													 whereClauseList,
													 whereClauseVars,
													 stripeReadContext,
													 snapshot);

		readState->currentStripeMetadata = stripeMetadata;
	}

	return true;
}

/*
 * ColumnarReadIsCurrentStripe returns true if stripe being read contains
 * row with given rowNumber.
 */
static bool
ColumnarReadIsCurrentStripe(ColumnarReadState *readState, uint64_t rowNumber)
{
	if (!StripeReadInProgress(readState))
	{
		return false;
	}

	StripeMetadata *currentStripeMetadata = readState->currentStripeMetadata;
	if (rowNumber >= currentStripeMetadata->firstRowNumber &&
		rowNumber <= StripeGetHighestRowNumber(currentStripeMetadata))
	{
		return true;
	}

	return false;
}


/*
 * ColumnarReadGetCurrentStripe returns StripeMetadata for the stripe that is
 * being read.
 */
static StripeMetadata *
ColumnarReadGetCurrentStripe(ColumnarReadState *readState)
{
	return readState->currentStripeMetadata;
}


/*
 * ReadStripeRowByRowNumber reads row with rowNumber from given
 * stripeReadState into columnValues and columnNulls.
 * Errors out if no such row exists in the stripe being read.
 */
static bool
ReadStripeRowByRowNumber(ColumnarReadState *readState,
						 uint64_t rowNumber, Datum *columnValues,
						 bool *columnNulls)
{
	StripeMetadata *stripeMetadata = ColumnarReadGetCurrentStripe(readState);
	StripeReadState *stripeReadState = readState->stripeReadState;

	if (rowNumber < stripeMetadata->firstRowNumber)
	{
		/* not expected but be on the safe side */
		pg_log(ERROR, "row offset cannot be negative");
	}

	/* find the exact chunk group to be read */
	uint64_t stripeRowOffset = rowNumber - stripeMetadata->firstRowNumber;
	int chunkGroupIndex = stripeRowOffset / stripeMetadata->chunkGroupRowCount;
	if (!StripeReadIsCurrentChunkGroup(stripeReadState, chunkGroupIndex))
	{
		if (stripeReadState->chunkGroupReadState)
		{
			EndChunkGroupRead(stripeReadState->chunkGroupReadState);
		}

		stripeReadState->chunkGroupIndex = chunkGroupIndex;
		stripeReadState->chunkGroupReadState = BeginChunkGroupRead(
			stripeReadState->stripeBuffers,
			stripeReadState->chunkGroupIndex,
			stripeReadState->tupleDescriptor,
			stripeReadState->projectedColumnList,
			stripeReadState->stripeReadContext,
			stripeReadState,
			readState->currentStripeMetadata->id
			);

		uint64_t chunkFirstRowNumber =
			stripeMetadata->firstRowNumber +
			stripeReadState->chunkGroupReadState->chunkStripeRowOffset;

		if (bioseq_enable_dml)
		{
			RowMaskWriteStateEntry *rowMaskEntry = 
				RowMaskFindWriteState(stripeReadState->relation->rd_locator.relNumber,
									  GetCurrentSubTransactionId(), rowNumber);
			if (rowMaskEntry != NULL)
			{
				stripeReadState->chunkGroupReadState->rowMask = rowMaskEntry->mask;
				stripeReadState->chunkGroupReadState->rowMaskCached = true;
			}
			else if (stripeReadState->chunkGroupReadState->chunkGroupDeletedRows > 0)
			{
				stripeReadState->chunkGroupReadState->rowMask =
					ReadChunkRowMask(stripeReadState->relation->rd_locator,
										readState->snapshot,
										stripeReadState->stripeReadContext,
										chunkFirstRowNumber,
										stripeReadState->chunkGroupReadState->rowCount);
				stripeReadState->chunkGroupReadState->rowMaskCached = false;
			}
		}
		else
		{
			stripeReadState->chunkGroupReadState->rowMask = NULL;
		}
	}

	return ReadChunkGroupRowByRowOffset(stripeReadState->chunkGroupReadState,
										stripeMetadata, stripeRowOffset,
										columnValues, columnNulls);
}


/*
 * StripeReadIsCurrentChunkGroup returns true if chunk group being read is
 * the has given chunkGroupIndex in its stripe.
 */
static bool
StripeReadIsCurrentChunkGroup(StripeReadState *stripeReadState, int chunkGroupIndex)
{
	if (!stripeReadState->chunkGroupReadState)
	{
		return false;
	}

	return (stripeReadState->chunkGroupIndex == chunkGroupIndex);
}


/*
 * ReadChunkGroupRowByRowOffset reads row with stripeRowOffset from given
 * chunkGroupReadState into columnValues and columnNulls.
 * Errors out if no such row exists in the chunk group being read.
 */
static bool
ReadChunkGroupRowByRowOffset(ChunkGroupReadState *chunkGroupReadState,
							 StripeMetadata *stripeMetadata,
							 uint64_t stripeRowOffset, Datum *columnValues,
							 bool *columnNulls)
{
	/* set the exact row number to be read from given chunk roup */
	chunkGroupReadState->currentRow = stripeRowOffset %
									  stripeMetadata->chunkGroupRowCount;
	int isDeleted = 0;
	ReadChunkGroupNextRow(chunkGroupReadState, columnValues, columnNulls,
						  &isDeleted);
	return !isDeleted;
}


/*
 * StripeReadInProgress returns true if we already started reading a stripe.
 */
static bool
StripeReadInProgress(ColumnarReadState *readState)
{
	return readState->stripeReadState != NULL;
}


/*
 * HasUnreadStripe returns true if we still have stripes to read during current
 * read operation.
 */
static bool
HasUnreadStripe(ColumnarReadState *readState)
{
	return readState->currentStripeMetadata != NULL;
}


/*
 * ColumnarRescan clears the position where we were scanning so that the next read starts at
 * the beginning again
 */
void
ColumnarRescan(ColumnarReadState *readState, List *scanQual)
{
	MemoryContextScope ctx_guard{readState->scanContext};

	ColumnarResetRead(readState);

	/* set currentStripeMetadata for the first stripe to read */
	AdvanceStripeRead(readState);

	readState->chunkGroupsFiltered = 0;

	readState->whereClauseList = copyObject(scanQual);
}


/*
 * Finishes a columnar read operation.
 */
void
ColumnarEndRead(ColumnarReadState *readState)
{
	if (readState->snapshotRegisteredByUs)
	{
		/*
		 * init_bioseq_read_state created a new snapshot and registered it,
		 * so now forget it.
		 */
		UnregisterSnapshot(readState->snapshot);
	}

	MemoryContextDelete(readState->stripeReadContext);
	if (readState->currentStripeMetadata)
	{
		pfree(readState->currentStripeMetadata);
	}

	if (readState) {
		pfree(readState);
	}
}


/*
 * ColumnarResetRead resets the stripe and the chunk group that is
 * being read currently (if any).
 */
void
ColumnarResetRead(ColumnarReadState *readState)
{
	if (StripeReadInProgress(readState))
	{
		pfree(readState->currentStripeMetadata);
		readState->currentStripeMetadata = NULL;

		readState->stripeReadState = NULL;
		MemoryContextReset(readState->stripeReadContext);
	}
}


/*
 * BeginStripeRead allocates state for reading a stripe.
 */
static StripeReadState *
BeginStripeRead(StripeMetadata *stripeMetadata, Relation rel, TupleDesc tupleDesc,
				List *projectedColumnList, List *whereClauseList, List *whereClauseVars,
				MemoryContext stripeReadContext, Snapshot snapshot)
{
	MemoryContextScope ctx_guard{stripeReadContext};

	StripeReadState *stripeReadState = make0<StripeReadState>();

	stripeReadState->relation = rel;
	stripeReadState->tupleDescriptor = tupleDesc;
	stripeReadState->columnCount = tupleDesc->natts;
	stripeReadState->chunkGroupReadState = NULL;
	stripeReadState->projectedColumnList = projectedColumnList;
	stripeReadState->stripeReadContext = stripeReadContext;

	stripeReadState->stripeBuffers = LoadFilteredStripeBuffers(rel,
															   stripeMetadata,
															   tupleDesc,
															   projectedColumnList,
															   whereClauseList,
															   whereClauseVars,
															   &stripeReadState->
															   chunkGroupsFiltered,
															   snapshot);

	stripeReadState->rowCount = stripeReadState->stripeBuffers->rowCount;

	return stripeReadState;
}


/*
 * AdvanceStripeRead updates chunkGroupsFiltered and sets
 * currentStripeMetadata for next stripe read.
 */
static void
AdvanceStripeRead(ColumnarReadState *readState)
{
	MemoryContextScope ctx_guard{readState->scanContext};

	if (readState->parallelColumnarScan == 0)
	{
		/* if not read any stripes yet, start from the first one .. */
		uint64_t lastReadRowNumber = COLUMNAR_INVALID_ROW_NUMBER;
		if (StripeReadInProgress(readState))
		{
			/* .. otherwise, continue with the next stripe */
			lastReadRowNumber = StripeGetHighestRowNumber(readState->currentStripeMetadata);

			readState->chunkGroupsFiltered +=
				readState->stripeReadState->chunkGroupsFiltered;
		}

		readState->currentStripeMetadata = FindNextStripeByRowNumber(readState->relation,
																	lastReadRowNumber,
																	readState->snapshot);
	}
	else
	{
		if (StripeReadInProgress(readState))
		{
			readState->chunkGroupsFiltered +=
				readState->stripeReadState->chunkGroupsFiltered;
		}

		SpinLockAcquire(&readState->parallelColumnarScan->mutex);

		/* Fetch atomic next stripe id to be read by this scan. */
		uint64_t nextStripeId = 
			pg_atomic_fetch_add_u64(&readState->parallelColumnarScan->nextStripeId, 1);

		uint64_t nextHigherStripeId = nextStripeId;

		readState->currentStripeMetadata = FindNextStripeForParallelWorker(readState->relation,
																		   readState->snapshot,
																		   nextStripeId,
																		   &nextHigherStripeId);

		/* 
		 * There exists higher stripe id than this one so adjust and 
		 * add +1 for next workers.
		 */
		if (nextHigherStripeId != nextStripeId)
		{
			pg_atomic_write_u64(&readState->parallelColumnarScan->nextStripeId,
								nextHigherStripeId + 1);
		}

		SpinLockRelease(&readState->parallelColumnarScan->mutex);
	}

	if (readState->currentStripeMetadata &&
		StripeWriteState(readState->currentStripeMetadata) != STRIPE_WRITE_FLUSHED &&
		!SnapshotMightSeeUnflushedStripes(readState->snapshot))
	{
		/*
		 * To be on the safe side, error out if we don't expect to encounter
		 * with an un-flushed stripe. Otherwise, we will skip such stripes
		 * until finding a flushed one.
		 */
		pg_log(ERROR, UNEXPECTED_STRIPE_READ_ERR_MSG,
			   RelationGetRelationName(readState->relation),
			   readState->currentStripeMetadata->id);
	}

	while (readState->currentStripeMetadata &&
		   StripeWriteState(readState->currentStripeMetadata) != STRIPE_WRITE_FLUSHED)
	{
		readState->currentStripeMetadata =
			FindNextStripeByRowNumber(readState->relation,
									  readState->currentStripeMetadata->firstRowNumber,
									  readState->snapshot);
	}

	readState->stripeReadState = NULL;
	MemoryContextReset(readState->stripeReadContext);
}


/*
 * SnapshotMightSeeUnflushedStripes returns true if given snapshot is
 * expected to see un-flushed stripes either because of other backends'
 * pending writes or aborted transactions.
 */
static bool
SnapshotMightSeeUnflushedStripes(Snapshot snapshot)
{
	if (snapshot == InvalidSnapshot)
	{
		return false;
	}

	switch (snapshot->snapshot_type)
	{
		case SNAPSHOT_ANY:
		case SNAPSHOT_DIRTY:
		case SNAPSHOT_NON_VACUUMABLE:
		{
			return true;
		}

		default:
			return false;
	}
}


/*
 * ReadStripeNextRow: If more rows can be read from the current stripe, fill
 * in non-NULL columnValues and return true. Otherwise, return false.
 *
 * On entry, all entries in columnNulls should be true; this function only
 * sets non-NULL entries.
 *
 */
static bool
ReadStripeNextRow(StripeReadState *stripeReadState, Datum *columnValues,
				  bool *columnNulls,
				  uint64_t stripeFirstRowNumber,
				  Snapshot snapshot, uint64_t stripeId)
{
	if (stripeReadState->currentRow >= stripeReadState->rowCount)
	{
		Assert(stripeReadState->currentRow == stripeReadState->rowCount);
		return false;
	}

	while (true)
	{
		if (stripeReadState->chunkGroupReadState == NULL)
		{
			stripeReadState->chunkGroupReadState = BeginChunkGroupRead(
				stripeReadState->stripeBuffers,
				stripeReadState->
				chunkGroupIndex,
				stripeReadState->
				tupleDescriptor,
				stripeReadState->
				projectedColumnList,
				stripeReadState->
				stripeReadContext,
				stripeReadState,
				stripeId
				);
			
			if (bioseq_enable_dml &&
				stripeReadState->chunkGroupReadState->chunkGroupDeletedRows != 0)
			{
				uint64_t chunkFirstRowNumber = 
					stripeFirstRowNumber + 
					stripeReadState->chunkGroupReadState->chunkStripeRowOffset;
				stripeReadState->chunkGroupReadState->rowMask = 
					ReadChunkRowMask(stripeReadState->relation->rd_locator,
									 snapshot,
									 stripeReadState->stripeReadContext,
									 chunkFirstRowNumber,
									 stripeReadState->chunkGroupReadState->rowCount);
				stripeReadState->chunkGroupReadState->rowMaskCached = false;
			}
			else
			{
				stripeReadState->chunkGroupReadState->rowMask = NULL;
			}
		}

		int32_t deletedColumnsNumber = 0;
		if (!ReadChunkGroupNextRow(stripeReadState->chunkGroupReadState, columnValues,
								   columnNulls, &deletedColumnsNumber))
		{
			/* if this chunk group is exhausted, fetch the next one and loop */

			stripeReadState->currentRow += deletedColumnsNumber;

			bool isStripeExhausted = stripeReadState->currentRow >= stripeReadState->rowCount;

			EndChunkGroupRead(stripeReadState->chunkGroupReadState);
			stripeReadState->chunkGroupReadState = NULL;

			if (isStripeExhausted)
				return false;

			stripeReadState->chunkGroupIndex++;
				
			continue;
		}

		stripeReadState->currentRow += 1 + deletedColumnsNumber;
		return true;
	}

	Assert(stripeReadState->currentRow == stripeReadState->rowCount);
	return false;
}


/*
 * BeginChunkGroupRead allocates state for reading a chunk.
 */
static ChunkGroupReadState *
BeginChunkGroupRead(StripeBuffers *stripeBuffers, int chunkIndex, TupleDesc tupleDesc,
					List *projectedColumnList, MemoryContext cxt, StripeReadState *state, uint64_t stripeId)
{
	if (chunkIndex < 0 || static_cast<uint32_t>(chunkIndex) >= stripeBuffers->chunkCount)
	{
		pg_log(ERROR, "chunk group index {} is outside selected chunk count {}",
			   chunkIndex, stripeBuffers->chunkCount);
	}

	uint32_t chunkGroupRowCount =
		stripeBuffers->selectedChunkGroupRowCounts[chunkIndex];
	uint32_t chunkGroupRowOffset =
		stripeBuffers->selectedChunkGroupRowOffset[chunkIndex];
	uint32_t chunkGroupDeletedRows =
		stripeBuffers->selectedChunkGroupDeletedRows[chunkIndex];

	MemoryContextScope ctx_guard{cxt};
	ChunkGroupReadState *chunkGroupReadState = make0<ChunkGroupReadState>();

	chunkGroupReadState->currentRow = 0;
	chunkGroupReadState->rowCount = chunkGroupRowCount;
	chunkGroupReadState->chunkStripeRowOffset = chunkGroupRowOffset;
	chunkGroupReadState->chunkGroupDeletedRows = chunkGroupDeletedRows;
	chunkGroupReadState->columnCount = tupleDesc->natts;
	chunkGroupReadState->projectedColumnList = projectedColumnList;

	chunkGroupReadState->chunkGroupData = DeserializeChunkData(stripeBuffers, chunkIndex,
															   chunkGroupRowCount,
															   tupleDesc,
															   projectedColumnList, state, stripeId);

	return chunkGroupReadState;
}


/*
 * EndChunkRead finishes a chunk read.
 */
static void
EndChunkGroupRead(ChunkGroupReadState *chunkGroupReadState)
{
	FreeChunkBufferValueArray(chunkGroupReadState->chunkGroupData);
	FreeChunkData(chunkGroupReadState->chunkGroupData);
	if (chunkGroupReadState->rowMask != NULL && !chunkGroupReadState->rowMaskCached)
		pfree(chunkGroupReadState->rowMask);
	chunkGroupReadState->rowMask = NULL;
	pfree(chunkGroupReadState);
}


/*
 * ReadChunkGroupNextRow: if more rows can be read from the current chunk
 * group, fill in non-NULL columnValues and return true. Otherwise, return
 * false.
 *
 * On entry, all entries in columnNulls should be true; this function only
 * sets non-NULL entries.
 */
static bool
ReadChunkGroupNextRow(ChunkGroupReadState *chunkGroupReadState, Datum *columnValues,
					  bool *columnNulls, int32_t *deletedColumnsNumber)
{
	/*
	 * Initialize to all-NULL. Only non-NULL projected attributes will be set.
	 */
	memset(columnNulls, true, sizeof(bool) * chunkGroupReadState->columnCount);

	while (chunkGroupReadState->currentRow < chunkGroupReadState->rowCount)
	{
		if (chunkGroupReadState->rowMask != NULL)
		{
			int8_t checkColumnMask = 1 << (chunkGroupReadState->currentRow % 8);
			int8_t checkLookupMask = VARDATA(chunkGroupReadState->rowMask)[chunkGroupReadState->currentRow / 8 ];

			if (checkLookupMask & checkColumnMask)
			{
				chunkGroupReadState->currentRow++;
				*deletedColumnsNumber += 1;
				continue;
			}
		}

		for (auto attno : PIntList{chunkGroupReadState->projectedColumnList})
		{
			const ChunkData *chunkGroupData = chunkGroupReadState->chunkGroupData;
			const int rowIndex = chunkGroupReadState->currentRow;

			/* attno is 1-indexed; existsArray is 0-indexed */
			const uint32_t columnIndex = attno - 1;

			if (chunkGroupData->existsArray[columnIndex][rowIndex])
			{
				columnValues[columnIndex] = chunkGroupData->valueArray[columnIndex][rowIndex];
				columnNulls[columnIndex] = false;
			}
		}

		chunkGroupReadState->currentRow++;
		return true;
	}

	return false;

}


/*
 * ColumnarReadChunkGroupsFiltered
 *
 * Return the number of chunk groups filtered during this read operation.
 */
int64_t
ColumnarReadChunkGroupsFiltered(ColumnarReadState *state)
{
	return state->chunkGroupsFiltered;
}


/*
 * CreateEmptyChunkDataArray creates data buffers to keep deserialized exist and
 * value arrays for requested columns in columnMask.
 */
ChunkData *
CreateEmptyChunkData(uint32_t columnCount, bool *columnMask, uint32_t chunkGroupRowCount)
{
	uint32_t columnIndex = 0;

	ChunkData *chunkData = make0<ChunkData>();
	chunkData->existsArray = make_array<bool*>(columnCount);
	chunkData->valueArray = make_array<Datum*>(columnCount);
	/* 'cache' member will be 'false' because of 'palloc0' */
	chunkData->valueBufferArray = make1<StringInfo>(columnCount * sizeof(cached_StringInfo));
	chunkData->columnCount = columnCount;
	chunkData->rowCount = chunkGroupRowCount;

	/* allocate chunk memory for deserialized data */
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		if (columnMask[columnIndex])
		{
			chunkData->existsArray[columnIndex] = make_array<bool>(chunkGroupRowCount);
			chunkData->valueArray[columnIndex] = make_array<Datum>(chunkGroupRowCount);
			chunkData->valueBufferArray[columnIndex] = NULL;
		}
	}

	return chunkData;
}



static inline bool
CachedStringInfo(StringInfo info)
{
	return reinterpret_cast<cached_StringInfo *>(info)->cached;
}

/* FreeChunkValueArrayBuffer relase valueBufferArray memory. */
void FreeChunkBufferValueArray(ChunkData *chunkData)
{
	uint32_t columnIndex = 0;

	if (chunkData == NULL)
	{
		return;
	}

	for (columnIndex = 0; columnIndex < chunkData->columnCount; columnIndex++)
	{
		if (chunkData->valueBufferArray[columnIndex] != NULL && !CachedStringInfo(chunkData->valueBufferArray[columnIndex]))
		{
			pfree(chunkData->valueBufferArray[columnIndex]->data);
			pfree(chunkData->valueBufferArray[columnIndex]);
		}
	}
}


/*
 * LoadFilteredStripeBuffers reads serialized stripe data from the given file.
 * The function skips over chunks whose rows are refuted by restriction qualifiers,
 * and only loads columns that are projected in the query.
 */
static StripeBuffers *
LoadFilteredStripeBuffers(Relation relation, StripeMetadata *stripeMetadata,
						  TupleDesc tupleDescriptor, List *projectedColumnList,
						  List *whereClauseList, List *whereClauseVars,
						  int64 *chunkGroupsFiltered, Snapshot snapshot)
{
	uint32_t columnIndex = 0;
	uint32_t columnCount = tupleDescriptor->natts;

	bool *projectedColumnMask = ProjectedColumnMask(columnCount, projectedColumnList);

	StripeSkipList *stripeSkipList = ReadStripeSkipList(relation->rd_locator,
														stripeMetadata->id,
														tupleDescriptor,
														stripeMetadata->chunkCount,
														snapshot);
	StripeSkipList *selectedChunkSkipList = stripeSkipList;

	if (list_length(whereClauseVars) > 0)
	{
		bool *selectedChunkMask = SelectedChunkMask(stripeSkipList, whereClauseList,
													whereClauseVars, chunkGroupsFiltered);

		selectedChunkSkipList = SelectedChunkSkipList(stripeSkipList, projectedColumnMask,
													 selectedChunkMask);
	}

	/* load column data for projected columns */
	ColumnBuffers **columnBuffersArray = make_array<ColumnBuffers*>(columnCount);

	for (columnIndex = 0; columnIndex < stripeMetadata->columnCount; columnIndex++)
	{
		if (projectedColumnMask[columnIndex])
		{
			ColumnChunkSkipNode *chunkSkipNode =
				selectedChunkSkipList->chunkSkipNodeArray[columnIndex];
			Form_pg_attribute attributeForm = TupleDescAttr(tupleDescriptor, columnIndex);
			uint32_t chunkCount = selectedChunkSkipList->chunkCount;

			ColumnBuffers *columnBuffers = LoadColumnBuffers(relation, chunkSkipNode,
															 chunkCount,
															 stripeMetadata->fileOffset,
															 attributeForm);

			columnBuffersArray[columnIndex] = columnBuffers;
		}
	}

	StripeBuffers *stripeBuffers = make0<StripeBuffers>();
	stripeBuffers->columnCount = columnCount;
	stripeBuffers->chunkCount = selectedChunkSkipList->chunkCount;
	stripeBuffers->rowCount = StripeSkipListRowCount(selectedChunkSkipList);
	stripeBuffers->columnBuffersArray = columnBuffersArray;
	stripeBuffers->selectedChunkGroupRowCounts =
		selectedChunkSkipList->chunkGroupRowCounts;
	stripeBuffers->selectedChunkGroupRowOffset =
		selectedChunkSkipList->chunkGroupRowOffset;
	stripeBuffers->selectedChunkGroupDeletedRows =
		selectedChunkSkipList->chunkGroupDeletedRows;
	

	return stripeBuffers;
}


/*
 * LoadColumnBuffers reads serialized column data from the given file. These
 * column data are laid out as sequential chunks in the file; and chunk positions
 * and lengths are retrieved from the column chunk skip node array.
 */
static ColumnBuffers *
LoadColumnBuffers(Relation relation, ColumnChunkSkipNode *chunkSkipNodeArray,
				  uint32_t chunkCount, uint64_t stripeOffset,
				  Form_pg_attribute attributeForm)
{
	uint32_t chunkIndex = 0;
	ColumnChunkBuffers **chunkBuffersArray =
		make_array<ColumnChunkBuffers*>(chunkCount);

	for (chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++)
	{
		chunkBuffersArray[chunkIndex] = make0<ColumnChunkBuffers>();
	}

	/*
	 * We first read the "exists" chunks. We don't read "values" array here,
	 * because "exists" chunks are stored sequentially on disk, and we want to
	 * minimize disk seeks.
	 */
	for (chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++)
	{
		ColumnChunkSkipNode *chunkSkipNode = &chunkSkipNodeArray[chunkIndex];
		uint64_t existsOffset = stripeOffset + chunkSkipNode->existsChunkOffset;
		StringInfo rawExistsBuffer = makeStringInfo();

		enlargeStringInfo(rawExistsBuffer, chunkSkipNode->existsLength);
		rawExistsBuffer->len = chunkSkipNode->existsLength;
		ColumnarStorageRead(relation, existsOffset, rawExistsBuffer->data,
							chunkSkipNode->existsLength);

		chunkBuffersArray[chunkIndex]->existsBuffer = rawExistsBuffer;
	}

	/* then read "values" chunks, which are also stored sequentially on disk */
	for (chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++)
	{
		ColumnChunkSkipNode *chunkSkipNode = &chunkSkipNodeArray[chunkIndex];
		CompressionType compressionType = chunkSkipNode->valueCompressionType;
		uint64_t valueOffset = stripeOffset + chunkSkipNode->valueChunkOffset;
		StringInfo rawValueBuffer = makeStringInfo();

		enlargeStringInfo(rawValueBuffer, chunkSkipNode->valueLength);
		rawValueBuffer->len = chunkSkipNode->valueLength;
		ColumnarStorageRead(relation, valueOffset, rawValueBuffer->data,
							chunkSkipNode->valueLength);

		chunkBuffersArray[chunkIndex]->valueBuffer = rawValueBuffer;
		chunkBuffersArray[chunkIndex]->valueCompressionType = compressionType;
		chunkBuffersArray[chunkIndex]->decompressedValueSize =
			chunkSkipNode->decompressedValueSize;
	}

	ColumnBuffers *columnBuffers = make0<ColumnBuffers>();
	columnBuffers->chunkBuffersArray = chunkBuffersArray;

	return columnBuffers;
}


/*
 * SelectedChunkMask walks over each column's chunks and checks if a chunk can
 * be filtered without reading its data. The filtering happens when all rows in
 * the chunk can be refuted by the given qualifier conditions.
 */
static bool *
SelectedChunkMask(StripeSkipList *stripeSkipList, List *whereClauseList,
				  List *whereClauseVars, int64 *chunkGroupsFiltered)
{
	uint32_t chunkIndex = 0;

	bool *selectedChunkMask = make_array<bool>(stripeSkipList->chunkCount);
	memset(selectedChunkMask, true, stripeSkipList->chunkCount * sizeof(bool));

	for (auto column : PList<Var>(whereClauseVars))
	{
		if (column->varattno <= 0 ||
			static_cast<uint32_t>(column->varattno) > stripeSkipList->columnCount)
		{
			pg_log(ERROR, "restriction column {} is outside stripe column count {}",
				   column->varattno, stripeSkipList->columnCount);
		}

		uint32_t columnIndex = column->varattno - 1;

		/* if this column's data type doesn't have a comparator, skip it */
		FmgrInfo *comparisonFunction = GetFunctionInfoOrNull(column->vartype,
															 BTREE_AM_OID,
															 BTORDER_PROC);
		if (comparisonFunction == NULL)
		{
			continue;
		}

		Node *baseConstraint = BuildBaseConstraint(column);
		for (chunkIndex = 0; chunkIndex < stripeSkipList->chunkCount; chunkIndex++)
		{
			ColumnChunkSkipNode *chunkSkipNodeArray =
				stripeSkipList->chunkSkipNodeArray[columnIndex];
			ColumnChunkSkipNode *chunkSkipNode = &chunkSkipNodeArray[chunkIndex];

			/*
			 * A column chunk with comparable data type can miss min/max values
			 * if all values in the chunk are NULL.
			 */
			if (!chunkSkipNode->hasMinMax)
			{
				continue;
			}

			UpdateConstraint(baseConstraint, chunkSkipNode->minimumValue,
							 chunkSkipNode->maximumValue);

			auto constraintList = PList<Node>::of(baseConstraint);
			bool predicateRefuted =
				predicate_refuted_by(constraintList, whereClauseList, false);
			if (predicateRefuted && selectedChunkMask[chunkIndex])
			{
				selectedChunkMask[chunkIndex] = false;
				*chunkGroupsFiltered += 1;
			}
		}
	}

	return selectedChunkMask;
}



/*
 * BuildBaseConstraint builds and returns a base constraint. This constraint
 * implements an expression in the form of (var <= max && var >= min), where
 * min and max values represent a chunk's min and max values. These chunk
 * values are filled in after the constraint is built. This function is based
 * on a similar function from CitusDB's shard pruning logic.
 */
static Node *
BuildBaseConstraint(Var *variable)
{
	OpExpr *lessThanExpr = MakeOpExpression(variable, BTLessEqualStrategyNumber);
	OpExpr *greaterThanExpr = MakeOpExpression(variable, BTGreaterEqualStrategyNumber);

	Node *baseConstraint = make_and_qual(reinterpret_cast<Node *>(lessThanExpr), reinterpret_cast<Node *>(greaterThanExpr));

	return baseConstraint;
}


/*
 * GetClauseVars extracts the Vars from the given clauses for the purpose of
 * building constraints that can be refuted by predicate_refuted_by(). It also
 * deduplicates and sorts them.
 */
static List *
GetClauseVars(List *whereClauseList, int natts)
{
	/*
	 * We don't recurse into or include aggregates, window functions, or
	 * PHVs. We don't expect any PHVs during execution; and Vars found inside
	 * an aggregate or window function aren't going to be useful in forming
	 * constraints that can be refuted.
	 */
	int flags = 0;
	List *vars = pull_var_clause(reinterpret_cast<Node *>(whereClauseList), flags);
	Var **deduplicate = make_array<Var *>(natts);

	for (auto node : PList<Node>(vars))
	{
		Assert(IsA(node, Var));

		Var *var = reinterpret_cast<Var *>(node);
		int idx = var->varattno - 1;

		if (deduplicate[idx] != NULL)
		{
			/* if they have the same varattno, the rest should be identical */
			Assert(equal(var, deduplicate[idx]));
		}

		deduplicate[idx] = var;
	}

	PList<Var> whereClauseVars;
	for (int i = 0; i < natts; i++)
	{
		Var *var = deduplicate[i];
		if (var != NULL)
		{
			whereClauseVars.append(var);
		}
	}

	pfree(deduplicate);

	return whereClauseVars;
}


/*
 * MakeOpExpression builds an operator expression node. This operator expression
 * implements the operator clause as defined by the variable and the strategy
 * number. The function is copied from CitusDB's shard pruning logic.
 */
static OpExpr *
MakeOpExpression(Var *variable, int16_t strategyNumber)
{
	Oid typeId = variable->vartype;
	Oid typeModId = variable->vartypmod;
	Oid collationId = variable->varcollid;

	Oid accessMethodId = BTREE_AM_OID;

	/* Load the operator from system catalogs */
	Oid operatorId = GetOperatorByType(typeId, accessMethodId, strategyNumber);

	/* No operator for typeId, search for operator class type */
	if (operatorId == InvalidOid)
	{
		Oid operatorClassId = GetDefaultOpClass(typeId, accessMethodId);
		Oid opcintype = get_opclass_input_type(operatorClassId);
		operatorId = GetOperatorByType(opcintype, accessMethodId, strategyNumber);
	}

	Const *constantValue = makeNullConst(typeId, typeModId, collationId);

	/* Now make the expression with the given variable and a null constant */
	OpExpr *expression = reinterpret_cast<OpExpr *>(make_opclause(operatorId,
												  InvalidOid, /* no result type yet */
												  false, /* no return set */
												  reinterpret_cast<Expr *>(variable),
												  reinterpret_cast<Expr *>(constantValue),
												  InvalidOid, collationId));

	/* Set implementing function id and result type */
	expression->opfuncid = get_opcode(operatorId);
	expression->opresulttype = get_func_rettype(expression->opfuncid);

	return expression;
}


/*
 * GetOperatorByType returns operator Oid for the given type, access method,
 * and strategy number. Note that this function incorrectly errors out when
 * the given type doesn't have its own operator but can use another compatible
 * type's default operator. The function is copied from CitusDB's shard pruning
 * logic.
 */
static Oid
GetOperatorByType(Oid typeId, Oid accessMethodId, int16_t strategyNumber)
{
	/* Get default operator class from pg_opclass */
	Oid operatorClassId = GetDefaultOpClass(typeId, accessMethodId);

	Oid operatorFamily = get_opclass_family(operatorClassId);

	Oid operatorId = get_opfamily_member(operatorFamily, typeId, typeId, strategyNumber);

	return operatorId;
}


/*
 * UpdateConstraint updates the base constraint with the given min/max values.
 * The function is copied from CitusDB's shard pruning logic.
 */
static void
UpdateConstraint(Node *baseConstraint, Datum minValue, Datum maxValue)
{
	BoolExpr *andExpr = reinterpret_cast<BoolExpr *>(baseConstraint);
	PList<Node> argList{andExpr->args};
	Node *lessThanExpr = argList.first();
	Node *greaterThanExpr = argList.second();

	Node *minNode = get_rightop(reinterpret_cast<Expr *>(greaterThanExpr));
	Node *maxNode = get_rightop(reinterpret_cast<Expr *>(lessThanExpr));

	Assert(IsA(minNode, Const));
	Assert(IsA(maxNode, Const));

	Const *minConstant = reinterpret_cast<Const *>(minNode);
	Const *maxConstant = reinterpret_cast<Const *>(maxNode);

	minConstant->constvalue = minValue;
	maxConstant->constvalue = maxValue;

	minConstant->constisnull = false;
	maxConstant->constisnull = false;

	minConstant->constbyval = true;
	maxConstant->constbyval = true;
}


/*
 * SelectedChunkSkipList constructs a new StripeSkipList in which the
 * non-selected chunks are removed from the given stripeSkipList.
 */
static StripeSkipList *
SelectedChunkSkipList(StripeSkipList *stripeSkipList, bool *projectedColumnMask,
					  bool *selectedChunkMask)
{
	uint32_t selectedChunkCount = 0;
	uint32_t chunkIndex = 0;
	uint32_t columnIndex = 0;
	uint32_t columnCount = stripeSkipList->columnCount;
	uint32_t selectedChunkIndex = 0;

	for (chunkIndex = 0; chunkIndex < stripeSkipList->chunkCount; chunkIndex++)
	{
		if (selectedChunkMask[chunkIndex])
		{
			selectedChunkCount++;
		}
	}

	if (selectedChunkCount == stripeSkipList->chunkCount)
	{
		return stripeSkipList;
	}

	ColumnChunkSkipNode **selectedChunkSkipNodeArray =
		make_array<ColumnChunkSkipNode*>(columnCount);

	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		bool firstColumn = columnIndex == 0;
		selectedChunkIndex = 0;

		/* first column's chunk skip node is always read */
		if (!projectedColumnMask[columnIndex] && !firstColumn)
		{
			selectedChunkSkipNodeArray[columnIndex] = NULL;
			continue;
		}

		Assert(stripeSkipList->chunkSkipNodeArray[columnIndex] != NULL);

		selectedChunkSkipNodeArray[columnIndex] = make_array<ColumnChunkSkipNode>(selectedChunkCount);

		for (chunkIndex = 0; chunkIndex < stripeSkipList->chunkCount; chunkIndex++)
		{
			if (selectedChunkMask[chunkIndex])
			{
				selectedChunkSkipNodeArray[columnIndex][selectedChunkIndex] =
					stripeSkipList->chunkSkipNodeArray[columnIndex][chunkIndex];
				selectedChunkIndex++;
			}
		}
	}

	selectedChunkIndex = 0;
	uint32_t *chunkGroupRowCounts = make_array<uint32_t>(selectedChunkCount);
	uint32_t *chunkGroupRowOffset = make_array<uint32_t>(selectedChunkCount);
	uint32_t *chunkGroupDeletedRows = make_array<uint32_t>(selectedChunkCount);

	for (chunkIndex = 0; chunkIndex < stripeSkipList->chunkCount; chunkIndex++)
	{
		if (selectedChunkMask[chunkIndex])
		{
			chunkGroupRowCounts[selectedChunkIndex] =
				stripeSkipList->chunkGroupRowCounts[chunkIndex];
			chunkGroupRowOffset[selectedChunkIndex] =
				stripeSkipList->chunkGroupRowOffset[chunkIndex];
			chunkGroupDeletedRows[selectedChunkIndex] =
				stripeSkipList->chunkGroupDeletedRows[chunkIndex];
			selectedChunkIndex++;
		}
	}

	StripeSkipList *selectedChunkSkipList = make0<StripeSkipList>();
	selectedChunkSkipList->chunkSkipNodeArray = selectedChunkSkipNodeArray;
	selectedChunkSkipList->chunkCount = selectedChunkCount;
	selectedChunkSkipList->columnCount = stripeSkipList->columnCount;
	selectedChunkSkipList->chunkGroupRowCounts = chunkGroupRowCounts;
	selectedChunkSkipList->chunkGroupRowOffset = chunkGroupRowOffset;
	selectedChunkSkipList->chunkGroupDeletedRows = chunkGroupDeletedRows;

	return selectedChunkSkipList;
}


/*
 * StripeSkipListRowCount counts the number of rows in the given stripeSkipList.
 * To do this, the function finds the first column, and sums up row counts across
 * all chunks for that column.
 */
static uint32_t
StripeSkipListRowCount(StripeSkipList *stripeSkipList)
{
	uint32_t stripeSkipListRowCount = 0;
	uint32_t chunkIndex = 0;
	uint32_t *chunkGroupRowCounts = stripeSkipList->chunkGroupRowCounts;

	for (chunkIndex = 0; chunkIndex < stripeSkipList->chunkCount; chunkIndex++)
	{
		uint32_t chunkGroupRowCount = chunkGroupRowCounts[chunkIndex];
		stripeSkipListRowCount += chunkGroupRowCount;
	}

	return stripeSkipListRowCount;
}


/*
 * ProjectedColumnMask returns a boolean array in which the projected columns
 * from the projected column list are marked as true.
 */
static bool *
ProjectedColumnMask(uint32_t columnCount, List *projectedColumnList)
{
	bool *projectedColumnMask = make_array<bool>(columnCount);

	for (auto attno : PIntList{projectedColumnList})
	{
		if (attno <= 0 || static_cast<uint32_t>(attno) > columnCount)
		{
			pg_log(ERROR, "projected column {} is outside column count {}",
				   attno, columnCount);
		}

		/* attno is 1-indexed; projectedColumnMask is 0-indexed */
		int columnIndex = attno - 1;
		projectedColumnMask[columnIndex] = true;
	}

	return projectedColumnMask;
}


/*
 * DeserializeBoolArray reads an array of bits from the given buffer and stores
 * it in provided bool array.
 */
static void
DeserializeBoolArray(StringInfo boolArrayBuffer, bool *boolArray,
					 uint32_t boolArrayLength)
{
	uint32_t boolArrayIndex = 0;

	uint32_t maximumBoolCount = boolArrayBuffer->len * 8;
	if (boolArrayLength > maximumBoolCount)
	{
		pg_log(ERROR, "insufficient data for reading boolean array");
	}

	for (boolArrayIndex = 0; boolArrayIndex < boolArrayLength; boolArrayIndex++)
	{
		uint32_t byteIndex = boolArrayIndex / 8;
		uint32_t bitIndex = boolArrayIndex % 8;
		uint8_t bitmask = (1 << bitIndex);

		uint8_t shiftedBit = (boolArrayBuffer->data[byteIndex] & bitmask);
		if (shiftedBit == 0)
		{
			boolArray[boolArrayIndex] = false;
		}
		else
		{
			boolArray[boolArrayIndex] = true;
		}
	}
}


/*
 * DeserializeDatumArray reads an array of datums from the given buffer and stores
 * them in provided datumArray. If a value is marked as false in the exists array,
 * the function assumes that the datum isn't in the buffer, and simply skips it.
 */
static void
DeserializeDatumArray(StringInfo datumBuffer, bool *existsArray, uint32_t datumCount,
					  bool datumTypeByValue, int datumTypeLength,
					  char datumTypeAlign, Datum *datumArray)
{
	uint32_t datumIndex = 0;
	uint32_t currentDatumDataOffset = 0;

	for (datumIndex = 0; datumIndex < datumCount; datumIndex++)
	{
		if (!existsArray[datumIndex])
		{
			continue;
		}

		char *currentDatumDataPointer = datumBuffer->data + currentDatumDataOffset;

		datumArray[datumIndex] = fetch_att(currentDatumDataPointer, datumTypeByValue,
										   datumTypeLength);
		currentDatumDataOffset = att_addlength_datum(currentDatumDataOffset,
													 datumTypeLength,
													 datumArray[datumIndex]);
		currentDatumDataOffset = att_align_nominal(currentDatumDataOffset,
												   datumTypeAlign);

		if (currentDatumDataOffset > datumBuffer->len)
		{
			pg_log(ERROR, "insufficient data left in datum buffer: {}, {}", currentDatumDataOffset, datumBuffer->len);
		}
	}
}


/*
 * DeserializeChunkGroupData deserializes requested data chunk for all columns and
 * stores in chunkDataArray. It uncompresses serialized data if necessary. The
 * function also deallocates data buffers used for previous chunk, and compressed
 * data buffers for the current chunk which will not be needed again. If a column
 * data is not present serialized buffer, then default value (or null) is used
 * to fill value array.
 */
static ChunkData *
DeserializeChunkData(StripeBuffers *stripeBuffers, uint64_t chunkIndex,
					 uint32_t rowCount, TupleDesc tupleDescriptor,
					 List *projectedColumnList, StripeReadState *state, uint64_t stripeId)
{
	int columnIndex = 0;
	bool *columnMask = ProjectedColumnMask(tupleDescriptor->natts, projectedColumnList);


	ChunkData *chunkData = CreateEmptyChunkData(tupleDescriptor->natts, columnMask,
												rowCount);

	for (columnIndex = 0; columnIndex < stripeBuffers->columnCount; columnIndex++)
	{
		Form_pg_attribute attributeForm = TupleDescAttr(tupleDescriptor, columnIndex);
		ColumnBuffers *columnBuffers = stripeBuffers->columnBuffersArray[columnIndex];
		bool columnAdded = false;

		if (columnBuffers == NULL && columnMask[columnIndex])
		{
			columnAdded = true;
		}


		if (columnBuffers != NULL)
		{
			ColumnChunkBuffers *chunkBuffers =
				columnBuffers->chunkBuffersArray[chunkIndex];
			bool shouldCache = bioseq_enable_page_cache == true && chunkBuffers->valueCompressionType != COMPRESSION_NONE;

			if (shouldCache)
			{
				ColumnarMarkChunkGroupInUse(state->relation->rd_id, stripeId, chunkIndex);
			}

			/* decompress and deserialize current chunk's data */
			StringInfo valueBuffer = NULL;
			// DOC0276 — does THIS chunk read OWN `valueBuffer` (a fresh
			// decompressed/cached copy the free path may pfree), or is it the
			// SHARED raw stripe buffer (owned by LoadColumnBuffers, alive for the
			// whole stripe read)? `DecompressBuffer` returns the raw buffer as-is
			// for COMPRESSION_NONE; the legacy code then repalloc'd it and stored
			// it for `FreeChunkBufferValueArray` to pfree after the chunk read —
			// which moves and then frees the shared raw buffer, leaving
			// `chunkBuffers->valueBuffer` dangling. Any later RE-READ of the same
			// chunk group then dereferences freed memory → SIGSEGV. This is
			// invisible to a single forward pass (each chunk read once) but fatal
			// the moment a reader revisits a chunk group: the segmented recompute
			// path (DOC0276) does so via its overlap (backward chunk-group
			// transitions at segment boundaries), and any random-access reader can
			// too. So for the NONE / non-cached case use the raw buffer in place
			// and never hand it to the free path.
			bool valueBufferOwned = true;

			if (shouldCache)
			{
				valueBuffer = static_cast<StringInfo>(ColumnarRetrieveCache(state->relation->rd_id, stripeId, chunkIndex, columnIndex));
			}

			if (valueBuffer == NULL)
			{
				if (chunkBuffers->valueCompressionType == COMPRESSION_NONE && !shouldCache)
				{
					/* Shared raw stripe buffer — use in place; do NOT repalloc or free it. */
					valueBuffer = chunkBuffers->valueBuffer;
					valueBufferOwned = false;
				}
				else
				{
					MemoryContext oldMemoryContext;
					if (shouldCache)
					{
						oldMemoryContext = MemoryContextSwitchTo(ColumnarCacheMemoryContext());
					}

					valueBuffer = DecompressBuffer(chunkBuffers->valueBuffer,
									 chunkBuffers->valueCompressionType,
									 chunkBuffers->decompressedValueSize);

					valueBuffer = static_cast<StringInfo>(repalloc(valueBuffer, sizeof(cached_StringInfo)));
					reinterpret_cast<cached_StringInfo *>(valueBuffer)->cached = false;

					if (shouldCache)
					{

						reinterpret_cast<cached_StringInfo *>(valueBuffer)->cached = true;

						ColumnarAddCacheEntry(state->relation->rd_id, stripeId, chunkIndex, columnIndex, valueBuffer);
						MemoryContextSwitchTo(oldMemoryContext);
					}
				}
			}

			DeserializeBoolArray(chunkBuffers->existsBuffer,
								 chunkData->existsArray[columnIndex],
								 rowCount);
			DeserializeDatumArray(valueBuffer, chunkData->existsArray[columnIndex],
								  rowCount, attributeForm->attbyval,
								  attributeForm->attlen, attributeForm->attalign,
								  chunkData->valueArray[columnIndex]);

			/* store current chunk's data buffer to be freed at next chunk read —
			 * NEVER the shared raw stripe buffer (see valueBufferOwned above). */
			chunkData->valueBufferArray[columnIndex] = valueBufferOwned ? valueBuffer : NULL;
		}
		else if (columnAdded)
		{
			/*
			 * This is a column that was added after creation of this stripe.
			 * So we use either the default value or NULL.
			 */
			if (attributeForm->atthasdef)
			{
				int rowIndex = 0;

				Datum defaultValue = ColumnDefaultValue(tupleDescriptor->constr,
														attributeForm);

				for (rowIndex = 0; rowIndex < rowCount; rowIndex++)
				{
					chunkData->existsArray[columnIndex][rowIndex] = true;
					chunkData->valueArray[columnIndex][rowIndex] = defaultValue;
				}
			}
			else
			{
				memset(chunkData->existsArray[columnIndex], false,
					   rowCount * sizeof(bool));
			}
		}
	}

	return chunkData;
}


/*
 * ColumnDefaultValue returns default value for given column. Only const values
 * are supported. The function errors on any other default value expressions.
 */
static Datum
ColumnDefaultValue(TupleConstr *tupleConstraints, Form_pg_attribute attributeForm)
{
	Node *defaultValueNode = NULL;
	int defValIndex = 0;

	for (defValIndex = 0; defValIndex < tupleConstraints->num_defval; defValIndex++)
	{
		AttrDefault attrDefault = tupleConstraints->defval[defValIndex];
		if (attrDefault.adnum == attributeForm->attnum)
		{
			defaultValueNode = static_cast<Node*>(stringToNode(attrDefault.adbin));
			break;
		}
	}

	Assert(defaultValueNode != NULL);

	/* try reducing the default value node to a const node */
	defaultValueNode = eval_const_expressions(NULL, defaultValueNode);
	if (IsA(defaultValueNode, Const))
	{
		Const *constNode = reinterpret_cast<Const *>(defaultValueNode);
		return constNode->constvalue;
	}
	else
	{
		const char *columnName = NameStr(attributeForm->attname);
		PgReport(ERROR).msg("unsupported default value for column \"{}\"", columnName)
					   .hint("Expression is either mutable or "
							 "does not evaluate to constant value");
	}
}

/* Vectorization */

bool
ColumnarReadNextVector(ColumnarReadState *readState,  Datum *columnValues,
					   bool *columnNulls, uint64 *rowNumber, int *newVectorSize)
{
	while (true)
	{
		if (!StripeReadInProgress(readState))
		{
			if (!HasUnreadStripe(readState))
			{
				return false;
			}

			readState->stripeReadState = BeginStripeRead(readState->currentStripeMetadata,
														 readState->relation,
														 readState->tupleDescriptor,
														 readState->projectedColumnList,
														 readState->whereClauseList,
														 readState->whereClauseVars,
														 readState->stripeReadContext,
														 readState->snapshot);
		}

		if (!ReadStripeNextVector(readState->stripeReadState, columnValues, columnNulls, 
								  newVectorSize,
								  readState->currentStripeMetadata->id,
								  readState->snapshot,
								  rowNumber,
								  readState->currentStripeMetadata->firstRowNumber))
		{
			AdvanceStripeRead(readState);
			
			// We have collected rows from last stripe, break loop
			if (*newVectorSize)
				return true;
			
			continue;
		}

		return true;
	}

	return false;
}


static bool
ReadStripeNextVector(StripeReadState *stripeReadState, Datum *columnValues,
					 bool *columnNulls, int *newVectorSize,
					 uint64 stripeId,
					 Snapshot snapshot,
					 uint64 *rowNumber,
					 uint64 stripeFirstRowNumber)
{
	if (stripeReadState->currentRow >= stripeReadState->rowCount)
	{
		Assert(stripeReadState->currentRow == stripeReadState->rowCount);
		return false;
	}

	int32_t *columnValueOffset = make_array<int32_t>(stripeReadState->tupleDescriptor->natts);
	memset(columnValueOffset, 0, sizeof(int32_t) * stripeReadState->tupleDescriptor->natts);

	while (true)
	{
		uint64_t chunkFirstRowNumber = 0;

		if (stripeReadState->chunkGroupReadState == NULL)
		{
			stripeReadState->chunkGroupReadState = BeginChunkGroupRead(
				stripeReadState->stripeBuffers,
				stripeReadState->
				chunkGroupIndex,
				stripeReadState->
				tupleDescriptor,
				stripeReadState->
				projectedColumnList,
				stripeReadState->
				stripeReadContext,
				stripeReadState,
				stripeId);

			chunkFirstRowNumber = stripeFirstRowNumber +
								  stripeReadState->chunkGroupReadState->chunkStripeRowOffset;

			if (bioseq_enable_dml &&
				stripeReadState->chunkGroupReadState->chunkGroupDeletedRows != 0)
			{
				stripeReadState->chunkGroupReadState->rowMask =
					ReadChunkRowMask(stripeReadState->relation->rd_locator,
									 snapshot,
									 stripeReadState->stripeReadContext,
									 chunkFirstRowNumber,
									 stripeReadState->chunkGroupReadState->rowCount);
			}
			else
			{
				stripeReadState->chunkGroupReadState->rowMask = NULL;
			}
		}
		else
		{
			chunkFirstRowNumber = stripeFirstRowNumber +
						stripeReadState->chunkGroupReadState->chunkStripeRowOffset;
		}

		if (!ReadChunkGroupNextVector(stripeReadState->chunkGroupReadState,
									  columnValues, columnNulls, 
									  stripeReadState->tupleDescriptor,
									  columnValueOffset, 
									  newVectorSize,
									  rowNumber,
									  chunkFirstRowNumber))
		{
			/*
			 * if *newVectorSize is non-zero, it means there might be some data in
			 * chunkGroupReadState->chunkGroupData referenced by VectorColumn.  In
			 * this case, do NOT release ChunkGroupReadState.
			 */
			if (*newVectorSize == 0)
			{
				/* if this chunk group is exhausted, fetch the next one and loop */
				EndChunkGroupRead(stripeReadState->chunkGroupReadState);
				stripeReadState->chunkGroupReadState = NULL;
				stripeReadState->chunkGroupIndex++;
				continue;
			}
		}
		else
			stripeReadState->currentRow += stripeReadState->chunkGroupReadState->rowCount;

		pfree(columnValueOffset);
		return true;
	}

	pfree(columnValueOffset);
	return false;
}


static bool
ReadChunkGroupNextVector(ChunkGroupReadState *chunkGroupReadState, Datum *columnValues,
						 bool *columnNulls, TupleDesc tupleDesc, 
						 int32_t *columnValueOffset, int *chunkReadRows,
						 uint64 *rowNumber,
						 uint64 stripeFirstRowNumber)
{
	if (chunkGroupReadState->currentRow >= chunkGroupReadState->rowCount)
	{
		Assert(chunkGroupReadState->currentRow == chunkGroupReadState->rowCount);
		return false;
	}

	/*
	 * Initialize to all-NULL. Only non-NULL projected attributes will be set.
	 */
	memset(columnNulls, true, sizeof(bool) * chunkGroupReadState->columnCount);

	int i;
	int rowNumberIndex = 0;

	for (i = 0; i < chunkGroupReadState->rowCount; i ++)
	{
		if (chunkGroupReadState->currentRow >= chunkGroupReadState->rowCount)
			return false;
		
		if (*chunkReadRows >= COLUMNAR_VECTOR_COLUMN_SIZE)
			break;

		if (chunkGroupReadState->rowMask != NULL)
		{
			int8_t checkColumnMask = 1 << (chunkGroupReadState->currentRow % 8);
			int8_t checkLookupMask = VARDATA(chunkGroupReadState->rowMask)[chunkGroupReadState->currentRow / 8];

			if (checkLookupMask & checkColumnMask)
			{
				chunkGroupReadState->currentRow++;
				continue;
			}
		}

		for (auto attno : PIntList{chunkGroupReadState->projectedColumnList})
		{
			const ChunkData *chunkGroupData = chunkGroupReadState->chunkGroupData;
			const int rowIndex = chunkGroupReadState->currentRow;

			/* attno is 1-indexed; existsArray is 0-indexed */
			const uint32_t columnIndex = attno - 1;

			VectorColumn* vectorColumn = reinterpret_cast<VectorColumn*>(columnValues[columnIndex]);

			if (chunkGroupData->existsArray[columnIndex][rowIndex])
			{
				int8_t *writeColumnRowPosition =
					reinterpret_cast<int8_t *>(vectorColumn->value) + columnValueOffset[columnIndex];


				/*
				 * For data types which have len less or equal sizeof(Datum) we can
				 * use `store_att_byval` function.
				 */
				if (vectorColumn->columnTypeLen <= sizeof(Datum))
				{
					store_att_byval(writeColumnRowPosition,
									chunkGroupData->valueArray[columnIndex][rowIndex],
									vectorColumn->columnTypeLen);
				}
				else
				{
					memcpy(writeColumnRowPosition,
						   reinterpret_cast<int8_t *>(chunkGroupData->valueArray[columnIndex][rowIndex]),
						   vectorColumn->columnTypeLen);
				}

				vectorColumn->isnull[vectorColumn->dimension] = false;
			}

			vectorColumn->dimension++;
			columnValueOffset[columnIndex] += vectorColumn->columnTypeLen;
		}

		(*chunkReadRows)++;
		chunkGroupReadState->currentRow++;
		rowNumber[rowNumberIndex++] = stripeFirstRowNumber + chunkGroupReadState->currentRow - 1;
	}

	return true;
}
