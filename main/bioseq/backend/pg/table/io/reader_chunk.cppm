/*-------------------------------------------------------------------------
 *
 * table/io/reader_chunk.cppm
 *
 * Chunk-group loading, deserialization, and per-chunk row materialisation for
 * the bioseq table reader.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

export module bioseq.table.io:read_chunk;

import bioseq.common.mlogging;
import bioseq.vector.types;
import bioseq.storage.compression;
import bioseq.storage.physical;
import bioseq.storage.metadata;
import bioseq.pg_utils;
import bioseq.pg_support;
import bioseq.config;
import :buffers;
import :cache;
import :pruning;
import bioseq.pg_utils.nodes;   // DOC0333 — pg_node_try_as<Const>

using namespace bioseq::pg_utils;   // DOC0333 — node view helpers live in bioseq::pg_utils

static void
DeserializeBoolArray(StringInfo boolArrayBuffer, bool *boolArray,
					 uint32_t boolArrayLength)
{
	uint32_t boolArrayIndex = 0;

	uint32_t maximumBoolCount = boolArrayBuffer->len * 8;
	if (boolArrayLength > maximumBoolCount)
	{
		bioseq_log<ERR, PG>("insufficient data for reading boolean array");
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
			bioseq_log<ERR, PG>("insufficient data left in datum buffer: {}, {}",
								currentDatumDataOffset, datumBuffer->len);
		}
	}
}

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
	if (Const *constNode = pg_node_try_as<Const>(defaultValueNode))
	{
		return constNode->constvalue;
	}
	else
	{
		const char *columnName = NameStr(attributeForm->attname);
		bioseq_report<ERR, PG>().msg("unsupported default value for column \"{}\"", columnName)
					   .hint("Expression is either mutable or "
							 "does not evaluate to constant value");
	}
}

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
		BioseqColumnStorageRead(relation, existsOffset, rawExistsBuffer->data,
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
		BioseqColumnStorageRead(relation, valueOffset, rawValueBuffer->data,
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

export StripeBuffers *
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
			bool shouldCache = bioseq::settings::enable_column_cache == true &&
				chunkBuffers->valueCompressionType != COMPRESSION_NONE;

			if (shouldCache)
			{
				BioseqColumnMarkChunkGroupInUse(state->relation->rd_id, stripeId, chunkIndex);
			}

			/* decompress and deserialize current chunk's data */
			StringInfo valueBuffer = NULL;
			bool valueBufferOwned = true;

			if (shouldCache)
			{
				valueBuffer = static_cast<StringInfo>(BioseqColumnRetrieveCache(
					state->relation->rd_id, stripeId, chunkIndex, columnIndex));
			}

			if (valueBuffer == NULL)
			{
				if (chunkBuffers->valueCompressionType == COMPRESSION_NONE && !shouldCache)
				{
					/* Shared raw stripe buffer: use in place and leave owner unchanged. */
					valueBuffer = chunkBuffers->valueBuffer;
					valueBufferOwned = false;
				}
				else
				{
					MemoryContext oldMemoryContext;
					if (shouldCache)
					{
						oldMemoryContext = MemoryContextSwitchTo(BioseqColumnCacheMemoryContext());
					}

					valueBuffer = DecompressBuffer(chunkBuffers->valueBuffer,
									 chunkBuffers->valueCompressionType,
									 chunkBuffers->decompressedValueSize);

					valueBuffer = static_cast<StringInfo>(repalloc(valueBuffer, sizeof(cached_StringInfo)));
					reinterpret_cast<cached_StringInfo *>(valueBuffer)->cached = false;

					if (shouldCache)
					{
						reinterpret_cast<cached_StringInfo *>(valueBuffer)->cached = true;

						BioseqColumnAddCacheEntry(state->relation->rd_id, stripeId, chunkIndex,
												  columnIndex, valueBuffer);
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

			/* Never pass the shared raw stripe buffer to the chunk free path. */
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

export ChunkGroupReadState *
BeginChunkGroupRead(StripeBuffers *stripeBuffers, int chunkIndex, TupleDesc tupleDesc,
					List *projectedColumnList, MemoryContext cxt, StripeReadState *state,
					uint64_t stripeId)
{
	if (chunkIndex < 0 || static_cast<uint32_t>(chunkIndex) >= stripeBuffers->chunkCount)
	{
		bioseq_log<ERR, PG>("chunk group index {} is outside selected chunk count {}",
			   chunkIndex, stripeBuffers->chunkCount);
	}

	uint32_t chunkGroupRowCount =
		stripeBuffers->selectedChunkGroupRowCounts[chunkIndex];
	uint32_t chunkGroupRowOffset =
		stripeBuffers->selectedChunkGroupRowOffset[chunkIndex];
	uint32_t chunkGroupDeletedRows =
		stripeBuffers->selectedChunkGroupDeletedRows[chunkIndex];

	bioseq::pg_support::MemoryContextScope ctx_guard{cxt};
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

export void
EndChunkGroupRead(ChunkGroupReadState *chunkGroupReadState)
{
	FreeChunkBufferValueArray(chunkGroupReadState->chunkGroupData);
	FreeChunkData(chunkGroupReadState->chunkGroupData);
	if (chunkGroupReadState->rowMask != NULL && !chunkGroupReadState->rowMaskCached)
		pfree(chunkGroupReadState->rowMask);
	chunkGroupReadState->rowMask = NULL;
	pfree(chunkGroupReadState);
}

export bool
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
			int8_t checkLookupMask =
				VARDATA(chunkGroupReadState->rowMask)[chunkGroupReadState->currentRow / 8 ];

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

export bool
ReadChunkGroupNextVector(ChunkGroupReadState *chunkGroupReadState, Datum *columnValues,
						 bool *columnNulls, TupleDesc tupleDesc,
						 int32_t *columnValueOffset, int *chunkReadRows,
						 uint64 *rowNumber,
						 uint64 stripeFirstRowNumber)
{
	(void) tupleDesc;
	(void) columnValueOffset;

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

		if (*chunkReadRows >= BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE)
			break;

		if (chunkGroupReadState->rowMask != NULL)
		{
			int8_t checkColumnMask = 1 << (chunkGroupReadState->currentRow % 8);
			int8_t checkLookupMask =
				VARDATA(chunkGroupReadState->rowMask)[chunkGroupReadState->currentRow / 8];

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
			const bool is_null =
				!chunkGroupData->existsArray[columnIndex][rowIndex];
			WriteDatumToVectorColumn(
				vectorColumn,
				is_null
					? static_cast<Datum>(0)
					: chunkGroupData->valueArray[columnIndex][rowIndex],
				is_null,
				vectorColumn->dimension);
		}

		(*chunkReadRows)++;
		chunkGroupReadState->currentRow++;
		rowNumber[rowNumberIndex++] = stripeFirstRowNumber + chunkGroupReadState->currentRow - 1;
	}

	return true;
}
