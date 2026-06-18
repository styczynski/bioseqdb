/*-------------------------------------------------------------------------
 *
 * table/io/columnar_writer.cppm
 *
 * Writer side of the bioseq columnar table format. Owns the
 * BioseqColumnWriteState lifecycle: in-memory chunk accumulation, chunk
 * compression, stripe assembly, file-level metadata writes (skip nodes,
 * chunk-group summaries) and the WAL-emitting flush path. The reader
 * side lives in columnar_reader.cppm.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include "safe_lib.h"

export module bioseq.table.io:writer;

import bioseq.common.mlogging;
import bioseq.storage.compression;
import bioseq.storage.physical;
import bioseq.storage.metadata;   // DOC0322 §9.2 — the column-store data-model structs (was types.h)
import bioseq.config;             // DOC0322 §9.2 — CompressionType + COMPRESSION_* (moved to config)
import bioseq.pg_utils;
import :buffers;

export void BioseqColumnFlushPendingWrites(BioseqColumnWriteState *state);


static StripeBuffers * CreateEmptyStripeBuffers(uint32_t stripeMaxRowCount,
												uint32_t chunkRowCount,
												uint32_t columnCount);
static StripeSkipList * CreateEmptyStripeSkipList(uint32_t stripeMaxRowCount,
												  uint32_t chunkRowCount,
												  uint32_t columnCount);
static void FlushStripe(BioseqColumnWriteState *writeState);
static StringInfo SerializeBoolArray(bool *boolArray, uint32_t boolArrayLength);
static void SerializeChunkData(BioseqColumnWriteState *writeState, uint32_t chunkIndex,
							   uint32_t rowCount);
static void UpdateChunkSkipNodeMinMax(ColumnChunkSkipNode *chunkSkipNode,
									  Datum columnValue, bool columnTypeByValue,
									  int columnTypeLength, Oid columnCollation,
									  FmgrInfo *comparisonFunction);
static StringInfo CopyStringInfo(StringInfo sourceString);

/*
 * BioseqColumnBeginWrite initializes a columnar data load operation and returns a table
 * handle. This handle should be used for adding the row values and finishing the
 * data load operation.
 */
export BioseqColumnWriteState *
BioseqColumnBeginWrite(RelFileLocator relfilelocator,
				   BioseqColumnOptions options,
				   TupleDesc tupleDescriptor)
{
	/* get comparison function pointers for each of the columns */
	uint32_t columnCount = tupleDescriptor->natts;
	FmgrInfo **comparisonFunctionArray = make_array<FmgrInfo*>(columnCount);
	for (uint32_t columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		FmgrInfo *comparisonFunction = NULL;
		FormData_pg_attribute *attributeForm = TupleDescAttr(tupleDescriptor,
															 columnIndex);

		if (!attributeForm->attisdropped)
		{
			Oid typeId = attributeForm->atttypid;

			comparisonFunction = GetFunctionInfoOrNull(typeId, BTREE_AM_OID,
													   BTORDER_PROC);
		}

		comparisonFunctionArray[columnIndex] = comparisonFunction;
	}

	/*
	 * We allocate all stripe specific data in the stripeWriteContext, and
	 * reset this memory context once we have flushed the stripe to the file.
	 * This is to avoid memory leaks.
	 */
	MemoryContext stripeWriteContext = AllocSetContextCreate(CurrentMemoryContext,
															 "Stripe Write Memory Context",
															 ALLOCSET_DEFAULT_SIZES);

	bool *columnMaskArray = make_array<bool>(columnCount);
	memset(columnMaskArray, true, columnCount * sizeof(bool));

	ChunkData *chunkData = CreateEmptyChunkData(columnCount, columnMaskArray,
												options.chunkRowCount);

	BioseqColumnWriteState *writeState = make0<BioseqColumnWriteState>();
	writeState->relfilelocator = relfilelocator;
	writeState->options = options;
	writeState->tupleDescriptor = CreateTupleDescCopy(tupleDescriptor);
	writeState->comparisonFunctionArray = comparisonFunctionArray;
	writeState->stripeBuffers = NULL;
	writeState->stripeSkipList = NULL;
	writeState->emptyStripeReservation = NULL;
	writeState->stripeWriteContext = stripeWriteContext;
	writeState->chunkData = chunkData;
	writeState->compressionBuffer = NULL;
	writeState->perTupleContext = AllocSetContextCreate(CurrentMemoryContext,
														"BioseqColumn per tuple context",
														ALLOCSET_DEFAULT_SIZES);

	return writeState;
}

/*
 * BioseqColumnWriteRow adds a row to the columnar table. If the stripe is not initialized,
 * we create structures to hold stripe data and skip list. Then, we serialize and
 * append data to serialized value buffer for each of the columns and update
 * corresponding skip nodes. Then, whole chunk data is compressed at every
 * rowChunkCount insertion. Then, if row count exceeds stripeMaxRowCount, we flush
 * the stripe, and add its metadata to the table footer.
 *
 * Returns the "row number" assigned to written row.
 */
export uint64_t
BioseqColumnWriteRow(BioseqColumnWriteState *writeState, Datum *columnValues, bool *columnNulls)
{
	uint32_t columnIndex = 0;
	StripeBuffers *stripeBuffers = writeState->stripeBuffers;
	StripeSkipList *stripeSkipList = writeState->stripeSkipList;
	uint32_t columnCount = writeState->tupleDescriptor->natts;
	BioseqColumnOptions *options = &writeState->options;
	const uint32_t chunkRowCount = options->chunkRowCount;
	ChunkData *chunkData = writeState->chunkData;
	MemoryContextScope ctx_guard{writeState->stripeWriteContext};

	uint32_t chunkIndex;
	uint32_t chunkRowIndex;

	if (stripeBuffers)
	{
		chunkIndex = stripeBuffers->rowCount / chunkRowCount;
		chunkRowIndex = stripeBuffers->rowCount % chunkRowCount;
		/*
		* For each column, we first need to check to see if the next row will fit
		* inside the chunk buffer.  If it does not fit, then we need to serialize
		* the stripe and make a new stripe for insertion.
		*/
		bool fits = true;

		for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
		{

			/* Check for nulls, skip if null. */
			if (columnNulls[columnIndex])
			{
				continue;
			}

			Form_pg_attribute attributeForm =
				TupleDescAttr(writeState->tupleDescriptor, columnIndex);

			int columnTypeLength = attributeForm->attlen;
			char columnTypeAlign = attributeForm->attalign;

			uint32_t datumLength = att_addlength_datum(0, columnTypeLength, columnValues[columnIndex]);
			uint32_t datumLengthAligned = att_align_nominal(datumLength, columnTypeAlign);

			/* Check to see if we are within the 1 gigabyte value. */
			if (static_cast<long>(chunkData->valueBufferArray[columnIndex]->len) + static_cast<long>(datumLengthAligned) > 1024000000)
			{
				fits = false;
				break;
			}
		}

		if (!fits)
		{
			/* Flush the stripe. */
			BioseqColumnFlushPendingWrites(writeState);

			/* Then set up for new stripeBuffers. */
			stripeBuffers = NULL;

			chunkData->rowCount = 0;
		}
	}

	if (stripeBuffers == NULL)
	{
		stripeBuffers = CreateEmptyStripeBuffers(options->stripeRowCount,
												 chunkRowCount, columnCount);
		stripeSkipList = CreateEmptyStripeSkipList(options->stripeRowCount,
												   chunkRowCount, columnCount);
		writeState->stripeBuffers = stripeBuffers;
		writeState->stripeSkipList = stripeSkipList;
		writeState->compressionBuffer = makeStringInfo();

		Oid relationId = RelidByRelfilenumber(writeState->relfilelocator.spcOid,
											  writeState->relfilelocator.relNumber);
		Relation relation = safe_relation_open(relationId, NoLock);
		writeState->emptyStripeReservation =
			ReserveEmptyStripe(relation, columnCount, chunkRowCount,
							   options->stripeRowCount);
		relation_close(relation, NoLock);

		/*
		 * serializedValueBuffer lives in stripe write memory context so it needs to be
		 * initialized when the stripe is created.
		 */
		for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
		{
			cached_StringInfo *value = make0<cached_StringInfo>();
			initStringInfo(&value->data);
			value->cached = false;
			chunkData->valueBufferArray[columnIndex] = reinterpret_cast<StringInfo>(value);
		}
	}

	chunkIndex = stripeBuffers->rowCount / chunkRowCount;
	chunkRowIndex = stripeBuffers->rowCount % chunkRowCount;

	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		ColumnChunkSkipNode **chunkSkipNodeArray = stripeSkipList->chunkSkipNodeArray;
		ColumnChunkSkipNode *chunkSkipNode =
			&chunkSkipNodeArray[columnIndex][chunkIndex];

		if (columnNulls[columnIndex])
		{
			chunkData->existsArray[columnIndex][chunkRowIndex] = false;
		}
		else
		{
			FmgrInfo *comparisonFunction =
				writeState->comparisonFunctionArray[columnIndex];
			Form_pg_attribute attributeForm =
				TupleDescAttr(writeState->tupleDescriptor, columnIndex);
			bool columnTypeByValue = attributeForm->attbyval;
			int columnTypeLength = attributeForm->attlen;
			Oid columnCollation = attributeForm->attcollation;
			char columnTypeAlign = attributeForm->attalign;

			chunkData->existsArray[columnIndex][chunkRowIndex] = true;

			uint32_t datumLength = att_addlength_datum(0, columnTypeLength, columnValues[columnIndex]);
			uint32_t datumLengthAligned = att_align_nominal(datumLength, columnTypeAlign);

			if (datumLengthAligned >= (256UL << 20))
			{
				bioseq_log<ERR, PG>( "Error with insert on column \'{}\'. Inserting {} bytes, exceeding 256MB",
							 attributeForm->attname.data, datumLength);
			}

			bioseq::pg_utils::pg_datum::from_raw(columnValues[columnIndex])
				.append_serialized(chunkData->valueBufferArray[columnIndex],
								   columnTypeByValue, columnTypeLength,
								   columnTypeAlign);

			UpdateChunkSkipNodeMinMax(chunkSkipNode, columnValues[columnIndex],
									  columnTypeByValue, columnTypeLength,
									  columnCollation, comparisonFunction);
		}

		chunkSkipNode->rowCount++;
	}

	stripeSkipList->chunkCount = chunkIndex + 1;

	/* last row of the chunk is inserted serialize the chunk */
	if (chunkRowIndex == chunkRowCount - 1)
	{
		SerializeChunkData(writeState, chunkIndex, chunkRowCount);
	}

	uint64_t writtenRowNumber = writeState->emptyStripeReservation->stripeFirstRowNumber +
							  stripeBuffers->rowCount;
	stripeBuffers->rowCount++;
	if (stripeBuffers->rowCount >= options->stripeRowCount)
	{
		BioseqColumnFlushPendingWrites(writeState);
	}

	return writtenRowNumber;
}


/*
 * BioseqColumnEndWrite finishes a columnar data load operation. If we have an unflushed
 * stripe, we flush it.
 */
export void
BioseqColumnEndWrite(BioseqColumnWriteState *writeState)
{
	BioseqColumnFlushPendingWrites(writeState);

	MemoryContextDelete(writeState->stripeWriteContext);
	pfree(writeState->comparisonFunctionArray);
	FreeChunkData(writeState->chunkData);
	pfree(writeState);
}


void
BioseqColumnFlushPendingWrites(BioseqColumnWriteState *writeState)
{
	StripeBuffers *stripeBuffers = writeState->stripeBuffers;
	if (stripeBuffers != NULL)
	{
		MemoryContextScope ctx_guard{writeState->stripeWriteContext};

		FlushStripe(writeState);
		MemoryContextReset(writeState->stripeWriteContext);

		/* set stripe data and skip list to NULL so they are recreated next time */
		writeState->stripeBuffers = NULL;
		writeState->stripeSkipList = NULL;
	}
}


/*
 * BioseqColumnWritePerTupleContext
 *
 * Return per-tuple context for columnar write operation.
 */
export MemoryContext
BioseqColumnWritePerTupleContext(BioseqColumnWriteState *state)
{
	return state->perTupleContext;
}


/*
 * CreateEmptyStripeBuffers allocates an empty StripeBuffers structure with the given
 * column count.
 */
static StripeBuffers *
CreateEmptyStripeBuffers(uint32_t stripeMaxRowCount, uint32_t chunkRowCount,
						 uint32_t columnCount)
{
	uint32_t columnIndex = 0;
	uint32_t maxChunkCount = (stripeMaxRowCount / chunkRowCount) + 1;
	ColumnBuffers **columnBuffersArray = make_array<ColumnBuffers*>(columnCount);

	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		uint32_t chunkIndex = 0;
		ColumnChunkBuffers **chunkBuffersArray =
			make_array<ColumnChunkBuffers*>(maxChunkCount);

		for (chunkIndex = 0; chunkIndex < maxChunkCount; chunkIndex++)
		{
			chunkBuffersArray[chunkIndex] = make0<ColumnChunkBuffers>();
			chunkBuffersArray[chunkIndex]->existsBuffer = NULL;
			chunkBuffersArray[chunkIndex]->valueBuffer = NULL;
			chunkBuffersArray[chunkIndex]->valueCompressionType = COMPRESSION_NONE;
		}

		columnBuffersArray[columnIndex] = make0<ColumnBuffers>();
		columnBuffersArray[columnIndex]->chunkBuffersArray = chunkBuffersArray;
	}

	StripeBuffers *stripeBuffers = make0<StripeBuffers>();
	stripeBuffers->columnBuffersArray = columnBuffersArray;
	stripeBuffers->columnCount = columnCount;
	stripeBuffers->rowCount = 0;

	return stripeBuffers;
}


/*
 * CreateEmptyStripeSkipList allocates an empty StripeSkipList structure with
 * the given column count. This structure has enough chunks to hold statistics
 * for stripeMaxRowCount rows.
 */
static StripeSkipList *
CreateEmptyStripeSkipList(uint32_t stripeMaxRowCount, uint32_t chunkRowCount,
						  uint32_t columnCount)
{
	uint32_t columnIndex = 0;
	uint32_t maxChunkCount = (stripeMaxRowCount / chunkRowCount) + 1;

	ColumnChunkSkipNode **chunkSkipNodeArray =
		make_array<ColumnChunkSkipNode*>(columnCount);
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		chunkSkipNodeArray[columnIndex] =
			make_array<ColumnChunkSkipNode>(maxChunkCount);
	}

	StripeSkipList *stripeSkipList = make0<StripeSkipList>();
	stripeSkipList->columnCount = columnCount;
	stripeSkipList->chunkCount = 0;
	stripeSkipList->chunkSkipNodeArray = chunkSkipNodeArray;

	return stripeSkipList;
}


/*
 * FlushStripe flushes current stripe data into the file. The function first ensures
 * the last data chunk for each column is properly serialized and compressed. Then,
 * the function creates the skip list and footer buffers. Finally, the function
 * flushes the skip list, data, and footer buffers to the file.
 */
static void
FlushStripe(BioseqColumnWriteState *writeState)
{
	uint32_t columnIndex = 0;
	uint32_t chunkIndex = 0;
	StripeBuffers *stripeBuffers = writeState->stripeBuffers;
	StripeSkipList *stripeSkipList = writeState->stripeSkipList;
	ColumnChunkSkipNode **columnSkipNodeArray = stripeSkipList->chunkSkipNodeArray;
	TupleDesc tupleDescriptor = writeState->tupleDescriptor;
	uint32_t columnCount = tupleDescriptor->natts;
	uint32_t chunkCount = stripeSkipList->chunkCount;
	uint32_t chunkRowCount = writeState->options.chunkRowCount;
	uint32_t lastChunkIndex = stripeBuffers->rowCount / chunkRowCount;
	uint32_t lastChunkRowCount = stripeBuffers->rowCount % chunkRowCount;
	uint64_t stripeSize = 0;
	uint64_t stripeRowCount = stripeBuffers->rowCount;

	bioseq_log_pg(DEBUG1, "Flushing Stripe of size {}", stripeBuffers->rowCount);

	Oid relationId = RelidByRelfilenumber(writeState->relfilelocator.spcOid,
										  writeState->relfilelocator.relNumber);

	Relation relation = safe_relation_open(relationId, NoLock);

	/*
	 * check if the last chunk needs serialization , the last chunk was not serialized
	 * if it was not full yet, e.g.  (rowCount > 0)
	 */
	if (lastChunkRowCount > 0)
	{
		SerializeChunkData(writeState, lastChunkIndex, lastChunkRowCount);
	}

	/* update buffer sizes in stripe skip list */
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		ColumnChunkSkipNode *chunkSkipNodeArray = columnSkipNodeArray[columnIndex];
		ColumnBuffers *columnBuffers = stripeBuffers->columnBuffersArray[columnIndex];

		for (chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++)
		{
			ColumnChunkBuffers *chunkBuffers =
				columnBuffers->chunkBuffersArray[chunkIndex];
			uint64_t existsBufferSize = chunkBuffers->existsBuffer->len;
			ColumnChunkSkipNode *chunkSkipNode = &chunkSkipNodeArray[chunkIndex];

			chunkSkipNode->existsChunkOffset = stripeSize;
			chunkSkipNode->existsLength = existsBufferSize;
			stripeSize += existsBufferSize;
		}

		for (chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++)
		{
			ColumnChunkBuffers *chunkBuffers =
				columnBuffers->chunkBuffersArray[chunkIndex];
			uint64_t valueBufferSize = chunkBuffers->valueBuffer->len;
			CompressionType valueCompressionType = chunkBuffers->valueCompressionType;
			ColumnChunkSkipNode *chunkSkipNode = &chunkSkipNodeArray[chunkIndex];

			chunkSkipNode->valueChunkOffset = stripeSize;
			chunkSkipNode->valueLength = valueBufferSize;
			chunkSkipNode->valueCompressionType = valueCompressionType;
			chunkSkipNode->valueCompressionLevel = writeState->options.compressionLevel;
			chunkSkipNode->decompressedValueSize = chunkBuffers->decompressedValueSize;

			stripeSize += valueBufferSize;
		}
	}

	StripeMetadata *stripeMetadata =
		CompleteStripeReservation(relation, writeState->emptyStripeReservation->stripeId,
								  stripeSize, stripeRowCount, chunkCount);

	uint64_t currentFileOffset = stripeMetadata->fileOffset;

	/*
	 * Each stripe has only one section:
	 * Data section, in which we store data for each column continuously.
	 * We store data for each for each column in chunks. For each chunk, we
	 * store two buffers: "exists" buffer, and "value" buffer. "exists" buffer
	 * tells which values are not NULL. "value" buffer contains values for
	 * present values. For each column, we first store all "exists" buffers,
	 * and then all "value" buffers.
	 */

	/* flush the data buffers */
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		ColumnBuffers *columnBuffers = stripeBuffers->columnBuffersArray[columnIndex];

		for (chunkIndex = 0; chunkIndex < stripeSkipList->chunkCount; chunkIndex++)
		{
			ColumnChunkBuffers *chunkBuffers =
				columnBuffers->chunkBuffersArray[chunkIndex];
			StringInfo existsBuffer = chunkBuffers->existsBuffer;

			BioseqColumnStorageWrite(relation, currentFileOffset,
								 existsBuffer->data, existsBuffer->len);
			currentFileOffset += existsBuffer->len;
		}

		for (chunkIndex = 0; chunkIndex < stripeSkipList->chunkCount; chunkIndex++)
		{
			ColumnChunkBuffers *chunkBuffers =
				columnBuffers->chunkBuffersArray[chunkIndex];
			StringInfo valueBuffer = chunkBuffers->valueBuffer;

			BioseqColumnStorageWrite(relation, currentFileOffset,
								 valueBuffer->data, valueBuffer->len);
			currentFileOffset += valueBuffer->len;
		}
	}

	SaveChunkGroups(writeState->relfilelocator,
					stripeMetadata->id,
					writeState->chunkGroupRowCounts);
	SaveStripeSkipList(writeState->relfilelocator,
					   stripeMetadata->id,
					   stripeSkipList, tupleDescriptor);
	SaveEmptyRowMask(LookupStorageId(writeState->relfilelocator),
					 stripeMetadata->id,
					 stripeMetadata->firstRowNumber,
					 writeState->chunkGroupRowCounts);

	writeState->chunkGroupRowCounts = NIL;

	relation_close(relation, NoLock);
}


/*
 * SerializeBoolArray serializes the given boolean array and returns the result
 * as a StringInfo. This function packs every 8 boolean values into one byte.
 */
static StringInfo
SerializeBoolArray(bool *boolArray, uint32_t boolArrayLength)
{
	uint32_t boolArrayIndex = 0;
	uint32_t byteCount = ((boolArrayLength * sizeof(bool)) + (8 - sizeof(bool))) / 8;

	StringInfo boolArrayBuffer = makeStringInfo();
	enlargeStringInfo(boolArrayBuffer, byteCount);
	boolArrayBuffer->len = byteCount;
	memset(boolArrayBuffer->data, 0, byteCount);

	for (boolArrayIndex = 0; boolArrayIndex < boolArrayLength; boolArrayIndex++)
	{
		if (boolArray[boolArrayIndex])
		{
			uint32_t byteIndex = boolArrayIndex / 8;
			uint32_t bitIndex = boolArrayIndex % 8;
			boolArrayBuffer->data[byteIndex] |= (1 << bitIndex);
		}
	}

	return boolArrayBuffer;
}


/*
 * SerializeChunkData serializes and compresses chunk data at given chunk index with given
 * compression type for every column.
 */
static void
SerializeChunkData(BioseqColumnWriteState *writeState, uint32_t chunkIndex, uint32_t rowCount)
{
	uint32_t columnIndex = 0;
	StripeBuffers *stripeBuffers = writeState->stripeBuffers;
	ChunkData *chunkData = writeState->chunkData;
	CompressionType requestedCompressionType = writeState->options.compressionType;
	int compressionLevel = writeState->options.compressionLevel;
	const uint32_t columnCount = stripeBuffers->columnCount;
	StringInfo compressionBuffer = writeState->compressionBuffer;

	writeState->chunkGroupRowCounts =
		lappend_int(writeState->chunkGroupRowCounts, rowCount);

	/* serialize exist values, data values are already serialized */
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		ColumnBuffers *columnBuffers = stripeBuffers->columnBuffersArray[columnIndex];
		ColumnChunkBuffers *chunkBuffers = columnBuffers->chunkBuffersArray[chunkIndex];

		chunkBuffers->existsBuffer =
			SerializeBoolArray(chunkData->existsArray[columnIndex], rowCount);
	}

	/*
	 * check and compress value buffers, if a value buffer is not compressable
	 * then keep it as uncompressed, store compression information.
	 */
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		ColumnBuffers *columnBuffers = stripeBuffers->columnBuffersArray[columnIndex];
		ColumnChunkBuffers *chunkBuffers = columnBuffers->chunkBuffersArray[chunkIndex];
		CompressionType actualCompressionType = COMPRESSION_NONE;

		StringInfo serializedValueBuffer = chunkData->valueBufferArray[columnIndex];

		Assert(requestedCompressionType >= 0 &&
			   requestedCompressionType < COMPRESSION_COUNT);

		chunkBuffers->decompressedValueSize =
			chunkData->valueBufferArray[columnIndex]->len;

		/*
		 * if serializedValueBuffer is be compressed, update serializedValueBuffer
		 * with compressed data and store compression type.
		 */
		bool compressed = CompressBuffer(serializedValueBuffer, compressionBuffer,
										 requestedCompressionType,
										 compressionLevel);

		if (compressed)
		{
			serializedValueBuffer = compressionBuffer;
			actualCompressionType = requestedCompressionType;
		}

		/* store (compressed) value buffer */
		chunkBuffers->valueCompressionType = actualCompressionType;
		chunkBuffers->valueBuffer = CopyStringInfo(serializedValueBuffer);

		/* valueBuffer needs to be reset for next chunk's data */
		resetStringInfo(chunkData->valueBufferArray[columnIndex]);
	}
}


/*
 * UpdateChunkSkipNodeMinMax takes the given column value, and checks if this
 * value falls outside the range of minimum/maximum values of the given column
 * chunk skip node. If it does, the function updates the column chunk skip node
 * accordingly.
 */
static void
UpdateChunkSkipNodeMinMax(ColumnChunkSkipNode *chunkSkipNode, Datum columnValue,
						  bool columnTypeByValue, int columnTypeLength,
						  Oid columnCollation, FmgrInfo *comparisonFunction)
{
	bool hasMinMax = chunkSkipNode->hasMinMax;
	Datum previousMinimum = chunkSkipNode->minimumValue;
	Datum previousMaximum = chunkSkipNode->maximumValue;
	Datum currentMinimum = 0;
	Datum currentMaximum = 0;

	/* if type doesn't have a comparison function, skip min/max values */
	if (comparisonFunction == NULL)
	{
		return;
	}

	if (!hasMinMax)
	{
		currentMinimum = bioseq::pg_utils::pg_datum::from_raw(columnValue)
			.copy(columnTypeByValue, columnTypeLength).raw();
		currentMaximum = bioseq::pg_utils::pg_datum::from_raw(columnValue)
			.copy(columnTypeByValue, columnTypeLength).raw();
	}
	else
	{
		Datum minimumComparisonDatum = FunctionCall2Coll(comparisonFunction,
														 columnCollation, columnValue,
														 previousMinimum);
		Datum maximumComparisonDatum = FunctionCall2Coll(comparisonFunction,
														 columnCollation, columnValue,
														 previousMaximum);
		int minimumComparison = DatumGetInt32(minimumComparisonDatum);
		int maximumComparison = DatumGetInt32(maximumComparisonDatum);

		if (minimumComparison < 0)
		{
			if (!columnTypeByValue)
				pfree(DatumGetPointer(previousMinimum));

			currentMinimum = bioseq::pg_utils::pg_datum::from_raw(columnValue)
				.copy(columnTypeByValue, columnTypeLength).raw();
		}
		else
		{
			currentMinimum = previousMinimum;
		}

		if (maximumComparison > 0)
		{
			if (!columnTypeByValue)
				pfree(DatumGetPointer(previousMaximum));

			currentMaximum = bioseq::pg_utils::pg_datum::from_raw(columnValue)
				.copy(columnTypeByValue, columnTypeLength).raw();
		}
		else
		{
			currentMaximum = previousMaximum;
		}
	}

	chunkSkipNode->hasMinMax = true;
	chunkSkipNode->minimumValue = currentMinimum;
	chunkSkipNode->maximumValue = currentMaximum;
}


/*
 * CopyStringInfo creates a deep copy of given source string allocating only needed
 * amount of memory.
 */
static StringInfo
CopyStringInfo(StringInfo sourceString)
{
	StringInfo targetString = make0<StringInfoData>();

	if (sourceString->len > 0)
	{
		targetString->data = make1<char>(sourceString->len);
		targetString->len = sourceString->len;
		targetString->maxlen = sourceString->len;
		memcpy(targetString->data, sourceString->data, sourceString->len);
	}

	return targetString;
}


export bool
ContainsPendingWrites(BioseqColumnWriteState *state)
{
	return state->stripeBuffers != NULL && state->stripeBuffers->rowCount != 0;
}
