#pragma once

#include "common.h"
#include "config/config.h"



/* Enumaration for columnar table's compression method */
typedef enum
{
	COMPRESSION_TYPE_INVALID = -1,
	COMPRESSION_NONE = 0,
	COMPRESSION_PG_LZ = 1,
	COMPRESSION_LZ4 = 2,
	COMPRESSION_ZSTD = 3,

	COMPRESSION_COUNT
} CompressionType;

/*
 * ColumnarOptions holds the option values to be used when reading or writing
 * a columnar table. To resolve these values, we first check foreign table's options,
 * and if not present, we then fall back to the default values specified above.
 */
typedef struct ColumnarOptions
{
	uint64 stripeRowCount;
	uint32 chunkRowCount;
	CompressionType compressionType;
	int compressionLevel;
} ColumnarOptions;


/* ColumnChunkSkipNode contains statistics for a ColumnChunkData. */
typedef struct ColumnChunkSkipNode
{
	/* statistics about values of a column chunk */
	bool hasMinMax;
	Datum minimumValue;
	Datum maximumValue;
	uint64 rowCount;

	/*
	 * Offsets and sizes of value and exists streams in the column data.
	 * These enable us to skip reading suppressed row chunks, and start reading
	 * a chunk without reading previous chunks.
	 */
	uint64 valueChunkOffset;
	uint64 valueLength;
	uint64 existsChunkOffset;
	uint64 existsLength;

	/*
	 * This is used for (1) determining destination size when decompressing,
	 * (2) calculating compression rates when logging stats.
	 */
	uint64 decompressedValueSize;

	CompressionType valueCompressionType;
	int valueCompressionLevel;
} ColumnChunkSkipNode;


/*
 * StripeSkipList can be used for skipping row chunks. It contains a column chunk
 * skip node for each chunk of each column. chunkSkipNodeArray[column][chunk]
 * is the entry for the specified column chunk.
 */
typedef struct StripeSkipList
{
	ColumnChunkSkipNode **chunkSkipNodeArray;
	uint32 *chunkGroupRowCounts;
	uint32 *chunkGroupRowOffset;
	uint32 *chunkGroupDeletedRows;
	uint32 columnCount;
	uint32 chunkCount;
} StripeSkipList;


typedef struct cached_StringInfo
{
	StringInfoData data;
	bool cached;
} cached_StringInfo;

/*
 * ChunkData represents a chunk of data for multiple columns. valueArray stores
 * the values of data, and existsArray stores whether a value is present.
 * valueBuffer is used to store (uncompressed) serialized values
 * referenced by Datum's in valueArray. It is only used for by-reference Datum's.
 * There is a one-to-one correspondence between valueArray and existsArray.
 */
typedef struct ChunkData
{
	uint32 rowCount;
	uint32 columnCount;

	/*
	 * Following are indexed by [column][row]. If a column is not projected,
	 * then existsArray[column] and valueArray[column] are NULL.
	 */
	bool **existsArray;
	Datum **valueArray;

	/* valueBuffer keeps actual data for type-by-reference datums from valueArray. */
	StringInfo *valueBufferArray;
} ChunkData;


/*
 * ColumnChunkBuffers represents a chunk of serialized data in a column.
 * valueBuffer stores the serialized values of data, and existsBuffer stores
 * serialized value of presence information. valueCompressionType contains
 * compression type if valueBuffer is compressed. Finally rowCount has
 * the number of rows in this chunk.
 */
typedef struct ColumnChunkBuffers
{
	StringInfo existsBuffer;
	StringInfo valueBuffer;
	CompressionType valueCompressionType;
	uint64 decompressedValueSize;
} ColumnChunkBuffers;


/*
 * ColumnBuffers represents data buffers for a column in a row stripe. Each
 * column is made of multiple column chunks.
 */
typedef struct ColumnBuffers
{
	ColumnChunkBuffers **chunkBuffersArray;
} ColumnBuffers;


/* StripeBuffers represents data for a row stripe. */
typedef struct StripeBuffers
{
	uint32 columnCount;
	uint32 chunkCount;
	uint32 rowCount;
	ColumnBuffers **columnBuffersArray;

	uint32 *selectedChunkGroupRowCounts;
	uint32 *selectedChunkGroupRowOffset;
	uint32 *selectedChunkGroupDeletedRows;
} StripeBuffers;


/* return value of StripeWriteState to decide stripe write state */
typedef enum StripeWriteStateEnum
{
	/* stripe write is flushed to disk, so it's readable */
	STRIPE_WRITE_FLUSHED,

	/*
	 * Writer transaction did abort either before inserting into
	 * bioseq.stripe or after.
	 */
	STRIPE_WRITE_ABORTED,

	/*
	 * Writer transaction is still in-progress. Note that it is not certain
	 * if it is being written by current backend's current transaction or
	 * another backend.
	 */
	STRIPE_WRITE_IN_PROGRESS
} StripeWriteStateEnum;


/* Parallel Custom Scan shared data */
typedef struct ParallelColumnarScanData
{
	slock_t mutex;
	pg_atomic_uint64 nextStripeId;	/* Fetch next stripe id to be read and increment */
	char snapshotData[FLEXIBLE_ARRAY_MEMBER];
} ParallelColumnarScanData;
typedef struct ParallelColumnarScanData *ParallelColumnarScan;


typedef bool (*ColumnarSupportsIndexAM_type)(char *);
typedef const char *(*CompressionTypeStr_type)(CompressionType);
typedef bool (*IsColumnarTableAmTable_type)(Oid);
typedef bool (*ReadColumnarOptions_type)(Oid, ColumnarOptions *);

/* ColumnarReadState represents state of a columnar scan. */
struct ColumnarReadState;
typedef struct ColumnarReadState ColumnarReadState;


/* ColumnarWriteState represents state of a columnar write operation. */
struct ColumnarWriteState;
typedef struct ColumnarWriteState ColumnarWriteState;

/* RowMaskWriteStateEntry represents write state entry of bioseq.row_mask row */
struct RowMaskWriteStateEntry;
typedef struct RowMaskWriteStateEntry RowMaskWriteStateEntry;

/* Cache statistics for when caching is enabled and used. */
typedef struct ColumnarCacheStatistics
{
	uint64 hits;
	uint64 misses;
	uint64 evictions;
	uint64 writes;
	uint64 maximumCacheSize;
	uint64 endingCacheSize;
	uint64 entries;
} ColumnarCacheStatistics;

/*
 * BioseqIndexScanState (the CustomScanState wrapper for the bioseq
 * IndexScan custom node) lives in bioseq/backend/table/bioseq_scan_state.cppm
 * along with its layout invariants and helper methods.
 */


/*
 * StripeMetadata represents information about a stripe. This information is
 * stored in the metadata table "bioseq.stripe".
 */
typedef struct StripeMetadata
{
	uint64 fileOffset;
	uint64 dataLength;
	uint32 columnCount;
	uint32 chunkCount;
	uint32 chunkGroupRowCount;
	uint64 rowCount;
	uint64 id;
	uint64 firstRowNumber;

	/* see StripeWriteState */
	bool aborted;

	/*
	 * If write operation is in-progress (i.e. StripeWriteState returned
	 * STRIPE_WRITE_IN_PROGRESS), then insertedByCurrentXact is used to
	 * distinguish whether it's being written by current transaction or
	 * not.
	 */
	bool insertedByCurrentXact;
} StripeMetadata;

/*
 * EmptyStripeReservation represents information for an empty stripe
 * reservation.
 */
typedef struct EmptyStripeReservation
{
	uint64 stripeId;
	uint64 stripeFirstRowNumber;
} EmptyStripeReservation;

struct RowMaskWriteStateEntry
{
	uint64 id;
	uint64 storageId;
	uint64 stripeId;
	uint32 chunkId;
	int64 startRowNumber;
	int64 endRowNumber;
	int32 deletedRows;
	bytea *mask;
};


struct ChunkGroupReadState
{
	int64 currentRow;
	int64 rowCount;
	int columnCount;
	List *projectedColumnList;  /* borrowed reference */
	ChunkData *chunkGroupData;
	bytea *rowMask;
	bool rowMaskCached; /* If rowMask metadata is cached and borrowed */
	uint32 chunkStripeRowOffset; 
	uint32 chunkGroupDeletedRows;
};

struct StripeReadState
{
	int columnCount;
	int64 rowCount;
	int64 currentRow;
	TupleDesc tupleDescriptor;
	Relation relation;
	int chunkGroupIndex;
	int64 chunkGroupsFiltered;
	MemoryContext stripeReadContext;
	StripeBuffers *stripeBuffers;   /* allocated in stripeReadContext */
	List *projectedColumnList;      /* borrowed reference */
	ChunkGroupReadState *chunkGroupReadState; /* owned */
};

struct ColumnarReadState
{
	TupleDesc tupleDescriptor;
	Relation relation;

	StripeMetadata *currentStripeMetadata;
	StripeReadState *stripeReadState;

	/*
	 * Integer list of attribute numbers (1-indexed) for columns needed by the
	 * query.
	 */
	List *projectedColumnList;

	List *whereClauseList;
	List *whereClauseVars;

	MemoryContext stripeReadContext;
	int64 chunkGroupsFiltered;

	/*
	 * Memory context guaranteed to be not freed during scan so we can
	 * safely use for any memory allocations regarding ColumnarReadState
	 * itself.
	 */
	MemoryContext scanContext;

	Snapshot snapshot;
	bool snapshotRegisteredByUs;

	/* Parallel exeuction */
	ParallelColumnarScan parallelColumnarScan;
};


struct ColumnarWriteState
{
	TupleDesc tupleDescriptor;
	FmgrInfo **comparisonFunctionArray;
	RelFileLocator relfilelocator;

	MemoryContext stripeWriteContext;
	MemoryContext perTupleContext;
	StripeBuffers *stripeBuffers;
	StripeSkipList *stripeSkipList;
	EmptyStripeReservation *emptyStripeReservation;
	ColumnarOptions options;
	ChunkData *chunkData;

	List *chunkGroupRowCounts;

	/*
	 * compressionBuffer buffer is used as temporary storage during
	 * data value compression operation. It is kept here to minimize
	 * memory allocations. It lives in stripeWriteContext and gets
	 * deallocated when memory context is reset.
	 */
	StringInfo compressionBuffer;

	/*
	 * DOC0260 Phase A — per-write-state seq_caches (sub-table cache used
	 * by sequence_storage_manager). Opaque void* so types.h stays C-shape;
	 * the actual `bioseq::storage::seq_caches*` is placement-new'd into
	 * stripeWriteContext by bioseq_init_write_state and destroyed by
	 * ColumnarEndWrite. Holds the per-write-state cap snapshot.
	 */
	void *seq_caches;
};
