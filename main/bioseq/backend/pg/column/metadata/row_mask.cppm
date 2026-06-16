/*-------------------------------------------------------------------------
 *
 * column/metadata-row-mask.cppm
 *
 * Narrow storage_metadata partition for row-mask metadata access.
 *
 *-------------------------------------------------------------------------
 */
export module bioseq.storage.metadata:row_mask;

import :core;

export using ::BioseqColumnRowMaskRelationId;
export using ::BioseqColumnRowMaskIndexRelationId;
export using ::SaveEmptyRowMask;
export using ::FlushRowMaskCache;
export using ::ReadChunkRowMask;
