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

extern "C" {
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "catalog/namespace.h"
}

export module bioseq_columnar;

import mlogging;
import bioseq_table;
import config;
import pg_utils;
import bioseq_metadata_admin;
import bioseq_sidecar;
import bioseq_storage;

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
 * read_sequence_index gates the sidecar read path. When OFF the batch
 * processor always recomputes per-row minimizers for mmsearch targets
 * (identical to the historical behaviour before the sidecar index read).
 * When ON the custom scan opens bioseq_internal.index_sidecar for each
 * buffered row and, if a blob exists, rehydrates the TargetIndex rather
 * than re-computing it from the compact sequence. Rows without a sidecar
 * entry (INSERT happened under bioseq.update_index_on_insert = off) still
 * recompute — the recomputed count is surfaced in EXPLAIN.
 */
bool bioseq_read_sequence_index = true;

/* DOC0279 §13 — test-only knob: forces the minimizers_per_sequence external
 * sort's run buffer to a raw byte size (bypassing the 64 MiB floor) so a small
 * fixture can be made to spill >=2 runs. 0 => use the computed RUN_BUFFER_BYTES. */
int bioseq_index_run_buffer_bytes = 0;

/*
 * DOC0263 §11.3.2 / DOC0264 — test-only knob. When on, the dispatch buffer
 * wraps row sequences in a synthetic chunked scanner (parity tests). Default
 * off; PGC_SUSET. The synthetic scanner has NO Postgres backing — the
 * production fragmented scanner is `sequence_scanner_pg` (DOC0264). With
 * the macro-gating removed in DOC0274, the chunked path is always compiled
 * into the dispatch_buffer TU.
 */
bool bioseq_test_force_chunked_scanner = false;

/*
 * DOC0264 §9.2 — test parity knob. Default OFF. Production stub-backed
 * entries flow through `with_lazy_postgres<S>` (the production scanner).
 * When ON, the dispatch loop materialises the stub via fetch_payload and
 * wraps the resulting full varlena in `with_lazy_in_memory` instead,
 * matching the Phase B baseline path. Integration tests flip this on to
 * verify the production scanner_pg pipeline returns the same MatchResult
 * set as the materialised baseline.
 */
bool bioseq_test_force_inmemory_scanner = false;

/*
 * DOC0260 Phase A — per-shard cap for the length-bucketed sub-table
 * sharding. A new shard is allocated when the active one would cross
 * `bioseq_storage_max_seq_sub_table_gbs * 2^30` bytes of payload. Special
 * value 0 forces a fresh shard on every insert (test-only). Default 1024
 * GiB (= 1 TiB).
 */
int bioseq_storage_max_seq_sub_table_gbs = 1024;

/*
 * Aligner parameter GUCs — one per field of Minimap2Params so the user can
 * tune the minimap2 pipeline per-session without recompiling. Global defaults
 * match mm_mapopt_init() in options.c; per-call overrides are delivered via
 * the aligner_options() composite type passed as mmsearch's optional third
 * argument. See sequences/minimap_port.cppm for field-by-field documentation.
 */
int  bioseq_aligner_kmer_size       = 15;
int  bioseq_aligner_window_size     = 10;
int  bioseq_aligner_max_gap         = 5000;
int  bioseq_aligner_max_gap_ref     = -1;
int  bioseq_aligner_bandwidth       = 500;
int  bioseq_aligner_bandwidth_long  = 0;
int  bioseq_aligner_max_chain_skip  = 25;
int  bioseq_aligner_max_chain_iter  = 5000;
int  bioseq_aligner_min_seed_count  = 3;
int  bioseq_aligner_min_chain_score = 40;
double bioseq_aligner_chain_gap_scale  = 0.8;
double bioseq_aligner_chain_skip_scale = 0.0;
double bioseq_aligner_mask_level    = 0.5;
int  bioseq_aligner_mask_len        = 2147483647;   /* INT_MAX per options.c */
double bioseq_aligner_pri_ratio     = 0.8;
int  bioseq_aligner_best_n          = 5;
int  bioseq_aligner_match_score     = 2;
int  bioseq_aligner_mismatch_pen    = 4;
int  bioseq_aligner_gap_open1       = 4;
int  bioseq_aligner_gap_ext1        = 2;
int  bioseq_aligner_gap_open2       = 24;
int  bioseq_aligner_gap_ext2        = 1;
int  bioseq_aligner_sc_ambi         = 1;
int  bioseq_aligner_zdrop           = 400;
int  bioseq_aligner_end_bonus       = -1;
int  bioseq_aligner_max_occ         = 500;
int  bioseq_aligner_mid_occ         = 500;
int  bioseq_aligner_block_min_size  = 32;
int  bioseq_aligner_block_max_size  = 256;
/*
 * Chaining algorithm selection — SCALAR_DP (O(n·iter), minimap2 baseline),
 * SIMD_DP (same algorithm, Highway-vectorised forward pass), RMQ (AVL+RMQ
 * tree; O(n log n)). Base-alignment selection — KSW2 (dual-affine DP, the
 * minimap2 default) or BLOCK_ALIGNER (single-affine, Highway-vectorised).
 * The enums below are parsed from GUC strings; the backend dispatches on
 * them via constexpr if so the hot path stays branchless.
 */
int  bioseq_aligner_chaining_algorithm  = 0;  /* matches ChainingAlgorithmType::SCALAR_MM2 */
int  bioseq_aligner_base_alignment_algorithm = 1;  /* matches BaseAlignmentAlgorithmType::BLOCK_ALIGNER */
/*
 * Runtime profiling toggle. When true, BatchProcessor::process_sequences
 * picks the profiling pipeline (context_args_profiling_full); when false,
 * picks context_args_no_profiling (zero overhead). Defaults to false.
 */
bool bioseq_aligner_enable_profiling = false;

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
	/* MetadataKindRegistry: idempotent init followed by per-module
	 * registrations. The registry singleton is empty until populated
	 * here; future modules (e.g., a sneaky_snake prefilter or BWA-FM
	 * index module) can chain their own register_*_metadata_kinds()
	 * calls right after sidecar's. */
	bioseq::metadata_admin::init_registry();
	bioseq::sidecar::register_default_metadata_kinds();

	/* DOC0260 §11.3 step 2 — register relcache invalidation callback
	 * for the per-Relation BioseqAmCache slot installed on rd_amcache.
	 * Postgres clears rd_amcache on a full relcache flush, but per-relid
	 * invalidations still need our callback so the small_vector heap
	 * spill is freed before the slot is dropped. */
	CacheRegisterRelcacheCallback(bioseq::storage::bioseq_am_cache_invalidate,
								  PointerGetDatum(NULL));

	/* DOC0260 §11.3 step 3 — orphan reaper. We deliberately do NOT call
	 * `ReapOrphanSeqSubTables()` directly from `_PG_init` because there is
	 * no active transaction at module-load time and the reaper uses SPI
	 * (which requires `IsTransactionState() == true`). Operators run the
	 * sweep via the `bioseq.reap_orphan_seq_sub_tables()` SQL function
	 * declared in `bioseq--1.0.sql`. */
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

	// DOC0263 §11.3.2 / DOC0264 — test-only chunked-scanner force knob.
	DefineCustomBoolVariable(
		"bioseq.test_force_chunked_scanner",
		"Test-only: wrap dispatch-buffer row sequences in a synthetic chunked "
		"scanner (DOC0263 parity tests). The chunked path is always compiled "
		"in (DOC0274 removed the BIOSEQ_ENABLE_FRAGMENTED_DISPATCH build flag). "
		"DO NOT USE IN PRODUCTION — the synthetic scanner has no Postgres "
		"backing.",
		NULL,
		&bioseq_test_force_chunked_scanner,
		false,
		PGC_SUSET,
		GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
		NULL, NULL, NULL
	);

	// DOC0264 §9.2 — test parity GUC. Default off. Production stub-backed
	// entries flow through `with_lazy_postgres<S>`; when this GUC is on, the
	// dispatch loop materialises the stub via fetch_payload and uses the
	// `with_lazy_in_memory` baseline. Integration tests use this to verify
	// scanner_pg produces the same MatchResult set as the materialised path.
	DefineCustomBoolVariable(
		"bioseq.test_force_inmemory_scanner",
		"Test-only: force the dispatch buffer to materialise stub-backed "
		"sequences via fetch_payload and dispatch through with_lazy_in_memory "
		"(matches the Phase B baseline). Default off — production goes through "
		"the Phase C with_lazy_postgres scanner_pg.",
		NULL,
		&bioseq_test_force_inmemory_scanner,
		false,
		PGC_SUSET,
		GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
		NULL, NULL, NULL
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
		"When true, (re)build every index whose per-table policy is "
		"UPDATE_ON_INSERT for each bioseq.seq value written into a USING bioseq "
		"table (fresh-table defaults: minimizers_per_fragment and symbol_counts).",
		NULL,
		&bioseq_update_index_on_insert,
		true,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomBoolVariable(
		"bioseq.read_sequence_index",
		"When true, the batch processor assembles the active per-sequence "
		"minimizer index from its chunked storage for each buffered row instead "
		"of recomputing it. Rows without a stored index still recompute.",
		NULL,
		&bioseq_read_sequence_index,
		true,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomIntVariable(
		"bioseq.index_run_buffer_bytes",
		"Test-only: raw byte size of the minimizers_per_sequence external-sort "
		"run buffer (bypasses the 64 MiB floor); 0 uses the computed value.",
		NULL,
		&bioseq_index_run_buffer_bytes,
		0, 0, INT_MAX,
		PGC_SUSET,
		GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
		NULL,
		NULL,
		NULL);

	DefineCustomIntVariable(
		"bioseq.storage.max_seq_sub_table_gbs",
		"Maximum size, in gibibytes, of one sequence storage sub-table shard. "
		"A new shard is allocated when the active one would cross this size. "
		"Special value 0 forces a fresh shard on every insert -- test-only; "
		"do NOT use in production multi-writer workloads.",
		NULL,
		&bioseq_storage_max_seq_sub_table_gbs,
		1024,
		0,
		INT_MAX,
		PGC_SUSET,
		0,
		NULL,
		NULL,
		NULL);

	/* ---------------- Aligner parameter GUCs (global defaults) -------------- */
	DefineCustomIntVariable("bioseq.aligner.kmer_size",
							"Minimizer k-mer size for the aligner pipeline.",
							NULL, &bioseq_aligner_kmer_size, 15,
							1, 64, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.window_size",
							"Minimizer window size.",
							NULL, &bioseq_aligner_window_size, 10,
							1, 1024, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.max_gap",
							"Maximum reference gap in chaining.",
							NULL, &bioseq_aligner_max_gap, 5000,
							1, 1 << 30, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.max_gap_ref",
							"Explicit reference gap (-1 uses max_gap).",
							NULL, &bioseq_aligner_max_gap_ref, -1,
							-1, 1 << 30, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.bandwidth",
							"Bandwidth for chaining and alignment.",
							NULL, &bioseq_aligner_bandwidth, 500,
							1, 1 << 20, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.bandwidth_long",
							"Long-join bandwidth (0 disables long join).",
							NULL, &bioseq_aligner_bandwidth_long, 0,
							0, 1 << 20, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.max_chain_skip",
							"Max skip for early termination in chaining DP.",
							NULL, &bioseq_aligner_max_chain_skip, 25,
							0, 1 << 20, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.max_chain_iter",
							"Max predecessors examined per anchor.",
							NULL, &bioseq_aligner_max_chain_iter, 5000,
							1, 1 << 22, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.min_seed_count",
							"Minimum seeds per chain.",
							NULL, &bioseq_aligner_min_seed_count, 3,
							0, 1 << 20, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.min_chain_score",
							"Minimum chain score.",
							NULL, &bioseq_aligner_min_chain_score, 40,
							0, 1 << 20, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("bioseq.aligner.chain_gap_scale",
							 "Chaining gap penalty scale.",
							 NULL, &bioseq_aligner_chain_gap_scale, 0.8,
							 0.0, 100.0, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("bioseq.aligner.chain_skip_scale",
							 "Chaining skip penalty scale.",
							 NULL, &bioseq_aligner_chain_skip_scale, 0.0,
							 0.0, 100.0, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("bioseq.aligner.mask_level",
							 "Primary/secondary overlap threshold.",
							 NULL, &bioseq_aligner_mask_level, 0.5,
							 0.0, 1.0, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.mask_len",
							"Maximum uncovered length for secondary classification.",
							NULL, &bioseq_aligner_mask_len, 2147483647,
							0, 2147483647, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("bioseq.aligner.pri_ratio",
							 "Secondary-to-primary score ratio threshold.",
							 NULL, &bioseq_aligner_pri_ratio, 0.8,
							 0.0, 1.0, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.best_n",
							"Max secondary alignments to keep per primary.",
							NULL, &bioseq_aligner_best_n, 5,
							0, 1024, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.match_score",
							"Match score (a).", NULL,
							&bioseq_aligner_match_score, 2,
							1, 127, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.mismatch_pen",
							"Mismatch penalty (b).", NULL,
							&bioseq_aligner_mismatch_pen, 4,
							1, 127, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.gap_open1",
							"Short gap open (q).", NULL,
							&bioseq_aligner_gap_open1, 4,
							0, 127, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.gap_ext1",
							"Short gap extend (e).", NULL,
							&bioseq_aligner_gap_ext1, 2,
							0, 127, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.gap_open2",
							"Long gap open (q2).", NULL,
							&bioseq_aligner_gap_open2, 24,
							0, 127, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.gap_ext2",
							"Long gap extend (e2).", NULL,
							&bioseq_aligner_gap_ext2, 1,
							0, 127, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.sc_ambi",
							"Penalty for ambiguous base (N).", NULL,
							&bioseq_aligner_sc_ambi, 1,
							0, 127, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.zdrop",
							"Z-drop threshold.", NULL,
							&bioseq_aligner_zdrop, 400,
							-1, 1 << 20, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.end_bonus",
							"Bonus for reaching sequence ends (-1 to disable).", NULL,
							&bioseq_aligner_end_bonus, -1,
							-1, 1 << 20, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.max_occ",
							"Max seed occurrence to keep.", NULL,
							&bioseq_aligner_max_occ, 500,
							1, 1 << 24, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.mid_occ",
							"Mid-level seed occurrence threshold.", NULL,
							&bioseq_aligner_mid_occ, 500,
							1, 1 << 24, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.block_min_size",
							"Block aligner minimum block size (power of 2).",
							NULL, &bioseq_aligner_block_min_size, 32,
							8, 4096, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.block_max_size",
							"Block aligner maximum block size (power of 2).",
							NULL, &bioseq_aligner_block_max_size, 256,
							8, 16384, PGC_USERSET, 0, NULL, NULL, NULL);

	static const struct config_enum_entry chaining_algorithm_options[] = {
		{ "scalar_mm2",   0, false },
		{ "simd_mm2",     1, false },
		{ "rmq",          2, false },
		{ NULL, 0, false }
	};
	DefineCustomEnumVariable("bioseq.aligner.chaining_algorithm",
							 "Chaining algorithm: scalar_mm2, simd_mm2, rmq.",
							 NULL, &bioseq_aligner_chaining_algorithm, 0,
							 chaining_algorithm_options,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	static const struct config_enum_entry base_alignment_algorithm_options[] = {
		{ "ksw2",          0, false },
		{ "block_aligner", 1, false },
		{ NULL, 0, false }
	};
	DefineCustomEnumVariable("bioseq.aligner.base_alignment_algorithm",
							 "Base alignment algorithm: ksw2, block_aligner.",
							 NULL, &bioseq_aligner_base_alignment_algorithm, 1,
							 base_alignment_algorithm_options,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
		"bioseq.aligner.enable_profiling",
		"Enable bioseq runtime profiler for the aligner pipeline.",
		NULL,
		&bioseq_aligner_enable_profiling,
		false,
		PGC_USERSET,
		0,
		NULL, NULL, NULL);
}

}
