/*-------------------------------------------------------------------------
 *
 * column/metadata-seq-sub-table.cppm
 *
 * Narrow storage_metadata partition for sequence sub-table catalog rows.
 *
 *-------------------------------------------------------------------------
 */
export module bioseq.storage.metadata:seq_sub_table;

import :core;

export using ::Natts_seq_sub_table;
export using ::Anum_seq_sub_table_database_oid;
export using ::Anum_seq_sub_table_tablespace_oid;
export using ::Anum_seq_sub_table_main_relfilenode;
export using ::Anum_seq_sub_table_column_attno;
export using ::Anum_seq_sub_table_bucket_idx;
export using ::Anum_seq_sub_table_shard_idx;
export using ::Anum_seq_sub_table_sub_table_relid;
export using ::Anum_seq_sub_table_sidecar_relid;
export using ::Anum_seq_sub_table_bytes_written;
export using ::Anum_seq_sub_table_is_full;
export using ::Anum_seq_sub_table_total_payload_bytes;
export using ::SubTableRow;
export using ::SubTableMetadataRelationId;
export using ::SubTableMetadataPKeyIndexRelationId;
export using ::SubTableMetadataActiveShardIndexRelationId;
export using ::LookupActiveSubTableShard;
export using ::LookupSubTableShard;
export using ::InsertSubTableShard;
export using ::UpdateSubTableShardBytesWritten;
export using ::ListSubTableShardsForMain;
export using ::DeleteSubTableShardsForMain;
