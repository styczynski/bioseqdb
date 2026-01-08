#pragma once

// Temporary config


#define COLUMNAR_MODULE_NAME "citus_columnar"

#define COLUMNAR_SETOPTIONS_HOOK_SYM "ColumnarTableSetOptions_hook"

/* Defines for valid option names */
#define OPTION_NAME_COMPRESSION_TYPE "compression"
#define OPTION_NAME_STRIPE_ROW_COUNT "stripe_row_limit"
#define OPTION_NAME_CHUNK_ROW_COUNT "chunk_group_row_limit"

/* Limits for option parameters */
#define STRIPE_ROW_COUNT_MINIMUM 1000
#define STRIPE_ROW_COUNT_MAXIMUM 100000000
#define CHUNK_ROW_COUNT_MINIMUM 1000
#define CHUNK_ROW_COUNT_MAXIMUM 100000000
#define COMPRESSION_LEVEL_MIN 1
#define COMPRESSION_LEVEL_MAX 19

/* Columnar file signature */
#define COLUMNAR_VERSION_MAJOR 2
#define COLUMNAR_VERSION_MINOR 0

/* miscellaneous defines */
#define COLUMNAR_TUPLE_COST_MULTIPLIER 10
#define COLUMNAR_POSTSCRIPT_SIZE_LENGTH 1
#define COLUMNAR_POSTSCRIPT_SIZE_MAX 256
#define COLUMNAR_BYTES_PER_PAGE (BLCKSZ - SizeOfPageHeaderData)

/* Columnar row mask byte array size */
#define COLUMNAR_ROW_MASK_CHUNK_SIZE 10000

/* Flag to indicate is vectorized aggregate used in execution */
#define CUSTOM_SCAN_VECTORIZED_AGGREGATE 1

