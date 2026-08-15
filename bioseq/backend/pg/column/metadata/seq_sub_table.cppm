/**
 * @file seq_sub_table.cppm
 * @brief Sequence sub-table catalog rows and manager operations.
 *
 * The row owns tuple decoding and the manager owns catalog operations; this
 * partition exposes only those two semantic interfaces.
 */
export module bioseq.postgres.storage.metadata:seq_sub_table;

import :core;

export using ::sequence_sub_table_row;
export using ::sequence_sub_table_manager;
