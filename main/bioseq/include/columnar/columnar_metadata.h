/*-------------------------------------------------------------------------
 *
 * bioseq_metadata.h
 *
 * Type and function declarations for Columnar metadata.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef COLUMNAR_METADATA_H
#define COLUMNAR_METADATA_H

#include "pg_version_compat.h"
#include "types.h"

extern List * StripesForRelfilenode(RelFileLocator relfilelocator, ScanDirection scanDirection);
extern uint32 DeletedRowsForStripe(RelFileLocator relfilelocator,
								   uint32 chunkCount,
								   uint64 stripeId);
extern Size DecompressedLengthForStripe(RelFileLocator relfilelocator, uint64 stripeId);
extern void ColumnarStorageUpdateIfNeeded(Relation rel, bool isUpgrade);
extern StripeMetadata * RewriteStripeMetadataRowWithNewValues(Relation rel, uint64 stripeId,
              uint64 sizeBytes, uint64 fileOffset, uint64 rowCount, uint64 chunkCount);

#endif /* COLUMNAR_METADATA_H */
