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

/* constants for bioseq.options */
#define Natts_bioseq_options 5
#define Anum_bioseq_options_regclass 1
#define Anum_bioseq_options_chunk_group_row_limit 2
#define Anum_bioseq_options_stripe_row_limit 3
#define Anum_bioseq_options_compression_level 4
#define Anum_bioseq_options_compression 5

/* constants for bioseq.stripe */
#define Natts_bioseq_stripe 9
#define Anum_bioseq_stripe_storageid 1
#define Anum_bioseq_stripe_stripe 2
#define Anum_bioseq_stripe_file_offset 3
#define Anum_bioseq_stripe_data_length 4
#define Anum_bioseq_stripe_column_count 5
#define Anum_bioseq_stripe_chunk_row_count 6
#define Anum_bioseq_stripe_row_count 7
#define Anum_bioseq_stripe_chunk_count 8
#define Anum_bioseq_stripe_first_row_number 9

/* constants for bioseq.chunk_group */
#define Natts_bioseq_chunkgroup 5
#define Anum_bioseq_chunkgroup_storageid 1
#define Anum_bioseq_chunkgroup_stripe 2
#define Anum_bioseq_chunkgroup_chunk 3
#define Anum_bioseq_chunkgroup_row_count 4
#define Anum_bioseq_chunkgroup_deleted_rows 5

/* constants for bioseq.chunk */
#define Natts_bioseq_chunk 14
#define Anum_bioseq_chunk_storageid 1
#define Anum_bioseq_chunk_stripe 2
#define Anum_bioseq_chunk_attr 3
#define Anum_bioseq_chunk_chunk 4
#define Anum_bioseq_chunk_minimum_value 5
#define Anum_bioseq_chunk_maximum_value 6
#define Anum_bioseq_chunk_value_stream_offset 7
#define Anum_bioseq_chunk_value_stream_length 8
#define Anum_bioseq_chunk_exists_stream_offset 9
#define Anum_bioseq_chunk_exists_stream_length 10
#define Anum_bioseq_chunk_value_compression_type 11
#define Anum_bioseq_chunk_value_compression_level 12
#define Anum_bioseq_chunk_value_decompressed_size 13
#define Anum_bioseq_chunk_value_count 14

/* constants for bioseq.row_mask */
#define Natts_bioseq_row_mask 8
#define Anum_bioseq_row_mask_id 1
#define Anum_bioseq_row_mask_storage_id 2
#define Anum_bioseq_row_mask_stripe_id 3
#define Anum_bioseq_row_mask_chunk_id 4
#define Anum_bioseq_row_mask_start_row_number 5
#define Anum_bioseq_row_mask_end_row_number 6
#define Anum_bioseq_row_mask_deleted_rows 7
#define Anum_bioseq_row_mask_mask 8
