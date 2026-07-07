module;
#include "common.h"

#include <string_view>

export module bioseq.config;

import bioseq.pg_utils;

/*
 * DOC0322 §6.5/§9.2 — config now OWNS the compression-method enum (moved here
 * from the deleted backend/types.h). It stays UNSCOPED with the original
 * enumerator names and values: they are reinterpret_cast'd / stored on disk and
 * used pervasively across the column-store modules. config is the right home
 * because the option table + Parse/StrCompressionType below operate on it and
 * config is a leaf imported by every module that touches compression.
 */
export enum CompressionType : int
{
	COMPRESSION_TYPE_INVALID = -1,
	COMPRESSION_NONE = 0,
	COMPRESSION_PG_LZ = 1,
	COMPRESSION_LZ4 = 2,
	COMPRESSION_ZSTD = 3,

	COMPRESSION_COUNT
};

/*
 * DOC0322 §6.5 — catalog (Anum_ and Natts_) and tuning constants migrated from
 * the deleted backend/config/config.h as exported constexpr. Names/values are
 * preserved exactly: the Anum_ and Natts_ identifiers name the bioseq.* catalog
 * tables and are copied verbatim for the scripted rename; the BIOSEQ_COLUMN_*
 * tuning constants keep their spelling (the columnar->bioseq_column rename is a
 * later step). Object-like string macros become std::string_view (or const
 * char* where passed straight to PG C); object-like numeric macros become
 * constexpr int/Size.
 */
export constexpr const char* BIOSEQ_COLUMN_MODULE_NAME = "bioseq_column";

export constexpr const char* BIOSEQ_COLUMN_SETOPTIONS_HOOK_SYM = "BioseqColumnTableSetOptions_hook";

/* Defines for valid option names */
export constexpr const char* OPTION_NAME_COMPRESSION_TYPE = "compression";
export constexpr const char* OPTION_NAME_STRIPE_ROW_COUNT = "stripe_row_limit";
export constexpr const char* OPTION_NAME_CHUNK_ROW_COUNT = "chunk_group_row_limit";

/* Limits for option parameters */
export constexpr int STRIPE_ROW_COUNT_MINIMUM = 1000;
export constexpr int STRIPE_ROW_COUNT_MAXIMUM = 100000000;
export constexpr int CHUNK_ROW_COUNT_MINIMUM = 1000;
export constexpr int CHUNK_ROW_COUNT_MAXIMUM = 100000000;
export constexpr int COMPRESSION_LEVEL_MIN = 1;
export constexpr int COMPRESSION_LEVEL_MAX = 19;

/* BioseqColumn file signature */
export constexpr int BIOSEQ_COLUMN_VERSION_MAJOR = 2;
export constexpr int BIOSEQ_COLUMN_VERSION_MINOR = 0;

/* miscellaneous defines */
export constexpr int BIOSEQ_COLUMN_TUPLE_COST_MULTIPLIER = 10;
export constexpr int BIOSEQ_COLUMN_POSTSCRIPT_SIZE_LENGTH = 1;
export constexpr int BIOSEQ_COLUMN_POSTSCRIPT_SIZE_MAX = 256;
export constexpr Size BIOSEQ_COLUMN_BYTES_PER_PAGE = (BLCKSZ - SizeOfPageHeaderData);

/* BioseqColumn row mask byte array size */
export constexpr int BIOSEQ_COLUMN_ROW_MASK_CHUNK_SIZE = 10000;

/* Flag to indicate is vectorized aggregate used in execution */
export constexpr int CUSTOM_SCAN_VECTORIZED_AGGREGATE = 1;

/* constants for bioseq.options */
export constexpr int Natts_bioseq_options = 5;
export constexpr int Anum_bioseq_options_regclass = 1;
export constexpr int Anum_bioseq_options_chunk_group_row_limit = 2;
export constexpr int Anum_bioseq_options_stripe_row_limit = 3;
export constexpr int Anum_bioseq_options_compression_level = 4;
export constexpr int Anum_bioseq_options_compression = 5;

/* constants for bioseq.stripe */
export constexpr int Natts_bioseq_stripe = 9;
export constexpr int Anum_bioseq_stripe_storageid = 1;
export constexpr int Anum_bioseq_stripe_stripe = 2;
export constexpr int Anum_bioseq_stripe_file_offset = 3;
export constexpr int Anum_bioseq_stripe_data_length = 4;
export constexpr int Anum_bioseq_stripe_column_count = 5;
export constexpr int Anum_bioseq_stripe_chunk_row_count = 6;
export constexpr int Anum_bioseq_stripe_row_count = 7;
export constexpr int Anum_bioseq_stripe_chunk_count = 8;
export constexpr int Anum_bioseq_stripe_first_row_number = 9;

/* constants for bioseq.chunk_group */
export constexpr int Natts_bioseq_chunkgroup = 5;
export constexpr int Anum_bioseq_chunkgroup_storageid = 1;
export constexpr int Anum_bioseq_chunkgroup_stripe = 2;
export constexpr int Anum_bioseq_chunkgroup_chunk = 3;
export constexpr int Anum_bioseq_chunkgroup_row_count = 4;
export constexpr int Anum_bioseq_chunkgroup_deleted_rows = 5;

/* constants for bioseq.chunk */
export constexpr int Natts_bioseq_chunk = 14;
export constexpr int Anum_bioseq_chunk_storageid = 1;
export constexpr int Anum_bioseq_chunk_stripe = 2;
export constexpr int Anum_bioseq_chunk_attr = 3;
export constexpr int Anum_bioseq_chunk_chunk = 4;
export constexpr int Anum_bioseq_chunk_minimum_value = 5;
export constexpr int Anum_bioseq_chunk_maximum_value = 6;
export constexpr int Anum_bioseq_chunk_value_stream_offset = 7;
export constexpr int Anum_bioseq_chunk_value_stream_length = 8;
export constexpr int Anum_bioseq_chunk_exists_stream_offset = 9;
export constexpr int Anum_bioseq_chunk_exists_stream_length = 10;
export constexpr int Anum_bioseq_chunk_value_compression_type = 11;
export constexpr int Anum_bioseq_chunk_value_compression_level = 12;
export constexpr int Anum_bioseq_chunk_value_decompressed_size = 13;
export constexpr int Anum_bioseq_chunk_value_count = 14;

/* constants for bioseq.row_mask */
export constexpr int Natts_bioseq_row_mask = 8;
export constexpr int Anum_bioseq_row_mask_id = 1;
export constexpr int Anum_bioseq_row_mask_storage_id = 2;
export constexpr int Anum_bioseq_row_mask_stripe_id = 3;
export constexpr int Anum_bioseq_row_mask_chunk_id = 4;
export constexpr int Anum_bioseq_row_mask_start_row_number = 5;
export constexpr int Anum_bioseq_row_mask_end_row_number = 6;
export constexpr int Anum_bioseq_row_mask_deleted_rows = 7;
export constexpr int Anum_bioseq_row_mask_mask = 8;

export const config_enum_entry bioseq_compression_options[] =
{
	{ "none", COMPRESSION_NONE, false },
	{ "pglz", COMPRESSION_PG_LZ, false },
#if HAVE_CITUS_LIBLZ4
	{ "lz4", COMPRESSION_LZ4, false },
#endif
#if HAVE_LIBZSTD
	{ "zstd", COMPRESSION_ZSTD, false },
#endif
	{ NULL, 0, false }
};

/*
 * ParseCompressionType converts a string to a compression type.
 * For compression algorithms that are invalid or not compiled, it
 * returns COMPRESSION_TYPE_INVALID.
 */
export CompressionType ParseCompressionType(const char *compressionTypeString)
{
	Assert(compressionTypeString != NULL);

	for (int compressionIndex = 0;
		 bioseq_compression_options[compressionIndex].name != NULL;
		 compressionIndex++)
	{
		const char *compressionName = bioseq_compression_options[compressionIndex].name;
		if (strncmp(compressionTypeString, compressionName, NAMEDATALEN) == 0)
		{
			return static_cast<CompressionType>(bioseq_compression_options[compressionIndex].val);
		}
	}

	return COMPRESSION_TYPE_INVALID;
}


/*
 * CompressionTypeStr returns string representation of a compression type.
 * For compression algorithms that are invalid or not compiled, it
 * returns NULL.
 */
export const char* CompressionTypeStr(CompressionType requestedType)
{
	for (int compressionIndex = 0;
		 bioseq_compression_options[compressionIndex].name != NULL;
		 compressionIndex++)
	{
		CompressionType compressionType =
			static_cast<CompressionType>(bioseq_compression_options[compressionIndex].val);
		if (compressionType == requestedType)
		{
			return bioseq_compression_options[compressionIndex].name;
		}
	}

	return NULL;
}


/*
 * Default values for option parameters. Moved here from columnar.cppm
 * (DOC0322 §9.3) so they are module-exported and visible to bioseq_guc_init
 * (which still lives in bioseq_column) and to any reader that imports config.
 */
export constexpr int DEFAULT_STRIPE_ROW_COUNT = 150000;
export constexpr int DEFAULT_CHUNK_ROW_COUNT = 10000;

#if HAVE_LIBZSTD
export constexpr int DEFAULT_COMPRESSION_TYPE = COMPRESSION_ZSTD;
#elif HAVE_CITUS_LIBLZ4
export constexpr int DEFAULT_COMPRESSION_TYPE = COMPRESSION_LZ4;
#else
export constexpr int DEFAULT_COMPRESSION_TYPE = COMPRESSION_PG_LZ;
#endif

/*
 * GUC backing variables (DOC0322 §9.3). Plain int/bool/double globals exported
 * from the config leaf module so every GUC reader can `import config;` to read
 * them, while the registration (bioseq_guc_init) stays in bioseq_column and
 * takes their addresses.
 */
export int bioseq_compression = DEFAULT_COMPRESSION_TYPE;
export int bioseq_stripe_row_limit = DEFAULT_STRIPE_ROW_COUNT;
export int bioseq_chunk_group_row_limit = DEFAULT_CHUNK_ROW_COUNT;
export int bioseq_compression_level = 3;
export bool bioseq_enable_parallel_execution = true;
export int bioseq_min_parallel_processes = 8;
export bool bioseq_enable_vectorization = true;
export bool bioseq_enable_dml = true;
export bool bioseq_enable_page_cache = false;
export int bioseq_page_cache_size = 200U;
export bool bioseq_index_scan = true;
export bool bioseq_update_index_on_insert = true;
/*
 * DOC0279 §11.3/§13 — read_sequence_index gates the query-time
 * assemble-vs-recompute choice. When OFF the batch processor always
 * recomputes per-row minimizers for mmsearch targets (identical to the
 * historical behaviour). When ON the dispatch loop assembles the active
 * per-sequence minimizer index from its chunked storage (deserialise +
 * globalise, no re-collection) for each buffered row that has one. Rows
 * without a stored index still recompute — surfaced in EXPLAIN.
 */
export bool bioseq_read_sequence_index = true;

/* DOC0279 §13 — test-only knob: forces the minimizers_per_sequence external
 * sort's run buffer to a raw byte size (bypassing the 64 MiB floor) so a small
 * fixture can be made to spill >=2 runs. 0 => use the computed RUN_BUFFER_BYTES. */
export int bioseq_index_run_buffer_bytes = 0;
export int  bioseq_scheduler_max_concurrency = 0;
export int  bioseq_scheduler_pipeline_concurrency_mode = 0;
export bool bioseq_scheduler_allow_pg_parallel_composition = false;
export bool bioseq_load_overlap_enabled = false;
export bool bioseq_index_async_persist = true;
export bool bioseq_recompute_async_persist = false;
export int  bioseq_index_memory_cap_mb = 0;

/*
 * DOC0263 §11.3.2 / DOC0264 — test-only knob. When on, the dispatch buffer
 * wraps row sequences in a synthetic chunked scanner (parity tests). Default
 * off; PGC_SUSET. The synthetic scanner has NO Postgres backing — the
 * production fragmented scanner is `sequence_scanner_pg` (DOC0264). With
 * the macro-gating removed in DOC0274, the chunked path is always compiled
 * into the dispatch_buffer TU.
 */
export bool bioseq_test_force_chunked_scanner = false;

/*
 * DOC0264 §9.2 — test parity knob. Default OFF. Production stub-backed
 * entries flow through `with_lazy_postgres<S>` (the production scanner).
 * When ON, the dispatch loop materialises the stub via fetch_payload and
 * wraps the resulting full varlena in `with_lazy_in_memory` instead,
 * matching the Phase B baseline path. Integration tests flip this on to
 * verify the production scanner_pg pipeline returns the same MatchResult
 * set as the materialised baseline.
 */
export bool bioseq_test_force_inmemory_scanner = false;

/*
 * DOC0260 Phase A — per-shard cap for the length-bucketed sub-table
 * sharding. A new shard is allocated when the active one would cross
 * `bioseq_storage_max_seq_sub_table_gbs * 2^30` bytes of payload. Special
 * value 0 forces a fresh shard on every insert (test-only). Default 1024
 * GiB (= 1 TiB).
 */
export int bioseq_storage_max_seq_sub_table_gbs = 1024;

/*
 * Aligner parameter GUCs — one per field of Minimap2Params so the user can
 * tune the minimap2 pipeline per-session without recompiling. Global defaults
 * match mm_mapopt_init() in options.c; per-call overrides are delivered via
 * the aligner_options() composite type passed as mmsearch's optional third
 * argument. See sequences/minimap_port.cppm for field-by-field documentation.
 */
export int  bioseq_aligner_kmer_size       = 15;
export int  bioseq_aligner_window_size     = 10;
export int  bioseq_aligner_max_gap         = 5000;
export int  bioseq_aligner_max_gap_ref     = -1;
export int  bioseq_aligner_bandwidth       = 500;
export int  bioseq_aligner_bandwidth_long  = 0;
export int  bioseq_aligner_max_chain_skip  = 25;
export int  bioseq_aligner_max_chain_iter  = 5000;
export int  bioseq_aligner_min_seed_count  = 3;
export int  bioseq_aligner_min_chain_score = 40;
export double bioseq_aligner_chain_gap_scale  = 0.8;
export double bioseq_aligner_chain_skip_scale = 0.0;
export double bioseq_aligner_mask_level    = 0.5;
export int  bioseq_aligner_mask_len        = 2147483647;   /* INT_MAX per options.c */
export double bioseq_aligner_pri_ratio     = 0.8;
export int  bioseq_aligner_best_n          = 5;
export int  bioseq_aligner_match_score     = 2;
export int  bioseq_aligner_mismatch_pen    = 4;
export int  bioseq_aligner_gap_open1       = 4;
export int  bioseq_aligner_gap_ext1        = 2;
export int  bioseq_aligner_gap_open2       = 24;
export int  bioseq_aligner_gap_ext2        = 1;
export int  bioseq_aligner_sc_ambi         = 1;
export int  bioseq_aligner_zdrop           = 400;
export int  bioseq_aligner_end_bonus       = -1;
export int  bioseq_aligner_max_occ         = 500;
export int  bioseq_aligner_mid_occ         = 500;
export int  bioseq_aligner_block_min_size  = 32;
export int  bioseq_aligner_block_max_size  = 256;
/*
 * Chaining algorithm selection — SCALAR_DP (O(n·iter), minimap2 baseline),
 * SIMD_DP (same algorithm, Highway-vectorised forward pass), RMQ (AVL+RMQ
 * tree; O(n log n)). Base-alignment selection — KSW2 (dual-affine DP, the
 * minimap2 default) or BLOCK_ALIGNER (single-affine, Highway-vectorised).
 * The enums below are parsed from GUC strings; the backend dispatches on
 * them via constexpr if so the hot path stays branchless.
 */
export int  bioseq_aligner_chaining_algorithm  = 0;  /* matches ChainingAlgorithmType::SCALAR_MM2 */
export int  bioseq_aligner_base_alignment_algorithm = 1;  /* matches BaseAlignmentAlgorithmType::BLOCK_ALIGNER */
export int  bioseq_aligner_minimizer_algorithm = 0;  /* DOC0286: matches MinimizerAlgorithmType::SCALAR */
/*
 * Runtime profiling toggle. When true, BatchProcessor::process_sequences
 * picks the profiling pipeline (context_args_profiling_full); when false,
 * picks context_args_no_profiling (zero overhead). Defaults to false.
 */
export bool bioseq_aligner_enable_profiling = false;
/*
 * Enable Vulkan execution for Vulkan-backed aligner algorithms. When a Vulkan
 * algorithm is selected and this is false, the pipeline fails instead of
 * running a CPU substitute. Defaults to true.
 */
export bool bioseq_aligner_vulkan_enabled = true;
/*
 * DOC0307 Phase B — opt into the Vulkan seed-hit lookup and the GPU-resident
 * minimizer index upload cache when Vulkan is enabled. Seed GPU execution
 * defaults to false; selected Vulkan seed execution fails when the runtime
 * cannot run it.
 */
export bool bioseq_aligner_vulkan_seed_hits_enabled = false;
export bool bioseq_aligner_vulkan_index_cache_enabled = true;
/*
 * DOC0310 Phase C — resident two-submit Vulkan frontend (parallel ordered
 * sketch + GPU target-index build + GPU seed count/emit + device anchor sort)
 * and the optional GPU base-alignment extension. Defaults keep the frontend off;
 * selected Phase C execution fails when the GPU path cannot run.
 */
export bool bioseq_aligner_vulkan_fused_frontend_enabled = false;
export bool bioseq_aligner_vulkan_gpu_index_build_enabled = false;
export bool bioseq_aligner_vulkan_extend_enabled = false;
export int  bioseq_aligner_vulkan_phase_c_tile_bp = 2048;
export int  bioseq_aligner_vulkan_phase_c_radix_bits = 8;
