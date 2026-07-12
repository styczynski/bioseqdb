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


extern "C" {
#include "miscadmin.h"
#include "utils/inval.h"
#include "utils/guc_tables.h"
#include "utils/lsyscache.h"
#include "catalog/namespace.h"
}

export module bioseq.column;

import bioseq.common.mlogging;
import bioseq.table;                      // bioseq_tableam_init, bioseq_customscan_init
import bioseq.table.am_cache;             // bioseq_am_cache_invalidate
import bioseq.config;
import bioseq.pg_utils;
import bioseq.storage.manager;
import bioseq.scheduler.supervisor_pg;
import bioseq.storage.indexing_pg.residency;
import bioseq.indexing.minimizer_sequence;
import bioseq.column.planner_hook;        // DOC0322 §9.4 — exported bioseq_planner_init
import bioseq.table.batch_sort_scan;      // exported bioseq_batch_sort_init (sequence ORDER BY)
#if BIOSEQ_EXTERNAL_PG
import bioseq.pg.external.relation;        // DOC0372 — bioseq_external_tableam_init
import bioseq.pg.external.scan;            // DOC0372 — bioseq_external_scan_init
#endif

/*
 * DOC0322 §7.7/§9.4 — the file-body `extern "C++"` wrapper is gone. bioseq_init
 * is `export`ed (so columnar.h's extern can be removed); bioseq_guc_init stays
 * a file-internal static.
 *
 * GUC backing variables + DEFAULT_* option constants moved to the `config`
 * module (DOC0322 §9.3) where they are `export`ed; they are read here via
 * `import config;`. bioseq_guc_init below takes their addresses.
 */
static void bioseq_guc_init();

/*
 * bioseq_init is the module's top-level initialiser. It wires up the GUC
 * variables, installs the bioseq table access method handler and registers
 * the planner hooks that route queries through the columnar custom scan.
 */
export void
bioseq_init()
{
	bioseq_log<DBG>("[bioseq] pg: bioseq_init() entered");
	bioseq_guc_init();
	if (process_shared_preload_libraries_in_progress) {
		bioseq::parallelism::pg::install_scheduler_shmem_hooks();
		bioseq::parallelism::pg::register_scheduler_supervisor();
	}
	bioseq::parallelism::pg::install_pg_shader_provider_if_available();
	bioseq_tableam_init();
	// DOC0322 §7.5 — the customscan init call moved out of bioseq_tableam_init
	// to here so :access_method no longer imports :custom_scan (the bioseq_table
	// partition graph stays a DAG). bioseq_customscan_init is exported from
	// :custom_scan and imported directly here so the bioseq.table aggregate
	// does not need to re-export the heavy custom scan partition on GCC.
	bioseq_customscan_init();
#if BIOSEQ_EXTERNAL_PG
	/* DOC0372 — external logical relations. Register the external table access
	 * method's reloption kind and the external CustomScan (methods + path
	 * builder slot). Gated by BIOSEQ_EXTERNAL_PG, which is defined only when the
	 * provider stack is compiled into the extension; without it the always-owned
	 * path-builder slot stays null and the columnar hook falls through. */
	bioseq_external_tableam_init();
	bioseq_external_scan_init();
#endif
	bioseq_planner_init();
	/* Outer custom scan that sorts sequences for `ORDER BY <bioseq.seq>`,
	 * replacing PostgreSQL's own Sort via a create_upper_paths_hook. */
	bioseq_batch_sort_init();
	/* DOC0279 §12.1 — the legacy MetadataKindRegistry / sidecar kind
	 * registration is gone; the set of indexes is the compile-time
	 * pg_supported_indexes::list_types() registry (no runtime init). */

	/* DOC0260 §11.3 step 2 — register relcache invalidation callback
	 * for the per-Relation BioseqAmCache slot installed on rd_amcache.
	 * Postgres clears rd_amcache on a full relcache flush, but per-relid
	 * invalidations still need our callback so the small_vector heap
	 * spill is freed before the slot is dropped. */
	CacheRegisterRelcacheCallback(bioseq::storage::bioseq_am_cache_invalidate,
								  PointerGetDatum(NULL));
	bioseq::storage::indexing::install_index_residency_callbacks();

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
	bioseq_log<DBG>("[bioseq] pg: bioseq_guc_init() entered");
	if (auto* existing = find_option("bioseq.logging", false, true, ERROR);
	    existing != nullptr && existing->vartype == PGC_BOOL) {
		bioseq_log<DBG>("[bioseq] pg: bioseq_guc_init() skipped; GUCs already registered");
		return;
	}

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

	static const struct config_enum_entry pipeline_concurrency_mode_options[] = {
		{ "single", 0, false },
		{ "parallel", 1, false },
		{ "task_parallel", 2, false },
		{ NULL, 0, false }
	};

	DefineCustomIntVariable(
		"bioseq.scheduler.max_concurrency",
		"Maximum bioseq TBB worker slots per backend (0 = CPU count).",
		NULL,
		&bioseq_scheduler_max_concurrency,
		0,
		0,
		1024,
		PGC_SIGHUP,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomEnumVariable(
		"bioseq.scheduler.pipeline_concurrency_mode",
		"Pipeline concurrency: single, parallel, task_parallel.",
		NULL,
		&bioseq_scheduler_pipeline_concurrency_mode,
		0,
		pipeline_concurrency_mode_options,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomBoolVariable(
		"bioseq.scheduler.allow_pg_parallel_composition",
		"Allow a PostgreSQL parallel worker process to use more than one bioseq TBB slot.",
		NULL,
		&bioseq_scheduler_allow_pg_parallel_composition,
		false,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomBoolVariable(
		"bioseq.load_overlap",
		"Overlap bioseq.load parsing/encoding with main-backend columnar writes.",
		NULL,
		&bioseq_load_overlap_enabled,
		false,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomBoolVariable(
		"bioseq.index_async_persist",
		"Persist opportunistically built in-memory indexes after returning query results.",
		NULL,
		&bioseq_index_async_persist,
		true,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomBoolVariable(
		"bioseq.recompute_async_persist",
		"Stage a minimizers_per_fragment index for async persistence after no-index recompute.",
		NULL,
		&bioseq_recompute_async_persist,
		false,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);

	DefineCustomIntVariable(
		"bioseq.index_memory_cap_mb",
		"Maximum RAM budget for resident index runs/chunks/assembled indexes (0 = live MemoryBudget).",
		NULL,
		&bioseq_index_memory_cap_mb,
		0,
		0,
		INT_MAX,
		PGC_USERSET,
		GUC_UNIT_MB,
		NULL,
		NULL,
		NULL);

	static const struct config_enum_entry mps_pair_sort_impl_options[] = {
		{ "std_sort", static_cast<int>(bioseq::indexing::detail::PairRunSortImpl::std_sort), false },
		{ "simd_u128_vqsort", static_cast<int>(bioseq::indexing::detail::PairRunSortImpl::simd_u128_vqsort), false },
		{ "simd_u128_vec_stream_1", static_cast<int>(bioseq::indexing::detail::PairRunSortImpl::simd_u128_vec_stream_1), false },
		{ "simd_u128_vec_stream_2", static_cast<int>(bioseq::indexing::detail::PairRunSortImpl::simd_u128_vec_stream_2), false },
		{ "simd_u128_vec_stream_4", static_cast<int>(bioseq::indexing::detail::PairRunSortImpl::simd_u128_vec_stream_4), false },
		{ NULL, 0, false }
	};

	DefineCustomEnumVariable(
		"bioseq.mps_pair_sort_impl",
		"Sort implementation for minimizers_per_sequence pair runs.",
		NULL,
		&bioseq::indexing::detail::bioseq_pair_run_sort_impl,
		static_cast<int>(bioseq::indexing::detail::PairRunSortImpl::std_sort),
		mps_pair_sort_impl_options,
		PGC_USERSET,
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
		{ "scalar_mm2",       0, false },
		{ "simd_mm2",         1, false },
		{ "rmq",              2, false },
		{ "vulkan_tiled_dp",  3, false },
		{ NULL, 0, false }
	};
	DefineCustomEnumVariable("bioseq.aligner.chaining_algorithm",
							 "Chaining algorithm: scalar_mm2, simd_mm2, rmq, vulkan_tiled_dp.",
							 NULL, &bioseq_aligner_chaining_algorithm, 0,
							 chaining_algorithm_options,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable("bioseq.aligner.vulkan_enabled",
							 "Enable Vulkan execution for Vulkan-backed algorithms; selected Vulkan algorithms fail when disabled.",
							 NULL, &bioseq_aligner_vulkan_enabled, true,
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

	/* DOC0286 §10.2 — minimizer kernel selection for the mmsearch seeding
	 * stage. two_stack_nthash is the SIMD two-stack nthash path the E2E bench
	 * forces. Maps to MinimizerAlgorithmType (minimap_port.cppm). */
	static const struct config_enum_entry minimizer_algorithm_options[] = {
		{ "scalar",             0, false },
		{ "simd_minimap2_slow", 1, false },
		{ "two_stack_nthash",   2, false },
		{ "vulkan_minimap_int", 3, false },
		{ NULL, 0, false }
	};
	DefineCustomEnumVariable("bioseq.aligner.minimizer_algorithm",
							 "Minimizer algorithm: scalar, simd_minimap2_slow, two_stack_nthash, vulkan_minimap_int.",
							 NULL, &bioseq_aligner_minimizer_algorithm, 0,
							 minimizer_algorithm_options,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	/* DOC0307 Phase B — Vulkan seed-hit lookup + GPU-resident index upload cache. */
	DefineCustomBoolVariable("bioseq.aligner.vulkan_seed_hits_enabled",
							 "Opt into Vulkan seed-hit lookup when Vulkan is enabled; selected Vulkan seed execution fails when unavailable.",
							 NULL, &bioseq_aligner_vulkan_seed_hits_enabled, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("bioseq.aligner.vulkan_index_cache_enabled",
							 "Cache GPU-resident minimizer index uploads while their memory permit is held.",
							 NULL, &bioseq_aligner_vulkan_index_cache_enabled, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	/* DOC0310 Phase C — resident two-submit Vulkan frontend + GPU index build. */
	DefineCustomBoolVariable("bioseq.aligner.vulkan_fused_frontend_enabled",
							 "Opt into the Phase C resident two-submit Vulkan frontend (parallel ordered sketch + GPU seed count/emit + device anchor sort).",
							 NULL, &bioseq_aligner_vulkan_fused_frontend_enabled, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("bioseq.aligner.vulkan_gpu_index_build_enabled",
							 "Opt into building the target minimizer index entirely on the GPU (device radix sort + race-free slot table).",
							 NULL, &bioseq_aligner_vulkan_gpu_index_build_enabled, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("bioseq.aligner.vulkan_extend_enabled",
							 "Enable the optional Phase C GPU base-alignment extension under the block aligner.",
							 NULL, &bioseq_aligner_vulkan_extend_enabled, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.vulkan_phase_c_tile_bp",
							 "Phase C ordered-sketch tile size (base pairs per tile).",
							 NULL, &bioseq_aligner_vulkan_phase_c_tile_bp, 2048,
							 1, 1 << 24, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("bioseq.aligner.vulkan_phase_c_radix_bits",
							 "Phase C device radix sort digit width (bits per pass).",
							 NULL, &bioseq_aligner_vulkan_phase_c_radix_bits, 8,
							 1, 16, PGC_USERSET, 0, NULL, NULL, NULL);

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
