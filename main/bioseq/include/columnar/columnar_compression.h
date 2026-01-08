/*-------------------------------------------------------------------------
 *
 * bioseq_compression.h
 *
 * Type and function declarations for compression methods.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef COLUMNAR_COMPRESSION_H
#define COLUMNAR_COMPRESSION_H

#include "pg_version_compat.h"
#include "types.h"

extern bool CompressBuffer(StringInfo inputBuffer,
						   StringInfo outputBuffer,
						   CompressionType compressionType,
						   int compressionLevel);
extern StringInfo DecompressBuffer(StringInfo buffer, CompressionType compressionType,
								   uint64 decompressedSize);

#endif /* COLUMNAR_COMPRESSION_H */
