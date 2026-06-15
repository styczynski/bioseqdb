/*-------------------------------------------------------------------------
 *
 * table/io/buffers.cppm
 *
 * Shared table-IO allocation helpers for ChunkData buffers. Reader and
 * writer both use these helpers; keeping them in a leaf partition prevents
 * writer from reaching into reader by declaration.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

export module bioseq_table_io:buffers;

import storage_metadata;
import pg_utils;

/*
 * CreateEmptyChunkData creates data buffers to keep deserialized exist and
 * value arrays for requested columns in columnMask.
 */
export ChunkData*
CreateEmptyChunkData(uint32_t columnCount, bool* columnMask, uint32_t chunkGroupRowCount)
{
	uint32_t columnIndex = 0;

	ChunkData* chunkData = make0<ChunkData>();
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
	return reinterpret_cast<cached_StringInfo*>(info)->cached;
}

/* FreeChunkValueArrayBuffer releases valueBufferArray memory. */
export void
FreeChunkBufferValueArray(ChunkData* chunkData)
{
	uint32_t columnIndex = 0;

	if (chunkData == NULL)
	{
		return;
	}

	for (columnIndex = 0; columnIndex < chunkData->columnCount; columnIndex++)
	{
		if (chunkData->valueBufferArray[columnIndex] != NULL &&
			!CachedStringInfo(chunkData->valueBufferArray[columnIndex]))
		{
			pfree(chunkData->valueBufferArray[columnIndex]->data);
			pfree(chunkData->valueBufferArray[columnIndex]);
		}
	}
}

/*
 * FreeChunkData deallocates exist/value arrays for requested columns. The
 * serialized value buffers live in the read/write context and are released by
 * FreeChunkBufferValueArray when they are not cached.
 */
export void
FreeChunkData(ChunkData* chunkData)
{
	uint32_t columnIndex = 0;

	if (chunkData == NULL)
	{
		return;
	}

	for (columnIndex = 0; columnIndex < chunkData->columnCount; columnIndex++)
	{
		if (chunkData->existsArray[columnIndex] != NULL)
		{
			pfree(chunkData->existsArray[columnIndex]);
		}

		if (chunkData->valueArray[columnIndex] != NULL)
		{
			pfree(chunkData->valueArray[columnIndex]);
		}
	}

	pfree(chunkData->existsArray);
	pfree(chunkData->valueArray);
	pfree(chunkData);
}
