/*-------------------------------------------------------------------------
 *
 * columnar_metadata.cppm
 *
 * Manages metadata for columnar relations in separate, shared metadata
 * tables in the "bioseq" catalog schema.
 *
 *   * holds basic stripe information including data size and row counts
 *   * holds basic chunk and chunk group information like data offsets and
 *     min/max values (used for Chunk Group Filtering)
 *   * useful for fast VACUUM operations (e.g. reporting with VACUUM VERBOSE)
 *   * useful for stats/costing
 *   * maps logical row numbers to stripe IDs
 *   * TODO: visibility information
 *
 * Each metadata table is opened by oid through the per-call helpers in
 * the "bioseq.foo_relid" family further down. Lookups go through index
 * scans (systable_beginscan_ordered) so they're transactional and
 * MVCC-safe.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include "safe_lib.h"

#include <sys/stat.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

export module bioseq.storage.metadata:core;

import bioseq.storage.physical;
import bioseq.common.mlogging;
import bioseq.config;
import bioseq.pg_utils;
import :types;
import :tuple_ops;


//export ItemPointerData row_number_to_tid(uint64_t rowNumber);
export uint64_t LookupStorageId(RelFileLocator relfilelocator);
export StripeMetadata * FindStripeWithMatchingFirstRowNumber(Relation relation,
															 uint64_t rowNumber,
															 Snapshot snapshot);
export uint64_t StripeGetHighestRowNumber(StripeMetadata *stripeMetadata);
export EState *
create_estate_for_relation(Relation rel)
{
	return bioseq::metadata::tuple_ops::create_estate_for_relation(rel);
}

// ---------------------------------------------------------------------------
// DOC0260 Phase A — bioseq_internal.seq_sub_table catalog access.
// ---------------------------------------------------------------------------

export constexpr int Natts_seq_sub_table                       = 11;
export constexpr int Anum_seq_sub_table_database_oid           = 1;
export constexpr int Anum_seq_sub_table_tablespace_oid         = 2;
export constexpr int Anum_seq_sub_table_main_relfilenode       = 3;
export constexpr int Anum_seq_sub_table_column_attno           = 4;
export constexpr int Anum_seq_sub_table_bucket_idx             = 5;
export constexpr int Anum_seq_sub_table_shard_idx              = 6;
export constexpr int Anum_seq_sub_table_sub_table_relid        = 7;
export constexpr int Anum_seq_sub_table_sidecar_relid          = 8;
export constexpr int Anum_seq_sub_table_bytes_written          = 9;
export constexpr int Anum_seq_sub_table_is_full                = 10;
export constexpr int Anum_seq_sub_table_total_payload_bytes    = 11;

export struct SubTableRow {
	Oid             database_oid       = InvalidOid;
	Oid             tablespace_oid     = InvalidOid;
	Oid             main_relfilenode   = InvalidOid;
	int16_t         column_attno       = 0;
	int16_t         bucket_idx         = 0;
	int32_t         shard_idx          = 0;
	Oid             sub_table_relid    = InvalidOid;
	Oid             sidecar_relid      = InvalidOid;
	std::uint64_t   bytes_written      = 0;
	bool            is_full            = false;
	std::uint64_t   total_payload_bytes = 0;
};


export [[nodiscard]] std::optional<SubTableRow>
LookupActiveSubTableShard(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
                          int16_t column_attno, int16_t bucket_idx);

export [[nodiscard]] std::optional<SubTableRow>
LookupSubTableShard(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
                    int16_t column_attno, int16_t bucket_idx, int32_t shard_idx);

export [[nodiscard]] SubTableRow
InsertSubTableShard(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
                    int16_t column_attno, int16_t bucket_idx, int32_t shard_idx,
                    Oid sub_table_relid, Oid sidecar_relid);

export SubTableRow
UpdateSubTableShardBytesWritten(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
                                int16_t column_attno, int16_t bucket_idx, int32_t shard_idx,
                                std::uint64_t delta_bytes,
                                std::uint64_t cap_bytes);

export [[nodiscard]] std::vector<SubTableRow>
ListSubTableShardsForMain(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode);

export void
DeleteSubTableShardsForMain(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode);


/*
 * GetFunctionInfoOrNull first resolves the operator for the given data type,
 * access method, and support procedure. The function then uses the resolved
 * operator's identifier to fill in a function manager object, and returns
 * this object. This function is based on a similar function from CitusDB's code.
 */
export FmgrInfo *
GetFunctionInfoOrNull(Oid typeId, Oid accessMethodId, int16_t procedureId)
{
	FmgrInfo *functionInfo = NULL;

	/* get default operator class from pg_opclass for datum type */
	Oid operatorClassId = GetDefaultOpClass(typeId, accessMethodId);
	if (operatorClassId == InvalidOid)
	{
		return NULL;
	}

	Oid operatorFamilyId = get_opclass_family(operatorClassId);
	if (operatorFamilyId == InvalidOid)
	{
		return NULL;
	}

	Oid operatorId = get_opfamily_proc(operatorFamilyId, typeId, typeId, procedureId);

	/* No operator for typeId, search for operator class type*/
	if (operatorId == InvalidOid)
	{
		Oid opcintype = get_opclass_input_type(operatorClassId);
		operatorId = get_opfamily_proc(operatorFamilyId, opcintype, opcintype, procedureId);
	}

	if (operatorId != InvalidOid)
	{
		functionInfo = make0<FmgrInfo>();

		/* fill in the FmgrInfo struct using the operatorId */
		fmgr_info(operatorId, functionInfo);
	}

	return functionInfo;
}

Oid safe_get_relname_relid(const char *relname, Oid relnamespace) {
	//elog(NOTICE, "BioseqColumn [safe_safe_get_relname_relid] " __FILE__ ":" TOSTRING(__LINE__) " => ");
	//elog(NOTICE, relname);
	const Oid relid = get_relname_relid(relname, relnamespace);
	//elog(NOTICE, "BioseqColumn [safe_safe_get_relname_relid] [SUCCESS]" __FILE__ ":" TOSTRING(__LINE__));
	return relid;
}

/* RowNumberLookupMode to be used in StripeMetadataLookupRowNumber */
enum RowNumberLookupMode
{
	/*
	 * Find the stripe whose firstRowNumber is less than or equal to given
	 * input rowNumber.
	 */
	FIND_LESS_OR_EQUAL,

	/*
	 * Find the stripe whose firstRowNumber is greater than input rowNumber.
	 */
	FIND_GREATER
};

static void InsertEmptyStripeMetadataRow(uint64_t storageId, uint64_t stripeId,
										 uint32_t columnCount, uint32_t chunkGroupRowCount,
										 uint64_t firstRowNumber);
static void GetHighestUsedAddressAndId(uint64_t storageId,
									   uint64_t *highestUsedAddress,
									   uint64_t *highestUsedId);
static StripeMetadata * UpdateStripeMetadataRow(uint64_t storageId, uint64_t stripeId,
												bool *update, Datum *newValues);
static List * ReadDataFileStripeList(uint64_t storageId, Snapshot snapshot,
									 ScanDirection scanDirection);
export StripeMetadata * BuildStripeMetadata(Relation bioseqColumnStripes,
											HeapTuple heapTuple);
static void ReadChunkGroupRowCounts(uint64_t storageId, uint64_t stripe,
									uint32_t chunkGroupCount,
									uint32_t **chunkGroupRowCounts,
									uint32_t **chunkGroupDeletedRows,
									Snapshot snapshot);
static Oid BioseqColumnStorageIdSequenceRelationId(void);
export Oid BioseqColumnStripeRelationId(void);
export Oid BioseqColumnStripePKeyIndexRelationId(void);
static Oid BioseqColumnStripeFirstRowNumberIndexRelationId(void);
static Oid BioseqColumnOptionsRelationId(void);
static Oid BioseqColumnOptionsIndexRegclass(void);
static Oid BioseqColumnChunkRelationId(void);
static Oid BioseqColumnChunkGroupRelationId(void);
// DOC0322 §7.1 — UpdateRowMask relocates to storage_metadata:write_state_row_mask
// and calls these two resolvers, so they are exported (sibling partitions cannot
// reach module-internal `static` functions).
export Oid BioseqColumnRowMaskRelationId(void);
static Oid BioseqColumnRowMaskSeqId(void);
static Oid BioseqColumnChunkIndexRelationId(void);
static Oid BioseqColumnChunkGroupIndexRelationId(void);
export Oid BioseqColumnRowMaskIndexRelationId(void);
static Oid BioseqColumnRowMaskStripeIndexRelationId(void);
static Oid BioseqColumnNamespaceId(void);
static uint64_t GetHighestUsedRowNumber(uint64_t storageId);
static void DeleteStorageFromBioseqColumnMetadataTable(Oid metadataTableId,
												   AttrNumber storageIdAtrrNumber,
												   Oid storageIdIndexId,
												   uint64_t storageId);
static void DeleteStripeFromBioseqColumnMetadataTable(Oid metadataTableId,
												  AttrNumber storageIdAtrrNumber,
												  AttrNumber stripeIdAttrNumber,
												  Oid storageIdIndexId,
												  uint64_t storageId,
												  uint64_t stripeId);
static bool WriteBioseqColumnOptions(Oid regclass, BioseqColumnOptions *options, bool overwrite);
static StripeMetadata * StripeMetadataLookupRowNumber(Relation relation, uint64_t rowNumber,
													  Snapshot snapshot,
													  RowNumberLookupMode lookupMode);
static void CheckStripeMetadataConsistency(StripeMetadata *stripeMetadata);


/* ----------------
 *		bioseq.options definition.
 * ----------------
 */
struct FormData_bioseq_options
{
	Oid regclass;
	int32_t chunk_group_row_limit;
	int32_t stripe_row_limit;
	int32_t compressionLevel;
	NameData compression;

#ifdef CATALOG_VARLEN           /* variable-length fields start here */
#endif
};
using Form_bioseq_options = FormData_bioseq_options *;



/*
 * InitBioseqColumnOptions initialized the columnar table options. Meaning it writes the
 * default options to the options table if not already existing.
 */
export void
InitBioseqColumnOptions(Oid regclass)
{
	/*
	 * When upgrading we retain options for all columnar tables by upgrading
	 * "bioseq.options" catalog table, so we shouldn't do anything here.
	 */
	if (IsBinaryUpgrade)
	{
		return;
	}

	BioseqColumnOptions defaultOptions = {
		.chunkRowCount = static_cast<uint32_t>(bioseq_chunk_group_row_limit),
		.stripeRowCount = static_cast<uint32_t>(bioseq_stripe_row_limit),
		.compressionType = static_cast<CompressionType>(bioseq_compression),
		.compressionLevel = bioseq_compression_level
	};

	WriteBioseqColumnOptions(regclass, &defaultOptions, false);
}


/*
 * SetBioseqColumnOptions writes the passed table options as the authoritive options to the
 * table irregardless of the optiones already existing or not. This can be used to put a
 * table in a certain state.
 */
export void
SetBioseqColumnOptions(Oid regclass, BioseqColumnOptions *options)
{
	WriteBioseqColumnOptions(regclass, options, true);
}


/*
 * WriteBioseqColumnOptions writes the options to the catalog table for a given regclass.
 *  - If overwrite is false it will only write the values if there is not already a record
 *    found.
 *  - If overwrite is true it will always write the settings
 *
 * The return value indicates if the record has been written.
 */
static bool
WriteBioseqColumnOptions(Oid regclass, BioseqColumnOptions *options, bool overwrite)
{
	/*
	 * When upgrading we should retain the options from the previous
	 * cluster and don't write new options.
	 */
	Assert(!IsBinaryUpgrade);

	bool written = false;

	bool nulls[Natts_bioseq_options] = { 0 };
	Datum values[Natts_bioseq_options] = {
		ObjectIdGetDatum(regclass),
		Int32GetDatum(options->chunkRowCount),
		Int32GetDatum(options->stripeRowCount),
		Int32GetDatum(options->compressionLevel),
		0, /* to be filled below */
	};

	NameData compressionName = { 0 };
	namestrcpy(&compressionName, CompressionTypeStr(options->compressionType));
	values[Anum_bioseq_options_compression - 1] = NameGetDatum(&compressionName);

	/* create heap tuple and insert into catalog table */
	Relation bioseqColumnOptions = safe_relation_open(BioseqColumnOptionsRelationId(),
											 RowExclusiveLock);
	TupleDesc tupleDescriptor = RelationGetDescr(bioseqColumnOptions);

	/* find existing item to perform update if exist */
	ScanKeyData scanKey[1] = { 0 };
	ScanKeyInit(&scanKey[0], Anum_bioseq_options_regclass, BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(regclass));

	Relation index = index_open(BioseqColumnOptionsIndexRegclass(), AccessShareLock);
	SysScanDesc scanDescriptor = systable_beginscan_ordered(bioseqColumnOptions, index, NULL,
															1, scanKey);

	HeapTuple heapTuple = systable_getnext_ordered(scanDescriptor, ForwardScanDirection);
	if (HeapTupleIsValid(heapTuple))
	{
		if (overwrite)
		{
			/* TODO check if the options are actually different, skip if not changed */
			/* update existing record */
			bool update[Natts_bioseq_options] = { 0 };
			update[Anum_bioseq_options_chunk_group_row_limit - 1] = true;
			update[Anum_bioseq_options_stripe_row_limit - 1] = true;
			update[Anum_bioseq_options_compression_level - 1] = true;
			update[Anum_bioseq_options_compression - 1] = true;

			HeapTuple tuple = heap_modify_tuple(heapTuple, tupleDescriptor,
												values, nulls, update);
			CatalogTupleUpdate(bioseqColumnOptions, &tuple->t_self, tuple);
			written = true;
		}
	}
	else
	{
		/* inserting new record */
		HeapTuple newTuple = heap_form_tuple(tupleDescriptor, values, nulls);
		CatalogTupleInsert(bioseqColumnOptions, newTuple);
		written = true;
	}

	if (written)
	{
		CommandCounterIncrement();
	}

	systable_endscan_ordered(scanDescriptor);
	index_close(index, AccessShareLock);
	relation_close(bioseqColumnOptions, RowExclusiveLock);

	return written;
}


/*
 * DeleteBioseqColumnTableOptions removes the columnar table options for a regclass. When
 * missingOk is false it will throw an error when no table options can be found.
 *
 * Returns whether a record has been removed.
 */
export bool
DeleteBioseqColumnTableOptions(Oid regclass, bool missingOk)
{
	bool result = false;

	/*
	 * When upgrading we shouldn't delete or modify table options and
	 * retain options from the previous cluster.
	 */
	Assert(!IsBinaryUpgrade);

	Relation bioseqColumnOptions = try_safe_relation_open(BioseqColumnOptionsRelationId(),
												 RowExclusiveLock);
	if (bioseqColumnOptions == NULL)
	{
		/* extension has been dropped */
		return false;
	}

	/* find existing item to remove */
	ScanKeyData scanKey[1] = { 0 };
	ScanKeyInit(&scanKey[0], Anum_bioseq_options_regclass, BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(regclass));

	Relation index = index_open(BioseqColumnOptionsIndexRegclass(), AccessShareLock);
	SysScanDesc scanDescriptor = systable_beginscan_ordered(bioseqColumnOptions, index, NULL,
															1, scanKey);

	HeapTuple heapTuple = systable_getnext_ordered(scanDescriptor, ForwardScanDirection);
	if (HeapTupleIsValid(heapTuple))
	{
		CatalogTupleDelete(bioseqColumnOptions, &heapTuple->t_self);
		CommandCounterIncrement();

		result = true;
	}
	else if (!missingOk)
	{
		bioseq_log<ERR, PG>( "missing options for regclass: {}", regclass);
	}

	systable_endscan_ordered(scanDescriptor);
	index_close(index, AccessShareLock);
	relation_close(bioseqColumnOptions, RowExclusiveLock);

	return result;
}


export bool
ReadBioseqColumnOptions(Oid regclass, BioseqColumnOptions *options)
{
	ScanKeyData scanKey[1];

	ScanKeyInit(&scanKey[0], Anum_bioseq_options_regclass, BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(regclass));

	Oid bioseqColumnOptionsOid = BioseqColumnOptionsRelationId();
	Relation bioseqColumnOptions = try_safe_relation_open(bioseqColumnOptionsOid, AccessShareLock);
	if (bioseqColumnOptions == NULL)
	{
		/*
		 * Extension has been dropped. This can be called while
		 * dropping extension or database via ObjectAccess().
		 */
		return false;
	}

	Relation index = try_safe_relation_open(BioseqColumnOptionsIndexRegclass(), AccessShareLock);
	if (index == NULL)
	{
		table_close(bioseqColumnOptions, AccessShareLock);

		/* extension has been dropped */
		return false;
	}

	SysScanDesc scanDescriptor = systable_beginscan_ordered(bioseqColumnOptions, index, NULL,
															1, scanKey);

	HeapTuple heapTuple = systable_getnext_ordered(scanDescriptor, ForwardScanDirection);
	if (HeapTupleIsValid(heapTuple))
	{
		Form_bioseq_options tupOptions = reinterpret_cast<Form_bioseq_options>(GETSTRUCT(heapTuple));

		options->chunkRowCount = tupOptions->chunk_group_row_limit;
		options->stripeRowCount = tupOptions->stripe_row_limit;
		options->compressionLevel = tupOptions->compressionLevel;
		options->compressionType = ParseCompressionType(NameStr(tupOptions->compression));
	}
	else
	{
		/* populate options with system defaults */
		options->compressionType = static_cast<CompressionType>(bioseq_compression);
		options->stripeRowCount = static_cast<uint32_t>(bioseq_stripe_row_limit);
		options->chunkRowCount = static_cast<uint32_t>(bioseq_chunk_group_row_limit);
		options->compressionLevel = bioseq_compression_level;
	}

	systable_endscan_ordered(scanDescriptor);
	index_close(index, AccessShareLock);
	relation_close(bioseqColumnOptions, AccessShareLock);

	return true;
}


/*
 * SaveStripeSkipList saves chunkList for a given stripe as rows
 * of bioseq.chunk.
 */
export void
SaveStripeSkipList(RelFileLocator relfilelocator, uint64_t stripe, StripeSkipList *chunkList,
				   TupleDesc tupleDescriptor)
{
	uint32_t columnIndex = 0;
	uint32_t chunkIndex = 0;
	uint32_t columnCount = chunkList->columnCount;

	uint64_t storageId = LookupStorageId(relfilelocator);
	Oid bioseqColumnChunkOid = BioseqColumnChunkRelationId();
	Relation bioseqColumnChunk = table_open(bioseqColumnChunkOid, RowExclusiveLock);
	bioseq::metadata::tuple_ops::ModifyState *modifyState = bioseq::metadata::tuple_ops::start_modify_relation(bioseqColumnChunk);

	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		for (chunkIndex = 0; chunkIndex < chunkList->chunkCount; chunkIndex++)
		{
			ColumnChunkSkipNode *chunk =
				&chunkList->chunkSkipNodeArray[columnIndex][chunkIndex];

			Datum values[Natts_bioseq_chunk] = {
				UInt64GetDatum(storageId),
				Int64GetDatum(stripe),
				Int32GetDatum(columnIndex + 1),
				Int32GetDatum(chunkIndex),
				0, /* to be filled below */
				0, /* to be filled below */
				Int64GetDatum(chunk->valueChunkOffset),
				Int64GetDatum(chunk->valueLength),
				Int64GetDatum(chunk->existsChunkOffset),
				Int64GetDatum(chunk->existsLength),
				Int32GetDatum(chunk->valueCompressionType),
				Int32GetDatum(chunk->valueCompressionLevel),
				Int64GetDatum(chunk->decompressedValueSize),
				Int64GetDatum(chunk->rowCount)
			};

			bool nulls[Natts_bioseq_chunk] = { false };

			if (chunk->hasMinMax)
			{
				values[Anum_bioseq_chunk_minimum_value - 1] =
					PointerGetDatum(bioseq::metadata::tuple_ops::datum_to_bytea(chunk->minimumValue,
												 TupleDescAttr(tupleDescriptor, columnIndex)));
				values[Anum_bioseq_chunk_maximum_value - 1] =
					PointerGetDatum(bioseq::metadata::tuple_ops::datum_to_bytea(chunk->maximumValue,
												 TupleDescAttr(tupleDescriptor, columnIndex)));
			}
			else
			{
				nulls[Anum_bioseq_chunk_minimum_value - 1] = true;
				nulls[Anum_bioseq_chunk_maximum_value - 1] = true;
			}

			bioseq::metadata::tuple_ops::insert_tuple_and_enforce_constraints(modifyState, values, nulls);
		}
	}

	bioseq::metadata::tuple_ops::finish_modify_relation(modifyState);
	table_close(bioseqColumnChunk, RowExclusiveLock);
}


/*
 * SaveChunkGroups saves the metadata for given chunk groups in bioseq.chunk_group.
 */
export void
SaveChunkGroups(RelFileLocator relfilelocator, uint64_t stripe,
				List *chunkGroupRowCounts)
{
	uint64_t storageId = LookupStorageId(relfilelocator);
	Oid bioseqColumnChunkGroupOid = BioseqColumnChunkGroupRelationId();
	Relation bioseqColumnChunkGroup = table_open(bioseqColumnChunkGroupOid, RowExclusiveLock);
	bioseq::metadata::tuple_ops::ModifyState *modifyState = bioseq::metadata::tuple_ops::start_modify_relation(bioseqColumnChunkGroup);

	int chunkId = 0;

	for (auto rowCount : PIntList{chunkGroupRowCounts})
	{
		Datum values[Natts_bioseq_chunkgroup] = {
			UInt64GetDatum(storageId),
			Int64GetDatum(stripe),
			Int32GetDatum(chunkId),
			Int64GetDatum(rowCount),
			Int64GetDatum(0)   /* deleted_rows: bigint, read back via DatumGetUInt64 */
		};

		bool nulls[Natts_bioseq_chunkgroup] = { false };

		bioseq::metadata::tuple_ops::insert_tuple_and_enforce_constraints(modifyState, values, nulls);
		chunkId++;
	}

	bioseq::metadata::tuple_ops::finish_modify_relation(modifyState);
	table_close(bioseqColumnChunkGroup, RowExclusiveLock);
}


/*
 * SaveEmptyRowMask saves the metadata for inserted rows in bioseq.mask_row
 */
export bool 
SaveEmptyRowMask(uint64_t storageId, uint64_t stripeId,
				 uint64_t stripeStartRowNumber, List *chunkGroupRowCounts)
{
	Oid bioseqColumnRowMaskOid = BioseqColumnRowMaskRelationId();
	Oid bioseqColumnRowMaskSeq = BioseqColumnRowMaskSeqId();
	Relation bioseqColumnRowMask = table_open(bioseqColumnRowMaskOid, RowExclusiveLock);
	bioseq::metadata::tuple_ops::ModifyState *modifyState = bioseq::metadata::tuple_ops::start_modify_relation(bioseqColumnRowMask);

	uint64_t chunkIterationStartRowNumber = stripeStartRowNumber;
	uint64_t chunkIterationEndRowNumber = stripeStartRowNumber -  1;

	int chunkId = 0;

	bool chunkInserted = true;

	for (auto rowCount : PIntList{chunkGroupRowCounts})
	{
		if (!chunkInserted)
			break;

		uint16_t rowMaskIterations =
			(rowCount % BIOSEQ_COLUMN_ROW_MASK_CHUNK_SIZE) ? 
				(rowCount / BIOSEQ_COLUMN_ROW_MASK_CHUNK_SIZE + 1) :
				(rowCount / BIOSEQ_COLUMN_ROW_MASK_CHUNK_SIZE);

		for(uint16_t n = 0; n < rowMaskIterations; n++)
		{
			uint16_t maskSize;

			/* Last iteration */
			if (n == (rowMaskIterations - 1))
			{
				uint16_t lastIterationRowSize = rowCount % BIOSEQ_COLUMN_ROW_MASK_CHUNK_SIZE ? 
						rowCount % BIOSEQ_COLUMN_ROW_MASK_CHUNK_SIZE : BIOSEQ_COLUMN_ROW_MASK_CHUNK_SIZE;
				chunkIterationEndRowNumber += lastIterationRowSize;
				maskSize = (lastIterationRowSize) % 8 ? 
								lastIterationRowSize / 8 + 1 :
								lastIterationRowSize / 8;
			}
			else
			{
				chunkIterationEndRowNumber += BIOSEQ_COLUMN_ROW_MASK_CHUNK_SIZE;
				maskSize = BIOSEQ_COLUMN_ROW_MASK_CHUNK_SIZE / 8;
			}

			bytea *initialLookupRecord = make1<bytea>(maskSize + VARHDRSZ);
			SET_VARSIZE(initialLookupRecord, maskSize + VARHDRSZ);

			int64_t nextSeqId = nextval_internal(bioseqColumnRowMaskSeq, false);

			Datum values[Natts_bioseq_row_mask] = {
				Int64GetDatum(nextSeqId),
				Int64GetDatum(storageId),
				Int64GetDatum(stripeId),
				Int32GetDatum(chunkId),
				Int64GetDatum(chunkIterationStartRowNumber),
				Int64GetDatum(chunkIterationEndRowNumber),
				Int32GetDatum(0), 
				0, /* to be filled below */
			};

			values[Anum_bioseq_row_mask_mask - 1] =
				PointerGetDatum(initialLookupRecord);

			bool nulls[Natts_bioseq_row_mask] = { false };

			/*
			 * bioseq.row_mask has UNIQUE constraint which can throw
			 * error so we need to catch this and return to function caller
			 * if saving empty row mask was succesful or not.
			 */
			PG_TRY();
			{
				bioseq::metadata::tuple_ops::insert_tuple_and_enforce_constraints(modifyState, values, nulls);
			}
			
			PG_CATCH();
			{
				FlushErrorState();
				chunkInserted = false;
				break;
			}

			PG_END_TRY();

			chunkIterationStartRowNumber += BIOSEQ_COLUMN_ROW_MASK_CHUNK_SIZE;
		}

		chunkIterationStartRowNumber = chunkIterationEndRowNumber + 1;
		chunkId++;
	}

	bioseq::metadata::tuple_ops::finish_modify_relation(modifyState);
	table_close(bioseqColumnRowMask, RowExclusiveLock);

	return chunkInserted;
}


/*
 * ReadStripeSkipList fetches chunk metadata for a given stripe.
 */
export StripeSkipList *
ReadStripeSkipList(RelFileLocator relfilelocator, uint64_t stripe, TupleDesc tupleDescriptor,
				   uint32_t chunkCount, Snapshot snapshot)
{
	int32_t columnIndex = 0;
	int32_t chunkGroupIndex = 0;
	int32_t chunkGroupRowOffsetAcc = 0;
	HeapTuple heapTuple = NULL;
	uint32_t columnCount = tupleDescriptor->natts;
	ScanKeyData scanKey[2];

	uint64_t storageId = LookupStorageId(relfilelocator);

	Oid bioseqColumnChunkOid = BioseqColumnChunkRelationId();
	Relation bioseqColumnChunk = table_open(bioseqColumnChunkOid, AccessShareLock);
	Relation index = index_open(BioseqColumnChunkIndexRelationId(), AccessShareLock);

	ScanKeyInit(&scanKey[0], Anum_bioseq_chunk_storageid,
				BTEqualStrategyNumber, F_INT8EQ, UInt64GetDatum(storageId));
	ScanKeyInit(&scanKey[1], Anum_bioseq_chunk_stripe,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(stripe));

	SysScanDesc scanDescriptor = systable_beginscan_ordered(bioseqColumnChunk, index,
															snapshot, 2, scanKey);

	StripeSkipList *chunkList = make0<StripeSkipList>();
	chunkList->chunkCount = chunkCount;
	chunkList->columnCount = columnCount;
	chunkList->chunkSkipNodeArray = make_array<ColumnChunkSkipNode*>(columnCount);
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		chunkList->chunkSkipNodeArray[columnIndex] =
			make_array<ColumnChunkSkipNode>(chunkCount);
	}

	while (HeapTupleIsValid(heapTuple = systable_getnext_ordered(scanDescriptor,
																 ForwardScanDirection)))
	{
		Datum datumArray[Natts_bioseq_chunk];
		bool isNullArray[Natts_bioseq_chunk];

		heap_deform_tuple(heapTuple, RelationGetDescr(bioseqColumnChunk), datumArray,
						  isNullArray);

		int32_t attr = DatumGetInt32(datumArray[Anum_bioseq_chunk_attr - 1]);
		int32_t chunkIndex = DatumGetInt32(datumArray[Anum_bioseq_chunk_chunk - 1]);

		if (attr <= 0 || attr > columnCount)
		{
			bioseq_report<ERR, PG>().msg("invalid columnar chunk entry").detail("Attribute number out of range: {}", attr);
		}

		if (chunkIndex < 0 || chunkIndex >= chunkCount)
		{
			bioseq_report<ERR, PG>().msg("invalid columnar chunk entry").detail("Chunk number out of range: {}", chunkIndex);
		}

		columnIndex = attr - 1;

		ColumnChunkSkipNode *chunk =
			&chunkList->chunkSkipNodeArray[columnIndex][chunkIndex];
		chunk->rowCount = DatumGetInt64(datumArray[Anum_bioseq_chunk_value_count -
												   1]);
		chunk->valueChunkOffset =
			DatumGetInt64(datumArray[Anum_bioseq_chunk_value_stream_offset - 1]);
		chunk->valueLength =
			DatumGetInt64(datumArray[Anum_bioseq_chunk_value_stream_length - 1]);
		chunk->existsChunkOffset =
			DatumGetInt64(datumArray[Anum_bioseq_chunk_exists_stream_offset - 1]);
		chunk->existsLength =
			DatumGetInt64(datumArray[Anum_bioseq_chunk_exists_stream_length - 1]);
		chunk->valueCompressionType =
			static_cast<CompressionType>(DatumGetInt32(datumArray[Anum_bioseq_chunk_value_compression_type - 1]));
		chunk->valueCompressionLevel =
			DatumGetInt32(datumArray[Anum_bioseq_chunk_value_compression_level - 1]);
		chunk->decompressedValueSize =
			DatumGetInt64(datumArray[Anum_bioseq_chunk_value_decompressed_size - 1]);

		if (isNullArray[Anum_bioseq_chunk_minimum_value - 1] ||
			isNullArray[Anum_bioseq_chunk_maximum_value - 1])
		{
			chunk->hasMinMax = false;
		}
		else
		{
			bytea *minValue = DatumGetByteaP(
				datumArray[Anum_bioseq_chunk_minimum_value - 1]);
			bytea *maxValue = DatumGetByteaP(
				datumArray[Anum_bioseq_chunk_maximum_value - 1]);

			chunk->minimumValue =
				bioseq::metadata::tuple_ops::bytea_to_datum(minValue, TupleDescAttr(tupleDescriptor, columnIndex));
			chunk->maximumValue =
				bioseq::metadata::tuple_ops::bytea_to_datum(maxValue, TupleDescAttr(tupleDescriptor, columnIndex));

			chunk->hasMinMax = true;
		}
	}

	systable_endscan_ordered(scanDescriptor);
	index_close(index, AccessShareLock);
	table_close(bioseqColumnChunk, AccessShareLock);

	ReadChunkGroupRowCounts(storageId, stripe, chunkCount,
							&chunkList->chunkGroupRowCounts,
							&chunkList->chunkGroupDeletedRows,
							snapshot);

	chunkList->chunkGroupRowOffset = make_array<uint32_t>(chunkCount);

	for (chunkGroupIndex = 0; chunkGroupIndex < chunkCount; chunkGroupIndex++)
	{
		chunkList->chunkGroupRowOffset[chunkGroupIndex] = chunkGroupRowOffsetAcc;
		chunkGroupRowOffsetAcc += chunkList->chunkGroupRowCounts[chunkGroupIndex];
	}

	return chunkList;
}


/*
 * ReadChunkRowMask fetches chunk row mask for columnar relation.
 */
export bytea *
ReadChunkRowMask(RelFileLocator relfilelocator, Snapshot snapshot,
				 MemoryContext cxt,
				 uint64_t stripeFirstRowNumber, int rowCount)
{
	HeapTuple heapTuple = NULL;
	ScanKeyData scanKey[3];

	uint64_t storageId = LookupStorageId(relfilelocator);

	Oid bioseqColumnRowMaskOid = BioseqColumnRowMaskRelationId();
	Relation bioseqColumnRowMask = table_open(bioseqColumnRowMaskOid, AccessShareLock);
	Relation index = index_open(BioseqColumnRowMaskIndexRelationId(), AccessShareLock);

	bytea *chunkRowMaskBytea = nullptr;
	SysScanDesc scanDescriptor = nullptr;
	{
		MemoryContextScope ctx_guard{cxt};

		uint16_t chunkMaskSize =
			(rowCount % BIOSEQ_COLUMN_ROW_MASK_CHUNK_SIZE) ?
				(rowCount / 8 + 1) :
				(rowCount / 8);

		chunkRowMaskBytea = make1<bytea>(chunkMaskSize + VARHDRSZ);
		SET_VARSIZE(chunkRowMaskBytea, chunkMaskSize + VARHDRSZ);

		ScanKeyInit(&scanKey[0], Anum_bioseq_row_mask_storage_id,
					BTEqualStrategyNumber, F_INT8EQ, UInt64GetDatum(storageId));
		ScanKeyInit(&scanKey[1], Anum_bioseq_row_mask_start_row_number,
					BTGreaterEqualStrategyNumber, F_INT8GE, UInt64GetDatum(stripeFirstRowNumber));
		ScanKeyInit(&scanKey[2], Anum_bioseq_row_mask_end_row_number,
					BTLessEqualStrategyNumber, F_INT8LE, UInt64GetDatum(stripeFirstRowNumber + rowCount - 1));

		scanDescriptor = systable_beginscan_ordered(bioseqColumnRowMask,
													index,
													SnapshotSelf, 3, scanKey);

		int pos = 0;
		while (HeapTupleIsValid(heapTuple = systable_getnext_ordered(scanDescriptor,
																	 ForwardScanDirection)))
		{
			Datum datumArray[Natts_bioseq_row_mask];
			bool isNullArray[Natts_bioseq_row_mask];

			heap_deform_tuple(heapTuple, RelationGetDescr(bioseqColumnRowMask), datumArray, isNullArray);
			bytea * currentRowMask = DatumGetByteaP(datumArray[Anum_bioseq_row_mask_mask - 1]);

			memcpy(VARDATA(chunkRowMaskBytea) + pos,
				   VARDATA(currentRowMask),
				   VARSIZE_ANY_EXHDR(currentRowMask));

			pos += VARSIZE_ANY_EXHDR(currentRowMask);
		}
	}

	systable_endscan_ordered(scanDescriptor);
	index_close(index, AccessShareLock);
	table_close(bioseqColumnRowMask, AccessShareLock);

	return chunkRowMaskBytea;
}


export void FlushRowMaskCache(RowMaskWriteStateEntry *rowMaskEntry)
{
	HeapTuple oldHeapTuple = NULL;

	ScanKeyData scanKey;

	Oid bioseqColumnRowMaskOid = BioseqColumnRowMaskRelationId();
	Relation bioseqColumnRowMask =
		table_open(bioseqColumnRowMaskOid, AccessShareLock);
	TupleDesc tupleDescriptor = RelationGetDescr(bioseqColumnRowMask);

	Relation index = index_open(BioseqColumnRowMaskIndexRelationId(), AccessShareLock);

	ScanKeyInit(&scanKey, Anum_bioseq_row_mask_id,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(rowMaskEntry->id));

	SysScanDesc scanDescriptor = systable_beginscan_ordered(bioseqColumnRowMask,
															index,
															NULL, 1, &scanKey);

	oldHeapTuple = systable_getnext_ordered(scanDescriptor, BackwardScanDirection);

	index_close(index, AccessShareLock);

	if (HeapTupleIsValid(oldHeapTuple))
	{
		bool update[Natts_bioseq_row_mask] = { 0 };
		bool nulls[Natts_bioseq_row_mask] = { 0 };
		Datum values[Natts_bioseq_row_mask] = { 0 };

		// Update deleted row count
		update[Anum_bioseq_row_mask_deleted_rows - 1] = true;
		values[Anum_bioseq_row_mask_deleted_rows - 1] = rowMaskEntry->deletedRows;

		// Update mask byte array
		update[Anum_bioseq_row_mask_mask - 1] = true;
		values[Anum_bioseq_row_mask_mask - 1] = PointerGetDatum(rowMaskEntry->mask);

		HeapTuple newHeapTuple = heap_modify_tuple(oldHeapTuple, tupleDescriptor,
												   values, nulls, update);

		CatalogTupleUpdate(bioseqColumnRowMask, &oldHeapTuple->t_self, newHeapTuple);

		heap_freetuple(newHeapTuple);
	}

	systable_endscan_ordered(scanDescriptor);
	table_close(bioseqColumnRowMask, AccessShareLock);

	CommandCounterIncrement();
}


/*
 * FindStripeByRowNumber returns StripeMetadata for the stripe whose
 * firstRowNumber is greater than given rowNumber. If no such stripe
 * exists, then returns NULL.
 */
export StripeMetadata *
FindNextStripeByRowNumber(Relation relation, uint64_t rowNumber, Snapshot snapshot)
{
	return StripeMetadataLookupRowNumber(relation, rowNumber, snapshot, FIND_GREATER);
}


/*
 * FindStripeByRowNumber returns StripeMetadata for the stripe that contains
 * the row with rowNumber. If no such stripe exists, then returns NULL.
 */
export StripeMetadata *
FindStripeByRowNumber(Relation relation, uint64_t rowNumber, Snapshot snapshot)
{
	StripeMetadata *stripeMetadata =
		FindStripeWithMatchingFirstRowNumber(relation, rowNumber, snapshot);
	if (!stripeMetadata)
	{
		return NULL;
	}

	if (rowNumber > StripeGetHighestRowNumber(stripeMetadata))
	{
		return NULL;
	}

	return stripeMetadata;
}


/*
 * FindStripeWithMatchingFirstRowNumber returns a StripeMetadata object for
 * the stripe that has the greatest firstRowNumber among the stripes whose
 * firstRowNumber is smaller than or equal to given rowNumber. If no such
 * stripe exists, then returns NULL.
 *
 * Note that this doesn't mean that found stripe certainly contains the tuple
 * with given rowNumber. This is because, it also needs to be verified if
 * highest row number that found stripe contains is greater than or equal to
 * given rowNumber. For this reason, unless that additional check is done,
 * this function is mostly useful for checking against "possible" constraint
 * violations due to concurrent writes that are not flushed by other backends
 * yet.
 */
StripeMetadata *
FindStripeWithMatchingFirstRowNumber(Relation relation, uint64_t rowNumber,
									 Snapshot snapshot)
{
	return StripeMetadataLookupRowNumber(relation, rowNumber, snapshot,
										 FIND_LESS_OR_EQUAL);
}

/* FindNextStripeForParallelWorker returns next stripe that should be assigned
 * for worker. Approach here is to calculate module of stripe id with total number
 * of workers that are running for execution. 
 */

export StripeMetadata * 
FindNextStripeForParallelWorker(Relation relation,
								Snapshot snapshot,
								uint64_t nextStripeId,
								uint64_t * nextHigherStripeId)
{
	StripeMetadata *foundStripeMetadata = NULL;

	uint64_t storageId = BioseqColumnStorageGetStorageId(relation, false);
	ScanKeyData scanKey[2];

	ScanKeyInit(&scanKey[0], Anum_bioseq_stripe_storageid,
				BTEqualStrategyNumber, F_INT8EQ, UInt64GetDatum(storageId));

	ScanKeyInit(&scanKey[1], Anum_bioseq_stripe_stripe,
				BTGreaterEqualStrategyNumber, F_INT8GE, UInt64GetDatum(nextStripeId));

	Relation bioseqColumnStripes = table_open(BioseqColumnStripeRelationId(), AccessShareLock);

	Relation index = index_open(BioseqColumnStripePKeyIndexRelationId(),
								AccessShareLock);

	SysScanDesc scanDescriptor = systable_beginscan_ordered(bioseqColumnStripes, index,
															snapshot, 2,
															scanKey);

	HeapTuple heapTuple = systable_getnext_ordered(scanDescriptor, ForwardScanDirection);
	if (HeapTupleIsValid(heapTuple))
	{
		foundStripeMetadata = BuildStripeMetadata(bioseqColumnStripes, heapTuple);
		*nextHigherStripeId = foundStripeMetadata->id;
	}

	systable_endscan_ordered(scanDescriptor);
	index_close(index, AccessShareLock);
	table_close(bioseqColumnStripes, AccessShareLock);

	return foundStripeMetadata;
}


/*
 * StripeWriteState returns write state of given stripe.
 */
export StripeWriteStateEnum
StripeWriteState(StripeMetadata *stripeMetadata)
{
	if (stripeMetadata->aborted)
	{
		return STRIPE_WRITE_ABORTED;
	}
	else if (stripeMetadata->rowCount > 0)
	{
		return STRIPE_WRITE_FLUSHED;
	}
	else
	{
		return STRIPE_WRITE_IN_PROGRESS;
	}
}


/*
 * StripeGetHighestRowNumber returns rowNumber of the row with highest
 * rowNumber in given stripe.
 */
uint64_t
StripeGetHighestRowNumber(StripeMetadata *stripeMetadata)
{
	return stripeMetadata->firstRowNumber + stripeMetadata->rowCount - 1;
}


/*
 * StripeMetadataLookupRowNumber returns StripeMetadata for the stripe whose
 * firstRowNumber is less than or equal to (FIND_LESS_OR_EQUAL), or is
 * greater than (FIND_GREATER) given rowNumber by doing backward index
 * scan on stripe_first_row_number_idx.
 * If no such stripe exists, then returns NULL.
 */
static StripeMetadata *
StripeMetadataLookupRowNumber(Relation relation, uint64_t rowNumber, Snapshot snapshot,
							  RowNumberLookupMode lookupMode)
{
	Assert(lookupMode == FIND_LESS_OR_EQUAL || lookupMode == FIND_GREATER);

	StripeMetadata *foundStripeMetadata = NULL;

	uint64_t storageId = BioseqColumnStorageGetStorageId(relation, false);
	ScanKeyData scanKey[2];
	ScanKeyInit(&scanKey[0], Anum_bioseq_stripe_storageid,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(storageId));

	StrategyNumber strategyNumber = InvalidStrategy;
	RegProcedure procedure = InvalidOid;
	if (lookupMode == FIND_LESS_OR_EQUAL)
	{
		strategyNumber = BTLessEqualStrategyNumber;
		procedure = F_INT8LE;
	}
	else if (lookupMode == FIND_GREATER)
	{
		strategyNumber = BTGreaterStrategyNumber;
		procedure = F_INT8GT;
	}
	ScanKeyInit(&scanKey[1], Anum_bioseq_stripe_first_row_number,
				strategyNumber, procedure, UInt64GetDatum(rowNumber));


	Relation bioseqColumnStripes = table_open(BioseqColumnStripeRelationId(), AccessShareLock);
	Relation index = index_open(BioseqColumnStripeFirstRowNumberIndexRelationId(),
								AccessShareLock);
	SysScanDesc scanDescriptor = systable_beginscan_ordered(bioseqColumnStripes, index,
															snapshot, 2,
															scanKey);

	ScanDirection scanDirection = NoMovementScanDirection;
	if (lookupMode == FIND_LESS_OR_EQUAL)
	{
		scanDirection = BackwardScanDirection;
	}
	else if (lookupMode == FIND_GREATER)
	{
		scanDirection = ForwardScanDirection;
	}
	HeapTuple heapTuple = systable_getnext_ordered(scanDescriptor, scanDirection);
	if (HeapTupleIsValid(heapTuple))
	{
		foundStripeMetadata = BuildStripeMetadata(bioseqColumnStripes, heapTuple);
	}

	systable_endscan_ordered(scanDescriptor);
	index_close(index, AccessShareLock);
	table_close(bioseqColumnStripes, AccessShareLock);

	return foundStripeMetadata;
}


/*
 * CheckStripeMetadataConsistency first decides if stripe write operation for
 * given stripe is "flushed", "aborted" or "in-progress", then errors out if
 * its metadata entry contradicts with this fact.
 *
 * Checks performed here are just to catch bugs, so it is encouraged to call
 * this function whenever a StripeMetadata object is built from an heap tuple
 * of bioseq.stripe. Currently, BuildStripeMetadata is the only function
 * that does this.
 */
static void
CheckStripeMetadataConsistency(StripeMetadata *stripeMetadata)
{
	bool stripeLooksInProgress =
		stripeMetadata->rowCount == 0 && stripeMetadata->chunkCount == 0 &&
		stripeMetadata->fileOffset == BioseqColumnInvalidLogicalOffset &&
		stripeMetadata->dataLength == 0;

	/*
	 * Even if stripe is flushed, fileOffset and dataLength might be equal
	 * to 0 for zero column tables, but those two should still be consistent
	 * with respect to each other.
	 */
	bool stripeLooksFlushed =
		stripeMetadata->rowCount > 0 && stripeMetadata->chunkCount > 0 &&
		((stripeMetadata->fileOffset != BioseqColumnInvalidLogicalOffset &&
		  stripeMetadata->dataLength > 0) ||
		 (stripeMetadata->fileOffset == BioseqColumnInvalidLogicalOffset &&
		  stripeMetadata->dataLength == 0));

	StripeWriteStateEnum stripeWriteState = StripeWriteState(stripeMetadata);
	if (stripeWriteState == STRIPE_WRITE_FLUSHED && stripeLooksFlushed)
	{
		/*
		 * If stripe was flushed to disk, then we expect stripe to store
		 * at least one tuple.
		 */
		return;
	}
	else if (stripeWriteState == STRIPE_WRITE_IN_PROGRESS && stripeLooksInProgress)
	{
		/*
		 * If stripe was not flushed to disk, then values of given four
		 * fields should match the columns inserted by
		 * InsertEmptyStripeMetadataRow.
		 */
		return;
	}
	else if (stripeWriteState == STRIPE_WRITE_ABORTED && (stripeLooksInProgress ||
														  stripeLooksFlushed))
	{
		/*
		 * Stripe metadata entry for an aborted write can be complete or
		 * incomplete. We might have aborted the transaction before or after
		 * inserting into stripe metadata.
		 */
		return;
	}

	bioseq_log<ERR, PG>( "unexpected stripe state, stripe metadata entry for stripe with id={} is not consistent", stripeMetadata->id);
}


/*
 * FindStripeWithHighestRowNumber returns StripeMetadata for the stripe that
 * has the row with highest rowNumber by doing backward index scan on
 * stripe_first_row_number_idx. If given relation is empty, then returns NULL.
 */
export StripeMetadata *
FindStripeWithHighestRowNumber(Relation relation, Snapshot snapshot)
{
	StripeMetadata *stripeWithHighestRowNumber = NULL;

	uint64_t storageId = BioseqColumnStorageGetStorageId(relation, false);
	ScanKeyData scanKey[1];
	ScanKeyInit(&scanKey[0], Anum_bioseq_stripe_storageid,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(storageId));

	Relation bioseqColumnStripes = table_open(BioseqColumnStripeRelationId(), AccessShareLock);
	Relation index = index_open(BioseqColumnStripeFirstRowNumberIndexRelationId(),
								AccessShareLock);
	SysScanDesc scanDescriptor = systable_beginscan_ordered(bioseqColumnStripes, index,
															snapshot, 1, scanKey);

	HeapTuple heapTuple = systable_getnext_ordered(scanDescriptor, BackwardScanDirection);
	if (HeapTupleIsValid(heapTuple))
	{
		stripeWithHighestRowNumber = BuildStripeMetadata(bioseqColumnStripes, heapTuple);
	}

	systable_endscan_ordered(scanDescriptor);
	index_close(index, AccessShareLock);
	table_close(bioseqColumnStripes, AccessShareLock);

	return stripeWithHighestRowNumber;
}


/*
 * ReadChunkGroupRowCounts returns an array of row counts of chunk groups and 
 * deleted rows count for each chunk group for given stripe.
 * Arrays that are updated will be allocated in this function.
 */
static void
ReadChunkGroupRowCounts(uint64_t storageId, uint64_t stripe, uint32_t chunkGroupCount,
						uint32_t **chunkGroupRowCounts, uint32_t **chunkGroupDeletedRows,
						Snapshot snapshot)
{
	Oid bioseqColumnChunkGroupOid = BioseqColumnChunkGroupRelationId();
	Relation bioseqColumnChunkGroup = table_open(bioseqColumnChunkGroupOid, AccessShareLock);
	Relation index = index_open(BioseqColumnChunkGroupIndexRelationId(), AccessShareLock);

	ScanKeyData scanKey[2];
	ScanKeyInit(&scanKey[0], Anum_bioseq_chunkgroup_storageid,
				BTEqualStrategyNumber, F_INT8EQ, UInt64GetDatum(storageId));
	ScanKeyInit(&scanKey[1], Anum_bioseq_chunkgroup_stripe,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(stripe));

	SysScanDesc scanDescriptor =
		systable_beginscan_ordered(bioseqColumnChunkGroup, index, snapshot, 2, scanKey);

	HeapTuple heapTuple = NULL;

	*chunkGroupRowCounts = make_array<uint32_t>(chunkGroupCount);
	*chunkGroupDeletedRows = make_array<uint32_t>(chunkGroupCount);

	/*
	 * Since we have now updates of `chunk_group`, there could be multiple tuples
	 * retrieved with changed only deleted_row count. We expect that last modified
	 * version will be retrieved last so it we just update information based on tuple
	 * chunk group index.
	 */
	while (HeapTupleIsValid(heapTuple = systable_getnext_ordered(scanDescriptor,
																 ForwardScanDirection)))
	{
		if (HeapTupleHeaderIsHotUpdated(heapTuple->t_data))
			continue;

		Datum datumArray[Natts_bioseq_chunkgroup];
		bool isNullArray[Natts_bioseq_chunkgroup];

		heap_deform_tuple(heapTuple,
						  RelationGetDescr(bioseqColumnChunkGroup),
						  datumArray, isNullArray);

		uint32_t tupleChunkGroupIndex =
			DatumGetUInt32(datumArray[Anum_bioseq_chunkgroup_chunk - 1]);

		if (tupleChunkGroupIndex >= chunkGroupCount)
		{
			bioseq_log<WRN, PG>( "Tuple chunk group higher than chunk group count: {}, {} (storage_id = {}, stripe_id = {})", tupleChunkGroupIndex, chunkGroupCount, storageId, stripe);
			continue;
		}

		(*chunkGroupRowCounts)[tupleChunkGroupIndex] =
			static_cast<uint32_t>(DatumGetUInt64(datumArray[Anum_bioseq_chunkgroup_row_count - 1]));

		(*chunkGroupDeletedRows)[tupleChunkGroupIndex] =
			static_cast<uint32_t>(DatumGetUInt64(datumArray[Anum_bioseq_chunkgroup_deleted_rows - 1]));
	}

	systable_endscan_ordered(scanDescriptor);
	index_close(index, AccessShareLock);
	table_close(bioseqColumnChunkGroup, AccessShareLock);
}


/*
 * UpdateChunkGroupDeletedRows updates `deleted_rows` column for each chunk group.
 */
export void
UpdateChunkGroupDeletedRows(uint64_t storageId, uint64_t stripe, 
							uint32_t chunkGroupId, uint32_t deletedRowNumber)
{

	HeapTuple oldHeapTuple = NULL;

	Oid bioseqColumnChunkGroupOid = BioseqColumnChunkGroupRelationId();
	Relation bioseqColumnChunkGroup = table_open(bioseqColumnChunkGroupOid, AccessShareLock);
	TupleDesc tupleDescriptor = RelationGetDescr(bioseqColumnChunkGroup);
	Relation index = index_open(BioseqColumnChunkGroupIndexRelationId(), AccessShareLock);

	ScanKeyData scanKey[3];
	ScanKeyInit(&scanKey[0], Anum_bioseq_chunkgroup_storageid,
				BTEqualStrategyNumber, F_INT8EQ, UInt64GetDatum(storageId));
	ScanKeyInit(&scanKey[1], Anum_bioseq_chunkgroup_stripe,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(stripe));
	ScanKeyInit(&scanKey[2], Anum_bioseq_chunkgroup_chunk,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(chunkGroupId));

	SysScanDesc scanDescriptor =
		systable_beginscan_ordered(bioseqColumnChunkGroup, index, NULL, 3, scanKey);

	oldHeapTuple = systable_getnext_ordered(scanDescriptor, BackwardScanDirection);

	index_close(index, AccessShareLock);

	if (HeapTupleIsValid(oldHeapTuple))
	{
		bool update[Natts_bioseq_chunkgroup] = { 0 };
		bool nulls[Natts_bioseq_chunkgroup] = { 0 };
		Datum values[Natts_bioseq_chunkgroup] = { 0 };

		// Update deleted row count
		update[Anum_bioseq_chunkgroup_deleted_rows - 1] = true;
		values[Anum_bioseq_chunkgroup_deleted_rows - 1] = Int64GetDatum(deletedRowNumber);

		HeapTuple newHeapTuple = heap_modify_tuple(oldHeapTuple, tupleDescriptor,
												   values, nulls, update);

		CatalogTupleUpdate(bioseqColumnChunkGroup, &oldHeapTuple->t_self, newHeapTuple);

		heap_freetuple(newHeapTuple);
	}

	systable_endscan_ordered(scanDescriptor);
	table_close(bioseqColumnChunkGroup, AccessShareLock);

	CommandCounterIncrement();
}


/*
 * InsertEmptyStripeMetadataRow adds a row to bioseq.stripe for the empty
 * stripe reservation made for stripeId.
 */
static void
InsertEmptyStripeMetadataRow(uint64_t storageId, uint64_t stripeId, uint32_t columnCount,
							 uint32_t chunkGroupRowCount, uint64_t firstRowNumber)
{
	bool nulls[Natts_bioseq_stripe] = { false };

	Datum values[Natts_bioseq_stripe] = { 0 };
	values[Anum_bioseq_stripe_storageid - 1] =
		UInt64GetDatum(storageId);
	values[Anum_bioseq_stripe_stripe - 1] =
		UInt64GetDatum(stripeId);
	values[Anum_bioseq_stripe_column_count - 1] =
		UInt32GetDatum(columnCount);
	values[Anum_bioseq_stripe_chunk_row_count - 1] =
		UInt32GetDatum(chunkGroupRowCount);
	values[Anum_bioseq_stripe_first_row_number - 1] =
		UInt64GetDatum(firstRowNumber);

	/* stripe has no rows yet, so initialize rest of the columns accordingly */
	values[Anum_bioseq_stripe_row_count - 1] =
		UInt64GetDatum(0);
	values[Anum_bioseq_stripe_file_offset - 1] =
		UInt64GetDatum(BioseqColumnInvalidLogicalOffset);
	values[Anum_bioseq_stripe_data_length - 1] =
		UInt64GetDatum(0);
	values[Anum_bioseq_stripe_chunk_count - 1] =
		UInt32GetDatum(0);

	Oid bioseqColumnStripesOid = BioseqColumnStripeRelationId();
	Relation bioseqColumnStripes = table_open(bioseqColumnStripesOid, RowExclusiveLock);

	bioseq::metadata::tuple_ops::ModifyState *modifyState = bioseq::metadata::tuple_ops::start_modify_relation(bioseqColumnStripes);

	bioseq::metadata::tuple_ops::insert_tuple_and_enforce_constraints(modifyState, values, nulls);

	bioseq::metadata::tuple_ops::finish_modify_relation(modifyState);

	table_close(bioseqColumnStripes, RowExclusiveLock);
}


/*
 * StripesForRelfilenode returns a list of StripeMetadata for stripes
 * of the given relfilelocator.
 */
export List *
StripesForRelfilenode(RelFileLocator relfilelocator, ScanDirection scanDirection)
{
	uint64_t storageId = LookupStorageId(relfilelocator);

	return ReadDataFileStripeList(storageId, GetTransactionSnapshot(), scanDirection);
}


/*
 * BioseqColumnTableRowCount returns the exact row count of a table using skiplists.
 *
 * DOC0322 §7.4 — moved here from table/columnar_customscan.cppm (where it was an
 * `extern "C++"` reach). Its body iterates StripesForRelfilenode + the
 * PList<StripeMetadata> view, both owned by storage_metadata:core, so it lives
 * here and is reached by callers via `import storage_metadata;`.
 */
export uint64_t
BioseqColumnTableRowCount(Relation relation)
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
 * DeletedRowsForStripe returns number of deleted rows for stripe
 * of the given relfilelocator.
 */
export uint32_t
DeletedRowsForStripe(RelFileLocator relfilelocator, uint32_t chunkCount, uint64_t stripeId)
{
	uint64_t storageId = LookupStorageId(relfilelocator);

	uint32_t *chunkGroupRowCounts;
	uint32_t *chunkGroupDeletedRows;
	int i;

	uint32_t deletedRows = 0;
	
	ReadChunkGroupRowCounts(storageId, stripeId, chunkCount,
							&chunkGroupRowCounts, &chunkGroupDeletedRows,
							GetTransactionSnapshot());

	for (i = 0; i < chunkCount; i++)
	{
		deletedRows += chunkGroupDeletedRows[i];
	}

	pfree(chunkGroupRowCounts);
	pfree(chunkGroupDeletedRows);

	return deletedRows;
}

/*
 * DecompressedLengthForStripe returns total size of all decompressed rows and chunk
 * for given stripe
 */
export Size
DecompressedLengthForStripe(RelFileLocator relfilelocator, uint64_t stripeId)
{
	HeapTuple heapTuple = NULL;
	ScanKeyData scanKey[2];

	uint64_t storageId = LookupStorageId(relfilelocator);

	Oid bioseqColumnChunkOid = BioseqColumnChunkRelationId();
	Relation bioseqColumnChunk = table_open(bioseqColumnChunkOid, AccessShareLock);
	Relation index = index_open(BioseqColumnChunkIndexRelationId(), AccessShareLock);

	ScanKeyInit(&scanKey[0], Anum_bioseq_chunk_storageid,
				BTEqualStrategyNumber, F_INT8EQ, UInt64GetDatum(storageId));
	ScanKeyInit(&scanKey[1], Anum_bioseq_chunk_stripe,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(stripeId));

	SysScanDesc scanDescriptor = systable_beginscan_ordered(bioseqColumnChunk, index,
															GetTransactionSnapshot(), 2, scanKey);

	Size decompressedChunkSize = 0;

	while (HeapTupleIsValid(heapTuple = systable_getnext_ordered(scanDescriptor,
																 ForwardScanDirection)))
	{
		Datum datumArray[Natts_bioseq_chunk];
		bool isNullArray[Natts_bioseq_chunk];

		heap_deform_tuple(heapTuple, RelationGetDescr(bioseqColumnChunk), datumArray,
						  isNullArray);

		decompressedChunkSize +=
			DatumGetInt64(datumArray[Anum_bioseq_chunk_value_decompressed_size - 1]);
	}

	systable_endscan_ordered(scanDescriptor);
	index_close(index, AccessShareLock);
	table_close(bioseqColumnChunk, AccessShareLock);

	return decompressedChunkSize;
}


/*
 * GetHighestUsedAddress returns the highest used address for the given
 * relfilelocator across all active and inactive transactions.
 *
 * This is used by truncate stage of VACUUM, and VACUUM can be called
 * for empty tables. So this doesn't throw errors for empty tables and
 * returns 0.
 */
export uint64_t
GetHighestUsedAddress(RelFileLocator relfilelocator)
{
	uint64_t storageId = LookupStorageId(relfilelocator);

	uint64_t highestUsedAddress = 0;
	uint64_t highestUsedId = 0;
	GetHighestUsedAddressAndId(storageId, &highestUsedAddress, &highestUsedId);

	return highestUsedAddress;
}


/*
 * GetHighestUsedAddressAndId returns the highest used address and id for
 * the given relfilelocator across all active and inactive transactions.
 */
static void
GetHighestUsedAddressAndId(uint64_t storageId,
						   uint64_t *highestUsedAddress,
						   uint64_t *highestUsedId)
{
	ListCell *stripeMetadataCell = NULL;

	SnapshotData SnapshotDirty;
	InitDirtySnapshot(SnapshotDirty);

	List *stripeMetadataList = ReadDataFileStripeList(storageId, &SnapshotDirty,
													  ForwardScanDirection);

	*highestUsedId = 0;

	/* file starts with metapage */
	*highestUsedAddress = BIOSEQ_COLUMN_BYTES_PER_PAGE;

	for (auto stripe : PList<StripeMetadata>(stripeMetadataList))
	{
		uint64_t lastByte = stripe->fileOffset + stripe->dataLength - 1;
		*highestUsedAddress = std::max(*highestUsedAddress, lastByte);
		*highestUsedId = std::max(*highestUsedId, static_cast<uint64_t>(stripe->id));
	}
}


/*
 * ReserveEmptyStripe reserves an empty stripe for given relation
 * and inserts it into bioseq.stripe. It is guaranteed that concurrent
 * writes won't overwrite the returned stripe.
 */
export EmptyStripeReservation *
ReserveEmptyStripe(Relation rel, uint64_t columnCount, uint64_t chunkGroupRowCount,
				   uint64_t stripeRowCount)
{
	EmptyStripeReservation *stripeReservation = make0<EmptyStripeReservation>();

	uint64_t storageId = BioseqColumnStorageGetStorageId(rel, false);

	stripeReservation->stripeId = BioseqColumnStorageReserveStripeId(rel);
	stripeReservation->stripeFirstRowNumber =
		BioseqColumnStorageReserveRowNumber(rel, stripeRowCount);

	/*
	 * XXX: Instead of inserting a dummy entry to bioseq.stripe and
	 * updating it when flushing the stripe, we could have a hash table
	 * in shared memory for the bookkeeping of ongoing writes.
	 */
	InsertEmptyStripeMetadataRow(storageId, stripeReservation->stripeId,
								 columnCount, chunkGroupRowCount,
								 stripeReservation->stripeFirstRowNumber);

	return stripeReservation;
}


/*
 * CompleteStripeReservation completes reservation of the stripe with
 * stripeId for given size and in-place updates related stripe metadata tuple
 * to complete reservation.
 */
export StripeMetadata *
CompleteStripeReservation(Relation rel, uint64_t stripeId, uint64_t sizeBytes,
						  uint64_t rowCount, uint64_t chunkCount)
{
	uint64_t resLogicalStart = BioseqColumnStorageReserveData(rel, sizeBytes);
	uint64_t storageId = BioseqColumnStorageGetStorageId(rel, false);

	bool update[Natts_bioseq_stripe] = { false };
	update[Anum_bioseq_stripe_file_offset - 1] = true;
	update[Anum_bioseq_stripe_data_length - 1] = true;
	update[Anum_bioseq_stripe_row_count - 1] = true;
	update[Anum_bioseq_stripe_chunk_count - 1] = true;

	Datum newValues[Natts_bioseq_stripe] = { 0 };
	newValues[Anum_bioseq_stripe_file_offset - 1] = Int64GetDatum(resLogicalStart);
	newValues[Anum_bioseq_stripe_data_length - 1] = Int64GetDatum(sizeBytes);
	newValues[Anum_bioseq_stripe_row_count - 1] = UInt64GetDatum(rowCount);
	newValues[Anum_bioseq_stripe_chunk_count - 1] = Int32GetDatum(chunkCount);

	return UpdateStripeMetadataRow(storageId, stripeId, update, newValues);
}


/*
 * UpdateStripeMetadataRow updates stripe metadata tuple for the stripe with
 * stripeId according to given newValues and update arrays.
 * Note that this function shouldn't be used for the cases where any indexes
 * of stripe metadata should be updated according to modifications done.
 *
 * This was originally implemented on top of heap_inplace_update(), which was
 * removed in PG-18. The systable_inplace_update_* API is available in both
 * PG-17 and PG-18 and is the upstream-recommended replacement, so we use it
 * unconditionally rather than maintaining two versions of the function body.
 */
static StripeMetadata *
UpdateStripeMetadataRow(uint64_t storageId, uint64_t stripeId, bool *update,
						Datum *newValues)
{
	ScanKeyData scanKey[2];
	ScanKeyInit(&scanKey[0], Anum_bioseq_stripe_storageid,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(storageId));
	ScanKeyInit(&scanKey[1], Anum_bioseq_stripe_stripe,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(stripeId));

	Oid bioseqColumnStripesOid = BioseqColumnStripeRelationId();
	Relation bioseqColumnStripes = table_open(bioseqColumnStripesOid, AccessShareLock);
	TupleDesc tupleDescriptor = RelationGetDescr(bioseqColumnStripes);

	Oid indexId = BioseqColumnStripePKeyIndexRelationId();
	bool indexOk = OidIsValid(indexId);

	void *state = nullptr;
	HeapTuple oldTuple = nullptr;
	systable_inplace_update_begin(bioseqColumnStripes, indexId, indexOk,
								  /* snapshot */ nullptr,
								  2, scanKey, &oldTuple, &state);

	if (!HeapTupleIsValid(oldTuple))
	{
		systable_inplace_update_cancel(state);
		table_close(bioseqColumnStripes, AccessShareLock);
		bioseq_log<ERR, PG>( "attempted to modify an unexpected stripe, columnar storage with id={} does not have stripe with id={}", storageId, stripeId);
	}

	/*
	 * systable_inplace_update_finish doesn't allow changing the size of the
	 * original tuple, so we don't allow setting any Datum's to NULL values.
	 */
	bool newNulls[Natts_bioseq_stripe] = { false };
	HeapTuple modifiedTuple = heap_modify_tuple(oldTuple, tupleDescriptor,
												newValues, newNulls, update);

	systable_inplace_update_finish(state, modifiedTuple);

	/*
	 * BuildStripeMetadata expects a real heap tuple with MVCC fields, which
	 * is what oldTuple is — the in-place update preserved its header.
	 */
	StripeMetadata *modifiedStripeMetadata = BuildStripeMetadata(bioseqColumnStripes,
																 modifiedTuple);

	CommandCounterIncrement();

	heap_freetuple(modifiedTuple);
	table_close(bioseqColumnStripes, AccessShareLock);

	/* return StripeMetadata object built from modified tuple */
	return modifiedStripeMetadata;
}


/*
 * ReadDataFileStripeList reads the stripe list for a given storageId
 * in the given snapshot.
 */
static List *
ReadDataFileStripeList(uint64_t storageId, Snapshot snapshot, ScanDirection scanDirection)
{
	PList<StripeMetadata> stripeMetadataList;
	ScanKeyData scanKey[1];
	HeapTuple heapTuple;

	ScanKeyInit(&scanKey[0], Anum_bioseq_stripe_storageid,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(storageId));

	Oid bioseqColumnStripesOid = BioseqColumnStripeRelationId();

	Relation bioseqColumnStripes = table_open(bioseqColumnStripesOid, AccessShareLock);
	Relation index = index_open(BioseqColumnStripeFirstRowNumberIndexRelationId(),
								AccessShareLock);

	SysScanDesc scanDescriptor = systable_beginscan_ordered(bioseqColumnStripes, index,
															snapshot, 1,
															scanKey);

	while (HeapTupleIsValid(heapTuple = systable_getnext_ordered(scanDescriptor,
																 scanDirection)))
	{
		StripeMetadata *stripeMetadata = BuildStripeMetadata(bioseqColumnStripes, heapTuple);
		stripeMetadataList.append(stripeMetadata);
	}

	systable_endscan_ordered(scanDescriptor);
	index_close(index, AccessShareLock);
	table_close(bioseqColumnStripes, AccessShareLock);

	return stripeMetadataList;
}


/*
 * BuildStripeMetadata builds a StripeMetadata object from given heap tuple.
 *
 * NB: heapTuple must be a proper heap tuple with MVCC fields.
 */
StripeMetadata *
BuildStripeMetadata(Relation bioseqColumnStripes, HeapTuple heapTuple)
{
	Assert(RelationGetRelid(bioseqColumnStripes) == BioseqColumnStripeRelationId());

	Datum datumArray[Natts_bioseq_stripe];
	bool isNullArray[Natts_bioseq_stripe];
	heap_deform_tuple(heapTuple, RelationGetDescr(bioseqColumnStripes),
					  datumArray, isNullArray);

	StripeMetadata *stripeMetadata = make0<StripeMetadata>();
	stripeMetadata->id = DatumGetInt64(datumArray[Anum_bioseq_stripe_stripe - 1]);
	stripeMetadata->fileOffset = DatumGetInt64(
		datumArray[Anum_bioseq_stripe_file_offset - 1]);
	stripeMetadata->dataLength = DatumGetInt64(
		datumArray[Anum_bioseq_stripe_data_length - 1]);
	stripeMetadata->columnCount = DatumGetInt32(
		datumArray[Anum_bioseq_stripe_column_count - 1]);
	stripeMetadata->chunkCount = DatumGetInt32(
		datumArray[Anum_bioseq_stripe_chunk_count - 1]);
	stripeMetadata->chunkGroupRowCount = DatumGetInt32(
		datumArray[Anum_bioseq_stripe_chunk_row_count - 1]);
	stripeMetadata->rowCount = DatumGetInt64(
		datumArray[Anum_bioseq_stripe_row_count - 1]);
	stripeMetadata->firstRowNumber = DatumGetUInt64(
		datumArray[Anum_bioseq_stripe_first_row_number - 1]);

	/*
	 * If there is unflushed data in a parent transaction, then we would
	 * have already thrown an error before starting to scan the table.. If
	 * the data is from an earlier subxact that committed, then it would
	 * have been flushed already. For this reason, we don't care about
	 * subtransaction id here.
	 */
	TransactionId entryXmin = HeapTupleHeaderGetXmin(heapTuple->t_data);
	stripeMetadata->aborted = !TransactionIdIsInProgress(entryXmin) &&
							  TransactionIdDidAbort(entryXmin);
	stripeMetadata->insertedByCurrentXact =
		TransactionIdIsCurrentTransactionId(entryXmin);

	CheckStripeMetadataConsistency(stripeMetadata);

	return stripeMetadata;
}


/*
 * DeleteMetadataRows removes the rows with given relfilelocator from columnar
 * metadata tables.
 */
export void
DeleteMetadataRows(RelFileLocator relfilelocator)
{
	/*
	 * During a restore for binary upgrade, metadata tables and indexes may or
	 * may not exist.
	 */
	if (IsBinaryUpgrade)
	{
		return;
	}

	uint64_t storageId = LookupStorageId(relfilelocator);

	DeleteStorageFromBioseqColumnMetadataTable(BioseqColumnStripeRelationId(),
										   Anum_bioseq_stripe_storageid,
										   BioseqColumnStripePKeyIndexRelationId(),
										   storageId);
	DeleteStorageFromBioseqColumnMetadataTable(BioseqColumnChunkGroupRelationId(),
										   Anum_bioseq_chunkgroup_storageid,
										   BioseqColumnChunkGroupIndexRelationId(),
										   storageId);
	DeleteStorageFromBioseqColumnMetadataTable(BioseqColumnChunkRelationId(),
										   Anum_bioseq_chunk_storageid,
										   BioseqColumnChunkIndexRelationId(),
										   storageId);
	DeleteStorageFromBioseqColumnMetadataTable(BioseqColumnRowMaskRelationId(),
										   Anum_bioseq_row_mask_storage_id,
										   BioseqColumnRowMaskIndexRelationId(),
										   storageId);
}


/*
 * DeleteMetadataRowsForStripeId removes the rows with given relfilelocator and 
 * stripe id from columnar metadata tables.
 */
export void
DeleteMetadataRowsForStripeId(RelFileLocator relfilelocator, uint64_t stripeId)
{
	/*
	 * During a restore for binary upgrade, metadata tables and indexes may or
	 * may not exist.
	 */
	if (IsBinaryUpgrade)
	{
		return;
	}

	uint64_t storageId = LookupStorageId(relfilelocator);

	DeleteStripeFromBioseqColumnMetadataTable(
		BioseqColumnStripeRelationId(),
		Anum_bioseq_stripe_storageid,
		Anum_bioseq_stripe_stripe,
		BioseqColumnStripePKeyIndexRelationId(),
		storageId, stripeId);
	DeleteStripeFromBioseqColumnMetadataTable(
		BioseqColumnChunkGroupRelationId(),
		Anum_bioseq_chunkgroup_storageid,
		Anum_bioseq_chunkgroup_stripe,
		BioseqColumnChunkGroupIndexRelationId(),
		storageId, stripeId);
	DeleteStripeFromBioseqColumnMetadataTable(
		BioseqColumnChunkRelationId(),
		Anum_bioseq_chunk_storageid,
		Anum_bioseq_chunk_stripe,
		BioseqColumnChunkIndexRelationId(),
		storageId, stripeId);
	DeleteStripeFromBioseqColumnMetadataTable(
		BioseqColumnRowMaskRelationId(),
		Anum_bioseq_row_mask_storage_id,
		Anum_bioseq_row_mask_stripe_id,
		BioseqColumnRowMaskStripeIndexRelationId(),
		storageId, stripeId);
}


/*
 * DeleteStorageFromBioseqColumnMetadataTable removes the rows with given
 * storageId from given columnar metadata table.
 */
static void
DeleteStorageFromBioseqColumnMetadataTable(Oid metadataTableId,
									   AttrNumber storageIdAtrrNumber,
									   Oid storageIdIndexId, uint64_t storageId)
{
	ScanKeyData scanKey[1];
	ScanKeyInit(&scanKey[0], storageIdAtrrNumber, BTEqualStrategyNumber,
				F_INT8EQ, UInt64GetDatum(storageId));

	Relation metadataTable = try_safe_relation_open(metadataTableId, AccessShareLock);
	if (metadataTable == NULL)
	{
		/* extension has been dropped */
		return;
	}

	Relation index = index_open(storageIdIndexId, AccessShareLock);

	SysScanDesc scanDescriptor = systable_beginscan_ordered(metadataTable, index, NULL,
															1, scanKey);

	bioseq::metadata::tuple_ops::ModifyState *modifyState = bioseq::metadata::tuple_ops::start_modify_relation(metadataTable);

	HeapTuple heapTuple;
	while (HeapTupleIsValid(heapTuple = systable_getnext_ordered(scanDescriptor,
																 ForwardScanDirection)))
	{
		bioseq::metadata::tuple_ops::delete_tuple_and_enforce_constraints(modifyState, heapTuple);
	}

	systable_endscan_ordered(scanDescriptor);

	bioseq::metadata::tuple_ops::finish_modify_relation(modifyState);

	index_close(index, AccessShareLock);
	table_close(metadataTable, AccessShareLock);
}


/*
 * DeleteStripeFromBioseqColumnMetadataTable removes the rows in columnar
 * metadata table that match storageId and stripeId.
 */
static void
DeleteStripeFromBioseqColumnMetadataTable(Oid metadataTableId,
									  AttrNumber storageIdAtrrNumber,
									  AttrNumber stripeIdAttrNumber,
									  Oid storageIdIndexId,
									  uint64_t storageId,
									  uint64_t stripeId)
{
	ScanKeyData scanKey[2];
	ScanKeyInit(&scanKey[0], storageIdAtrrNumber, BTEqualStrategyNumber,
				F_INT8EQ, UInt64GetDatum(storageId));
	ScanKeyInit(&scanKey[1], stripeIdAttrNumber, BTEqualStrategyNumber,
				F_INT8EQ, UInt64GetDatum(stripeId));

	Relation metadataTable = try_safe_relation_open(metadataTableId, RowShareLock);
	if (metadataTable == NULL)
	{
		/* extension has been dropped */
		return;
	}

	Relation index = index_open(storageIdIndexId, RowShareLock);
	SysScanDesc scanDescriptor = systable_beginscan_ordered(metadataTable, index, NULL,
															2, scanKey);

	bioseq::metadata::tuple_ops::ModifyState *modifyState = bioseq::metadata::tuple_ops::start_modify_relation(metadataTable);

	HeapTuple heapTuple;
	while (HeapTupleIsValid(heapTuple = systable_getnext_ordered(scanDescriptor,
																 ForwardScanDirection)))
	{
		bioseq::metadata::tuple_ops::delete_tuple_and_enforce_constraints(modifyState, heapTuple);
	}

	systable_endscan_ordered(scanDescriptor);

	bioseq::metadata::tuple_ops::finish_modify_relation(modifyState);

	index_close(index, RowShareLock);
	table_close(metadataTable, RowShareLock);
}


/*
 * BioseqColumnStorageIdSequenceRelationId returns relation id of bioseq.stripe.
 * TODO: should we cache this similar to citus?
 */
static Oid
BioseqColumnStorageIdSequenceRelationId(void)
{
	return safe_get_relname_relid("storageid_seq", BioseqColumnNamespaceId());
}


/*
 * BioseqColumnStripeRelationId returns relation id of bioseq.stripe.
 * TODO: should we cache this similar to citus?
 */
Oid
BioseqColumnStripeRelationId(void)
{
	return safe_get_relname_relid("stripe", BioseqColumnNamespaceId());
}


/*
 * BioseqColumnStripePKeyIndexRelationId returns relation id of bioseq.stripe_pkey.
 * TODO: should we cache this similar to citus?
 */
Oid
BioseqColumnStripePKeyIndexRelationId(void)
{
	return safe_get_relname_relid("stripe_pkey", BioseqColumnNamespaceId());
}


/*
 * BioseqColumnStripeFirstRowNumberIndexRelationId returns relation id of
 * bioseq.stripe_first_row_number_idx.
 * TODO: should we cache this similar to citus?
 */
static Oid
BioseqColumnStripeFirstRowNumberIndexRelationId(void)
{
	return safe_get_relname_relid("stripe_first_row_number_idx", BioseqColumnNamespaceId());
}


/*
 * BioseqColumnOptionsRelationId returns relation id of bioseq.options.
 */
static Oid
BioseqColumnOptionsRelationId(void)
{

	return safe_get_relname_relid("options", BioseqColumnNamespaceId());
}


/*
 * BioseqColumnOptionsIndexRegclass returns relation id of bioseq.options_pkey.
 */
static Oid
BioseqColumnOptionsIndexRegclass(void)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnOptionsIndexRegclass() entered");
	return safe_get_relname_relid("options_pkey", BioseqColumnNamespaceId());
}


/*
 * BioseqColumnChunkRelationId returns relation id of bioseq.chunk.
 * TODO: should we cache this similar to citus?
 */
static Oid
BioseqColumnChunkRelationId(void)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnChunkRelationId() entered");
	return safe_get_relname_relid("chunk", BioseqColumnNamespaceId());
}


/*
 * BioseqColumnChunkGroupRelationId returns relation id of bioseq.chunk_group.
 * TODO: should we cache this similar to citus?
 */
static Oid
BioseqColumnChunkGroupRelationId(void)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnChunkGroupRelationId() entered");
	return safe_get_relname_relid("chunk_group", BioseqColumnNamespaceId());
}


/*
 * BioseqColumnRowMaskRelationId returns relation id of bioseq.row_mask.
 */
Oid
BioseqColumnRowMaskRelationId(void)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnRowMaskRelationId() entered");
	return safe_get_relname_relid("row_mask", BioseqColumnNamespaceId());
}


/*
 * BioseqColumnRowMaskSeqId returns relation id of bioseq.row_mask_seq.
 */
static Oid
BioseqColumnRowMaskSeqId(void)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnRowMaskSeqId() entered");
	return safe_get_relname_relid("row_mask_seq", BioseqColumnNamespaceId());
}



/*
 * BioseqColumnChunkIndexRelationId returns relation id of bioseq.chunk_pkey.
 * TODO: should we cache this similar to citus?
 */
static Oid
BioseqColumnChunkIndexRelationId(void)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnChunkIndexRelationId() entered");
	return safe_get_relname_relid("chunk_pkey", BioseqColumnNamespaceId());
}


/*
 * BioseqColumnChunkGroupIndexRelationId returns relation id of bioseq.chunk_group_pkey.
 * TODO: should we cache this similar to citus?
 */
static Oid
BioseqColumnChunkGroupIndexRelationId(void)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnChunkGroupIndexRelationId() entered");
	return safe_get_relname_relid("chunk_group_pkey", BioseqColumnNamespaceId());
}


/*
 * BioseqColumnRowMaskIndexRelationId returns relation id 
 * of bioseq.row_mask_pkey
 */
Oid
BioseqColumnRowMaskIndexRelationId(void)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnRowMaskIndexRelationId() entered");
	return safe_get_relname_relid("row_mask_pkey", BioseqColumnNamespaceId());
}

/*
 * BioseqColumnRowMaskStripeIndexRelationId returns relation id 
 * of bioseq.bioseq_row_mask_stripe_unique
 */
static Oid
BioseqColumnRowMaskStripeIndexRelationId(void)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnRowMaskStripeIndexRelationId() entered");
	return safe_get_relname_relid("row_mask_stripe_unique", BioseqColumnNamespaceId());
}


/*
 * BioseqColumnNamespaceId returns namespace id of the schema we store columnar
 * related tables.
 */
static Oid
BioseqColumnNamespaceId(void)
{
	Oid nspace = get_namespace_oid("bioseq_internal", true);
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnNamespaceId() looking up bioseq namespace");
	/* if schema is earlier than 11.1-1 */
	if (!OidIsValid(nspace))
	{
		nspace = get_namespace_oid("bioseq", false);
	}
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnNamespaceId() returning oid");
	return nspace;
}


/*
 * LookupStorageId reads storage metapage to find the storage ID for the given relfilelocator. It returns
 * false if the relation doesn't have a meta page yet.
 */
uint64_t
LookupStorageId(RelFileLocator relfilelocator)
{
	bioseq_log<DBG>("[bioseq] pg: LookupStorageId() entered");
	Oid relationId = RelidByRelfilenumber(RelationTablespace_compat(relfilelocator),
										  RelationPhysicalIdentifierNumber_compat(
											  relfilelocator));

	Relation relation = safe_relation_open(relationId, AccessShareLock);
	uint64_t storageId = BioseqColumnStorageGetStorageId(relation, false);
	table_close(relation, AccessShareLock);

	return storageId;
}


/*
 * BioseqColumnMetadataNewStorageId - create a new, unique storage id and return
 * it.
 */
export uint64_t
BioseqColumnMetadataNewStorageId()
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnMetadataNewStorageId() entered");
	return nextval_internal(BioseqColumnStorageIdSequenceRelationId(), false);
}


/*
 * BioseqColumnStorageUpdateIfNeeded - upgrade columnar storage to the current version by
 * using information from the metadata tables.
 */
export void
BioseqColumnStorageUpdateIfNeeded(Relation rel, bool isUpgrade)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnStorageUpdateIfNeeded() entered");
	if (BioseqColumnStorageIsCurrent(rel))
	{
		return;
	}

	BlockNumber nblocks = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnStorageUpdateIfNeeded() checking nblocks<2");
	if (nblocks < 2)
	{
		BioseqColumnStorageInit(rel->rd_smgr, BioseqColumnMetadataNewStorageId());
		return;
	}

	uint64_t storageId = BioseqColumnStorageGetStorageId(rel, true);

	uint64_t highestId;
	uint64_t highestOffset;
	GetHighestUsedAddressAndId(storageId, &highestOffset, &highestId);

	uint64_t reservedStripeId = highestId + 1;
	uint64_t reservedOffset = highestOffset + 1;
	uint64_t reservedRowNumber = GetHighestUsedRowNumber(storageId) + 1;
	BioseqColumnStorageUpdateCurrent(rel, isUpgrade, reservedStripeId,
								 reservedRowNumber, reservedOffset);
}


/*
 * GetHighestUsedRowNumber returns the highest used rowNumber for given
 * storageId. Returns BIOSEQ_COLUMN_INVALID_ROW_NUMBER if storage with
 * storageId has no stripes.
 * Note that normally we would use BioseqColumnStorageGetReservedRowNumber
 * to decide that. However, this function is designed to be used when
 * building the metapage itself during upgrades.
 */
static uint64_t
GetHighestUsedRowNumber(uint64_t storageId)
{
	uint64_t highestRowNumber = BIOSEQ_COLUMN_INVALID_ROW_NUMBER;

	List *stripeMetadataList = ReadDataFileStripeList(storageId,
													  GetTransactionSnapshot(),
													  ForwardScanDirection);
	for (auto stripeMetadata : PList<StripeMetadata>(stripeMetadataList))
	{
		highestRowNumber = std::max(highestRowNumber,
							   StripeGetHighestRowNumber(stripeMetadata));
	}

	return highestRowNumber;
}

/*
 * RewriteMetadataRowWithNewValues rewrites a given metadata entry
 * for a storageId and a stripeId in place with a new offset,
 * rowCount, sizeBytes, and chunkCount.
 *
 * This is used in the vacuum UDF to fill any existing holes
 * if possible.
 */
export StripeMetadata *
RewriteStripeMetadataRowWithNewValues(Relation rel, uint64_t stripeId,
              uint64_t sizeBytes, uint64_t fileOffset, uint64_t rowCount, uint64_t chunkCount)
{
	uint64_t storageId = BioseqColumnStorageGetStorageId(rel, false);

	bool update[Natts_bioseq_stripe] = { false };
	update[Anum_bioseq_stripe_file_offset - 1] = true;
	update[Anum_bioseq_stripe_data_length - 1] = true;
	update[Anum_bioseq_stripe_row_count - 1] = true;
	update[Anum_bioseq_stripe_chunk_count - 1] = true;

	Datum newValues[Natts_bioseq_stripe] = { 0 };
	newValues[Anum_bioseq_stripe_file_offset - 1] = Int64GetDatum(fileOffset);
	newValues[Anum_bioseq_stripe_data_length - 1] = Int64GetDatum(sizeBytes);
	newValues[Anum_bioseq_stripe_row_count - 1] = UInt64GetDatum(rowCount);
	newValues[Anum_bioseq_stripe_chunk_count - 1] = Int32GetDatum(chunkCount);

	return UpdateStripeMetadataRow(storageId, stripeId, update, newValues);
}

// ---------------------------------------------------------------------------
// DOC0260 Phase A — bioseq_internal.seq_sub_table catalog access.
// Catalog access happens via the systable API so MVCC + transactional
// semantics work the same way as for bioseq.stripe.
// ---------------------------------------------------------------------------

export Oid SubTableMetadataRelationId(void) {
	return safe_get_relname_relid("seq_sub_table", BioseqColumnNamespaceId());
}

export Oid SubTableMetadataPKeyIndexRelationId(void) {
	return safe_get_relname_relid("seq_sub_table_pkey", BioseqColumnNamespaceId());
}

export Oid SubTableMetadataActiveShardIndexRelationId(void) {
	return safe_get_relname_relid("seq_sub_table_active_shard_idx", BioseqColumnNamespaceId());
}

static SubTableRow BuildSubTableRowFromTuple(HeapTuple heapTuple, TupleDesc td) {
	Datum values[Natts_seq_sub_table];
	bool  nulls[Natts_seq_sub_table];
	heap_deform_tuple(heapTuple, td, values, nulls);
	SubTableRow row;
	row.database_oid        = DatumGetObjectId(values[Anum_seq_sub_table_database_oid - 1]);
	row.tablespace_oid      = DatumGetObjectId(values[Anum_seq_sub_table_tablespace_oid - 1]);
	row.main_relfilenode    = DatumGetObjectId(values[Anum_seq_sub_table_main_relfilenode - 1]);
	row.column_attno        = DatumGetInt16(values[Anum_seq_sub_table_column_attno - 1]);
	row.bucket_idx          = DatumGetInt16(values[Anum_seq_sub_table_bucket_idx - 1]);
	row.shard_idx           = DatumGetInt32(values[Anum_seq_sub_table_shard_idx - 1]);
	row.sub_table_relid     = DatumGetObjectId(values[Anum_seq_sub_table_sub_table_relid - 1]);
	row.sidecar_relid       = DatumGetObjectId(values[Anum_seq_sub_table_sidecar_relid - 1]);
	row.bytes_written       = static_cast<std::uint64_t>(
		DatumGetInt64(values[Anum_seq_sub_table_bytes_written - 1]));
	row.is_full             = DatumGetBool(values[Anum_seq_sub_table_is_full - 1]);
	row.total_payload_bytes = static_cast<std::uint64_t>(
		DatumGetInt64(values[Anum_seq_sub_table_total_payload_bytes - 1]));
	return row;
}

std::optional<SubTableRow>
LookupActiveSubTableShard(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
                          int16_t column_attno, int16_t bucket_idx)
{
	const Oid relid = SubTableMetadataRelationId();
	if (!OidIsValid(relid)) {
		return std::nullopt;
	}
	Relation rel = safe_relation_open(relid, AccessShareLock);
	TupleDesc td = RelationGetDescr(rel);

	const Oid idx_oid = SubTableMetadataActiveShardIndexRelationId();
	const bool idx_ok = OidIsValid(idx_oid);

	ScanKeyData keys[5];
	ScanKeyInit(&keys[0], Anum_seq_sub_table_database_oid,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(database_oid));
	ScanKeyInit(&keys[1], Anum_seq_sub_table_tablespace_oid,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(tablespace_oid));
	ScanKeyInit(&keys[2], Anum_seq_sub_table_main_relfilenode,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(main_relfilenode));
	ScanKeyInit(&keys[3], Anum_seq_sub_table_column_attno,
	            BTEqualStrategyNumber, F_INT2EQ, Int16GetDatum(column_attno));
	ScanKeyInit(&keys[4], Anum_seq_sub_table_bucket_idx,
	            BTEqualStrategyNumber, F_INT2EQ, Int16GetDatum(bucket_idx));

	SysScanDesc scan = systable_beginscan(rel, idx_oid, idx_ok, nullptr, 5, keys);
	std::optional<SubTableRow> result;
	HeapTuple tup = nullptr;
	while (HeapTupleIsValid(tup = systable_getnext(scan))) {
		SubTableRow row = BuildSubTableRowFromTuple(tup, td);
		if (!row.is_full) {
			result = row;
			break;
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return result;
}

std::optional<SubTableRow>
LookupSubTableShard(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
                    int16_t column_attno, int16_t bucket_idx, int32_t shard_idx)
{
	const Oid relid = SubTableMetadataRelationId();
	if (!OidIsValid(relid)) {
		return std::nullopt;
	}
	Relation rel = safe_relation_open(relid, AccessShareLock);
	TupleDesc td = RelationGetDescr(rel);

	const Oid idx_oid = SubTableMetadataPKeyIndexRelationId();
	const bool idx_ok = OidIsValid(idx_oid);

	ScanKeyData keys[6];
	ScanKeyInit(&keys[0], Anum_seq_sub_table_database_oid,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(database_oid));
	ScanKeyInit(&keys[1], Anum_seq_sub_table_tablespace_oid,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(tablespace_oid));
	ScanKeyInit(&keys[2], Anum_seq_sub_table_main_relfilenode,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(main_relfilenode));
	ScanKeyInit(&keys[3], Anum_seq_sub_table_column_attno,
	            BTEqualStrategyNumber, F_INT2EQ, Int16GetDatum(column_attno));
	ScanKeyInit(&keys[4], Anum_seq_sub_table_bucket_idx,
	            BTEqualStrategyNumber, F_INT2EQ, Int16GetDatum(bucket_idx));
	ScanKeyInit(&keys[5], Anum_seq_sub_table_shard_idx,
	            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(shard_idx));

	SysScanDesc scan = systable_beginscan(rel, idx_oid, idx_ok, nullptr, 6, keys);
	std::optional<SubTableRow> result;
	HeapTuple tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup)) {
		result = BuildSubTableRowFromTuple(tup, td);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return result;
}

SubTableRow
InsertSubTableShard(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
                    int16_t column_attno, int16_t bucket_idx, int32_t shard_idx,
                    Oid sub_table_relid, Oid sidecar_relid)
{
	const Oid relid = SubTableMetadataRelationId();
	if (!OidIsValid(relid)) {
		bioseq_log<ERR, PG>( "bioseq storage: seq_sub_table catalog not present");
	}

	// Race-loss handling — if the unique partial index already accepted a
	// row for the same (db, spc, relfilenode, attno, bucket) with
	// is_full=FALSE, return the winner instead of trying to insert a duplicate.
	{
		auto existing = LookupActiveSubTableShard(
			database_oid, tablespace_oid, main_relfilenode,
			column_attno, bucket_idx);
		if (existing.has_value() && existing->shard_idx == shard_idx) {
			return *existing;
		}
		if (existing.has_value() && existing->shard_idx != shard_idx) {
			return *existing;
		}
	}

	Relation rel = safe_relation_open(relid, RowExclusiveLock);
	TupleDesc td = RelationGetDescr(rel);

	Datum values[Natts_seq_sub_table] = { 0 };
	bool  nulls[Natts_seq_sub_table]  = { false };
	values[Anum_seq_sub_table_database_oid - 1]        = ObjectIdGetDatum(database_oid);
	values[Anum_seq_sub_table_tablespace_oid - 1]      = ObjectIdGetDatum(tablespace_oid);
	values[Anum_seq_sub_table_main_relfilenode - 1]    = ObjectIdGetDatum(main_relfilenode);
	values[Anum_seq_sub_table_column_attno - 1]        = Int16GetDatum(column_attno);
	values[Anum_seq_sub_table_bucket_idx - 1]          = Int16GetDatum(bucket_idx);
	values[Anum_seq_sub_table_shard_idx - 1]           = Int32GetDatum(shard_idx);
	values[Anum_seq_sub_table_sub_table_relid - 1]     = ObjectIdGetDatum(sub_table_relid);
	values[Anum_seq_sub_table_sidecar_relid - 1]       = ObjectIdGetDatum(sidecar_relid);
	values[Anum_seq_sub_table_bytes_written - 1]       = Int64GetDatum(0);
	values[Anum_seq_sub_table_is_full - 1]             = BoolGetDatum(false);
	values[Anum_seq_sub_table_total_payload_bytes - 1] = Int64GetDatum(0);

	HeapTuple tup = heap_form_tuple(td, values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
	CommandCounterIncrement();

	SubTableRow row;
	row.database_oid       = database_oid;
	row.tablespace_oid     = tablespace_oid;
	row.main_relfilenode   = main_relfilenode;
	row.column_attno       = column_attno;
	row.bucket_idx         = bucket_idx;
	row.shard_idx          = shard_idx;
	row.sub_table_relid    = sub_table_relid;
	row.sidecar_relid      = sidecar_relid;
	row.bytes_written      = 0;
	row.is_full            = false;
	row.total_payload_bytes = 0;
	return row;
}

SubTableRow
UpdateSubTableShardBytesWritten(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode,
                                int16_t column_attno, int16_t bucket_idx, int32_t shard_idx,
                                std::uint64_t delta_bytes,
                                std::uint64_t cap_bytes)
{
	const Oid relid = SubTableMetadataRelationId();
	const Oid idx_oid = SubTableMetadataPKeyIndexRelationId();
	if (!OidIsValid(relid)) {
		bioseq_log<ERR, PG>( "bioseq storage: seq_sub_table catalog not present");
	}

	Relation rel = safe_relation_open(relid, RowExclusiveLock);
	TupleDesc td = RelationGetDescr(rel);

	ScanKeyData keys[6];
	ScanKeyInit(&keys[0], Anum_seq_sub_table_database_oid,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(database_oid));
	ScanKeyInit(&keys[1], Anum_seq_sub_table_tablespace_oid,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(tablespace_oid));
	ScanKeyInit(&keys[2], Anum_seq_sub_table_main_relfilenode,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(main_relfilenode));
	ScanKeyInit(&keys[3], Anum_seq_sub_table_column_attno,
	            BTEqualStrategyNumber, F_INT2EQ, Int16GetDatum(column_attno));
	ScanKeyInit(&keys[4], Anum_seq_sub_table_bucket_idx,
	            BTEqualStrategyNumber, F_INT2EQ, Int16GetDatum(bucket_idx));
	ScanKeyInit(&keys[5], Anum_seq_sub_table_shard_idx,
	            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(shard_idx));

	SysScanDesc scan = systable_beginscan(rel, idx_oid, OidIsValid(idx_oid),
	                                      nullptr, 6, keys);
	HeapTuple oldTuple = systable_getnext(scan);
	if (!HeapTupleIsValid(oldTuple)) {
		systable_endscan(scan);
		table_close(rel, RowExclusiveLock);
		bioseq_log<ERR, PG>(
		       "bioseq storage: cannot update missing sub-table shard "
		       "(db={}, spc={}, relfilenode={}, attno={}, bucket={}, shard={})",
		       static_cast<unsigned>(database_oid),
		       static_cast<unsigned>(tablespace_oid),
		       static_cast<unsigned>(main_relfilenode),
		       static_cast<int>(column_attno),
		       static_cast<int>(bucket_idx),
		       static_cast<int>(shard_idx));
	}

	SubTableRow current = BuildSubTableRowFromTuple(oldTuple, td);
	const std::uint64_t new_bytes_written =
		current.bytes_written + delta_bytes;
	const std::uint64_t new_total_payload =
		current.total_payload_bytes + delta_bytes;
	// DOC0260 §10.2 — is_full is driven only by a real byte cap. The cap=0
	// shard-per-insert sentinel is implemented entirely by the insert_payload
	// rollover (which, when the active shard already holds a row, marks it full
	// and allocates the next shard, passing cap_bytes=1 so this branch flips
	// is_full). We deliberately do NOT mark a shard full here merely because
	// cap_bytes==0: the per-commit byte flush from close_all must leave a
	// single-row shard ACTIVE so the next insert's rollover is the one (and
	// only) place that advances the shard index — keeping the full-marking and
	// the new-shard creation in one transaction, exactly like the normal
	// byte-cap rollover.
	const bool now_full = current.is_full
		|| (cap_bytes != 0 && new_bytes_written >= cap_bytes);

	Datum newValues[Natts_seq_sub_table] = { 0 };
	bool  newNulls [Natts_seq_sub_table] = { false };
	bool  doReplace[Natts_seq_sub_table] = { false };
	newValues[Anum_seq_sub_table_bytes_written - 1]       = Int64GetDatum(
		static_cast<std::int64_t>(new_bytes_written));
	doReplace[Anum_seq_sub_table_bytes_written - 1]       = true;
	newValues[Anum_seq_sub_table_total_payload_bytes - 1] = Int64GetDatum(
		static_cast<std::int64_t>(new_total_payload));
	doReplace[Anum_seq_sub_table_total_payload_bytes - 1] = true;
	newValues[Anum_seq_sub_table_is_full - 1]             = BoolGetDatum(now_full);
	doReplace[Anum_seq_sub_table_is_full - 1]             = true;

	HeapTuple newTuple = heap_modify_tuple(oldTuple, td, newValues, newNulls, doReplace);
	CatalogTupleUpdate(rel, &oldTuple->t_self, newTuple);
	systable_endscan(scan);
	heap_freetuple(newTuple);
	table_close(rel, RowExclusiveLock);
	CommandCounterIncrement();

	current.bytes_written       = new_bytes_written;
	current.total_payload_bytes = new_total_payload;
	current.is_full             = now_full;
	return current;
}

std::vector<SubTableRow>
ListSubTableShardsForMain(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode)
{
	std::vector<SubTableRow> out;
	const Oid relid = SubTableMetadataRelationId();
	if (!OidIsValid(relid)) {
		return out;
	}
	Relation rel = safe_relation_open(relid, AccessShareLock);
	TupleDesc td = RelationGetDescr(rel);

	ScanKeyData keys[3];
	ScanKeyInit(&keys[0], Anum_seq_sub_table_database_oid,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(database_oid));
	ScanKeyInit(&keys[1], Anum_seq_sub_table_tablespace_oid,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(tablespace_oid));
	ScanKeyInit(&keys[2], Anum_seq_sub_table_main_relfilenode,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(main_relfilenode));

	SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, nullptr, 3, keys);
	HeapTuple tup = nullptr;
	while (HeapTupleIsValid(tup = systable_getnext(scan))) {
		out.push_back(BuildSubTableRowFromTuple(tup, td));
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return out;
}

void
DeleteSubTableShardsForMain(Oid database_oid, Oid tablespace_oid, Oid main_relfilenode)
{
	const Oid relid = SubTableMetadataRelationId();
	if (!OidIsValid(relid)) {
		return;
	}
	Relation rel = safe_relation_open(relid, RowExclusiveLock);

	ScanKeyData keys[3];
	ScanKeyInit(&keys[0], Anum_seq_sub_table_database_oid,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(database_oid));
	ScanKeyInit(&keys[1], Anum_seq_sub_table_tablespace_oid,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(tablespace_oid));
	ScanKeyInit(&keys[2], Anum_seq_sub_table_main_relfilenode,
	            BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(main_relfilenode));

	SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, nullptr, 3, keys);
	HeapTuple tup = nullptr;
	while (HeapTupleIsValid(tup = systable_getnext(scan))) {
		CatalogTupleDelete(rel, &tup->t_self);
	}
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);
	CommandCounterIncrement();
}
