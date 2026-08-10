/*-------------------------------------------------------------------------
 *
 * column/metadata-row-mask.cppm
 *
 * Narrow storage_metadata partition for row-mask metadata access.
 *
 *-------------------------------------------------------------------------
 */
export module bioseq.postgres.storage.metadata:row_mask;

import :core;

export using ::column_row_mask_relation_oid;
export using ::column_row_mask_index_oid;
export using ::save_empty_row_mask;
export using ::flush_row_mask_cache;
export using ::read_chunk_row_mask;
