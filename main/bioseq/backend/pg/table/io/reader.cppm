/*-------------------------------------------------------------------------
 *
 * table/io/columnar_reader.cppm
 *
 * Reader side of the bioseq columnar table format. Owns the
 * BioseqColumnReadState lifecycle: locating stripes via the metadata
 * tables, decompressing chunk groups, applying chunk-group filter
 * pushdowns, and yielding tuples (or vectorised slots) to the
 * executor. Pairs with columnar_writer.cppm.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include "safe_lib.h"

#include <cstdint>
#include <limits>

export module bioseq.table.io:reader;

import bioseq.common.mlogging;
import bioseq.vector.types;
import bioseq.storage.metadata;
import bioseq.pg_utils;
import bioseq.pg_support;
import bioseq.config;                  // DOC0322 §9.3 — bioseq::settings::enable_dml, bioseq::settings::enable_column_cache GUCs
import :cache;
import :pruning;
import :read_chunk;

/*
 * fmt-style format string used by bioseq_log_pg() when a stripe is unflushed
 * yet the read path tries to consume it. Kept as a `const char*` rather
 * than an inline string so call sites use the same wording.
 */
static constexpr const char* UNEXPECTED_STRIPE_READ_ERR_MSG =
	"attempted to read an unexpected stripe while reading columnar "
	"table {}, stripe with id={} is not flushed";


export int64_t
BioseqColumnReadChunkGroupsFiltered(BioseqColumnReadState *state);
export bool
BioseqColumnSetStripeReadState(BioseqColumnReadState *readState,
						   StripeMetadata *startStripeMetadata);
export bool
BioseqColumnReadNextRow(BioseqColumnReadState *readState, Datum *columnValues, bool *columnNulls,
					uint64_t *rowNumber);
export BioseqColumnReadState *
BioseqColumnBeginRead(Relation relation, TupleDesc tupleDescriptor,
				  List *projectedColumnList, List *whereClauseList,
				  MemoryContext scanContext, Snapshot snapshot,
				  bool randomAccess,
				  ParallelBioseqColumnScan parallelBioseqColumnScan);
export bool
BioseqColumnReadNextVector(BioseqColumnReadState *readState,  Datum *columnValues,
					   bool *columnNulls, uint64 *rowNumber, int *newVectorSize);
export void
BioseqColumnRescan(BioseqColumnReadState *readState, List *scanQual);
export void
BioseqColumnEndRead(BioseqColumnReadState *readState);
export void
BioseqColumnReadRowByRowNumberOrError(BioseqColumnReadState *readState,
								  uint64_t rowNumber, Datum *columnValues,
								  bool *columnNulls);
export void
BioseqColumnReadFlushPendingWrites(BioseqColumnReadState *readState);
export bool
BioseqColumnReadRowByRowNumber(BioseqColumnReadState *readState,
						   uint64_t rowNumber, Datum *columnValues,
						   bool *columnNulls);
void
BioseqColumnResetRead(BioseqColumnReadState *readState);


/* static function declarations */
static MemoryContext CreateStripeReadMemoryContext(void);
static bool BioseqColumnReadIsCurrentStripe(BioseqColumnReadState *readState,
										uint64_t rowNumber);
static StripeMetadata * BioseqColumnReadGetCurrentStripe(BioseqColumnReadState *readState);
static bool ReadStripeRowByRowNumber(BioseqColumnReadState *readState,
									 uint64_t rowNumber, Datum *columnValues,
									 bool *columnNulls);
static bool StripeReadIsCurrentChunkGroup(StripeReadState *stripeReadState,
										  int chunkGroupIndex);
static bool ReadChunkGroupRowByRowOffset(ChunkGroupReadState *chunkGroupReadState,
										 StripeMetadata *stripeMetadata,
										 uint64_t stripeRowOffset, Datum *columnValues,
										 bool *columnNulls);
static bool StripeReadInProgress(BioseqColumnReadState *readState);
static bool HasUnreadStripe(BioseqColumnReadState *readState);
static StripeReadState * BeginStripeRead(StripeMetadata *stripeMetadata, Relation rel,
										 TupleDesc tupleDesc, List *projectedColumnList,
										 List *whereClauseList, List *whereClauseVars,
										 MemoryContext stripeReadContext,
										 Snapshot snapshot);
static void AdvanceStripeRead(BioseqColumnReadState *readState);
static bool SnapshotMightSeeUnflushedStripes(Snapshot snapshot);
static bool ReadStripeNextRow(StripeReadState *stripeReadState, Datum *columnValues,
							  bool *columnNulls,
							  uint64_t stripeFirstRowNumber,
							  Snapshot snapshot, uint64_t stripeId);

/* Vectorization */
static bool ReadStripeNextVector(StripeReadState *stripeReadState, Datum *columnValues,
								 bool *columnNulls, int *newVectorSize,
								 uint64 stripeId,
								 Snapshot snapshot,
								 uint64 *rowNumber,
								 uint64 stripeFirstRowNumber);

/*
 * BioseqColumnBeginRead initializes a columnar read operation. This function returns a
 * read handle that's used during reading rows and finishing the read operation.
 *
 * projectedColumnList is an integer list of attribute numbers (1-indexed).
 */
BioseqColumnReadState *
BioseqColumnBeginRead(Relation relation, TupleDesc tupleDescriptor,
				  List *projectedColumnList, List *whereClauseList,
				  MemoryContext scanContext, Snapshot snapshot,
				  bool randomAccess,
				  ParallelBioseqColumnScan parallelBioseqColumnScan)
{
	/*
	 * We allocate all stripe specific data in the stripeReadContext, and reset
	 * this memory context before loading a new stripe. This is to avoid memory
	 * leaks.
	 */
	MemoryContext stripeReadContext = CreateStripeReadMemoryContext();

	BioseqColumnReadState *readState = make0<BioseqColumnReadState>();
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
	 * Note that BioseqColumnReadFlushPendingWrites might update those two by
	 * registering a new snapshot.
	 */
	readState->snapshot = snapshot;
	readState->snapshotRegisteredByUs = false;

	/* Parallel execution */
	readState->parallelBioseqColumnScan = parallelBioseqColumnScan;

	if (!randomAccess)
	{
		/*
		 * If have parallel columnar scan we don't flush states.
		 */
		if (readState->parallelBioseqColumnScan == NULL)
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
			BioseqColumnReadFlushPendingWrites(readState);
		}

		/*
		 * AdvanceStripeRead sets currentStripeMetadata for the first stripe
		 * to read if not doing random access. Otherwise, reader (i.e.:
		 * BioseqColumnReadRowByRowNumber) would already decide the stripe to read
		 * on-the-fly.
		 *
		 * Moreover, Since we don't flush pending writes for random access,
		 * AdvanceStripeRead might encounter with stripe metadata entries due
		 * to current transaction's pending writes even when using an MVCC
		 * snapshot, but AdvanceStripeRead would throw an error for that.
		 * Note that this is not the case with for plain table scan methods
		 * (i.e.: SeqScan and BioseqColumn CustomScan).
		 *
		 * For those reasons, we don't call AdvanceStripeRead if we will do
		 * random access.
		 */
		AdvanceStripeRead(readState);
	}

	return readState;
}


/*
 * BioseqColumnReadFlushPendingWrites flushes pending writes for read operation
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
BioseqColumnReadFlushPendingWrites(BioseqColumnReadState *readState)
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
 * BioseqColumnReadNextRow tries to read a row from the columnar table. On success, it sets
 * column values, column nulls and rowNumber (if passed to be non-NULL), and returns true.
 * If there are no more rows to read, the function returns false.
 */
bool
BioseqColumnReadNextRow(BioseqColumnReadState *readState, Datum *columnValues, bool *columnNulls,
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
}


/*
 * BioseqColumnReadRowByRowNumberOrError is a wrapper around
 * BioseqColumnReadRowByRowNumber that throws an error if tuple
 * with rowNumber does not exist.
 */
void
BioseqColumnReadRowByRowNumberOrError(BioseqColumnReadState *readState,
								  uint64_t rowNumber, Datum *columnValues,
								  bool *columnNulls)
{
	if (!BioseqColumnReadRowByRowNumber(readState, rowNumber,
									columnValues, columnNulls))
	{
		bioseq_log<ERR, PG>( "cannot read from columnar table {}, tuple with "
					  "row number {} does not exist",
					  RelationGetRelationName(readState->relation),
					  rowNumber);
	}
}


/*
 * BioseqColumnReadRowByRowNumber reads row with rowNumber from given relation
 * into columnValues and columnNulls, and returns true. If no such row
 * exists, then returns false.
 */
bool
BioseqColumnReadRowByRowNumber(BioseqColumnReadState *readState,
						   uint64_t rowNumber, Datum *columnValues,
						   bool *columnNulls)
{
	if (!BioseqColumnReadIsCurrentStripe(readState, rowNumber))
	{
		Relation bioseqColumnRelation = readState->relation;
		Snapshot snapshot = readState->snapshot;
		StripeMetadata *stripeMetadata = FindStripeByRowNumber(bioseqColumnRelation,
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
			bioseq_log<ERR, PG>( UNEXPECTED_STRIPE_READ_ERR_MSG,
				   RelationGetRelationName(bioseqColumnRelation),
				   stripeMetadata->id);
		}

		/* do the cleanup before reading a new stripe */
		BioseqColumnResetRead(readState);

		TupleDesc relationTupleDesc = RelationGetDescr(bioseqColumnRelation);
		List *whereClauseList = NIL;
		List *whereClauseVars = NIL;
		MemoryContext stripeReadContext = readState->stripeReadContext;
		readState->stripeReadState = BeginStripeRead(stripeMetadata,
													 bioseqColumnRelation,
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
 * BioseqColumnSetStripeReadState 
 */
bool
BioseqColumnSetStripeReadState(BioseqColumnReadState *readState,
						   StripeMetadata *startStripeMetadata)
{
	if (!BioseqColumnReadIsCurrentStripe(readState, startStripeMetadata->firstRowNumber))
	{
		Relation bioseqColumnRelation = readState->relation;
		Snapshot snapshot = readState->snapshot;
		StripeMetadata *stripeMetadata = 
			FindStripeByRowNumber(bioseqColumnRelation,
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
			bioseq_log<ERR, PG>( UNEXPECTED_STRIPE_READ_ERR_MSG,
				   RelationGetRelationName(bioseqColumnRelation),
				   stripeMetadata->id);
		}

		/* do the cleanup before reading a new stripe */
		BioseqColumnResetRead(readState);

		TupleDesc relationTupleDesc = RelationGetDescr(bioseqColumnRelation);
		List *whereClauseList = NIL;
		List *whereClauseVars = NIL;
		MemoryContext stripeReadContext = readState->stripeReadContext;
		readState->stripeReadState = BeginStripeRead(stripeMetadata,
													 bioseqColumnRelation,
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
 * BioseqColumnReadIsCurrentStripe returns true if stripe being read contains
 * row with given rowNumber.
 */
static bool
BioseqColumnReadIsCurrentStripe(BioseqColumnReadState *readState, uint64_t rowNumber)
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
 * BioseqColumnReadGetCurrentStripe returns StripeMetadata for the stripe that is
 * being read.
 */
static StripeMetadata *
BioseqColumnReadGetCurrentStripe(BioseqColumnReadState *readState)
{
	return readState->currentStripeMetadata;
}


/*
 * ReadStripeRowByRowNumber reads row with rowNumber from given
 * stripeReadState into columnValues and columnNulls.
 * Errors out if no such row exists in the stripe being read.
 */
static bool
ReadStripeRowByRowNumber(BioseqColumnReadState *readState,
						 uint64_t rowNumber, Datum *columnValues,
						 bool *columnNulls)
{
	StripeMetadata *stripeMetadata = BioseqColumnReadGetCurrentStripe(readState);
	StripeReadState *stripeReadState = readState->stripeReadState;

	if (rowNumber < stripeMetadata->firstRowNumber)
	{
		/* not expected but be on the safe side */
		bioseq_log<ERR, PG>( "row offset cannot be negative");
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

		if (bioseq::settings::enable_dml)
		{
			row_mask_row *rowMaskEntry =
				RowMaskFindWriteState(stripeReadState->relation->rd_locator.relNumber,
									  GetCurrentSubTransactionId(), rowNumber);
			if (rowMaskEntry != NULL)
			{
				Assert(rowMaskEntry->mask.has_value());
				stripeReadState->chunkGroupReadState->rowMask =
					*rowMaskEntry->mask;
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
StripeReadInProgress(BioseqColumnReadState *readState)
{
	return readState->stripeReadState != NULL;
}


/*
 * HasUnreadStripe returns true if we still have stripes to read during current
 * read operation.
 */
static bool
HasUnreadStripe(BioseqColumnReadState *readState)
{
	return readState->currentStripeMetadata != NULL;
}


/*
 * BioseqColumnRescan clears the position where we were scanning so that the next read starts at
 * the beginning again
 */
void
BioseqColumnRescan(BioseqColumnReadState *readState, List *scanQual)
{
	bioseq::pg_support::MemoryContextScope ctx_guard{readState->scanContext};

	BioseqColumnResetRead(readState);

	/* set currentStripeMetadata for the first stripe to read */
	AdvanceStripeRead(readState);

	readState->chunkGroupsFiltered = 0;

	readState->whereClauseList = copyObject(scanQual);
}


/*
 * Finishes a columnar read operation.
 */
void
BioseqColumnEndRead(BioseqColumnReadState *readState)
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

	pfree(readState);
}


/*
 * BioseqColumnResetRead resets the stripe and the chunk group that is
 * being read currently (if any).
 */
void
BioseqColumnResetRead(BioseqColumnReadState *readState)
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
	bioseq::pg_support::MemoryContextScope ctx_guard{stripeReadContext};

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
AdvanceStripeRead(BioseqColumnReadState *readState)
{
	bioseq::pg_support::MemoryContextScope ctx_guard{readState->scanContext};

	if (readState->parallelBioseqColumnScan == 0)
	{
		/* if not read any stripes yet, start from the first one .. */
		uint64_t lastReadRowNumber = BIOSEQ_COLUMN_INVALID_ROW_NUMBER;
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

		auto *parallel_scan = readState->parallelBioseqColumnScan;
		for (;;)
		{
			/* Stripe ids are relation-local, and aborted reservations may leave
			 * gaps.  Resolve the next relation-local id without holding a
			 * spinlock: the catalog/index scan is interruptible, and a parallel
			 * worker terminated while holding a spinlock would poison the DSM
			 * lock for every peer. */
			uint64 claimed_id = pg_atomic_read_u64(&parallel_scan->nextStripeId);
			constexpr uint64 max_stripe_id = static_cast<uint64>(
				std::numeric_limits<std::int64_t>::max());
			constexpr uint64 eof_sentinel = max_stripe_id + 1u;
			if (claimed_id == eof_sentinel)
			{
				readState->currentStripeMetadata = nullptr;
				break;
			}
			if (claimed_id == 0u || claimed_id > max_stripe_id)
			{
				bioseq_log<ERR, PG>(
					"bioseq parallel scan cursor is outside the stripe-id domain");
			}

			std::uint64_t lookup_found_id = claimed_id;
			StripeMetadata *candidate = FindNextStripeForParallelWorker(
				readState->relation, readState->snapshot, claimed_id,
				&lookup_found_id);

			if (candidate == nullptr)
			{
				readState->currentStripeMetadata = nullptr;
				break;
			}

			if (lookup_found_id < claimed_id
				|| lookup_found_id > max_stripe_id)
			{
				pfree(candidate);
				bioseq_log<ERR, PG>(
					"bioseq parallel scan returned an invalid stripe id");
			}

			/* CAS makes the lookup and gap skip one logical claim.  A peer
			 * that raced on the same candidate retries from the winner's
			 * successor, so no stripe is duplicated. */
			const uint64 found_id = static_cast<uint64>(lookup_found_id);
			const uint64 successor = found_id + 1;
			uint64 expected = claimed_id;
			if (pg_atomic_compare_exchange_u64(
					&parallel_scan->nextStripeId, &expected, successor))
			{
				readState->currentStripeMetadata = candidate;
				break;
			}

			if (candidate != nullptr)
			{
				pfree(candidate);
			}
		}
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
		bioseq_log<ERR, PG>( UNEXPECTED_STRIPE_READ_ERR_MSG,
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
			
			if (bioseq::settings::enable_dml &&
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
}


/*
 * BioseqColumnReadChunkGroupsFiltered
 *
 * Return the number of chunk groups filtered during this read operation.
 */
int64_t
BioseqColumnReadChunkGroupsFiltered(BioseqColumnReadState *state)
{
	return state->chunkGroupsFiltered;
}

	/* Vectorization */

bool
BioseqColumnReadNextVector(BioseqColumnReadState *readState,  Datum *columnValues,
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

			if (bioseq::settings::enable_dml &&
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
}
