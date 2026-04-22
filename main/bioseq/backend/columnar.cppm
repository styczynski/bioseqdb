/*-------------------------------------------------------------------------
 *
 * columnar.cppm
 *
 * Module entry point for the bioseq columnar extension. Registers the GUC
 * variables that steer compression, stripe layout, parallel execution and
 * column caching, then hands control off to the table-access-method and
 * planner-hook initialisers.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"

export module bioseq_columnar;

import mlogging;
import bioseq_table;
import config;
import pg_utils;

extern "C++" {

/* Default values for option parameters */
constexpr int DEFAULT_STRIPE_ROW_COUNT = 150000;
constexpr int DEFAULT_CHUNK_ROW_COUNT = 10000;

#if HAVE_LIBZSTD
constexpr int DEFAULT_COMPRESSION_TYPE = COMPRESSION_ZSTD;
#elif HAVE_CITUS_LIBLZ4
constexpr int DEFAULT_COMPRESSION_TYPE = COMPRESSION_LZ4;
#else
constexpr int DEFAULT_COMPRESSION_TYPE = COMPRESSION_PG_LZ;
#endif

static void bioseq_guc_init();

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
bool bioseq_update_index_on_insert = true;

/*
 * bioseq_init is the module's top-level initialiser. It wires up the GUC
 * variables, installs the bioseq table access method handler and registers
 * the planner hooks that route queries through the columnar custom scan.
 */
void
bioseq_init()
{
	JustLog("[bioseq] pg: bioseq_init() entered");
	bioseq_guc_init();
	bioseq_tableam_init();
	bioseq_planner_init();
}


/*
 * bioseq_guc_init registers all bioseq.* GUC variables with the PostgreSQL
 * configuration machinery. Keeps variable defaults and bounds in one place
 * so tuning is auditable.
 */
static void
bioseq_guc_init()
{
	JustLog("[bioseq] pg: bioseq_guc_init() entered");
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

	DefineCustomBoolVariable(
		"bioseq.update_index_on_insert",
		"When true, compute and persist a minimizer TargetIndex for every "
		"bioseq.seq value written into a USING bioseq table.",
		NULL,
		&bioseq_update_index_on_insert,
		true,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);
}

}
