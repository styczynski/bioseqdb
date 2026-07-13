/*-------------------------------------------------------------------------
 *
 * metadata-types.cppm
 *
 * Shared storage_metadata data model and ABI guards. These structs used to
 * live in metadata-core; they are isolated here so table IO, write state,
 * read-state cache, and metadata catalog code can depend on one leaf
 * partition instead of the full catalog implementation.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include <cstddef>
#include <type_traits>

export module bioseq.storage.metadata:types;

import bioseq.config;

/*-------------------------------------------------------------------------
 * DOC0322 §9.2 — the C-shape column-store data model formerly in the deleted
 * backend/types.h is dissolved into this module (storage_metadata:core), the
 * single shareable home: read_state_cache (a storage_metadata partition) needs
 * BioseqColumnReadState and cannot import bioseq_table_io, and every other consumer
 * already imports storage_metadata (or :core as a sibling partition). Each
 * struct/enum is `export`ed with its field order/types preserved VERBATIM —
 * they are reinterpret_cast'd across modules, so the layout is ABI-shared.
 * (CompressionType moved to `config`; the PG type aliases uint64/Datum/etc. are
 * provided by common.h in the GMF.) The four dead fn-ptr typedefs
 * (BioseqColumnSupportsIndexAM_type, CompressionTypeStr_type, IsBioseqColumnTableAmTable_type,
 * ReadBioseqColumnOptions_type) had no users anywhere and are dropped.
 *-----------------------------------------------------------------------*/

/*
 * BioseqColumnOptions holds the option values to be used when reading or writing
 * a columnar table. To resolve these values, we first check foreign table's
 * options, and if not present, we then fall back to the default values.
 */
export struct BioseqColumnOptions
{
	uint64 stripeRowCount;
	uint32 chunkRowCount;
	CompressionType compressionType;
	int compressionLevel;
};

/* ColumnChunkSkipNode contains statistics for a ColumnChunkData. */
export struct ColumnChunkSkipNode
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
};

/*
 * StripeSkipList can be used for skipping row chunks. It contains a column chunk
 * skip node for each chunk of each column. chunkSkipNodeArray[column][chunk]
 * is the entry for the specified column chunk.
 */
export struct StripeSkipList
{
	ColumnChunkSkipNode **chunkSkipNodeArray;
	uint32 *chunkGroupRowCounts;
	uint32 *chunkGroupRowOffset;
	uint32 *chunkGroupDeletedRows;
	uint32 columnCount;
	uint32 chunkCount;
};

export struct cached_StringInfo
{
	StringInfoData data;
	bool cached;
};

/*
 * ChunkData represents a chunk of data for multiple columns. valueArray stores
 * the values of data, and existsArray stores whether a value is present.
 * valueBuffer is used to store (uncompressed) serialized values referenced by
 * Datum's in valueArray. It is only used for by-reference Datum's. There is a
 * one-to-one correspondence between valueArray and existsArray.
 */
export struct ChunkData
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
};

/*
 * ColumnChunkBuffers represents a chunk of serialized data in a column.
 * valueBuffer stores the serialized values of data, and existsBuffer stores
 * serialized value of presence information. valueCompressionType contains
 * compression type if valueBuffer is compressed. Finally rowCount has the
 * number of rows in this chunk.
 */
export struct ColumnChunkBuffers
{
	StringInfo existsBuffer;
	StringInfo valueBuffer;
	CompressionType valueCompressionType;
	uint64 decompressedValueSize;
};

/*
 * ColumnBuffers represents data buffers for a column in a row stripe. Each
 * column is made of multiple column chunks.
 */
export struct ColumnBuffers
{
	ColumnChunkBuffers **chunkBuffersArray;
};

/* StripeBuffers represents data for a row stripe. */
export struct StripeBuffers
{
	uint32 columnCount;
	uint32 chunkCount;
	uint32 rowCount;
	ColumnBuffers **columnBuffersArray;

	uint32 *selectedChunkGroupRowCounts;
	uint32 *selectedChunkGroupRowOffset;
	uint32 *selectedChunkGroupDeletedRows;
};

/* return value of StripeWriteState to decide stripe write state */
export enum StripeWriteStateEnum
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
};

/* Parallel Custom Scan shared data */
export struct ParallelBioseqColumnScanData
{
	slock_t mutex;
	pg_atomic_uint64 nextStripeId;	/* Fetch next stripe id to be read and increment */
	char snapshotData[FLEXIBLE_ARRAY_MEMBER];
};
export typedef struct ParallelBioseqColumnScanData *ParallelBioseqColumnScan;

/* Cache statistics for when caching is enabled and used. */
export struct BioseqColumnCacheStatistics
{
	uint64 hits;
	uint64 misses;
	uint64 evictions;
	uint64 writes;
	uint64 maximumCacheSize;
	uint64 endingCacheSize;
	uint64 entries;
};

/*
 * StripeMetadata represents information about a stripe. This information is
 * stored in the metadata table "bioseq.stripe".
 */
export struct StripeMetadata
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
	 * distinguish whether it's being written by current transaction or not.
	 */
	bool insertedByCurrentXact;
};

/*
 * EmptyStripeReservation represents information for an empty stripe
 * reservation.
 */
export struct EmptyStripeReservation
{
	uint64 stripeId;
	uint64 stripeFirstRowNumber;
};

/* RowMaskWriteStateEntry represents write state entry of bioseq.row_mask row */
export struct RowMaskWriteStateEntry
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

export struct ChunkGroupReadState
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

export struct StripeReadState
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

/* BioseqColumnReadState represents state of a columnar scan. */
export struct BioseqColumnReadState
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
	 * Memory context guaranteed to be not freed during scan so we can safely
	 * use for any memory allocations regarding BioseqColumnReadState itself.
	 */
	MemoryContext scanContext;

	Snapshot snapshot;
	bool snapshotRegisteredByUs;

	/* Parallel exeuction */
	ParallelBioseqColumnScan parallelBioseqColumnScan;
};

/* BioseqColumnWriteState represents state of a columnar write operation. */
export struct BioseqColumnWriteState
{
	TupleDesc tupleDescriptor;
	FmgrInfo **comparisonFunctionArray;
	RelFileLocator relfilelocator;

	MemoryContext stripeWriteContext;
	MemoryContext perTupleContext;
	StripeBuffers *stripeBuffers;
	StripeSkipList *stripeSkipList;
	EmptyStripeReservation *emptyStripeReservation;
	BioseqColumnOptions options;
	ChunkData *chunkData;

	List *chunkGroupRowCounts;

	/*
	 * compressionBuffer buffer is used as temporary storage during data value
	 * compression operation. It is kept here to minimize memory allocations.
	 * It lives in stripeWriteContext and gets deallocated when memory context
	 * is reset.
	 */
	StringInfo compressionBuffer;

	/*
	 * DOC0260 Phase A — per-write-state seq_caches (sub-table cache used by
	 * sequence_storage_manager). Opaque void* so this struct stays decoupled
	 * from bioseq::storage; the actual `bioseq::storage::seq_caches*` is
         * placement-new'd into stripeWriteContext's stable owning parent and
         * destroyed by the write-state pop hook before BioseqColumnEndWrite on
         * commit. It must outlive every reset of the stripe scratch context.
         * Holds the per-write-state cap snapshot.
	 */
	void *seq_caches;
};

/*-------------------------------------------------------------------------
 * DOC0322 §9.1/§14.1 — ABI layout guards for the structs reinterpret_cast'd
 * across modules. Each replica mirrors the original C field layout above; the
 * static_assert pins std::is_standard_layout plus sizeof + the offsetof of a
 * key field, so a future field reorder/retype is a hard compile error. (Field
 * order is unchanged here, so they pass today.) ParallelBioseqColumnScanData has a
 * FLEXIBLE_ARRAY_MEMBER, so only is_standard_layout is asserted for it (sizeof
 * of a FAM struct is not meaningful). VectorColumn and the bioseq_table_types
 * scan descriptors carry their own guards in their owning modules.
 *-----------------------------------------------------------------------*/
namespace {
struct _BioseqColumnReadState_ref
{
	TupleDesc tupleDescriptor;
	Relation relation;
	StripeMetadata *currentStripeMetadata;
	StripeReadState *stripeReadState;
	List *projectedColumnList;
	List *whereClauseList;
	List *whereClauseVars;
	MemoryContext stripeReadContext;
	int64 chunkGroupsFiltered;
	MemoryContext scanContext;
	Snapshot snapshot;
	bool snapshotRegisteredByUs;
	ParallelBioseqColumnScan parallelBioseqColumnScan;
};
static_assert(std::is_standard_layout_v<BioseqColumnReadState> &&
			  sizeof(BioseqColumnReadState) == sizeof(_BioseqColumnReadState_ref) &&
			  offsetof(BioseqColumnReadState, tupleDescriptor) == offsetof(_BioseqColumnReadState_ref, tupleDescriptor) &&
			  offsetof(BioseqColumnReadState, parallelBioseqColumnScan) == offsetof(_BioseqColumnReadState_ref, parallelBioseqColumnScan),
			  "BioseqColumnReadState layout drift (reinterpret_cast'd across modules)");

struct _BioseqColumnWriteState_ref
{
	TupleDesc tupleDescriptor;
	FmgrInfo **comparisonFunctionArray;
	RelFileLocator relfilelocator;
	MemoryContext stripeWriteContext;
	MemoryContext perTupleContext;
	StripeBuffers *stripeBuffers;
	StripeSkipList *stripeSkipList;
	EmptyStripeReservation *emptyStripeReservation;
	BioseqColumnOptions options;
	ChunkData *chunkData;
	List *chunkGroupRowCounts;
	StringInfo compressionBuffer;
	void *seq_caches;
};
static_assert(std::is_standard_layout_v<BioseqColumnWriteState> &&
			  sizeof(BioseqColumnWriteState) == sizeof(_BioseqColumnWriteState_ref) &&
			  offsetof(BioseqColumnWriteState, tupleDescriptor) == offsetof(_BioseqColumnWriteState_ref, tupleDescriptor) &&
			  offsetof(BioseqColumnWriteState, seq_caches) == offsetof(_BioseqColumnWriteState_ref, seq_caches),
			  "BioseqColumnWriteState layout drift (reinterpret_cast'd across modules)");

struct _ParallelBioseqColumnScanData_ref
{
	slock_t mutex;
	pg_atomic_uint64 nextStripeId;
	char snapshotData[FLEXIBLE_ARRAY_MEMBER];
};
static_assert(std::is_standard_layout_v<ParallelBioseqColumnScanData>,
			  "ParallelBioseqColumnScanData must stay standard-layout (FAM struct, reinterpret_cast'd across modules)");
}  // namespace
