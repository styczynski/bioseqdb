/*-------------------------------------------------------------------------
 *
 * column/metadata/seq_sub_table.cppm
 *
 * Narrow storage_metadata partition for sequence sub-table catalog rows.
 * The row owns tuple decoding and the manager owns catalog operations; this
 * partition exposes only those two semantic interfaces.
 *
 *-------------------------------------------------------------------------
 */
export module bioseq.postgres.storage.metadata:seq_sub_table;

import :core;

export using ::sequence_sub_table_row;
export using ::sequence_sub_table_manager;
