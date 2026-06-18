/*-------------------------------------------------------------------------
 *
 * column/metadata/seq_sub_table.cppm
 *
 * Narrow storage_metadata partition for sequence sub-table catalog rows.
 * DOC0329: the field-number constants + free functions are now folded into
 * SubTableRow (tuple ctor) and SubTableManager (snake_case static methods);
 * this facade re-exports just those two types.
 *
 *-------------------------------------------------------------------------
 */
export module bioseq.storage.metadata:seq_sub_table;

import :core;

export using ::SubTableRow;
export using ::SubTableManager;
