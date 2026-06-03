/*-------------------------------------------------------------------------
 *
 * columnar.h
 *
 * Type and function declarations for Columnar
 *
 * Copyright (c) Citus Data, Inc.
 * Copyright (c) Hydra, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef COLUMNAR_H
#define COLUMNAR_H

#include "common.h"

#include "columnar/columnar_write_state_row_mask.h"


#include "config/config.h"
#include "types.h"

/* GUCs */
extern int bioseq_compression;
extern int bioseq_stripe_row_limit;
extern int bioseq_chunk_group_row_limit;
extern int bioseq_compression_level;
extern bool bioseq_enable_parallel_execution;
extern int bioseq_min_parallel_processes;
extern bool bioseq_enable_vectorization;
extern bool bioseq_enable_dml;
extern bool bioseq_enable_page_cache;
extern int bioseq_page_cache_size;
extern bool bioseq_index_scan;
extern bool bioseq_update_index_on_insert;
extern bool bioseq_read_sequence_index;
extern int  bioseq_index_run_buffer_bytes;  /* DOC0279 §13 — test-only run buffer override */
/*
 * DOC0260 Phase A — controls the per-shard payload cap for the
 * length-bucketed sub-table sharding. Value is in GiB; 0 is a documented
 * test-only sentinel that forces a new shard per insert.
 */
extern int bioseq_storage_max_seq_sub_table_gbs;
/* DOC0264 §9.2 — test parity GUC. When on, the dispatch buffer materialises
 * stub-backed sequences via fetch_payload and dispatches through
 * `with_lazy_in_memory` instead of the production `with_lazy_postgres`
 * scanner_pg path. Default off. */
extern bool bioseq_test_force_inmemory_scanner;

/* Aligner parameter GUCs — see sequences/minimap_port.cppm Minimap2Params
 * for field-by-field documentation. Kept in plain extern form so any module
 * that #includes columnar.h can read them without additional plumbing. */
extern int  bioseq_aligner_kmer_size;
extern int  bioseq_aligner_window_size;
extern int  bioseq_aligner_max_gap;
extern int  bioseq_aligner_max_gap_ref;
extern int  bioseq_aligner_bandwidth;
extern int  bioseq_aligner_bandwidth_long;
extern int  bioseq_aligner_max_chain_skip;
extern int  bioseq_aligner_max_chain_iter;
extern int  bioseq_aligner_min_seed_count;
extern int  bioseq_aligner_min_chain_score;
extern double bioseq_aligner_chain_gap_scale;
extern double bioseq_aligner_chain_skip_scale;
extern double bioseq_aligner_mask_level;
extern int  bioseq_aligner_mask_len;
extern double bioseq_aligner_pri_ratio;
extern int  bioseq_aligner_best_n;
extern int  bioseq_aligner_match_score;
extern int  bioseq_aligner_mismatch_pen;
extern int  bioseq_aligner_gap_open1;
extern int  bioseq_aligner_gap_ext1;
extern int  bioseq_aligner_gap_open2;
extern int  bioseq_aligner_gap_ext2;
extern int  bioseq_aligner_sc_ambi;
extern int  bioseq_aligner_zdrop;
extern int  bioseq_aligner_end_bonus;
extern int  bioseq_aligner_max_occ;
extern int  bioseq_aligner_mid_occ;
extern int  bioseq_aligner_block_min_size;
extern int  bioseq_aligner_block_max_size;
extern int  bioseq_aligner_chaining_algorithm;
extern int  bioseq_aligner_base_alignment_algorithm;
extern bool bioseq_aligner_enable_profiling;


/* called when the user changes options on the given relation */
typedef void (*ColumnarTableSetOptions_hook_type)(Oid relid, ColumnarOptions options);

extern void bioseq_init(void);

/* Function declarations for writing to a columnar table */
// extern ColumnarWriteState * ColumnarBeginWrite(RelFileLocator relfilelocator,
// 											   ColumnarOptions options,
// 											   TupleDesc tupleDescriptor);
// extern uint64 ColumnarWriteRow(ColumnarWriteState *state, Datum *columnValues,
// 							   bool *columnNulls);
// extern void ColumnarFlushPendingWrites(ColumnarWriteState *state);
// extern void ColumnarEndWrite(ColumnarWriteState *state);
// extern bool ContainsPendingWrites(ColumnarWriteState *state);
// extern MemoryContext ColumnarWritePerTupleContext(ColumnarWriteState *state);

/* Function declarations for reading from columnar table */

// /* functions applicable for both sequential and random access */
// extern ColumnarReadState * ColumnarBeginRead(Relation relation,
// 											 TupleDesc tupleDescriptor,
// 											 List *projectedColumnList,
// 											 List *qualConditions,
// 											 MemoryContext scanContext,
// 											 Snapshot snaphot,
// 											 bool randomAccess,
// 											 ParallelColumnarScan parallaleColumnarScan);
// extern void ColumnarReadFlushPendingWrites(ColumnarReadState *readState);
// extern void ColumnarEndRead(ColumnarReadState *state);
// extern void ColumnarResetRead(ColumnarReadState *readState);

// /* functions only applicable for sequential access */
// extern bool ColumnarReadNextRow(ColumnarReadState *state, Datum *columnValues,
// 								bool *columnNulls, uint64 *rowNumber);
// extern bool ColumnarReadNextVector(ColumnarReadState *readState, Datum *columnValues,
// 								   bool *columnNulls, uint64 *rowNumber,
// 								   int *newVectorSize);
// extern int64 ColumnarReadChunkGroupsFiltered(ColumnarReadState *state);
// extern void ColumnarRescan(ColumnarReadState *readState, List *scanQual);

// /* functions only applicable for random access */
// extern void ColumnarReadRowByRowNumberOrError(ColumnarReadState *readState,
// 											  uint64 rowNumber, Datum *columnValues,
// 											  bool *columnNulls);
// extern bool ColumnarReadRowByRowNumber(ColumnarReadState *readState,
// 									   uint64 rowNumber, Datum *columnValues,
// 									   bool *columnNulls);
// extern bool ColumnarSetStripeReadState(ColumnarReadState *readState,
// 									   StripeMetadata *startStripeMetadata);

/* Function declarations for common functions */
// extern FmgrInfo * GetFunctionInfoOrNull(Oid typeId, Oid accessMethodId,
// 										int16 procedureId);
// extern ChunkData * CreateEmptyChunkData(uint32 columnCount, bool *columnMask,
// 										uint32 chunkGroupRowCount);
// extern void FreeChunkData(ChunkData *chunkData);
// extern void FreeChunkBufferValueArray(ChunkData *chunkData);
// extern uint64 ColumnarTableRowCount(Relation relation);
// extern const char * CompressionTypeStr(CompressionType type);
// extern ItemPointerData row_number_to_tid(uint64 rowNumber);
// extern uint64 LookupStorageId(RelFileLocator relfilelocator);

/* bioseq_metadata_tables.c */
// extern void InitColumnarOptions(Oid regclass);
// extern void SetColumnarOptions(Oid regclass, ColumnarOptions *options);
// extern bool DeleteColumnarTableOptions(Oid regclass, bool missingOk);
// extern bool ReadColumnarOptions(Oid regclass, ColumnarOptions *options);
// extern bool IsColumnarTableAmTable(Oid relationId);

// /* bioseq_metadata_tables.c */
// extern void DeleteMetadataRows(RelFileLocator relfilelocator);
// extern void DeleteMetadataRowsForStripeId(RelFileLocator relfilelocator, uint64 stripeId);
// extern uint64 ColumnarMetadataNewStorageId(void);
// extern uint64 GetHighestUsedAddress(RelFileLocator relfilelocator);
// extern EmptyStripeReservation * ReserveEmptyStripe(Relation rel, uint64 columnCount,
// 												   uint64 chunkGroupRowCount,
// 												   uint64 stripeRowCount);
// extern StripeMetadata * CompleteStripeReservation(Relation rel, uint64 stripeId,
// 												  uint64 sizeBytes, uint64 rowCount,
// 												  uint64 chunkCount);
// extern void SaveStripeSkipList(RelFileLocator relfilelocator, uint64 stripe,
// 							   StripeSkipList *stripeSkipList,
// 							   TupleDesc tupleDescriptor);
// extern void SaveChunkGroups(RelFileLocator relfilelocator, uint64 stripe,
// 							List *chunkGroupRowCounts);
// extern void UpdateChunkGroupDeletedRows(uint64 storageId, uint64 stripe,
// 										uint32 chunkGroupId, uint32 deletedRowNumber);
// extern StripeSkipList * ReadStripeSkipList(RelFileLocator relfilelocator, uint64 stripe,
// 										   TupleDesc tupleDescriptor,
// 										   uint32 chunkCount,
// 										   Snapshot snapshot);
// extern StripeMetadata * FindNextStripeByRowNumber(Relation relation, uint64 rowNumber,
// 												  Snapshot snapshot);
// extern StripeMetadata * FindStripeByRowNumber(Relation relation, uint64 rowNumber,
// 											  Snapshot snapshot);
// extern StripeMetadata * FindStripeWithMatchingFirstRowNumber(Relation relation,
// 															 uint64 rowNumber,
// 															 Snapshot snapshot);
// extern StripeMetadata *  FindNextStripeForParallelWorker(Relation relation,
// 														 Snapshot snapshot,
// 														 uint64 nextStripeId,
// 														 uint64 * nextHigherStripeId);
// extern StripeWriteStateEnum StripeWriteState(StripeMetadata *stripeMetadata);
// extern uint64 StripeGetHighestRowNumber(StripeMetadata *stripeMetadata);
// extern StripeMetadata * FindStripeWithHighestRowNumber(Relation relation,
// 													   Snapshot snapshot);
// extern "C" Datum bioseq_relation_storageid(PG_FUNCTION_ARGS);
// extern bool SaveEmptyRowMask(uint64 storageId, uint64 stripeId,
// 							 uint64 stripeStartRowNumber, List *chunkGroupRowCounts);
// extern bool UpdateRowMask(RelFileLocator relfilelocator, uint64 storageId,
// 						  Snapshot snapshot, uint64 rowNumber);
// extern void FlushRowMaskCache(RowMaskWriteStateEntry *rowMaskEntry);
// extern bytea * ReadChunkRowMask(RelFileLocator relfilelocator, Snapshot snapshot,
// 								MemoryContext ctx,
// 								uint64 stripeFirstRowNumber, int rowCount);
// extern "C" Datum create_table_row_mask(PG_FUNCTION_ARGS);
// extern EState * create_estate_for_relation(Relation rel);

/* bioseq_planner_hook.c */
extern void bioseq_planner_init(void);

/* write_state_interface.c */
extern void FlushWriteStateWithNewSnapshot(Oid relfilelocator,
										   Snapshot * snapshot,
										   bool * snapShotRegisteredByUs);
extern void FlushWriteStateForAllRels(SubTransactionId currentSubXid,
									  SubTransactionId parentSubXid);
extern void DiscardWriteStateForAllRels(SubTransactionId currentSubXid,
										SubTransactionId parentSubXid);
extern void MarkRelfilenodeDropped(Oid relfilelocator, SubTransactionId currentSubXid);
extern void NonTransactionDropWriteState(Oid relfilelocator);
extern bool PendingWritesInUpperTransactions(Oid relfilelocator,
											 SubTransactionId currentSubXid);

/* write_state_management.c */
extern ColumnarWriteState * bioseq_init_write_state(Relation relation, TupleDesc
													  tupdesc,
														Oid tupSlotRelationId,
													  SubTransactionId currentSubXid);
extern void ColumnarPopWriteStateForAllRels(SubTransactionId currentSubXid,
											SubTransactionId parentSubXid,
											bool commit);
extern void ColumnarMarkRelfilenodeDroppedColumnar(Oid relfilelocator,
												   SubTransactionId currentSubXid);
extern void ColumnarNonTransactionDropWriteState(Oid relfilelocator);
extern bool ColumnarPendingWritesInUpperTransactions(Oid relfilelocator,
													 SubTransactionId currentSubXid);
extern void FlushWriteStateForRelfilenode(Oid relfilelocator, SubTransactionId
										  currentSubXid);
extern MemoryContext GetColumnarWriteContextForDebug(void);

/* write_state_row_mask.c */
extern RowMaskWriteStateEntry * RowMaskInitWriteState(Oid relfilelocator,
													  uint64 storageId,
													  SubTransactionId currentSubXid,
							 						  bytea *rowMask);
extern void RowMaskFlushWriteStateForRelfilenode(Oid relfilelocator, 
												 SubTransactionId currentSubXid);
extern RowMaskWriteStateEntry * RowMaskFindWriteState(Oid relfilelocator,
					  								  SubTransactionId currentSubXid,
													  uint64 rowId);
extern void  RowMaskPopWriteStateForAllRels(SubTransactionId currentSubXid,
											SubTransactionId parentSubXid,
											bool commit);
extern void RowMaskMarkRelfilenodeDropped(Oid relfilelocator,
										  SubTransactionId currentSubXid);
extern void RowMaskNonTransactionDrop(Oid relfilelocator);
extern bool RowMaskPendingWritesInUpperTransactions(Oid relfilelocator,
													SubTransactionId currentSubXid);
extern MemoryContext GetRowMaskWriteStateContextForDebug(void);

/* columanar_read_state_cache.c */
extern ColumnarReadState ** InitColumnarReadStateCache(Relation relation,
													  SubTransactionId currentSubXid);
extern ColumnarReadState ** FindReadStateCache(Relation relation,
											   SubTransactionId currentSubXid);
extern void CleanupReadStateCache(SubTransactionId currentSubXid);
extern MemoryContext GetColumnarReadStateCache(void);

/* bioseq_cache.c */
// extern void ColumnarMarkChunkGroupInUse(uint64 relId, uint64 stripeId, uint32 chunkId);
// extern void ColumnarAddCacheEntry(uint64, uint64, uint64, uint32, void *);
// extern void *ColumnarRetrieveCache(uint64, uint64, uint64, uint32);
// extern void ColumnarResetCache(void);
// extern ColumnarCacheStatistics *ColumnarGetCacheStatistics(void);
// extern MemoryContext ColumnarCacheMemoryContext(void);



#endif /* COLUMNAR_H */
