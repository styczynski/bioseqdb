/*-------------------------------------------------------------------------
 *
 * columnar.c
 *
 * This file contains...
 *
 * Copyright (c) 2016, Citus Data, Inc.
 * Copyright (c) Hydra, Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "common.h"

#include <iostream>

#include <sys/stat.h>
#include <unistd.h>

#include "safe_lib.h"

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"

/* Default values for option parameters */
#define DEFAULT_STRIPE_ROW_COUNT 150000
#define DEFAULT_CHUNK_ROW_COUNT 10000

#if HAVE_LIBZSTD
#define DEFAULT_COMPRESSION_TYPE COMPRESSION_ZSTD
#elif HAVE_CITUS_LIBLZ4
#define DEFAULT_COMPRESSION_TYPE COMPRESSION_LZ4
#else
#define DEFAULT_COMPRESSION_TYPE COMPRESSION_PG_LZ
#endif

import mlogging;
import bioseq_table;
import config;

static void bioseq_guc_init(void);

int bioseq_compression = DEFAULT_COMPRESSION_TYPE;
int bioseq_stripe_row_limit = DEFAULT_STRIPE_ROW_COUNT;
int bioseq_chunk_group_row_limit = DEFAULT_CHUNK_ROW_COUNT;
int bioseq_compression_level = 3;
bool bioseq_enable_parallel_execution = true;
int bioseq_min_parallel_processes = 8;
bool bioseq_enable_vectorization = true;
bool bioseq_enable_dml = true;
bool bioseq_enable_page_cache = false;
int bioseq_page_cache_size = 200U;
bool bioseq_index_scan = true;

void
bioseq_init(void)
{
	BMethod(bioseq_init);
	bioseq_guc_init();
	bioseq_tableam_init();
	bioseq_planner_init();
}


static void
bioseq_guc_init()
{
	// Enable logging
	DefineCustomBoolVariable(
		"bioseq.logging",
		"Enable dedicated extension logging",
		NULL,
		&enable_logging,
		false,              // Disabled by default.
		PGC_SUSET,          // Only allow superusers to modify
		0,                  // No flags for this GUC
		NULL, NULL, NULL    // No variable hooks necessary
	);
	DefineCustomEnumVariable("bioseq.compression",
							 "Compression type for bioseq.",
							 NULL,
							 &bioseq_compression,
							 DEFAULT_COMPRESSION_TYPE,
							 bioseq_compression_options,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("bioseq.compression_level",
							"Compression level to be used with zstd.",
							NULL,
							&bioseq_compression_level,
							3,
							COMPRESSION_LEVEL_MIN,
							COMPRESSION_LEVEL_MAX,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("bioseq.stripe_row_limit",
							"Maximum number of tuples per stripe.",
							NULL,
							&bioseq_stripe_row_limit,
							DEFAULT_STRIPE_ROW_COUNT,
							STRIPE_ROW_COUNT_MINIMUM,
							STRIPE_ROW_COUNT_MAXIMUM,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("bioseq.chunk_group_row_limit",
							"Maximum number of rows per chunk.",
							NULL,
							&bioseq_chunk_group_row_limit,
							DEFAULT_CHUNK_ROW_COUNT,
							CHUNK_ROW_COUNT_MINIMUM,
							CHUNK_ROW_COUNT_MAXIMUM,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("bioseq.enable_parallel_execution",
							 "Enables parallel execution",
							 NULL,
							 &bioseq_enable_parallel_execution,
							 true,
							 PGC_USERSET,
							 GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							 NULL, 
							 NULL, 
							 NULL);

	DefineCustomIntVariable("bioseq.min_parallel_processes",
							"Minimum number of parallel processes",
							NULL,
							&bioseq_min_parallel_processes,
							8,
							1,
							32,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("bioseq.enable_vectorization",
							 "Enables vectorized execution",
							 NULL,
							 &bioseq_enable_vectorization,
							 true,
							 PGC_USERSET,
							 GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							 NULL, 
							 NULL, 
							 NULL);

	DefineCustomBoolVariable("bioseq.enable_dml",
							"Enables DML",
							NULL,
							&bioseq_enable_dml,
							true,
							PGC_USERSET,
							GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							NULL, 
							NULL, 
							NULL);

	DefineCustomBoolVariable("bioseq.enable_column_cache",
							"Enables column based caching",
							NULL,
							&bioseq_enable_page_cache,
							false,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("bioseq.column_cache_size",
							"Size of the column based cache in megabytes",
							NULL,
							&bioseq_page_cache_size,
							200U,
							20U,
							20000U,
							PGC_USERSET,
							GUC_UNIT_MB,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("bioseq.enable_bioseq_index_scan",
							 "Enables custom columnar index scan",
							 NULL,
							 &bioseq_index_scan,
							 true,
							 PGC_USERSET,
							 GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							 NULL, 
							 NULL, 
							 NULL);
}

