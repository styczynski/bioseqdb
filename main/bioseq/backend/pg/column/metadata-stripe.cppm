/*-------------------------------------------------------------------------
 *
 * column/metadata-stripe.cppm
 *
 * Narrow storage_metadata partition for storage-id, stripe, chunk-group,
 * and skip-list metadata access.
 *
 *-------------------------------------------------------------------------
 */
export module storage_metadata:stripe;

import :core;

export using ::LookupStorageId;
export using ::DeleteMetadataRows;
export using ::DeleteMetadataRowsForStripeId;
export using ::BioseqColumnMetadataNewStorageId;
export using ::GetHighestUsedAddress;
export using ::ReserveEmptyStripe;
export using ::CompleteStripeReservation;
export using ::SaveStripeSkipList;
export using ::SaveChunkGroups;
export using ::UpdateChunkGroupDeletedRows;
export using ::ReadStripeSkipList;
export using ::FindNextStripeByRowNumber;
export using ::FindStripeByRowNumber;
export using ::FindStripeWithMatchingFirstRowNumber;
export using ::FindNextStripeForParallelWorker;
export using ::StripeWriteState;
export using ::StripeGetHighestRowNumber;
export using ::FindStripeWithHighestRowNumber;
export using ::BioseqColumnStripeRelationId;
export using ::BioseqColumnStripePKeyIndexRelationId;
export using ::BuildStripeMetadata;
export using ::StripesForRelfilenode;
export using ::BioseqColumnTableRowCount;
export using ::DeletedRowsForStripe;
export using ::DecompressedLengthForStripe;
export using ::BioseqColumnStorageUpdateIfNeeded;
export using ::RewriteStripeMetadataRowWithNewValues;
